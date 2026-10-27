#pragma once

#include <chrono>
#include <string>
#include <cstdint>

struct App;

// Status line at the bottom ("Ready" / "Measuring" / "Recording") and the connection +
// frame-rate corner at the right end of the main menu bar.
struct StatusBarState
{
    uint64_t frames = 0; // Trace::end at `sampled`
    std::chrono::steady_clock::time_point sampled{};
    double rate = 0.0;   // frames/s over the last second
    bool measuring = false; // as of the last sample
    std::string notice;     // shown after Ready / Measuring until notice_until
    std::chrono::steady_clock::time_point notice_until{};
};

// A message in the status bar for `seconds` (e.g. "big.log changed on disk, reloading").
void status_bar_notice(StatusBarState& s, std::string text, double seconds = 10.0);

// frames = Trace::end (counts on through clears and pruning). Re-bases while not measuring and
// on the first call of a measurement, so the first rate covers only that measurement's frames;
// then rate = new frames / elapsed time once at least a second has passed. No ImGui.
void status_bar_sample(StatusBarState& s, bool measuring, uint64_t frames, std::chrono::steady_clock::time_point now);

// Bottom side bar. Call before draw_workspace so it sits below the workspace tabs.
void draw_status_bar(App& app, StatusBarState& s);
// Right end of the main menu bar; called by draw_main_menu before EndMainMenuBar.
// Draws the update pill (ui/updater) left of it; true when that was clicked.
bool draw_menu_status(const App& app, const StatusBarState& s);
