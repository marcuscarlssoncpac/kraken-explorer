#include "ui/vim_nav.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <format>
#include <string_view>
#include <utility>

#include <imgui.h>
#include <imgui_internal.h> // ImGuiContext::Windows / NavWindow

namespace
{

constexpr double gg_timeout = 0.5; // seconds between the two g of gg
constexpr double ctrl_w_timeout = 1.0;

bool listening()
{
    return !ImGui::GetIO().WantTextInput;
}

// The window move of a key after Ctrl+w: a typed letter, or the letter with Ctrl still held.
bool window_dir(VimDir& dir)
{
    ImGuiIO& io = ImGui::GetIO();
    constexpr std::array<std::pair<ImGuiKey, VimDir>, 5> keys = {{
        {ImGuiKey_H, VimDir::Left},
        {ImGuiKey_J, VimDir::Down},
        {ImGuiKey_K, VimDir::Up},
        {ImGuiKey_L, VimDir::Right},
        {ImGuiKey_W, VimDir::Next},
    }};
    if (io.KeyCtrl)
    {
        for (const auto& [key, d] : keys)
        {
            if (ImGui::IsKeyPressed(key, false))
            {
                dir = d;
                return true;
            }
        }
        return false;
    }
    if (io.InputQueueCharacters.empty())
    {
        return false;
    }
    const ImWchar c = io.InputQueueCharacters[0];
    io.InputQueueCharacters.resize(0); // the key belongs to the window move, not the focused list
    switch (c)
    {
    case 'h': dir = VimDir::Left; return true;
    case 'j': dir = VimDir::Down; return true;
    case 'k': dir = VimDir::Up; return true;
    case 'l': dir = VimDir::Right; return true;
    case 'w': dir = VimDir::Next; return true;
    default: return false;
    }
}

// The list's window or a child of it has focus. NoPopupHierarchy: an open popup (the copy
// menu) takes the keys away from the list under it.
bool list_focused()
{
    return ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows | ImGuiFocusedFlags_NoPopupHierarchy);
}

// Menu letter of every item: y for the first, else the first letter of the label not taken
// yet ("Rx Frames" r, "Rx Errors" x, "Rx Overrun" o), 0 when the label has none left.
std::string yank_keys(std::span<const VimYankItem> items)
{
    std::string keys(items.size(), '\0');
    std::string used = "y";
    for (std::size_t i = 1; i < items.size(); ++i)
    {
        for (const char ch : items[i].label)
        {
            const char k = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
            if (std::isalpha(static_cast<unsigned char>(k)) && used.find(k) == std::string::npos)
            {
                keys[i] = k;
                used += k;
                break;
            }
        }
    }
    if (!items.empty())
    {
        keys[0] = 'y';
    }
    return keys;
}

} // namespace

bool vim_nav(VimNav& v, int& selected, int count, int page_rows, bool& focus_search, int& h_delta)
{
    if (!listening() || !list_focused())
    {
        return false;
    }
    const ImGuiIO& io = ImGui::GetIO();
    const double now = ImGui::GetTime();
    const int page = std::max(page_rows, 1);
    int target = selected;
    bool jumped = false; // gg / G move even when selected is -1
    for (const ImWchar c : io.InputQueueCharacters)
    {
        if (c == 'g')
        {
            if (v.g_time >= 0.0 && now - v.g_time <= gg_timeout)
            {
                target = 0;
                jumped = true;
                v.g_time = -1.0;
            }
            else
            {
                v.g_time = now;
            }
            continue;
        }
        v.g_time = -1.0;
        switch (c)
        {
        case 'j': target = std::max(target, -1) + 1; break;
        case 'k': target -= 1; break;
        case 'G': target = count - 1; jumped = true; break;
        case 'h': --h_delta; break;
        case 'l': ++h_delta; break;
        case '/': focus_search = true; break;
        default: break;
        }
    }
    if (io.KeyCtrl && !io.KeyShift && !io.KeyAlt)
    {
        target += ImGui::IsKeyPressed(ImGuiKey_D) ? page / 2 + page % 2 : 0;
        target -= ImGui::IsKeyPressed(ImGuiKey_U) ? page / 2 + page % 2 : 0;
        target += ImGui::IsKeyPressed(ImGuiKey_F) ? page : 0;
        target -= ImGui::IsKeyPressed(ImGuiKey_B) ? page : 0;
    }
    if (count <= 0 || (target == selected && !jumped))
    {
        return false;
    }
    target = std::clamp(target, 0, count - 1);
    const bool moved = target != selected;
    selected = target;
    return moved;
}

int vim_pick_window(std::span<const VimRect> rects, int from, VimDir dir, int prefer) noexcept
{
    const int n = static_cast<int>(rects.size());
    if (n == 0)
    {
        return -1;
    }
    if (from < 0 || from >= n)
    {
        return 0;
    }
    if (dir == VimDir::Next)
    {
        return n > 1 ? (from + 1) % n : -1;
    }
    const auto centre = [&](int i, float& x, float& y) {
        const VimRect& r = rects[static_cast<std::size_t>(i)];
        x = (r.x0 + r.x1) * 0.5f;
        y = (r.y0 + r.y1) * 0.5f;
    };
    float cx = 0.0f;
    float cy = 0.0f;
    centre(from, cx, cy);
    int best = -1;
    float best_d = 0.0f;
    for (int i = 0; i < n; ++i)
    {
        float x = 0.0f;
        float y = 0.0f;
        centre(i, x, y);
        const float dx = x - cx;
        const float dy = y - cy;
        const bool ahead = dir == VimDir::Left ? dx < 0.0f : dir == VimDir::Right ? dx > 0.0f
                         : dir == VimDir::Up   ? dy < 0.0f
                                               : dy > 0.0f;
        if (i != from && ahead && i == prefer)
        {
            return i;
        }
        const float d = dx * dx + dy * dy;
        if (i != from && ahead && (best < 0 || d < best_d))
        {
            best = i;
            best_d = d;
        }
    }
    return best;
}

bool vim_yank()
{
    if (!listening() || !list_focused())
    {
        return false;
    }
    ImVector<ImWchar>& q = ImGui::GetIO().InputQueueCharacters;
    const auto it = std::find(q.begin(), q.end(), ImWchar('y'));
    if (it == q.end())
    {
        return false;
    }
    q.erase(it);
    return true;
}

void vim_yank_menu(VimNav& v, std::span<const VimYankItem> items)
{
    if (v.yank_menu == 1)
    {
        ImGui::OpenPopup("##yank");
        v.yank_menu = 2;
    }
    if (v.yank_menu != 2)
    {
        return;
    }
    const ImVec2 pos = ImGui::GetWindowPos();
    ImGui::SetNextWindowPos(ImVec2(pos.x, pos.y + ImGui::GetWindowSize().y), ImGuiCond_Appearing, ImVec2(0.0f, 1.0f));
    if (!ImGui::BeginPopup("##yank")) // Esc / click outside closed it
    {
        v.yank_menu = 0;
        return;
    }
    const std::string keys = yank_keys(items);
    int pick = -1;
    for (const ImWchar c : ImGui::GetIO().InputQueueCharacters)
    {
        if (const auto it = std::find(keys.begin(), keys.end(), static_cast<char>(c)); c < 128 && it != keys.end())
        {
            pick = static_cast<int>(it - keys.begin());
        }
    }
    ImGui::TextDisabled("Copy");
    if (ImGui::BeginTable("##items", 3, ImGuiTableFlags_SizingFixedFit))
    {
        for (std::size_t i = 0; i < items.size(); ++i)
        {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::PushID(static_cast<int>(i));
            if (ImGui::Selectable("##pick", false, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap))
            {
                pick = static_cast<int>(i);
            }
            ImGui::PopID();
            ImGui::SameLine();
            ImGui::TextColored(ImGui::GetStyleColorVec4(ImGuiCol_CheckMark), "%c", keys[i] != '\0' ? keys[i] : ' ');
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(items[i].label.c_str());
            ImGui::TableNextColumn();
            // One line of the text, cut at ~40 characters; the clipboard gets all of it.
            const std::string_view text = items[i].text;
            const std::size_t end = std::min(text.find('\n'), std::size_t{40});
            ImGui::TextDisabled("%.*s%s", static_cast<int>(std::min(end, text.size())), text.data(), end < text.size() ? "…" : "");
        }
        ImGui::EndTable();
    }
    if (pick >= 0)
    {
        ImGui::SetClipboardText(items[static_cast<std::size_t>(pick)].text.c_str());
        ImGui::GetIO().InputQueueCharacters.resize(0); // the letter was the menu's, not the list's
        ImGui::CloseCurrentPopup();
        v.yank_menu = 0;
    }
    ImGui::EndPopup();
}

void vim_window_nav(VimNav& v, unsigned tab_uid)
{
    ImGuiIO& io = ImGui::GetIO();
    if (!listening() || ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId))
    {
        v.w_time = -1.0;
        return;
    }
    const double now = ImGui::GetTime();
    VimDir dir = VimDir::Next;
    if (v.w_time >= 0.0 && now - v.w_time <= ctrl_w_timeout)
    {
        if (!window_dir(dir))
        {
            return;
        }
        v.w_time = -1.0;
    }
    else
    {
        v.w_time = -1.0;
        if (io.KeyCtrl && !io.KeyShift && !io.KeyAlt && ImGui::IsKeyPressed(ImGuiKey_W, false))
        {
            v.w_time = now;
            return;
        }
        // Shift+h/j/k/l: the typed character arrives upper-case, so any keyboard layout works.
        ImVector<ImWchar>& q = io.InputQueueCharacters;
        const auto it = std::find_if(q.begin(), q.end(), [](ImWchar c) { return c == 'H' || c == 'J' || c == 'K' || c == 'L'; });
        if (it == q.end() || !io.KeyShift)
        {
            return;
        }
        dir = *it == 'H' ? VimDir::Left : *it == 'J' ? VimDir::Down : *it == 'K' ? VimDir::Up : VimDir::Right;
        q.erase(it);
    }

    // Last frame's windows of this tab: top-level, drawn and not a hidden dock tab.
    std::array<char, 16> suffix_buf{};
    const auto end = std::format_to_n(suffix_buf.data(), suffix_buf.size(), "@{}", tab_uid).out;
    const std::string_view suffix(suffix_buf.data(), end);
    constexpr std::size_t max_windows = 32;
    std::array<VimRect, max_windows> rects{};
    std::array<ImGuiWindow*, max_windows> windows{};
    std::size_t n = 0;
    int from = -1;
    const ImGuiContext& g = *ImGui::GetCurrentContext();
    const ImGuiWindow* focused = g.NavWindow != nullptr ? g.NavWindow->RootWindow : nullptr;
    for (ImGuiWindow* w : g.Windows)
    {
        if (n == max_windows)
        {
            break;
        }
        // A docked window carries ImGuiWindowFlags_ChildWindow too (BeginDocked), and DockIsActive
        // is false for a node holding a single window: skip only windows docked nowhere. Hidden
        // covers the unselected tabs of a tabbed node.
        const bool child = (w->Flags & ImGuiWindowFlags_ChildWindow) != 0 && w->DockNode == nullptr;
        if (!w->WasActive || w->Hidden || child || !std::string_view(w->Name).ends_with(suffix))
        {
            continue;
        }
        from = w == focused ? static_cast<int>(n) : from;
        rects[n] = {.x0 = w->Pos.x, .y0 = w->Pos.y, .x1 = w->Pos.x + w->Size.x, .y1 = w->Pos.y + w->Size.y};
        windows[n++] = w;
    }
    int prefer = -1;
    for (std::size_t i = 0; i < n; ++i)
    {
        prefer = windows[i]->ID == v.w_came_from ? static_cast<int>(i) : prefer;
    }
    const int to = vim_pick_window(std::span(rects.data(), n), from, dir, prefer);
    if (to >= 0)
    {
        v.w_came_from = from >= 0 ? windows[static_cast<std::size_t>(from)]->ID : 0;
        ImGui::SetWindowFocus(windows[static_cast<std::size_t>(to)]->Name);
    }
}
