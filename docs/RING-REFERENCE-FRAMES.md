# Ring Interface Reference Frames

Companion to the ring interface contract, section 14. These are complete, full-size frames for trial checks, one pair for each of the two PL frame sizes. Every value in them (node ids, sequence numbers, slot and frame numbers, signal levels, data patterns) is a test value and will change once the design is in good shape.

All multi-byte fields are little-endian. Hex values are in memory order, lowest address first.

## Frame sizes

| Mode | PL frame | Words | PDU in the ring | Message data in one frame |
|---|---|---|---|---|
| FEC off | 280 bytes | 70 | 276 bytes | 252 bytes |
| FEC on, rate 3/4 | 208 bytes | 52 | 204 bytes | 180 bytes |

The air frame is the same length in both modes. With FEC on, 52 words of data are expanded by the encoder to the same 2240 bits on the air, so only 208 bytes of the frame carry data.

## What each frame is

Each reference frame is one MAC PDU in the layout of the MAC frame specification v2, written the way the PDU codec writes it but without the final 4-byte CRC-32, which the PL owns. The PDU has a 9-byte header, a 6-byte control block, a 3-byte slot bitmap and one message. The message uses the test protocol (0xF0), class 4, and asks for an acknowledgement. The message data is a counting pattern, so a wrong or missing byte is easy to see.

The PL does not read inside the frame. It carries the bytes. The descriptor is written by the PL on receive, and read by the MAC software on both paths. The PL never sees the transmit descriptor.

Checks that apply to every frame below:

* The ring frame is the PL frame size minus 4 bytes. The PL appends the CRC-32 (IEEE 802.3) over every PDU byte on transmit, and checks and removes it on receive, so the PDU and the CRC-32 fill the PL frame exactly and these examples need no padding.
* There is no CRC-32 and no end-of-burst marker inside the ring frame bytes. The frame end is the descriptor length, which does not count the CRC-32.
* The burst header, the length it carries, any padding and the modem framing are added by the PL and are not part of these bytes.

## FEC off: 280-byte frame

The PL frame is 280 bytes. The ring frame is the PDU without its CRC-32, 276 bytes. The PL appends the 4-byte CRC-32 on transmit and checks and removes it on receive, so the PDU and the CRC-32 together fill the PL frame and nothing is padded. The message data is 252 bytes (276 minus 18 bytes of PDU overhead, minus 6 bytes of message header).

### Receive reference frame, 276 bytes

Direction: node 1 sends to node 2, received by node 2. The data bytes count up from 00.

Frame bytes:

```
41 A8 21 10 00 FF FF 01 00 10 00 00 00 00 03 20
00 00 F0 02 00 FC A0 07 00 01 02 03 04 05 06 07
08 09 0A 0B 0C 0D 0E 0F 10 11 12 13 14 15 16 17
18 19 1A 1B 1C 1D 1E 1F 20 21 22 23 24 25 26 27
28 29 2A 2B 2C 2D 2E 2F 30 31 32 33 34 35 36 37
38 39 3A 3B 3C 3D 3E 3F 40 41 42 43 44 45 46 47
48 49 4A 4B 4C 4D 4E 4F 50 51 52 53 54 55 56 57
58 59 5A 5B 5C 5D 5E 5F 60 61 62 63 64 65 66 67
68 69 6A 6B 6C 6D 6E 6F 70 71 72 73 74 75 76 77
78 79 7A 7B 7C 7D 7E 7F 80 81 82 83 84 85 86 87
88 89 8A 8B 8C 8D 8E 8F 90 91 92 93 94 95 96 97
98 99 9A 9B 9C 9D 9E 9F A0 A1 A2 A3 A4 A5 A6 A7
A8 A9 AA AB AC AD AE AF B0 B1 B2 B3 B4 B5 B6 B7
B8 B9 BA BB BC BD BE BF C0 C1 C2 C3 C4 C5 C6 C7
C8 C9 CA CB CC CD CE CF D0 D1 D2 D3 D4 D5 D6 D7
D8 D9 DA DB DC DD DE DF E0 E1 E2 E3 E4 E5 E6 E7
E8 E9 EA EB EC ED EE EF F0 F1 F2 F3 F4 F5 F6 F7
F8 F9 FA FB
```

| Offset | Bytes | Field | Value |
|---|---|---|---|
| 0 | 41 A8 | frame control | 0xA841 |
| 2 | 21 | sequence | 0x21 |
| 3 | 10 00 | net id | 0x0010 |
| 5 | FF FF | destination | broadcast |
| 7 | 01 00 | source | node 1 |
| 9 | 10 | time quality | 0x10 |
| 10 | 00 | flags | none set |
| 11 | 00 00 | break-in invoker | none |
| 13 | 00 | counts | no Block ACK, no link report |
| 14 | 03 | slot bytes | 3 |
| 15 | 20 00 00 | slot claims | 20 00 00 |
| 18 | F0 | message protocol | test protocol 0xF0 |
| 19 | 02 00 | message destination | node 2 |
| 21 | FC A0 | length and flags | 0xA0FC: length 252, class 4, acknowledgement requested |
| 23 | 07 | link sequence | 7 |
| 24 | 00 01 02 03 | data, first 4 bytes | first bytes of the pattern |

The data runs from offset 24 to offset 275, 252 bytes. The CRC-32 the PL adds on the air, or checks and removes on receive, is computed over bytes 0 to 275. Its value is CRC-32 0xC28F65AB, sent least significant byte first: AB 65 8F C2. It is not in the ring frame.

RX descriptor, 32 bytes:

```
03 00 00 00 14 01 05 00 34 12 00 00 78 00 00 00
C0 FE 50 00 78 00 0F 00 E8 03 00 00 00 00 00 00
```

| Offset | Bytes | Field | Value |
|---|---|---|---|
| 0 | 03 00 00 00 | flags | FILLED and CRC_OK |
| 4 | 14 01 | length | 276 |
| 6 | 05 00 | slot in frame | 5 |
| 8 | 34 12 00 00 | superframe | 0x1234 |
| 12 | 78 00 00 00 | arrival offset | +120 ns |
| 16 | C0 FE | RSSI | -80.00 dBm |
| 18 | 50 00 | SNR | 20.00 dB |
| 20 | 78 00 | RX gain | 30.00 dB |
| 22 | 0F 00 | metric flags | RSSI, SNR, gain, arrival offset valid; RSSI not calibrated |
| 24 | E8 03 00 00 | receive count | 1000 |
| 28 | 00 00 00 00 | reserved | zero |

### Transmit reference frame, 276 bytes

Direction: node 2 sends to node 1. The data bytes count down from FF.

Frame bytes:

```
41 A8 22 10 00 FF FF 02 00 10 00 00 00 00 03 00
02 00 F0 01 00 FC A0 08 FF FE FD FC FB FA F9 F8
F7 F6 F5 F4 F3 F2 F1 F0 EF EE ED EC EB EA E9 E8
E7 E6 E5 E4 E3 E2 E1 E0 DF DE DD DC DB DA D9 D8
D7 D6 D5 D4 D3 D2 D1 D0 CF CE CD CC CB CA C9 C8
C7 C6 C5 C4 C3 C2 C1 C0 BF BE BD BC BB BA B9 B8
B7 B6 B5 B4 B3 B2 B1 B0 AF AE AD AC AB AA A9 A8
A7 A6 A5 A4 A3 A2 A1 A0 9F 9E 9D 9C 9B 9A 99 98
97 96 95 94 93 92 91 90 8F 8E 8D 8C 8B 8A 89 88
87 86 85 84 83 82 81 80 7F 7E 7D 7C 7B 7A 79 78
77 76 75 74 73 72 71 70 6F 6E 6D 6C 6B 6A 69 68
67 66 65 64 63 62 61 60 5F 5E 5D 5C 5B 5A 59 58
57 56 55 54 53 52 51 50 4F 4E 4D 4C 4B 4A 49 48
47 46 45 44 43 42 41 40 3F 3E 3D 3C 3B 3A 39 38
37 36 35 34 33 32 31 30 2F 2E 2D 2C 2B 2A 29 28
27 26 25 24 23 22 21 20 1F 1E 1D 1C 1B 1A 19 18
17 16 15 14 13 12 11 10 0F 0E 0D 0C 0B 0A 09 08
07 06 05 04
```

| Offset | Bytes | Field | Value |
|---|---|---|---|
| 0 | 41 A8 | frame control | 0xA841 |
| 2 | 22 | sequence | 0x22 |
| 3 | 10 00 | net id | 0x0010 |
| 5 | FF FF | destination | broadcast |
| 7 | 02 00 | source | node 2 |
| 9 | 10 | time quality | 0x10 |
| 10 | 00 | flags | none set |
| 11 | 00 00 | break-in invoker | none |
| 13 | 00 | counts | no Block ACK, no link report |
| 14 | 03 | slot bytes | 3 |
| 15 | 00 02 00 | slot claims | 00 02 00 |
| 18 | F0 | message protocol | test protocol 0xF0 |
| 19 | 01 00 | message destination | node 1 |
| 21 | FC A0 | length and flags | 0xA0FC: length 252, class 4, acknowledgement requested |
| 23 | 08 | link sequence | 8 |
| 24 | FF FE FD FC | data, first 4 bytes | first bytes of the pattern |

The data runs from offset 24 to offset 275, 252 bytes. The CRC-32 the PL adds on the air, or checks and removes on receive, is computed over bytes 0 to 275. Its value is CRC-32 0xAADDAD8E, sent least significant byte first: 8E AD DD AA. It is not in the ring frame.

TX descriptor (software side only, the PL does not read it), 32 bytes:

```
01 00 00 00 14 01 04 00 35 12 00 00 07 00 05 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
```

| Offset | Bytes | Field | Value |
|---|---|---|---|
| 0 | 01 00 00 00 | flags | READY |
| 4 | 14 01 | length | 276 |
| 6 | 04 | traffic class | 4, data |
| 7 | 00 | reserved | zero |
| 8 | 35 12 00 00 | target superframe | 0x1235 |
| 12 | 07 00 | target frame | 7 |
| 14 | 05 00 | target slot | 5 |
| 16 | 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 | reserved (16 bytes) | zero |

What the PL receives for this frame: the reply to the pull for superframe 0x1235, frame 7. It is a list of one item: slot 5, traffic class 4, and the 276 frame bytes above, unchanged. The PL then appends the CRC-32.


## FEC on: 208-byte frame

The PL frame is 208 bytes. The ring frame is the PDU without its CRC-32, 204 bytes. The PL appends the 4-byte CRC-32 on transmit and checks and removes it on receive, so the PDU and the CRC-32 together fill the PL frame and nothing is padded. The message data is 180 bytes (204 minus 18 bytes of PDU overhead, minus 6 bytes of message header).

### Receive reference frame, 204 bytes

Direction: node 1 sends to node 2, received by node 2. The data bytes count up from 00.

Frame bytes:

```
41 A8 21 10 00 FF FF 01 00 10 00 00 00 00 03 20
00 00 F0 02 00 B4 A0 07 00 01 02 03 04 05 06 07
08 09 0A 0B 0C 0D 0E 0F 10 11 12 13 14 15 16 17
18 19 1A 1B 1C 1D 1E 1F 20 21 22 23 24 25 26 27
28 29 2A 2B 2C 2D 2E 2F 30 31 32 33 34 35 36 37
38 39 3A 3B 3C 3D 3E 3F 40 41 42 43 44 45 46 47
48 49 4A 4B 4C 4D 4E 4F 50 51 52 53 54 55 56 57
58 59 5A 5B 5C 5D 5E 5F 60 61 62 63 64 65 66 67
68 69 6A 6B 6C 6D 6E 6F 70 71 72 73 74 75 76 77
78 79 7A 7B 7C 7D 7E 7F 80 81 82 83 84 85 86 87
88 89 8A 8B 8C 8D 8E 8F 90 91 92 93 94 95 96 97
98 99 9A 9B 9C 9D 9E 9F A0 A1 A2 A3 A4 A5 A6 A7
A8 A9 AA AB AC AD AE AF B0 B1 B2 B3
```

| Offset | Bytes | Field | Value |
|---|---|---|---|
| 0 | 41 A8 | frame control | 0xA841 |
| 2 | 21 | sequence | 0x21 |
| 3 | 10 00 | net id | 0x0010 |
| 5 | FF FF | destination | broadcast |
| 7 | 01 00 | source | node 1 |
| 9 | 10 | time quality | 0x10 |
| 10 | 00 | flags | none set |
| 11 | 00 00 | break-in invoker | none |
| 13 | 00 | counts | no Block ACK, no link report |
| 14 | 03 | slot bytes | 3 |
| 15 | 20 00 00 | slot claims | 20 00 00 |
| 18 | F0 | message protocol | test protocol 0xF0 |
| 19 | 02 00 | message destination | node 2 |
| 21 | B4 A0 | length and flags | 0xA0B4: length 180, class 4, acknowledgement requested |
| 23 | 07 | link sequence | 7 |
| 24 | 00 01 02 03 | data, first 4 bytes | first bytes of the pattern |

The data runs from offset 24 to offset 203, 180 bytes. The CRC-32 the PL adds on the air, or checks and removes on receive, is computed over bytes 0 to 203. Its value is CRC-32 0x1C3C4B58, sent least significant byte first: 58 4B 3C 1C. It is not in the ring frame.

RX descriptor, 32 bytes:

```
03 00 00 00 CC 00 05 00 34 12 00 00 78 00 00 00
C0 FE 50 00 78 00 0F 00 E8 03 00 00 00 00 00 00
```

| Offset | Bytes | Field | Value |
|---|---|---|---|
| 0 | 03 00 00 00 | flags | FILLED and CRC_OK |
| 4 | CC 00 | length | 204 |
| 6 | 05 00 | slot in frame | 5 |
| 8 | 34 12 00 00 | superframe | 0x1234 |
| 12 | 78 00 00 00 | arrival offset | +120 ns |
| 16 | C0 FE | RSSI | -80.00 dBm |
| 18 | 50 00 | SNR | 20.00 dB |
| 20 | 78 00 | RX gain | 30.00 dB |
| 22 | 0F 00 | metric flags | RSSI, SNR, gain, arrival offset valid; RSSI not calibrated |
| 24 | E8 03 00 00 | receive count | 1000 |
| 28 | 00 00 00 00 | reserved | zero |

### Transmit reference frame, 204 bytes

Direction: node 2 sends to node 1. The data bytes count down from FF.

Frame bytes:

```
41 A8 22 10 00 FF FF 02 00 10 00 00 00 00 03 00
02 00 F0 01 00 B4 A0 08 FF FE FD FC FB FA F9 F8
F7 F6 F5 F4 F3 F2 F1 F0 EF EE ED EC EB EA E9 E8
E7 E6 E5 E4 E3 E2 E1 E0 DF DE DD DC DB DA D9 D8
D7 D6 D5 D4 D3 D2 D1 D0 CF CE CD CC CB CA C9 C8
C7 C6 C5 C4 C3 C2 C1 C0 BF BE BD BC BB BA B9 B8
B7 B6 B5 B4 B3 B2 B1 B0 AF AE AD AC AB AA A9 A8
A7 A6 A5 A4 A3 A2 A1 A0 9F 9E 9D 9C 9B 9A 99 98
97 96 95 94 93 92 91 90 8F 8E 8D 8C 8B 8A 89 88
87 86 85 84 83 82 81 80 7F 7E 7D 7C 7B 7A 79 78
77 76 75 74 73 72 71 70 6F 6E 6D 6C 6B 6A 69 68
67 66 65 64 63 62 61 60 5F 5E 5D 5C 5B 5A 59 58
57 56 55 54 53 52 51 50 4F 4E 4D 4C
```

| Offset | Bytes | Field | Value |
|---|---|---|---|
| 0 | 41 A8 | frame control | 0xA841 |
| 2 | 22 | sequence | 0x22 |
| 3 | 10 00 | net id | 0x0010 |
| 5 | FF FF | destination | broadcast |
| 7 | 02 00 | source | node 2 |
| 9 | 10 | time quality | 0x10 |
| 10 | 00 | flags | none set |
| 11 | 00 00 | break-in invoker | none |
| 13 | 00 | counts | no Block ACK, no link report |
| 14 | 03 | slot bytes | 3 |
| 15 | 00 02 00 | slot claims | 00 02 00 |
| 18 | F0 | message protocol | test protocol 0xF0 |
| 19 | 01 00 | message destination | node 1 |
| 21 | B4 A0 | length and flags | 0xA0B4: length 180, class 4, acknowledgement requested |
| 23 | 08 | link sequence | 8 |
| 24 | FF FE FD FC | data, first 4 bytes | first bytes of the pattern |

The data runs from offset 24 to offset 203, 180 bytes. The CRC-32 the PL adds on the air, or checks and removes on receive, is computed over bytes 0 to 203. Its value is CRC-32 0x61079B76, sent least significant byte first: 76 9B 07 61. It is not in the ring frame.

TX descriptor (software side only, the PL does not read it), 32 bytes:

```
01 00 00 00 CC 00 04 00 35 12 00 00 07 00 05 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
```

| Offset | Bytes | Field | Value |
|---|---|---|---|
| 0 | 01 00 00 00 | flags | READY |
| 4 | CC 00 | length | 204 |
| 6 | 04 | traffic class | 4, data |
| 7 | 00 | reserved | zero |
| 8 | 35 12 00 00 | target superframe | 0x1235 |
| 12 | 07 00 | target frame | 7 |
| 14 | 05 00 | target slot | 5 |
| 16 | 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 | reserved (16 bytes) | zero |

What the PL receives for this frame: the reply to the pull for superframe 0x1235, frame 7. It is a list of one item: slot 5, traffic class 4, and the 204 frame bytes above, unchanged. The PL then appends the CRC-32.
