# HH-SDR Self-Healing MANET

A C11 implementation of the **Application / Control Software and networking**
portion of the HH-SDR software-defined radio architecture: a self-healing mobile
ad-hoc network (MANET) control plane, data plane, and radio abstraction.

Implemented from two reference documents kept with the project:

- `HH-SDR-Self-Healing-MANET-Architecture.md` — the architecture (referred to
  below as **Doc 1**)
- `docs/HH-SDR-Network-Topology-HWSW-Interface-Specification.md` — the HW/SW
  interface specification, which numbers every interface `HTI-01` … `HTI-16`

---

## 1. Overview

### What this is

A radio network where **every node is also a router**, there is no base station,
and no central coordinator. Nodes discover each other over the air, build
multi-hop routes, continuously assess link quality, and repair the network
themselves when links degrade, nodes fail, or the network splits apart.

### The problem it solves

In a tactical or field deployment there is no fixed infrastructure. Radios move,
terrain blocks paths, interference comes and goes, and units are lost. The
network must:

- form itself with no configuration beyond a node identity
- route traffic across several hops when nodes cannot hear each other directly
- notice a failing link **before** it fails outright, and route around it
- distinguish *"my neighbour is gone"* from *"the channel is jammed"* from
  *"my own radio has died"* — because the correct response differs
- keep operating when the network splits, and rejoin cleanly when it heals

### MANET and the SDR/radio layer

Two different concerns, deliberately separated:

| Layer | Concern | Status here |
|---|---|---|
| **MANET networking** | Who are my neighbours? Where do I send this packet? Is this link healthy? Has it failed? | **Implemented** |
| **Radio / SDR** | Modulation, RF, sample streams, PHY metrics | **Abstracted** — the hardware does not exist yet |

The MANET stack needs only two things from a radio: *send this frame*, and
*here is a received frame plus the RF quality measured for it*. Everything
below that — waveform, modem, PHY, RF front end — sits behind one interface.

This matters because **the FPGA/PL side of HH-SDR is not available yet.** The
networking stack is complete and testable today; the radio is a defined seam
waiting for hardware.

---

## 2. Architecture

### The stack

```
    ┌──────────────────────────────────────────────────────────┐
    │  Application            hh_node_send(dst, payload, len)  │
    ├──────────────────────────────────────────────────────────┤
    │  MANET Control Plane            (7 components, C11)      │
    │                                                          │
    │    Discovery ──► Neighbor ──┬──► Routing ──┐             │
    │        ▲                    ├──► Topology  │             │
    │        │                    └──► LinkHealth│             │
    │        │                            │      │             │
    │        └──── cadence hint ──────────┤      │             │
    │                                     ▼      │             │
    │                          FailureDetector   │             │
    │                                     │      │             │
    │                                     ▼      │             │
    │                           SelfHealing ─────┘             │
    │                                                          │
    │   all wired through a typed pub/sub event bus            │
    ├──────────────────────────────────────────────────────────┤
    │  Data Plane          Packet Forwarder                    │
    │                      reads route snapshot, never routes  │
    ├──────────────────────────────────────────────────────────┤
    │  Radio / SDR Abstraction     hh_radio_ops_t  (8 fns)     │  ◄── THE SEAM
    ├──────────────────────────────────────────────────────────┤
    │  Hardware Adapter                                        │
    │    hw  → FPGA/PL — NOT IMPLEMENTED (returns ENOTIMPL)    │
    │    mock → tests/sim, used by tests and the simulator     │
    ├──────────────────────────────────────────────────────────┤
    │  FPGA / PL / PHY / RF        does not exist yet          │
    └──────────────────────────────────────────────────────────┘
```

### The seven control-plane components

| Component | Owns | Does **not** own |
|---|---|---|
| **Discovery** | Beacon scheduling, acquisition vs steady cadence, replay rejection | Whether a neighbour is valid |
| **Neighbor Manager** | The one-hop neighbour table — *the* authoritative source | Multi-hop reachability |
| **Link Health** | Fusing RSSI/SNR/PER/silence into a link state, with hysteresis | What to *do* about a bad link |
| **Routing** | Route computation, next-hop selection, the published route table | Link classification |
| **Failure Detector** | Confirming a failure after a debounce hold-down | Recomputing routes |
| **Topology** | An operator-facing graph — a *read model* | Anything on the routing path |
| **Self-Healing** | Repair orchestration: alternate routes, rediscovery, channel change | Per-packet forwarding |

### Control plane vs data plane

This split is the single most important property of the design.

**Control plane** decides *where traffic should go*. It runs discovery,
evaluates link health, computes routes, and repairs failures. It is allowed to
be slow.

**Data plane** moves packets. For every packet it does exactly one thing:
read the current route snapshot pointer and look up a next hop.

```
   Routing Engine                          Packet Forwarder
   ──────────────                          ────────────────
   builds a new immutable                  reads one atomic
   snapshot off to the side                pointer, flat next-hop
            │                              lookup, transmits
            │  single atomic
            └──── pointer store ───────►   never takes a lock
                                           never waits on control plane
                                           never computes a route
```

The forwarder takes **no lock**, performs **no routing computation**, and never
walks the topology graph. A reader holding an older snapshot keeps a coherent
view rather than seeing a half-updated table.

### The radio / SDR abstraction

One vtable, `hh_radio_ops_t` (`include/hhsdr/radio/radio.h`), with eight
functions:

| Function | HTI | Purpose |
|---|---|---|
| `open` / `close` | — | Acquire and release the radio |
| `transmit` | HTI-03 | Send one framed message |
| `set_rx_callback` + `poll` | HTI-04, HTI-05 | Deliver received frames plus measured RF metrics |
| `get_status` | HTI-02 | Channel, waveform, error counters |
| `get_link_metrics` | HTI-05 | Per-neighbour RSSI / SNR / PER / retransmits |
| `set_channel` | HTI-14 | Retune, or report unsupported |

The unit of exchange is a **frame**, not a sample buffer. The MANET stack has no
use for baseband samples, so every DSP and PHY decision stays below the seam.

### Where the future FPGA fits

Implementing those eight functions is the **entire** FPGA integration task.

```
   Today                              After FPGA integration
   ─────                              ──────────────────────
   MANET stack       (works)          MANET stack        (unchanged)
        │                                  │
   hh_radio_ops_t    (defined)        hh_radio_ops_t     (unchanged)
        │                                  │
   hw adapter        (ENOTIMPL)       hw adapter         (real)
        │                                  │
   ✗ no hardware                      FPGA / PL / PHY / RF
```

There is **no AXI register map, DMA API, PHY control API, RF tuner API, or
sample-streaming interface anywhere in this repository** — inventing them before
the hardware contract exists would guarantee rework. See
[docs/HARDWARE-DEPENDENCIES.md](docs/HARDWARE-DEPENDENCIES.md).

---

## 3. Implementation status

### Implemented and tested (production C11)

Discovery · Neighbor management · Link health monitoring · Routing · Packet
forwarding · Failure detection · Topology · Self-healing · Node assembly and
lifecycle · Configuration · Structured logging · Telemetry · SCA compatibility
layer.

174 tests across 16 suites, clean under AddressSanitizer and
UndefinedBehaviorSanitizer.

### Simulated (test/development only, never in production)

Virtual radio, virtual network medium, packet loss, delay, RSSI/SNR/PER
injection, node and link failure, partitions, merges, mobility.

All of this lives under `tests/sim/` and `tools/sim/` and is **never linked into
the `hh-manet` daemon.**

### Hardware-independent

The whole MANET stack. It depends on `hh_radio_ops_t` and nothing below it.

### Hardware-dependent — not implemented

| Item | Status |
|---|---|
| FPGA/PL radio adapter | Stub returning `HH_ERR_NOT_IMPLEMENTED` |
| Real RF metric units (dBm scaling, PER encoding) | Interim floats; encoding TBD |
| Beacon wire byte layout | Versioned interim format; PHY decision pending |
| Channel/waveform encodings | Interim bit-fields |

**No real RF behaviour is claimed anywhere.** Every metric the stack consumes
today is supplied by the test harness.

### SCA 2.2.2 — compatible, not conformant

| Claim | Status |
|---|---|
| SCA-compatible **architecture** | **Yes** — classification, lifecycle, properties, ports follow SCA 2.2.2 |
| SCA **implementation** | **Partial** — lifecycle enforced, 23-property surface, port declarations |
| SCA **conformance** | **No, and not claimed** — no CORBA ORB, no Domain Profile XML, no conformance testing |

Full gap list: [docs/SCA-COMPATIBILITY.md](docs/SCA-COMPATIBILITY.md).

### Not yet implemented

QoS queueing · security/crypto interfaces · Python operator console (the C
telemetry export exists and is ready to feed one) · CORBA/ORB layer · Domain
Profile XML · PetaLinux packaging (designed in
[docs/DEPLOYMENT-ARCHITECTURE.md](docs/DEPLOYMENT-ARCHITECTURE.md), not built).

---

## 4. Repository structure

```
include/hhsdr/          Public headers
  core/                 types, seq arithmetic, clock, log, config, events, dispatcher
  radio/                radio.h  ◄── THE hardware seam; wire format; hw adapter
  manet/                discovery, neighbor, link_health, routing, route_table,
                        failure_detector, topology, self_healing, node, telemetry
  dataplane/            forwarder.h — the fast path
  sca/                  resource.h — SCA 2.2.2 compatibility layer

src/                    Implementations, mirroring include/
  adapters/hw/          hw_adapter.c — PLACEHOLDER, returns ENOTIMPL
  main.c                hh-manet daemon entry point

tests/
  unit/                 per-component tests
  integration/          component-seam tests
  scenario/             14 multi-node MANET scenarios
  sim/                  TEST-ONLY: mock_radio, vclock, netsim
  hh_test.h             dependency-free assertion harness

tools/
  sim/                  hh-manet-sim — the interactive simulation driver
  demo_selfheal.c       minimal self-healing demonstration

config/node.example.conf   every tunable, documented
docs/                      hardware, SCA, and deployment documentation
```

**Key files** for a new engineer, in reading order:

1. `include/hhsdr/radio/radio.h` — the hardware seam, and what is deliberately absent
2. `include/hhsdr/manet/node.h` — how the components are wired together
3. `include/hhsdr/manet/route_table.h` — the lock-free control/data plane contract
4. `tools/sim/hh_manet_sim.c` — the scenarios, and how to drive the stack

---

## 5. Building

Requires a C11 compiler and CMake ≥ 3.10. **No external dependencies.**

```sh
mkdir -p build && cd build
cmake ..
make -j4
```

Sanitizer build (AddressSanitizer + UndefinedBehaviorSanitizer):

```sh
mkdir -p build-asan && cd build-asan
cmake .. -DHH_SANITIZE=ON
make -j4 && ctest
```

Note: this project targets CMake 3.10, which predates `cmake -S . -B build`.
Use the `mkdir build && cd build && cmake ..` form shown above.

---

## 6. Running

All commands assume you are in `build/`.

### The simulation — start here

```sh
./hh-manet-sim --scenario basic          # discovery and routing
./hh-manet-sim --scenario multihop --trace   # per-packet, per-hop tracing
./hh-manet-sim --scenario all            # every scenario in sequence
./hh-manet-sim --help                    # all options
```

### The daemon

```sh
./hh-manet -c ../config/node.example.conf -n 1
```

This **will fail to start**, and that is correct:

```
node start failed: ENOTIMPL

The radio adapter has no hardware backend: the FPGA/PL software API
is not available, so no radio contract can be honored yet.
```

The daemon is the real production entry point. It cannot run without a radio,
and it says so rather than pretending. The stack itself is exercised by the
tests and the simulator.

### Tests

```sh
ctest                          # all 174 tests, ~0.25s
ctest --output-on-failure      # show output for failures
ctest -R routing               # one suite
ctest -V -R scenarios          # verbose, see each scenario name

./tests/test_scenarios         # run a suite directly
./tests/test_link_health
```

| Category | Command |
|---|---|
| Unit tests | `ctest -R 'test_(seq\|config\|log\|dispatcher\|wire\|radio\|discovery\|neighbor\|link_health\|routing\|forwarder\|recovery\|telemetry_sca)'` |
| Integration tests | `ctest -R test_integration` |
| Scenario tests | `ctest -R test_scenarios` |
| Simulator tests | `ctest -R test_simui` |

---

## 7. Testing — four distinct levels

| Level | Question it answers | Where | Count |
|---|---|---|---|
| **Unit** | Does this component behave correctly in isolation? | `tests/unit/` | 141 |
| **Integration** | Do components work together over the event bus? | `tests/integration/` | 10 |
| **Scenario** | Does the *network* behave correctly end to end? | `tests/scenario/` | 14 |
| **Simulator** | Is what the simulator shows actually real? | `tests/unit/test_simui.c` | 9 |

**Interactive simulation** (`hh-manet-sim`) is a *fifth* thing and is not a
test: it exists so an engineer can **see** the system operating. It prints
network state and events; it does not assert.

### What each level catches

Unit tests verify logic precisely — hysteresis thresholds, sequence wraparound,
LRU eviction, two-phase invalidation.

Integration tests verify the seams — that a link-health transition really
reaches the failure detector, and that recovery really drives routing.

Scenario tests verify emergent behaviour, and they earn their keep. Two real
protocol bugs were found only at this level, because both need three nodes
exchanging updates over time:

- a **count-to-infinity loop** where routes through a dead link never withdrew
- **good routes ageing out** on a quiet network

Simulator tests verify that the observability itself is honest — that hop traces
match the route the production forwarder chose, and that statistics come from
production counters rather than a parallel tally.

---

## 8. MANET simulation

### How it works

Each virtual node is a **real `hh_node_t`** running the production control plane
and data plane. Only the hardware boundary is replaced:

```
    Production                        Simulation
    ──────────                        ──────────
    Application                       hh-manet-sim  (driver)
        │                                 │
    MANET networking      ◄── same ──►  MANET networking   (unmodified)
        │                                 │
    Radio abstraction     ◄── same ──►  Radio abstraction  (unmodified)
        │                                 │
    hw adapter (FPGA)                   mock radio
        │                                 │
    FPGA / PL / RF                      virtual network
```

There is **no second networking implementation.** The simulator replaces the
radio, not the stack.

### Creating nodes and moving packets

```c
netsim_t sim;
netsim_init(&sim, 1234);                    /* seeded, reproducible */

netsim_add_node(&sim, 1);                   /* node A */
netsim_add_node(&sim, 2);                   /* node B */
netsim_add_node(&sim, 3);                   /* node C */

netsim_link_up(&sim, 1, 2, -55.0f);         /* A <-> B at -55 dBm */
netsim_link_up(&sim, 2, 3, -55.0f);         /* B <-> C */

netsim_start_all(&sim);                     /* SCA configure + start */
netsim_run(&sim, 4000, 10);                 /* 4 s virtual, 10 ms steps */

/* Send through the production data plane */
hh_node_send(&netsim_node(&sim, 1)->node, 3, payload, len, sim.vc.now);
```

Each node independently owns its node id, neighbour table, routing state,
topology, link-health state, failure/recovery state, radio instance, and
forwarding queues — because each is a complete `hh_node_t`.

**How a packet actually moves:** `hh_node_send` → forwarder looks up the route
snapshot → transmits to the next hop via the mock radio → the virtual medium
applies loss/delay/RSSI and delivers into the next node's radio → that node's
forwarder decrements TTL and repeats → the destination delivers locally.

### What the virtual network can control

| Control | Function |
|---|---|
| Connectivity | `netsim_link_up` / `netsim_link_down` / `netsim_link_up_directed` |
| Packet loss | `netsim_link_set_loss` (0.0 – 1.0) |
| Delay | `netsim_link_set_delay` (ms) |
| RSSI / SNR / PER | `netsim_link_set_quality` |
| PHY errors | `netsim_link_set_phy_errors` |
| Node failure | `netsim_node_fail` / `netsim_node_recover` |
| Partition / merge | `netsim_partition` / `netsim_merge` |

Asymmetric links (beacons landing one way only) are expressible with the
directed variants — the link table is directional.

### Determinism

Timing is fully virtual and loss uses a seeded PRNG, so the same seed replays
exactly:

```sh
./hh-manet-sim --scenario multihop --seed 7    # identical every run
```

---

## 9. Example scenario: A — B — C, walked through

```sh
./hh-manet-sim --scenario recovery --trace
```

**The topology and what happens:**

```
   A <-> B <-> C        break A-B, detect, restore, recover
```

**Step 1 — nodes start and discover each other.** Real events, captured from
the production log stream:

```
  [00.010] A  DISCOVERY        entered steady state (neighbor_heard)
  [00.010] A  NEIGHBOR UP      discovered B
  [00.010] A  ROUTE INSTALLED  to B via B
  [00.010] B  NEIGHBOR UP      discovered A
  [00.010] B  NEIGHBOR UP      discovered C
  [00.210] A  ROUTE INSTALLED  to C via B          ◄── multi-hop route formed
```

**Step 2 — data flows A → C** through the production forwarder:

```
  route: A -> B -> C

  [00.000]   SEND     packet 001  A -> C
  [00.000]   FORWARD  packet 001  A -> B
  [00.010]   DELIVER  packet 001 -> C (2 hops, 10ms)
```

**Step 3-4 — break A-B, and watch the real state machine run:**

```
  [00.000] >> LINK DOWN  A <-> B
  [00.320] A  LINK DEGRADED    B: Healthy -> Degraded (cause: node-failure)
  [00.320] A  CADENCE ADAPTED  200ms -> 100ms        ◄── beacons speed up
  [00.520] A  LINK SUSPECT     B: Degraded -> SuspectedFailure
  [00.920] A  LINK FAILED      B: SuspectedFailure -> Failed
  [00.920] A  FAILURE CONFIRMED B (cause: node-failure)
  [00.920] A  RECOVERY STARTED target B
  [00.920] A  ROUTE INVALIDATED to B (failure_cascade)
```

Note the full pipeline: degradation → suspicion → confirmation → recovery →
invalidation. **Suspicion alone never invalidates a route** — confirmation is
required, exactly as the architecture specifies.

**Step 5-7 — restore the link, and the network heals:**

```
  [00.000] >> LINK UP    A <-> B
  [00.100] A  NEIGHBOR UP      discovered B
  [00.300] A  ROUTE INSTALLED  to C via B
```

Then traffic resumes and statistics are reported.

### To see alternate-route selection instead

```sh
./hh-manet-sim --scenario alternate
```

With a diamond topology (`A–B–D` and `A–C–D`), breaking the active path shows
the route shifting from `A -> B -> D` to `A -> C -> D` with connectivity
preserved.

### To walk through it one step at a time

```sh
./hh-manet-sim --scenario recovery --step
```

---

## 10. Troubleshooting

| Symptom | Cause and fix |
|---|---|
| `cmake: unknown option -S` | CMake 3.10 predates `-S/-B`. Use `mkdir build && cd build && cmake ..` |
| `hh-manet` exits with `ENOTIMPL` | **Expected.** No FPGA radio backend exists. Use `hh-manet-sim` or `ctest` |
| `node start failed` / `node_id not set` | Pass `-n <id>`, or set `node_id` in the config file |
| `unknown radio adapter 'mock'` | The mock radio is test-only and deliberately not selectable from the production daemon |
| Segfault in a test that builds two simulators | `netsim_t` is ~6.5 MB; two on an 8 MB stack overflows. Use file-scope statics (see `tests/unit/test_simui.c`) |
| Simulator prints no events | Something reset the global log level — call `simui_events_refresh()` after starting nodes |
| A scenario behaves differently between runs | It shouldn't. Pass `--seed N` explicitly and report it; determinism is a tested property |
| Test fails only under sanitizers | Real bug. Build with `-DHH_SANITIZE=ON` and read the ASan report |
| Want to see what a node actually thinks | `hh_telemetry_dump()`, or run the simulator without `--quiet` |

**Diagnosing a routing problem:** raise the log level. Every state transition is
logged as a structured `key=value` record:

```sh
./hh-manet-sim --scenario alternate        # events shown by default
./hh-manet -c ../config/node.example.conf -n 1 -v debug
```

---

## 11. Hardware integration

### Abstracted today

Everything RF-facing, behind `hh_radio_ops_t`. The stack consumes RSSI, SNR,
PER, retransmission counts, PHY errors, ACK outcomes, and latency — all as
plain values delivered through that interface, **supplied by the test harness
today**.

### To be connected to real hardware

Implement the eight functions in `hh_radio_ops_t`. That is the whole task.

```c
/* src/adapters/hw/hw_adapter.c — every operation currently returns
   HH_ERR_NOT_IMPLEMENTED and logs why */

open              → acquire PL / RF resources
close             → release them
transmit          → hand a framed message to the modulation path
set_rx_callback   → register demodulated-frame delivery
poll              → pump received frames + measured metrics into the callback
get_status        → channel, waveform, error counters
get_link_metrics  → per-neighbour RSSI / SNR / PER / retransmits
set_channel       → retune, or return HH_ERR_UNSUPPORTED
```

### Deliberately not defined

AXI register maps · DMA descriptors · PHY/modem control · RF tuner control ·
sample streaming · modulation/RCC engine interfaces.

The HW/SW specification records the radio C ABI as TBD. Guessing at
it would produce code that must be rewritten once the real contract lands.

### What still needs a hardware decision

Metric units and encodings · beacon wire byte layout · capability and waveform
bit-fields · maximum frame size · channel-change failure behaviour · all timer
and threshold values (current config values are a working operating point, not
specification values).

Deployment packaging is designed in
[docs/DEPLOYMENT-ARCHITECTURE.md](docs/DEPLOYMENT-ARCHITECTURE.md).

---

## 12. Development workflow

### Adding a feature

1. **Read the reference docs first.** Doc 1 defines component responsibilities
   and boundaries; the HW/SW spec numbers the interfaces. Most design questions
   are already answered there.
2. **Respect the boundaries.** Neighbor Manager is the only writer of one-hop
   state. Routing is the only writer of the route table. The forwarder never
   computes a route. Topology is never in the routing path.
3. **Write the test with the code.** Assert on published events and observable
   behaviour, not internal state.
4. **Build warning-clean.** The project compiles with `-Wall -Wextra -Wshadow
   -Wpointer-arith -Wcast-qual -Wstrict-prototypes` and no warnings.
5. **Run everything**: `ctest`, then the sanitizer build, then the affected
   simulator scenario.

### Adding a simulator scenario

Add a `scenario_*` function in `tools/sim/hh_manet_sim.c`, register it in
`main()`, and add it to the usage text. Use `simui_*` helpers for output so it
matches the existing style — and so it stays derived from real state.

### The rule that matters most

**Production code must never reference the simulator.** This is verifiable:

```sh
grep -rn "netsim\|mock_radio\|simui" src/ include/     # must return nothing
nm build/libhhsdr_core.a | grep -c "netsim\|mock_radio" # must be 0
```

If either check fails, the separation that makes this architecture testable
without hardware has been broken.
