# dataterm

Data terminal application for task F02-AK-4. It sends and receives text messages between nodes as CoAP (RFC 7252) over UDP on the routed network. Unicast messages are acknowledged and retried, and the receiver counts lost messages from Message ID gaps.

It is an ordinary Linux socket program built on libcoap. Routing (OLSRd2) and IP forwarding are done by Linux. It never uses the `radiod` control socket.

## Message

One message is one CoAP POST to `/msg` with Content-Format 0 (plain UTF-8 text), on UDP port 5683.

| Part | Value |
|---|---|
| Version | 1 |
| Type | CON, acknowledged and retried. NON when `--retries 0` |
| Code | 0.02 POST |
| Message ID | 16 bits, counting up per destination |
| Token | 4 random bytes |
| Options | Uri-Path `msg`, Content-Format 0 |
| Payload | `0xFF` marker, then the text |

"HELLO" is 19 bytes on the wire: `44 02 <mid> <token> B3 6D 73 67 10 FF 48 45 4C 4C 4F`. The answer is an ACK with 2.04 Changed, 8 bytes: `64 44 <mid> <token>`.

The sender is named by its IP address, looked up in the `--peer` table. A message from an address not in the table is still delivered and acknowledged, and is shown by IP.

## Versioning

A newer sender can add features without breaking older receivers:

- An unknown elective option (even option number) is skipped, and the message is delivered.
- An unknown critical option (odd option number) is refused: 4.02 Bad Option on a CON, a reject on a NON. The sender then knows to fall back.
- A Content-Format other than 0 is refused with 4.15 Unsupported Content-Format.
- A message with a CoAP version other than 1 is not accepted.

## Behaviour

- libcoap sends the ACKs and RSTs and runs the retry timer. The first wait is `--timeout` (2000 ms, at least 1000 ms) plus up to 50 % at random, and it doubles on each of `--retries` retransmissions (4).
- `--retries 0` sends each message once as NON. The 2.04 reply is waited for 1.5 times `--timeout`, and a message with no reply counts as given up.
- One message is on the air per destination at a time. Up to 8 per destination can be outstanding; more are refused with "busy".
- Each message is delivered once. A repeat (lost ACK, sender retried) is acknowledged again but not delivered twice.
- A jump in Message ID is counted as missing messages (`event=gap`). When a sender restarts, its source port changes and counting starts again (`event=rx_sender_restart`), so a restart is not counted as loss.
- Messages over 1232 bytes are refused, so the IP packet fits 1280 bytes. Don't-fragment is set on every socket.
- DSCP is set on every socket with `--dscp` (default 0, the data class value is not decided yet). libcoap gives no access to its sockets, so dataterm finds them among its own open files.
- The node id to IP table comes from `--peer`.

## Use

```bash
# Board B, node 2: receive and acknowledge
dataterm -n 2 --peer 1=10.0.1.1 recv

# Board A, node 1: interactive chat to node 2
dataterm -n 1 --peer 2=10.0.2.2 chat 2

# Board A, node 1: scripted test, 1000 messages of 64 bytes every 50 ms
dataterm -n 1 --peer 2=10.0.2.2 send 2 --count 1000 --interval 50 --size 64

# Same with retry off (NON), to measure raw link loss
dataterm -n 1 --peer 2=10.0.2.2 --retries 0 send 2 --count 1000 --interval 50 --size 64
```

`dataterm --help` lists every option. Logs go to stderr as `key=value` records. At exit each side logs `event=stats`, and `event=summary_tx` or `event=summary_rx` with loss percentages. `recv` stops on Ctrl-C or after `--duration` seconds.

## Counters for the test log

| Record | Field | Meaning |
|---|---|---|
| `summary_tx` | `acked`, `given_up` | Messages acknowledged, and given up after all retries |
| `summary_tx` | `rst`, `refused`, `unreachable` | Peer reset, peer error code, ICMP or socket error |
| `summary_tx` | `residual_loss_pct` | Messages not acknowledged |
| `summary_rx` | `missing`, `loss_pct` | Messages missing from Message ID gaps |
| `summary_rx` | `duplicates` | Repeats seen and not delivered again |

libcoap retries inside the library, so there is no per-message retry count. The run with `--retries 0` gives the raw link loss.

## Dependency: libcoap

dataterm needs libcoap 4.3, found by `pkg-config` (`libcoap-3` or `libcoap-3-notls`) or as libcoap's CMake package (`CMAKE_PREFIX_PATH`). If CMake finds neither, dataterm and its tests are skipped with a warning.

- Host: `sudo apt install libcoap3-dev` (Ubuntu 22.04 gives 4.3.0), or a source build installed in `/usr/local`. The code builds with 4.3.0, release 4.3.5, and the newer development code installed on this PC.
- Board: built from source in the cross-build image, step 2 below.

## Cross-build for the board

The board is the Zynq reference board: ARMv7 hard-float, PetaLinux 2019.1, glibc 2.28. The build uses the Debian 10 cross compiler image from `tools/olsrd2-arm-cross-build`, which matches the board's GCC 8.3 and glibc 2.28. See that folder's README for the board details.

### 1. Build image (once)

Skip this if `docker image ls | grep f01ak5-armbuild` already lists the image.

```bash
# from the repo root
docker build -t f01ak5-armbuild \
  -f tools/olsrd2-arm-cross-build/Dockerfile.arm tools/olsrd2-arm-cross-build
```

### 2. Build libcoap for ARM

libcoap release 4.3.5, static, without DTLS, TCP or thread locking, installed into `build-arm/coap`. libcoap's CMake file asks for a C++ compiler but has no C++ code, and the image has none, so the copy is set to C only.

```bash
# from the repo root
mkdir -p build-arm
rm -rf build-arm/libcoap-4.3.5 build-arm/coap-build build-arm/coap
git -C ~/tools/libcoap archive --prefix=libcoap-4.3.5/ v4.3.5 | tar -x -C build-arm
sed -i 's/LANGUAGES CXX C)/LANGUAGES C)/' build-arm/libcoap-4.3.5/CMakeLists.txt
docker run --rm --user "$(id -u):$(id -g)" -e HOME=/tmp \
  -v "$PWD:/src:ro" -v "$PWD/build-arm:/build" f01ak5-armbuild bash -euc '
    cmake -S /build/libcoap-4.3.5 -B /build/coap-build -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_TOOLCHAIN_FILE=/src/tools/olsrd2-arm-cross-build/arm-linux-gnueabihf.cmake \
      -DCMAKE_INSTALL_PREFIX=/build/coap -DBUILD_SHARED_LIBS=OFF \
      -DENABLE_DTLS=OFF -DENABLE_TCP=OFF -DENABLE_THREAD_SAFE=OFF -DENABLE_OSCORE=OFF \
      -DENABLE_WS=OFF -DENABLE_DOCS=OFF -DENABLE_EXAMPLES=OFF -DENABLE_TESTS=OFF
    cmake --build /build/coap-build -- -j "$(nproc)"
    cmake --build /build/coap-build --target install' 2>&1 | tee build-arm/coap-build.log
```

### 3. Build `dataterm`

```bash
# from the repo root
docker run --rm --user "$(id -u):$(id -g)" -e HOME=/tmp \
  -v "$PWD:/src:ro" -v "$PWD/build-arm:/build" f01ak5-armbuild bash -euc '
    cmake -S /src -B /build -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_TOOLCHAIN_FILE=/src/tools/olsrd2-arm-cross-build/arm-linux-gnueabihf.cmake \
      -DCMAKE_PREFIX_PATH=/build/coap
    cmake --build /build --target dataterm -- -j "$(nproc)"
    cd /build/apps/dataterm
    file dataterm
    arm-linux-gnueabihf-readelf -d dataterm | grep NEEDED
    arm-linux-gnueabihf-objdump -T dataterm | grep -o "GLIBC_[0-9.]*" | sort -uV | tail -1
    sha256sum dataterm' 2>&1 | tee build-arm/build.log
```

The binary is `build-arm/apps/dataterm/dataterm`. Check `build-arm/build.log`:

- CMake prints `dataterm: libcoap 4.3.5`.
- `file` shows `ELF 32-bit LSB executable, ARM, EABI5 ... dynamically linked, interpreter /lib/ld-linux-armhf.so.3`.
- `NEEDED` lists only C library parts (`libc`, and possibly `libm` or `librt`). libcoap is linked in statically.
- The highest GLIBC version is 2.28 or lower.

### 4. Install on each board

```bash
B=192.168.50.2     # board address, repeat for the second board
scp build-arm/apps/dataterm/dataterm root@$B:/usr/local/bin/dataterm
ssh root@$B 'chmod +x /usr/local/bin/dataterm
  /lib/ld-linux-armhf.so.3 --list /usr/local/bin/dataterm
  /usr/local/bin/dataterm --help | head -3'
```

The board has no `ldd`. `ld-linux-armhf.so.3 --list` must show a path for every library, none `not found`.

### 5. Two-board run

Start OLSRd2 on both boards first, as in `tools/olsrd2-arm-cross-build/README.md` step 4, and check each board has a route to the other with `ip route`. Then, with board A as node 1 and board B as node 2:

```sh
# board B
dataterm -n 2 --peer 1=<board-A-ip> recv --duration 120 2> /tmp/dataterm-rx.log

# board A, while B is running
dataterm -n 1 --peer 2=<board-B-ip> send 2 --count 1000 --interval 50 --size 64 2> /tmp/dataterm-tx.log
```

Run it a second time with `--retries 0` on board A to measure raw link loss. Copy the logs back to `evidence/f02-ak4/` with the build logs and the `ip route` output of both boards:

```bash
mkdir -p evidence/f02-ak4
scp root@<board-A-ip>:/tmp/dataterm-tx.log root@<board-B-ip>:/tmp/dataterm-rx.log evidence/f02-ak4/
cp build-arm/build.log build-arm/coap-build.log evidence/f02-ak4/
```

The loss figures are in the `event=summary_tx` and `event=summary_rx` lines (see "Counters for the test log").

## Tests

- `test_dataterm_peers`: the node id to address table.
- `test_dataterm_coap`: two instances over loopback with a lossy relay between them. Covers delivery, a lost message counted as a gap, recovery by retransmission, a lost ACK, a sender restart, the limits, and DSCP and don't-fragment on every socket.
- `test_dataterm_wire`: a raw UDP socket as the peer. Covers the exact bytes sent, the ACK, the versioning rules, an unknown sender, and RST and error replies.

They run in real time with libcoap's minimum timeout of 1 s, about 6 s in all. Run with `ctest`.

## Open points

- The board run on two Rev3 boards is not done yet.
- The DSCP value for the data class is not decided.
- The node id to IP table (D11) is not yet confirmed by the project owner.
- Group and broadcast messages (NON to a multicast address) are not in scope yet.
- Message security (OSCORE, RFC 8613) waits for the security decision.
