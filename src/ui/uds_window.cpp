/*
  Copyright (c) 2026 Schildkroet

  This file is part of Kraken Explorer.

  Kraken Explorer is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 2 of the License, or
  (at your option) any later version.

  Kraken Explorer is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with Kraken Explorer.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "ui/uds_window.h"

#include <algorithm>
#include <cfloat>
#include <charconv>
#include <format>
#include <optional>

#include <imgui.h>
#include <imgui_internal.h>
#include <misc/cpp/imgui_stdlib.h>

#include "app.h"
#include "ui/diag_dialog.h"
#include "ui/raw_tx.h"
#include "ui/script_window.h"
#include "ui/workspace_tabs.h"

namespace
{

constexpr auto tester_present_period = std::chrono::seconds(2);
constexpr auto scan_window = std::chrono::milliseconds(300);

std::optional<uint32_t> parse_hex_id(std::string_view s)
{
    while (!s.empty() && s.front() == ' ')
    {
        s.remove_prefix(1);
    }
    if (s.starts_with("0x") || s.starts_with("0X"))
    {
        s.remove_prefix(2);
    }
    uint32_t v = 0;
    const auto [end, ec] = std::from_chars(s.data(), s.data() + s.size(), v, 16);
    if (ec != std::errc{} || end != s.data() + s.size() || v > can_id_mask_extended)
    {
        return std::nullopt;
    }
    return v;
}

void log_line(App& app, UdsWindow& w, const std::string& line)
{
    {
        const std::scoped_lock lock(w.mutex);
        if (w.log.empty())
        {
            w.log_start = std::chrono::steady_clock::now();
        }
        const double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - w.log_start).count();
        w.log += std::format("{:9.3f}  {}\n", t, line);
        ++w.log_total;
    }
    if (app.tasks.wake != nullptr)
    {
        app.tasks.wake();
    }
}

std::string target_text(const UdsTarget& t)
{
    return std::format("{:0{}X} -> {:0{}X}", t.tx_id, t.extended ? 8 : 3, t.rx_id, t.extended ? 8 : 3);
}

void worker_loop(std::stop_token stop, App& app, UdsWindow& w)
{
    auto next_tp = std::chrono::steady_clock::now();
    while (!stop.stop_requested())
    {
        std::function<void()> job;
        UdsTarget tp_target;
        bool tp = false;
        {
            std::unique_lock lock(w.mutex);
            w.cv.wait_until(lock, stop, next_tp, [&] { return !w.jobs.empty(); });
            if (stop.stop_requested())
            {
                return;
            }
            if (!w.jobs.empty())
            {
                job = std::move(w.jobs.front());
                w.jobs.pop_front();
                w.busy = true;
            }
            tp = w.tester_present;
            tp_target = w.target;
        }
        if (job)
        {
            job();
            const std::scoped_lock lock(w.mutex);
            w.busy = !w.jobs.empty();
        }
        if (std::chrono::steady_clock::now() >= next_tp)
        {
            next_tp = std::chrono::steady_clock::now() + tester_present_period;
            if (tp)
            {
                const std::array<uint8_t, 2> req = {0x3E, 0x80}; // no answer wanted
                (void)uds_request(app.uds_client, [&app](const BusMessage& m) { return uds_send(app, m); }, tp_target, req);
            }
        }
    }
}

void ensure_worker(App& app, UdsWindow& w)
{
    if (!w.worker.joinable())
    {
        w.worker = std::jthread([&app, &w](std::stop_token stop) { worker_loop(stop, app, w); });
    }
}

void post(App& app, UdsWindow& w, std::function<void()> job)
{
    ensure_worker(app, w);
    {
        const std::scoped_lock lock(w.mutex);
        w.jobs.push_back(std::move(job));
        w.busy = true;
    }
    w.cv.notify_all();
}

// The selected ECU, or the manual ids when they parse.
std::optional<UdsTarget> current_target(const UdsWindow& w)
{
    if (w.ecu >= 0 && w.ecu < static_cast<int>(w.learner.ecus.size()))
    {
        const UdsEcu& e = w.learner.ecus[static_cast<std::size_t>(w.ecu)];
        return UdsTarget{.iface = e.iface, .tx_id = e.tx_id, .rx_id = e.rx_id, .extended = e.extended};
    }
    const auto tx = parse_hex_id(w.tx_text);
    const auto rx = parse_hex_id(w.rx_text);
    if (!tx || !rx || (!w.extended && (*tx > can_id_mask_standard || *rx > can_id_mask_standard)))
    {
        return std::nullopt;
    }
    return UdsTarget{.iface = w.iface, .tx_id = *tx, .rx_id = *rx, .extended = w.extended};
}

void send_request(App& app, UdsWindow& w, std::vector<uint8_t> req)
{
    const std::optional<UdsTarget> t = current_target(w);
    if (!t || req.empty())
    {
        return;
    }
    post(app, w, [&app, &w, t = *t, req = std::move(req)]
    {
        log_line(app, w, std::format("{}  >> {}", target_text(t), format_hex_bytes(req)));
        const UdsResult r = uds_request(app.uds_client, [&app](const BusMessage& m) { return uds_send(app, m); }, t, req);
        if (!r.error.empty())
        {
            log_line(app, w, "   " + r.error);
        }
        else if (r.suppressed)
        {
            log_line(app, w, "   sent (no response requested)");
        }
        else
        {
            std::string text = uds_describe(req, r.response);
            for (std::size_t at = text.find('\n'); at != std::string::npos; at = text.find('\n', at + 1))
            {
                text.insert(at + 1, 13, ' '); // DTC lists: every line under the first
            }
            log_line(app, w, std::format("   << {}\n             {}", format_hex_bytes(r.response), text));
        }
    });
}

// Functional TesterPresent on 11 and 29 bit, then each answer's name (DID F197, else VIN F190).
void scan(App& app, UdsWindow& w, uint16_t iface)
{
    post(app, w, [&app, &w, iface]
    {
        const auto send = [&app](const BusMessage& m) { return uds_send(app, m); };
        log_line(app, w, "scan: 7DF and 18DB33F1 >> 3E 00");
        std::vector<UdsEcu> found = uds_scan(app.uds_client, send, iface, scan_window);
        const UdsTiming quick{.p2 = std::chrono::milliseconds(300), .p2_star = std::chrono::milliseconds(2000)};
        for (UdsEcu& e : found)
        {
            const UdsTarget t{.iface = e.iface, .tx_id = e.tx_id, .rx_id = e.rx_id, .extended = e.extended};
            for (const uint16_t did : {0xF197, 0xF190})
            {
                const std::array<uint8_t, 3> req = {0x22, static_cast<uint8_t>(did >> 8), static_cast<uint8_t>(did)};
                const UdsResult r = uds_request(app.uds_client, send, t, req, quick);
                if (r.response.size() > 3 && r.response[0] == 0x62)
                {
                    for (auto it = r.response.begin() + 3; it != r.response.end(); ++it)
                    {
                        if (*it >= 0x20 && *it < 0x7F)
                        {
                            e.name += static_cast<char>(*it);
                        }
                    }
                    break;
                }
            }
            log_line(app, w, std::format("   found {}{}{}", target_text(t), e.guessed ? " (request id guessed)" : "",
                                         e.name.empty() ? "" : "  " + e.name));
        }
        if (found.empty())
        {
            log_line(app, w, "   no ECU answered");
        }
        const std::scoped_lock lock(w.mutex);
        w.found.insert(w.found.end(), found.begin(), found.end());
    });
}

std::string ecu_label(const App& app, const UdsEcu& e)
{
    const std::string iface = e.iface < app.ifaces.size() ? app.ifaces[e.iface].info.name : std::to_string(e.iface);
    return std::format("{}: {}{}{}", iface, target_text({.tx_id = e.tx_id, .rx_id = e.rx_id, .extended = e.extended}),
                       e.guessed ? " ?" : "", e.name.empty() ? "" : "  " + e.name);
}

// Hex text of n bytes as a big-endian number (DID, routine id).
std::optional<std::vector<uint8_t>> hex_field(const std::string& text, std::size_t n)
{
    auto b = parse_hex_bytes(text);
    if (!b || b->size() != n)
    {
        return std::nullopt;
    }
    return b;
}

std::vector<uint8_t> concat(std::initializer_list<uint8_t> head, std::span<const uint8_t> a, std::span<const uint8_t> b = {})
{
    std::vector<uint8_t> out(head);
    out.insert(out.end(), a.begin(), a.end());
    out.insert(out.end(), b.begin(), b.end());
    return out;
}

} // namespace

bool uds_send(App& app, const BusMessage& m)
{
    return m.iface < app.ifaces.size() && iface_send(app.ifaces[m.iface], m);
}

void uds_window_on_rx(UdsWindow& w, std::span<const BusMessage> batch)
{
    for (const BusMessage& m : batch)
    {
        (void)uds_learn(w.learner, m);
    }
}

void uds_window_shutdown(App& app, UdsWindow& w)
{
    uds_client_cancel(app.uds_client);
    w.worker = {}; // request_stop + join
}

void draw_uds_window(App& app, UdsWindow& w, const WorkspaceTab& tab)
{
    // Scan results and the log come from the worker.
    bool changed = false;
    bool busy = false;
    {
        const std::scoped_lock lock(w.mutex);
        for (const UdsEcu& e : w.found)
        {
            uds_learner_add(w.learner, e);
        }
        w.found.clear();
        if (w.log_total != w.log_seen)
        {
            w.log_seen = w.log_total;
            w.log_view = w.log;
            changed = true;
        }
        busy = w.busy;
    }
    const std::optional<UdsTarget> target = current_target(w);
    if (target)
    {
        const std::scoped_lock lock(w.mutex);
        w.target = *target; // tester present follows the selection
    }
    if (!w.open)
    {
        return;
    }
    const float em = ImGui::GetFontSize();
    ImGui::SetNextWindowSize(ImVec2(em * 46.0f, em * 34.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin(workspace_window_name(tab, "UDS Diagnostics").c_str(), &w.open))
    {
        ImGui::End();
        return;
    }

    // --- Target ---
    const std::string preview = w.ecu >= 0 && w.ecu < static_cast<int>(w.learner.ecus.size())
        ? ecu_label(app, w.learner.ecus[static_cast<std::size_t>(w.ecu)]) : std::string("Manual");
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("ECU");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(em * 22.0f);
    if (ImGui::BeginCombo("##ecu", preview.c_str()))
    {
        if (ImGui::Selectable("Manual", w.ecu < 0))
        {
            w.ecu = -1;
        }
        for (std::size_t i = 0; i < w.learner.ecus.size(); ++i)
        {
            const std::string label = ecu_label(app, w.learner.ecus[i]) + std::format("##{}", i);
            if (ImGui::Selectable(label.c_str(), w.ecu == static_cast<int>(i)))
            {
                w.ecu = static_cast<int>(i);
            }
        }
        ImGui::EndCombo();
    }
    ImGui::SetItemTooltip("ECUs learned from the traffic (request/response pairs) and found by Scan.\n"
                          "? marks a request id guessed as response id - 8.");
    ImGui::SameLine();
    ImGui::BeginDisabled(busy);
    if (ImGui::Button("Scan"))
    {
        scan(app, w, w.ecu >= 0 ? w.learner.ecus[static_cast<std::size_t>(w.ecu)].iface : w.iface);
    }
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("Functional TesterPresent to 0x7DF and 0x18DB33F1 on the interface; every ECU that\n"
                          "answers is added to the list and asked for its name (DID F197, else the VIN)");
    if (w.ecu < 0)
    {
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Interface");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(em * 10.0f);
        draw_iface_combo("##iface", app, w.iface, BusType::CAN);
        ImGui::SameLine();
        ImGui::TextUnformatted("Request");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(em * 5.0f);
        ImGui::InputText("##tx", &w.tx_text, ImGuiInputTextFlags_CharsHexadecimal);
        ImGui::SameLine();
        ImGui::TextUnformatted("Response");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(em * 5.0f);
        ImGui::InputText("##rx", &w.rx_text, ImGuiInputTextFlags_CharsHexadecimal);
        ImGui::SameLine();
        ImGui::Checkbox("29-bit", &w.extended);
    }
    const bool ok = target.has_value();
    if (!ok)
    {
        ImGui::TextDisabled("Request and response ids must be hex (11-bit up to 7FF unless 29-bit)");
    }
    ImGui::Separator();

    // --- Services ---
    ImGui::BeginDisabled(!ok);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Session");
    ImGui::SameLine();
    for (const auto& [label, sub] : {std::pair{"Default", 0x01}, {"Programming", 0x02}, {"Extended", 0x03}})
    {
        if (ImGui::Button(label))
        {
            send_request(app, w, {0x10, static_cast<uint8_t>(sub)});
        }
        ImGui::SameLine();
    }
    {
        const std::scoped_lock lock(w.mutex);
        ImGui::Checkbox("Tester present", &w.tester_present);
    }
    ImGui::SetItemTooltip("3E 80 every 2 s to the selected ECU, keeps a non-default session open");
    if (w.tester_present)
    {
        ensure_worker(app, w); // it sends them
    }
    ImGui::SameLine();
    if (ImGui::Button("ECU Reset"))
    {
        send_request(app, w, {0x11, 0x01});
    }
    ImGui::SetItemTooltip("11 01 hard reset");

    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("DID");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(em * 4.0f);
    ImGui::InputText("##did", &w.did, ImGuiInputTextFlags_CharsHexadecimal);
    const auto did = hex_field(w.did, 2);
    ImGui::SameLine();
    ImGui::BeginDisabled(!did);
    if (ImGui::Button("Read"))
    {
        send_request(app, w, concat({0x22}, *did));
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(em * 14.0f);
    ImGui::InputTextWithHint("##did_data", "data (hex)", &w.did_data);
    const auto did_data = parse_hex_bytes(w.did_data);
    ImGui::SameLine();
    ImGui::BeginDisabled(!did_data || did_data->empty());
    if (ImGui::Button("Write"))
    {
        send_request(app, w, concat({0x2E}, *did, *did_data));
    }
    ImGui::EndDisabled();
    ImGui::EndDisabled();

    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("DTC");
    ImGui::SameLine();
    if (ImGui::Button("Read DTCs"))
    {
        send_request(app, w, {0x19, 0x02, 0xFF});
    }
    ImGui::SetItemTooltip("19 02 FF: every DTC with any status bit set");
    ImGui::SameLine();
    if (ImGui::Button("Clear DTCs"))
    {
        send_request(app, w, {0x14, 0xFF, 0xFF, 0xFF});
    }
    ImGui::SetItemTooltip("14 FF FF FF: all groups");

    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Routine");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(em * 4.0f);
    ImGui::InputText("##rid", &w.routine, ImGuiInputTextFlags_CharsHexadecimal);
    const auto rid = hex_field(w.routine, 2);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(em * 10.0f);
    ImGui::InputTextWithHint("##rid_data", "options (hex)", &w.routine_data);
    const auto rid_data = parse_hex_bytes(w.routine_data);
    ImGui::BeginDisabled(!rid || !rid_data);
    for (const auto& [label, sub] : {std::pair{"Start", 0x01}, {"Stop", 0x02}, {"Results", 0x03}})
    {
        ImGui::SameLine();
        if (ImGui::Button(label))
        {
            send_request(app, w, concat({0x31, static_cast<uint8_t>(sub)}, *rid, *rid_data));
        }
    }
    ImGui::EndDisabled();

    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Raw");
    ImGui::SameLine();
    const auto raw = parse_hex_bytes(w.raw);
    ImGui::SetNextItemWidth(-ImGui::CalcTextSize("Send").x - ImGui::GetStyle().FramePadding.x * 2.0f - ImGui::GetStyle().ItemSpacing.x);
    const bool enter = ImGui::InputTextWithHint("##raw", "request bytes (hex), Enter sends", &w.raw, ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    ImGui::BeginDisabled(!raw || raw->empty());
    if ((ImGui::Button("Send") || enter) && raw && !raw->empty())
    {
        send_request(app, w, *raw);
    }
    ImGui::EndDisabled();
    ImGui::EndDisabled(); // !ok

    if (ImGui::Button("Clear log"))
    {
        const std::scoped_lock lock(w.mutex);
        w.log.clear();
        ++w.log_total;
    }
    if (busy)
    {
        ImGui::SameLine();
        ImGui::TextDisabled("waiting for the ECU...");
    }

    ImGui::PushFont(app.fonts.mono, 0.0f);
    draw_console_box("##log", w.log_view, -FLT_MIN, -FLT_MIN, changed, w.log_follow, w.log_active);
    ImGui::PopFont();
    ImGui::End();
}
