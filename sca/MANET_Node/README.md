# MANET_Node — SCA 2.2.2 component wrapper

Phase 1 of REDHAWK/OpenCPI integration: make the existing MANET C
implementation conform to the SCA 2.2.2 component model as defined in the
[SCA 2.2.2 specification](https://www.wirelessinnovation.org/assets/work_products/sca_version_2_2_2.pdf)
(Appendix C: IDL; Appendix D: Domain Profile / XML descriptors). **No REDHAWK
or OpenCPI runtime, ORB, or Core Framework component is used, installed, or
required by anything in this directory.**

## Scope of this phase (explicit)

Implemented:
- SPD (`MANET_Node.spd.xml`), SCD (`MANET_Node.scd.xml`), PRF
  (`MANET_Node.prf.xml`) — hand-authored against the SCA 2.2.2 Appendix D
  Domain Profile DTDs.
- A plain-C adapter (`adapter/manet_sca_adapter.[hc]`) implementing the
  *operation shapes* of `CF::LifeCycle`, `CF::TestableObject`,
  `CF::PropertySet`, `CF::PortSupplier`, and `CF::Resource` (SCA 2.2.2
  Appendix C), delegating every operation to the existing, unmodified
  `hh_node_t` (`include/hhsdr/manet/node.h`) and the existing SCA lifecycle
  guard (`hh_sca_resource_t`, `include/hhsdr/sca/resource.h`).
- Lifecycle ordering (`initialize → connect ports → configure → start → stop
  → releaseObject`), property `configure`/`query`, and `runTest` behavior —
  all already enforced by `src/sca/resource.c`, which this adapter reuses
  rather than reimplements.

Explicitly **not** implemented (per instruction, and consistent with
`docs/SCA-COMPATIBILITY.md`'s existing gap list):
- `DomainManager`, `DeviceManager`, `ApplicationFactory`, or any Application
  deployment/orchestration.
- Any CORBA ORB, IDL-compiled stub/skeleton, or Naming/Event Service. The
  adapter takes and returns plain C types, not `CORBA::Any` / `CF::Properties`.
- Any REDHAWK-specific API (`Resource_impl`, `PropertyEmitter`,
  `redhawk-codegen` output, BulkIO, etc.).
- OpenCPI integration or FPGA/HDL code of any kind.

## Component boundary

This SCD wraps **the assembled node** (`hh_node_t`), not the seven
individually-classified Resources in `src/sca/resource.c`'s component table
(`DiscoveryManager`, `NeighborManager`, `LinkHealthMonitor`,
`FailureDetector`, `TopologyManager`, `RoutingEngine`, `SelfHealingManager`).
Those seven modules are wired to each other exclusively through an
in-process event dispatcher (`src/core/dispatcher.c`) via direct C calls —
never CORBA, never inter-process — so today they are not separate component
boundaries. `resource.c`'s table remains the correct basis for a *future*,
finer-grained decomposition; this phase wraps the boundary the C code
actually has. See `MANET_Node.scd.xml`'s header comment for the full
reasoning, and its AMBIGUITIES note for what is explicitly left open.

## Ports

Derived only from `hh_node_t`'s real external entry points and the radio
contract it depends on (`include/hhsdr/radio/radio.h`):

| Port | Direction | Source in C code |
|---|---|---|
| `radio_frame_in` | provides | `hh_node_on_frame()` — inbound frame + link sample (HTI-04/05) |
| `radio_frame_out` | uses | `hh_radio_transmit()` via the node's owned `hh_radio_t` (HTI-03; also carries HTI-14 channel control) |
| `app_data_in` | provides | `hh_node_send()` — opaque application payload |
| `status_out` | provides | `hh_telemetry_node_status()` / `hh_telemetry_format_*()` — read-only |

No RF/IQ/sample-level port exists: the radio contract's unit of exchange is
a framed message (`hh_frame_t`), not a sample buffer — a deliberate
boundary choice already documented in `radio.h`, unchanged here.

## Properties

All 23 entries in `MANET_Node.prf.xml` are a direct transcription of
`g_props[]` in `src/sca/resource.c` — same ids, same `configure`/
`execparam`/`allocation` kinds, same descriptions. None were invented; see
the PRF file's own header comment for the exact mapping rules and two open
questions about `kind="allocation"` semantics.

## Build

`CMakeLists.txt` in this directory is a **standalone** fragment, not wired
into the repository's top-level build. It is not built, linked, or tested by
this change. See that file's header for the (documented-only, not executed)
build invocation.

## What remains before a real SCA/REDHAWK runtime can host this component

1. **IDL authoring.** No compiled IDL exists yet for `hh_frame_t`,
   `hh_link_sample_t`, `hh_node_status_t`, or a `CF::Properties` mapping for
   `hh_config_t`. The SCD's port `repid`s (`IDL:HHSDR/MANET/*`) name port
   *semantics*, not a working CORBA binding — authoring `HHSDR.idl` is
   required before a real ORB can generate stubs/skeletons for these ports.
2. **ORB / ORB binding.** `manet_sca_adapter.[hc]` is the seam a CORBA
   servant would wrap; no servant, skeleton, or ORB integration exists.
3. **Domain Profile assembly (SAD/DCD/DMD)** and any `DomainManager`/
   `DeviceManager` — out of scope by instruction, not started.
4. **REDHAWK version/toolchain decision** for when that phase begins: see
   `docs/SCA-COMPATIBILITY.md` and the version/OS findings recorded in the
   top-level integration notes for this branch (REDHAWK 2.2.x line is the
   one whose specification documentation targets SCA 2.2.2; REDHAWK 2.3.x
   targets SCA 2.3.0; REDHAWK 3.0 does not target a specific SCA version).
   REDHAWK's own installation is RPM/yum-based for RHEL/CentOS 6/7 and is
   not installed anywhere by this change.
