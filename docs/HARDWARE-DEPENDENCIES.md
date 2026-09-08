# Hardware Dependency Tracking

Per. This is the authoritative list of what runs today, what is
simulated for testing, what exists only as an interface, and what is blocked on
hardware. It is updated as functionality lands.

The goal it serves: **future FPGA integration should require implementing the
hardware adapter, not rewriting the MANET stack.**

## Legend

| Status | Meaning |
|---|---|
| **AVAILABLE NOW** | Implemented in production C, runs without hardware |
| **SIMULATED FOR TESTING** | Real production code, driven by test-only inputs |
| **ABSTRACTED / INTERFACE ONLY** | Contract defined; no backend behind it yet |
| **HARDWARE-DEPENDENT / TBD** | Blocked on an FPGA/PL/PHY decision |

## The single integration seam

Everything hardware-facing is behind one vtable, `hh_radio_ops_t`
(`include/hhsdr/radio/radio.h`). The production stack depends on that struct and
nothing below it. Implementing its seven operations is the entirety of what
FPGA/PL integration must supply:

| Operation | HTI | What hardware must do |
|---|---|---|
| `open` / `close` | — | Acquire/release PL and RF resources |
| `transmit` | HTI-03 | Hand a framed message to the modulation path |
| `set_rx_callback` + `poll` | HTI-04, HTI-05 | Deliver demodulated frames with measured metrics |
| `get_status` | HTI-02 | Report channel, waveform, error counters |
| `get_link_metrics` | HTI-05 | Report per-neighbor RSSI/SNR/PER/retransmits |
| `set_channel` | HTI-14 | Retune, or report `HH_ERR_UNSUPPORTED` |

## Deliberately NOT defined

No AXI register map, DMA descriptor/buffer API, PHY or modem control API, RF
transceiver/tuner API, sample-streaming interface, or modulation/RCC engine
interface appears anywhere in this repository. The HW/SW Interface Specification
 records the Radio/SDR Interface C ABI as TBD; none of it is guessed at.

The unit of exchange at the boundary is a **frame**, not a sample buffer. The
MANET control plane has no use for baseband samples, so every DSP/PHY decision
stays below the boundary where it belongs.

## Status by area

### AVAILABLE NOW (production C, no hardware needed)
- Core types, wraparound-safe sequence arithmetic, clock abstraction
- Structured logging and configuration (load, validate, apply)
- Typed event definitions for HTI-04 … HTI-16
- Event dispatcher with bounded per-subscriber queues
- Beacon and route-update binary wire codec
- **Discovery Manager** — acquisition/steady phases, adaptive cadence, duplicate
  and replay rejection
- **Neighbor Manager** — authoritative one-hop table, cadence-based expiry, LRU
  eviction, `NeighborUp/Down/Changed`
- **Link Health Monitor** — multi-signal fusion, hysteresis, hold-downs, cause
  classification
- **Routing Engine** — distance-vector with sequence freshness, composite
  metric, split horizon with poisoned reverse, warm alternates, two-phase
  invalidation, dampening
- **Packet Forwarder** — lock-free snapshot reads, bounded pending queue, TTL
- **Failure Detector** — debounced confirmation, retraction, corroboration counts
- **Topology Manager** — read-model graph, partition and merge detection
- **Self-Healing Manager** — full recovery pipeline, strategy by cause hint
- **Node assembly** — event wiring, SCA lifecycle, control loop
- **Management/Telemetry** — node/neighbor/route/topology/radio status export
- **SCA compatibility layer** — lifecycle enforcement, property surface, port
  declarations, component classification

### ABSTRACTED / INTERFACE ONLY
- `hh_radio_ops_t` — the radio/SDR contract (no production backend)
- `hh_hw_adapter` — real, linkable, returns `HH_ERR_NOT_IMPLEMENTED` from every
  hardware-dependent operation and logs why. It fails loudly rather than
  pretending to work.

### SIMULATED FOR TESTING (test-only, under `tests/`)
- `mock_radio` — implements the same `hh_radio_ops_t` contract, supplying
  controlled frames and metrics through the interfaces real hardware will use
- `vclock` — virtual clock making every scenario deterministic
- `netsim` — multi-node simulator: virtual links, per-direction loss, delay,
  RSSI/SNR/PER and PHY-error injection, node/link failure and recovery,
  partitions, merges, mobility, asymmetric links

All RF metric values consumed by the stack today (RSSI, SNR, PER, retransmits,
PHY errors, ACK outcomes, latency) are **supplied by the test harness**. Real
values will come from the hardware adapter through the identical interface.

Verified separation: no file under `src/` or `include/` references anything
under `tests/`, the production library contains zero test symbols, and there are
no simulation-specific branches in production code.

### HARDWARE-DEPENDENT / TBD
| Item | Spec reference | Notes |
|---|---|---|
| Radio/SDR C ABI backend |.5 | The adapter implementation itself |
| Metric units and encodings (RSSI, SNR, PER, retransmits, latency) |.2 | Stack carries them as floats; units unconfirmed |
| Beacon wire byte layout |.1 | This repo defines a versioned interim layout |
| `capabilities`/`radio_caps`/`supported_waveforms` bit-fields |.7 | Interim encoding in `core/types.h` |
| `position` / `power` field formats |.8 | Carried as optional, flagged present/absent |
| Max frame size | — | `HH_RADIO_MAX_FRAME` is a placeholder bound |
| Channel-change failure fallback |.11 | This stack falls back to route-based recovery |
| Timer/threshold values |.4 | Config defaults are an operating point, not spec values |
