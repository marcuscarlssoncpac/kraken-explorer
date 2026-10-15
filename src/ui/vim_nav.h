#pragma once

#include <span>
#include <string>
#include <vector>

// Vim motions for list-like windows and for moving focus between the windows of a tab.
// No <imgui.h> here: file_dialog.h (and through it app.h) includes this header.
// Letters are read as typed characters (io.InputQueueCharacters), so they work on any keyboard layout.
// The keys as (keys, action) rows, also shown by the "?" help overlay; keep in step with vim_nav.cpp.
struct VimKey
{
    const char* keys;
    const char* action;
};
inline constexpr VimKey vim_keys[] = {
    {"j / k", "Next / previous row"},
    {"gg / G", "First / last row"},
    {"Ctrl+d / Ctrl+u", "Half a page down / up"},
    {"Ctrl+f / Ctrl+b", "A page down / up"},
    {"h / l", "Left / right (parent / child, tab, scroll)"},
    {"/", "Focus the search field"},
    {"Shift+h/j/k/l", "Focus the window left / below / above / right"},
    {"Ctrl+w h/j/k/l", "Same, vim style"},
    {"Ctrl+w w", "Focus the next window"},
    {"1 - 9", "Switch to workspace tab N"},
    {"y", "Copy menu for the selected row: y = whole row, letter = one column"},
};

struct VimNav
{
    double g_time = -1.0; // ImGui::GetTime() of a first 'g' waiting for the second, -1 = none
    double w_time = -1.0; // time of Ctrl+w waiting for h/j/k/l/w (vim_window_nav), -1 = none
    unsigned w_came_from = 0; // ImGuiID of the window the last Ctrl+w move left, 0 = none
    int yank_menu = 0;    // vim_yank_menu: 1 = open it next call, 2 = open, 0 = closed
};

// One thing the copy menu offers: its column label and the text that goes to the clipboard.
struct VimYankItem
{
    std::string label;
    std::string text;
};

// Call inside the list's window (or its table) every frame. selected is clamped to
// [0, count); -1 = nothing selected (j/k then start at row 0). Returns true when selected
// changed. After a move the caller scrolls to the row: with an ImGuiListClipper call
// clipper.IncludeItemByIndex(selected) after Begin() and ImGui::SetScrollHereY() on that row.
bool vim_nav(VimNav& v, int& selected, int count, int page_rows, bool& focus_search, int& h_delta);

// True once when 'y' was typed in the focused window (no text field active); the key is consumed.
[[nodiscard]] bool vim_yank();

// Which-key style copy menu (nvim's y + motion): a popup at the bottom of the current window
// listing items, y copies the first (the whole row), the shown letter copies that item, Esc or
// a click outside closes. Set v.yank_menu = 1 with the items ready, then call every frame from
// the same window; the popup stays until v.yank_menu is 0 again.
void vim_yank_menu(VimNav& v, std::span<const VimYankItem> items);

struct VimRect
{
    float x0 = 0.0f;
    float y0 = 0.0f;
    float x1 = 0.0f;
    float y1 = 0.0f;
};

enum class VimDir
{
    Left,
    Down,
    Up,
    Right,
    Next, // Ctrl+w w: the following window, wrapping
};

// Index of the window whose centre is nearest to rects[from]'s centre and lies in the
// half-plane of dir; -1 when there is none. from outside the span picks 0. prefer (the window
// the last move came from) wins when it lies in that half-plane, so Ctrl+w l, Ctrl+w h returns.
[[nodiscard]] int vim_pick_window(std::span<const VimRect> rects, int from, VimDir dir, int prefer = -1) noexcept;

// Ctrl+w motions between the visible windows of workspace tab tab_uid (names "...@<uid>",
// see workspace_window_name). The focused window shows the theme's selected tab / title colour.
void vim_window_nav(VimNav& v, unsigned tab_uid);
