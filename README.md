# HH-SDR Self-Healing MANET — Control Plane & Data Plane

C11 implementation of the Application/Control Software and networking portion of
the HH-SDR architecture, per `HH-SDR-Self-Healing-MANET-Architecture.md` (Doc 1)
and `HH-SDR-Network-Topology-HWSW-Interface-Specification.md` (the HTI spec).

Language follows Doc 1 §13, which specifies C for the control plane, data plane,
and radio abstraction.

## Build and test

```sh
mkdir -p build && cd build
cmake .. && make -j4
ctest --output-on-failure
```

Sanitizer build: `cmake .. -DHH_SANITIZE=ON`.

Requires only a C11 compiler and CMake ≥ 3.10. No external dependencies.

## Running

```sh
./build/hh-manet -c config/node.example.conf -n 1
```

This **will fail to start**, and that is correct: the FPGA/PL software API does
not exist yet, so the hardware radio adapter reports `HH_ERR_NOT_IMPLEMENTED`
rather than pretending to work. The MANET stack itself is fully exercised by
`ctest`, which drives this same production code through a test radio adapter.

## Layout

```
include/hhsdr/       Public headers
  core/              Types, sequence arithmetic, clock, log, config, events, dispatcher
  radio/             Radio abstraction (the one hardware seam), wire format, HW stub
  manet/             Control plane: discovery, neighbor, link health, routing,
                     failure detection, topology, self-healing, node, telemetry
  dataplane/         Packet forwarder
  sca/               SCA 2.2.2 compatibility layer
src/                 Implementations, mirroring include/
  adapters/hw/       Hardware adapter — PLACEHOLDER, returns NOT_IMPLEMENTED
tests/
  unit/              Per-component tests
  integration/       Component-seam tests
  scenario/          14 multi-node MANET scenarios
  sim/               TEST-ONLY: mock radio, virtual clock, network simulator
config/              Example configuration
docs/                Hardware dependency and SCA compatibility tracking
```

## Architecture

```
Application  ──►  hh_node_send()
Control      ──►  Discovery │ Neighbor │ LinkHealth │ Routing
                  FailureDetector │ Topology │ SelfHealing
                  (wired through a typed pub/sub bus, never nested calls)
Data plane   ──►  Packet Forwarder — reads the published route snapshot only
Radio        ──►  hh_radio_ops_t  ◄── THE hardware seam
Adapter      ──►  hw (TBD stub)   │   mock (tests only)
```

Key invariants, each enforced by test rather than convention:

- **Neighbor Manager is the sole writer** of one-hop truth; everyone else reads
  snapshots or `NeighborUp/Down/Changed` events.
- **Forwarding never routes.** It reads one atomically-swapped snapshot pointer,
  takes no lock, and performs a flat next-hop lookup — never a graph walk.
- **Topology is a read model**, never in the routing decision path.
- **Degradation alone never invalidates a route.** Confirmation is required.
- **No single signal** can move a link past `Degraded`.

## Hardware status

The FPGA/PL API is unavailable. Everything hardware-facing sits behind one
vtable, `hh_radio_ops_t` — 8 functions. No AXI register map, DMA API, PHY/modem
control, RF tuner API, sample-streaming interface, or modulation/RCC engine
interface is invented anywhere in this repository.

Future FPGA integration should require implementing that adapter, not rewriting
the MANET stack. See `docs/HARDWARE-DEPENDENCIES.md`.

## SCA 2.2.2

SCA-compatible **architecture**: yes. SCA **implementation**: partial (lifecycle,
properties, ports, classification). SCA **conformance**: not claimed — no ORB, no
Domain Profile XML, no conformance testing. See `docs/SCA-COMPATIBILITY.md`.
