# radiod ↔ OpenCPI integration

How radiod became the single control interface for the OpenCPI QPSK SDR
workers, what was added, and how to build and run it.

---

## 1. What was already there

Neither side needed to be invented. The integration connects two things that
already existed.

**radiod** (this repository) is a C11 daemon: a single-threaded `poll()` loop
that owns one `hh_radio_ops_t` backend and serves a line-oriented `key=value`
request/response protocol (`protocol/include/hhsdr/protocol/rc.h`) over a UNIX
socket. `radioctl` is its CLI, reaching it through `librc`. It had ten command
verbs, a lifecycle state machine and a fault registry — and a mock backend,
because `unknown.md` U-03/U-04 recorded the OpenCPI side as unspecified.

**RADHAWK** (the OpenCPI project, `api/radhawk.h`) is that missing OpenCPI side,
and it already has exactly the shape needed:

- a **plain C ABI** over the C++ ACI, so it is callable without a C++ toolchain;
- a **session API** — `rh_start`, `rh_set_params`, `rh_get_status`, `rh_stop` —
  which is start-once-then-retune, not run-and-exit;
- property writes through **`Application::setProperty()`**, OpenCPI's own
  mechanism, type-checked by the framework against the worker's property XML;
- validation that already encodes this project's coupling rules (scrambler ==
  descrambler, occupied bandwidth below 11 MHz, `iq_mode` 0..3);
- a cross-process **run lock**, because an OpenCPI application owns the PL
  container exclusively;
- a **`dry_run`** mode that validates and reports without touching hardware.

So the work was to connect them, not to build either half.

---

## 2. The constraint that decided the architecture

The obvious design — radiod loads a `libradhawk.so` — does not work with this
CDK, and it is worth recording exactly why, because it is not obvious and it is
not a preference.

The OpenCPI runtime archives are compiled **without `-fPIC`**:

```
$ g++ -shared -o libradhawk.so radhawk.o \
      $CDK/ubuntu18_04/lib/libocpi_application.a ...
/usr/bin/ld: libocpi_application.a(...Application.o): relocation R_X86_64_PC32
    against symbol '_ZNK4OCPI4Util9TokenIter5tokenEv' can not be used when
    making a shared object; recompile with -fPIC
/usr/bin/ld: final link failed: Bad value
```

Only the driver plug-ins (`libocpi_*_s.so`) are shared; the core ACI is static
and non-PIC. **The ACI links into an executable, statically, and nothing else.**

That leaves two options, and the second is better on every axis:

| | link the ACI into radiod | a helper executable |
|---|---|---|
| radiod's language | becomes C++-linked | stays C11, `-Wstrict-prototypes` |
| radiod's build | needs `OCPI_CDK_DIR` at configure time; 16 MB static binary; **unbuildable** where no CDK exists | unchanged — no new dependency at all |
| threading (U-12) | the ACI shares the control loop's process | the ACI has its own process |
| an ACI crash (U-04) | takes down the control plane | takes down the agent; radiod reports it |
| build knowledge | the ACI link order duplicated in CMake | OpenCPI's own `application.mk` |

So the OpenCPI side is a helper process, **rh_agent**, and that boundary
resolves three items `unknown.md` had open — U-11 (C versus C++ at the ACI),
U-12 (threading model for PL ownership) and part of U-04 (ACI error
containment) — rather than working around them.

**The single-owner rule is intact.** radiod spawns exactly one agent, is the
only process holding its pipes, and kills it on exit. RADHAWK's own advisory
lock on `/tmp/radhawk.lock` enforces exclusivity a second time, below radiod,
so nothing depends on convention.

---

## 3. The architecture

```
  operator                              test automation
      │                                        │
      ├── radiod -i  (console on this terminal) │
      └── radioctl ────────────────────────────┘
                    │
                    │  librc  →  rc protocol, one line per message,
                    │            over a UNIX domain socket
                    ▼
               ┌─────────────────────────────────────────────┐
               │  radiod                                     │
               │    lifecycle state machine, fault registry   │
               │                                             │
               │    hh_radio_ops_t        ── mock | opencpi   │
               │    hh_radiod_props_ops_t ── the property     │
               │                             surface, if the  │
               │                             backend has one  │
               └─────────────────────────────────────────────┘
                    │  pipe pair, 9-verb line protocol
                    │  (radiod/include/hhsdr/radiod/opencpi_agent_proto.h)
                    ▼
               ┌─────────────────────────────────────────────┐
               │  rh_agent    (OpenCPI project,               │
               │               applications/rh_agent/)        │
               │    owns the property registry                │
               │    holds the RADHAWK run lock while running   │
               └─────────────────────────────────────────────┘
                    │  rh_* C ABI
                    ▼
               libradhawk  →  OpenCPI ACI  →  qpsk_tx / qpsk_rx / AD9361
```

Radio-specific behaviour stays in radiod; OpenCPI-specific behaviour stays in
the agent. Nothing in radiod includes an OpenCPI header or references an
OpenCPI symbol, and nothing in the agent knows about sockets, clients or the
MANET stack.

### The rule that makes it survive the next worker

**radiod contains no worker name, property name, property type or range.**

The agent publishes its registry once, at startup, in reply to `props`. radiod
stores it, serves `opencpi list` from it, and thereafter only routes requests
and enforces the access class the agent declared. It supplies exactly one thing
the agent cannot know for itself — whether the application is currently running.

So **adding a property is one row in `rh_agent.cc`.** No radiod change, no
protocol change, no client change. The test suite proves this holds: the stub
agent in `radiod/tests/stub_agent.c` publishes workers called `w1` and `w2`, and
every assertion passes without `qpsk_tx` existing anywhere.

---

## 4. Property handling

### Access classes

Transcribed from what `radhawk.h` states, which in turn records what the
hardware tolerates — not chosen here:

| class | source in RADHAWK | meaning |
|---|---|---|
| `rw` | in `rh_params_t` | Writable OpenCPI property, changeable while running. The HDL latches the framing-sensitive ones (scrambler, descrambler, `mod_select`) on a frame boundary, so a write landing mid-frame cannot split a frame between two interpretations. |
| `rw-start` | in `rh_config_t` but **not** `rh_params_t` | Cannot change while running. `sym_mode` must agree with the AD9361 clock chain, so changing it alone starves or overruns the data port; every AD9361 key is consumed by `ad9361_init`, which stops `DATA_CLK` — the fabric's only clock — when it re-runs. |
| `ro` | in `rh_result_t` | A counter or flag sampled out of the PL. |

radiod enforces one rule from these, and it is a **refusal, never a queue**:

```
radiod> opencpi set qpsk_tx sym_mode 1        # while running
error: opencpi_set rejected (ESTATE): 'qpsk_tx.sym_mode' can only be set while
       the application is stopped; run `stop`, set it, then `start`
```

Silently deferring that write would leave the operator believing a change had
taken effect. A read-only property read before `start` is refused for the
matching reason — a stale or zeroed counter reported as current is how a dead
link reads healthy.

### Where a value is validated, and by whom

| check | who | example |
|---|---|---|
| well-formed command | `rc_cli.c` | a value containing a space is refused rather than truncated on the wire |
| name exists | radiod, from the published registry | `ENOTFOUND: no such property 'qpsk_tx.wibble'` |
| access class vs. state | radiod | the `ESTATE` refusal above |
| is an integer | the agent, with `strtoll`'s end pointer checked | `16384x` is refused, **not** read as `16384`, and `abc` is not read as `0` |
| range and coupling | RADHAWK's own `rh_config_validate` / `rh_params_validate` | `rrc_backoff 99999 out of range 0..65535`; `enable_scrambler must equal enable_descrambler` |
| property type | the OpenCPI framework, in `setProperty()` | against the worker's property XML |

Range and coupling knowledge is **not duplicated** in radiod. RADHAWK's message
names the parameter and the reason, and it is passed through to the operator
unaltered rather than flattened into a status code.

### Staged versus live

| radiod state | what a write does | what it says |
|---|---|---|
| not `RUNNING` | staged into the configuration the next `start` will carry (`cfg` → `rh_config_t`) | `detail=staged for the next start` |
| `RUNNING` | applied immediately (`set` → `rh_set_params`) | `detail=applied to the running application` |

A rejected write leaves the staged configuration exactly as it was; the agent
stages into a copy and restores it if RADHAWK refuses the result.

A writable property that nothing has set reads back as **`keep`**, not `0`, so
"nobody chose a value" stays distinguishable from "the value is zero". That
mirrors RADHAWK's `RH_KEEP`, which exists for the same reason.

---

## 5. Commands

Two front ends, one grammar. `protocol/src/rc_cli.c` is the single translator,
so a command means the same thing wherever it is typed.

### Existing verbs — unchanged

```
init   configure <node_id> [ch]   start   stop   status   stats
set-channel <ch>   inject-fault <kind>   clear-fault   shutdown
```

`start` and `stop` now deploy and release the **OpenCPI application** when the
backend is `opencpi`; `status` reports whether it is running.

### New verbs

```
opencpi list                       the workers the backend publishes
opencpi list <worker>              that worker's properties, with type and access
opencpi get  <worker> <property>
opencpi set  <worker> <property> <value>
opencpi status                     every readable property, in one sample
opencpi start <worker>             shorthand for: opencpi set <worker> enable 1
opencpi stop  <worker>             shorthand for: opencpi set <worker> enable 0
```

`start`/`stop` bring the whole application up and down. `opencpi start/stop
<worker>` runs or halts one worker inside it, through that worker's own
`enable` property — a real Writable property on both `qpsk_tx` and `qpsk_rx`,
not an invented operation.

On the wire these are four verbs, not seven, and they carry an opaque
`(worker, property, value)` triple. `opencpi status` samples everything in one
exchange on purpose: reading counters one at a time would return them from
different instants, and a word-count comparison across two instants is exactly
the acceptance oracle this project depends on.

---

## 6. Build

### radiod, radioctl, librc — no new dependency

```bash
cd hh-sdr-manet
mkdir -p build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Debug
make -j8
ctest
```

Unchanged. `hhsdr_radiod` gained `src/backends/opencpi_backend.c`, which
includes no OpenCPI header and references no OpenCPI symbol, so this builds
identically on a host with no CDK. The only new source-level dependency anywhere
is `-ldl`-free: there is none.

### rh_agent — built by OpenCPI, in the OpenCPI project

```bash
cd <opencpi-project>
source ./setup-env.sh                    # sets OCPI_CDK_DIR and OCPI_XILINX_DIR

cd applications/rh_agent
make                                     # host  -> target-ubuntu18_04/rh_agent
make RccPlatforms=xilinx19_2_aarch32        # board -> target-xilinx19_2_aarch32/rh_agent
```

`applications/rh_agent/Makefile` is two lines of content plus
`include $(OCPI_CDK_DIR)/include/application.mk` — the same build every other
ACI program in that project uses, so the ACI include path, the static link order
of `libocpi_*.a` and the `makeStaticWorker.sh` post-link step are OpenCPI's
business and are not reproduced by hand anywhere.

`radhawk.cc` and `radhawk.h` in that directory are **symlinks** to `../../api/`,
matching the convention the other `rh_*` applications already use, so the agent
cannot drift from the published RADHAWK API.

It compiles against two header-only files from this repository, for the wire
verbs and the access-class vocabulary. Override their location if the two
checkouts are not siblings:

```bash
make HH_SDR_MANET=/path/to/hh-sdr-manet
```

---

## 7. Running it

### With the real radio, on the board

The boot order is **mandatory and unchanged** — the AD9361 supplies the fabric's
only clock, so it must be initialised before the container is under OpenCPI
control. RADHAWK's `rh_start` runs `ad9361_init` itself when any radio key is
set, which is why the AD9361 keys are `rw-start`: they have to be chosen before
`start`, not after.

```bash
# 1. start the daemon (console attached, so there is a prompt)
./radiod -b opencpi -a /opt/txrx_worker_rt_ch2_dma/rh_agent -i

# or as a daemon, with a configuration file, and drive it with radioctl:
./radiod -c /etc/radiod.conf
```

```
radiod> opencpi list
count=4
app
qpsk_tx
qpsk_rx
radio

radiod> opencpi list qpsk_tx
worker=qpsk_tx
count=11
enable                 bool       rw
src_sel                bool       rw
sym_mode               bool       rw-start
enable_scrambler       bool       rw
mod_select             uchar      rw
rrc_backoff            ushort     rw
dbg_in_words           ulonglong  ro
...

radiod> opencpi set app xml qpsk_dma_null_app.xml
radiod> opencpi set radio tx_lo_hz 225000000
radiod> opencpi set radio rx_lo_hz 225000000
radiod> opencpi set radio tx_atten_mdB 10000
radiod> opencpi set qpsk_tx sym_mode 0
radiod> init
radiod> configure 1
radiod> start                          # deploys the application; may take
                                       # seconds, ad9361_init runs here
radiod> status
state=running
operational=1

radiod> opencpi set qpsk_tx rrc_backoff 16384    # live, no restart
radiod> opencpi get qpsk_rx err_count
radiod> opencpi status
radiod> opencpi stop qpsk_tx                     # halt one worker
radiod> stop                                     # release the application
radiod> quit
```

The same session with `radioctl`, one command per invocation:

```bash
radioctl opencpi set radio tx_lo_hz 225000000
radioctl init && radioctl configure 1 && radioctl start
radioctl opencpi set qpsk_tx rrc_backoff 16384
radioctl opencpi status
```

or interactively on one connection:

```bash
radioctl shell
```

### Without hardware

`app.dry_run` makes `rh_start` validate everything and report every property it
*would* set, without touching the FPGA. Useful on a development host:

```
radiod> opencpi set app dry_run 1
radiod> init
radiod> configure 1
radiod> start
radiod> opencpi status
```

---

## 8. Limitations, stated plainly

**Properties that cannot be changed while running.** Every `rw-start` property:
`qpsk_tx.sym_mode`, all eight `radio.*` keys, and all six `app.*` settings.
`opencpi list <worker>` marks each one. Writing one while the application runs is
refused with the reason.

**`set-channel` returns `EUNSUPPORTED`.** No logical-channel-to-RF mapping exists
for this radio (`unknown.md` U-14), and inventing a channel plan would be worse
than refusing. Retuning is reached explicitly with
`opencpi set radio tx_lo_hz <Hz>` — which, being an AD9361 key, requires a stop
and start.

**`transmit`, `set_rx_callback` and `get_link_metrics` return
`NOT_IMPLEMENTED`.** `rc.h` excludes HTI-03/04/05 from the control contract
because they are data-plane, high-rate interfaces, and this application's
payload path is DMA (`DMA-PORTING-GUIDE.md`), not request/response IPC.

**`hh_radio_status_t` fields that read zero.** `frames_tx`, `frames_rx`,
`channel` and `waveform_id`. This application counts words, samples and bits —
there is no frame counter anywhere in the chain. A frame count *could* be divided
out of `dbg_in_words` (the frame is 2240 bits = 70 words) and deliberately is
not: a derived number presented as a measured one is how a partially-working link
gets mistaken for a healthy one. Every real counter is in `opencpi status`.
`opencpi.h` documents each mapping and each deliberate zero.

**`frequency_hz` is the requested LO, not a read-back.** RADHAWK hands LO
settings to `ad9361_init` and does not read the synthesiser back, exactly as
`ad9361_proxy`'s own properties report what was requested at runtime.

**Every agent call is synchronous** and blocks radiod's control loop until it
answers or times out. `opencpi_reply_timeout_ms` (3 s) and
`opencpi_start_timeout_ms` (60 s) bound that. A timed-out agent is dropped, since
its state is then unknown.

**`mod_select = 1` will not lock.** Unchanged and not a fault of this work: the
supplied MATLAB modulator's mode 1 is degenerate (`Q := I`), so the QPSK receiver
never synchronises. See the OpenCPI project's `CLAUDE.md` §15.2.

**Not run on hardware.** Everything here is verified on the host: 36 CTest cases
including an end-to-end run of the real `radiod` binary against a stub agent, and
a manual end-to-end run of the real `rh_agent` in `dry_run` mode. The agent is
built for both `ubuntu18_04` and `xilinx19_2_aarch32`, but no part of this has
driven a PL.

---

## 9. Where things live

| file | what it is |
|---|---|
| `protocol/include/hhsdr/protocol/rc.h` | four new verbs, the request/response fields they carry |
| `protocol/src/rc.c` | their codec |
| `protocol/include/hhsdr/protocol/rc_cli.h`, `src/rc_cli.c` | **new** — the shared command grammar and output layout |
| `radiod/include/hhsdr/radiod/props.h` | **new** — the generic property vtable; why radiod knows no names |
| `radiod/include/hhsdr/radiod/opencpi_agent_proto.h` | **new** — the agent wire contract, and the linker evidence for it |
| `radiod/include/hhsdr/radiod/opencpi.h` | **new** — the backend, and every `hh_radio_status_t` mapping |
| `radiod/src/backends/opencpi_backend.c` | **new** — agent supervision, registry, access enforcement |
| `radiod/src/radiod.c` | verb routing; the optional interactive console |
| `radiod/radiod_main.c` | backend selection (`-b`), agent path (`-a`), console (`-i`) |
| `radiod/src/config.c` | `radio_backend`, `opencpi_agent`, the two timeouts |
| `radioctl/radioctl_main.c` | new subcommands via the shared grammar; `radioctl shell` |
| `radiod/tests/stub_agent.c` | **new, test only** — a deterministic agent |
| `radiod/tests/test_radiod_props.c` | **new** — radiod's routing, including the unavailable path |
| `radiod/tests/test_radiod_opencpi.c` | **new** — the backend, and end to end through the daemon |
| `protocol/tests/test_rc_cli.c` | **new** — the grammar and the new codec payloads |
| *(OpenCPI project)* `applications/rh_agent/` | **new** — the agent, built by `application.mk` |
