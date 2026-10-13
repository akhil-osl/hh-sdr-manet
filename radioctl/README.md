# `radioctl/` — the radio-control CLI

The command-line front end to `radiod`. This is the public control interface:
the architecture requires test automation (ATP/BIT scripts, iperf3 harnesses,
CORE/EMANE runs) to drive the radio through it rather than through private or
internal access.

**Target:** `radioctl`
**Depends on:** `librc` **only** — never `hhsdr_radiod`, never OpenCPI, never the PL

```
operator / ATP script ──► radioctl ──► librc ──► protocol ──► radiod
```

Every radio operation `radioctl` performs is a **request that `radiod`
services**. It holds no radio state and touches no hardware, which is what keeps
the single-owner rule intact.

---

## Quick start

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug && cmake --build build -j8

./build/radiod/radiod &                        # start the daemon

R=./build/radioctl/radioctl
$R init
$R configure 42
$R start
$R status
$R set-channel 11
$R stats
$R shutdown
```

Real output from that sequence:

```
$ radioctl init
state=initialized

$ radioctl configure 42
state=configured

$ radioctl start
state=running

$ radioctl status
state=running
operational=1
channel=0
frequency_hz=0.0
waveform_id=0

$ radioctl set-channel 11
state=running

$ radioctl stats
frames_tx=0
frames_rx=0
tx_errors=0
rx_errors=0
requests_total=6
requests_rejected=0
```

---

## Commands

These are **the protocol's ten verbs, one-to-one**. There are no others, and
none are synthesised client-side.

| Command | Arguments | Purpose |
|---|---|---|
| `init` | — | `created` → `initialized` |
| `configure` | `<node_id> [channel]` | set identity; → `configured` |
| `start` | — | open the backend; → `running` |
| `stop` | — | close the backend; → `stopped` |
| `status` | — | lifecycle state + radio status |
| `stats` | — | frame and request counters |
| `set-channel` | `<channel>` | request a channel change (`running` only) |
| `inject-fault` | `<kind>` | inject a fault for validation |
| `clear-fault` | — | clear **all** injected faults |
| `shutdown` | — | release the daemon; it exits afterwards |

Fault kinds: `none`, `tx_failure`, `rx_silence`, `hw_fault`, `backend_io`.

**Both spellings work.** The CLI accepts hyphens (`set-channel`) and the wire
protocol's underscores (`set_channel`), so scripts written either way run
unchanged.

### Options

| Option | Meaning |
|---|---|
| `-s <path>` | socket path (default `/tmp/hh-radiod.sock`) |
| `-h`, `--help` | usage; exits 0 |

---

## Exit codes — the contract with your scripts

This is the part that matters most for automation:

| Code | Meaning | When |
|---|---|---|
| **0** | Success | `radiod` accepted the request |
| **2** | Usage error | Bad command or missing argument. **Refused locally — `radiod` is never contacted.** |
| **3** | Transport failure | Could not reach `radiod`: not running, wrong path, socket gone |
| **4** | Rejected | `radiod` answered and **refused** the request |

**3 and 4 are deliberately distinct.** A rejection is a *successful* exchange
carrying a refusal — the daemon is alive and said no. Collapsing them would make
"the radio is in the wrong state" indistinguishable from "the radio is gone",
which is exactly the distinction a BIT script needs.

```
$ radioctl start                          # before init
radioctl: start rejected (ESTATE)
$ echo $?
4

$ radioctl -s /tmp/nope.sock status
radioctl: cannot connect to radiod at /tmp/nope.sock (EIO)
$ echo $?
3

$ radioctl bogus-command
radioctl: unknown command 'bogus-command'
$ echo $?
2
```

Diagnostics go to **stderr**; command output goes to **stdout**. So
`radioctl status > out.txt` captures only the payload.

### Scripting pattern

```bash
set -e                                     # any non-zero aborts

radioctl init
radioctl configure "$NODE_ID"
radioctl start

if ! radioctl set-channel "$CH"; then
    case $? in
        3) echo "radiod unreachable — is it running?" >&2 ;;
        4) echo "channel change refused — radio not in running state" >&2 ;;
    esac
    exit 1
fi
```

Parsing output is straightforward, since every line is `key=value`:

```bash
state=$(radioctl status | grep '^state=' | cut -d= -f2)
[ "$state" = running ] || { echo "not running: $state" >&2; exit 1; }
```

---

## Output format

One `key=value` per line. Which keys appear depends on the command:

- **`status`** → `state`, `operational` (0/1), `channel`, `frequency_hz`, `waveform_id`
- **`stats`** → `frames_tx`, `frames_rx`, `tx_errors`, `rx_errors`, `requests_total`, `requests_rejected`
- **everything else** → `state` alone

`radioctl` prints exactly the fields the protocol carries for that response and
nothing more — no derived, defaulted or placeholder values.

---

## Implementation

[`radioctl_main.c`](radioctl_main.c), just under 200 lines, no state:

1. Parse options (`-s`, `-h`), then the subcommand via `parse_command()` —
   which maps hyphenated spellings then falls through to `hh_rc_cmd_parse()`.
2. Read per-command arguments. Only `configure`, `set-channel` and
   `inject-fault` take any; anything extra is a usage error.
3. `hh_rc_client_connect()` → `hh_rc_client_call()` → `hh_rc_client_close()`.
4. Map the outcome to an exit code; print the payload via `print_response()`.

**One connection per invocation.** A CLI process is short-lived, so there is
nothing to gain from a persistent connection — and `radiod` tolerates up to 16
concurrent clients regardless.

Validation happens **before** connecting, so a malformed command costs nothing
and never disturbs the daemon.

---

## Known limitations

**No timeout.** Inherited from `librc`: if `radiod` accepts a connection and
then never replies, `radioctl` blocks indefinitely. Wrap it in `timeout(1)` if
that matters:

```bash
timeout 5 radioctl status || echo "timed out or failed: $?"
```

**No `--json`.** Output is `key=value` only. Easy to add, deliberately not
guessed at.

**No `faults` command.** `radiod` maintains a full fault registry internally,
but it is not exposed over the protocol: reporting it needs a new response
payload or an async event, and neither wire format is specified
([`../unknown.md`](../unknown.md), U-01/U-05).

**No `reset`.** Recovery is `clear-fault`, or `stop` then `start`.

**`set-channel` takes an opaque integer.** The mapping from channel index to RF
frequency is undefined (`radio.h` records it as *"logical channel index; mapping
to RF TBD"*), and its relationship to a hopset is unspecified (U-14). The value
is passed through untouched.

---

## Testing

```bash
ctest --test-dir build -R test_radioctl --output-on-failure
```

[`tests/test_radioctl.c`](tests/test_radioctl.c) runs the **real `radioctl`
binary against the real `radiod` binary** — both forked as child processes, with
stdout captured and exit codes asserted. Five cases: full lifecycle, fault
inject/clear cycle, rejection (exit 4), absent daemon (exit 3), and malformed
invocations (exit 2).

It asserts on exit status and stdout because those *are* the CLI's contract with
its callers — testing internals instead would miss the thing that actually
breaks scripts.
