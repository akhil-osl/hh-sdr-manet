# UNKNOWNS — Blocked Contracts and Open Decisions

Target architecture: **HH-SDR PS/PL Software Architecture — Three-Plane Bridge**
(Doc No: OSL-628-BD-101 Rev: 00, dated 08-Sep-2026, Ref IWO:628 HH-SDR).

This file is the authoritative register of everything the target architecture
names but does **not** specify closely enough to implement. The project rule is
absolute:

> **Nothing in this file is guessed at, stubbed with invented values, or
> approximated in code.** Where a contract is missing, the code either does not
> exist yet, or it fails loudly and honestly — following the precedent already
> set by `radiod/src/backends/hw_adapter.c`, which returns
> `HH_ERR_NOT_IMPLEMENTED` from every hardware operation rather than pretending
> to work.

Each entry records: what is missing, why radiod needs it, where it must come
from, what it blocks, and what can safely proceed without it.

Engineer IDs referenced below are from the drawing's Note 4:
**E5** = Linux BSP (drivers, radiod) · **E6** = Network/Application (OLSRv2,
librc users) · **E1–E4** = HDL/OpenCPI. See ADD §1.4.

---

## Status legend

| Status | Meaning |
|---|---|
| **BLOCKING** | Work cannot start until this is answered |
| **DEFERRED** | Deliberately out of scope for the current round |
| **PARTIAL** | Some of the contract is known; the rest is not |

---

## U-01 — ICD-2 TLV wire format

**Status:** BLOCKING (for TLV work only)

**What is missing:** The entire ICD-2 specification. The drawing labels the
librc↔radiod link "ICD-2 TLV / UNIX socket" and describes librc/radioctl as
"versioned TLV over UNIX socket", but no byte-level contract exists anywhere.
Specifically undefined:

- TLV header layout (type/length field widths and order)
- Version field, and version-negotiation behaviour on connect
- Numeric command IDs and response IDs (today's protocol sends ASCII verb names)
- Transaction/request IDs for correlating replies (today: strictly one-in-flight)
- Payload length encoding and maximum message size
- Status/error code registry (today: `hh_status_t` as a signed decimal)
- Byte order and alignment/padding rules
- Asynchronous event framing (how radiod pushes events to a client)
- Timeout behaviour on both sides
- Connection lifecycle: handshake, graceful close, keepalive
- Backward/forward compatibility rules for unknown types

**Why radiod needs it:** It is the sole control contract between radiod and
every client (OLSRv2 via librc, radioctl, applications, test automation). Its
exact bytes determine the codec, the client library, and the CLI.

**Where it must come from:** System architect / the ICD document set implied by
the drawing's document number. Confirmed by the user on 2026-09-21: **no ICD-2
specification exists yet.**

**What it blocks:** A TLV codec, protocol versioning, version negotiation,
asynchronous event delivery over the socket, and any client built to survive the
ASCII→TLV transition.

**What can proceed:** Everything else. The existing line-oriented ASCII
`key=value` protocol (`protocol/src/rc.c`) is working, tested, and carried
forward unchanged. It is deliberately housed in its own `protocol/` component so
an ICD-2 codec can be added alongside it without touching radiod's state machine
or the client library.

**Explicitly NOT done:** No TLV type IDs, command IDs, or wire structures have
been invented.

---

## U-02 — `rc_*` public API signatures

**Status:** PARTIAL

**What is missing:** The drawing shows applications calling an "rc_* API" into
librc. The function names, signatures, return conventions, opaque handle types,
and threading guarantees of that API are unspecified.

**Why radiod needs it:** librc's public header is the API/ABI boundary that
external applications and OLSRv2 compile against. Freezing it prematurely means
breaking callers later.

**Where it must come from:** System architect, with E6 (Network/Application) as
the primary consumer.

**What it blocks:** Publishing a stable `rc_*` public header.

**What can proceed:** librc exists and works today under its existing
`hh_rc_client_*` API (`hh_rc_client_connect` / `hh_rc_client_call` /
`hh_rc_client_close`). That API is real and tested. The `rc_*` names from the
drawing have **not** been invented; if the eventual API differs, an `rc_*`
facade can be layered over the existing client without changing radiod.

---

## U-03 — OpenCPI application XML and worker identities

**Status:** BLOCKING

**What is missing:**

- The OpenCPI application XML name and filesystem path
- Worker instance names as deployed. The drawing names five PS RCC workers —
  `waveform_ctrl`, `drc`, `mac_ps`, `ad9361_config_proxy`, `telemetry` — but
  these are box labels, not verified instance names.
- Each worker's property names, types, units, ranges, and defaults
- Which properties are readable, writable, or volatile
- The PL container/assembly name and bitstream identity

**Why radiod needs it:** Note 1 on the drawing states *"only radiod opens
OpenCPI. All other processes use librc (ICD-2). Never a second ACI instance."*
radiod cannot open, configure, or control an application it cannot name.

**Where it must come from:** E1–E4 (HDL/OpenCPI). The OpenCPI project skeleton
at `/home/ospl/projects/manet-opencpi/manet/` is currently **empty** — its
`project-metadata.xml` declares `local.manet.manet` with `<workers/>`,
`<tests/>` and `<specs/>` all empty, and no HDL, specs, or assemblies exist.

**What it blocks:** Every part of OpenCPI ownership: application create/load,
worker discovery, property configuration, start/stop.

**What can proceed:** radiod's structure, lifecycle state machine, socket
server, dispatch, and mock backend — all of which are backend-agnostic by
design (radiod depends only on the `hh_radio_ops_t` vtable, never on a concrete
backend).

**Explicitly NOT done:** No OpenCPI API calls, worker names, or property names
appear anywhere in the source.

---

## U-04 — OpenCPI ACI lifecycle and error contract

**Status:** BLOCKING

**What is missing:** The required call sequence and its failure semantics —
initialize, create/load, configure, start, stop, shutdown — plus which ACI
calls throw, what exception types surface, how partial-start failure is
recovered, and whether reload is permitted without a process restart.

**Why radiod needs it:** The drawing assigns radiod the full OpenCPI
application lifecycle. radiod's existing 7-state machine
(`created → initialized → configured → running ⇄ stopped`, plus `faulted` and
`released`) must be mapped onto it, and it is not yet known whether the two
sequences align 1:1.

**Where it must come from:** E1–E4 (HDL/OpenCPI), plus OpenCPI framework
documentation for the version in use.

**What it blocks:** radiod's OpenCPI backend, and fault handling for PL-level
failures.

**What can proceed:** The lifecycle state machine itself already exists and is
tested (`radiod/tests/test_radiod_state_machine.c`). Mapping is a later step.

**Related:** see U-11 (the ACI is C++; this project is C11).

---

## U-05 — Event and fault taxonomy

**Status:** PARTIAL

**What is missing:**

- The authoritative list of hardware/PL fault conditions
- Severity levels and their meanings
- Which faults latch and which are transient
- Who may clear a latched fault, and under what conditions
- Required persistence/history depth
- Which events must be **pushed** asynchronously versus polled
- Event ordering and delivery guarantees, and behaviour on client backlog

**Why radiod needs it:** The drawing assigns *"Events, fault registry"* to
radiod as a core responsibility.

**Where it must come from:** System architect (semantics) plus E1–E4 (the
hardware fault list).

**What it blocks:** The real fault taxonomy, severity handling, and pushed
events over the socket (the last also requires U-01 for async framing).

**What can proceed:** The registry **mechanism** — a bounded fault table with
latch/clear, first/last timestamps and counters — built over the five fault
kinds that already exist and are already tested:
`none`, `tx_failure`, `rx_silence`, `hw_fault`, `backend_io`
(`protocol/include/hhsdr/protocol/rc.h`). The enum is designed to extend without
breaking the existing wire format.

---

## U-06 — ICD-1 PDU-over-DMA descriptor layout

**Status:** BLOCKING (data plane)

**What is missing:** The drawing's Note 2 states the data-plane contract at a
high level — *"one PDU = one descriptor = one skb; TLAST marks PDU boundary;
length carried in descriptor, not in payload"* — but the implementable detail is
absent: descriptor field layout and widths, ring sizes and alignment, AXI-HP
addressing, per-class queue mapping, the DSCP→bearer-class mapping, zero-copy
buffer ownership rules, and completion/interrupt semantics.

**Why radiod needs it:** Indirectly but importantly. The data plane must stay
separate from radiod's control socket, and resolving that separation requires
knowing where data-plane packet movement actually goes.

**Where it must come from:** E5 (Linux BSP) and E1–E4 (HDL), jointly.

**What it blocks:** The `manet0` net_device driver, and the split of
`hh_radio_ops_t` into control and data halves (see U-15).

**What can proceed:** Control-plane work is unaffected. radiod already excludes
data-plane operations from its IPC by explicit design.

---

## U-07 — ICD-3 time register map

**Status:** BLOCKING (time plane)

**What is missing:** The AXI-Lite time register map — register offsets and
widths for frame/slot/hop/ns counters, the seqlock read protocol exposed to
users, PHC (`/dev/ptpN`) device naming and capabilities, 1PPS disciplining
behaviour, and holdover semantics on GNSS loss.

**Why radiod needs it:** The time plane must stay separate from ordinary
control messages. Test automation reads time via `clock_gettime(PHC)`, not
through radiod — but the boundary must be confirmed rather than assumed.

**Where it must come from:** E5 (Linux BSP) and E1–E4 (HDL), jointly.

**What it blocks:** The radio clock driver, PHC integration, and any
time-related radiod reporting.

**What can proceed:** All control-plane work. **The entire time plane is absent
from this repository** — a sweep for `PHC`, `PTP`, `1PPS`, `PPS`, `GNSS`,
`seqlock`, `TDMA`, `fb_controller` and `frame_timer` returns zero hits.

---

## U-08 — MANET STROBE semantics and RF state authority

**Status:** BLOCKING (PL integration)

**What is missing:** The drawing's Note 3 is emphatic — *"MANET STROBE from
mac_pl is the sole RF state authority; rf_ctrl_fsm may only add safety
(interlock), never contradict it."* The signal's actual semantics are
undefined: timing, polarity, width, the handshake with `rf_ctrl_fsm`, and how
the documented FSM states (`RF_OFF`, `RX_PREP`, `RX`, `TX_PREP`, `TX`,
`TURNAROUND`, `FAULT`, `SAFE`) are entered and left.

**Why radiod needs it:** To know what it must **not** do. If mac_pl is the sole
RF state authority, radiod must never drive T/R switching directly, and the
boundary needs to be explicit before any PL control is written.

**Where it must come from:** E1–E4 (HDL).

**What it blocks:** `rf_ctrl_fsm` integration and any radiod-side RF state
reporting.

**What can proceed:** Control-plane work. Nothing in the repository currently
touches RF state.

---

## U-09 — OLSRv2 versus the existing MANET stack

**Status:** DEFERRED (by decision, 2026-09-21)

**What is missing:** A decision on the fate of the existing routing stack.

The target names an **OLSRv2 daemon (RFC 7181)** that obtains *"Neighbour stats
from librc"*. The repository contains a custom, working, well-tested
distance-vector implementation (~2.6k LOC across `src/manet/`) with link-health
fusion, self-healing, topology management and partition/merge detection.

**These are incompatible data-flow models, and this is the substantive issue:**

- Target: OLSRv2 receives neighbour data **through radiod**, via librc/ICD-2.
- Current: `src/manet/` obtains neighbour data by transmitting and receiving its
  own beacons through its own `hh_radio_t` handle
  (`src/manet/discovery.c:101`, `src/manet/node.c:204`).

**Why radiod needs it:** It determines whether radiod must expose neighbour
statistics over ICD-2, and therefore part of radiod's command surface.

**Where it must come from:** System architect, with E6.

**What it blocks:** Routing work and the neighbour-stats command surface.

**What can proceed:** The radiod refactor. `src/manet/` is untouched this round
and continues to work exactly as before. This incompatibility will not dissolve
under refactoring and needs an explicit decision before the two can coexist.

---

## U-10 — Socket path and access control

**Status:** PARTIAL

**What is missing:** The production socket path and its access policy.

**Current state:** `/tmp/hh-radiod.sock`
(`protocol/include/hhsdr/protocol/rc.h`), created with **no explicit mode, no
umask handling, and no peer-credential check**. `/tmp` is world-writable, so any
local user can connect to the control plane or squat the path.

**Why radiod needs it:** radiod is the single PL owner. Unrestricted access to
its control socket is unrestricted access to the radio.

**Where it must come from:** System architect / integrator. A conventional
choice would be `/run/radiod.sock` owned by a dedicated group, but the correct
answer depends on the PetaLinux image's user model, which is not in this
repository.

**What it blocks:** Production deployment hardening.

**What can proceed:** Development and bench use at the current path, which
remains overridable via radiod's `-s` flag.

---

## U-11 — Implementation language for the OpenCPI boundary

**Status:** BLOCKING (OpenCPI only)

**What is missing:** A decision on how C11 code reaches a C++ API.

This project is strict C11 (`project(hh_sdr_manet C)` in the root
`CMakeLists.txt`, compiled with `-Wstrict-prototypes`). The OpenCPI ACI is a
**C++** API. Undecided: whether to enable `CXX` and isolate a C++ translation
unit behind a C shim, whether a supported C binding exists for the OpenCPI
version in use, and what the exception→`hh_status_t` mapping should be.

**Why radiod needs it:** It determines radiod's build configuration and the
internal structure of its OpenCPI backend.

**Where it must come from:** E1–E4 (OpenCPI) plus the project's build owner.

**What it blocks:** radiod's OpenCPI backend implementation.

**What can proceed:** Everything else. The project remains pure C11 today.

---

## U-12 — Threading model for PL ownership

**Status:** PARTIAL

**What is missing:** Whether radiod may remain single-threaded once it owns a
live OpenCPI application.

**Current state:** radiod is **deliberately single-threaded**, documented in
`radiod/include/hhsdr/radiod/radiod.h`: *"Single-threaded, poll-driven… No
locking, no threads."* A repository-wide sweep confirms zero `pthread` and zero
`mutex` usage.

**Why it matters:** Owning an OpenCPI application while simultaneously serving
clients, delivering asynchronous events, and polling faults may require
concurrency — or may not, if the ACI is poll-friendly. Adding threads would
introduce the project's first synchronisation requirements, so the decision
should be made deliberately rather than by accident.

**Where it must come from:** System architect, informed by ACI behaviour (U-04).

**What it blocks:** radiod's concurrency design, should threads prove necessary.

**What can proceed:** All current work. The single-threaded model is adequate
for the mock control plane and is not being changed speculatively.

---

## U-13 — Audio path ownership

**Status:** PARTIAL

**What is missing:** Confirmation that the audio path is outside radiod's
control scope.

The drawing shows `audio_pl` (I2S slave, codec master; AEC via NLMS; NS via
Wiener) delivering *"clean PCM 8 kHz (AXI-Stream)"* **directly to Applications**
(Voice / MELPe on PS) — bypassing radiod, the IP stack and `manet0` entirely.
That is a fourth path alongside the three named planes.

Undefined: whether radiod configures or monitors `audio_pl` at all, whether it
reports audio faults, and how the PCM stream reaches userspace (ALSA? a
character device? something else?).

**Why radiod needs it:** To establish whether audio is in or out of its
responsibility. Note 1's single-owner rule means that if `audio_pl` is part of
the OpenCPI application, only radiod may configure it — even though the PCM
data path does not traverse radiod.

**Where it must come from:** System architect plus E1–E4.

**What it blocks:** Audio configuration and fault reporting, if in scope.

**What can proceed:** Everything else. No audio code exists in this repository.

---

## U-14 — Frequency-hopping control ownership

**Status:** BLOCKING (PL integration)

**What is missing:** Whether and how radiod participates in frequency hopping.

The drawing shows `fh_controller` performing **1000 hop/s LO retune** with
`HOP_TRIG` / `ACK` / dwell, receiving a hop tick from the PL time base and a
"hopset id" from the control plane. Undefined: the hopset-id property name and
encoding, who owns hopset selection, whether radiod sets it via a worker
property, and the failure semantics of a missed hop.

**Why radiod needs it:** At 1000 hop/s, hopping is far too fast for
request/response IPC — so it must be PL-autonomous, with radiod configuring only
the hopset. The exact division of labour must be confirmed, not assumed.

**Note:** this interacts with the existing `set_channel` command. radiod today
exposes `set_channel` with an **opaque** integer channel index; the mapping from
that index to RF frequency, and its relationship to a hopset, is undefined
(`include/hhsdr/radio/radio.h` records channel as *"logical channel index;
mapping to RF TBD"*).

**Where it must come from:** E1–E4 (HDL) plus the system architect.

**What it blocks:** Hopset control and the meaning of `set_channel` against real
hardware.

**What can proceed:** `set_channel` continues to work as an opaque pass-through,
which is correct and non-committal until the mapping exists.

---

## U-15 — Control/data split of `hh_radio_ops_t`

**Status:** BLOCKING (arises with real hardware)

**What is missing:** A decision on how the data plane reaches hardware once
radiod is the sole PL owner.

`hh_radio_ops_t` (`include/hhsdr/radio/radio.h`) is a **single vtable spanning
two planes**:

- Control: `get_status`, `set_channel`
- Data: `transmit`, `set_rx_callback`, `poll`
- Lifecycle: `open`, `close`
- Metrics: `get_link_metrics`

Today this is harmless because `hh-manet` and `radiod` are separate processes
that each construct their own backend. Under the target, only radiod may open
the PL — yet `src/dataplane/forwarder.c:53`, `src/manet/discovery.c:101` and
`src/manet/node.c:349` all call `hh_radio_transmit` on a locally-constructed
backend, and `src/manet/self_healing.c:69` calls `hh_radio_set_channel`
directly.

Candidate resolutions (none chosen; **this is a decision, not a recommendation**):

1. Split into `hh_radio_ctrl_ops_t` (radiod-owned) and `hh_radio_data_ops_t`
   (a separate DMA/shm path per ICD-1).
2. radiod performs PL setup only; the data plane attaches to already-configured
   DMA rings out of band.
3. `manet0` becomes a kernel net_device and the userspace data path disappears
   entirely — the closest match to the drawing.

**Why radiod needs it:** It constrains `radio.h`, on which every production
component depends.

**Where it must come from:** System architect, informed by ICD-1 (U-06).

**What it blocks:** Severing `hh-manet` from PL-facing code (the "Phase 6b" step
that enforces Note 1 in full).

**What can proceed:** The structural refactor. `hw_adapter` is being relocated
into `radiod/src/backends/` **without** severing `hh-manet`'s link to it, so
behaviour is unchanged. The dual-owner condition is currently latent and
harmless — `hw_adapter` is a stub whose every hardware operation returns
`HH_ERR_NOT_IMPLEMENTED` — but it becomes a genuine conflict the moment a real
backend exists, and must be resolved before then.

---

## Summary

| ID | Topic | Status | Primary source |
|---|---|---|---|
| U-01 | ICD-2 TLV wire format | BLOCKING | System architect |
| U-02 | `rc_*` API signatures | PARTIAL | System architect / E6 |
| U-03 | OpenCPI app XML + workers | BLOCKING | E1–E4 |
| U-04 | ACI lifecycle + errors | BLOCKING | E1–E4 |
| U-05 | Event/fault taxonomy | PARTIAL | Architect + E1–E4 |
| U-06 | ICD-1 PDU descriptor layout | BLOCKING | E5 + E1–E4 |
| U-07 | ICD-3 time register map | BLOCKING | E5 + E1–E4 |
| U-08 | MANET STROBE semantics | BLOCKING | E1–E4 |
| U-09 | OLSRv2 vs existing stack | DEFERRED | Architect + E6 |
| U-10 | Socket path + permissions | PARTIAL | Architect / integrator |
| U-11 | C vs C++ at the ACI | BLOCKING | E1–E4 + build owner |
| U-12 | Threading model | PARTIAL | Architect |
| U-13 | Audio path ownership | PARTIAL | Architect + E1–E4 |
| U-14 | Frequency-hopping control | BLOCKING | E1–E4 + architect |
| U-15 | Control/data vtable split | BLOCKING | Architect |

**Unblocked and proceeding:** the structural refactor — `protocol/`, `librc/`,
`radioctl/`, `radiod/` — carrying today's working ASCII control protocol
forward unchanged, with no invented contracts.
