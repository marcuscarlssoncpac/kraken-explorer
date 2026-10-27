#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <string>

#include <imgui.h>
#include <imgui_internal.h> // FindWindowByName, ActivateItemByID

#include "app.h"
#include "ui_test.h"
#include "ui/script_window.h"
#include "ui/workspace_tabs.h"

// No python_init: the console is a plain mutex + vector, and python_poll only joins a finished
// worker, so the window draws without an interpreter.
static void draw_frame(App& app, const WorkspaceTab& tab)
{
    ImGui::NewFrame();
    ImGui::SetNextWindowPos({0, 0});
    ImGui::SetNextWindowSize({800, 600});
    draw_script_window(app, app.script, tab);
    ImGui::EndFrame();
}

static void key(ImGuiKey k, bool down)
{
    ImGui::GetIO().AddKeyEvent(k, down);
}

TEST_CASE("the console text box shows the runs joined, clears with the console, and copies")
{
    const UiTest ui;
    App app;
    const WorkspaceTab tab{.title = "Script", .uid = 1};

    python_console_append(app.python, "hello\n", false);
    python_console_append(app.python, "boom\n", true);
    draw_frame(app, tab);
    draw_frame(app, tab);
    CHECK(app.script.console_text == "hello\nboom\n");

    SUBCASE("Clear empties the box")
    {
        python_console_clear(app.python);
        draw_frame(app, tab);
        CHECK(app.script.console_text.empty());
    }

    SUBCASE("Ctrl+A, Ctrl+C in the focused box copies the output")
    {
        ImGuiWindow* win = ImGui::FindWindowByName(workspace_window_name(tab, "Python Script").c_str());
        REQUIRE(win != nullptr);
        ImGui::ActivateItemByID(win->GetID("##console"));
        draw_frame(app, tab); // activation is processed when the item is drawn again
        draw_frame(app, tab); // the active box requests its Ctrl+A / Ctrl+C routes; granted a frame later
        ImGui::SetClipboardText("");
        for (const ImGuiKey k : {ImGuiKey_A, ImGuiKey_C})
        {
            key(ImGuiMod_Ctrl, true);
            key(k, true);
            draw_frame(app, tab);
            key(k, false);
            key(ImGuiMod_Ctrl, false);
            draw_frame(app, tab);
        }
        CHECK(std::string(ImGui::GetClipboardText()) == "hello\nboom\n");
    }
}
