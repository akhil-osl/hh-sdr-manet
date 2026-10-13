# HH-SDR Self-Healing MANET

A C11 implementation of a self-healing mobile ad-hoc network (MANET) control
plane, data plane, and radio abstraction — the application/control-software
and networking portion of the HH-SDR software-defined radio architecture.

> **New here?** Read [ARCHITECTURE.md](ARCHITECTURE.md) first — it maps how
> every component relates, and what is built versus still blocked.

> **Routing engine (2026-10-08):** OLSRv2, run by OLSRd2 (OONF) as an
> external Linux routing daemon. HH-SDR provides the network-interface
> adapter `hh_netif` (`src/netif/`) between the Linux interface and the
> radio abstraction. The self-healing distance-vector stack described below
> is kept as the legacy reference and simulator workload. See
> ARCHITECTURE.md, "OLSRv2: who owns routing", and `unknown.md` U-09, U-24.

---

## Overview

A radio network where every node is also a router: there is no base station
and no central coordinator. Nodes discover each other over the air, build
multi-hop routes, continuously assess link quality, and repair the network
themselves when links degrade, nodes fail, or the network splits apart.

The network:

- forms itself with no configuration beyond a node identity
- routes traffic across several hops when nodes cannot hear each other directly
- notices a failing link before it fails outright, and routes around it
- distinguishes a lost neighbor from a jammed channel from a failed radio,
  since the correct response differs
- keeps operating when the network splits, and rejoins cleanly when it heals

MANET networking and the radio/SDR layer are deliberately separated:

| Layer | Concern | Status |
|---|---|---|
| MANET networking | Neighbors, routing, link health, failure/recovery | Implemented |
| Radio / SDR | Modulation, RF, sample streams, PHY metrics | Abstracted behind one interface |

The MANET stack needs only two things from a radio: send a frame, and deliver
a received frame plus its measured RF quality. Everything below that —
waveform, modem, PHY, RF front end — sits behind that one interface, so the
networking stack is complete and testable without radio hardware.

---

## Architecture

```
    ┌──────────────────────────────────────────────────────────┐
    │  Application                                              │
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
    │  Radio / SDR Abstraction     hh_radio_ops_t               │
    ├──────────────────────────────────────────────────────────┤
    │  Hardware Adapter (implemented per target platform)      │
    ├──────────────────────────────────────────────────────────┤
    │  FPGA / PL / PHY / RF                                    │
    └──────────────────────────────────────────────────────────┘
```

**Control-plane components:**

| Component | Owns |
|---|---|
| Discovery | Beacon scheduling and neighbor discovery |
| Neighbor Manager | The one-hop neighbor table |
| Link Health | Fusing RF/MAC signals into a link state, with hysteresis |
| Routing | Route computation, next-hop selection, the published route table |
| Failure Detector | Confirming a failure after a debounce hold-down |
| Topology | An operator-facing aggregate graph (read model) |
| Self-Healing | Repair orchestration: alternate routes, rediscovery, channel change |

**Control plane vs. data plane.** The control plane decides where traffic
should go and is allowed to be slow. The data plane moves packets: for every
packet the forwarder reads the current route snapshot and looks up a next
hop. It takes no lock, performs no routing computation, and never blocks on
the control plane.

**Radio/SDR abstraction.** A single interface, `hh_radio_ops_t`
(`include/hhsdr/radio/radio.h`), covers everything the MANET stack needs from
a radio: open/close, transmit, receive, status, link metrics, and channel
control. The unit of exchange is a frame, not a sample buffer — every DSP and
PHY decision stays below that interface. Connecting real hardware means
implementing that interface for the target platform; no other part of the
stack changes.

---

## Implementation status

- **Implemented and tested:** Discovery, neighbor management, link health
  monitoring, routing, packet forwarding, failure detection, topology,
  self-healing, node lifecycle, configuration, structured logging, telemetry,
  and an SCA 2.2.2 compatibility layer (compatible architecture; not a
  conformance claim — see [docs/SCA-COMPATIBILITY.md](docs/SCA-COMPATIBILITY.md)).
- **Hardware-independent:** the entire MANET stack depends only on the radio
  abstraction, not on anything below it.
- **Hardware-dependent, not yet implemented:** the real radio adapter, RF
  metric units/encodings, beacon wire byte layout, and channel/waveform
  encodings — all intentionally left open pending a defined hardware
  contract. See [docs/HARDWARE-DEPENDENCIES.md](docs/HARDWARE-DEPENDENCIES.md).
- **Not yet implemented:** QoS queueing, security/crypto interfaces, and
  deployment packaging (designed in
  [docs/DEPLOYMENT-ARCHITECTURE.md](docs/DEPLOYMENT-ARCHITECTURE.md)). A
  development-only telemetry bridge to an external operator console GUI
  exists (`hh-manet-telemetry-serve`, plaintext, simulator-only so far —
  see [GUI integration](#gui-integration)); nothing wired to the
  production daemon yet.

---

## Repository structure

The radio-control plane is organised as self-contained components, each with
its own headers, sources, tests and `CMakeLists.txt`. Dependencies flow one
way: `radioctl -> librc -> protocol`, and `radiod -> protocol`. A client never
links the daemon.

```
radiod/                 The radio-control daemon (control plane)
  include/hhsdr/radiod/  radiod.h, config.h, events.h, mock_backend.h
  src/                   radiod.c, config.c, events.c
    backends/            mock_backend.c, hw_adapter.c (stub)
  radiod_main.c          daemon entry point
  tests/

librc/                  Radio-control client library — depends on protocol only
radioctl/               Radio-control CLI — depends on librc only
protocol/               Control-protocol codec, shared by daemon and clients

drivers/                EMPTY — manet0 net_device, radio clock/PHC (see unknown.md)
workers/                EMPTY — OpenCPI RCC workers (see unknown.md)
fpga/                   EMPTY — PL/FPGA fabric (see unknown.md)

include/hhsdr/          Public headers
  core/                 types, sequence arithmetic, clock, log, config, events, dispatcher
  radio/                radio.h — the hardware abstraction; wire format
  netif/                netif.h — network interface ↔ radio adapter (OLSRv2 path)
  manet/                legacy stack: discovery, neighbor, link_health, routing,
                        route_table, failure_detector, topology, self_healing,
                        node, telemetry
  dataplane/            forwarder.h — the legacy stack's fast path
  sca/                  resource.h — SCA 2.2.2 compatibility layer

src/                    Implementations, mirroring include/
  main.c                 hh-manet daemon entry point

tests/
  unit/                  per-component tests
  integration/           component-seam tests
  scenario/               multi-node MANET scenarios
  sim/                    test-only virtual radio and network medium

tools/
  sim/                    hh-manet-sim — an interactive simulation driver
                          hh-manet-telemetry-serve — long-running driver that
                          serves live telemetry over TCP (see GUI integration)

config/node.example.conf   every tunable, documented
docs/                       hardware, SCA, and deployment documentation
```

---

## Building

Requires a C11 compiler and CMake >= 3.10. No external dependencies.

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

---

## Running

All commands assume you are in `build/`.

**Simulation** — the fastest way to see the system operating:

```sh
./hh-manet-sim --scenario basic          # discovery and routing
./hh-manet-sim --scenario multihop --trace   # per-packet, per-hop tracing
./hh-manet-sim --scenario recovery --trace   # link failure and self-healing
./hh-manet-sim --help                    # all options
```

**Live telemetry for a GUI** — a long-running simulator that serves node/
topology telemetry over TCP as JSON, for the MA-OI operator console (see
[GUI integration](#gui-integration) below):

```sh
./hh-manet-telemetry-serve --port 5566 --nodes 4 --step-ms 100
```

**Daemon:**

```sh
./hh-manet -c ../config/node.example.conf -n 1
```

The daemon requires a real radio backend to run; the networking stack itself
is exercised through the simulator and tests.

**Tests:**

```sh
ctest                          # run everything
ctest --output-on-failure
ctest -R routing               # one suite
```

---

## Testing

| Level | Verifies |
|---|---|
| Unit | Individual component behavior in isolation |
| Integration | Components working together over the event bus |
| Scenario | End-to-end multi-node network behavior |
| Simulator | That the simulator's observability matches production behavior |

Interactive simulation (`hh-manet-sim`) is a separate tool, not a test — it
exists to let an engineer watch the system operate.

---

## Simulation

Each virtual node runs the real production control plane and data plane;
only the radio is replaced with a virtual one, so there is no second
networking implementation to keep in sync.

```c
netsim_t sim;
netsim_init(&sim, 1234);

netsim_add_node(&sim, 1);
netsim_add_node(&sim, 2);
netsim_add_node(&sim, 3);

netsim_link_up(&sim, 1, 2, -55.0f);
netsim_link_up(&sim, 2, 3, -55.0f);

netsim_start_all(&sim);
netsim_run(&sim, 4000, 10);

hh_node_send(&netsim_node(&sim, 1)->node, 3, payload, len, sim.vc.now);
```

The virtual network can control connectivity, packet loss, delay, RF
quality, PHY errors, node failure/recovery, and network partition/merge.
Timing is fully virtual and loss uses a seeded PRNG, so a run with a given
seed replays identically.

---

## GUI Integration

Live node/topology telemetry can be streamed to the MA-OI PyQt6 operator
console (a separate repository) for visualization during development and
testing. Full design and scope is in
[docs/GUI-INTEGRATION-PLAN.md](docs/GUI-INTEGRATION-PLAN.md) — this is the
short version to get it running.

**What talks to what:**

```
hh-manet-telemetry-serve  ──TCP, newline-delimited JSON──►  MA-OI (PyQt6 GUI)
(this repo, no hardware needed)                              network panel
```

Plaintext only today — see the plan's §5a-1 for the open transport-security
decision before using this beyond local development.

### 1. Build this repo

```sh
mkdir -p build && cd build
cmake ..
make -j4 hh-manet-telemetry-serve
```

### 2. Start the telemetry server

```sh
./hh-manet-telemetry-serve --port 5566 --nodes 4 --step-ms 100
```

Runs a small line-topology simulation indefinitely (real production MANET
stack, virtual radio, no hardware) and serves each node's telemetry to any
TCP client connecting on `127.0.0.1:5566`. Leave it running; `Ctrl-C` to
stop. Run `./hh-manet-telemetry-serve --help` for all options.

### 3. Point MA-OI at it

In the MA-OI repo, set the network source to live in
`config/settings.yaml` (or via the Settings dialog once exposed there):

```yaml
network:
  network_source: live_socket
  manet_telemetry_host: 127.0.0.1
  manet_telemetry_port: 5566
```

Then run MA-OI as usual (see that repo's own README/Docker setup). Its
Network Topology panel will show the live nodes/links from step 2 instead
of the built-in simulated topology — no other MA-OI configuration changes.

### 4. Confirm it's working

With the server from step 2 running, a quick manual check without the GUI:

```sh
# from this repo
nc 127.0.0.1 5566 | head -1
```

Each line is one node's status as JSON (`node_id`, `state`, neighbor/route
counts, per-neighbor `link_state`, …). If nothing arrives, confirm the
server is still running and the port matches on both sides.

---

## Hardware integration

Everything RF-facing sits behind the `hh_radio_ops_t` interface. Connecting
real hardware means implementing that interface for the target platform —
open/close, transmit, receive, status, link metrics, and channel control.
No other part of the MANET stack needs to change.

Deliberately left undefined until a hardware contract exists: register-level
APIs, DMA/streaming interfaces, PHY/modem control, and RF tuner control —
along with RF metric units/encodings, beacon wire byte layout, and
capability/waveform bit-field encodings.

Deployment packaging is designed in
[docs/DEPLOYMENT-ARCHITECTURE.md](docs/DEPLOYMENT-ARCHITECTURE.md).

---

## Documentation

- [docs/HARDWARE-DEPENDENCIES.md](docs/HARDWARE-DEPENDENCIES.md) — what depends on real hardware and what doesn't
- [docs/SCA-COMPATIBILITY.md](docs/SCA-COMPATIBILITY.md) — SCA 2.2.2 compatibility scope and gaps
- [docs/DEPLOYMENT-ARCHITECTURE.md](docs/DEPLOYMENT-ARCHITECTURE.md) — deployment packaging design
- [docs/IMPLEMENTATION-WALKTHROUGH.md](docs/IMPLEMENTATION-WALKTHROUGH.md) — a guided tour of the implementation
- [docs/GUI-INTEGRATION-PLAN.md](docs/GUI-INTEGRATION-PLAN.md) — connecting the MA-OI operator console for live visualization
