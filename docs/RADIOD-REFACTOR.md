# radiod Refactor — Plan and Progress

Target architecture: **HH-SDR PS/PL Software Architecture — Three-Plane Bridge**
(Doc No: OSL-628-BD-101 Rev: 00).

This document tracks the restructuring of `radiod` into a self-contained
component, together with the `protocol/`, `librc/` and `radioctl/` components
the target architecture names.

Open contracts are recorded in [`unknown.md`](../unknown.md). Nothing listed
there is guessed at in code.

---

## Scope

**In scope — structural only:**

- Separate `radiod` into its own top-level component
- Extract the control-protocol codec into a shared `protocol/` component
- Extract the client library into `librc/`
- Add the `radioctl` CLI, which the target names but which does not exist today
- Correct the dependency direction so that `librc` never depends on the daemon
- Harden the daemon and add a fault registry using only already-known semantics

**Out of scope this round:**

| Item | Reason |
|---|---|
| ICD-2 TLV codec | No specification exists — see U-01 |
| OpenCPI / ACI integration | No app XML, worker names or lifecycle — U-03, U-04 |
| RCC workers, drivers, FPGA | No contracts — U-03, U-06, U-07 |
| OLSRv2 migration | Deferred by decision — U-09 |
| Severing `hh-manet` from the PL adapter | Behaviour change — U-15 |

**Preserved throughout:** the existing line-oriented ASCII `key=value` control
protocol, the 7-state lifecycle, all 10 command verbs, and the whole of
`src/manet/`.

---

## Invariants

Every phase must satisfy all of these before it is committed:

1. The build succeeds with **zero warnings** under
   `-Wall -Wextra -Wshadow -Wpointer-arith -Wcast-qual -Wstrict-prototypes`.
2. **All tests pass** (28 at baseline; the count only grows).
3. Behaviour is unchanged unless the phase explicitly states otherwise.
4. Dependencies flow one way only. In particular `librc` must never depend on
   `hhsdr_radiod`.
5. No contract from `unknown.md` is invented, stubbed with placeholder values,
   or approximated.

---

## Phases

| # | Phase | Status |
|---|---|---|
| 0 | Baseline, sanitizer verification, `unknown.md` | **DONE** |
| 1 | Extract `protocol/` | **DONE** |
| 2 | Extract `librc/` | **DONE** |
| 3 | Add `radioctl/` CLI | pending |
| 4 | Create `radiod/`, move the daemon | pending |
| 5 | Split `hhsdr_core` into base and MANET libraries | pending |
| 6a | Relocate `hw_adapter` into `radiod/src/backends/` | pending |
| 7 | radiod configuration file and daemon hardening | pending |
| 8 | Event/fault registry (in-process) | pending |
| 9 | Empty labelled scaffolding for drivers/workers/fpga | pending |

Phase **6b** — severing `hh-manet`'s link to the PL adapter, which is what
fully enforces the drawing's Note 1 — is **deliberately not in this round**. It
is the only genuinely behaviour-changing step and is blocked on U-15.

---

## Phase 0 — Baseline (DONE)

Established the reference point that every later phase is measured against.

- Tagged `pre-radiod-refactor` at commit `763cdb1`
- Verified a clean out-of-tree build: **0 warnings**
- Verified **28/28** tests passing
- Verified a clean **AddressSanitizer + UBSan** build and run: 28/28, 0 warnings
- Authored [`unknown.md`](../unknown.md) — 15 entries (U-01 … U-15)

Rollback: `git checkout pre-radiod-refactor`.

---

## Phase 1 — Extract `protocol/` (DONE)

The control-protocol codec previously lived inside `hhsdr_core`, which meant
`hh-manet` linked a control protocol it never speaks, and no client library
could exist without also linking the daemon.

Moved, with history preserved via `git mv`:

| From | To |
|---|---|
| `include/hhsdr/radio/rc.h` | `protocol/include/hhsdr/protocol/rc.h` |
| `src/radio/rc.c` | `protocol/src/rc.c` |
| `tests/unit/test_rc_protocol.c` | `protocol/tests/test_rc_protocol.c` |

New target `hhsdr_protocol`. The header guard became `HHSDR_PROTOCOL_RC_H`, and
five files had their include path updated. No symbol was renamed and no
behaviour changed.

Verified: `hhsdr_core` now exports **zero** `hh_rc_*` codec symbols;
`hhsdr_protocol` exports **nine**. Build clean, 28/28 passing.

---

## Phase 2 — Extract `librc/` (DONE)

The client library previously lived *inside* the daemon library
(`src/radiod/rc_client.c` compiled into `hhsdr_radiod`). Any client wishing to
talk to radiod therefore had to link the entire daemon — the exact inversion the
architecture's separate `librc` box rules out.

Moved, with history preserved:

| From | To |
|---|---|
| `src/radiod/rc_client.c` | `librc/src/rc_client.c` |
| `include/hhsdr/radiod/rc_client.h` | `librc/include/hhsdr/librc/rc_client.h` |

New target `librc` (artifact `librc.a`), depending on `hhsdr_protocol` only.
Header guard became `HHSDR_LIBRC_RC_CLIENT_H`.

The end-to-end daemon test now links **`librc` instead of `hhsdr_radiod`**. That
is the load-bearing change: the test drives the daemon exactly as an external
client would, so the separation is enforced by the build rather than merely
documented.

Verified: `librc.a` exports the three `hh_rc_client_*` symbols;
`hhsdr_radiod` exports **zero** of them; the e2e test's link line references
`librc` and not the daemon library. Build clean, 28/28 passing.

**Not done — U-02.** The drawing shows an `rc_*` API. Those signatures are
unspecified, so the existing `hh_rc_client_*` API is carried forward unchanged
rather than inventing names.

---

## Target structure

```
hh-sdr-manet/
├── unknown.md              register of blocked contracts
├── protocol/               shared control-protocol codec
│   ├── include/hhsdr/protocol/
│   ├── src/
│   └── tests/
├── radiod/                 the daemon — self-contained
│   ├── include/hhsdr/radiod/
│   ├── src/
│   │   └── backends/       mock, hw (stub)
│   ├── radiod_main.c
│   └── tests/
├── librc/                  client library; depends on protocol only
├── radioctl/               CLI; depends on librc only
├── include/ + src/         core, manet, radio/wire, dataplane, sca (unchanged)
├── tools/sim/              development tooling (unchanged)
├── tests/                  unit, integration, scenario, sim (unchanged)
└── drivers/ workers/ fpga/ empty, labelled with blocking unknowns
```

### Dependency direction

```
radioctl ──► librc ──► protocol ──► core_base
radiod   ──► hhsdr_radiod ──► protocol ──► core_base
                  └──► backends (mock, hw)
hh-manet ──► hhsdr_manet ──► core_base
```

Arrows point one way only. There are no cycles at baseline, and none may be
introduced.

---

## Why the protocol is being moved rather than replaced

The target calls for "versioned TLV over UNIX socket". The repository
implements a line-oriented ASCII `key=value` protocol, chosen deliberately and
documented as such in the protocol header — it reuses the project's existing
configuration and telemetry conventions instead of introducing a new
serialization layer.

Replacing it requires the ICD-2 specification, which does not exist (U-01).
Inventing a wire format now would create a second, competing protocol that
would have to be reconciled later.

The protocol codec is therefore moved into its own `protocol/` component,
independent of both the daemon and the client. When ICD-2 is specified, its
codec is added alongside the existing one without touching radiod's state
machine, its dispatch logic, or the client library.
