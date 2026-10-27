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

#include "decoders/uds_client.h"

#include <algorithm>
#include <format>
#include <optional>
#include <thread>

#include "decoders/decoders.h"

namespace
{

using Clock = std::chrono::steady_clock;

constexpr int64_t pairing_window_ns = 2'000'000'000; // an 11-bit answer belongs to a request this recent
constexpr std::size_t max_pending = 32;
constexpr int max_fc_wait = 10; // N_WFTmax: flow control WAITs in a row before giving up

bool functional_id(const UdsTarget& t)
{
    return t.extended ? ((t.tx_id >> 16) & 0xFF) == 0xDB : t.tx_id == 0x7DF;
}

// Services with a sub-function byte, whose bit 7 is suppressPosRspMsgIndicationBit.
bool has_suppress_bit(uint8_t sid)
{
    switch (sid)
    {
    case 0x10: case 0x11: case 0x19: case 0x27: case 0x28: case 0x29: case 0x2C: case 0x31: case 0x3E:
    case 0x83: case 0x85: case 0x86: case 0x87:
        return true;
    default:
        return false;
    }
}

// The SIDs ISO 14229-1 assigns; anything else in a first byte is not a UDS request.
bool is_request_sid(uint8_t sid)
{
    switch (sid)
    {
    case 0x10: case 0x11: case 0x14: case 0x19: case 0x22: case 0x23: case 0x24: case 0x27: case 0x28:
    case 0x29: case 0x2A: case 0x2C: case 0x2E: case 0x2F: case 0x31: case 0x34: case 0x35: case 0x36:
    case 0x37: case 0x38: case 0x3D: case 0x3E: case 0x83: case 0x84: case 0x85: case 0x86: case 0x87:
        return true;
    default:
        return false;
    }
}

// Registered for its lifetime, so a reply that comes before the request call returns from
// send() is not missed.
struct WaiterScope
{
    UdsClient& c;
    UdsWaiter w;

    WaiterScope(UdsClient& client, UdsWaiter waiter) : c(client), w(std::move(waiter))
    {
        const std::scoped_lock lock(c.mutex);
        c.waiters.push_back(&w);
        c.active.store(static_cast<int>(c.waiters.size()), std::memory_order_relaxed);
    }
    ~WaiterScope()
    {
        const std::scoped_lock lock(c.mutex);
        std::erase(c.waiters, &w);
        c.active.store(static_cast<int>(c.waiters.size()), std::memory_order_relaxed);
    }
    WaiterScope(const WaiterScope&) = delete;
    WaiterScope& operator=(const WaiterScope&) = delete;

    std::optional<BusMessage> next(Clock::time_point deadline)
    {
        std::unique_lock lock(c.mutex);
        if (!c.cv.wait_until(lock, deadline, [this] { return !w.frames.empty() || c.cancel.load(); }) || c.cancel.load())
        {
            return std::nullopt;
        }
        const BusMessage m = w.frames.front();
        w.frames.erase(w.frames.begin());
        return m;
    }
};

BusMessage frame_for(const UdsTarget& t, uint32_t id)
{
    BusMessage m{};
    m.id = id;
    m.iface = t.iface;
    m.flags = t.extended ? bus_flag::extended : 0;
    set_length(m, 8);
    m.data.fill(t.padding);
    return m;
}

bool send_frame(const UdsSend& send, const UdsTarget& t, std::span<const uint8_t> bytes)
{
    BusMessage m = frame_for(t, t.tx_id);
    std::ranges::copy(bytes.first(std::min<std::size_t>(bytes.size(), 8)), m.data.begin());
    return send(m);
}

// STmin byte of a flow control (ISO 15765-2): 0..0x7F ms, 0xF1..0xF9 100..900 us, else 127 ms.
std::chrono::microseconds st_min(uint8_t v)
{
    if (v <= 0x7F)
    {
        return std::chrono::milliseconds(v);
    }
    if (v >= 0xF1 && v <= 0xF9)
    {
        return std::chrono::microseconds((v - 0xF0) * 100);
    }
    return std::chrono::milliseconds(127);
}

// First UDS byte of an ISO-TP single or first frame (nullopt for CF / FC / anything else).
std::optional<std::span<const uint8_t>> first_payload(const BusMessage& m)
{
    const int len = std::min<int>(m.len, 8);
    if (len < 2)
    {
        return std::nullopt;
    }
    const uint8_t pci = m.data[0] >> 4;
    if (pci == 0)
    {
        const int n = m.data[0] & 0x0F;
        if (n < 1 || n > len - 1)
        {
            return std::nullopt;
        }
        return std::span<const uint8_t>(m.data.data() + 1, static_cast<std::size_t>(n));
    }
    if (pci == 1 && len >= 3)
    {
        return std::span<const uint8_t>(m.data.data() + 2, static_cast<std::size_t>(len - 2));
    }
    return std::nullopt;
}

} // namespace

void uds_client_cancel(UdsClient& c)
{
    c.cancel.store(true);
    c.cv.notify_all();
}

void uds_client_rx(void* client, const BusMessage& msg)
{
    UdsClient& c = *static_cast<UdsClient*>(client);
    if (c.active.load(std::memory_order_relaxed) == 0 || has_flag(msg, bus_flag::tx) || is_error_frame(msg)
        || has_flag(msg, bus_flag::rtr) || msg.type != BusType::CAN)
    {
        return;
    }
    const bool ext = has_flag(msg, bus_flag::extended);
    bool queued = false;
    {
        const std::scoped_lock lock(c.mutex);
        for (UdsWaiter* w : c.waiters)
        {
            if (w->iface == msg.iface && w->extended == ext && (w->any_id || w->id == can_id(msg)))
            {
                w->frames.push_back(msg);
                queued = true;
            }
        }
    }
    if (queued)
    {
        c.cv.notify_all();
    }
}

UdsResult uds_request(UdsClient& c, const UdsSend& send, const UdsTarget& t, std::span<const uint8_t> request,
                      const UdsTiming& timing)
{
    UdsResult r;
    if (request.empty() || request.size() > 4095)
    {
        r.error = "request must be 1..4095 bytes";
        return r;
    }
    const uint8_t sid = request[0];
    const auto send_error = [&t] { return std::format("interface {} not open or send failed", t.iface); };
    WaiterScope scope(c, {.iface = t.iface, .extended = t.extended, .id = t.rx_id});

    if (request.size() <= 7)
    {
        std::array<uint8_t, 8> sf{};
        sf[0] = static_cast<uint8_t>(request.size());
        std::ranges::copy(request, sf.begin() + 1);
        if (!send_frame(send, t, std::span(sf).first(request.size() + 1)))
        {
            r.error = send_error();
            return r;
        }
    }
    else
    {
        std::array<uint8_t, 8> ff{};
        ff[0] = static_cast<uint8_t>(0x10 | (request.size() >> 8));
        ff[1] = static_cast<uint8_t>(request.size() & 0xFF);
        std::ranges::copy(request.first(6), ff.begin() + 2);
        if (!send_frame(send, t, ff))
        {
            r.error = send_error();
            return r;
        }
        std::size_t at = 6;
        uint8_t sn = 1;
        while (at < request.size())
        {
            // Wait for the flow control: CTS (go), WAIT (keep waiting), overflow (give up).
            uint8_t block = 0;
            std::chrono::microseconds gap{};
            int waits = 0;
            for (auto deadline = Clock::now() + timing.n_bs;;)
            {
                const std::optional<BusMessage> m = scope.next(deadline);
                if (!m)
                {
                    r.error = c.cancel.load() ? "cancelled" : "no flow control from the ECU (N_Bs)";
                    return r;
                }
                if (m->len < 3 || (m->data[0] >> 4) != 3)
                {
                    continue;
                }
                const uint8_t fs = m->data[0] & 0x0F;
                if (fs == 1)
                {
                    if (++waits > max_fc_wait)
                    {
                        r.error = "the ECU kept asking to wait (N_WFTmax)";
                        return r;
                    }
                    deadline = Clock::now() + timing.n_bs;
                    continue;
                }
                if (fs != 0)
                {
                    r.error = "the ECU refused the request size (flow control overflow)";
                    return r;
                }
                block = m->data[1];
                gap = st_min(m->data[2]);
                break;
            }
            for (int sent = 0; at < request.size() && (block == 0 || sent < block); ++sent)
            {
                std::array<uint8_t, 8> cf{};
                cf[0] = static_cast<uint8_t>(0x20 | sn);
                const std::size_t n = std::min<std::size_t>(7, request.size() - at);
                std::ranges::copy(request.subspan(at, n), cf.begin() + 1);
                if (!send_frame(send, t, std::span(cf).first(n + 1)))
                {
                    r.error = send_error();
                    return r;
                }
                at += n;
                sn = static_cast<uint8_t>((sn + 1) & 0x0F);
                if (at < request.size() && gap.count() > 0)
                {
                    std::this_thread::sleep_for(gap);
                }
            }
        }
    }

    if (request.size() >= 2 && has_suppress_bit(sid) && (request[1] & 0x80) != 0)
    {
        r.suppressed = true;
        return r;
    }

    bool pending = false; // NRC 0x78 seen: the wait is P2*
    for (auto deadline = Clock::now() + timing.p2;;)
    {
        const std::optional<BusMessage> m = scope.next(deadline);
        if (!m)
        {
            r.error = c.cancel.load() ? "cancelled"
                                      : std::format("no response within {} ms{}", (pending ? timing.p2_star : timing.p2).count(),
                                                    pending ? " after response pending" : "");
            return r;
        }
        const int len = std::min<int>(m->len, 8);
        const uint8_t pci = m->data[0] >> 4;
        std::vector<uint8_t> payload;
        if (pci == 0)
        {
            const int n = std::min(m->data[0] & 0x0F, len - 1);
            payload.assign(m->data.begin() + 1, m->data.begin() + 1 + std::max(n, 0));
        }
        else if (pci == 1 && len == 8)
        {
            const std::size_t size = static_cast<std::size_t>((m->data[0] & 0x0F) << 8 | m->data[1]);
            if (size <= 7)
            {
                continue; // not a valid first frame
            }
            if (functional_id(t))
            {
                // A flow control may not go to a functional address: the caller needs the physical id.
                r.error = "multi-frame response to a functional request: use the ECU's physical request id";
                return r;
            }
            payload.assign(m->data.begin() + 2, m->data.begin() + 8);
            // Clear to send everything, no gap: the tester reads as fast as frames come.
            const std::array<uint8_t, 3> fc = {0x30, 0x00, 0x00};
            if (!send_frame(send, t, fc))
            {
                r.error = send_error();
                return r;
            }
            uint8_t sn = 1;
            while (payload.size() < size)
            {
                const std::optional<BusMessage> cf = scope.next(Clock::now() + timing.n_cr);
                if (!cf)
                {
                    r.error = std::format("response cut off after {} of {} bytes (N_Cr)", payload.size(), size);
                    return r;
                }
                if ((cf->data[0] >> 4) != 2)
                {
                    continue;
                }
                if ((cf->data[0] & 0x0F) != sn)
                {
                    r.error = std::format("consecutive frame {} where {} was expected", cf->data[0] & 0x0F, sn);
                    return r;
                }
                const int n = std::min<int>(std::min<int>(cf->len, 8) - 1, static_cast<int>(size - payload.size()));
                payload.insert(payload.end(), cf->data.begin() + 1, cf->data.begin() + 1 + std::max(n, 0));
                sn = static_cast<uint8_t>((sn + 1) & 0x0F);
            }
        }
        if (payload.empty())
        {
            continue; // flow control, stray consecutive frame
        }
        if (payload[0] == 0x7F && payload.size() >= 3 && payload[1] == sid)
        {
            if (payload[2] == 0x78) // response pending
            {
                pending = true;
                deadline = Clock::now() + timing.p2_star;
                continue;
            }
            r.response = std::move(payload);
            return r;
        }
        if (payload[0] == static_cast<uint8_t>(sid + 0x40))
        {
            r.response = std::move(payload);
            return r;
        }
        // Someone else's answer on the same id: keep waiting for ours.
    }
}

std::vector<UdsResponder> uds_functional(UdsClient& c, const UdsSend& send, const UdsTarget& t,
                                         std::span<const uint8_t> request, std::chrono::milliseconds window)
{
    std::vector<UdsResponder> out;
    if (request.empty() || request.size() > 7)
    {
        return out;
    }
    const uint8_t sid = request[0];
    WaiterScope scope(c, {.iface = t.iface, .extended = t.extended, .any_id = true});
    std::array<uint8_t, 8> sf{};
    sf[0] = static_cast<uint8_t>(request.size());
    std::ranges::copy(request, sf.begin() + 1);
    if (!send_frame(send, t, std::span(sf).first(request.size() + 1)))
    {
        return out;
    }
    const uint8_t tester = static_cast<uint8_t>(t.tx_id & 0xFF);
    for (const auto deadline = Clock::now() + window;;)
    {
        const std::optional<BusMessage> m = scope.next(deadline);
        if (!m)
        {
            return out;
        }
        const uint32_t id = can_id(*m);
        // 29-bit: only physical answers to this tester (0x18DA<tester><ECU>).
        if (t.extended && (((id >> 16) & 0xFF) != 0xDA || ((id >> 8) & 0xFF) != tester))
        {
            continue;
        }
        const auto payload = first_payload(*m);
        if (!payload || (m->data[0] >> 4) != 0)
        {
            continue;
        }
        const std::span<const uint8_t> p = *payload;
        const bool answer = p[0] == static_cast<uint8_t>(sid + 0x40) || (p[0] == 0x7F && p.size() >= 2 && p[1] == sid);
        if (answer && std::ranges::none_of(out, [id](const UdsResponder& r) { return r.rx_id == id; }))
        {
            out.push_back({.rx_id = id, .response = {p.begin(), p.end()}});
        }
    }
}

std::vector<UdsEcu> uds_scan(UdsClient& c, const UdsSend& send, uint16_t iface, std::chrono::milliseconds window)
{
    std::vector<UdsEcu> found;
    const std::array<uint8_t, 2> tester_present = {0x3E, 0x00};
    for (const bool ext : {false, true})
    {
        const UdsTarget t{.iface = iface, .tx_id = ext ? 0x18DB33F1u : 0x7DFu, .extended = ext};
        for (const UdsResponder& r : uds_functional(c, send, t, tester_present, window))
        {
            found.push_back(uds_ecu_from_responder(t, r.rx_id));
        }
    }
    return found;
}

std::string uds_describe(std::span<const uint8_t> request, std::span<const uint8_t> response)
{
    if (response.empty())
    {
        return {};
    }
    if (response[0] == 0x7F)
    {
        return response.size() >= 3 ? std::format("NRC 0x{:02X} {}", response[2], uds_nrc_name(response[2]))
                                    : "negative response";
    }
    const auto ascii = [](std::span<const uint8_t> b)
    {
        std::string s;
        for (const uint8_t c : b)
        {
            s += c >= 0x20 && c < 0x7F ? static_cast<char>(c) : '.';
        }
        return s;
    };
    const auto hex = [](std::span<const uint8_t> b)
    {
        std::string s;
        for (const uint8_t c : b)
        {
            s += std::format("{}{:02X}", s.empty() ? "" : " ", c);
        }
        return s;
    };
    switch (response[0])
    {
    case 0x50:
        return response.size() >= 2 ? std::format("session 0x{:02X} active", response[1]) : "session changed";
    case 0x51:
        return "ECU reset accepted";
    case 0x54:
        return "DTCs cleared";
    case 0x62:
        if (response.size() >= 3)
        {
            const auto data = response.subspan(3);
            return std::format("DID {:02X}{:02X} = {}  \"{}\"", response[1], response[2], hex(data), ascii(data));
        }
        break;
    case 0x6E:
        if (response.size() >= 3)
        {
            return std::format("DID {:02X}{:02X} written", response[1], response[2]);
        }
        break;
    case 0x71:
        if (response.size() >= 4)
        {
            return std::format("routine {:02X}{:02X} sub 0x{:02X}{}{}", response[2], response[3], response[1],
                               response.size() > 4 ? ": " : "", hex(response.subspan(4)));
        }
        break;
    case 0x59:
        // 19 02 (by status mask): 59 02 <availability mask> then DTC (3 bytes) + status, repeated.
        if (response.size() >= 3 && request.size() >= 2 && (request[1] == 0x02 || request[1] == 0x0A))
        {
            std::string s;
            int n = 0;
            for (std::size_t i = 3; i + 4 <= response.size(); i += 4, ++n)
            {
                // The ISO 15031-6 letter is in the top two bits (P C B U), the rest as digits.
                const uint8_t hi = response[i];
                s += std::format("\n  {}{:01X}{:01X}{:02X}-{:02X}  status 0x{:02X}", "PCBU"[hi >> 6], (hi >> 4) & 0x3,
                                 hi & 0x0F, response[i + 1], response[i + 2], response[i + 3]);
            }
            return std::format("{} DTC{}{}", n, n == 1 ? "" : "s", s);
        }
        break;
    case 0x7E:
        return "tester present";
    case 0x67:
        if (response.size() >= 2)
        {
            return response[1] % 2 == 1 ? std::format("seed {}", hex(response.subspan(2))) : "security access granted";
        }
        break;
    default:
        break;
    }
    return std::format("positive response to {}", uds_service_name(static_cast<uint8_t>(response[0] - 0x40)));
}

UdsEcu uds_ecu_from_responder(const UdsTarget& t, uint32_t rx_id)
{
    UdsEcu e{.iface = t.iface, .extended = t.extended, .rx_id = rx_id};
    if (t.extended)
    {
        // 0x18DA<TA=tester><SA=ECU> answers 0x18DA<ECU><tester>.
        const uint32_t ecu = rx_id & 0xFF;
        const uint32_t tester = (rx_id >> 8) & 0xFF;
        e.tx_id = (rx_id & 0x1FFF0000u) | (ecu << 8) | tester;
    }
    else
    {
        e.tx_id = rx_id >= 8 ? rx_id - 8 : rx_id;
        e.guessed = true;
    }
    return e;
}

bool uds_learner_add(UdsLearner& l, const UdsEcu& ecu)
{
    for (UdsEcu& e : l.ecus)
    {
        if (e.iface == ecu.iface && e.extended == ecu.extended && e.rx_id == ecu.rx_id)
        {
            bool changed = false;
            if (e.guessed && !ecu.guessed)
            {
                e.tx_id = ecu.tx_id;
                e.guessed = false;
                changed = true;
            }
            if (!ecu.name.empty() && e.name != ecu.name)
            {
                e.name = ecu.name;
                changed = true;
            }
            return changed;
        }
    }
    l.ecus.push_back(ecu);
    return true;
}

bool uds_learn(UdsLearner& l, const BusMessage& frame)
{
    if (frame.type != BusType::CAN || is_error_frame(frame) || has_flag(frame, bus_flag::rtr))
    {
        return false;
    }
    const auto payload = first_payload(frame);
    if (!payload)
    {
        return false;
    }
    const std::span<const uint8_t> p = *payload;
    const uint32_t id = can_id(frame);
    const bool response = p[0] == 0x7F || (p[0] >= 0x40 && is_request_sid(static_cast<uint8_t>(p[0] - 0x40)));
    const uint8_t sid = p[0] == 0x7F ? (p.size() >= 2 ? p[1] : 0) : response ? static_cast<uint8_t>(p[0] - 0x40) : p[0];
    if (!is_request_sid(sid))
    {
        return false;
    }

    if (has_flag(frame, bus_flag::extended))
    {
        // Normal fixed addressing, 0x18DA<TA><SA>: an answer names both ends.
        if (((id >> 16) & 0xFF) != 0xDA || !response)
        {
            return false;
        }
        const uint32_t ecu = id & 0xFF;
        const uint32_t tester = (id >> 8) & 0xFF;
        return uds_learner_add(l, {.iface = frame.iface, .extended = true,
                                   .tx_id = (id & 0x1FFF0000u) | (ecu << 8) | tester, .rx_id = id});
    }

    std::erase_if(l.pending, [&](const UdsLearner::Pending& q) { return frame.ts_ns - q.ts_ns > pairing_window_ns; });
    if (!response)
    {
        if (l.pending.size() >= max_pending)
        {
            l.pending.erase(l.pending.begin());
        }
        l.pending.push_back({.iface = frame.iface, .id = id, .sid = sid, .ts_ns = frame.ts_ns});
        return false;
    }
    // The request answered: the one on id - 8 (the OBD convention) when it is pending, else the
    // newest of this service on the same bus. It is consumed, unless the answer was "pending".
    auto match = l.pending.end();
    for (auto it = l.pending.begin(); it != l.pending.end(); ++it)
    {
        if (it->iface == frame.iface && it->sid == sid && it->id != id
            && (match == l.pending.end() || it->id + 8 == id || match->id + 8 != id))
        {
            match = it;
        }
    }
    if (match == l.pending.end())
    {
        return false;
    }
    const bool functional = match->id == 0x7DF;
    const UdsEcu e{.iface = frame.iface, .tx_id = functional ? (id >= 8 ? id - 8 : id) : match->id, .rx_id = id,
                   .guessed = functional};
    if (!(p[0] == 0x7F && p.size() >= 3 && p[2] == 0x78) && !functional)
    {
        l.pending.erase(match);
    }
    return uds_learner_add(l, e);
}
