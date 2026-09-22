# REDHAWK 2.2.10 Core Framework — Runtime RAM Measurement

Measured runtime RAM consumption of the REDHAWK Core Framework (CF) and
representative component deployments on the target platform, with the
conditions under which the numbers were taken.

**Measurement date:** 2026-09-22
**Target:** `192.168.122.154` (`localhost.localdomain`)

---

## 1. Test conditions

### Platform

| Item | Value |
|---|---|
| CF | REDHAWK 2.2.10-3.el7 (x86_64) |
| OS | CentOS Linux 7.6.1810 (Core) |
| Kernel | 3.10.0-957.el7.x86_64 |
| Arch / bitness | x86_64, CF binaries ELF 64-bit LSB |
| CPU | Intel Xeon W-2235 @ 3.80 GHz, 4 vCPU (1 thread/core) |
| Virtualization | KVM guest |
| RAM | 8008864 kB total (7821 MiB) |
| Swap | 6 GiB, **0 B in use** throughout (no paging pressure) |
| ORB | omniORB 4.2.4-2.el7 |
| Python | 2.7.5-80.el7_6 |
| glibc | 2.17 |
| `vm.overcommit_memory` | 0 (heuristic) |
| Transparent hugepages | `always` |

### Domain configuration

| Item | Value |
|---|---|
| Domain name | `REDHAWK_DEV` |
| Device manager node | `DevMgr_localhost` |
| Registered device | `GPP_localhost` (single GPP) |
| Persistence | `False` |
| CF log level | `DEBUG_LEVEL 3`, `default.logging.properties` |
| GPP `mem_free` threshold | 1564 (MB) |

### Installed CF packages

`redhawk`, `redhawk-devel`, `redhawk-sdrroot-dom-mgr`, `redhawk-sdrroot-dev-mgr`,
`redhawk-sdrroot-dom-profile`, `redhawk-basic-components`, `redhawk-basic-devices`,
`redhawk-basic-waveforms`, `redhawk-codegen`, `redhawk-adminservice`,
`redhawk-ide`, `redhawk-qt-tools` — all 2.2.10-3.el7.

### Method

- Per-process figures read from `/proc/<pid>/smaps`, summing `Rss`, `Pss`,
  and `Private_Clean + Private_Dirty` (USS). Zombie processes excluded.
- Waveforms launched over the CORBA API (`redhawk.attach` →
  `createApplication` → `start`), allowed to settle **60 s**, then sampled.
- Each waveform released via `releaseObject()`; orphan sweep between trials
  confirmed `0` component processes remaining before the next measurement.
- Peak sampling: `free -k` every 0.5 s across an 80 s window spanning launch.

> **Read PSS, not RSS.** CF component processes each map the same REDHAWK and
> omniORB shared libraries, so summing RSS across processes double-counts them
> heavily (247372 kB RSS vs 114915 kB PSS for the same deployment). PSS
> apportions
> shared pages among their sharers and is the additive, non-double-counted
> figure. USS is what is actually reclaimed if the process exits.

---

## 2. Baseline — CF infrastructure, idle, no waveform

Domain up, GPP registered, zero applications deployed.

| Group | Process | RSS kB | PSS kB | USS kB |
|---|---|---:|---:|---:|
| INFRA_ORB | omniNames | 3648 | 1155 | 724 |
| INFRA_ORB | omniEvents | 6928 | 4590 | 4152 |
| CF_CORE | DomainManager | 18040 | 10001 | 7492 |
| CF_CORE | DeviceManager | 15904 | 7883 | 5392 |
| CF_DEVICE | GPP | 21828 | 13929 | 11496 |
| | **TOTAL** | **66348** | **37558** | **29256** |

**CF idle baseline ≈ 36.7 MiB PSS (64.8 MiB RSS).**

Split: ORB infrastructure 5.6 MiB · CF core (Domain+Device mgr) 17.5 MiB ·
GPP device 13.6 MiB.

System-wide at idle: `MemUsed 213756 kB`, `MemAvailable 7515252 kB`.

---

## 3. Deployment A — `rh.FM_mono_demo` (6 C++ components)

Components: `TuneFilterDecimate`, `psd` ×2 (`narrowband_psd`, `psd_1`),
`fastfilter`, `ArbitraryRateResampler`, `AmFmPmBasebandDemod` — all C++.

| Trial | WAVEFORM PSS kB | CF+ORB PSS kB | TOTAL PSS kB | TOTAL RSS kB |
|---|---:|---:|---:|---:|
| 1 | 47320 | 34210 | 114915 | 247372 |
| 2 | 51382 | 34358 | 119123 | 251592 |
| 3 (peak run) | 49342 | 30531 | 115296 | 247772 |

Per-component C++ cost, steady state: **6.9–9.2 MiB PSS** each
(~21.5–24 MiB RSS each, mostly shared library text).

Trial-to-trial spread ≈ 4.1 MiB PSS (~3.5%), attributable to allocator/buffer
state rather than deployment differences.

> The `TEST_HARNESS` row (`py:launch.py`, ~32.6 MiB PSS) is the Python
> measurement client holding the app reference — **not** part of the CF or
> waveform cost, and excluded from the figures above. In the totals quoted
> from raw snapshots it is listed separately.

**Waveform-attributable cost: ≈ 46.2–50.2 MiB PSS for 6 C++ components**
(deployment total excluding harness: **79.6–83.7 MiB PSS**).

---

## 4. Deployment B — `rh.basic_components_demo` (5 C++ + 1 Python)

Components: `SigGen` ×2 (`SigGen_sine`, `SigGen_noise`), `HardLimit`, `agc`,
`autocorrelate` (C++) and **`fcalc` (Python)**.

| Group | Process | RSS kB | PSS kB | USS kB |
|---|---|---:|---:|---:|
| WAVEFORM | SigGen_sine | 24164 | 9382 | 7024 |
| WAVEFORM | HardLimit_1 | 24236 | 9494 | 7192 |
| WAVEFORM | agc_1 | 22072 | 7395 | 5132 |
| WAVEFORM | **py:fcalc.py** | 26264 | **20301** | 19000 |
| WAVEFORM | autocorrelate_1 | 23936 | 9196 | 6912 |
| WAVEFORM | SigGen_noise | 22044 | 7270 | 4932 |
| | **WAVEFORM total** | **142716** | **63038** | **50192** |

CF+ORB at the same instant: 32.1 MiB PSS. Deployment total (excl. harness):
**93.7 MiB PSS** (95904 kB).

### Key finding — Python components cost ~2.5× C++

`fcalc` (Python) is **19.8 MiB PSS** (20301 kB) against 7.1–9.3 MiB for each
C++ peer, because each Python component hosts its own CPython 2.7 interpreter with
far less page sharing than the common C++ library set.

> **Measurement note:** an initial pass under-counted this waveform
> (44668 kB WAVEFORM PSS vs 63038 kB post-fix) because the process
> classifier matched components by `/proc/<pid>/exe` under `SDRROOT`.
> Python components exec `/usr/bin/python2.7`, so `fcalc` was missed.
> Classification was corrected to also match SDRROOT scripts in the
> process `cmdline`; figures above are post-fix. Any future CF RAM
> tooling must classify Java/Python-hosted components by cmdline, not exe.

---

## 5. Peak vs steady state

Sampling `free -k` at 0.5 s across launch of `FM_mono_demo`:

| Point | Used kB |
|---|---:|
| Pre-launch (t=0.5 s) | 216484 |
| Maximum (t=39.5 s) | 291920 |
| Delta | **+75436 kB (~73.7 MiB)** |

No transient overshoot above steady state was observed — memory rises
monotonically to the settled value during component spawn. Sizing can be
based on steady-state figures; no launch-time headroom spike to absorb.

---

## 6. Summary

| Configuration | PSS | RSS |
|---|---:|---:|
| CF idle (ORB + DomainMgr + DeviceMgr + GPP) | **36.7 MiB** | 64.8 MiB |
| \+ `FM_mono_demo` (6 C++ components) | **79.6–83.7 MiB** | ~204 MiB |
| \+ `basic_components_demo` (5 C++ + 1 Python) | **93.7 MiB** | ~207 MiB |

All figures exclude the Python measurement harness.

Planning rules of thumb on this platform:

- **CF fixed overhead: ~37 MiB PSS.** Paid once per node regardless of load.
- **C++ component: ~7–9 MiB PSS** each.
- **Python component: ~20 MiB PSS** each (~2.5× a C++ component).
- Estimate ≈ `37 + 8·(C++ count) + 20·(Python count)` MiB PSS. This predicts
  85 MiB for `FM_mono_demo` (actual 79.6–83.7) and 97 MiB for
  `basic_components_demo` (actual 93.7) — i.e. slightly conservative, within
  ~4% on both, so it is safe for sizing.
- Against 7821 MiB total RAM, a 6-component waveform consumes **~1.1%**;
  RAM is not a binding constraint at this scale on this platform.

### Caveats

- Single-node domain, one GPP, no FEI/hardware devices. A real deployment
  with tuner devices and drivers will add per-device cost not measured here.
- `DEBUG_LEVEL 3` and `PERSISTENCE False` are in force; higher verbosity or
  enabled persistence will shift CF core numbers.
- Idle dataflow: the demo waveforms process synthetic/file input. Component
  memory under sustained high sample-rate streaming with deeper queues will
  exceed these figures — buffer depth scales with data rate and is not
  exercised by this test.
- Figures are steady state at 60 s post-launch; long-run growth/leak
  behaviour was not assessed.
