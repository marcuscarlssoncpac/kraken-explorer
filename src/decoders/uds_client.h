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

// UDS client (ISO 14229 over ISO 15765-2 on classic CAN): sends a request as a single frame
// or segmented (first frame, then consecutive frames paced by the ECU's flow control), waits for
// the response with P2 / P2* (NRC 0x78 "response pending" extends the wait) and answers the
// ECU's first frame with a flow control. Blocking: call it from a worker, the Python thread or
// the REST thread, never from the main loop. Frames reach it through uds_client_rx, an
// RxConsumer on the RX threads.
//
// Address discovery: uds_learn pairs request and response ids from the traffic (passive),
// uds_functional sends one functional request and collects every ECU that answers.

#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <span>
#include <string>
#include <vector>

#include "core/bus_message.h"

struct UdsTarget
{
    uint16_t iface = 0;
    uint32_t tx_id = 0x7E0; // request id: physical, or functional (0x7DF, 0x18DB33F1; single-frame answers only)
    uint32_t rx_id = 0x7E8; // response id
    bool extended = false;  // 29-bit ids
    uint8_t padding = 0xCC; // fills every frame to 8 bytes
};

struct UdsTiming
{
    // ISO's P2 is 50 ms; gateways and USB adapters add latency, so wait longer before giving up.
    std::chrono::milliseconds p2{1000};
    std::chrono::milliseconds p2_star{5000}; // after NRC 0x78
    std::chrono::milliseconds n_bs{1000};    // the ECU's flow control after our first frame / block
    std::chrono::milliseconds n_cr{1000};    // between the ECU's consecutive frames
};

struct UdsResult
{
    std::vector<uint8_t> response; // positive (SID + 0x40, ...) or negative (7F SID NRC)
    std::string error;             // timeout, send failure, flow control overflow; empty when answered
    bool suppressed = false;       // suppressPosRspMsgIndicationBit set: no response was awaited
};

using UdsSend = std::function<bool(const BusMessage&)>;

struct UdsWaiter
{
    uint16_t iface = 0;
    bool extended = false;
    uint32_t id = 0;
    bool any_id = false;           // functional request: every id of the iface and id width
    std::vector<BusMessage> frames; // queued by uds_client_rx, guarded by UdsClient::mutex
};

// One per application; requests from several threads may run at once.
struct UdsClient
{
    std::mutex mutex;
    std::condition_variable cv;
    std::vector<UdsWaiter*> waiters;
    std::atomic<int> active{0}; // waiters.size(), read without the lock on the RX threads
    std::atomic<bool> cancel{false}; // every wait ends with an error (shutdown)
};

// RxConsumer: queues received frames for the requests waiting on them. Cheap when none waits.
void uds_client_rx(void* client, const BusMessage& msg);
// Ends every running and future request with an error: before the threads that call them join.
void uds_client_cancel(UdsClient& c);

[[nodiscard]] UdsResult uds_request(UdsClient& c, const UdsSend& send, const UdsTarget& t,
                                    std::span<const uint8_t> request, const UdsTiming& timing = {});

// Sends request as a single frame to t.tx_id and returns every response to it within window,
// one per responding id (rx_id is the responder's id, t.rx_id is ignored).
struct UdsResponder
{
    uint32_t rx_id = 0;
    std::vector<uint8_t> response;
};
[[nodiscard]] std::vector<UdsResponder> uds_functional(UdsClient& c, const UdsSend& send, const UdsTarget& t,
                                                       std::span<const uint8_t> request,
                                                       std::chrono::milliseconds window);

// Functional TesterPresent on 11-bit (0x7DF) and 29-bit (0x18DB33F1), every ECU that answers
// within window, by uds_ecu_from_responder.
[[nodiscard]] std::vector<struct UdsEcu> uds_scan(UdsClient& c, const UdsSend& send, uint16_t iface,
                                                   std::chrono::milliseconds window);

// Readable form of a response to request: DIDs as hex and text, DTCs, NRC names.
[[nodiscard]] std::string uds_describe(std::span<const uint8_t> request, std::span<const uint8_t> response);

// --- Address discovery ---

struct UdsEcu
{
    uint16_t iface = 0;
    bool extended = false;
    uint32_t tx_id = 0;   // tester -> ECU
    uint32_t rx_id = 0;   // ECU -> tester
    bool guessed = false; // tx_id inferred (answer to a functional request: rx_id - 8)
    std::string name;     // filled by the UI from DID F197 / F190 when it asks
};

struct UdsLearner
{
    struct Pending
    {
        uint16_t iface = 0;
        uint32_t id = 0;
        uint8_t sid = 0;
        int64_t ts_ns = 0;
    };
    std::vector<Pending> pending; // 11-bit requests waiting for their answer
    std::vector<UdsEcu> ecus;
};

// Learns from one frame (RX or TX echo) of the traffic. True when ecus changed.
bool uds_learn(UdsLearner& l, const BusMessage& frame);
// Adds or refines an ECU (same iface, id width and rx_id: an exact tx_id replaces a guess, a
// name is taken over).
// True when ecus changed.
bool uds_learner_add(UdsLearner& l, const UdsEcu& ecu);
// The ECU behind a responder of a functional request sent with t: 29-bit 0x18DA<TA><SA> is
// exact, 11-bit is the OBD convention (request = response - 8), marked guessed.
[[nodiscard]] UdsEcu uds_ecu_from_responder(const UdsTarget& t, uint32_t rx_id);
