# Design note: one-node MAC process (WP3)

| Field | Value |
|---|---|
| Document number | Unassigned |
| Version | 0.1 |
| Issue date | 24-Nov-2026 |
| Status | Draft for review. Section 5 lists the choices that are open |
| Author | Akhil |
| Project | HH-SDR MANET radio |
| Subject | A MAC process for one node: network interface, slot scheduler and frame rings |
| Classification | Company Confidential |

## 1 Purpose

`docs/BOARD-MAC-WORK-PLAN.md` needs a program that runs on one board. It takes IP packets from a network interface, sends them through the slot scheduler and the TX ring, and delivers packets from the RX ring back to the interface. This note says what can be reused, what is new, and what has to be chosen first.

Labels: IMPLEMENTED means read in the code. PROPOSED is a choice for this plan. UNKNOWN / OPEN needs an answer.

## 2 What exists

IMPLEMENTED:
- The simulator builds one node in `tests/sim/ring_net.c` lines 107 to 148. The steps are:
  1. `hh_mac_config_default`, `hh_mac_init`, `hh_mac_open`.
  2. `hh_slot_config_default`, then set `own_slots` and `logic_adds_crc = true`, then `hh_slot_init` with `hh_ring_mac_tx` as the transmit function.
  3. `hh_ring_shm_open` on the ring file, then `hh_ring_mac_init` with `lead_frames`.
- Each step of the loop is `hh_ring_mac_poll` (RX ring to the scheduler) and `hh_slot_poll` (`ring_net.c` lines 267 and 268).
- Received packets arrive through the MAC receive function (`hh_mac_receive_fn`, `ring_net.c` line 59), as an `HH_MAC_IND_FRAME`.
- `tools/sim/hh_ring_bridge.c` does all of this for N nodes in one process, with one TAP per node in a network namespace. Its loop runs the slots in real time and reads the TAP between slots.
- `tools/sim/slot_bridge_frames.c` converts between an Ethernet frame from the TAP and the MAC send call (`br_from_eth`, `br_to_eth`). It uses the link address `02-00-00-00-hh-ll`, and sends OLSRv2 packets as routing control and the rest as data.
- `hh_clock_monotonic()` (`src/core/clock.c`) is a real clock on CLOCK_MONOTONIC.

## 3 What is new

1. **The program.** One node, no network namespaces, no fake PL. It opens a ring file by path, opens one TAP, runs the slot clock from `hh_clock_monotonic`, and loops: read the TAP, `hh_ring_mac_poll`, `hh_slot_poll`, deliver received packets to the TAP.
2. **Settings.** Node id, net id, ring file path, own slots, lead frames, TAP name, MTU.
3. **A ring relay for the PC.** On a board `rh_agent` moves frames between the rings and the radio. A PC has no radio. To test two instances on a PC, a small stand-in program copies each TX ring frame into the other instance's RX ring with the CRC OK flag set. The same program serves as the WP2 test tool.
4. **Start-up on a board.** A script that sets the node id and the address from a per-board file, starts radiod with `ring_path`, and starts this program.

## 4 What stays the same

- The slot scheduler, the ring MAC driver and the PDU codec. No change.
- OLSRd2 as an unmodified external process.
- The frame conversion code, unless the TUN option below is chosen.

## 5 Choices that are open

These are decisions for the project. Each has a PROPOSED answer and its reason.

| Choice | Options | PROPOSED | Reason |
|---|---|---|---|
| Where the program lives | A: `tools/node/`, built beside the simulator tools and reusing `tools/sim/slot_bridge_frames.c`. B: a new product directory with the frame conversion moved to `src/netif/` | A for the first board test | `CLAUDE.md` says production targets never depend on tests or simulator code. The bridge reuse is allowed in tooling. Moving to B is a later step once the path works |
| Network interface type | TAP (Ethernet frames) or TUN (raw IP) | TAP | Reuses tested conversion code. The cost is fixed neighbour entries instead of ARP. TUN needs no neighbour entries, but needs new code to find the next hop from the IP address, and the MAC address convention is already in the specification |
| Settings | Command-line flags, or a config file | Flags, as the bridge does | No new file format. A start script reads the per-board file and passes the flags |
| First test | Two instances and the relay on this PC | Yes | Needs no board and no kernel change, because the PC kernel has TUN |

## 6 Unknowns

- The superframe alignment between two boards. Each process counts from its own start. Whether `hh_slot_receive` accepts a PDU when the sender's slot clock is not aligned with the receiver's is UNKNOWN / OPEN. It needs a read of the receive path in `src/mac/slot.c`.
- The slot position in the RX descriptor. In the simulator the fake PL fills it. On a board `rh_agent` fills it, and what it writes is UNKNOWN / OPEN.
- The PDU capacity. The default is a placeholder of 128 bytes, and the PL frame is 204 bytes at FEC rate 3/4. They have to be set to match.
- The TAP MTU. It has to be at most the largest packet one PDU carries, which the bridge prints at start.
- On the board the kernel has no TUN, so the program cannot run there until that changes.

## 7 Test ladder on this PC

1. Build the program and the relay. The existing tests still pass.
2. Start two instances and the relay, each in its own network namespace, so they stay separate.
3. `ping` between the two TAP addresses, with a capture of the frames on the rings.
4. Real OLSRd2 on both, and `iperf3`, as in `evidence/f04-ak1`.
5. Cross-build for ARM. You run the Docker step.
