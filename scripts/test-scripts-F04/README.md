# Two radio routing test scripts

POSIX sh scripts for the two radio routing test. Each runs on one node and
writes its evidence to `TEST_OUT`.

Status: written on the host. NOT YET TRIED on the target image. Expected
results per step are not written yet.

## Settings

| Variable | Meaning |
|---|---|
| TEST_IFACE | radio-facing interface on this node (required) |
| TEST_PEER | IP of the other node (required) |
| TEST_OUT | evidence directory, default `./test-evidence` |
| TEST_OLSRD2 | olsrd2 binary, default `olsrd2` |
| TEST_TELNET | olsrd2 telnet port, default 2009 |

## Steps

| Script | Step | Records |
|---|---|---|
| 01-link-setup.sh | link setup | routing protocol and version, interface state, MTU, ping reachability |
| 02-route-discovery.sh | route discovery | seconds until a route to the peer appears |
| 03-traffic.sh | ping and iperf3 | ping loss and delay, iperf3 TCP and UDP JSON (via tools/atp/atp-iperf3.sh) |
| 04-log-routing.sh | route table log | route table and link metrics every interval, control packets per second |
| run-all.sh | all of the above | stops at the first failed step |

Run order on each node: start the routing daemon, start `iperf3 -s` on the
peer, then run `run-all.sh`.

## Open points to confirm on the target

- Routing protocol: OLSRd2. Step 1 records the
  version that `olsrd2 --version` prints on the target; the signed-off
  version number is not recorded in the repository.
- Link metric query: step 4 sends `/nhdpinfo link` to the olsrd2 telnet port.
  The command name and whether it shows the valid flag depend on the olsrd2
  build and telnet plugin on the target. If it prints nothing, the log says
  "telnet unavailable".
- Control packet count: needs `tcpdump` and assumes OLSRv2 uses UDP port 269.
  Without tcpdump the count is skipped and logged as skipped.
- iperf3 results are "unjudged" until thresholds come from the expected
  results.
