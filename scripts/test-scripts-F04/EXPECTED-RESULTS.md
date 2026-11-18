# Expected results for the two radio routing test


The simulation has no 2-node run. The closest runs are 4-node line (olsrd2-4node-line) and the 4-node ring runs. A 2-node test is one hop, so each value below is marked:

- MEASURED: taken straight from the simulation.
- DERIVED: calculated from measured values, with the working shown.
- NOT TRACED (UNKNOWN / OPEN): no simulation value exists. Do not judge pass or fail on it.

Slot capacity (256 bytes in the real OLSRd2 run) is a placeholder, so every share and rate that depends on it can change.

## Step 1: link setup (01-link-setup.sh)

| Item | Expected | Trace |
|---|---|---|
| Routing protocol | OLSRd2 | MEASURED, the protocol run in the simulation |
| Routing version | 0.15.1 in the simulation; the target version must be recorded and compared | MEASURED for 0.15.1, target version NOT TRACED |
| Interface state | up | DERIVED, link must be up before routing starts |
| Peer answers ping | at least 1 reply of 3 | DERIVED from route formation below |
| MTU in use | NOT TRACED. The simulation fixture carried OLSRd2 packets of 58 to 103 bytes or more, all within a 229-byte limit at 256-byte slot capacity. The target MTU is not defined (unknown.md U-24, U-28) | NOT TRACED |

## Step 2: route discovery (02-route-discovery.sh)

| Item | Expected | Trace |
|---|---|---|
| Route to the neighbour appears | about 8 s after OLSRd2 start (node 1 to node 2) | MEASURED, olsrd2-4node-line |
| Script timeout | 60 s default leaves margin over 8 s | DERIVED |

A neighbour route needs only HELLO exchange, so the 3-hop figures (17 s and 18 s) do not apply to 2 nodes.

## Step 3: ping and iperf3 (03-traffic.sh)

| Item | Expected | Trace |
|---|---|---|
| Ping loss | 0 percent | MEASURED, 75 of 75 on 3 hops, 0 loss |
| Ping round trip | about 17 to 18 ms median for one hop | DERIVED, see below |
| iperf3 UDP, 20 kbit/s, 100-byte packets, 10 s | 0 lost | MEASURED, 250 packets 0 lost on 3 hops |
| iperf3 UDP jitter | below 1 ms | MEASURED, 0.08 ms and 0.24 ms |
| iperf3 TCP throughput | NOT TRACED. Only 40-byte packets at 390 bytes per second and UDP at 20 kbit/s were run. Fragmentation is excluded, so packets above one PDU are not simulated | NOT TRACED |
| iperf3 UDP at higher rates | NOT TRACED, depends on the placeholder slot capacity | NOT TRACED |

Ping round trip working. The 3-hop line (6 hop traversals) had a median of 51.6 ms, which is 8.6 ms per traversal. The 2-hop ring path (4 traversals) had a median of 35.6 ms, which is 8.9 ms per traversal. One hop is 2 traversals, so about 17 to 18 ms. Each traversal includes the wait for the next own slot (frame 20 ms), so this is an estimate to check on the target, not a measured figure.

Because TCP and higher-rate UDP are not traced, the iperf3 results are recorded as unjudged. Thresholds should be added here only after a simulation run with that traffic.

## Step 4: routing logs (04-log-routing.sh)

| Item | Expected | Trace |
|---|---|---|
| Control packets per node per second | at most 0.7: HELLO every 2 s (0.5 per second) plus TC every 5 s (0.2 per second), if each message is its own packet | DERIVED from the OLSRd2 0.15.1 defaults (hello_interval 2 s, tc_interval 5 s), read from olsrd2 --schema. Not counted on the air in the simulation |
| Routing control share of own slots | 1.31 percent of own slots carried a routing message; 0.54 percent of payload capacity | MEASURED, 4-node line. A 2-node share is not traced |
| Link metric values and valid flags | NOT TRACED. The simulation run did not record link metrics | NOT TRACED |
| Route table | one route to the peer, on the radio interface, present from the step 2 time until the end | DERIVED |

A 2-node net may send fewer TC messages than the 0.2 per second bound, so treat 0.7 as a ceiling, not a target.

## Not covered by the scripts

The simulation also measured route repair after a link cut: routes deleted 20.4 s after the cut (hello_validity 20 s), ping back 27.3 s after the cut on a ring. A 2-node net has no second path, so repair is not expected there, and the scripts have no link cut step.

## What is needed to close the gaps

- A 2-node simulation run (one hop), to replace the DERIVED round trip and route time with MEASURED values.
- A simulation run that records link metrics with valid flags and counts control packets per node per second.
- A defined MTU and slot capacity for the target (unknown.md U-24, U-28).
