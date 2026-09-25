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
| `atp-decode.sh` | a pcap file | analysis host | analysis host |
| `atp-radioctl.sh` | `radioctl` (public CLI) | mock backend | real backend |

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

### `atp-radioctl.sh` — control-plane checks

Everything goes through `radioctl`, the public control interface the
architecture requires test automation to use. The script relies on
`radioctl`'s documented exit codes and `key=value` output, and on nothing else.

```sh
# BIT: read-only, safe on a radio in service
tools/atp/atp-radioctl.sh health -o evidence --max-tx-errors 0

# ATP: walks the whole lifecycle; refuses unless radiod is freshly started
tools/atp/atp-radioctl.sh lifecycle -o evidence --node-id 42 --set-channel 11
```

| Mode | Does | Judges |
|---|---|---|
| `health` | `status`, `stats` only; changes nothing | radiod answers; state is `running` (or `--expect-state`); `operational=1`; optional counter limits |
| `lifecycle` | `start` before `init` (must be rejected), `init`, `configure`, `start`, `set-channel`, `inject-fault hw_fault`, `clear-fault`, `stats`, `stop` | every step returns the documented exit code and state |

**`lifecycle` changes the radio's state**, so it refuses to run unless radiod
is in its initial `created` state. It can never reconfigure a radio that is
already in use. `health` is the one to schedule for BIT.

In `health`, an unreachable or silent radiod is a **`fail`**, not an `error`.
The check worked, and the radio is the thing that is down. That is the same
distinction `radioctl` keeps between exits 3 and 4. `radioctl` has no timeout
of its own, so every call is bounded by `-T` (default 5 s).

The counters from `stats` are cumulative since radiod started. A BIT limit on
them is a limit on the total, not a rate.

The lifecycle expectations are radiod's documented behaviour, not invented
limits. Against the real hardware backend today, `start` fails honestly
(`hw_adapter` returns `HH_ERR_NOT_IMPLEMENTED`), so the lifecycle check
reports `fail` until a PL backend exists (U-03, U-04). That is the correct
result.

### `atp-decode.sh` — decode a capture into evidence (analysis host)

```sh
tools/atp/atp-decode.sh -r evidence/raw/capture.x.pcap -o evidence \
    --min-frames 100 --max-bad-frames 0 --max-seq-gaps 5
```

It runs tshark with the MANET dissector and writes counts per decode status.
For beacons it also reports sequence continuity per node: a gap is a beacon
the capture point never saw. Sequence wrap-around at 2^32 is handled. The
per-frame decode is kept as an artifact, so every number in the summary can
be checked.

This runs on the analysis host, never on the radio, because it needs tshark
with Lua.

---

## The Wireshark dissector

[`wireshark/hh_manet.lua`](wireshark/hh_manet.lua) decodes the two payload
layouts defined in [`src/radio/wire.c`](../../src/radio/wire.c):

| Protocol | Filter name | Layout |
|---|---|---|
| Beacon | `hhbeacon` | 48 bytes, fixed, `protocol_version` 1 |
| Routing update | `hhroute` | 5 + 11 × count bytes, count ≤ 24 |

```sh
tshark -X lua_script:tools/atp/wireshark/hh_manet.lua -r beacons.pcap -V
# or, for the GUI:
cp tools/atp/wireshark/hh_manet.lua ~/.local/lib/wireshark/plugins/
```

Every frame gets a `hhbeacon.status` or `hhroute.status` field. It holds the
same verdict the C decoder reaches: `ok`, `unsupported_version`, `malformed`
or `invalid_node`. A frame with an unknown protocol version is not decoded
past the version field, exactly as `hh_beacon_decode` refuses it.

**What it does not claim:**

- **The layouts are this implementation's choice, not a specification.** The
  architecture leaves the air format TBD. The dissector labels encoding-TBD
  fields as such (capability bits, coordinate units).
- **There is no frame header to decode.** Frame kind, source and destination
  are `hh_frame_t` struct fields that are never serialised. Captures
  therefore use private pcap link types instead of a header: `DLT_USER0` for
  beacons and `DLT_USER1` for routing updates. This is a test-tooling
  convention, not an air format.
- **On the radio, MANET frames have no defined capture point yet** (U-16).
  Until one exists, the dissector decodes captures generated from the
  encoder, not live radio traffic.

### How it is validated

```sh
ctest --test-dir build -R check_dissector --output-on-failure
```

[`wireshark/hh_wire_pcapgen.c`](wireshark/hh_wire_pcapgen.c) writes golden
captures with the **real encoder**. It also writes `expected.json`, which
records what the **real decoder** makes of each frame. The cases cover full
and empty optional fields, integer maxima, a future protocol version, a
truncated frame, node id 0, and routing updates with 0, 1 and 24 entries plus
out-of-range counts.
[`wireshark/check-dissector.sh`](wireshark/check-dissector.sh) decodes the
captures with tshark and compares every status and every field. It then
checks that `atp-decode.sh` summarises the same capture correctly.

The C code is the reference, not a second hand-written description of the
layout. If `wire.c` changes and the dissector does not, this test fails. The
test is skipped (exit 77) on hosts without tshark with Lua support, or
without jq.

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

## Self-tests: proving the scripts judge correctly

Three self-tests cover the tooling. Two run under `ctest`:

| Test | Covers | In ctest |
|---|---|---|
| `check_dissector` | dissector vs the C codec; `atp-decode.sh` | yes (skipped without tshark) |
| `check_atp_radioctl` | `atp-radioctl.sh` against the real `radiod` and `radioctl` binaries, through fresh, running, faulted and unreachable states | yes (skipped without jq) |
| `selftest/netns-selftest.sh` | iperf3, MGEN and capture scripts over real traffic | no, run by hand |

### The traffic self-test

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
sudo apt install iperf3 mgen tcpdump jq tshark
```

**Sandboxed editors:** when the shell runs inside a confined snap (for
example VS Code installed as a snap), Ubuntu's AppArmor profile for
`/usr/bin/tcpdump` refuses signals from that sandbox. `timeout` cannot stop
the capture, and it hangs. Run the self-test from an ordinary terminal. This
is a property of the host, not of the radio.
