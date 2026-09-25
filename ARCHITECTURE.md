# HH-SDR — How It All Fits Together

Orientation map for the whole repository: what each piece is, how they connect,
and how far the code has got toward the target architecture.

Start here, then follow the links into component READMEs for detail.

- Target: **HH-SDR PS/PL Software Architecture — Three-Plane Bridge**, Doc
  OSL-628-BD-101 Rev 00
- Blocked contracts: [`unknown.md`](unknown.md)
- Refactor history: [`docs/RADIOD-REFACTOR.md`](docs/RADIOD-REFACTOR.md)

---

## 1. The one-paragraph version

A software-defined radio node. A MANET stack finds neighbours and routes
packets; a control daemon owns the radio hardware; a CLI and a client library
drive that daemon from other processes. Today everything runs in Linux
userspace on x86 against a mock radio. The FPGA, the OpenCPI workers, the
drivers and the timing hardware named in the target architecture **do not exist
in this repository yet** — deliberately, because their contracts are not
specified.

---

## 2. Three planes, and why the split matters

The target architecture separates traffic by *what it carries* and *how fast it
must move*. Every design decision below follows from this split.

| Plane | Carries | Rate | Path |
|---|---|---|---|
| **Control** | commands, status, faults | slow, request/response | `radioctl` → `librc` → socket → `radiod` |
| **Data** | packets | high, per-packet | app → IP stack → `manet0` → DMA → MAC → modem → RF |
| **Time** | frame/slot/hop timing | hard real-time | GNSS 1PPS → PL time base → PHC |

**The planes must not merge.** A packet must never travel through the control
socket — request/response IPC cannot carry line-rate traffic, and one slow
control client would stall the data path. Equally, timing must not queue behind
ordinary control messages.

The code enforces this today: `radiod` deliberately excludes frame transmit and
per-frame metrics from its protocol, and the packet forwarder never performs
IPC.

Each plane has its own interface contract:

| Contract | Between | Status |
|---|---|---|
| **ICD-1** | `manet0` driver ↔ PL DMA | not specified ([U-06](unknown.md)) |
| **ICD-2** | clients ↔ `radiod` | not specified ([U-01](unknown.md)) |
| **ICD-3** | radio clock driver ↔ PL time regs | not specified ([U-07](unknown.md)) |

---

## 3. The single-owner rule

The most important architectural constraint, from the drawing's Note 1:

> Only `radiod` opens OpenCPI. All other processes use `librc` (ICD-2). Never a
> second ACI instance.

One process owns the radio. Everyone else asks that process. Without this, two
processes could configure the PL simultaneously and neither would know.

**Current state: not yet enforced.** `hh-manet` still constructs its own radio
adapter and calls `open()` on it. This is *latent* — the adapter is a stub that
acquires nothing, so nothing actually conflicts — but it becomes a real
collision the moment a working backend exists. Tracked as
[U-15](unknown.md); resolving it needs the data-plane decision first.

---

## 4. What exists, at a glance

```
╔══ CONTROL PLANE — built and working ═══════════════════════════════════╗
║                                                                         ║
║   radioctl ──► librc ──► protocol ──► AF_UNIX ──► radiod ──► backend   ║
║     CLI       client      codec        socket      daemon      mock     ║
║                                                                         ║
╚═════════════════════════════════════════════════════════════════════════╝

╔══ MANET STACK — built and working, but in-process ═════════════════════╗
║                                                                         ║
║   hh-manet ──► discovery, neighbour, link health, routing,             ║
║                forwarder, failure detection, topology, self-healing    ║
║                         │                                               ║
║                         └──► hh_radio_ops_t ──► hw_adapter (STUB)      ║
║                                                                         ║
╚═════════════════════════════════════════════════════════════════════════╝

╔══ NOT IMPLEMENTED — empty, labelled directories ═══════════════════════╗
║   drivers/   manet0 net_device, radio clock / PHC                      ║
║   workers/   OpenCPI RCC workers                                        ║
║   fpga/      PL fabric: DMA, MAC, modem, AD9361, timing                ║
╚═════════════════════════════════════════════════════════════════════════╝
```

---

## 5. Repository map

| Directory | What it is | Status |
|---|---|---|
| [`radiod/`](radiod/README.md) | the radio-control daemon | ✅ working |
| [`librc/`](librc/README.md) | client library — the C API to reach `radiod` | ✅ working |
| [`radioctl/`](radioctl/README.md) | CLI front end | ✅ working |
| [`protocol/`](protocol/README.md) | wire codec shared by daemon and clients | ✅ working (**not** ICD-2) |
| `src/manet/` | MANET control plane: discovery → routing → self-healing | ✅ working |
| `src/dataplane/` | packet forwarder (fast path) | ✅ working, in-process |
| `src/core/` | types, clock, log, config, events, dispatcher | ✅ shared primitives |
| `src/radio/` | `radio.h` hardware seam + beacon wire format | ✅ the key boundary |
| `src/sca/` | SCA 2.2.2 compatibility layer | ✅ compatible, not conformant |
| `tests/`, `tools/sim/` | test harness, simulator, dev tooling | ✅ 31 tests |
| [`tools/atp/`](tools/atp/README.md) | ATP/BIT evidence scripts, Wireshark dissector | ✅ working, validated off-radio |
| [`drivers/`](drivers/README.md) | manet0, radio clock | ❌ empty |
| [`workers/`](workers/README.md) | OpenCPI RCC workers | ❌ empty |
| [`fpga/`](fpga/README.md) | PL fabric | ❌ empty |

---

## 6. How the pieces relate

### Build targets and dependency direction

```
radioctl ──► librc ──► hhsdr_protocol ──┐
                                        ├──► hhsdr_core_base   (types, clock,
radiod   ──► hhsdr_radiod ──────────────┘                       log, events)
                  └──► backends: mock_backend, hw_adapter

hh-manet ──► hhsdr_core ──► hhsdr_core_base
                  └──► manet, dataplane, sca, wire
```

**Arrows point one way. No cycles.** Two rules hold this together:

1. **`librc` never links `hhsdr_radiod`.** A client that had to link the daemon
   to talk to the daemon would pull daemon internals into every caller's build.
   Enforced by the build: `test_radiod_daemon` links only `librc` and forks the
   real daemon binary.
2. **`radiod` never links the MANET stack.** It needs four core headers and the
   protocol codec — nothing about routing or topology.

### The hardware seam — the single most important interface

Everything hardware-facing sits behind one vtable,
`hh_radio_ops_t` in [`include/hhsdr/radio/radio.h`](include/hhsdr/radio/radio.h):

```
open   close   transmit   set_rx_callback
get_status   get_link_metrics   set_channel   poll
```

Eight functions. Implementing them is **the entirety** of what FPGA integration
must supply. Nothing above the seam knows about AXI registers, DMA descriptors,
PHY control or RF tuning — none of which are guessed at anywhere.

Who calls what:

| Caller | Uses | Plane |
|---|---|---|
| `radiod` | `open`, `close`, `get_status`, `set_channel`, `poll` | control |
| `hh-manet` | all eight, including `transmit`, `set_rx_callback` | control + **data** |

That asymmetry is the unresolved issue: **one vtable currently spans two
planes**. Splitting it is [U-15](unknown.md).

### Two daemons, no connection

`radiod` and `hh-manet` are separate processes that **do not talk to each
other** today. Each builds its own radio backend.

In the target, MANET routing takes neighbour statistics *from `librc`* — i.e.
through `radiod`. The existing stack instead gets them by sending and receiving
its own beacons. Those are incompatible data-flow models, and the target names
**OLSRv2 (RFC 7181)** rather than this custom distance-vector stack. Deferred
decision: [U-09](unknown.md).

---

## 7. Following a command end to end

`radioctl set-channel 11`:

```
1. radioctl        parse_command() maps "set-channel" → HH_RC_CMD_SET_CHANNEL
                   validates args BEFORE connecting
2. librc           hh_rc_client_connect()  → AF_UNIX/SOCK_STREAM
                   hh_rc_client_call()     → write, then block for one line
3. protocol        hh_rc_request_format()  → "set_channel channel=11\n"
   ─────────────────── socket boundary ───────────────────
4. radiod          poll() wakes → accept/read → memchr for '\n'
                   hh_rc_request_parse()
                   transition_allowed()    → legal only from running
                   hh_radio_set_channel()  → the backend vtable
                   hh_rc_response_format() → "ok set_channel state=running …"
5. back out        librc parses the line, radioctl prints key=value, exit 0
```

**Where a real radio would plug in:** step 4's `hh_radio_set_channel()`. Today
that reaches `mock_backend.c`. A real backend implements the same vtable — steps
1–3 and 5 never change.

---

## 8. Design decisions worth knowing

**Single-threaded, no locks.** Zero `pthread` or `mutex` in the repository.
`radiod` serves up to 16 clients round-robin from one `poll()` loop. Requests
serialize, so there is no shared-state hazard. Whether PL ownership forces
threads is [U-12](unknown.md).

**No dynamic allocation in the control path.** Fixed arrays throughout — 16
client slots, 512-byte line buffers, bounded queues. A 17th connection is
closed, not accommodated.

**Injected clock.** Components take a `hh_clock_t *`, never read wall-clock time
directly. Tests bind a virtual clock, so scenario timing is deterministic and
nothing sleeps. This is why 31 tests run in 0.6 s.

**Honest failure over fake success.** `hw_adapter.c` returns
`HH_ERR_NOT_IMPLEMENTED` from every hardware operation and logs why.
`hh-manet` with `radio_adapter="hw"` exits non-zero at startup. That is
intended: a node that cannot reach hardware says so rather than appearing to
run.

**Structured logging.** One `key=value` record per line, so operators and tests
read the same output. Tests assert on log records rather than internal state,
keeping assertions tied to observable behaviour.

---

## 9. Two things that look finished but are not

### The protocol is not ICD-2

The target specifies **versioned TLV**. What is implemented is line-oriented
ASCII `key=value` — no version field, no TLV, no transaction IDs, no async
events.

This is not an oversight. **No ICD-2 specification exists** ([U-01](unknown.md)),
so a TLV layout would be invented, then reconciled later against the real one.
The codec lives in its own `protocol/` component precisely so an ICD-2 codec can
be added beside it without touching `radiod` or `librc`.

### radiod is not yet the PL owner

No OpenCPI integration exists anywhere — no ACI call, no application handle, no
worker reference. The only mentions of OpenCPI in the source are comments saying
it is absent. Blocked on the application XML, worker names,
properties and ACI lifecycle ([U-03](unknown.md), [U-04](unknown.md)), plus the
C11-versus-C++ boundary ([U-11](unknown.md)).

What makes the gap safe: `radiod` depends only on `hh_radio_ops_t` and never
names a concrete backend. An OpenCPI backend implements that same contract and
drops into `radiod/src/backends/`.

---

## 10. Current versus target

| Area | Now | Target | Blocked by |
|---|---|---|---|
| Control daemon | ✅ working, mock backend | PL/OpenCPI owner | U-03, U-04 |
| Client library + CLI | ✅ working | same | — |
| Wire protocol | ASCII `key=value` | versioned TLV (ICD-2) | U-01 |
| Async events | none (poll only) | radiod pushes events | U-01, U-05 |
| Fault registry | ✅ in-process | exposed over ICD-2 | U-01, U-05 |
| PL ownership | none | single owner | U-03, U-15 |
| Data plane | in-process forwarder | manet0 → DMA → MAC → RF | U-06, U-15 |
| Time plane | **absent entirely** | 1PPS → PL → PHC | U-07 |
| MANET routing | custom distance-vector | OLSRv2 (RFC 7181) | U-09 |
| RCC workers | none | 5 PS workers | U-03 |
| FPGA fabric | none | DMA, MAC, modem, AD9361 | U-06, U-08, U-14 |
| Test automation | ✅ ATP/BIT scripts, dissector, JSON evidence | same, on the radio over `manet0` | U-06, U-07, U-16 |

---

## 11. Getting started

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j8
ctest --test-dir build                      # 31 tests, ~0.6 s
```

Drive the control plane — **two terminals**, so daemon logs and command output
stay separate:

```bash
# terminal 1
./build/radiod/radiod -v debug

# terminal 2
R=./build/radioctl/radioctl
$R init
$R configure 42
$R start
$R status
```

Order matters: the state machine is `created → initialized → configured →
running`. Skipping a step returns `ESTATE`, and the daemon log names which
states the command *would* have been legal from.

Run the MANET simulator:

```bash
./build/hh-manet-sim          # multi-node simulation
./build/demo_selfheal         # self-healing, readable output
```

`./build/hh-manet` exits non-zero by design — no hardware backend exists.

---

## 12. Where to read next

| Question | Document |
|---|---|
| How does the daemon work internally? | [`radiod/README.md`](radiod/README.md) |
| How do I write a client? | [`librc/README.md`](librc/README.md) |
| What commands exist, what exit codes? | [`radioctl/README.md`](radioctl/README.md) |
| What is actually on the wire? | [`protocol/README.md`](protocol/README.md) |
| What is blocked and who owns it? | [`unknown.md`](unknown.md) |
| How did the structure get this way? | [`docs/RADIOD-REFACTOR.md`](docs/RADIOD-REFACTOR.md) |
| What must hardware supply? | [`docs/HARDWARE-DEPENDENCIES.md`](docs/HARDWARE-DEPENDENCIES.md) |
| What are the HTI interfaces? | [`docs/HW-SW Interface.md`](docs/HW-SW%20Interface.md) |
| How does the MANET stack work? | [`docs/IMPLEMENTATION-WALKTHROUGH.md`](docs/IMPLEMENTATION-WALKTHROUGH.md) |
| How would this deploy? | [`docs/DEPLOYMENT-ARCHITECTURE.md`](docs/DEPLOYMENT-ARCHITECTURE.md) |
| What about SCA? | [`docs/SCA-COMPATIBILITY.md`](docs/SCA-COMPATIBILITY.md) |

---

## 13. The rule that governs this codebase

Nothing unspecified is invented. No TLV IDs, no OpenCPI worker names, no FPGA
registers, no AD9361 APIs, no ICD byte layouts.

Where a contract is missing, the code either does not exist or fails loudly and
honestly. Every such gap is recorded in [`unknown.md`](unknown.md) with what is
missing, why it is needed, where it must come from, and what can proceed
meanwhile.

This is why the repository looks incomplete against the architecture drawing —
it is incomplete, accurately, rather than complete-looking and wrong.
