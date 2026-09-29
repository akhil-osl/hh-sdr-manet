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
        backends/ — mock_backend.c (works) · hw_adapter.c (honest stub)
```

---

## Status: what works, what does not

| | |
|---|---|
| ✅ **Working** | daemon lifecycle, UNIX socket server, 14 command verbs, 7-state machine, config file, fault registry, interactive console, mock backend, **OpenCPI backend** |
| ⚠ **Host-verified only** | OpenCPI / PL ownership — 36 CTest cases including end to end through the real binary, but never run on a board |
| ❌ **Not implemented** | TLV protocol, async events, data plane |

**radiod is the PL owner now.** The architecture's Note 1 — *"only radiod opens
OpenCPI; never a second ACI instance"* — is implemented by
`src/backends/opencpi_backend.c`, which drives a real OpenCPI application
through the **rh_agent** helper and the **RADHAWK** API published by the OpenCPI
project. `../unknown.md` U-03, U-04, U-11 and U-12 are resolved. Start with
[`../docs/OPENCPI-INTEGRATION.md`](../docs/OPENCPI-INTEGRATION.md).

What made that safe, and still holds: radiod depends **only** on the
`hh_radio_ops_t` vtable (plus a second, orthogonal property vtable) and never
names a concrete backend. It also names **no worker and no property** — the
agent publishes its registry and radiod routes against it.

What is genuinely unfinished: **none of it has run on hardware.** The agent
builds for `xilinx19_2_aarch32` but has not driven a PL.

---

## Running it

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j8

./build/radiod/radiod                          # defaults: /tmp/hh-radiod.sock, mock backend
./build/radiod/radiod -s /run/radiod.sock -v debug
./build/radiod/radiod -c config/radiod.example.conf

# the real radio, with a prompt on this terminal
./build/radiod/radiod -b opencpi -a /opt/txrx_worker_rt_ch2_dma/rh_agent -i
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
| `-b <backend>` | `mock` (default) \| `opencpi` — overrides the config file |
| `-a <path>` | rh_agent binary, for the `opencpi` backend — overrides the config file |
| `-i` | also serve an interactive console on this terminal |

`-i` adds a `radiod> ` prompt on stdin, for an operator sitting at the terminal.
The control socket is served either way, and a typed line goes through the same
request pipeline as a socket client's, so the console can neither accept nor
refuse anything the socket would not. `help` lists the commands; `quit` or
Ctrl-D shuts the daemon down through the ordinary SHUTDOWN path.

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

Eight keys ([`config/radiod.example.conf`](../config/radiod.example.conf)),
deliberately separate from the MANET node's `hh_config_t`:

| Key | Default | Notes |
|---|---|---|
| `sock_path` | `/tmp/hh-radiod.sock` | `/tmp` is world-writable — see U-10 |
| `tick_interval_ms` | `10` | `poll()` timeout; bounds backend polling when idle |
| `client_idle_timeout_ms` | `0` | 0 = disabled (historical behaviour) |
| `log_level` | `info` | |
| `radio_backend` | `mock` | `mock` \| `opencpi`. An unknown name is rejected, not defaulted |
| `opencpi_agent` | *(compiled-in)* | rh_agent path; `/opt/txrx_worker_rt_ch2_dma/rh_agent` |
| `opencpi_reply_timeout_ms` | `3000` | ceiling on how long one agent call may block the loop |
| `opencpi_start_timeout_ms` | `60000` | larger because `start` may run `ad9361_init` |

Defaults reproduce pre-config behaviour exactly — including `radio_backend =
mock`, so an existing deployment is unaffected. A bad key reports its line
number and exits 1: `radiod: config error in x.conf at line 3: EINVAL`.

There is still **no per-worker or per-property configuration**, and there must
not be: property names, types and ranges belong to the backend, and properties
are set at run time with `opencpi set`. Hopset and TLV settings remain absent
because those contracts still do not exist (U-01, U-14).

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
 ├─ hh_mock_backend_init(&backend, &radio)            ← backend chosen HERE
 ├─ hh_radiod_init(&daemon, &radio, hh_clock_monotonic())
 ├─ hh_radiod_configure(&daemon, &cfg)
 ├─ hh_radiod_set_fault_hook(&daemon, fault_hook, &backend)
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

**Single-threaded. No locks, no threads, no atomics.** Zero `pthread` or `mutex`
usage in the whole repository.

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

### `opencpi_backend.c` — the real PL backend

Drives the OpenCPI application through the **rh_agent** helper process and the
**RADHAWK** C API (`api/radhawk.h` in the OpenCPI project). Selected with
`radio_backend = opencpi`, or `radiod -b opencpi`.

It implements **both** backend contracts:

| contract | what it provides |
|---|---|
| `hh_radio_ops_t` | `start`/`stop` deploy and release the OpenCPI application; `status` reports whether it is running |
| `hh_radiod_props_ops_t` | the `opencpi_*` verbs — list, get, set and sample worker properties |

Why a helper process rather than linking the ACI into radiod: in this CDK the
OpenCPI runtime archives are **not position-independent**, so the ACI links into
an executable and nothing else. `hhsdr/radiod/opencpi_agent_proto.h` carries the
measured linker error and the full reasoning. The boundary also resolves
`unknown.md` U-11 (C versus C++) and U-12 (threading) outright: the ACI runs in
its own process, so radiod stays C11 and single-threaded, and an ACI crash takes
the agent down rather than the control plane.

**radiod knows no property names.** The agent publishes its registry at startup
and radiod only routes, enforcing the access class the agent declared. Adding a
property is one row in `rh_agent.cc` — no radiod change, no protocol change.

Three access classes, and radiod's one rule:

| class | meaning | radiod's behaviour |
|---|---|---|
| `rw` | writable at any time | applied live while running, staged while stopped |
| `rw-start` | writable only while stopped | **refused** while running, never silently queued |
| `ro` | a live counter | readable only while running |

Building it changes nothing about radiod's build: no OpenCPI header is included
and no OpenCPI symbol referenced anywhere in `hhsdr_radiod`, so the library and
the daemon compile identically on a host with no CDK installed. See
`docs/OPENCPI-INTEGRATION.md` for the end-to-end picture and the build commands.

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

**Before adding OpenCPI**, read U-03, U-04, U-11 (the ACI is C++; this project
is strict C11) and U-15 in [`../unknown.md`](../unknown.md).
