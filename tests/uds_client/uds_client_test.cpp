// UDS client (decoders/uds_client): ISO-TP send and receive against a fake ECU that answers
// from inside the send callback, functional scans, address learning and response text.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <functional>
#include <initializer_list>
#include <vector>

#include "decoders/uds_client.h"

namespace
{

using Bytes = std::vector<uint8_t>;

BusMessage frame(uint32_t id, std::initializer_list<uint8_t> bytes, bool ext = false, uint16_t iface = 0, int64_t ts = 0)
{
    BusMessage m{};
    m.id = id;
    m.iface = iface;
    m.flags = ext ? bus_flag::extended : 0;
    m.ts_ns = ts;
    std::size_t i = 0;
    for (const uint8_t b : bytes)
    {
        m.data[i++] = b;
    }
    set_length(m, 8);
    return m;
}

// The ECU side: sees every frame the tester sends, answers through uds_client_rx.
struct Ecu
{
    UdsClient client;
    std::vector<BusMessage> sent; // by the tester
    std::function<void(Ecu&, const BusMessage&)> on_frame;

    UdsSend send()
    {
        return [this](const BusMessage& m)
        {
            sent.push_back(m);
            if (on_frame)
            {
                on_frame(*this, m);
            }
            return true;
        };
    }
    void reply(uint32_t id, std::initializer_list<uint8_t> bytes, bool ext = false)
    {
        uds_client_rx(&client, frame(id, bytes, ext));
    }
};

const UdsTarget engine{.tx_id = 0x7E0, .rx_id = 0x7E8};
const UdsTiming quick{.p2 = std::chrono::milliseconds(50), .p2_star = std::chrono::milliseconds(200),
                      .n_bs = std::chrono::milliseconds(50), .n_cr = std::chrono::milliseconds(50)};

} // namespace

TEST_CASE("single frame request and response, padded to 8 bytes")
{
    Ecu ecu;
    ecu.on_frame = [](Ecu& e, const BusMessage&) { e.reply(0x7E8, {0x06, 0x50, 0x03, 0x00, 0x32, 0x01, 0xF4}); };
    const Bytes req = {0x10, 0x03};
    const UdsResult r = uds_request(ecu.client, ecu.send(), engine, req, quick);
    CHECK(r.error.empty());
    CHECK(r.response == Bytes{0x50, 0x03, 0x00, 0x32, 0x01, 0xF4});
    REQUIRE(ecu.sent.size() == 1);
    CHECK(ecu.sent[0].id == 0x7E0);
    CHECK(ecu.sent[0].len == 8);
    CHECK(ecu.sent[0].data[0] == 0x02);
    CHECK(ecu.sent[0].data[3] == 0xCC);
    CHECK(uds_describe(req, r.response) == "session 0x03 active");
}

TEST_CASE("multi-frame response: the tester sends a flow control, the rest is reassembled")
{
    Ecu ecu;
    ecu.on_frame = [](Ecu& e, const BusMessage& m)
    {
        if (m.data[1] == 0x22)
        {
            e.reply(0x7E8, {0x10, 0x14, 0x62, 0xF1, 0x90, 'W', 'K', 'R'}); // 20 bytes
        }
        else if ((m.data[0] >> 4) == 3)
        {
            e.reply(0x7E8, {0x21, 'A', 'K', 'E', 'N', '0', '1', '2'});
            e.reply(0x7E8, {0x22, '3', '4', '5', '6', '7', '8', '9'});
        }
    };
    const Bytes req = {0x22, 0xF1, 0x90};
    const UdsResult r = uds_request(ecu.client, ecu.send(), engine, req, quick);
    REQUIRE(r.error.empty());
    CHECK(r.response.size() == 20);
    CHECK(uds_describe(req, r.response).ends_with("\"WKRAKEN0123456789\""));
    REQUIRE(ecu.sent.size() == 2);
    CHECK(ecu.sent[1].data[0] == 0x30); // clear to send, no block limit
}

TEST_CASE("multi-frame request honours block size and waits for the next flow control")
{
    Ecu ecu;
    int cfs = 0;
    ecu.on_frame = [&cfs](Ecu& e, const BusMessage& m)
    {
        const uint8_t pci = m.data[0] >> 4;
        if (pci == 1)
        {
            e.reply(0x7E8, {0x31}); // WAIT first: the tester keeps waiting
            e.reply(0x7E8, {0x30, 0x02, 0x01}); // then go, two frames per block, 1 ms apart
        }
        else if (pci == 2 && ++cfs % 2 == 0 && cfs < 3)
        {
            e.reply(0x7E8, {0x30, 0x02, 0x01});
        }
        if (pci == 2 && cfs == 3)
        {
            e.reply(0x7E8, {0x03, 0x6E, 0xF1, 0x90});
        }
    };
    Bytes req = {0x2E, 0xF1, 0x90};
    for (int i = 0; i < 21; ++i)
    {
        req.push_back(static_cast<uint8_t>('A' + i));
    }
    const UdsResult r = uds_request(ecu.client, ecu.send(), engine, req, quick);
    CHECK(r.error.empty());
    CHECK(r.response == Bytes{0x6E, 0xF1, 0x90});
    REQUIRE(ecu.sent.size() == 4); // FF + 3 CF (6 + 7 + 7 + 4 = 24)
    CHECK(ecu.sent[0].data[0] == 0x10);
    CHECK(ecu.sent[0].data[1] == 24);
    CHECK(ecu.sent[1].data[0] == 0x21);
    CHECK(ecu.sent[3].data[0] == 0x23);
    CHECK(ecu.sent[3].data[4] == 'U');
    CHECK(ecu.sent[3].data[5] == 0xCC); // padding after the last byte
}

TEST_CASE("flow control overflow and a missing flow control end the request")
{
    Ecu ecu;
    const Bytes req(12, 0x2E);
    ecu.on_frame = [](Ecu& e, const BusMessage&) { e.reply(0x7E8, {0x32}); };
    CHECK(uds_request(ecu.client, ecu.send(), engine, req, quick).error.find("overflow") != std::string::npos);
    ecu.on_frame = nullptr;
    CHECK(uds_request(ecu.client, ecu.send(), engine, req, quick).error.find("N_Bs") != std::string::npos);
}

TEST_CASE("response pending extends the wait, a negative response is a response")
{
    Ecu ecu;
    ecu.on_frame = [](Ecu& e, const BusMessage&)
    {
        e.reply(0x7E8, {0x03, 0x7F, 0x31, 0x78});
        e.reply(0x7E8, {0x05, 0x71, 0x01, 0xFF, 0x00, 0x00});
    };
    const Bytes routine = {0x31, 0x01, 0xFF, 0x00};
    UdsResult r = uds_request(ecu.client, ecu.send(), engine, routine, quick);
    CHECK(r.response == Bytes{0x71, 0x01, 0xFF, 0x00, 0x00});
    CHECK(uds_describe(routine, r.response) == "routine FF00 sub 0x01: 00");

    ecu.on_frame = [](Ecu& e, const BusMessage&) { e.reply(0x7E8, {0x03, 0x7F, 0x22, 0x31}); };
    const Bytes read = {0x22, 0x12, 0x34};
    r = uds_request(ecu.client, ecu.send(), engine, read, quick);
    CHECK(r.error.empty());
    CHECK(r.response == Bytes{0x7F, 0x22, 0x31});
    CHECK(uds_describe(read, r.response) == "NRC 0x31 Request Out Of Range");
}

TEST_CASE("no answer, an answer to another service or on another id: timeout")
{
    Ecu ecu;
    const Bytes req = {0x22, 0xF1, 0x90};
    CHECK(uds_request(ecu.client, ecu.send(), engine, req, quick).error == "no response within 50 ms");
    ecu.on_frame = [](Ecu& e, const BusMessage&)
    {
        e.reply(0x7E8, {0x02, 0x7E, 0x00}); // someone else's tester present
        e.reply(0x7E9, {0x03, 0x62, 0xF1, 0x90});
    };
    CHECK(uds_request(ecu.client, ecu.send(), engine, req, quick).error.starts_with("no response"));
    CHECK(ecu.client.active.load() == 0); // the waiter is gone
}

TEST_CASE("suppressPosRspMsgIndicationBit: sent, nothing awaited")
{
    Ecu ecu;
    const Bytes req = {0x3E, 0x80};
    const UdsResult r = uds_request(ecu.client, ecu.send(), engine, req, quick);
    CHECK(r.suppressed);
    CHECK(r.error.empty());
    CHECK(ecu.sent.size() == 1);
}

TEST_CASE("functional request collects every answering ECU once")
{
    Ecu ecu;
    ecu.on_frame = [](Ecu& e, const BusMessage&)
    {
        e.reply(0x7E8, {0x02, 0x7E, 0x00});
        e.reply(0x7E9, {0x02, 0x7E, 0x00});
        e.reply(0x7E8, {0x02, 0x7E, 0x00}); // twice: listed once
        e.reply(0x123, {0x08, 0x01, 0x02});  // other traffic
    };
    const Bytes tp = {0x3E, 0x00};
    const UdsTarget fn{.tx_id = 0x7DF};
    const auto found = uds_functional(ecu.client, ecu.send(), fn, tp, std::chrono::milliseconds(30));
    REQUIRE(found.size() == 2);
    CHECK(found[0].rx_id == 0x7E8);
    CHECK(found[1].rx_id == 0x7E9);
    const UdsEcu e = uds_ecu_from_responder(fn, 0x7E9);
    CHECK(e.tx_id == 0x7E1);
    CHECK(e.guessed);

    // 29-bit: only answers to this tester (0x18DAF1xx) count, and their address is exact.
    ecu.on_frame = [](Ecu& e, const BusMessage&)
    {
        e.reply(0x18DAF110, {0x02, 0x7E, 0x00}, true);
        e.reply(0x18DAF210, {0x02, 0x7E, 0x00}, true);
        e.reply(0x7E8, {0x02, 0x7E, 0x00});
    };
    const UdsTarget fn29{.tx_id = 0x18DB33F1, .extended = true};
    const auto found29 = uds_functional(ecu.client, ecu.send(), fn29, tp, std::chrono::milliseconds(30));
    REQUIRE(found29.size() == 1);
    const UdsEcu body = uds_ecu_from_responder(fn29, found29[0].rx_id);
    CHECK(body.tx_id == 0x18DA10F1);
    CHECK(body.rx_id == 0x18DAF110);
    CHECK(!body.guessed);
}

TEST_CASE("a functional request refuses a multi-frame answer, cancel ends a wait")
{
    Ecu ecu;
    ecu.on_frame = [](Ecu& e, const BusMessage&) { e.reply(0x7E8, {0x10, 0x14, 0x62, 0xF1, 0x90, 'W', 'K', 'R'}); };
    const Bytes req = {0x22, 0xF1, 0x90};
    const UdsTarget fn{.tx_id = 0x7DF, .rx_id = 0x7E8};
    CHECK(uds_request(ecu.client, ecu.send(), fn, req, quick).error.find("physical") != std::string::npos);
    CHECK(ecu.sent.size() == 1); // no flow control went out

    ecu.on_frame = [](Ecu& e, const BusMessage&) { uds_client_cancel(e.client); };
    const UdsTiming slow{.p2 = std::chrono::seconds(10)};
    CHECK(uds_request(ecu.client, ecu.send(), engine, req, slow).error == "cancelled");
}

TEST_CASE("DTC list text")
{
    const Bytes req = {0x19, 0x02, 0xFF};
    const Bytes rsp = {0x59, 0x02, 0xFF, 0x01, 0x23, 0x00, 0x2F, 0xC1, 0x55, 0x10, 0x09};
    CHECK(uds_describe(req, rsp) == "2 DTCs\n  P0123-00  status 0x2F\n  U0155-10  status 0x09");
}

TEST_CASE("learning: 11-bit pairs, functional guesses refined, 29-bit from the answer")
{
    UdsLearner l;
    // Request on 0x7E0, answer on 0x7E8 within the window: an exact pair.
    CHECK(!uds_learn(l, frame(0x7E0, {0x02, 0x10, 0x03}, false, 0, 1'000)));
    CHECK(uds_learn(l, frame(0x7E8, {0x06, 0x50, 0x03, 0x00, 0x32, 0x01, 0xF4}, false, 0, 2'000)));
    REQUIRE(l.ecus.size() == 1);
    CHECK(l.ecus[0].tx_id == 0x7E0);
    CHECK(l.ecus[0].rx_id == 0x7E8);
    CHECK(!l.ecus[0].guessed);
    // The same answer again teaches nothing new.
    CHECK(!uds_learn(l, frame(0x7E8, {0x02, 0x7E, 0x00}, false, 0, 3'000)));

    // Functional request: the answer's request id is guessed, later replaced by the real one.
    uds_learn(l, frame(0x7DF, {0x02, 0x3E, 0x00}, false, 0, 10'000));
    CHECK(uds_learn(l, frame(0x7EA, {0x02, 0x7E, 0x00}, false, 0, 11'000)));
    CHECK(l.ecus[1].tx_id == 0x7E2);
    CHECK(l.ecus[1].guessed);
    uds_learn(l, frame(0x7E2, {0x03, 0x22, 0xF1, 0x90}, false, 0, 20'000));
    CHECK(uds_learn(l, frame(0x7EA, {0x10, 0x14, 0x62, 0xF1, 0x90, 1, 2, 3}, false, 0, 21'000)));
    CHECK(l.ecus[1].tx_id == 0x7E2);
    CHECK(!l.ecus[1].guessed);

    // Two requests outstanding: the answer on request + 8 belongs to that one, which is consumed.
    uds_learn(l, frame(0x7E0, {0x02, 0x10, 0x03}, false, 0, 25'000));
    uds_learn(l, frame(0x7E3, {0x02, 0x10, 0x03}, false, 0, 25'500));
    CHECK(!uds_learn(l, frame(0x7E8, {0x02, 0x50, 0x03}, false, 0, 26'000))); // known already
    CHECK(uds_learn(l, frame(0x7EB, {0x02, 0x50, 0x03}, false, 0, 26'500)));
    CHECK(l.ecus.back().tx_id == 0x7E3);
    CHECK(!uds_learn(l, frame(0x7EC, {0x02, 0x50, 0x03}, false, 0, 27'000))); // nothing left to answer

    // An answer long after its request (or with no request) pairs with nothing.
    uds_learn(l, frame(0x700, {0x02, 0x10, 0x01}, false, 0, 30'000));
    CHECK(!uds_learn(l, frame(0x708, {0x02, 0x50, 0x01}, false, 0, 30'000 + 3'000'000'000LL)));

    // Another interface is another ECU, even on the same ids.
    uds_learn(l, frame(0x7E0, {0x02, 0x10, 0x03}, false, 1, 40'000));
    CHECK(uds_learn(l, frame(0x7E8, {0x02, 0x50, 0x03}, false, 1, 41'000)));
    CHECK(l.ecus.size() == 4);

    // 29-bit normal fixed addressing: the answer names both ends. Requests alone do not count.
    CHECK(!uds_learn(l, frame(0x18DA10F1, {0x02, 0x3E, 0x00}, true)));
    CHECK(uds_learn(l, frame(0x18DAF110, {0x02, 0x7E, 0x00}, true)));
    CHECK(l.ecus.back().tx_id == 0x18DA10F1);
    CHECK(l.ecus.back().extended);

    // Not UDS: plain data, J1939, error frames.
    CHECK(!uds_learn(l, frame(0x123, {0x08, 0x01, 0x02, 0x03})));
    CHECK(!uds_learn(l, frame(0x18FEF100, {0x02, 0x7E, 0x00}, true)));
    BusMessage err = frame(0x7E8, {0x02, 0x7E, 0x00});
    err.errors = bus_error::bit;
    CHECK(!uds_learn(l, err));
}
