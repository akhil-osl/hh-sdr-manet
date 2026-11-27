# Work plan: IP traffic over the radio on two boards

| Field | Value |
|---|---|
| Document number | Unassigned |
| Version | 0.1 |
| Issue date | 24-Nov-2026 |
| Status | Draft for review |
| Author | Akhil |
| Project | HH-SDR MANET radio |
| Subject | MAC process, network interface and addresses on the boards |
| Classification | Company Confidential |

## 1 Purpose

Today the two boards hold a radio link in both directions, but nothing carries IP packets over it. This plan lists the work that puts a MAC process, a network interface and a separate address on each board, so ping, routing, iperf and the application tasks can run over the radio.

Labels: IMPLEMENTED means read in the code. PROPOSED is a choice made for this plan. UNKNOWN / OPEN needs a check or an answer.

## 2 What exists

IMPLEMENTED:
- Ring library: `src/ring/`, `include/hhsdr/ring/` (ring, shm, pull, mac driver) and `tools/sim/hh_ring_bridge.c`. It is on `origin/main` (merge cf9885e). The branch `AC2O-T2589_feature/slot-scheduler-and-qos` does not contain it.
- `hh_ring_mac_t` in `mac_driver.h`: writes each PDU from the slot scheduler into the TX ring and passes RX ring frames to the scheduler. It refuses a scheduler that does not have `logic_adds_crc` set.
- `hh_ring_bridge` runs real OLSRd2 over the rings in simulation, with one network namespace and one TAP per node, all in one x86 process.
- `rh_agent` on the board (`sdr_official`, `api/pl_rings.h`, `api/radhawk.cc`) opens radiod's ring file (default `/tmp/hh_rings`). It writes the RX ring from the receiver and takes frames from the TX ring into the transmitter.
- `RH_RING_LOOPBACK` in `api/radhawk.cc:20` is 1 by default. It feeds the TX ring from the RX ring as a test. The comment says that 0 means rings only, ready for the real MAC.
- OLSRd2 for the boards: `tools/olsrd2-arm-cross-build/`. It ran on the boards on 28 Oct over wired Ethernet.
- Radio link in both directions on two boards, new bitstream, locked at the same time (24 Nov).

## 3 What is missing

- A MAC process that runs on one board. `hh-slot-bridge` is x86-64 only.
- A network interface on the board for the MAC process. The only TAP code is in the simulator (`tools/sim/hh_slot_bridge.c:138`). The board kernel has no TUN/TAP support.
- A separate IP address and node id for each board. Both boards use 192.168.10.2 on eth0, because the SD cards are identical copies.
- `rh_agent` built with `RH_RING_LOOPBACK` set to 0. Whether that build works with the deployed bitstream is UNKNOWN / OPEN.
- The radiod source that runs on the boards, built from a local merge that is not pushed.

## 4 Work packages

Each package ends with a check that can be repeated.

### WP0 Base branch

- Merge `origin/main` into the working branch, or base the new work on `origin/main`. PR 8 (the slot scheduler) is still open and has to merge with the ring library.
- Check: the ring tests and `hh-ring-bridge` build in the working tree.
- PROPOSED order: merge PR 8 first, then branch for this work.

### WP1 Board facts to read first

All read-only on a board. Not done yet, the serial port was busy.
- Does `/dev/net/tun` exist, and does the kernel support TAP? UNKNOWN / OPEN.
- Does radiod create `/tmp/hh_rings` on start, and what is in its `ring_path` setting? UNKNOWN / OPEN.
- Which `RH_RING_LOOPBACK` value is in the deployed `rh_agent`?
- How much free space and RAM the boards have for a second process.
- Check: a short list of answers, saved as a log.

Read on Board A, 24 Nov, read-only:
- `/dev/net/tun` does not exist. The kernel (4.19.0-xilinx-v2019.1) has `# CONFIG_TUN is not set`, and no `tun.ko` is installed. So a TAP interface cannot be created on the board with this kernel. `CONFIG_DUMMY`, `CONFIG_MACVLAN` and `CONFIG_VETH` are also not set. `CONFIG_PACKET=y`, `CONFIG_BRIDGE=m` and `CONFIG_MODULES=y` are set. WP4 is blocked by this.
- `/tmp/hh_rings` does not exist, as predicted from the radiod start line below.
- Memory 119 MB total, 37 MB free. Root disk 14 GB, 12 GB free.
- Board B is not checked. The cards are identical copies.

Read from the code and the deployed files, not yet confirmed on a board:
- radiod creates the ring file only when `ring_path` is set. The default is empty, meaning no rings (`radiod/README.md`, Frame rings). The boards start radiod as `./radiod -b opencpi -a .../rh_agent -s /tmp/r.sock`, with no `-c` config file. So `/tmp/hh_rings` probably does not exist, and `rh_agent` uses a private region that no other process can see. A MAC process needs radiod started with `ring_path` set.
- No build script in `sdr_official` sets `RH_RING_LOOPBACK`, so the default of 1 applies. The deployed `rh_agent` also contains `ring_loopback.c` assert strings. So it is probably the loopback build. A build with 0 is needed (WP2).
- The deployed `radiod` has the same md5 in `crc_gen_20261119` and `rx_relock_20261123` (7a3f80a2...). Whether it matches the binary on the cards is not checked.

### WP2 Rings only, no IP

Smallest proof that frames cross the radio through the ring files.
- Build `rh_agent` with `RH_RING_LOOPBACK` 0, for ARM, from `sdr_official`.
- Write a small tool for the board that writes one PDU into the TX ring, and one that reads the RX ring. The format is in `docs/RING-INTERFACE.md` sections 6 and 14.
- Check: a PDU written on board A appears in board B's RX ring with the CRC OK flag set, and the other way round.
- The TX path takes frames of one width only (one FEC mode) (`api/radhawk.cc`). The width has to match the radio's FEC mode.

### WP3 MAC process for one node

- New program for one board: the slot scheduler (`hh_slot`), the ring MAC driver, and a TAP. It is the single-node form of `hh_ring_bridge.c`.
- Settings: node id, net id, ring file path, slot plan, lead frames.
- Cross-build with the existing Docker flow for ARM. You run the Docker commands, under the project rule.
- Check: starts on a board, opens the ring file, and the counters in `hh_ring_mac_stats_t` change.
- Size is UNKNOWN until WP2 shows the rings work.

### WP4 Network interface and addresses

- Blocked: the board kernel has no TUN support (WP1). Options, to be chosen: build the kernel with `CONFIG_TUN=y`, or build `tun.ko` for 4.19.0-xilinx-v2019.1 (needs the kernel build tree), or use another interface type the kernel already has. Each needs the owner of the board image.
- A TAP on the board, named `manet0`. The product binding is undecided (see `unknown.md`, the 6 Nov note). PROPOSED for the first test: the simulator choices, labelled as a fixture. These are TAP, static neighbour entries instead of ARP, IPv4 `10.99.0.<id>/24`, link address `02-00-00-00-hh-ll`, and the MTU set to the largest packet one PDU carries.
- A per-board configuration file read at start, holding node id and address. The SD card image stays identical.
- Keep eth0 (192.168.10.2) unchanged for management.
- Check: `ping` between `10.99.0.1` and `10.99.0.2` over the radio, with a capture of the frames on the rings.

### WP5 Slot plan and time

- The MAC process needs a slot plan with no network entry logic. PROPOSED: a fixed slot map per node from configuration, so network entry is not a prerequisite. Entry stays under AC2O-T1846.
- Whether the slot scheduler accepts frames when the two nodes are not aligned to the same superframe is UNKNOWN / OPEN. It needs a read of the receive path in `src/mac/slot.c`.
- The PL does not slot-time packets. The PS sends a frame when one is waiting in the ring. The scheduler's timing assumes the PL sends at the slot (U-02). The effect on this test is UNKNOWN / OPEN.
- Each board has its own transmit and receive cable, so two nodes do not clash on the cables. That does not hold with three nodes.
- Check: the scheduler counters show PDUs received from the other node with no slot errors.

### WP6 Routing

- OLSRd2 for ARM from `tools/olsrd2-arm-cross-build/`, one instance per board on `manet0`.
- Check: routes form on both boards, `ping` by node address works, and the routing table shows the neighbour.

### WP7 Evidence ladder

Each step needs the one before it, and each gives a result worth recording.

| Step | Result | Unblocks |
|---|---|---|
| 1 WP2 | A frame crosses in both directions through the rings | The rest of this plan |
| 2 WP4 | Ping over the radio | AC2O-T2074 first run |
| 3 WP6 | OLSRd2 routes and iperf3 over the radio | AC2O-T2074 |
| 4 | Link break by TX off | AC2O-T2074 test, AC2O-T1853 |
| 5 | Data terminal over the radio | AC2O-T2080 |
| 6 | Video stream | AC2O-T1861 |
| 7 | A third node (a PC) | AC2O-T2078 |
| 8 | Slot allocation under real traffic | AC2O-T2589 measurement |

## 5 Dependencies

- Radio receiver relock (BUG-01): the new bitstream is loaded on both boards. It still needs the repeated signal-loss test.
- The radiod build on the boards is not in git. Push the merge, or rebuild from a known commit, so the board software is reproducible.
- The PL items stay open: the slot-timed path, per-frame slot, CRC result and signal quality. WP2 to WP6 do not wait for them. The AC2O-T2589 measurement does.
- The TX pull call and its ORB are not needed here, because `rh_agent` moves frames between the rings and the transmitter.

## 6 Open points

- Whether the TAP, the static neighbour entries and the addresses of WP4 are the product choice. They are a fixture.
- Whether `rh_agent` with `RH_RING_LOOPBACK` 0 works with the deployed bitstream.
- Whether the slot scheduler needs aligned superframes between nodes.
- The ring slot size and PDU capacity. The default ring geometry in `api/pl_rings.h` (private region) is 64 slots of 288 bytes, and the PDU capacity in the scheduler default is a placeholder of 128 bytes.
- Who owns each package. Not assigned.
- Effort is not estimated. It follows the WP2 and WP1 results.
