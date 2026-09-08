# PetaLinux Deployment Architecture

**Status: design only.** Nothing described here is implemented yet. This document
defines the target deployment so the recipes, packaging, and integration work can
be scoped and reviewed before any of it is written.

Facts below marked *(measured)* were taken from the current build; the rest are
design decisions to be confirmed against the actual board.

---

## 1. What the software actually needs

The deployment story is unusually simple because of what the implementation
does **not** do *(all measured on the current tree)*:

| Property | Value | Consequence for deployment |
|---|---|---|
| Heap allocation | **None** — no `malloc`/`free` anywhere in `src/` | No allocator tuning, no fragmentation risk, no OOM path |
| Threads | **None** — single control loop | No pthread linkage, no scheduler/affinity config needed |
| Runtime shared libs | **libc only** | No `libm` at runtime, no `libatomic`, no `libpthread` |
| POSIX surface | `clock_gettime`, `nanosleep`, `signal` | Satisfied by any glibc or musl PetaLinux rootfs |
| Files opened at runtime | **One** — the config file, read once at startup | No runtime filesystem dependency after init |
| Static state per node | **~277 KB** (`hh_node_t`) | Sized at build time; no growth under load |
| Wire format | Fixed 48-byte beacon, explicit little-endian | Endianness-safe across host/target |

`libm` is linked but not required at runtime *(measured: absent from `ldd`)* —
the float math resolves at compile time. The link flag should be dropped when
the Yocto recipe is written.

### Atomics

The only atomic operations are **pointer-width load/store** on the route-table
snapshot (`src/manet/route_table.c`). These are natively lock-free on ARMv7 and
ARM64, so **`libatomic` is not required**. This must be re-verified for the
chosen target with `-Wl,--no-undefined`; if the toolchain ever emits a call to
`__atomic_load_8`, the design intent is to keep pointer-width atomics rather
than link libatomic.

---

## 2. Artifacts on the PetaLinux target

Exactly four things ship. Nothing else is required at runtime.

| Path | Artifact | Type | Notes |
|---|---|---|---|
| `/usr/bin/hh-manet` | Node daemon | ELF, stripped | The only executable |
| `/etc/hh-manet/node.conf` | Node configuration | Text `key = value` | Per-node; `node_id` differs per unit |
| `/lib/systemd/system/hh-manet.service` | Service unit | systemd unit | Boot integration |
| `/usr/lib/libhhsdr_radio_hw.so` | **FPGA radio adapter** | Shared object | **Does not exist yet** — see |

**Deliberately not shipped:** headers, the static library, the test binaries, the
simulator, `demo_selfheal`, and CMake files. The target carries a daemon and its
configuration, nothing more.

### Optional, per site policy
- `/usr/share/hh-manet/node.example.conf` — reference configuration
- Debug (unstripped) binary kept **off-target** in the build artifact store, so
  a core dump can be symbolised without shipping symbols to the device.

---

## 3. Built on the development host (never cross-compiled)

These exist to validate the code and never reach the target:

| Artifact | Purpose |
|---|---|
| 15 test binaries (`ctest`) | 165 tests: unit, integration, 14 MANET scenarios |
| `libhhsim.a` | Mock radio, virtual clock, network simulator |
| `demo_selfheal` | Human-readable behavior demonstration |
| Sanitizer build (`-DHH_SANITIZE=ON`) | ASan/UBSan validation |

**This separation is already enforced in the source tree**, which is what makes
it safe: no file under `src/` or `include/` references anything under `tests/`,
and the production library contains zero test symbols. The Yocto recipe inherits
that guarantee rather than having to police it — it simply never builds
`tests/`.

Host validation runs on x86-64 because the logic is architecture-independent.
Cross-architecture confidence comes from.

---

## 4. What is cross-compiled

Only the production library and the daemon:

```
src/core/       src/radio/      src/manet/
src/dataplane/  src/sca/        src/adapters/     src/main.c
        │
        └── libhhsdr_core.a (static) ──► hh-manet
```

**Static linkage of the project's own code is intentional**: the daemon is one
self-contained binary with no internal `.so` versioning to manage in the field.
The only dynamic dependency is the C library, plus — in future — the FPGA
adapter, which is dynamic *by design*.

### Toolchain
The PetaLinux/Yocto SDK cross-toolchain for the target (ARM Cortex-A). The
project already builds warning-clean under `-Wall -Wextra -Wshadow
-Wpointer-arith -Wcast-qual -Wstrict-prototypes`; the cross build must keep
`-Werror` so a portability warning fails the build rather than shipping.

Two decisions to make when the recipe is written:
- **`-O2` vs `-Os`** — the binary is ~216 KB *(measured, x86-64, unstripped)*;
  `-Os` is likely the right default for an embedded target.
- **Hardware float ABI** — must match the PetaLinux machine configuration.

### Portability review needed before first cross build
1. `size_t` narrows from 64- to 32-bit on ARM32. The code uses `size_t` for all
   counts and `uint64_t` explicitly for counters, so this is expected to be
   clean — but it must be compiled and the warnings read, not assumed.
2. `hh_node_t` static footprint shrinks on ARM32 (fewer pointer bytes) — confirm
   it fits the target's memory budget.
3. Confirm no `__atomic_*` libcalls are emitted.

---

## 5. Cross-architecture validation

Building for ARM proves it *compiles*; it does not prove it *behaves*. Because
the tests are pure logic over a virtual clock with no hardware dependency, the
**entire 165-test suite can run on the target** — or under QEMU, which PetaLinux
already provides.

Proposed gate, in order:
1. Host build + full suite + sanitizers (already in place)
2. Cross-compile with `-Werror`, review every warning
3. Run the full suite under `qemu-arm` / PetaLinux QEMU
4. Run the suite on real hardware once a board is available

Steps 3–4 need the test binaries staged to the target, which is why a separate
**`hh-manet-tests` package** should exist — installed in development images
only, never in production images.

---

## 6. Configuration deployment

Configuration is already fully externalised: no threshold, interval, or topology
is compiled in. Every tunable is a `key = value` line, validated at load, with a
reported line number on error.

### Layering (later wins)
```
1. Built-in defaults        hh_config_defaults()  — always valid
2. /etc/hh-manet/node.conf  the deployed file
3. Command-line             -n <node_id>, -v <level>
```

### Per-node identity
`node_id` **must be unique per radio** — the design intent is to derive it at
provisioning time from a hardware-unique value (serial number, EEPROM ID, or MAC),
rather than hand-editing files per unit. The mechanism is TBD; what matters
architecturally is that identity comes from the device, not from the image, so a
single image can be flashed to every unit.

The daemon refuses to start when `node_id` is unset, rather than defaulting to
something that would collide across units.

### Base image vs. per-unit
- The **image** carries a template config with everything except identity.
- **Identity** is applied per unit at provisioning.
- Retunable properties are exposed through the SCA `configure` property surface
  (23 properties with stable ids), so a future management plane can adjust them
  at runtime without a reflash. `execparam`-kind properties such as `node_id`
  are fixed at launch by design.

---

## 7. Boot and service management

`systemd` (PetaLinux default). A `sysvinit` fallback would be a small shell
script if a project chooses `sysvinit`.

### Service intent

```ini
[Unit]
Description=HH-SDR Self-Healing MANET Node
After=network.target
# Add: After=<fpga-bitstream-load>.service once the PL is loaded at boot

[Service]
Type=simple
ExecStart=/usr/bin/hh-manet -c /etc/hh-manet/node.conf
Restart=on-failure
RestartSec=5s

[Install]
WantedBy=multi-user.target
```

### Ordering constraint — the important one

The daemon **must not start before the FPGA bitstream is loaded and the PL
radio interface is available.** The current stub adapter fails cleanly at
`open()` and the daemon exits with a diagnostic, so a premature start is safe
and visible rather than silently broken — but once real hardware exists, an
explicit `After=` / `Requires=` on the bitstream-load unit is required.

### Restart policy
`Restart=on-failure` is correct **only after** the radio adapter is real. With
today's stub the daemon always exits non-zero, so systemd would restart-loop.
Until the FPGA adapter exists, the service should ship **disabled**, or the
recipe should not install the unit at all.

### Logging
Structured `key=value` records go to stderr, which systemd captures into the
journal automatically. No log file, no rotation, no logging library. `journalctl
-u hh-manet` gives operators the same records the tests assert on.

---

## 8. FPGA hardware adapter packaging

This is the one deliberately unfinished seam, and the packaging choice matters
because it determines whether FPGA integration touches the MANET stack.

### The contract

The entire hardware surface is **one vtable, `hh_radio_ops_t`, with 8 functions**
(`include/hhsdr/radio/radio.h`):

```
open  close  transmit  set_rx_callback  get_status  get_link_metrics
set_channel  poll
```

Nothing in the control plane, data plane, or SCA layer references anything
below this line. No AXI register map, DMA API, PHY/modem control, RF tuner API,
or sample-streaming interface exists anywhere in the repository — verified by
grepping the preprocessed source with comments stripped.

### Recommended packaging: a separate shared object

```
libhhsdr_radio_hw.so   ← FPGA adapter, its own recipe, its own version
        │ implements
        ▼
hh_radio_ops_t (stable ABI)
        ▲
        │ depends on
hh-manet               ← MANET stack, unchanged across adapter revisions
```

**Why dynamic here, when everything else is static:** the FPGA bitstream, the PL
interface, and the adapter will iterate on a different schedule from the
networking stack. Separating them means a PL revision ships a new `.so` and a
new bitstream — not a rebuilt MANET daemon. That is precisely the property the
architecture was designed to deliver.

The adapter is selected by the existing `radio_adapter` configuration key, which
already exists and is already an SCA `execparam` property.

### Alternative considered
Static linkage of the adapter into `hh-manet` is simpler and avoids `dlopen`.
It is a reasonable choice **if** the adapter and stack will always ship as one
unit. It should be an explicit decision, not a default, because it couples two
things the architecture deliberately decoupled.

### What the adapter recipe must carry
- The adapter implementation (`.so`)
- A build/runtime dependency on the PL driver interface, whatever that becomes
- A declared relationship to the bitstream version it is valid against

### Bitstream
The FPGA bitstream is a separate artifact on its own lifecycle, loaded before
the service starts. Its packaging is a PetaLinux/Vivado concern outside
this stack's scope, but the **version relationship between bitstream and adapter
must be explicit**, since a mismatched pair is exactly the failure this
separation is meant to make obvious rather than mysterious.

---

## 9. Package summary

| Package | Contents | Image |
|---|---|---|
| `hh-manet` | daemon, config template, systemd unit | production + development |
| `hh-manet-tests` | 15 test binaries, demo | **development only** |
| `hh-manet-radio-hw` | FPGA adapter `.so` | production, once it exists |
| *(bitstream)* | PL image | production, separate lifecycle |

---

## 10. Open items to resolve before implementation

1. **`node_id` provisioning mechanism** — which hardware-unique value, read how.
2. **Bitstream-load unit name**, for the systemd `After=` dependency.
3. **Adapter linkage decision** — shared object (recommended) vs. static.
4. **Target memory budget** — confirm ~277 KB static state per node fits.
5. **`-Os` vs `-O2`**, and the float ABI matching the machine config.
6. **libatomic** — confirm not needed on the specific toolchain.
7. **Bitstream ↔ adapter version compatibility** expression.
8. **Whether the production image ships the service enabled** — it must not,
   until the radio adapter is real.
