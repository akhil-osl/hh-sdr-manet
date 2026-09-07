# SCA 2.2.2 Compatibility Status

Per task §8, which requires distinguishing three things that are often conflated.

| | Status |
|---|---|
| **SCA-compatible architecture** | **Yes.** Component boundaries, classification, lifecycle sequencing, property surface, and port declarations follow SCA 2.2.2 and Doc 1 §14. |
| **SCA implementation** | **Partial.** The lifecycle, property, and port model is implemented in C and enforced by tests. The CORBA/ORB layer and the Core Framework supervisory components are not implemented. |
| **SCA conformance** | **No, and not claimed.** No conformance testing has been performed. SCA 2.2.2 mandates a minimumCORBA ORB, which is absent. |

## What is implemented

### Component classification (`src/sca/resource.c`)
Matches Doc 1 §3 exactly, and a test asserts it:

| Component | SCA class |
|---|---|
| Discovery, Neighbor, LinkHealth, FailureDetector, Topology, SelfHealing | `Resource` |
| Routing Engine | `Resource`, **assembly controller** |
| Radio/SDR Interface | `Device` (Device Manager-owned) |
| Packet Forwarder | Outside the Resource graph |
| Management/Telemetry | `Service` |

### Lifecycle (`CF::LifeCycle` / `CF::Resource` shape)
`initialize` → connect ports → `configure` → `start` → `stop` → `releaseObject`.

The ordering is **enforced, not documented**: configure before port connection
is rejected, start before configure is rejected, and `stop` preserves
configuration so a restart needs no reconfigure. `hh_node_*` applies the same
sequence to the assembled node.

### Property surface (`CF::PropertySet` shape)
23 properties with stable ids (`hh::routing::max_hop_count`, …), each carrying a
`configure` / `execparam` / `allocation` kind and bound to a real configuration
key. `configure` and `query` round-trip. An `execparam` cannot be retuned at
runtime. A test asserts every declared property binds to a key the real
configuration accepts, so the surface cannot drift into decoration.

### Ports (SCD content basis)
Each component declares its `uses`/`provides` ports, each traced to a numbered
interface (HTI-02 … HTI-16) from the HW/SW Interface Specification. A test
asserts no port exists without an interface behind it.

### `CF::TestableObject`
`runTest` implements a self-check and raises `UnknownTest` for unrecognised
ids — which Doc 1 §12 records as itself conformant.

## Gaps — what would be required for actual conformance

These are **not** implemented, because implementing them would mean inventing
contracts that do not exist yet:

1. **CORBA ORB (minimumCORBA)** — mandated by SCA 2.2.2, absent here. The
   in-process typed event bus is the transport; no ORB sits at the boundary.
   Doc 1 §14 open item 3 also notes ORB call-path latency is unmeasured against
   the fast-path invariant.
2. **IDL-generated stubs and skeletons** — the HW/SW spec §4 supplies IDL struct
   definitions; no IDL compiler output or ORB binding exists.
3. **CORBA Naming and Event Services** — not present.
4. **Domain Manager / Device Manager / Application Factory** — not implemented
   as running components. Doc 1 §14 open item 4 additionally records the
   multi-node domain topology question as unresolved: each mesh node being a
   full SCA domain is a materially different shape from SCA's typical
   single-platform assumption.
5. **Domain Profile XML (SPD/SCD/PRF/SAD/DCD/DMD)** — the *content basis* exists
   (classification, port lists, property ids with kinds); the XML documents are
   not authored. Doc 1 §14 open item 2.
6. **A hardware `CF::Device` implementation** — blocked on the FPGA/PL contract,
   the same dependency tracked in `HARDWARE-DEPENDENCIES.md`.
7. **Certification / compliance test scope and process** — Doc 1 §14 open item 5.

## Deliberate non-goals

Per task §8, SCA concepts were applied where they fit and **not** forced
elsewhere. Internal types — the event dispatcher, neighbor table, route
snapshot, link-health state machine, wire codec — are implementation details SCA
does not require to be exposed, and they are not wrapped as SCA components. The
Packet Forwarder stays outside the Resource graph because Doc 1 §14 places it
there: descriptor-driven deployment governs lifecycle and interconnection, not
sub-microsecond forwarding-path timing.
