# `radiod/` — the radio-control daemon

`radiod` owns the radio and exposes it to other processes over a UNIX domain
socket. It is the control plane: lifecycle, configuration, status, channel
control and faults. It is **not** the data plane — packets never travel through
it.

**Targets:** `hhsdr_radiod` (library) + `radiod` (binary)
**Depends on:** `hhsdr_protocol`, `hhsdr_core_base` — *not* on `librc`, not on the MANET stack

```
radioctl / any client
        │  ASCII key=value over AF_UNIX/SOCK_STREAM   (ICD-2 in target: TLV)
        ▼
    ┌───────────────────────────────────────┐
    │  radiod                               │
    │   • lifecycle state machine (7 states)│
    │   • ≤16 clients, single-threaded      │
    │   • fault registry                    │
    └───────────────┬───────────────────────┘
                    │  hh_radio_ops_t  (8-function vtable, in-process)
                    ▼
        backends/ — mock_backend.c (works) · ocpi_backend.cpp (works, optional)
                    · hw_adapter.c (honest stub)
```

---

## Status: what works, what does not

| | |
|---|---|
| ✅ **Working** | daemon lifecycle, UNIX socket server, 10 command verbs, 7-state machine, config file, fault registry, mock backend, **OpenCPI backend** (owns an OpenCPI application through the ACI; tested on the host) |
| ❌ **Not implemented** | board bring-up steps, channel→frequency mapping, frames through radiod, TLV protocol, async events |

**radiod can own an OpenCPI application.** With `backend = ocpi` it creates,
initialises, configures and starts the application named by `ocpi_app` on
`start`, and stops and releases it on `stop` — the single-owner role of the
architecture's Note 1. It is tested against real OpenCPI 2.4.7 applications
on the development host. It has **not** yet run on the radio board, and the
board's bring-up steps (`ad9361_init`, the separate radio-setup application)
are not part of it: whether radiod owns them is open ([`../unknown.md`](../unknown.md), U-19).

radiod still depends **only** on the `hh_radio_ops_t` vtable; `radiod.c` did
not change to add the OpenCPI backend.

---

## Running it

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j8

./build/radiod/radiod                          # defaults: /tmp/hh-radiod.sock
./build/radiod/radiod -s /run/radiod.sock -v debug
./build/radiod/radiod -c config/radiod.example.conf
```

> **Note the path:** the binary is at `build/radiod/radiod` — `build/radiod/` is
> the component's build directory. (If CMake ever reports `Not a directory`
> here, you have a stale pre-refactor `build/` where `build/radiod` is a *file*:
> `rm -rf build` and reconfigure.)

| Flag | Meaning |
|---|---|
| `-c <file>` | config file (`key = value`) |
| `-s <path>` | socket path — **overrides the config file** |
| `-v <level>` | `error`\|`warn`\|`info`\|`debug`\|`trace` — overrides the config file |

Drive it with [`radioctl`](../radioctl/README.md):

```bash
./build/radioctl/radioctl init
./build/radioctl/radioctl configure 42
./build/radioctl/radioctl start
./build/radioctl/radioctl status
```

Stop it with `SIGINT`/`SIGTERM`, or `radioctl shutdown`. Either way
`hh_radiod_release()` unlinks the socket file.

### Configuration

Keys ([`config/radiod.example.conf`](../config/radiod.example.conf)),
deliberately separate from the MANET node's `hh_config_t`:

| Key | Default | Notes |
|---|---|---|
| `sock_path` | `/tmp/hh-radiod.sock` | `/tmp` is world-writable — see U-10 |
| `tick_interval_ms` | `10` | `poll()` timeout; bounds backend polling when idle |
| `client_idle_timeout_ms` | `0` | 0 = disabled (historical behaviour) |
| `log_level` | `info` | |
| `backend` | `mock` | `mock` or `ocpi` |
| `ocpi_app` | — | application XML; required when `backend = ocpi` |
| `ocpi_library_path` | — | exported as `OCPI_LIBRARY_PATH` before the app is created; empty = use the environment |
| `ocpi_property` | — | repeatable, `instance.property=value`, up to 16; written after `initialize`, before `start` |

Defaults reproduce pre-config behaviour exactly. A bad key reports its line
number and exits 1: `radiod: config error in x.conf at line 3: EINVAL`.

radiod names **no** worker, property or application itself — every one comes
from these keys. There are no PL, hopset or TLV settings: those contracts
don't exist, and configuration for them would be inventing them.

---

## The state machine

Seven states. The transition table is `transition_allowed()`
([`src/radiod.c`](src/radiod.c)); every request passes through it before any
work is done.

```
  created ──init──► initialized ──configure──► configured ──start──► running
                                       ▲                              │  ▲
                                       └──────configure───── stopped ◄┘  │
                                                              │  ▲       │
                                                       start ─┘  └─stop──┤
                                                                         │
                          faulted ◄──── start fails / inject_fault hw_fault
                             │
                             └── clear_fault ──► running     (stop also legal)

  any state ──shutdown──► released   (terminal; daemon exits)
```

| Verb | Legal from |
|---|---|
| `init` | `created` |
| `configure` | `initialized`, `configured`, `stopped` |
| `start` | `configured`, `stopped` |
| `stop` | `running`, `faulted` |
| `status`, `stats` | anything except `created`, `released` |
| `set_channel` | `running` **only** |
| `inject_fault`, `clear_fault` | `running`, `faulted` |
| `shutdown` | anything except `released` |

Rejection → `err <verb> reason=5` (`HH_ERR_STATE`), `requests_rejected++`, and a
`WARN` log line. **State is never mutated on a rejected request.**

Two transitions are automatic rather than commanded:
- `start` whose `hh_radio_open()` fails → **`faulted`**, and `hw_fault` is
  recorded in the registry.
- `set_channel` that fails → state unchanged, but `backend_io` is recorded.

---

## Key structures

### `hh_radiod_t` — the whole daemon

```c
typedef struct {
    hh_radio_t        *radio;        /* the backend vtable + instance         */
    const hh_clock_t  *clock;        /* injected: real or virtual (tests)     */
    hh_rc_state_t      state;        /* THE lifecycle state                   */
    hh_node_id_t       node_id;      /* set by configure                      */

    hh_radiod_fault_fn fault_fn;     /* backend-specific fault injection hook */
    void              *fault_ctx;

    int                listen_fd;    /* -1 until hh_radiod_listen()           */
    char               sock_path[HH_RADIOD_MAX_PATH];  /* kept so release() can unlink */
    hh_radiod_client_t clients[HH_RADIOD_MAX_CLIENTS]; /* fixed 16, no malloc */

    uint32_t           client_idle_timeout_ms;
    hh_radiod_faults_t faults;

    uint64_t requests_total;
    uint64_t requests_rejected;
} hh_radiod_t;
```

Everything is **caller-allocated and fixed-size**. `radiod` calls `malloc`
nowhere — client slots are a static array, and a 17th connection is closed
immediately rather than growing anything.

`clock` being a pointer is what makes the state machine testable without
sleeping: tests bind a virtual clock.

### `hh_radiod_client_t` — one connection

```c
typedef struct {
    int    fd;                        /* -1 = free slot                    */
    char   inbuf[HH_RC_MAX_LINE];     /* accumulates until '\n'            */
    size_t inlen;
    char   outbuf[HH_RC_MAX_LINE];    /* pending reply                     */
    size_t outlen, outsent;           /* outsent < outlen ⇒ partial write  */
    hh_time_ms_t last_activity;       /* for the idle timeout              */
} hh_radiod_client_t;
```

`outlen`/`outsent` exist because a socket may accept only part of a reply.
Without them a short `write()` silently truncated the response and
desynchronised that client's stream.

### `hh_radio_ops_t` — the hardware seam

Defined in [`../include/hhsdr/radio/radio.h`](../include/hhsdr/radio/radio.h),
shared with the MANET stack. Eight functions; implementing them is the entirety
of what hardware integration must supply:

```
open  close  transmit  set_rx_callback  get_status  get_link_metrics
set_channel  poll
```

radiod calls only **five** of them — `open`, `close`, `get_status`,
`set_channel`, `poll`. It never calls `transmit` or `set_rx_callback`, because
those are data plane.

---

## Call flow

```
main()                                            radiod_main.c
 ├─ hh_radiod_config_defaults / _load_file / _set     (CLI overrides file)
 ├─ make_backend(&cfg, ...)                           ← backend chosen HERE (`backend` key)
 ├─ hh_radiod_init(&daemon, &radio, hh_clock_monotonic())
 ├─ hh_radiod_configure(&daemon, &cfg)
 ├─ hh_radiod_set_fault_hook(&daemon, fault_hook, &backend)   (mock only)
 ├─ hh_radiod_listen(&daemon, cfg.sock_path)          → bind + listen(16)
 ├─ install_signal_handlers()                         → sigaction; SIGPIPE ignored
 │
 └─ while (!g_stop)
      ├─ hh_radiod_pollfds(&daemon, fds, …)           → listen_fd + live clients
      ├─ poll(fds, n, tick_interval_ms)               ← BLOCKS here
      ├─ hh_radiod_tick(&daemon, now)                 src/radiod.c
      │    ├─ accept_clients()      → accept4(SOCK_NONBLOCK), find free slot
      │    ├─ service_client() ×16
      │    │    ├─ flush_client()          resume any partial reply first
      │    │    ├─ read() → inbuf
      │    │    └─ while (memchr(inbuf,'\n',…))
      │    │         ├─ hh_rc_request_parse()
      │    │         ├─ hh_radiod_handle_request()  ← state machine + dispatch
      │    │         ├─ hh_rc_response_format() → outbuf
      │    │         └─ flush_client()
      │    ├─ expire_idle_clients()
      │    └─ if RUNNING: hh_radio_poll(radio, now)
      └─ if hh_radiod_shutdown_requested() break

 └─ hh_radiod_release()                              → close all fds, unlink socket
```

**`hh_radiod_handle_request()` is transport-independent**: the socket path and
the tests both call it, so state-machine behaviour has exactly one
implementation regardless of how the request arrived.

---

## Concurrency model

**Single-threaded. No locks, no threads, no atomics.** radiod's own code has
zero `pthread` or `mutex` usage.

One qualification with `backend = ocpi`: the OpenCPI runtime starts its own
container threads inside the process once an application exists, and links
`pthread` to do so. radiod still makes every ACI call from its one control
thread and shares no state with those threads, so nothing here needs a lock —
but the process is no longer single-threaded as a whole (U-12).

Consequences worth knowing:

- Up to 16 clients are served **round-robin, one pass per tick**. Requests are
  serialized; there is no interleaving and no shared-state hazard.
- A client is serviced only while it is not mid-reply, so **responses can never
  interleave** on one connection.
- One slow client cannot stall the daemon: a blocked write leaves the remainder
  queued and the loop moves on.
- A 17th connection is **accepted then immediately closed** — silently. The
  client sees a closed connection, not an error message.
- `poll()` means an idle daemon uses no CPU. Before this, a fixed 10 ms spin
  added up to 10 ms latency to every request; the test suite's wall time fell
  from 0.94 s to 0.59 s when it was fixed.

Signals set a `volatile sig_atomic_t` flag only. `SIGPIPE` is ignored so a
client vanishing mid-reply cannot kill the daemon.

---

## Fault registry

`hh_radiod_faults_t` ([`include/hhsdr/radiod/events.h`](include/hhsdr/radiod/events.h))
tracks, per fault kind: `active`, `count`, `first_seen`, `last_seen`,
`cleared_at`; plus a global `generation`, `asserted_total`, `cleared_total`.

Built over **only the five fault kinds the protocol already defines** — `none`,
`tx_failure`, `rx_silence`, `hw_fault`, `backend_io`. No severity levels, no
hardware fault codes: that taxonomy is unspecified (U-05).

Two behaviours that are deliberate, not incidental:

- **Re-asserting a standing fault is not a new event.** `count` and `last_seen`
  advance; `first_seen` and `generation` do not. A backend re-reporting the same
  condition every poll must not look like a storm, and the onset time must
  survive.
- **History survives a clear.** After clearing, `count` and `first_seen` remain —
  so *"happened once and recovered"* stays distinguishable from *"never
  happened"*. `generation` changes on both assert and clear, letting a polling
  client detect a flap it never directly observed.

`clear_fault` clears **every** kind, because the command carries no kind.

**Not exposed over the wire.** Reporting it needs a new response payload or an
async event, and neither format is specified (U-01/U-05). Read it in-process
with `hh_radiod_faults()`.

---

## Backends

### `mock_backend.c` — the working one

Deterministic `hh_radio_ops_t` implementation. Tracks `opened`, `operational`,
`channel`, `frequency_hz`, `waveform_id`, frame/error counters and
`active_fault`.

It is a **control-plane validation backend, not an RF simulator**: no waveform,
modulation or propagation modelling, and no synthetic frame generation.
`get_link_metrics` returns `HH_ERR_UNSUPPORTED` on purpose — per-frame metrics
are data plane.

Fault injection reaches it through the `hh_radiod_fault_fn` hook, which is the
single indirection point keeping radiod itself backend-agnostic.

### `ocpi_backend.cpp` — an OpenCPI application, through the ACI

Built only with `-DHH_WITH_OPENCPI=ON`. The one C++ file in radiod: the ACI is
C++ and OpenCPI 2.4.7's `aci/OcpiApi.h` declares nothing callable. Every entry
point is `extern "C"`, and every ACI exception (thrown as `std::string`) is
caught inside it and turned into an `hh_status_t` plus an `aci_error` log line.

| Operation | Does |
|---|---|
| `open` (radiod `start`) | create the application → `initialize` → write each `ocpi_property` → `start`. Any failure releases the half-built application. Same sequence as the radio project's `dma_stream.cc`. |
| `close` (radiod `stop`) | `stop`, then release. Safe twice. |
| `poll` | `wait(1 µs)`; an application that ended on its own is logged `app_finished` and reported not operational |
| `get_status` | `operational` = open and not finished; everything else 0 (no property is defined as its source) |
| `set_channel` | `HH_ERR_UNSUPPORTED` — no channel plan (U-14) |
| `transmit` | `HH_ERR_NOT_IMPLEMENTED` — data plane undecided (U-15) |
| `get_link_metrics` | `HH_ERR_UNSUPPORTED` |

`hh_ocpi_backend_get_property()` reads any property of the running
application; the tests use it to prove data is flowing.

Build and run it on a development host:

```bash
source ~/opencpi/cdk/opencpi-setup.sh -s
cmake -S . -B build-ocpi -DHH_WITH_OPENCPI=ON && cmake --build build-ocpi -j8
ctest --test-dir build-ocpi -R ocpi --output-on-failure
```

The link follows the CDK's `ocpisetup.mk` recipe with two corrections found
by linking against 2.4.7, recorded in `CMakeLists.txt`. The resulting binary is
statically linked against OpenCPI (about 16 MB in a Debug build). Cross-building
for the board (`xilinx19_2_aarch32`) has not been done yet.

### `hw_adapter.c` — the honest stub

Every hardware operation returns `HH_ERR_NOT_IMPLEMENTED` and logs why.
`hh_hw_adapter_is_stub()` returns `true` unconditionally. It fails loudly rather
than pretending to work.

> **Note:** `hh-manet` still links this adapter and constructs its own instance.
> That is a *latent* violation of the single-owner rule — harmless today because
> the stub acquires nothing, but a real conflict the moment a working backend
> exists. Severing it is blocked on U-15 (how the data plane reaches hardware
> once only radiod may open the PL).

---

## Tests

```bash
ctest --test-dir build -R 'radiod|mock_backend' --output-on-failure
```

| Test | Covers |
|---|---|
| `test_radiod_state_machine` | every transition, legal and illegal |
| `test_radiod_config` | defaults, parsing, validation, line-accurate errors |
| `test_radiod_faults` | registry in isolation **and** as radiod drives it |
| `test_mock_backend` | the `hh_radio_ops_t` contract |
| `test_radiod_daemon` | **end-to-end**: forks the real binary, drives it over a real socket |
| `test_ocpi_backend` | OpenCPI backend against real OpenCPI applications: start, data flowing, stop, restart, configured property applied, bad property / missing app fail cleanly, self-finishing app detected (`HH_WITH_OPENCPI` only) |
| `test_radiod_ocpi_daemon` | **end-to-end** with `backend = ocpi`: the real binary owns a real OpenCPI application; a failed start leaves radiod `faulted` (`HH_WITH_OPENCPI` only) |

The two OpenCPI tests use real time (short, bounded waits) rather than the
virtual clock, because they drive a real runtime moving real data. Their
fixtures in `tests/ocpi/` use only the stock `ocpi.core` file_read/file_write
components, so they stand in for a waveform without being one.

`test_radiod_daemon` links `librc` — *not* `hhsdr_radiod` — so it exercises the
daemon exactly as an external client would. That is what proves the client
library is genuinely independent of the daemon.

---

## Extending it

**Adding a command verb** touches three places: the enum + name table in
`protocol/`, `transition_allowed()`, and the dispatch `switch`. Add a test to
`test_radiod_state_machine.c`. Remember there is no protocol versioning — a new
verb breaks older peers in both directions.

**Adding a backend:** implement the 8 `hh_radio_ops_t` functions in
`src/backends/`, add an `init` that fills an `hh_radio_t`, and select it in
`radiod_main.c`. Nothing else changes — that is the seam working as intended.

**Before extending the OpenCPI backend** (board bring-up, status counters from
application properties, frames), read U-03, U-04, U-12, U-14, U-15 and U-19 in
[`../unknown.md`](../unknown.md).
