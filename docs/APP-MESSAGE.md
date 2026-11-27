# HH-SDR App Message Note

## Document control

| Item | Value |
|---|---|
| Title | HH-SDR app message note |
| Version | 1.0 (draft) |
| Date | 2026-10-28 |
| Author | Akhil |
| Applies to | Data terminal application on the handheld and vehicular radios |
| Status | Draft, not reviewed |

Revision history

| Version | Date | Change |
|---|---|---|
| 1.0 | 2026-10-28 | First issue |

## 1. Purpose

This note describes the message the data terminal sends and receives: its layout, the options it uses, its timers, and where message security will be added. The message is a CoAP message (RFC 7252) over UDP port 5683, built with libcoap.

Source and destination come from the IP addresses. Length comes from UDP. Integrity comes from the UDP checksum and the per-hop MAC CRC-32. The message has no fields of its own for these.

## 2. Layout

One message is one CoAP POST to the resource `/msg`. All multi-byte fields are big-endian.

| Field | Size | Value |
|---|---|---|
| Version | 2 bits | 1 |
| Type | 2 bits | 0 CON for a unicast message, 1 NON for a group or broadcast message |
| Token length | 4 bits | 4 |
| Code | 1 byte | 0x02, POST |
| Message ID | 2 bytes | Counts up by 1 per new message, kept per destination |
| Token | 4 bytes | Random for each request |
| Options | variable | Uri-Path `msg`, Content-Format 0 |
| Payload marker | 1 byte | 0xFF, present when a payload follows |
| Payload | n bytes | Plain UTF-8 text |

Example. The text "HELLO" is 19 bytes (mid is the Message ID, tok the token):

```
44 02 <mid:2> <tok:4> B3 6D 73 67 10 FF 48 45 4C 4C 4F
```

The receiver acknowledges it with an empty-payload reply, 8 bytes, code 2.04 Changed:

```
64 44 <mid:2> <tok:4>
```

The Message ID in the ACK is the one in the request. The token in the reply matches the reply to its request.

## 3. Options used

| Option | Value | Use |
|---|---|---|
| Uri-Path | `msg` | Names the service |
| Content-Format | 0 | Plain UTF-8 text |

Rules for options the receiver does not know:

- An unknown elective option (even number) is skipped and the message is delivered.
- An unknown critical option (odd number) is refused: 4.02 Bad Option for a CON, a reset for a NON.
- A Content-Format other than 0 is refused with 4.15.
- A CoAP version other than 1 is not accepted.

New features come as new options, so radios on different software keep exchanging messages.

## 4. Timers and retries

Timers follow RFC 7252 with its default values.

| Item | Value |
|---|---|
| First ACK timeout | 2 s, plus a random part of up to 1 s, so 2 to 3 s |
| Each retry | The timeout doubles |
| Retries | At most 4 |
| Give up | After the fifth wait, 62 to 93 s after the first send. The message is counted as given up |

The Message ID detects duplicates. A repeat of a message already delivered (for example after a lost ACK) is acknowledged again and not delivered twice.

A unicast message is confirmable (CON). Group and broadcast messages are always non-confirmable (NON): no ACK is asked for, so a group cannot answer all at once. For counting only, a unicast message sent with retries off is also sent as NON.

## 5. Loss counting

- The sender counts messages sent, acknowledged and given up (a CON message that ran out of retries).
- The receiver counts messages delivered, missing, and duplicates. A jump in the Message ID is counted as missing messages.
- If a sender restarts, its source port changes and the receiver starts counting again, so a restart is not counted as loss.

Loss per pair is the sent count against the delivered count for that pair.

## 6. Size

A message is refused if it is over 1232 bytes, so the IP packet fits the 1280-byte minimum. The don't-fragment flag is set, so the application never fragments. Larger data is the job of the lower layers.

## 7. Where message security will be added

Message security is not built. The decision on where it sits is open (OI-1).

The reference is OSCORE (RFC 8613). OSCORE protects the CoAP message itself: it adds an OSCORE option and replaces the payload and the protected options with an encrypted, authenticated body. If OSCORE is chosen:

- It goes inside the app message, between CoAP and the application text. The layout in section 2 stays the same up to the options.
- It protects the message from sender to receiver. The MAC security on each hop is separate and stays.
- Group messages need a group form of OSCORE or another method (OI-1).
- Key handling and zeroise follow the security architecture and are not part of this note.

Until the decision is made, messages are sent unprotected, and only the MAC security on each hop applies once it exists.

## 8. Open items

| Id | Item | State |
|---|---|---|
| OI-1 | Where message security sits, and whether it is OSCORE | TBD, with the security authority |
| OI-2 | Group and broadcast: the destination addresses and the group list | TBD. Not built |
| OI-3 | DSCP value for the data class | TBD. Set by a run option, default 0 |
| OI-4 | Whether the ACK timeout of 2 to 3 s suits the radio round trip across several hops | TBD. Tested on wired Ethernet only |
