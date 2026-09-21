# fpga/ — PL / FPGA fabric

**Status: EMPTY. Nothing here is implemented.**

This directory marks where the target architecture's PL components belong. No
HDL, constraints, or bitstream build exists here.

## What belongs here

From the architecture's PL (FPGA FABRIC) region:

| Block | Role |
|---|---|
| `audio_pl` | I2S slave (codec master), AEC (NLMS) + NS (Wiener), AXI-Stream to PS |
| `pdu_dma_tx` / `pdu_dma_rx` | AXI-HP DMA, descriptor length, TLAST = PDU boundary |
| `mac_pl` | TDMA slot timer and slot gating, PDU CRC, hop-synchronous frame, **generates MANET STROBE** |
| MODEM CHAIN | framer / FEC / modulator (TX), demod / FEC / deframer (RX), runtime properties |
| `ad9361` device workers | adc / dac / data_sub, LVDS 2R2T DDR |
| `PL TIME BASE` | 1PPS-disciplined counters: frame / slot / hop / ns |
| `fh_controller` | 1000 hop/s LO retune, HOP_TRIG / ACK / dwell |
| `rf_ctrl_fsm` | RF_OFF / RX_PREP / RX / TX_PREP / TX / TURNAROUND / FAULT / SAFE |

## Two constraints that bind software

**MANET STROBE is the sole RF state authority.** The drawing's Note 3:

> MANET STROBE from mac_pl is the sole RF state authority; rf_ctrl_fsm may only
> add safety (interlock), never contradict it.

This constrains radiod as much as the HDL: radiod must never drive T/R
switching directly. The signal's timing, polarity, width and handshake are
unspecified (U-08).

**Frequency hopping is PL-autonomous.** At 1000 hop/s, hopping is far too fast
for request/response IPC. radiod's role is presumably limited to selecting a
hopset — but the hopset-id property name and encoding, ownership of hopset
selection, and missed-hop failure semantics are all unspecified (U-14).

This also leaves radiod's existing `set_channel` command only partially
meaningful: it passes an **opaque** integer today, and
`include/hhsdr/radio/radio.h` records the channel as *"logical channel index;
mapping to RF TBD"*.

## Current state of the repository

No HDL of any kind. A sweep for `.vhd`, `.v`, `.sv`, `.bit`, `.hdl`, `.xml`,
plus `LVDS`, `SPI`, `I2S`, `AEC`, `interlock`, `TDMA` and `fh_controller`
returns **zero** hits.

The repository's hardware boundary is one vtable, `hh_radio_ops_t`
(`include/hhsdr/radio/radio.h`), whose header states plainly that it contains
no AXI register maps, DMA descriptors, PHY/modem control, RF tuner APIs, or
sample-streaming interfaces — because none of those contracts exist.

See [`../unknown.md`](../unknown.md).
