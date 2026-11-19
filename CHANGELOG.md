# Changelog

All notable changes to this project are listed here. The format follows Keep a Changelog. The project has no release numbers yet, so changes are grouped under Unreleased.

## Unreleased

### Frame rings between radiod, the MAC process and the PL

Adds the frame ring interface that carries MAC frames between the PL, radiod and the MAC process. The ring layout is in `docs/RING-INTERFACE.md`. Everything here runs on a host with a simulated PL and a simulated air. Nothing runs on a board yet.

#### Added

* Ring library `hhsdr_ring` (`include/hhsdr/ring/ring.h`, `src/ring/ring.c`). One shared region holds a 64 byte header, four indices on separate 32 byte lines, and an RX ring and a TX ring. Each ring is a table of 32 byte descriptors and the frame slots they describe, where descriptor i belongs to slot i. Rings are single producer and single consumer, with free-running 32-bit indices, release and acquire ordering, and a ready flag in each descriptor. The RX descriptor carries the length, slot, superframe, arrival offset, RSSI, SNR, gain, metric valid flags and a receive count. The TX descriptor carries the length, traffic class and the target superframe, frame and slot. Layout is little-endian and checked against golden descriptor bytes.
* TX pull logic `hhsdr_ring_pull` (`pull.h`, `pull.c`). Answers one pull for one TDMA frame from the TX ring. A frame whose target matches is returned and marked taken. A frame for an earlier target is dropped and counted as stale. A frame for a later target stays in the ring. Superframe order survives a wrap of the 32-bit count. Limits on items and buffer space are respected, and a bad descriptor is dropped and counted. There is no ORB in this code.
* MAC-side ring driver `hhsdr_ring_mac` (`mac_driver.h`, `mac_driver.c`). Its transmit function plugs into the slot scheduler and writes each PDU into the TX ring, targeted `lead_frames` TDMA frames after the slot it was built in, with the most urgent message class of the PDU as the ring class. Its receive function drains the RX ring into the scheduler, passing only frames the PL marked as CRC good. No change to the MAC was needed.
* Shared-memory file layer `hhsdr_ring_shm` (`shm.h`, `shm.c`). The owner creates a file, replacing a stale one, and lays out the rings in it. Any other process opens the same file by path and uses the same memory through the ring interface. The owner removes the file on close.
* radiod owns the rings. New configuration keys `ring_path`, `ring_rx_slots`, `ring_tx_slots` and `ring_slot_size`. With a path set, radiod creates the ring file at start, after it begins listening, and removes it on exit. With no path it creates nothing, so existing behaviour does not change. New functions `hh_radiod_open_rings` and `hh_radiod_rings`. Frames do not pass through radiod.
* Test infrastructure, never linked into production targets: a fake PL (`tests/sim/fake_pl`) that writes received frames and descriptors into the RX ring and pulls the TX frames once per TDMA frame, a ring-based multi-node network (`tests/sim/ring_net`), and `hh-ring-bridge` (`tools/sim/hh_ring_bridge.c`), the Linux TAP bridge that runs unmodified OLSRd2 over the rings. The ring network can keep each node's rings on the heap or in a ring file opened by path, with the MAC driver and the fake PL each mapping the file separately.
* `tools/olsrd2-mesh-emulation/slot-mesh.sh`: `BRIDGE_LOG=0` leaves out `--log` for the ring bridge, and `RING_RADIOD=1` starts one radiod per node to create the ring files.
* 46 new tests: `test_ring_region` (11), `test_ring_pull` (14), `test_ring_mac` (7), `test_ring_shm` (4), `test_ring_net` (7) and `test_radiod_rings` (3). The suite is 57 tests and passes on the standard and sanitizer builds with no warnings.
* Documents: `docs/RING-INTERFACE.md` (the ring interface contract, with worked frame examples), `docs/RING-REFERENCE-FRAMES.md` (four full-size reference frames with their descriptors, for checking an implementation) and `docs/RING-INTERFACE-COMPARISON.md` (the contract compared with the PS ring path in the radio and OpenCPI project).
* README: steps to generate `compile_commands.json` for clang tools.

#### Changed

* The radiod library now links `hhsdr_ring_shm`. radiod still does not link the MANET stack.
* radiod README: a section on the frame rings, and the new test in its test table.

#### Checked in simulation

Real, unmodified OLSRd2 over the rings, a simulated air and the fake PL, in network namespaces:

* 4-node line, ping node 1 to node 4: routes formed, 149 of 150 replies, average round trip 171.5 ms. iperf3 UDP at 20 kbit/s: 500 of 500 packets, 0 percent loss.
* 4-node ring, ping node 1 to node 3, link 1-2 cut for 15 s: the route moved to the other side about 11.4 s after the cut, and 56 pings were lost in a row. The repair time is set by the OLSRd2 timers.
* The same line run through ring files created by one real radiod per node and opened by path: 148 of 150 replies, average 171.4 ms, iperf3 0 percent loss.
* The one-frame lead costs one TDMA frame (20 ms) per hop and nothing else. The line round trip is about 120 ms above the run without rings, which matches 6 hop traversals at 20 ms.

#### Not done

* The PL side. There is no slot timing, no frame padding and length, and no per-frame slot, CRC result or signal quality from a real PL.
* The servant that answers the pull call, and the ORB dependency it needs.
* Handing the region to a process by file descriptor. Access is now by file path, limited by the file permissions only.
* A way for a process to detect that radiod restarted and replaced the ring file.
* The magic value in the region header is a placeholder.
* The real value of `lead_frames`, the ring sizes and the slot size are placeholders until the PL fixes them.
* The MAC process on a board, and the link quality feed into the routing cost.
* Nothing in this change has been run on a board or over a radio.

#### Known differences from the radio project

The PS ring path in the radio and OpenCPI project feeds frames to the PL continuously and finds received frames by hunting for the frame control and CRC. It uses a different ring type inside its own process. See `docs/RING-INTERFACE-COMPARISON.md` for the comparison.
