#include "ui/signal_search.h"

#include <algorithm>
#include <utility>

#include <imgui.h>
#include <imgui_internal.h> // ScrollToItem
#include <misc/cpp/imgui_stdlib.h>

#include "core/fuzzy.h"
#include "core/setup.h"
#include "core/trace.h"
#include "db/model/can_db.h"
#include "db/model/lin_db.h"
#include "ui/frame_cache.h"

namespace
{

void rebuild_entries(SignalSearch& s, const Setup& setup, const SignalSeen* seen)
{
    s.entries.clear();
    for (std::size_t ni = 0; ni < setup.networks.size(); ++ni)
    {
        const SetupNetwork& net = setup.networks[ni];
        for (const auto& db : net.can_dbs)
        {
            for (const auto& [raw_id, msg] : db->messages)
            {
                if (seen != nullptr && !seen->can.contains(raw_id))
                {
                    continue; // never on the bus / in the log
                }
                for (const CanDbSignal& sig : msg.signals)
                {
                    s.entries.push_back({.label = msg.name + '.' + sig.name, .network = ni, .raw_id = raw_id,
                                         .can_msg = &msg, .can_sig = &sig});
                }
            }
        }
        if (s.can_only)
        {
            continue;
        }
        for (const auto& db : net.lin_dbs)
        {
            for (const auto& [id, frame] : db->frames)
            {
                if (seen != nullptr && !seen->lin.contains(id))
                {
                    continue;
                }
                for (const LinSignal& sig : frame.signals)
                {
                    s.entries.push_back({.label = frame.name + '.' + sig.name, .network = ni, .raw_id = id,
                                         .lin_frame = &frame, .lin_sig = &sig});
                }
            }
        }
    }
}

const std::string& unit_of(const SignalEntry& e)
{
    return e.can_sig != nullptr ? e.can_sig->unit : e.lin_sig->unit;
}

} // namespace

namespace
{

// Frecency: the labels picked most recently (any search box) rank first among equal
// matches. ponytail: last 16 picks, in memory only; persist in the ini if it should survive a restart.
std::vector<std::string> g_recent;

int recency_bonus(const std::string& label)
{
    const auto it = std::ranges::find(g_recent, label);
    return it == g_recent.end() ? 0 : static_cast<int>(g_recent.size() - static_cast<std::size_t>(it - g_recent.begin()));
}

const SignalEntry* picked(const SignalEntry* e)
{
    if (e != nullptr)
    {
        std::erase(g_recent, e->label);
        g_recent.push_back(e->label);
        if (g_recent.size() > 16)
        {
            g_recent.erase(g_recent.begin());
        }
    }
    return e;
}

} // namespace

void signal_seen_update(SignalSeen& seen, const Trace& trace, const FrameCache* cache)
{
    const void* file = trace.file.empty() ? nullptr : trace.file.data();
    if (trace.clears != seen.clears || file != seen.file || trace.end < seen.scanned)
    {
        seen.can.clear();
        seen.lin.clear();
        seen.scanned = trace.begin;
        seen.clears = trace.clears;
        seen.file = file;
        ++seen.generation;
    }
    // Pruned frames stay counted: "seen" means it occurred, not that it is still retained.
    seen.scanned = std::max(seen.scanned, trace.begin);
    if (seen.scanned >= trace.end)
    {
        return;
    }
    bool changed = false;
    const auto note = [&](BusType type, uint32_t id, uint16_t flags, uint16_t errors)
    {
        if (errors != 0)
        {
            return; // error frames carry no usable id
        }
        if (type == BusType::CAN)
        {
            const bool ext = (flags & bus_flag::extended) != 0;
            const uint32_t key = (id & (ext ? can_id_mask_extended : can_id_mask_standard))
                                 | (ext ? 0x80000000u : 0u); // the DBC raw-id key (setup_find_can_message)
            changed |= seen.can.insert(key).second;
        }
        else
        {
            changed |= seen.lin.insert(static_cast<uint8_t>(id & 0x3F)).second;
        }
    };
    if (cache != nullptr && !trace.file.empty() && cache->recs.data() == trace.file.data())
    {
        // File view of a frame cache: the first frame of each (channel, id) row, not every frame.
        // ponytail: a standard and an extended frame sharing an id on one channel share a row, and
        // only the first one's kind is seen; scan the row's frames if such logs turn up.
        for (const FrameCacheRow& row : cache->rows)
        {
            const FrameCacheRec& r = cache->recs[cache->row_frames[row.first]];
            note(r.type, r.id, r.flags, r.errors);
        }
    }
    else if (!trace.file.empty())
    {
        // File view: the 32-byte records in place, no payload decode.
        for (uint64_t i = seen.scanned; i < trace.end; ++i)
        {
            const FrameCacheRec& r = trace.file[i - trace.begin];
            note(r.type, r.id, r.flags, r.errors);
        }
    }
    else
    {
        for (uint64_t i = seen.scanned; i < trace.end; ++i)
        {
            const BusMessage m = trace_at(trace, i);
            note(m.type, m.id, m.flags, m.errors);
        }
    }
    seen.scanned = trace.end;
    if (changed)
    {
        ++seen.generation;
    }
}

void signal_search_update(SignalSearch& s, const Setup& setup, const std::string& query, const SignalSeen* seen)
{
    const uint64_t seen_gen = seen != nullptr ? seen->generation : UINT64_MAX;
    const bool rebuilt = s.generation != setup.generation || s.seen_generation != seen_gen;
    if (rebuilt)
    {
        rebuild_entries(s, setup, seen);
        s.generation = setup.generation;
        s.seen_generation = seen_gen;
    }
    if (!rebuilt && query == s.query)
    {
        return;
    }
    s.query = query;
    s.hits.clear();
    for (std::size_t i = 0; i < s.entries.size(); ++i)
    {
        const int score = fuzzy_score(query, s.entries[i].label);
        if (score >= 0)
        {
            s.hits.push_back({.score = score + recency_bonus(s.entries[i].label), .entry = static_cast<int>(i)});
        }
    }
    // Best score first, then the shorter label (fzf's tie-break); equal ones keep setup order.
    std::stable_sort(s.hits.begin(), s.hits.end(), [&](const SearchHit& a, const SearchHit& b)
    {
        if (a.score != b.score)
        {
            return a.score > b.score;
        }
        return s.entries[static_cast<std::size_t>(a.entry)].label.size() < s.entries[static_cast<std::size_t>(b.entry)].label.size();
    });
    s.selected = 0;
    s.scroll_to_selected = true;
}

const SignalEntry* signal_search_input(SignalSearch& s, const Setup& setup, const char* hint, std::string& query, bool focus,
                                       const SignalSeen* seen)
{
    if (focus || s.focus_input)
    {
        ImGui::SetKeyboardFocusHere();
        s.focus_input = false;
    }
    // CallbackHistory makes the input own Up/Down (keyboard nav would otherwise leave the field).
    int step = 0;
    const auto history = [](ImGuiInputTextCallbackData* data)
    {
        *static_cast<int*>(data->UserData) += data->EventKey == ImGuiKey_DownArrow ? 1 : -1;
        return 0;
    };
    const bool enter = ImGui::InputTextWithHint("##signal_search", hint, &query,
                                                ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CallbackHistory, history, &step);
    signal_search_update(s, setup, query, seen);
    const int n = static_cast<int>(s.hits.size());
    if (step != 0 && n > 0)
    {
        s.selected = std::clamp(s.selected + step, 0, n - 1);
        s.scroll_to_selected = true;
    }
    if (enter && !query.empty() && s.selected < n)
    {
        return picked(&s.entries[static_cast<std::size_t>(s.hits[static_cast<std::size_t>(s.selected)].entry)]);
    }
    return nullptr;
}

const SignalEntry* signal_search_list(SignalSearch& s)
{
    const float row_h = ImGui::GetTextLineHeightWithSpacing();
    const int n = static_cast<int>(s.hits.size());
    int h_delta = 0;
    if (vim_nav(s.vim, s.selected, n, static_cast<int>(ImGui::GetWindowHeight() / row_h), s.focus_input, h_delta))
    {
        s.scroll_to_selected = true;
    }
    if (s.selected < 0 || s.selected >= n)
    {
        s.scroll_to_selected = false;
    }
    if (s.hits.empty())
    {
        ImGui::TextDisabled("No matching signal");
        return nullptr;
    }
    const SignalEntry* chosen = nullptr;
    // Same focus rule as the j/k above (vim_nav): Enter takes the row they moved to.
    if (s.selected >= 0 && s.selected < n && !ImGui::GetIO().WantTextInput
        && ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows | ImGuiFocusedFlags_NoPopupHierarchy)
        && ImGui::IsKeyPressed(ImGuiKey_Enter, false))
    {
        chosen = &s.entries[static_cast<std::size_t>(s.hits[static_cast<std::size_t>(s.selected)].entry)];
    }
    const ImU32 text_col = ImGui::GetColorU32(ImGuiCol_Text);
    const ImU32 match_col = ImGui::GetColorU32(ImGuiCol_CheckMark); // theme accent
    const ImU32 unit_col = ImGui::GetColorU32(ImGuiCol_TextDisabled);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImGuiListClipper clipper;
    clipper.Begin(n, row_h);
    if (s.scroll_to_selected)
    {
        clipper.IncludeItemByIndex(s.selected); // submitted even off-screen, so it can be scrolled to
    }
    while (clipper.Step())
    {
        for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row)
        {
            const SignalEntry& e = s.entries[static_cast<std::size_t>(s.hits[static_cast<std::size_t>(row)].entry)];
            ImGui::PushID(row);
            if (ImGui::Selectable("##hit", row == s.selected, ImGuiSelectableFlags_AllowDoubleClick))
            {
                s.selected = row;
                if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
                {
                    chosen = &e;
                }
            }
            ImGui::PopID();
            if (row == s.selected && std::exchange(s.scroll_to_selected, false))
            {
                ImGui::ScrollToItem(ImGuiScrollFlags_KeepVisibleEdgeY); // as little as it takes
            }
            // The label in runs: matched characters in the accent colour, the rest as text.
            static_cast<void>(fuzzy_score(s.query, e.label, &s.positions));
            ImVec2 pos = ImGui::GetItemRectMin();
            const auto put = [&](ImU32 col, const char* begin, const char* end)
            {
                dl->AddText(pos, col, begin, end);
                pos.x += ImGui::CalcTextSize(begin, end).x;
            };
            const char* text = e.label.data();
            std::size_t next = 0;
            const auto matched = [&](std::size_t i) { return next < s.positions.size() && static_cast<std::size_t>(s.positions[next]) == i; };
            for (std::size_t i = 0; i < e.label.size();)
            {
                const bool match = matched(i);
                std::size_t j = i;
                for (; j < e.label.size() && matched(j) == match; ++j)
                {
                    next += match ? 1 : 0;
                }
                put(match ? match_col : text_col, text + i, text + j);
                i = j;
            }
            if (const std::string& unit = unit_of(e); !unit.empty())
            {
                put(unit_col, " [", nullptr);
                put(unit_col, unit.data(), unit.data() + unit.size());
                put(unit_col, "]", nullptr);
            }
        }
    }
    return picked(chosen);
}
