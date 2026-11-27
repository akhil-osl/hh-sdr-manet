# Work plan: what runs on one board, what waits for two

Only one board is available. This plan covers every task in `TASK-STATUS.md`. "Waits for two" means the claim needs a second node and cannot be proven on one board. Run commands are in `docs/BOARD-RUNBOOK.md`.

## 1. Decisions that unblock several tasks

| Decision | Options | Tasks it affects |
|---|---|---|
| More-data bit (BUG-02) | A: the MAC never sets it (`src/mac/slot.c:333`, no bitstream change). B: the PL owner changes `crc32_frame_gen.v` and `crc32_frame_chk.v`, new bitstream. | T2074, T2589, T1861, T2080 (any run with several packets queued) |
| Link MTU | The tasks ask 1280 or more. The boards allow 172. MAC fragmentation does not exist. | T2074, T1861, T2080 |
| Status interface (U-03) | How the host reads link status and neighbours from a board. | T1853 |

## 2. Task by task

| Task | On one board now | On the PC now | Waits for two boards |
|---|---|---|---|
| T2074 routing over two radios | Nothing for the task claims. The BUG-02 fix, once chosen, can be re-checked with the heavy `mac_peer` loopback run. | Prepare the ARM `olsrd2` and `iperf3` files, add `[interface=manet0]` to `olsrd2-board.conf`, file the 26 Nov board outputs in evidence. | Ping, `tcpdump`, iperf, OLSRd2, link break, restart recovery, attenuation. |
| T1846 network entry | Nothing. | Cold start and founder logic, arrival offset check, re-entry tests in the simulator. Placeholder values stay placeholders. | Entry on the radio link, repeated runs, test log. |
| T2589 slot scheduler and QoS | Nothing. | Smoothing of demand, quick removal of a leaving member, connecting the DSCP table to the real data path. These are code and simulator work. | Measurement with iperf3 or RTP over the RF link. Allocation values (U-06) stay placeholders until then. |
| T1861 video trial | One-way payload rate on the loopback, after the BUG-02 decision. | Any stage of `docs/VIDEO-TRIAL-PLAN.md` that is host-only (encoder, decoder, VMAF). NOT CHECKED which stages those are. | Per-node share of a slotted net, live stream, RTCP return direction. |
| T1853 link status and neighbours | Link status read from the one board (`rx_fts link_status`, `rx_ber` counters). A cable pull or TX off on the loopback changes its own receiver. | Nothing until the status interface (U-03) is defined. | Neighbour table, link state by attenuation with the display update, screenshots. |
| T2078 three-node routing | Nothing. | Nothing. | Two radios and a PC as third node, traceroute, link break, repair time, set board clocks first. |
| T2080 data terminal | Nothing for the radio claims. A run to the PC over Ethernet gives only a baseline and is not a radio result. | Group and broadcast messages are not built. Messages of several sizes can be tested on the host. | Both ends of the radio link, sizes, delivery and latency, voice calls, capture over the radio. Each board needs its own address first. |

## 3. Order of work

1. Make the two decisions in section 1 for BUG-02 and the MTU.
2. On the PC, do the work in the "On the PC now" column.
3. On the one board, re-run the heavy loopback test after the BUG-02 change.
4. When the second board is on the cable, run T2074 first. The other radio tasks (T1846, T2078, T2080, T1853, and the measurements for T2589 and T1861) need its ping and routing to work.

## 4. Run order with two boards

Use `docs/BOARD-RUNBOOK.md` sections 3 to 5 on both boards, then section 7.

| Step | Test | Pass condition |
|---|---|---|
| 1 | Stop `mac_daemon`, bring up the radio on both boards | `link_status` 7, `crc_ok_count` rises on both |
| 2 | `hh-node` on both, `manet0` set up, ping 10 times | 10 of 10 replies |
| 3 | `tcpdump -i manet0` on both, full files kept | Each request seen with its reply, files copied to evidence |
| 4 | `iperf3` UDP, packets of 172 bytes or less | Datagrams received, loss and rate recorded |
| 5 | OLSRd2 on both boards | A route to the other board, ping through the route works |
| 6 | Link break: TX off on one board for 6 to 13 s with a ping running | Pings resume, `crc_ok_count` rises again |
| 7 | Restart `hh-node`, then `radiod`, on one board (BUG-01) | Link and ping come back without a reboot, or the failure is recorded |
| 8 | Attenuation, if an attenuator exists | Loss against attenuation recorded |
| 9 | Dataterm, entry, network status, routing with a third node | Per the pass lists under each task in `TASK-STATUS.md` |

Both boards have the same Ethernet address, so only one is on the cable at a time. Set each board's clock before any run that compares logs.

## 5. After each run

- Update the task's brief in `TASK-STATUS.md`.
- Put raw console outputs and captures in `evidence/<task>/<date>/` with a README that lists what each file shows.
- Record new gaps in `unknown.md` and mirror them in the vault.
