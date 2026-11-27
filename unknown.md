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
| **DECIDED** | The decision is made; the entry keeps its history and any follow-on gaps it points to |
| **OPEN** | A question is raised; code runs on stated assumptions that are not yet agreed |

---

## U-01 — Data terminal call setup and priority classes

**Status:** OPEN (PROPOSED values in code)

**Zoho task:** not yet given

**What is missing:** The data terminal now sends voice call setup and class
priority messages with values nobody has specified:

- Call setup payload: CoAP POST to `/call`, Content-Format 42, four bytes: call
  id (16 bits, big endian, never 0), type (1 request, 2 accept, 3 reject, 4
  release), reason (0 none, 1 busy, 2 declined, 3 timeout, 4 normal).
- Call timeouts: 10 s waiting for accept or reject, 30 s ringing. An active
  call has no timeout, because no keepalive or media contract exists.
- Both sides calling each other at once end in a busy reject on both sides.
- Send order voice, signalling, video, data; voice limited to a burst of 8
  then one send per 20 ms; data sent at least once in every 8 sends while it
  waits.
- Call setup is marked CS5, as RFC 4594 names for signalling. Interface spec
  7.1 has no CS5 row, so the MAC treats it as Data and call setup can wait
  behind other data on the air.
- CoAP keeps NSTART at 1, so each class session has one message outstanding
  and a second voice message waits for the first one's ACK. Raising NSTART for
  the voice and signalling sessions would change congestion behaviour.

**Why it is needed:** The payload layout is a wire contract between terminals.
The numbers decide how voice and data share the link. The CS5 mapping decides
whether call setup is delayed behind data.

**Where it must come from:** Network owner (payload, timeouts, glare rule, CS5
mapping, NSTART) and QA (traffic shares).

**Required resolution:**

- Approve or replace the payload layout and the two timeouts.
- Add a CS5 row, or choose another DSCP, in interface spec 7.1.
- Fix the voice cap, the data share and the NSTART value.
- Define an active-call keepalive.

**What can proceed:** the call state machine, the send queue and the tests,
with every value above a named constant or marked `TODO`.

---

## U-02 — Slot PDU hand-off between MAC software and the logic

**Status:** BLOCKING (for running the slot scheduler on hardware)

**Zoho task:** not yet given

**What is missing:** The binding of the slot scheduler's transmit and receive
seam to the logic. On the host, `src/mac/slot.c` hands each PDU v2 to a
transmit function and takes received PDUs through `hh_slot_receive()`. Not
defined for the board:

1. How a PDU is named for its slot in the logic's transmit buffer, and the
   lead time before the slot (`docs/SLOT-ENGINE-SPEC.md` OI-12, OI-14).
2. Receive buffer depth and how long software may take to read it (OI-17).
3. The slot capacity in bytes per PDU: the host code uses a placeholder (128 or
   256 bytes) because the burst rate and coding are not chosen (OI-7).
4. How the logic reports late or missed hand-over to software.

`hh_radio_ops_t` carries whole frames, not PDUs, so it is not this seam.

**Why it is needed:** Without it the scheduler cannot be tied to the FPGA slot
engine, and throughput figures from the host simulation cannot be turned into
real figures.

**Where it must come from:** Logic owner and network owner.

**What can proceed:** the host simulation (`tests/sim/slot_net`, `hh-slot-sim`,
`hh-slot-bridge`) with the capacity as a printed parameter.
## U-03 — Network plan fields: address widths, Net ID, command radio rule

**Status:** OPEN (limits in one table in `tools/hosttest/hosttest/netplan.py`)

**Zoho task:** not yet given

**What is missing:** The network plan page checks values that the sources do
not agree on or do not define:

- Address widths: the task update of 19 Oct 2026 (MAC frame spec v2) gives node
  addresses 0x0001 to 0xEFFF and group addresses 0xF000 to 0xFFFD (16 bit).
  `docs/MAC-NETWORK-INTERFACE.md` and `docs/MAC-SERVICE.md` show 32-bit node ids
  with groups at 0xF0000000 to 0xFFFFFFFE. The page follows the 16-bit values.
- Net ID: no width or range is defined. The page checks only a non-negative
  integer, one per net per RF head. The MAC docs describe a net identity as the
  founding superframe plus the founder's node id, which has a different shape.
- Command radio rule (break-in rule 13): the rule says command radios take the
  lowest addresses but gives no count or boundary. The page checks only that
  every command radio address is below every member address.
- EUI-64: checked for 8 bytes only. How it feeds the security nonce (frame spec
  4.5 and 4.9) is not in the repository.
- Consumers: dataterm accepts only a node id and peers on its command line. Net
  ID, group table and EUI-64 have no input anywhere, so the page saves them in
  the plan file only.
- Dataterm flags: the optional per-node IP in the plan exists only to build the
  `-P` peer flags and is not part of any specified plan.

**Why it is needed:** An operator tool that accepts or rejects addresses must
use the real limits, or it either blocks valid plans or passes invalid ones.

**Where it must come from:** Network owner (address widths, rule 13 boundary,
Net ID) and security owner (EUI-64 and nonce use).

**Required resolution:**

- Confirm the address widths and update whichever source is stale.
- Define the Net ID width and its relation to the beacon net identity.
- State the rule 13 boundary.
- Define how a plan reaches radiod, MAC and dataterm.

**What can proceed:** the plan page and its checks, with every limit in the
`LIMITS` table. The status page waits for a radiod status command and format.

---

## U-04 — Bench instrument control contract for test sequences

**Status:** OPEN

**Zoho task:** not yet given

**Scope:** the sequence runner in `tools/hosttest/`. A sequence step can today
only command the test board over the radio link. Steps that read a bench
instrument (signal generator, analyzer, power meter) are not implemented.

**What is missing:**

- Which instruments are allocated, with model and address
- The control interface (for example SCPI over TCP or VISA) and the commands
- The calibration and uncertainty budget that gives each limit, its source
  clause and its uncertainty

**Current implementation assumptions** (placeholders, not requirements):

- `config/sequence.example.json` carries a placeholder limit, source clause and
  uncertainty. They are not from any budget.
- No instrument step type exists. A sequence file cannot name an instrument.

**Why it is needed:** Without it a sequence cannot measure RF quantities, and a
logged pass cannot be traced to a calibrated limit.

**Where it must come from:** The test and hardware engineers, and the owner of
the calibration and uncertainty budget.

**Required resolution:**

- Name the instruments and their control interface, then add an instrument step
  type that returns `HH_ERR_UNSUPPORTED`-style failures for a missing driver.
- Replace the example limits with values from the budget.

**What can proceed:** sequence loading, limit checking, stop or continue on
failure and the log with unit serial number, all tested against the fake board.

## U-05 — MAC demand report in the control block

**Status:** OPEN

**Zoho task:** AC2O-T2589

**Scope:** the queue report of the PDU v2 control block (`include/hhsdr/mac/pdu.h`),
which every node uses to tell the others how much it has queued.

**What is missing:**

- Where a MAC format version sits in the frame (slot engine specification OI-19).
- The report as built, written into the frame specification so logic and software
  agree on the field.

**Current implementation:**

- Every own-slot PDU carries the report: 5 bytes of the 128-byte slot, one byte per
  class 0 to 4 (flag 0x20).
- Each byte is the offered load of the class in messages per frame: the larger of
  the messages sent in the last frame and the messages sent so far in this frame
  plus those still queued, capped at 255.
- There is no voice call count or age. One voice call of several messages a frame
  cannot be told from several calls, so two nodes asking for voice in the same
  frame can both be admitted.

**Why it is needed:** Without a fixed report every node cannot compute the same
slot map.

**Where it must come from:** The frame specification owner.

**Required resolution:** Fix the format version position and write the report
field into the frame specification.

**What can proceed:** the allocator, its tests and the simulation.

## U-06 — Dynamic slot allocation policy

**Status:** OPEN

**Zoho task:** AC2O-T2589

**Scope:** `src/mac/alloc.c` and the slot scheduler. Every node computes the same
slot map from the same reports, with no master.

**What is missing:**

- Slots per frame and the number reserved for voice, which depend on the MAC
  frame and slot structure.
- Measured values for the voice reserve, class weights, per-node cap, floor, idle
  slots for joining radios, staleness and gone limits, and messages per slot.
  They stay placeholders until measured on the RF link.
- The report cutoff.
- Smoothing of demand and a rule keeping slot positions stable between maps, after
  the RF-link measurements and the slot hand-off (U-02).

**Current implementation:**

- Voice reserve 4 slots, video to data weight 2 to 1, cap 8 of 20 slots, one idle
  slot for a joiner, a report stale after 5 frames and a node gone after 250
  frames, one message per slot per frame. All are marked TODO in the code and are
  fixed in the build, so every node runs the same values.
- Three allocation classes (voice, video, data). Control and emergency stay
  outside the sharing.
- The map is computed at each superframe boundary and applied one superframe
  later. A lower node id keeps a slot that two nodes claim. Entry grants stay,
  and the slots of a grant not yet confirmed are kept out of the map.
- The DSCP table maps EF to voice, CS3, AF31 to AF33, CS4 and AF41 to AF43 to
  video, and the rest, call signalling (CS5) included, to data.
- Voice admission: `hh_slot_voice_admit` is a read-only query on the demand table.
  The caller and the callee each check their own view, when a request is placed
  and when one arrives, and refuse with a busy reply. There is no tie-break and
  no second check at accept.

**Why it is needed:** The values decide how the frame is shared. They are
tuning, but the same values must be at every node.

**Where it must come from:** The network owner and the logic owner.

**Required resolution:** Measure and fix the values on the RF link, and state the
report cutoff.

**What can proceed:** the host simulation and its tests with the placeholder
values. Hardware timing and the hand-off to the logic wait on the MAC frame and slot
structure, the burst-mode air interface and U-02.

---

## U-07 — Network entry: founder, re-entry after isolation

**Status:** OPEN

**Zoho task:** AC2O-T1846

**Scope:** `src/mac/entry.c`, cold start and re-entry on the radio link.

**What is missing:**

- The founder: which slot it takes while slots follow demand (U-06), the beacon it
  sends (the entry slot, window and time quality), its MAC state, and the merge of
  two nets that founded together. Until the founder sends something, every silent
  radio founds on its own: the host simulation shows three of three radios founding
  in a cold start.
- Net time without GNSS.
- Re-entry after an isolation longer than `t_age`: the radio is not told that its
  slots were given away, so both radios send in the same slot and destroy each
  other's PDU. It needs the "slots heard in use" field of the MAC beacon.
- Whether age-out needs agreement from all neighbours, and the tie-break when two
  radios claim one slot and both are heard (the code lets the lower node id keep it;
  no document states this for slots).
- The entry values `n_found` (4), the founding jitter (0 to 2), `t_age` (5) and
  `t_quarantine` (3) are examples with no source.

**Current implementation:** a radio that hears no member for `n_found` superframes
plus its jitter reports a founding event. A slot owner not heard for `t_age`
superframes is aged out and its slots stay unused for `t_quarantine` more. A radio
that resets joins again from the start and never reclaims its old slot. A member gives
up an own slot that a member grants to another radio, and joins again if none is left.
Tested on the host simulation only.

**What can proceed:** the host simulation and its tests. The radio-link proof waits on
a second board.

---

## Summary

| ID | Topic | Status | Primary source | Zoho task |
|---|---|---|---|---|
| U-01 | Data terminal call setup and priority classes | OPEN | Network owner + QA | not yet given |
| U-02 | Slot PDU hand-off between MAC software and the logic | BLOCKING | Logic + network owners | not yet given |
| U-03 | Network plan fields: address widths, Net ID, command radio rule | OPEN | Network owner + security owner | not yet given |
| U-04 | Bench instrument control contract for test sequences | OPEN | Test and hardware engineers + budget owner | not yet given |
| U-05 | MAC demand report in the control block | OPEN | Frame specification owner | AC2O-T2589 |
| U-06 | Dynamic slot allocation policy | OPEN | Network owner + logic owner | AC2O-T2589 |
| U-07 | Network entry: founder, re-entry after isolation | OPEN | MAC owner + network owner | AC2O-T1846 |

**Unblocked and proceeding:** the structural refactor — `protocol/`, `librc/`,
`radioctl/`, `radiod/` — carrying today's working ASCII control protocol
forward unchanged, with no invented contracts.

**Added 2026-09-28:** radiod's OpenCPI backend (`backend = ocpi`), which owns
one OpenCPI application through the ACI. It takes the application and every
property from configuration and names nothing itself, and is tested on the
development host against real OpenCPI 2.4.7 applications. Board operation
has not been run on a board.

**Added 2026-11-06 (simulation fixture, not a project contract):** `tools/sim/hh_slot_bridge.c`
runs real OLSRd2 over the slot-timed host simulation with these choices, made
only to exercise the stack: TAP framing; link address `02-00-00-00-hh-ll` as in
MAC frame specification v2 section 4.5, so the destination address names the next
hop; IPv4 `10.99.0.<id>/24`; static neighbour entries instead of ARP; TAP MTU
equal to the largest packet one PDU carries (fragmentation excluded); OLSRv2
packets (UDP port 269 or IP protocol 138) sent as routing control and all else
as data. The Linux interface binding for the product remains undecided.

**Added 2026-10-08:** the OLSRv2 architecture. The
network-interface adapter `hh_netif` (`src/netif/`) carries payloads between
a host interface and `hh_radio_ops_t`, with the next hop as the MAC
destination and broadcast as id 0. It invents none of the Linux interface binding contract: the frame
kind is the caller's, destination ids are passed through, and the payload is
opaque. Tested on an emulated three-node medium; no OLSRd2 is involved yet.
