"""
Print the J1939 source address of received 29-bit frames, and keep a live
table of which SA sends each PGN (the same thing the TX generator learns to
replace a DBC's placeholder SA).

Usage: Start the measurement on a bus with J1939 traffic, then run.
Press Stop to end the script.
"""
import kraken

def pgn_of(can_id):
    """PGN of a 29-bit id: PDU1 (PF < 240) excludes the PS byte, it is the
    destination address; PDU2 includes it as the group extension."""
    pf = (can_id >> 16) & 0xFF
    pgn = (can_id >> 8) & 0x3FF00  # EDP, DP and PF
    if pf >= 240:
        pgn |= (can_id >> 8) & 0xFF
    return pgn

seen = {}  # PGN -> SA last seen on the bus

print("Waiting for J1939 (29-bit) frames...\n")
while True:
    for msg in kraken.receive(timeout=0.1):
        if not msg.extended or not msg.is_rx:
            continue
        sa = msg.id & 0xFF
        pgn = pgn_of(msg.id)
        pf = (msg.id >> 16) & 0xFF
        dest = f" DA 0x{(msg.id >> 8) & 0xFF:02X}" if pf < 240 else ""
        print(f"{msg.timestamp:.6f}  0x{msg.id:08X}  PGN 0x{pgn:05X}{dest}  SA 0x{sa:02X}")
        if seen.get(pgn) != sa:
            seen[pgn] = sa
            print("  PGN -> SA table:", {f"0x{p:05X}": f"0x{s:02X}" for p, s in sorted(seen.items())})
