# What MANET Needs From the Radio

Eight operations. This is the complete list.

---

| # | Operation | Parameters | Returns | Purpose |
|---|---|---|---|---|
| 1 | `open` | — | status | Acquire the radio |
| 2 | `close` | — | status | Release the radio |
| 3 | `transmit` | frame | status | Send one frame |
| 4 | `set_rx_callback` | callback, ctx | status | Register where received frames are delivered |
| 5 | `get_status` | → status struct | status | Radio health and counters |
| 6 | `get_link_metrics` | node id → metrics struct | status | Per-node signal quality (optional) |
| 7 | `set_channel` | channel index | status | Retune (optional) |
| 8 | `poll` | current time (ms) | status | Deliver received frames |

Optional operations may report "unsupported" — MANET continues with reduced
capability.

---

## Operation details

### 1. `open`

Acquire the radio and make it ready to send and receive.

| | |
|---|---|
| Parameters | none |
| Returns | success, or an error |

Called once at startup. If this fails, MANET shuts down and reports why rather
than running without a radio. Do not report success from an incomplete
implementation.

---

### 2. `close`

Release the radio.

| | |
|---|---|
| Parameters | none |
| Returns | success, or an error |

Called once at shutdown. Must be safe to call twice, and safe to call when
`open` never succeeded.

---

### 3. `transmit`

Send one frame over the air.

| | |
|---|---|
| Parameters | frame (see *Data we send*) |
| Returns | success, or an error |

- The frame is valid only during the call — copy what is needed before
  returning.
- `dst = 0` means broadcast to everyone in range. Any other value is a unicast
  to that node.
- The payload is opaque. Do not parse, modify, or reorder it.
- **Must not block.** Queue the frame and return.
- Success means the frame was accepted for transmission — **not** that anyone
  received it. Delivery failure is reported later through `ack_success`.

---

### 4. `set_rx_callback`

Register the function that received frames are handed to.

| | |
|---|---|
| Parameters | callback function, context pointer |
| Returns | success, or an error |

Store both and do nothing else. The context pointer is opaque — pass it back
unchanged with every callback, and never dereference it.

The callback is invoked later, from `poll` only.

---

### 5. `get_status`

Report radio health and cumulative counters.

| | |
|---|---|
| Parameters | output struct (see *On request*) |
| Returns | success, or an error |

Called periodically. Keep it cheap and non-blocking — return cached values
rather than querying hardware if querying is slow.

`operational = false` is how MANET learns its own radio has failed, so report it
honestly.

---

### 6. `get_link_metrics` — optional

Report signal quality for one node, measured independently of frame arrival.

| | |
|---|---|
| Parameters | node ID, output struct (see *Per received frame*) |
| Returns | success, "not found", or "unsupported" |

- "not found" — no measurement exists for that node.
- "unsupported" — no independent sampling is possible at all.

Genuinely optional. If measurements only exist when a frame arrives, report
"unsupported" and MANET relies solely on the per-frame metrics.

---

### 7. `set_channel` — optional

Retune to a different channel.

| | |
|---|---|
| Parameters | channel index (a logical index, not a frequency) |
| Returns | success, "unsupported", or an error |

The mapping from index to actual frequency is yours to define — document
whatever is chosen.

Used when MANET concludes the current channel is being interfered with. If
"unsupported" is reported, it reorganises the network instead. Both paths work.

---

### 8. `poll`

Deliver every frame received since the last call.

| | |
|---|---|
| Parameters | current time in milliseconds |
| Returns | success, or an error |

For each frame received, invoke the callback once, passing the frame **and** the
signal quality measured for that frame.

**The callback must be invoked only from inside `poll`** — never from a separate
thread, an interrupt handler, or a DMA completion handler. MANET is
single-threaded with no locks.

If frames arrive asynchronously, buffer them internally and deliver them here.

Called roughly every 10 ms. Do not block.

The frame and metrics may point at temporary memory — MANET copies what it needs
before the callback returns.

---

## Data we send

One frame:

| Field | Type | Meaning |
|---|---|---|
| kind | enum | beacon / data / routing |
| src | uint32 | sending node ID |
| dst | uint32 | destination node ID; **0 = broadcast** |
| len | uint16 | payload length |
| data | bytes | payload, max 512 — **opaque, do not modify** |

---

## Data we need back

### Per received frame

The frame as sent, **plus** the signal quality measured while receiving it:

| Field | Type | Units | Required |
|---|---|---|---|
| neighbor_id | uint32 | — | Yes |
| rssi | float | dBm | Yes |
| snr | float | dB | Yes |
| per | float | 0.0–1.0 | Yes |
| phy_errors | uint32 | count | Yes |
| retransmit_count | uint32 | count | 0 if untracked |
| ack_success | bool | — | only if valid |
| ack_valid | bool | — | Yes — false if unavailable |
| latency_ms | float | ms | only if valid |
| latency_valid | bool | — | Yes — false if unmeasured |

Set the `_valid` flag false rather than supplying a made-up value.

### On request

| Field | Type | Meaning | Required |
|---|---|---|---|
| operational | bool | false = radio has failed | Yes |
| channel | uint32 | current channel index | Yes |
| frames_tx | uint64 | cumulative | Yes |
| frames_rx | uint64 | cumulative | Yes |
| tx_errors | uint64 | cumulative | Yes |
| rx_errors | uint64 | cumulative CRC/decode failures | Yes |
| frequency_hz | float | 0 if unknown | Optional |
| waveform_id | uint32 | 0 if unknown | Optional |

---

## Constraints

- `transmit` must not block. Success means accepted, not delivered.
- Received frames are delivered only when `poll` is called — never from a
  separate thread or interrupt. MANET is single-threaded and lock-free.
- `poll` is called roughly every 10 ms.
- Payload bytes must arrive back byte-identical.
- `close` must be safe to call twice, and after a failed `open`.

---

## Open items

1. Real maximum frame size — 512 is a placeholder.
2. RSSI/SNR available in true dBm/dB, or a raw scale plus conversion?
3. Can CRC/decode errors be counted separately from frames that never arrived?
4. Channel change supported? Index-to-frequency mapping, count, retune time.
5. ACK reporting available?
6. Per-node metrics available on request, or only per received frame?
7. Do src/dst/kind travel with the frame, or should we put them in the payload?
