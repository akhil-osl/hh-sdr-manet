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
2. **All tests pass** (28 at baseline; the count only grows — 29 as of Phase 3).
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
| 3 | Add `radioctl/` CLI | **DONE** |
| 4 | Create `radiod/`, move the daemon | **DONE** |
| 5 | Split `hhsdr_core` into base and MANET libraries | **DONE** |
| 6a | Relocate `hw_adapter` into `radiod/src/backends/` | **DONE** |
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

## Phase 3 — Add `radioctl/` (DONE)

The architecture names a `radioctl` CLI and requires test automation to drive
the radio through it rather than through private access. No such binary
existed; this phase adds it. Purely additive — no existing file changed.

Created:

| File | Purpose |
|---|---|
| `radioctl/radioctl_main.c` | the CLI |
| `radioctl/CMakeLists.txt` | target definition |
| `radioctl/tests/test_radioctl.c` | end-to-end test |

**Subcommands map one-to-one onto the ten verbs the protocol already defines.**
No verb, parameter or output field is invented. Multi-word verbs accept both
the hyphenated CLI spelling (`set-channel`) and the wire spelling
(`set_channel`), so scripts written either way work.

Exit statuses, because a CLI's exit code is its contract with scripts:

| Code | Meaning |
|---|---|
| 0 | success |
| 2 | usage error — refused locally, radiod never contacted |
| 3 | cannot reach radiod (transport) |
| 4 | radiod answered and rejected the request |

The distinction between 3 and 4 matters: a rejection is a *successful* exchange
carrying a refusal, and automation needs to tell that apart from a dead daemon.

`test_radioctl` runs the **real `radioctl` binary against the real `radiod`
binary** — five cases covering the full lifecycle, the fault cycle, rejection,
absent daemon, and malformed invocations. It links `librc` only.

Verified manually against a live daemon: lifecycle transitions reported
correctly, `set-channel 11` persisted into a subsequent `status`, injected
`hw_fault` flipped `operational` to 0, and the three exit statuses behaved as
documented. Build clean, **29/29** passing.

---

## Phase 4 — Create `radiod/` (DONE)

The daemon is now a self-contained component. Eight files moved, history
preserved:

| From | To |
|---|---|
| `src/radiod/radiod.c` | `radiod/src/radiod.c` |
| `src/radiod/mock_backend.c` | `radiod/src/backends/mock_backend.c` |
| `include/hhsdr/radiod/radiod.h` | `radiod/include/hhsdr/radiod/radiod.h` |
| `include/hhsdr/radiod/mock_backend.h` | `radiod/include/hhsdr/radiod/mock_backend.h` |
| `tools/radiod/radiod_main.c` | `radiod/radiod_main.c` |
| `tests/radiod/test_radiod_daemon.c` | `radiod/tests/test_radiod_daemon.c` |
| `tests/unit/test_radiod_state_machine.c` | `radiod/tests/test_radiod_state_machine.c` |
| `tests/unit/test_mock_backend.c` | `radiod/tests/test_mock_backend.c` |

The `hhsdr/radiod/` include prefix was kept, so **no source file needed an
include edit**. The emptied `src/radiod/`, `tools/radiod/`, `tests/radiod/` and
`include/hhsdr/radiod/` directories were removed.

Each component now registers its own tests alongside the code they exercise,
rather than from the top-level `tests/` tree.

### Two build-system details worth recording

**Binary output directory.** An initial `RUNTIME_OUTPUT_DIRECTORY
${CMAKE_BINARY_DIR}` override made the link step emit `Linking C executable .`
and fail with *"cannot open output file .: Is a directory"*. The override was
unnecessary — tests locate binaries through `$<TARGET_FILE:...>` — so it was
dropped rather than worked around.

**Test guard.** Component tests were first guarded on `BUILD_TESTING`, which
silently registered **zero** of them: this project calls `enable_testing()`
directly rather than `include(CTest)`, so `BUILD_TESTING` is never defined. The
guard is now an explicit `HH_BUILD_TESTS`, set in the root `CMakeLists.txt`.
Caught only because the test count dropped from 29 to 25.

Verified: **29/29** passing, 0 warnings, and clean under
AddressSanitizer + UBSan.

---

## Phase 5 — Split `hhsdr_core` (DONE)

`hhsdr_core` was a single library holding both the shared primitives and the
whole MANET stack, so the control-plane components declared a dependency on
routing, topology, the forwarder and the SCA layer that none of them call.

Split into two targets. No source file moved and no code changed:

| Target | Contents |
|---|---|
| `hhsdr_core_base` | `types.c`, `clock.c`, `log.c`, `events.c` |
| `hhsdr_core` | `config.c`, `dispatcher.c`, `wire.c`, `hw_adapter.c`, all of `manet/`, `dataplane/`, `sca/` — layered on the base |

The split point was chosen by reading actual includes: the entire
radiod/librc/radioctl stack includes only four core headers — `types.h`,
`clock.h`, `log.h` and `radio.h` (which itself needs only `events.h` and
`types.h`).

`hhsdr_core` keeps its name, so `hh-manet`, the tests and the simulation
tooling are unaffected.

### Honest accounting of the benefit

The `radiod` and `radioctl` binaries are **byte-for-byte identical** before and
after this phase, and both contained **zero** MANET symbols beforehand. The
static linker was already discarding the unused objects.

So this phase delivers **no runtime or size improvement**. Its value is
structural: the dependency graph now *states* the boundary explicitly rather
than leaving it to linker garbage collection, so a future accidental coupling
fails at link time instead of silently bloating the daemon. That is worth
having, but it should not be reported as an optimisation.

Verified: 29/29 passing, 0 warnings.

---

## Phase 6a — Relocate `hw_adapter` (DONE)

The PL-facing hardware adapter now sits where the single PL owner keeps its
backends:

| From | To |
|---|---|
| `src/adapters/hw/hw_adapter.c` | `radiod/src/backends/hw_adapter.c` |

The header stays at `include/hhsdr/radio/hw_adapter.h`. It has five consumers
(`src/main.c`, `src/manet/telemetry.c`, and three tests); leaving it in place
means none of them needed an edit, and the header is genuinely shared until the
ownership question is settled.

**`hh-manet` still compiles and links it, so behaviour is unchanged.** Verified
by running the daemon: with the hardware adapter selected it still fails at
`open()` with `ENOTIMPL` and the same diagnostic as before.

### Why the link was not severed

Severing `hh-manet` from the PL-facing adapter is what *fully* enforces the
architecture's single-owner rule. It is deliberately not done here because:

- It changes `hh-manet`'s startup behaviour, which this refactor is otherwise
  careful not to do.
- It touches six files across the MANET stack (`main.c`, `node.c`,
  `self_healing.c`, `forwarder.c`, `discovery.c`, `telemetry.c`).
- It is blocked on **U-15** — how the data plane reaches hardware once only
  radiod may open the PL.

The dual-ownership condition is currently **latent and harmless**: the adapter
is a stub whose every hardware operation returns `HH_ERR_NOT_IMPLEMENTED`, so
nothing is actually acquired twice. It becomes a real conflict the moment a
working backend exists, and must be resolved before then.

Verified: 29/29 passing, 0 warnings, `hh-manet` behaviour byte-identical.

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
