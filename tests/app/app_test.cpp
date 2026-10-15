// app_measurement_start: pressing "Release the Kraken" with no interface selected, or with
// every enabled interface down, must refuse to start and tell the user what to do.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <string>
#include <thread>

#include <imgui.h>

#include "app.h"
#include "drivers/driver.h"
#include "ui_test.h"

namespace
{

const DriverOps down_driver = {
    .name = "Fake",
    .enumerate = nullptr,
    .open = [](Iface&, const IfaceConfig&) { return false; }, // the link is down
    .close = [](Iface&) {},
    .send = [](Iface&, const BusMessage&) { return false; },
    .read = [](Iface&, BusMessage*, int, int) { return 0; },
    .stats = nullptr,
    .lin_sleep_wakeup = nullptr,
    .lin_set_schedule = nullptr,
    .lin_diag_request = nullptr,
};

const DriverOps up_driver = {
    .name = "Fake",
    .enumerate = nullptr,
    .open = [](Iface&, const IfaceConfig&) { return true; },
    .close = [](Iface&) {},
    .send = [](Iface&, const BusMessage&) { return true; },
    .read = [](Iface&, BusMessage*, int, int timeout_ms)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(std::min(timeout_ms, 5)));
        return 0;
    },
    .stats = nullptr,
    .lin_sleep_wakeup = nullptr,
    .lin_set_schedule = nullptr,
    .lin_diag_request = nullptr,
};

void add_fake(App& app, const DriverOps& ops, const char* name, bool enabled = true)
{
    Iface& i = app.ifaces.emplace_back();
    i.ops = &ops;
    i.index = static_cast<uint16_t>(app.ifaces.size() - 1);
    i.info.name = name;
    if (app.setup.networks.empty())
    {
        app.setup.networks.push_back({.name = "Net"});
    }
    app.setup.networks[0].interfaces.push_back({.driver = "Fake", .name = name, .enabled = enabled});
}

} // namespace

TEST_CASE("start with no interface selected is refused with an actionable error")
{
    const UiTest ui;
    App app;
    app_measurement_start(app);
    CHECK_FALSE(app.measuring);                                             // nothing to measure on
    CHECK(app.measurement_error.find("Setup Interface") != std::string::npos); // ... and this is the fix
    app_measurement_stop(app);
}

TEST_CASE("start with every enabled interface down is refused and names the interface")
{
    const UiTest ui;
    App app;
    add_fake(app, down_driver, "fake0");
    app.setup.networks[0].interfaces.push_back({.driver = "Fake", .name = "gone0", .enabled = true}); // not enumerated
    app_measurement_start(app);
    CHECK_FALSE(app.measuring);
    CHECK(app.measurement_error.find("fake0") != std::string::npos);
    CHECK(app.measurement_error.find("gone0") != std::string::npos);
    app_measurement_stop(app);
}

TEST_CASE("start with one interface up and one down still starts, with a notice")
{
    const UiTest ui;
    App app;
    add_fake(app, up_driver, "fake0");
    add_fake(app, down_driver, "fake1");
    app_measurement_start(app);
    CHECK(app.measuring);
    CHECK(app.measurement_error.empty()); // a notice, not the blocking error
    app_measurement_stop(app);
}

TEST_CASE("the refused start shows the Cannot start measurement modal until closed")
{
    const UiTest ui;
    App app;
    app.measurement_error = "No interface selected.";
    for (int i = 0; i < 2; ++i)
    {
        ImGui::NewFrame();
        draw_measurement_error(app);
        ImGui::EndFrame();
    }
    ImGui::NewFrame();
    CHECK(ImGui::IsPopupOpen("Cannot start measurement"));
    draw_measurement_error(app);
    ImGui::EndFrame();
}
