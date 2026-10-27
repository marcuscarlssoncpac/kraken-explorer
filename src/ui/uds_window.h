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

// The "UDS Diagnostics" window: pick an ECU (learned from the traffic, found by a functional
// scan, or typed in), then sessions, tester present, ECU reset, DIDs, DTCs, routines or a raw
// request. Requests run on the window's worker thread; the log below is a copyable text box.
// One state for every tab, like the Python Script window.

#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include "decoders/uds_client.h"

struct App;
struct WorkspaceTab;

struct UdsWindow
{
    bool open = false;
    UdsLearner learner;          // main thread: ECUs learned from the traffic and the scans
    int ecu = -1;                // learner.ecus index; -1: the manual ids below
    uint16_t iface = 0;          // manual target's interface (App::ifaces index)
    std::string tx_text = "7E0"; // manual request id, hex
    std::string rx_text = "7E8"; // manual response id, hex
    bool extended = false;       // manual target uses 29-bit ids
    std::string did = "F190";
    std::string did_data;
    std::string routine = "FF00";
    std::string routine_data;
    std::string raw = "22 F1 90";
    std::string log_view;        // copy of log for the read-only text box
    uint64_t log_seen = 0;
    bool log_follow = false;     // new lines last frame: scroll to the bottom
    bool log_active = false;     // the user is selecting in the log

    // Shared with the worker, guarded by mutex.
    std::mutex mutex;
    std::condition_variable_any cv; // waits on the worker's stop token too
    std::deque<std::function<void()>> jobs;
    std::string log;
    uint64_t log_total = 0;
    std::vector<UdsEcu> found;   // scan results for the main thread to merge
    UdsTarget target;            // current target, for tester present
    bool tester_present = false; // 3E 80 every 2 s to target
    bool busy = false;           // a job is running
    std::chrono::steady_clock::time_point log_start{}; // the log's first line
    std::jthread worker;         // last: stops and joins before the rest goes
};

// Main thread, every frame: learns ECUs from the batch of new frames.
void uds_window_on_rx(UdsWindow& w, std::span<const BusMessage> batch);

// Sends a frame on app.ifaces[m.iface]; for UdsClient requests from any thread.
bool uds_send(App& app, const BusMessage& m);

void draw_uds_window(App& app, UdsWindow& w, const WorkspaceTab& tab);

// Ends the running request and joins the worker: before the interfaces close (main.cpp).
void uds_window_shutdown(App& app, UdsWindow& w);
