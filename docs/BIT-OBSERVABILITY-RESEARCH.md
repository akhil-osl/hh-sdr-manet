# BIT, ATP Tooling and Observability — Research and Recommended Approach

**Task:** HH-SDR Task 2 — research BIT methodologies, ATP tooling, network
logging, monitoring and observability approaches applicable to the MANET/radio
system; identify relevant methods and tools; document a recommended approach.

**Date:** 2026-09-28 · **Status:** research, for review by the system architect
and the test lead · **Target architecture:** OSL-628-BD-101 Rev 00

**Governing rule.** Nothing unspecified is invented (see [`../CLAUDE.md`](../CLAUDE.md) §2).
This document names no fault codes, thresholds, register maps, TLV IDs or
footprint figures that are not in a cited source. Where something is unknown it
says so and cites the gap in [`../unknown.md`](../unknown.md) (U-01 … U-19).

**Baseline note.** This was written against the working tree of 2026-09-28,
which contains uncommitted work: a radiod OpenCPI backend
(`radiod/src/backends/ocpi_backend.cpp`, built with `-DHH_WITH_OPENCPI=ON`,
host-tested), U-03/U-04/U-11 moved to PARTIAL, and new entries U-17 (which
OpenCPI project is the baseline), U-18 (waveform/MAC gap) and U-19 (board
bring-up vs single owner). If that work changes before it is committed, §2.1,
§4 and §8.3 need re-checking.
Every statement is labelled with where it comes from:

| Tag | Meaning |
|---|---|
| **[REPO]** | exists in this repository today (path given) |
| **[OCPI]** | exists in the OpenCPI radio project today (`opencpi_project/txrx_worker_rt_ch2_dma_Mishra_c/`, read-only reference) |
| **[EXT]** | external standard, datasheet or tool (reference number in §10) |
| **[REC]** | recommended by this document; not yet built |
| **[BLOCKED U-xx]** | cannot be done until that gap is closed |

A caution on **[OCPI]**. The OpenCPI project is a QPSK/PRBS bring-up waveform on
the same board class. It is **not** the HH-SDR MANET waveform; the HH-SDR worker
set (`waveform_ctrl`, `drc`, `mac_ps`, `ad9361_config_proxy`, `telemetry`) is
still unspecified (U-03), which project is the integration baseline is open
(U-17), and the QPSK chain has no MAC, no CRC and no per-frame RSSI/SNR
(U-18). Its counters and methods are cited here as proven patterns and as
board facts, not as the HH-SDR contract.

---

## 1. Purpose and scope

### 1.1 Terms as used here

| Term | Meaning in this project |
|---|---|
| **BIT** (Built-In Test) | Tests the radio runs on itself, with no external equipment, in the field. Three kinds: **PBIT** (power-on), **IBIT** (initiated by operator or maintainer, may interrupt service), **CBIT** (continuous or periodic, must not interrupt service). |
| **ATP** (Acceptance Test Procedure) | A documented test run against a unit or a network, with limits, producing evidence for acceptance. Uses external equipment where needed (peer nodes, traffic generators, spectrum analyser). |
| **Observability** | Everything that lets a person or a program see what the running system is doing: logs, counters, status, packet captures, events. BIT and ATP both consume it. |

### 1.2 The three consumers

| Consumer | Needs | Timescale | Where |
|---|---|---|---|
| **Operator** (HMI, field) | "Is my radio working? Am I in the net? Who can I reach?" Go/no-go, not counters. | seconds | on the unit |
| **Test / ATP** (lab, acceptance) | Repeatable measured numbers against written limits, with raw evidence kept. | minutes | bench or test range |
| **Maintenance** (depot, post-mission) | What failed, when, in what order; which replaceable unit. Full logs and fault history. | after the fact | log download |

A fourth, conditional consumer is a **network management station (NMS)** reaching
nodes over the mesh. Whether one exists, and which protocol it uses, is an open
decision (§9, Q6).

### 1.3 Out of scope

Waveform and DSP verification (BER curves, spectrum masks) are hardware-team
work in the OpenCPI project; this document only covers how their results would
reach BIT and evidence records. RF safety interlocks are PL work (U-08); this
document only records what software must do when it cannot guarantee them.

---

## 2. What exists today

### 2.1 In this repository [REPO]

| Item | Location | What it gives | Limits |
|---|---|---|---|
| ATP/BIT scripts | [`tools/atp/`](../tools/atp/README.md) | `atp-iperf3.sh`, `atp-mgen.sh`, `atp-capture.sh`, `atp-decode.sh`, `atp-radioctl.sh` (`health` = read-only BIT, `lifecycle` = ATP) | Validated on network namespaces and the mock backend only |
| Evidence schema | `tools/atp/README.md` "Evidence records" | JSON `hh-atp-evidence/1`: `check`, `result`, `reason`, UTC start/finish, `time_source`, `host`, `params`, `metrics`, `thresholds`, `artifacts` | `time_source` is always `"system"` (U-07) |
| Five results | same | `pass` 0 · `fail` 1 · `error` 3 · `blocked` 4 · `unjudged` 5 (exit 2 = usage) | Only `pass` is a pass |
| Script self-tests | `tools/atp/selftest/`, `tools/atp/wireshark/check-dissector.sh` | Prove scripts fail on an impaired link and report `blocked`/`unjudged`/`error` honestly | `netns-selftest.sh` not in ctest (moves real traffic) |
| MANET dissector | `tools/atp/wireshark/hh_manet.lua` | Decodes beacon and routing-update payloads, checked against the C codec | No on-radio capture point (U-16) |
| radiod state machine | [`radiod/README.md`](../radiod/README.md) | 7 states incl. `faulted`; automatic `faulted` on failed `open` | — |
| radiod fault registry | `radiod/include/hhsdr/radiod/events.h` | Per kind: `active`, `count`, `first_seen`, `last_seen`, `cleared_at`; `generation`, `asserted_total`, `cleared_total`. Kinds: `none`, `tx_failure`, `rx_silence`, `hw_fault`, `backend_io` | **In-process only; not on the wire** (U-01, U-05). No severity, no hardware fault list (U-05) |
| `radioctl status` / `stats` | [`radioctl/README.md`](../radioctl/README.md) | `state`, `operational`, `channel`, `frequency_hz`, `waveform_id`; `frames_tx/rx`, `tx_errors`, `rx_errors`, `requests_total`, `requests_rejected` | Counters cumulative since radiod start; no faults command; no timeout |
| radioctl exit codes | same | 0 ok · 2 usage · 3 transport (radiod unreachable) · 4 rejected | 3 vs 4 is exactly the "radio gone" vs "wrong state" split BIT needs |
| radiod OpenCPI backend (uncommitted, host-tested) | `radiod/include/hhsdr/radiod/ocpi_backend.h`, `radiod/src/backends/ocpi_backend.cpp` | Owns one OpenCPI application via the ACI: create → initialize → setProperty → start; `poll` notices the app finishing; `get_status.operational` = app created, started, not finished. Application path and property values come from configuration | **Reads no status properties**; frame/error counters stay zero because no property is defined as their source. Board bring-up (`ad9361_init`, radio-setup app) not owned (U-19). Board operation waits on U-17, U-19 |
| Fault injection | `radioctl inject-fault <kind>` / `clear-fault` | Lets a BIT or ATP script prove its own detection path | `clear-fault` clears all kinds |
| Structured log | [`include/hhsdr/core/log.h`](../include/hhsdr/core/log.h), `src/core/log.c` | `lvl=… comp=… event=… key=value…`, one line, ≤ 512 bytes, pluggable sink | **No timestamp and no sequence number in the record**; default sink is stderr |
| MANET telemetry | [`include/hhsdr/manet/telemetry.h`](../include/hhsdr/manet/telemetry.h) | `hh_node_status_t`: neighbours, routes, partition, beacon counts, failures, recoveries, forwarder drops, `events_dropped`, radio availability | Only dumped at `hh-manet` exit (`src/main.c:117`); `hh-manet` exits at start-up without a hardware backend, so none of this is observable on the target today |
| SCA `runTest` | [`include/hhsdr/sca/resource.h`](../include/hhsdr/sca/resource.h), `src/sca/resource.c:320` | Test id 1 (`HH_SCA_TEST_SELF_CHECK`) returns `component=… lifecycle=… result=pass`; any other id → `UnknownTest` (`HH_ERR_NOTFOUND`) | A lifecycle self-report, not a hardware test. No ORB, so not callable remotely ([`SCA-COMPATIBILITY.md`](SCA-COMPATIBILITY.md)) |
| HTI-01 Node Status | [`HW-SW Interface.md`](HW-SW%20Interface.md) §6 | Required: `node_id` + per-component health, periodic and on change | Refresh interval and encoding "implementation-defined" |
| Radio status contract | [`MANET-RADIO-REQUIREMENTS.md`](MANET-RADIO-REQUIREMENTS.md) | `get_status` must report `operational` honestly; per-frame RSSI/SNR/PER/phy_errors with `_valid` flags | Units and availability are open items 2–6 in that file |
| Deployment design | [`DEPLOYMENT-ARCHITECTURE.md`](DEPLOYMENT-ARCHITECTURE.md) §7 | Assumes systemd; logs to journal via stderr | Conflicts with the board image — see §2.3 |

### 2.2 In the OpenCPI radio project [OCPI]

Observable properties declared in `components/specs/*.xml` (all `Volatile`,
i.e. read-only status):

| Worker | Status properties | BIT meaning (per that project's docs) |
|---|---|---|
| `ad9361_proxy` (RCC) | `init_done` | AD9361 initialisation completed |
| `qpsk_rx` (HDL) | `in_sync`, `nsynced`, `bit_count`, `err_count`, `overflow`, `word_count`, `dbg_sample_count`, `dbg_out_words`, `coarse_rstcs`, `loop_filter_rst`, `norm_coarse_freq_est` | Acquisition, in-fabric PRBS BER, RX overflow, consumer word count |
| `qpsk_tx` (HDL) | `dbg_word_count`, `dbg_sym_count`, `dbg_out_samples`, `dbg_out_dropped`, `dbg_in_words`, `dbg_in_underrun` | Producer word count, PS starvation (underrun), output drops |
| `qpsk_ctrl` (RCC) | `poll_count`, `in_sync`, `bit_count`, `err_count`, `ber_e9`, `descrambler_in_force`, `tx_sym_count`, `rx_sample_count` | `poll_count` is the liveness signal: it freezes when the app dies, the mirrored `dbg_*` counts do not (CLAUDE.md §15.5, trap 11). **Not present in the DMA app** (`qpsk_dma_app.xml`) |
| `ocpi.core` `file_read` / `file_write` | `bytesRead`, `bytesWritten` | PS-side source/sink progress (read by `dma_stream`) |

Methods and facts established there:

| Fact | Source | Consequence for BIT |
|---|---|---|
| **Producer word count == consumer word count is the only trustworthy data-flow proof.** In-fabric BER and spectrum both false-pass when the source starves (the last word repeats and is whitened). | `DMA-PORTING-GUIDE.md` §7; CLAUDE.md §15A | A data-path BIT must compare counts first and only then trust BER. |
| Loopback BER cannot judge TX: LVDS bit-map errors cancel end-to-end (BER 1.8×10⁻⁴ measured on white-noise output). | CLAUDE.md §9, trap 1 | RF-path BIT cannot be a simple loopback BER. |
| The automated spectrum shoulder metric false-passes an aliased emission. | CLAUDE.md §9, trap 2 | Any automated RF limit needs a check that its input is valid. |
| ACI oracle: `applications/dma_stream/dma_stream.cc` reads `dbg_in_words`, `dbg_in_underrun`, `dbg_out_dropped`, `dbg_out_words`, `overflow`, `in_sync`, `bit_count`, `err_count`, `bytesRead`, `bytesWritten` every 10 s via `OA::Application::getProperty` | source file | The radiod OpenCPI backend already follows its open sequence; the status-read half is the part not yet adopted. C++ is now allowed in that backend only (U-11). |
| `ocpihdl` segfaults after a DMA app has run; on an unclocked fabric `ocpihdl search`/`ocpirun` is a **hard CPU hang**. | CLAUDE.md trap 31 | BIT must never probe the PL from a side process; reads go through the one ACI owner. |
| `ad9361_init` must run before anything touches the PL. `ad9361_proxy` must not be in a DMA app (its re-init stops the fabric clock). | CLAUDE.md traps 30–31 | PBIT ordering is fixed; AD9361 queries during service must go through whoever owns the AD9361. |
| **Killing the application does not stop the RF carrier** — the PL free-runs; carrier off needs a bitstream reload or power-cycle. | `DMA-PORTING-GUIDE.md` §8 item 4 | Software death is not a safe state (§9, Q4). |
| Target checks: `/sys/class/fpga_manager/fpga0/state` = `operating`; bitstream md5 vs artifacts (UUID match); `ocpidriver status`; `MemTotal` = 119540 kB (reserved DMA block active); `dmesg` oops/bad-page count | CLAUDE.md §8, §15A; `board_dma.sh`; `qa/v5_board_run.sh` | Ready-made, read-only PBIT candidates. |
| AD9361 TX BIST tone used for fault isolation (TX analog chain vs port data) via a raw register override in `ad9361_config.txt` | `board_bist.txt` | Proven IBIT technique; out of service only. |
| The linked ADI no-OS headers declare `ad9361_get_temperature`, `ad9361_get_rx_rssi`, `ad9361_get_tx_rssi` | `local/ad9361_include/ad9361_api.h` | Available in the library; **not called** by `ad9361_proxy` and **not exposed** as properties. |

### 2.3 Board image facts relevant to logging and tooling [OCPI]

Read from the PetaLinux rootfs archive
`deploy_dma_old_arch_ch2/rootfs/p2_base.tar.gz` and the device tree
`deploy/boot_reserved_dma/system.dts`. This is the OpenCPI project's image; the
HH-SDR production image may differ, but nothing else describes the board yet.

| Fact | Evidence | Consequence |
|---|---|---|
| Dual Cortex-A9 (XC7Z100), AD9361, ~117 MB usable RAM (`MemTotal` 119540 kB, 128 MB reserved for DMA) | `system.dts` `cpu@0/1 arm,cortex-a9`; START-HERE-CLAUDE.md | Tight memory budget for any agent/exporter |
| **Init is BusyBox sysvinit, not systemd** (`/sbin/init`, `/etc/inittab`, `/etc/init.d/*`) | rootfs listing | [`DEPLOYMENT-ARCHITECTURE.md`](DEPLOYMENT-ARCHITECTURE.md) §7 (systemd, journal) does not match this image |
| Logging is BusyBox `syslogd` + `klogd`, destination `file` → `/var/log/messages`, 64 KiB buffer, rotation commented out | `/etc/syslog-startup.conf` | No journal. stderr of a daemon is **not** captured automatically under sysvinit |
| `/var/log` → `/var/volatile/log`, and `/var/volatile` is **tmpfs** | `/etc/default/volatiles/00_core`, `/etc/fstab` | **All logs are lost at reboot or power loss.** No post-mission log exists today |
| BusyBox applets present include `syslogd`, `logread`, `logger`, `klogd`, `watchdog`, `start-stop-daemon`, `nc`, `httpd`, `ip`, `netstat`, `devmem` | `/etc/busybox.links.*` | Enough for a minimal logging/BIT runtime |
| Applets **absent**: `timeout`, `crond`, `pgrep`, `setsid`; `pkill -f` is a silent no-op | busybox links; OpenCPI scripts' comments | `tools/atp` scripts require `timeout`; periodic BIT cannot use cron |
| Not in the image: `jq`, `python3`, `tcpdump`, `iperf3`, `mgen`, `bash` | rootfs listing | `tools/atp` cannot run on this image as-is (it needs `jq`, `timeout`, and the traffic tools) |
| `dropbear` (SSH) present | `/usr/sbin/dropbear` | Maintenance log download path exists |
| Device tree enables Zynq XADC (`xlnx,zynq-xadc-1.00.a`) and the Cadence watchdog (`cdns,wdt-r1p2`, `timeout-sec = 10`) | `system.dts` | Die temperature/voltages and a hardware watchdog are candidates; kernel driver presence **not verified** |
| No RTC node found in the device tree | `system.dts` strings | Wall-clock time at boot is not trustworthy (see §7.4) |

---

## 3. BIT methodology survey

### 3.1 BIT types

| Type | When it runs | May interrupt service? | Typical content | Applicability here |
|---|---|---|---|---|
| **PBIT** | Once at power-on / start | Yes (radio not yet in service) | Configuration integrity, device presence, clock/lock, memory, self-loopback | High. The OpenCPI bring-up order (§2.2) is already a PBIT sequence, done by hand. |
| **IBIT** | On command | Yes (operator accepts outage) | Deeper tests: RF BIST tone/loopback, full data-path oracle, fault-injection self-check | High, but only when out of service. `atp-radioctl.sh lifecycle` is an IBIT shape today. |
| **CBIT** | Continuously or periodically | No | Counter monitoring, liveness, temperature/voltage, link health | High. `atp-radioctl.sh health` is a CBIT shape today; MANET link-health fusion is already a CBIT of the network. |

These three categories are standard terminology in the DoD testability
literature ([R1], [R2]); the terms themselves are defined in MIL-STD-1309 [R3].
The categories are used here as a vocabulary, not as a compliance claim: no
governing BIT standard has been named for HH-SDR (§9, Q1).

### 3.2 Metrics

| Metric | Meaning | How it would be computed here |
|---|---|---|
| **FD%** (fault detection) | Share of failures (normally weighted by failure rate) that BIT detects | Needs a failure-mode list with rates (an FMECA). None exists — **U-05** covers the fault list; failure rates are not in any repository |
| **FI%** (fault isolation) | Share of detected failures isolated to ≤ N replaceable units | Needs the replaceable-unit breakdown. Not defined anywhere in either repository |
| **FAR** (false alarm rate) | BIT failure indications with no confirmed fault, per operating hour or per test | Measurable once BIT runs on hardware; needs a logging and confirmation process |
| **Detection latency** | Time from fault onset to BIT report | Measurable in simulation now (virtual clock) and on hardware later |

MIL-HDBK-2165 frames these as requirements to be set and then predicted and
assessed ([R1]; it covers BIT fault coverage, BIT fault isolation and a maximum
BIT false-alarm rate). The exact wording and target values must come from the
governing specification. **This document sets no FD/FI/FAR targets** (§9, Q2).

Practical note on FAR for this radio: the OpenCPI experience (§2.2) shows the
opposite risk as well — **false passes** (starved source looks healthy;
loopback hides TX errors; spectrum metric passes an alias). Every BIT check
below is therefore paired with a validity precondition.

### 3.3 Standards and guidance — what each actually is

| Reference | What it is | Relevance |
|---|---|---|
| **MIL-HDBK-2165** (31 Jul 1995; supersedes MIL-STD-2165A; reclassified as handbook) [R1] | DoD testability handbook: testability programme planning, diagnostic concept, BIT requirements, testability design and assessment, reviews | The natural frame for FD/FI/FAR requirements if the customer is DoD-style. Guidance, not a test method. |
| **MIL-STD-1309** (rev. D, 12 Feb 1992) [R3] | Definitions of terms for testing, measurement and diagnostics | Vocabulary only |
| **IEEE Std 1232** (AI-ESTATE; 1232-2010, also IEC 62243:2012) [R4] | Exchange formats and service interfaces for diagnostic reasoners and diagnostic knowledge | Only relevant if a formal diagnostic model / reasoner is required. Heavy for this project; not recommended unless mandated. |
| **IEEE Std 1149.1** (JTAG boundary scan) [R5] | Structural board-level test of interconnects via a scan chain | Production/depot test of the PCB, not in-field BIT. The Zynq JTAG port is also the Vivado debug port. |
| **SCA 2.2.2 `CF::TestableObject::runTest(testId, testValues)`** [R6] | Black-box test hook on every Resource/Device; raises `UnknownTest` for an unknown id, `UnknownProperties` for bad inputs | The SCA-side BIT entry point. The repo implements its shape (§2.1). |
| **SCA 4.1 `TestableInterface`** [R7] | Renamed from TestableObject; requirements SCA19 (testId selects the test), SCA21 (results returned in testValues), SCA23 (`UnknownTest`), SCA24 (`UnknownProperties`); SCA546 (BaseComponent realises TestableInterface); test properties have kindtype `test` in the descriptor | If SCA 4.1 is the target, BIT tests become descriptor-declared `test` properties — which fits a "one BIT check = one id" design. |

---

## 4. Candidate BIT test inventory

Only data sources that exist today are named. "Status" says what is needed to
run the check on the radio.

- **A** = available now (repo, mock or off-radio)
- **B** = available on the board now by shell, read-only, outside radiod
- **C** = needs the radiod OpenCPI backend on the board, extended to read
  status properties. The backend exists (host-tested) but reads none today;
  board operation waits on U-17 and U-19. Property names would come from
  configuration, as the backend already does for writes — the HH-SDR names
  themselves are U-03
- **D** = needs a hardware/interface contract (U-xx named)

### 4.1 Platform and configuration (PBIT)

| # | Test | Type | Detects | Data source | Status |
|---|---|---|---|---|---|
| P1 | PL configured | PBIT, CBIT | Bitstream not loaded / FPGA manager error | `/sys/class/fpga_manager/fpga0/state` = `operating` [OCPI §8] | B |
| P2 | Bitstream ↔ artifact identity | PBIT | Mismatched builds on the two partitions (UUID mismatch) | md5 of `*.bin` vs `*.bitz`, or OpenCPI UUID [OCPI §8] | B (check method); the HH-SDR artifact names are U-03 |
| P3 | OpenCPI driver loaded | PBIT | Kernel driver absent | `ocpidriver status` [OCPI §8] | B — confirm it does not touch the PL before `ad9361_init` |
| P4 | DMA memory reservation active | PBIT | Wrong dtb; DMA allocation will fail | `MemTotal` vs the value for the deployed dtb (119540 kB on the OpenCPI image) | B; the expected value is image-specific |
| P5 | Kernel health | PBIT, CBIT | Oops / bad-page after driver load | `dmesg` count of `oops`/`bad page` [OCPI `board_dma.sh`] | B |
| P6 | AD9361 initialised | PBIT | SPI/init failure | `ad9361_proxy.init_done`; `ad9361_init` exit status | C (property read via ACI); who runs `ad9361_init` is U-19; HH-SDR worker name is U-03 |
| P7 | radiod up and lifecycle legal | PBIT | Daemon not running; start failed → `faulted` | `radioctl status` exit 0/3, `state`, `operational` | A (mock); unchanged on hardware |
| P8 | radiod configuration valid | PBIT | Bad config key | radiod exits 1 with line number | A |
| P9 | Software self-check | PBIT | Component lifecycle wrong | SCA `runTest(1)` per component | A (in-process only; no remote caller) |
| P10 | Image / version inventory | PBIT | Wrong software set | md5 manifest, as `qa/BUILD-MANIFEST.sha256` does [OCPI §15B] | B; HH-SDR manifest not defined |

### 4.2 Radio and data path

| # | Test | Type | Detects | Data source | Status |
|---|---|---|---|---|---|
| R1 | Data-flow oracle | CBIT | PS starvation, PL drops, stalled chain | producer count == consumer count; `dbg_in_underrun` = 0; `overflow` false [OCPI guide §7] | C for the OpenCPI QPSK app; the HH-SDR MAC/DMA counters are **U-03, U-06** |
| R2 | RX acquisition | CBIT | Loss of sync | `qpsk_rx.in_sync` [OCPI] | C (QPSK only); HH-SDR sync indicator U-03 |
| R3 | In-fabric BER | IBIT | Demod/descrambler faults | `bit_count`, `err_count` — **only after R1 passes** | C (QPSK PRBS test mode only); no PRBS mode is defined for the MANET waveform (U-03) |
| R4 | App liveness | CBIT | ACI application dead while PL free-runs | a counter that stops when the process dies (`qpsk_ctrl.poll_count` pattern) | C; not present in the DMA app; HH-SDR equivalent U-03 |
| R5 | radiod-reported radio health | CBIT | `operational=0`, rising error counters | `radioctl status`/`stats` | A (mock); meaning on hardware depends on the backend (U-03, U-04) |
| R6 | Fault registry | CBIT | Latched `hw_fault`, `backend_io`, `tx_failure`, `rx_silence` | `hh_radiod_faults()` | A in-process; **over the wire U-01, U-05** |
| R7 | AD9361 temperature | CBIT | Over-temperature | `ad9361_get_temperature` (no-OS) [R8] | C + a new proxy/worker property (U-03); limits: none specified |
| R8 | AD9361 RSSI | CBIT | Dead RX, blocked antenna (inference only) | `ad9361_get_rx_rssi` [R8] | C + new property (U-03); interpretation needs U-05 |
| R9 | AD9361 BIST tone / PRBS / loopback | IBIT | Isolates analog chain vs digital port | no-OS `ad9361_bist_tone`, `ad9361_bist_prbs`, `ad9361_bist_loopback` [R8]; UG-570 BIST [R9]; proven via raw override in `board_bist.txt` | C, out of service only; must not run concurrently with traffic. Pass criteria are **not** loopback BER (trap 1) |
| R10 | LO / gain readback | PBIT | Requested vs applied mismatch | no-OS `ad9361_get_rx_lo_freq`, `ad9361_get_tx_attenuation` [R8] | C + new property; note the proxy's properties report *requested* values only [OCPI §6] |
| R11 | Die temperature and supply rails | CBIT | Over-temperature, rail out of range | Zynq XADC via Linux IIO (`in_temp0_raw`, `in_voltage0_vccint_raw`, …) [R10], [R11] | B if the kernel driver is built (DT node present; driver not verified). Limits from the device datasheet, not from this document |
| R12 | Configuration memory integrity | CBIT | SEU in PL configuration | 7-series readback CRC (`POST_CRC`) or the SEM IP [R12] | D: a PL design decision (E1–E4); not in any current bitstream |
| R13 | RF state / safe state | CBIT | TX when it should not | `rf_ctrl_fsm` `FAULT`/`SAFE` states | D: **U-08** |
| R14 | Time plane lock / holdover | CBIT | GNSS/1PPS loss | PHC, 1PPS status | D: **U-07** |
| R15 | Hop timing | CBIT | Missed hops | `fh_controller` status | D: **U-14** |
| R16 | Audio path | CBIT | Codec failure | `audio_pl` | D: **U-13** |
| R17 | Hardware watchdog | CBIT | PS software hang | Cadence WDT in DT (`timeout-sec = 10`), BusyBox `watchdog` applet | B to enable; **what a watchdog reset does to the RF carrier is not known** (§9, Q4) |

The ILA is deliberately absent from this table. It is a netlist-inserted
debug instrument (`ILA_EN` must be 0 inside OpenCPI; CLAUDE.md trap 3 and
§15.6), read through JTAG with Vivado. It is a lab tool, not BIT.

### 4.3 Network (MANET)

| # | Test | Type | Detects | Data source | Status |
|---|---|---|---|---|---|
| N1 | Neighbour presence | CBIT | Isolation, radio deaf | `hh_node_status_t.neighbor_count`, `neighbor_ups/downs` | A in sim; on target needs `hh-manet` to run (hardware backend, U-15) and a periodic export (§8) |
| N2 | Partition | CBIT | Network split | `partitioned`, `reachable_nodes` | same as N1 |
| N3 | Beacon health | CBIT | Malformed/duplicate floods | `beacons_rx_malformed`, `beacons_rx_duplicate` | same as N1 |
| N4 | Self-healing activity | CBIT | Repeated failures/recoveries | `failures_confirmed`, `recoveries_started/completed` | same as N1 |
| N5 | Internal overload | CBIT | Event bus overflow | `events_dropped` | same as N1 |
| N6 | End-to-end throughput / loss / latency | IBIT, ATP | Degraded mesh | `atp-iperf3.sh`, `atp-mgen.sh` | A (netns); on radio needs `manet0` (**U-06**); latency needs sync (**U-07**) |
| N7 | Beacon sequence continuity | ATP | Lost beacons at a capture point | `atp-decode.sh` | A on generated pcaps; on radio **U-16** |

---

## 5. ATP tooling survey

### 5.1 Tools and where they fit

| Tool | What it is | Measures | In `tools/atp` | Runs on board image today? |
|---|---|---|---|---|
| **iperf3** (ESnet) [R13] | Active throughput tester, client/server, JSON output (`-J`) | TCP/UDP throughput, UDP loss and jitter | `atp-iperf3.sh` | No (not in image) |
| **MGEN** (US NRL) [R14] | Scripted traffic generator with per-packet logs | Loss, reordering, one-way latency (needs clock sync) | `atp-mgen.sh` | No |
| **tcpdump / libpcap** [R15] | Packet capture to pcap | Frames on an interface | `atp-capture.sh` | No |
| **tshark / Wireshark** [R16] | Dissection; Lua plugins | Decoded frames, field checks | `atp-decode.sh`, `hh_manet.lua` | Never — analysis host only, by design |
| **tc netem** (Linux) | Link impairment | — (injects loss/delay) | `netns-selftest.sh` | Host only |
| **radioctl** | Public control CLI | State, counters, exit codes | `atp-radioctl.sh` | Would run; script needs `timeout` and `jq` |
| **OpenCPI `ocpirun -d` / ACI** | Application runner / API | Worker properties | — | Yes; `ocpihdl` must not be used (trap 31) |
| **Spectrum analyser, signal generator** | External RF equipment | Emission mask, OBW, sensitivity | — | External; OpenCPI project's `obw.py` / shape metric exist, with the false-pass caveat |
| **CORE / EMANE** | Network emulators | Multi-node behaviour | — | Host only; named in the architecture as test automation, not adopted here |

### 5.2 How the existing `tools/atp` design maps to BIT and ATP

`tools/atp` already embodies the properties this research would recommend:

1. **Tests use only interfaces that survive to hardware** (IP address,
   interface name, `radioctl`). Moving to the radio changes arguments, not
   scripts.
2. **Five-valued results** keep "not measurable yet" (`blocked`) and "no limit
   given" (`unjudged`) out of the pass column. This is the direct defence
   against the false-pass problem in §3.2.
3. **Raw evidence is kept** beside the JSON summary, so every number can be
   re-derived.
4. **Self-tests prove the checks can fail.** A BIT check that has never been
   seen to fail proves nothing; `netns-selftest.sh` and `check_atp_radioctl`
   exercise the fail path.

Gaps found by this research:

| Gap | Detail | Proposed handling |
|---|---|---|
| Board image lacks `timeout`, `jq`, `crond` and the traffic tools | §2.3 | Decision §9, Q8: add BusyBox `timeout`/`crond` applets and `jq` to the image, or provide a small C BIT runner that writes the same JSON |
| `time_source` is always `system`; board has no RTC | §2.3, U-07 | Add monotonic time and boot identity to evidence (§8.4) |
| No evidence for PL/OpenCPI checks | R1–R10 | Same schema, new `check` names, produced through radiod (§8) |
| Fault registry unreadable from scripts | U-01, U-05 | `blocked` until a `faults` response is specified |

---

## 6. Network monitoring and observability options

Footprint figures on the target are **unmeasured** for every option below; no
cited source gives numbers for this board class, and a figure from another
platform would be misleading. §8 proposes measuring the chosen option with the
same method as [`CF-RAM-MEASUREMENT.md`](CF-RAM-MEASUREMENT.md).

| Option | What it is | Footprint on target | Mesh bandwidth | Fits BusyBox/PetaLinux? | Pros | Cons |
|---|---|---|---|---|---|---|
| **Custom key=value exporter** (extend `hh_telemetry_*`, radiod `status`/`stats`) | The repo's own format | Unmeasured; no new process if built into existing daemons | Only what is sent; can be batched and rate-limited | Yes — C11, libc only | Zero dependencies; tests already parse it; same format as logs | Not a standard; an NMS must be written for it |
| **syslog forwarding** (RFC 5424 / RFC 5426 UDP) [R17] | BusyBox `syslogd -R host:port -L` | Already in image | Every forwarded line costs air time; UDP, no delivery guarantee | Yes (in image) | No new software | Log lines are not metrics; loss under congestion is silent |
| **SNMP** (with IF-MIB [R18], NHDP-MIB RFC 6779 [R19], OLSRv2-MIB RFC 7184 [R20]) | Standard management; the MANET MIBs exist only for NHDP/OLSRv2 | Unmeasured (e.g. Net-SNMP agent) | Poll-driven; each walk costs air time | Needs an agent added to the image | Standard MIBs for OLSRv2 state and counters, incl. threshold notifications | MIBs only useful if U-09 chooses OLSRv2; SNMPv3 security must be configured |
| **NETCONF/YANG** (RFC 6241, RFC 7950) [R21] | Model-driven config and state over SSH | Unmeasured; typically a larger stack | Session-oriented, XML | Needs a server added | Transactional config; model-driven | Heaviest option; no MANET YANG model identified by this research |
| **Prometheus `node_exporter`** [R22] | HTTP `/metrics` endpoint, pulled by a server | Unmeasured on target; Go binary, ARMv7 builds published | Pull scrape over the mesh on every interval | Binary drop-in; no package in image | Standard ecosystem | Pull model is a poor fit for a bandwidth-limited mesh; host metrics only, not radio |
| **collectd / telegraf** [R23], [R24] | Push agents with plug-ins | Unmeasured on target | Configurable push | Would need adding (Yocto recipes exist for collectd) | Rich plug-in sets | Extra daemons in ~117 MB; most plug-ins irrelevant |
| **DLEP** (RFC 8175) [R25] | Radio↔router protocol: Destination Up/Down/Update, Current/Maximum Data Rate, Latency, Resources, Relative Link Quality (§13.12–13.19) | Unmeasured | Local link only (radio to attached router), not over the air | Would need an implementation | Standard way to give a router per-neighbour link metrics | Only relevant if a separate router consumes radio metrics — see U-09 |

### 6.1 Relevance to U-09 (OLSRv2 vs the existing stack)

- If **OLSRv2** is adopted, the standard observability comes with it: RFC 7184
  and RFC 6779 MIBs, OLSRv2 on UDP port 269 (RFC 5498) capturable on any IP
  interface with Wireshark's RFC 5444 dissector (U-16 note). The radio's link
  metrics would reach the router either through librc (as the drawing says) or
  through DLEP. Choosing between those two is part of U-09.
- If the **custom stack** stays, none of these MIBs apply. `hh_node_status_t`
  is then the observability model, and exporting it is our own work (§8).

This research does not recommend one routing choice over the other; it records
that the observability cost differs and should be weighed in the U-09 decision.

### 6.2 Flow and packet evidence

`atp-capture.sh` captures on `manet0` once it exists (U-06). MANET control
frames have no capture point (U-16). Capture on the radio costs CPU and RAM on
the node, and the pcap then has to leave the node — over the mesh (air time)
or by maintenance download. Recommendation: capture on the radio only for
IBIT/ATP runs, with a bounded duration and size, never as CBIT.

---

## 7. Logging approach on the target

### 7.1 Current state

- Records: `lvl= comp= event=` plus fields, ≤ 512 bytes, to stderr [REPO].
- Board: BusyBox `syslogd` to `/var/log/messages` on **tmpfs**; no rotation
  configured; sysvinit does not route daemon stderr to syslog [OCPI image].
- Result: on today's board image, radiod/hh-manet logs would go to the console
  or nowhere, and anything that did reach syslog is lost on reboot.

### 7.2 Recommended logging design [REC]

| Aspect | Recommendation | Why |
|---|---|---|
| Sink | Add a syslog sink behind the existing `hh_log_set_sink()` hook (`openlog`/`syslog(3)`, facility `daemon`, level from `hh_log_level_t`) | No new dependency; works with BusyBox `syslogd` and with journald if systemd is chosen later |
| Record identity | Add `seq=` (per-process counter) and `mono_ms=` (from the injected `hh_clock_t`) to every record | Order and loss detection without a trustworthy wall clock (§7.4); keeps the "never read time directly" rule |
| Volatile buffer | BusyBox `syslogd -C<size>` (shared-memory ring, read with `logread`) for the live view | No flash wear |
| Persistent store | A small persistent log on the ext4 partition for **WARN and above** plus BIT results, size-bounded with BusyBox `-s`/`-b` rotation | Survives power loss for post-mission download; bounded flash writes. Size and location: §9, Q7 |
| Kernel | `klogd` into the same syslog | Oops/driver messages alongside application records (P5) |
| Levels | INFO in service, DEBUG/TRACE only on command | Log volume; the forwarder already logs only at TRACE |
| Remote | Off by default; if enabled, WARN+ only, rate-limited, to a named collector | Air time (§7.3) |

The systemd/journal design in [`DEPLOYMENT-ARCHITECTURE.md`](DEPLOYMENT-ARCHITECTURE.md)
§7 is still valid **if** the HH-SDR image moves to systemd; the syslog sink
works under both. The init-system choice is §9, Q9.

### 7.3 Bandwidth over the mesh

The air data rate of the HH-SDR waveform, and therefore the budget available
to management traffic, is not specified anywhere. The OpenCPI DMA figure
(473 k words/s) is a PS↔PL loopback rate, not an air rate, and must not be used
as one. Until a budget exists (§9, Q5), the design rule is: **nothing
observability-related is sent over the air by default**; remote forwarding is
opt-in, WARN+ only, and rate-limited.

### 7.4 Time-stamping without a time plane

- There is no PHC and no 1PPS (U-07), and the device tree shows no RTC. The
  wall clock at boot is whatever the kernel starts with, unless set by
  network time or an operator.
- Therefore: every record and every evidence file carries **boot identity +
  monotonic time + sequence**, and wall-clock UTC is recorded as "as reported
  by the system", as `time_source: "system"` already does.
- Cross-node ordering (e.g. one-way latency, correlating a link failure seen by
  two nodes) stays `blocked` until U-07 is closed, exactly as `atp-mgen.sh`
  already reports.

---

## 8. Recommended approach

### 8.1 Principles

1. **One evidence format.** BIT results use the existing `hh-atp-evidence/1`
   JSON and the same five results and exit codes. A BIT run is an ATP check
   run on a timer or at boot.
2. **One owner for hardware reads.** Every PL/AD9361 read goes through the
   single ACI owner (radiod, per Note 1). BIT never runs `ocpihdl` or opens
   `/dev/mem` from a side process (trap 31; single-owner rule).
3. **Validity before verdict.** Each check states its precondition (e.g. R3
   BER is judged only if R1 counts match). If the precondition fails, the
   result is `fail` or `error`, never `pass`.
4. **Missing capability is `blocked`, citing the U-number.** No placeholder
   thresholds; no threshold means `unjudged`.
5. **Read-only CBIT; disruptive IBIT only on command** and only from a state
   where radiod permits it.
6. **Nothing over the air by default.**

### 8.2 Phase v1 — now, on the mock and off-radio

| Work | Detail |
|---|---|
| Keep and extend `tools/atp` | Add BIT check names for P7–P9 and R5 (`bit.radiod.health` etc.) reusing `atp-radioctl.sh health` |
| Structured-log upgrade | `seq=` and `mono_ms=` fields; syslog sink option (§7.2). Tests continue to assert on records |
| Periodic MANET status | Emit `hh_telemetry_format_status()` as a log record at a configured interval, not only at exit (N1–N5 become observable in sim and later on target). The interval is configuration, not a spec value |
| SCA `runTest` mapping | Keep test id 1 as the software self-check; unknown ids stay `UnknownTest`. Map `runTest` results into evidence records in-process. No new test ids are invented |
| Fault registry | Keep in-process; mark any BIT that needs it over the wire as `blocked` (U-01, U-05) |
| Self-tests | For each new check, a test that forces `fail` (fault injection via `inject-fault`, impaired netns) |

### 8.3 Phase v2 — on the board, via the radiod ACI backend

The ACI sequence (U-04) and the C++ boundary (U-11) are now resolved enough
for a host-tested backend to exist. On-board work starts when U-17 (baseline
project) and U-19 (bring-up ownership) are decided. U-12 remains relevant:
the OpenCPI runtime runs its own container threads inside radiod's process.

| Work | Detail |
|---|---|
| Platform PBIT script | P1–P5, P10 as a POSIX `sh` script run at boot **before** radiod starts PL work, in the order the OpenCPI project proved: FPGA state → driver → `ad9361_init` → application. Writes evidence. Never touches the PL before `ad9361_init` |
| radiod OpenCPI backend: status reads | Extend the existing backend to read configured worker status properties in-process (`getProperty`), as `dma_stream.cc` does, and maps them to `hh_radio_status_t` (`operational`, counters) and to fault-registry kinds that already exist (`hw_fault`, `backend_io`, `rx_silence`, `tx_failure`). Which property maps to which kind is a U-05 decision, recorded when made |
| Data-flow oracle as CBIT | R1 implemented inside radiod on the real HH-SDR counters, once U-03/U-06 name them |
| AD9361 health | R7, R8, R10 need new read-only properties on the AD9361 proxy worker (E1–E4, U-03). Until then `blocked` |
| XADC | R11 via IIO sysfs, read by a platform script or radiod; verify the kernel driver first |
| IBIT | R9 (AD9361 BIST) and the lifecycle check as an operator-initiated test from `stopped`, never while `running` |
| Image changes | Per §9, Q8/Q9: `timeout` + `crond` applets (or init-script loop), `jq` or a C evidence writer, persistent log partition, syslog sink |
| Watchdog | Enable only after Q4 is answered (what a reset does to the carrier) |

### 8.4 Phase v3 — after the hardware contracts

| Unblocks | Adds |
|---|---|
| U-01 + U-05 | `faults` response / async fault events; BIT reads the registry over the wire; severity and latching per the agreed taxonomy |
| U-06 | `manet0` counters (`ip -s link`), on-radio iperf3/MGEN ATP |
| U-07 | `time_source: "phc"`; one-way latency; cross-node event ordering; time-plane lock as CBIT (R14) |
| U-08 | RF safe-state BIT (R13); defined carrier-off action |
| U-09 | Either OLSRv2 MIBs/DLEP, or the custom telemetry as the management model |
| U-14, U-13 | Hop-timing and audio BIT (R15, R16) |
| U-16 | On-radio capture of MANET frames; beacon continuity (N7) |

### 8.5 How BIT results reuse the evidence schema and exit codes

A BIT result is one `hh-atp-evidence/1` record; no new schema is proposed.

| Field | BIT usage |
|---|---|
| `check` | Dotted name, `bit.<area>.<test>` (e.g. `bit.platform.fpga_state`, `bit.radiod.health`). Naming only; no numeric IDs invented |
| `result` / exit | `pass` 0 · `fail` 1 · `error` 3 · `blocked` 4 · `unjudged` 5 |
| `reason` | For `blocked`, the U-number; for `fail`, the violated limit or the failed precondition |
| `params` | BIT type (`pbit`/`ibit`/`cbit`), precondition results |
| `metrics` | The raw values read (counters, states, temperatures) |
| `thresholds` | Only limits that come from a cited document; otherwise empty → `unjudged` |
| `time_source` | `system` today; add boot identity and monotonic time (see §7.4) — a schema revision decision (Q10) |

`radioctl` exit codes map directly: 3 (radiod unreachable) → BIT `fail` (the
radio is down), 4 (rejected) → `fail` or `error` depending on whether the state
was expected, as `atp-radioctl.sh health` already does.

A BIT summary for the operator is then a reduction over the latest records:
any `fail` → NO-GO; any `blocked`/`unjudged` → "not verified", shown as such,
never as GO.

### 8.6 What each consumer gets

| Consumer | v1 | v2 | v3 |
|---|---|---|---|
| Operator | `radioctl status`; GO/NO-GO from BIT summary (sim) | Same on radio; LED/HMI mapping is an HMI decision | Async fault events |
| Test / ATP | `tools/atp` evidence | + PBIT/oracle evidence on board | + on-radio traffic, latency, capture |
| Maintenance | Logs in sim | Persistent WARN+ log and BIT evidence, downloaded over SSH (`dropbear`) | + fault history over the wire |
| NMS over mesh | none | none by default | per Q6 |

---

## 9. Open questions and decisions needed

| # | Question | Why it matters | Proposed owner |
|---|---|---|---|
| Q1 | Which BIT standard or customer specification governs (MIL-HDBK-2165 tailoring, customer SOW, other)? | Sets vocabulary, required analyses, and whether FD/FI must be predicted | System architect / customer |
| Q2 | Required FD%, FI% (to how many units), and maximum false-alarm rate? | Drives the size of the BIT inventory and the need for an FMECA | System architect + test lead |
| Q3 | RF BIT scope: is IBIT with AD9361 BIST/loopback required? Is on-air RF self-test permitted, or only into a load? | Determines whether R9 is built and its safety conditions | E1–E4 + system architect |
| Q4 | Carrier-off safety action when software dies. Killing the app does not stop the carrier; what does a watchdog reset do to the PL? Who guarantees RF off? | Safety; also blocks enabling the watchdog (R17). Related to U-08; the radiod `stop` side is recorded under U-04/U-19 | E1–E4 (PL) + system architect |
| Q5 | Telemetry/management bandwidth budget over the mesh (bytes/s per node, priority vs user traffic)? | Decides whether any remote logging or NMS polling is allowed | System architect + E6 |
| Q6 | Allowed management protocols over the mesh: none, syslog, SNMP, NETCONF, custom? Is there an NMS at all? | Selects §6 option; security accreditation | System architect / customer |
| Q7 | Log retention: how much persistent storage, which levels, how long; post-mission download method; any data-handling restrictions on logs? | Sizing of the persistent log; flash wear | E5 (BSP) + customer |
| Q8 | Production vs development image content: may the production image carry `jq`, BusyBox `timeout`/`crond`, tcpdump, iperf3? | `tools/atp` cannot run on the current image; §5.2 | E5 + test lead |
| Q9 | Init system for the HH-SDR image: BusyBox sysvinit (as on the board now) or systemd (as `DEPLOYMENT-ARCHITECTURE.md` assumes)? | Service supervision, log capture, BIT scheduling | E5 |
| Q10 | Evidence schema revision: add boot identity and monotonic time (`hh-atp-evidence/2`) or keep `/1` with extra `params` fields? | Evidence ordering without U-07 | Test lead |
| Q11 | Mapping of hardware status to fault kinds, severity and latching | Needed to turn worker counters into registry entries | Architect + E1–E4 (**U-05**) |
| Q12 | Operator HMI: what BIT indication is shown and where? | GO/NO-GO presentation | System architect |

One finding of this research may warrant a new `unknown.md` entry (not added
here, since this task writes only this file): the **board image content for
test and logging tools, the init system and log persistence** (Q7–Q9).
The carrier-off behaviour (Q4) is already partly recorded under U-04 and U-19;
the watchdog/process-death case is not, and may belong under U-08.

---

## 10. References

External sources consulted for this document. Where a section number could not
be verified from the source text, none is given.

| Ref | Source |
|---|---|
| R1 | MIL-HDBK-2165, *Testability Handbook for Systems and Equipment*, 31 Jul 1995 (supersedes MIL-STD-2165A). EverySpec: <https://everyspec.com/MIL-HDBK/MIL-HDBK-2000-2999/MIL-HDBK-2165_15131/>; GlobalSpec: <https://standards.globalspec.com/std/992814/mil-hdbk-2165> |
| R2 | Same handbook, BIT requirement items (fault coverage, isolation, false-alarm rate) as summarised at <https://www.scribd.com/document/461417997/MIL-HDBK-2165> — verify wording against the official copy |
| R3 | MIL-STD-1309D, *Definitions of Terms for Testing, Measurement and Diagnostics*, 12 Feb 1992. <https://everyspec.com/MIL-STD/MIL-STD-1300-1399/MIL-STD-1309D_87/> |
| R4 | IEEE Std 1232-2010, *Artificial Intelligence Exchange and Service Tie to All Test Environments (AI-ESTATE)*; IEC 62243:2012. <https://ieeexplore.ieee.org/document/5743076/> |
| R5 | IEEE Std 1149.1, *Standard Test Access Port and Boundary-Scan Architecture* (JTAG) |
| R6 | JTRS, *Software Communications Architecture Specification* v2.2.2, 15 May 2006, `CF::TestableObject` (IDL in Appendix C). <https://www.jtnc.mil/Resources-Catalog/Resource-Catalog-Article-View/Article/2083527/sca-222-specification/> |
| R7 | WInnForum, *SCA 4.1 Requirements Allocation, Objectives, and Verification Criteria*, WINNF-16-P-0025-V1.0.0 — requirements SCA19, SCA21, SCA23, SCA24, SCA546. <https://winnf.memberclicks.net/assets/work_products/Reports/winnf-16-p-0025-v1.0.0%20sca%204.1%20requirements%20allocation_objectives.pdf> |
| R8 | Analog Devices no-OS AD9361 driver, `ad9361_api.h` (`ad9361_get_temperature`, `ad9361_get_rx_rssi`, `ad9361_get_tx_rssi`, `ad9361_get_rx_lo_freq`, `ad9361_get_tx_attenuation`, `ad9361_get_en_state_machine_mode`, `ad9361_write_bist_reg`) and `ad9361.h` (`ad9361_bist_loopback`, `ad9361_bist_prbs`, `ad9361_bist_tone`, `enum ad9361_bist_mode`). <https://github.com/analogdevicesinc/no-OS/tree/main/drivers/rf-transceiver/ad9361> |
| R9 | Analog Devices, *AD9361 Reference Manual*, UG-570 (BIST and loopback). EngineerZone copy: <https://ez.analog.com/cfs-file/__key/telligent-evolution-components-attachments/00-441-00-00-00-07-91-97/AD9361_5F00_Reference_5F00_Manual_5F00_UG_2D00_570.pdf>; BIST discussion: <https://ez.analog.com/rf/wide-band-rf-transceivers/design-support/f/q-a/553940/ad9361-bist-loopback> |
| R10 | AMD/Xilinx, UG480 *7 Series FPGAs and Zynq-7000 SoC XADC User Guide*; XADC Linux wiki: <https://xilinx-wiki.atlassian.net/wiki/spaces/A/pages/18842132/XADC> |
| R11 | Linux driver `drivers/iio/adc/xilinx-xadc-core.c`. <https://github.com/Xilinx/linux-xlnx/blob/master/drivers/iio/adc/xilinx-xadc-core.c> |
| R12 | AMD/Xilinx, UG470 *7 Series FPGAs Configuration User Guide* (readback CRC, `POST_CRC`); PG036 *Soft Error Mitigation Controller*; AR# 67645 <https://www.xilinx.com/support/answers/67645.html> |
| R13 | iperf3, ESnet. <https://github.com/esnet/iperf> |
| R14 | MGEN, U.S. Naval Research Laboratory. <https://github.com/USNavalResearchLaboratory/mgen> |
| R15 | tcpdump / libpcap. <https://www.tcpdump.org/> |
| R16 | Wireshark / tshark, Lua dissector API. <https://www.wireshark.org/docs/> |
| R17 | RFC 5424 *The Syslog Protocol*; RFC 5426 *Transmission of Syslog Messages over UDP*; BusyBox `syslogd` options (`-O`, `-s`, `-b`, `-R`, `-L`, `-C`), `logread`, `timeout`, `crond`: <https://busybox.net/downloads/BusyBox.html> |
| R18 | RFC 2863 *The Interfaces Group MIB* |
| R19 | RFC 6779 *Definition of Managed Objects for the Neighborhood Discovery Protocol* (NHDP-MIB) |
| R20 | RFC 7184 *Definition of Managed Objects for the Optimized Link State Routing Protocol Version 2*, Apr 2014 (augments RFC 6779). <https://www.rfc-editor.org/rfc/rfc7184>; RFC 7181 (OLSRv2); RFC 5498 (MANET UDP port 269) |
| R21 | RFC 6241 *NETCONF*; RFC 7950 *YANG 1.1* |
| R22 | Prometheus `node_exporter`. <https://github.com/prometheus/node_exporter> |
| R23 | collectd. <https://collectd.org/> |
| R24 | Telegraf, InfluxData. <https://github.com/influxdata/telegraf> |
| R25 | RFC 8175 *Dynamic Link Exchange Protocol (DLEP)*, Jun 2017 — Destination Up/Down/Update (§12.11, §12.15, §12.17); metric data items (§13.12–13.20). <https://www.rfc-editor.org/rfc/rfc8175> |

Local sources: this repository's `CLAUDE.md`, `ARCHITECTURE.md`, `unknown.md`,
`tools/atp/README.md`, `radiod/README.md`, `radioctl/README.md`,
`protocol/README.md`, `docs/*`; the OpenCPI project's `CLAUDE.md` (§§6, 8, 9,
10, 12, 15.5, 15A, 15B), `DMA-PORTING-GUIDE.md` (§§7, 8),
`components/specs/*.xml`, `applications/dma_stream/dma_stream.cc`,
`board_bist.txt`, `qpsk_dma_app.xml`, `deploy/boot_reserved_dma/system.dts`,
and the rootfs archive `deploy_dma_old_arch_ch2/rootfs/p2_base.tar.gz`.
