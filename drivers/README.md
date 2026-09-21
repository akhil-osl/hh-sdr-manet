# drivers/ — kernel and device-driver components

**Status: EMPTY. Nothing here is implemented.**

This directory marks where the target architecture's kernel-side components
belong. It is a placeholder for structure, not a stub for behaviour: no
skeleton driver, no header, and no build target exists, because writing one
would mean inventing the contracts below.

## What belongs here

### `manet0` net_device driver — blocked on U-06

The data-plane driver from the architecture's DATA PLANE path:

```
Linux IP stack (skb priority / DSCP -> bearer class)
      |
   manet0 net_device
      |  ICD-1 PDU-over-DMA (AXI-HP)
   pdu_dma_tx / pdu_dma_rx
```

The drawing's Note 2 gives the shape of the contract — *one PDU = one
descriptor = one skb; TLAST marks the PDU boundary; length is carried in the
descriptor, not the payload* — but not the implementable detail: descriptor
field layout and widths, ring sizes and alignment, AXI-HP addressing,
per-class queue mapping, the DSCP-to-bearer-class mapping, zero-copy buffer
ownership, and completion/interrupt semantics.

**Owner: E5 (Linux BSP), with E1–E4 for the HDL side.**

### Radio clock driver — blocked on U-07

The time-plane driver:

```
GNSS 1PPS -> PL TIME BASE -> fb_controller -> slot/frame timing
                                           -> radio clock / PHC
```

Exposes `/dev/ptpN` plus time registers (frame / slot / hop / ns), read-only
to users via a seqlock. Needs the ICD-3 AXI-Lite register map, the seqlock
read protocol, PHC device naming and capabilities, 1PPS disciplining
behaviour, and holdover semantics on GNSS loss.

**Owner: E5 (Linux BSP), with E1–E4 for the HDL side.**

## Current state of the repository

Neither the data plane nor the time plane exists here. A sweep for `PHC`,
`PTP`, `1PPS`, `GNSS`, `seqlock`, `TDMA`, `manet0`, `net_device`, `DSCP` and
`skb` returns **zero** hits across all source, headers and build files.

Today's data path is entirely in userspace: `src/dataplane/forwarder.c` calls
`hh_radio_transmit()` on an in-process backend. How that path migrates to
`manet0` is an open question (U-15).

See [`../unknown.md`](../unknown.md).
