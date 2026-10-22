# dataterm

Data terminal application for task F02-AK-4. It sends and receives text messages between nodes over UDP on the routed network, with acknowledgement and retry by sequence number, and counts loss from sequence gaps.

It is an ordinary Linux socket program. Routing (OLSRd2) and IP forwarding are done by Linux. It never uses the `radiod` control socket.

## Message

One message is one UDP datagram: a 20-byte header and the text. Big endian.

| Offset | Size | Field | Use |
|---|---|---|---|
| 0 | 1 | Version | 1 |
| 1 | 1 | Type | 1 DATA, 2 ACK |
| 2 | 1 | Flags | Bit 0: ACK requested |
| 3 | 1 | Attempt | 0 on first send, plus 1 per retry. The ACK echoes it |
| 4 | 4 | Source node id | Original sender |
| 8 | 4 | Destination node id | Final receiver |
| 12 | 4 | Sequence number | Starts at 1 per destination. The ACK echoes it |
| 16 | 2 | Payload length | Must match the datagram size |
| 18 | 2 | CRC | CRC-16-CCITT, poly 0x1021, init 0xFFFF, over header (CRC as 0) and payload |
| 20 | n | Payload | Text |

## Behaviour

- Stop-and-wait: one message in flight per destination, up to 8 queued.
- ACK wait starts at 1000 ms and doubles per retry. At most 4 retries, then the message is given up and logged.
- The receiver acknowledges every DATA, duplicates included, and delivers each sequence once.
- A jump in sequence is counted as missing messages (`event=gap`). A lost first message counts too, since sequences start at 1.
- Messages over 1232 bytes are refused, so the IP packet fits 1280 bytes. The socket sets don't-fragment.
- DSCP is set on the socket with `--dscp` (default 0, the data class value is not decided yet).
- The node id to IP table comes from `--peer`. A receiver also learns the address of whoever sends to it, so a receiver needs no `--peer`.

## Use

```bash
# Board B, node 2: receive and acknowledge
dataterm -n 2 recv

# Board A, node 1: interactive chat to node 2
dataterm -n 1 --peer 2=10.0.2.2 chat 2

# Board A, node 1: scripted test, 1000 messages of 64 bytes every 50 ms
dataterm -n 1 --peer 2=10.0.2.2 send 2 --count 1000 --interval 50 --size 64

# Same with retry off, to measure raw link loss
dataterm -n 1 --peer 2=10.0.2.2 --retries 0 send 2 --count 1000 --interval 50 --size 64
```

`dataterm --help` lists every option. Logs go to stderr as `key=value` records. At exit each side logs `event=stats`, and `event=summary_tx` or `event=summary_rx` with loss percentages. `recv` stops on Ctrl-C or after `--duration` seconds.

## Counters for the test log

| Record | Field | Meaning |
|---|---|---|
| `summary_tx` | `first_try_loss_pct` | Messages not acknowledged on the first try |
| `summary_tx` | `residual_loss_pct` | Messages given up after all retries |
| `summary_rx` | `missing`, `loss_pct` | Messages missing from sequence gaps |
| `stats` | `tx_retries`, `rx_duplicates`, `rx_bad_crc` | Retries sent, duplicates received, corrupted messages |

## Tests

`test_dataterm_msg` (codec and the sample bytes), `test_dataterm` (engine on an emulated lossy link with a virtual clock) and `test_dataterm_udp` (real sockets over loopback). Run with `ctest`.

## Open points

- The board run on two Rev3 boards is not done yet.
- The DSCP value for the data class is not decided.
- The node id to IP table (D11) is not yet confirmed by the project owner.
