# `tools/atp/` — ATP and BIT evidence tooling

Scripts that measure the radio through its public interfaces and write the
results as JSON evidence, for the Acceptance Test Procedure (ATP) and for
Built-In Test (BIT) runs on a deployed radio.

This is the "Test Automation" block on the target architecture drawing
(OSL-628-BD-101): iperf3, MGEN, tcpdump and `radioctl`, driven by scripts.

**Status:** development and test tooling. Nothing here is linked into, or
installed by, a production target.

---

## The rule these scripts follow

They test **only through interfaces that will still exist on hardware**:

| Script | Talks to | Today | On the radio |
|---|---|---|---|
| `atp-iperf3.sh` | an IP address | network namespace | the mesh (`manet0`, U-06) |
| `atp-mgen.sh` | an IP address | network namespace | the mesh (`manet0`, U-06) |
| `atp-capture.sh` | a network interface | `veth0` | `manet0` |

Nothing reads simulator internals. Moving to hardware changes a peer address
or an interface name, never the scripts.

---

## Evidence records

Every check writes one JSON object (schema `hh-atp-evidence/1`) to
`<evidence-dir>/<check>.json`, and keeps the raw tool output under
`<evidence-dir>/raw/`. The record is also printed to stdout.

```json
{
  "schema": "hh-atp-evidence/1",
  "check": "mgen.udp",
  "result": "fail",
  "reason": "threshold violated: loss_pct=29.5 (max 5); latency_max_ms=100.194 (max 50)",
  "started_utc": "2026-09-25T06:31:10Z",
  "finished_utc": "2026-09-25T06:31:10Z",
  "time_source": "system",
  "host": "node-a",
  "params": { "flow": 1, "clock_synced": true, "...": "..." },
  "metrics": { "sent": 200, "received": 141, "loss_pct": 29.5,
               "latency_ms": { "min": 100.049, "avg": 100.109, "max": 100.194 } },
  "thresholds": { "max_loss_pct": 5, "max_latency_ms": 50 },
  "artifacts": [ "raw/mgen.udp.tx.txt", "raw/mgen.udp.rx.txt" ]
}
```

### Results, and why there are five

| Result | Exit | Meaning |
|---|---|---|
| `pass` | 0 | measured, and inside every threshold given |
| `fail` | 1 | measured, and outside at least one threshold |
| `error` | 3 | the check itself broke: tool missing, peer unreachable, unparseable output |
| `blocked` | 4 | cannot be measured because a capability does not exist yet; `reason` cites the `unknown.md` entry |
| `unjudged` | 5 | measured, but no threshold was given |

Exit 2 is a usage error, matching `radioctl`.

**Only `pass` is a pass.** `unjudged` exists because ATP limits belong to the
ATP document, not to these scripts, and no such document exists yet: with no
threshold, the script records the measurement and refuses to call it good.
`blocked` exists so a missing capability shows up as missing instead of
silently passing.

`time_source` is always `"system"`. Timestamps come from the host clock, not
the radio PHC, because the time plane does not exist yet (U-07).

---

## The scripts

Every script prints its full usage with `-h`.

### `atp-iperf3.sh` — throughput

```sh
# on the far node, once, as a service
iperf3 -s

# TCP: fail below 1 Mbit/s
tools/atp/atp-iperf3.sh -c 10.99.0.2 -o evidence -t 10 --min-bps 1000000

# UDP at 2 Mbit/s: fail above 5 % loss or 20 ms jitter
tools/atp/atp-iperf3.sh -c 10.99.0.2 -o evidence -u -b 2M \
    --max-loss-pct 5 --max-jitter-ms 20
```

It keeps iperf3's own `-J` output as the artifact. It does not start the
server: on the radio, each node runs `iperf3 -s`, and a script must not assume
it can start processes on another node.

### `atp-mgen.sh` — loss, reordering, latency

MGEN has a sender and a receiver, so the script has three sub-commands:

```sh
# receiving node
tools/atp/atp-mgen.sh rx -o evidence -t 15

# sending node: 50 packets/s of 256 bytes for 10 s
tools/atp/atp-mgen.sh tx -c 10.99.0.2 -o evidence -t 10 -r 50 -s 256

# wherever both logs are
tools/atp/atp-mgen.sh analyze -o evidence \
    --tx evidence/raw/mgen.udp.tx.txt --rx evidence/raw/mgen.udp.rx.txt \
    --max-loss-pct 5 --max-reordered 0
```

Loss is counted against the sender's own log, so packets lost at the end of a
run are counted too.

**Latency needs synchronised clocks.** One-way latency subtracts a send time
on one node from a receive time on another. Pass `--clock-synced` only when
that is true, for example when both ends run on one host. Without it, a
`--max-latency-ms` check reports `blocked`. On the radio, synchronised time is
the job of the time plane, which does not exist yet (U-07).

### `atp-capture.sh` — packet capture

```sh
tools/atp/atp-capture.sh -i manet0 -o evidence -t 30 -f "udp port 5000" \
    --min-packets 100 --max-kernel-drops 0
```

The radio only captures. The pcap is decoded afterwards on an analysis host.

A capture can only see what the radio exposes as an interface. User IP
traffic will cross `manet0`. The MANET's own beacon and routing frames have no
defined capture point yet (U-16).

---

## Running on the radio

Requirements on the target image: POSIX `sh`, `jq`, `timeout`, and the tool
each script drives (`iperf3`, `mgen`, `tcpdump`). There is no bash and no
Python dependency, because the PetaLinux image is not guaranteed to have
either. `jq` is the one added requirement: building JSON by hand in shell is
correct until a string contains a quote.

For BIT, run the same scripts on a timer (cron or a systemd timer) with a
short duration, and keep each run's evidence directory. The exit code gives
the outcome without parsing JSON.

---

## Self-test: proving the scripts judge correctly

```sh
tools/atp/selftest/netns-selftest.sh [evidence-dir]
```

It builds two network namespaces joined by a veth pair and runs every traffic
script across them:

- on a **clean link**, where every check must `pass`;
- on a link **impaired with `tc netem`** (30 % loss, 100 ms delay), where the
  same checks must `fail`. A script that can only pass proves nothing;
- in the **honesty cases**: no threshold gives `unjudged`, latency without
  synchronised clocks gives `blocked`, and no server gives `error`.

It needs no root: it runs in an unprivileged user namespace. It takes about
45 s and exits 77 (skipped) when a tool is missing. It is not part of `ctest`,
because it moves real traffic for seconds at a time, and `ctest` stays fast
and deterministic.

On a development host:

```sh
sudo apt install iperf3 mgen tcpdump jq
```

**Sandboxed editors:** when the shell runs inside a confined snap (for
example VS Code installed as a snap), Ubuntu's AppArmor profile for
`/usr/bin/tcpdump` refuses signals from that sandbox. `timeout` cannot stop
the capture, and it hangs. Run the self-test from an ordinary terminal. This
is a property of the host, not of the radio.
