# Ring Interface: contract compared with the PS ring path in the radio project

This compares the ring interface contract (`RING-INTERFACE.md`) with the PS ring path now in the radio and OpenCPI project (sdr_official, files `api/frame_ring.c`, `api/rx_extract.c`, `api/ring_loopback.c` and `api/radhawk.cc`, latest commits dated 2026-11-18, and `tools/latency/TIMING-REPORT-20261118.md`).

Labels: IMPLEMENTED means checked in the code. PROPOSED means in the contract only. UNKNOWN means not found in the code or not settled.

Not read: the whole of `radhawk.cc` outside the ring path, the GUI and `radio_mode.sh`.

## 1. Summary

The two sides agree on the frame content and disagree on how frames are moved and timed.

* Agreed: the MAC PDU v2 layout, the 70, 52 and 34 word frame sizes, three rings (data in, TX, RX), single-producer single-consumer rings, and the reference frames (the implementation tests our reference frames byte for byte).
* Different: the implementation pushes frames to the PL continuously and has no per-frame pull, no target frame or slot, no descriptors in the real path, and no region the MAC process can attach to.
* Open between the two: who pads a short PDU to the fixed frame size, and where the frame boundary comes from on receive.

## 2. What is the same

| Item | Contract | Implementation | State |
|---|---|---|---|
| PDU layout | MAC PDU v2, 22 bytes overhead at 3 bytes of slot bitmap, 6 bytes message header | 24 byte header, data, CRC-32 in the last word. Data words 63, 45, 27 | IMPLEMENTED. Same layout, same 252 bytes of data in 280 |
| Frame sizes | 280 bytes (FEC off) and 208 bytes (FEC 3/4) | 70, 52 and 34 words, which is 280, 208 and 136 bytes | IMPLEMENTED. The 136 byte size (FEC 1/2) is not in the contract |
| Ring kind | Single producer, single consumer, power of two slots | Same (`hh_ring_t`) | IMPLEMENTED |
| Reference frames | `RING-REFERENCE-FRAMES.md` | `test/ring_pdu` checks them, including the 32 byte descriptors | IMPLEMENTED in the test only |
| RX frames to the MAC side | RX ring | radiod's shared-memory RX frame ring, written with `hh_shm_ring_push` | IMPLEMENTED |

## 3. What differs

| Area | Contract | Implementation | Effect |
|---|---|---|---|
| Ring memory | One physically contiguous region with a 64 byte header, handed to the MAC process once by file descriptor | The data-in ring and the TX ring are private heap memory inside `rh_agent` (`calloc`). The RX frame ring is attached by a path | The MAC process cannot attach to the TX ring. The contract's attach sequence has no counterpart |
| Descriptors | 32 byte RX and TX descriptors beside every frame, with slot, superframe, metrics, class, target frame and slot | Not in the real path. The frame ring keeps a length per slot only. The test builds descriptors in a second ring | An RX frame carries no slot number, superframe, arrival offset, RSSI or CRC flag |
| TX timing | The PL pulls the frames of one TDMA frame, once per frame, and a late or missing reply gives a control-only frame | The PS fills DMA buffers from the TX ring as fast as the port takes them, with several frames per message | No target frame or slot, so nothing is timed to a TDMA slot. There is no pull call and no servant |
| TX underrun | PL sends a control-only frame | The TX bit serialiser re-sends a word, and 216,834 underruns in 30 s were measured with a single pass | Frames shift in the stream. The loop runs several passes to keep the port fed |
| Frame boundary on receive | The PL delivers one frame per descriptor | The PL cuts frames wherever the PS word stream happens to be. The extract module hunts for frame control 0xA841 and a good CRC, then takes every `frame_words` words | Works only because every frame has the same size. This is a workaround for a PL without frame boundaries |
| Bad frames | The contract text delivers them with the CRC flag clear. The slot engine spec drops them and counts them | The extract module pushes every frame, good or bad, to the RX frame ring and passes no flag | The MAC side cannot tell a bad frame from a good one without checking the CRC again |
| Slot size | Multiple of 32 bytes (cache line) | 70 words is 280 bytes | 280 is not a multiple of 32. A slot would be 288 |
| Metrics | RSSI, SNR, gain, arrival offset with valid flags | None carried | Routing cannot react to link quality on this path |
| Naming | RX ring is frames from the air to the MAC. TX ring is frames from the MAC to the air | "Raw data RX ring" is the data source feeding the MAC. "TX ring" matches. "Third ring" is the contract RX ring | Easy to confuse in a discussion |
| Start-up | radiod allocates, the MAC attaches over librc, the PL and servant are started | The agent allocates its own rings and attaches the RX frame ring if it exists | No hand-off |

## 4. Open between the two sides

1. **Padding of a short PDU.** The extract module takes a frame of exactly `frame_words` words with the CRC in the last word. The slot scheduler builds a PDU of the length its messages need, with the CRC after the last message, so an empty PDU is 33 bytes or less. A short PDU will not be found by the extractor. Something must pad to the fixed frame size and say where the CRC is. UNKNOWN: which side does it, and whether the CRC moves to the end of the padded frame.
2. **Where frame boundaries come from.** Contract: the slot engine and burst mode. Implementation: a continuous PL and a hunt. Either the PL gets burst framing, or the contract describes the hunt as the receive method for this PL.
3. **Latency.** Frame created in the PS to frame received in the PS measured 28.8 to 30.7 ms on one board, with about 27 to 29 ms of it buffering in the PS and the DMA. The rings are 64 frames deep, which is 9.4 ms of air, and one DMA message holds 14 frames, which is 2.07 ms. The contract assumes a lead time above 8 ms and a MAC preparation window under 12 ms in a 20 ms frame. The buffering has to fall below that, or the lead time has to be larger.
4. **Two ring designs.** The `hh_ring_t` push and pop rings and the descriptor rings of the contract are not the same thing. The contract's rings pair descriptor i with frame slot i, which the test says the contract did not yet state. They have to be one design, or the mapping from one to the other has to be written down.
5. **Radio board count.** The measurements are one board with an RF loopback cable. Two boards on the cable are not yet measured.

## 5. What would have to change

| Side | Change |
|---|---|
| Contract | Add the 34 word (136 byte) frame size. State where padding and the CRC go. Describe the hunt as an allowed receive method, or require frame boundaries from the PL. Use the same ring names as the implementation, or map them. Decide the slot size rule against the 280 byte frame. Deliver only good frames, and count the bad ones, as the slot engine spec says |
| Radio project | Put the TX ring and the RX ring in one shared region the MAC process can attach to. Add descriptors with the target frame and slot. Add a pull, or replace the continuous feed with a feed timed to the TDMA frame. Carry the CRC result and the receive slot into the RX ring |
| MAC process | Nothing for the current rings. The ring driver already reads and writes the contract rings, so it would be pointed at the shared region once it exists |

## 6. Questions for the radio project

1. Is the continuous feed a step towards the pull design, or the final transmit method?
2. Who pads a short PDU, and where is the CRC?
3. Will the PL get burst framing, or does the MAC side keep hunting?
4. Can the TX ring and the RX frame ring be one shared region with a header, as in the contract?
5. What does the latency need to be, and can the buffering be cut to meet it?
