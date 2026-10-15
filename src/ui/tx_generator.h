#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <span>
#include <thread>
#include <unordered_map>
#include <vector>

#include "core/bus_message.h"
#include "ui/vim_nav.h"

struct App;
struct CanDbMessage;
struct CanDbSignal;
struct Iface;
struct WorkspaceTab;
namespace pugi
{
class xml_node;
}

// What sends a row: its interval, the Send button only, or a received frame (+ delay).
enum class TxTrigger : uint8_t
{
    Cyclic,
    Manual,
    OnReceive,
};

// One row of "Active Cyclic Transmissions". msg.iface is the interface (App::ifaces index).
// Signal values live in msg.data (encoded on edit), so they persist with the bytes.
struct TxCyclic
{
    BusMessage msg;
    std::string name;
    int interval_ms = 100;
    bool enabled = false;  // cyclic sending on; the sender thread clears it when a send fails
    bool selected = false; // UI only
    std::chrono::steady_clock::time_point next_due{}; // default = due at once (fresh schedule)
    const CanDbMessage* db = nullptr;                 // for the bit layout and signal table
    TxTrigger trigger = TxTrigger::Cyclic;
    uint32_t rx_id = 0;                // OnReceive: this id (29 bits) ...
    bool rx_extended = false;
    uint16_t rx_iface = UINT16_MAX;    // ... arriving on this interface
    int delay_ms = 0;                  // ... sends after this delay
    bool pending = false;              // OnReceive fired, sent at next_due by the sender thread
    bool expanded = false;             // UI only: trigger + signal editor shown
};

// State of one Generator View (+ its Message View). Not movable (mutex, thread).
struct TxGenerator
{
    // Shared with the sender thread: everything below `mutex` up to the UI block.
    std::mutex mutex;
    std::condition_variable_any wake;
    bool changed = false;       // rows edited; wakes the sender to recompute its deadline
    std::vector<TxCyclic> rows;
    std::deque<Iface>* ifaces = nullptr;
    // OnReceive rows by tx_rx_key (main thread), rebuilt when rx_dirty is set.
    std::unordered_multimap<uint64_t, std::size_t> rx_index;
    bool rx_dirty = true;

    // UI (main thread only)
    uint16_t iface = UINT16_MAX; // interface for new rows and the DBC list
    // J1939: source address last seen on the bus per PGN, learned from RX frames in
    // tx_generator_on_rx. New rows from a J1939 DBC take the real node's SA instead of
    // the DBC's placeholder (tx_apply_j1939_sa).
    std::unordered_map<uint32_t, uint8_t> j1939_sa;
    char search[64] = {};
    std::vector<const CanDbMessage*> avail_selected;
    const CanDbMessage* layout_msg = nullptr; // shown in Layout View
    int zoom = 50;
    uint32_t manual_id = 1;
    int manual_dlc = 8;
    int interval_ms = 100;
    int current = -1;       // row shown in Message View, -1 = none
    int edit_row = -1;      // row being edited in the popup
    float avail_footer = 0.0f;  // height of the wrapped control rows, measured last frame
    float active_footer = 0.0f;
    BusMessage edit_msg;
    VimNav vim_avail;          // j/k/gg/G in the DBC list, Enter adds
    VimNav vim_active;         // ... in the transmit rows, Enter expands, Space runs/stops
    bool focus_search = false; // '/' in either list: focus the message search next frame

    std::jthread sender; // last: joined before the members it uses are destroyed
};

// Deadline-driven sender thread body: sends every enabled row when due, advancing from the
// deadline (no drift) and skipping missed slots instead of bursting. Started by the UI.
void tx_sender_loop(std::stop_token stop, TxGenerator& gen);

// Starts the sender thread if it is not running yet.
void tx_generator_start(TxGenerator& gen, std::deque<Iface>& ifaces);

// Disables every row. Call before the interfaces are closed.
void tx_generator_stop_all(TxGenerator& gen);

// Physical value -> frame bytes: clamped to [min, max] when the DBC gives a range.
void tx_signal_set(const CanDbSignal& sig, BusMessage& msg, double physical) noexcept;

// 29-bit CAN id whose PGN was seen on the bus: replaces its source address (low byte)
// with the last one observed for that PGN (TxGenerator::j1939_sa). No-op otherwise.
void tx_apply_j1939_sa(const TxGenerator& gen, BusMessage& msg) noexcept;

// Lookup key of an OnReceive trigger / received frame: interface, extended flag, id.
[[nodiscard]] constexpr uint64_t tx_rx_key(uint16_t iface, uint32_t id, bool extended) noexcept
{
    return (static_cast<uint64_t>(iface) << 32) | (extended ? 0x80000000u : 0u) | id;
}

// Main thread, with each RX batch: arms the enabled OnReceive rows whose id/interface
// matches (TX echoes and error frames ignored), due `delay_ms` after the frame's reception:
// `now + delay_ms - (wall_now_ns - ts_ns)` (a timestamp older than 1 s counts as received now).
// Wakes the sender.
void tx_generator_on_rx(TxGenerator& gen, std::span<const BusMessage> msgs,
                        std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now(),
                        int64_t wall_now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                            std::chrono::system_clock::now().time_since_epoch()).count());

// Draws the tab's "Generator View" and "Message View" windows.
void draw_tx_generator(App& app, const WorkspaceTab& tab, TxGenerator& gen);

// Workspace persistence, same <frame> attributes as the Qt TxGeneratorWindow (plus
// "driver", and trigger / rx_id / rx_extended / rx_interface / rx_driver / delay, which
// older files lack and then load as Cyclic). Rows load stopped; the interface is resolved by name against ifaces.
void tx_generator_save_xml(const TxGenerator& gen, const std::deque<Iface>& ifaces, pugi::xml_node el);
void tx_generator_load_xml(TxGenerator& gen, const std::deque<Iface>& ifaces, pugi::xml_node el);
