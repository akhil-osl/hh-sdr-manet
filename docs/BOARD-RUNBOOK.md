# Board runbook: start the radio and run the tests

Commands for the two Zynq reference boards, as used and checked on the boards. Run the board commands in the board's serial console. Each command says what it does. Anything marked NOT CHECKED was not run.

## 1. What is where

| Item | Value |
|---|---|
| Board 1 console | `/dev/ttyUSB0`, 115200 baud, 8 data bits, no parity, 1 stop bit |
| Board 2 console | `/dev/ttyUSB1`, same settings |
| Login | user `root`. The password is held by the team and is not written here. |
| Board prompt | `root@sdr_prj:` |
| Files on the board | `/opt/radhawk_each_worker_status` |
| Radio control tool | `./radioctl -s /tmp/r.sock` (run in that folder) |
| Radio log | `/tmp/sca-node/radiod.log` (`/tmp` is a link to `/var/tmp`) |
| Ethernet | PC `192.168.10.1` on `eno1`, board `192.168.10.2`. Both boards have the same address and MAC, so only one board may be on the Ethernet at a time. |

Only one program can hold a serial port. If a command says the port is open in another program, close that program first.

In every board section below, first run:
```
cd /opt/radhawk_each_worker_status
R="./radioctl -s /tmp/r.sock"
```
The second line sets a short name `$R` for the radio control tool.

## 2. State after a board boots

The boot sequence starts three programs:
- `radiod` (the radio daemon) with the ring config `/tmp/sca-node/radiod_rings.conf`
- `rh_agent` (started by `radiod`)
- `mac_daemon -n 1` (a MAC process for node 1)

The radio application is not started yet. At this point `$R status` answers `ESTATE` (rejected), and reading a live counter answers "start the application first". This is normal.

Check what runs:
```
ps | grep -E 'radiod|rh_agent|mac_' | grep -v grep
```

## 3. Stop mac_daemon

Only one MAC process may run, and it holds the lock `/tmp/hh_mac.lock`. `mac_peer` and `hh-node` refuse to start while `mac_daemon` runs. Take the number in the first column of its line from the `ps` output above:
```
kill <PID>
sleep 1
ps | grep mac_daemon | grep -v grep
```
The last line must print nothing. A reboot starts it again.

## 4. Start the radio

These six commands start the application and the transmitter, using PS-PRBS test data:
```
$R opencpi set app xml qpsk_voice_fec_app.xml
$R opencpi set app tx_source 1
$R init
$R configure 1
$R start
$R opencpi start tx
```
- Run them in this order. If one prints an error, stop and note the message.
- If the radio is already running, `init`, `configure` and `start` answer `ESTATE`, and the `xml` setting answers that it can only be set while the application is stopped. Then only `opencpi start tx` matters.
- Do not use `radio.sh` or `radio_up.sh` together with `hh-node`. They start `mac_daemon` again, which gives two writers on the TX ring.

## 5. Check the radio is alive

```
$R status
$R opencpi get rx_ber crc_ok_count; sleep 2; $R opencpi get rx_ber crc_ok_count
$R opencpi get rx_ber crc_bad_count
$R opencpi get rx_fts link_status
$R opencpi get tx enable
```
- `status` should say `state=running`.
- After a fresh start `crc_ok_count` and `crc_bad_count` are 0 until frames flow. With a MAC process sending, `crc_ok_count` rises.
- `link_status` 7 means the receiver is locked. It is not a reliable loss flag: with the far transmitter off it still read 7 in 86 of 88 samples. Watching `crc_ok_count` and `crc_bad_count` stop rising is the reliable sign of loss.
- `crc_bad_count` also counts the gap slots between MAC frames, so it is not an error rate.
- If both counters hold the same huge number (seen once: 3235791364) and do not move, the board is in a bad state. Run `$R opencpi start tx` and read them again.

Turn the transmitter off and on (for a link-break test):
```
$R opencpi set tx enable 0
$R opencpi start tx
```

## 6. One board in loopback with mac_peer

`mac_peer` is the owner's test MAC process. `-s` makes it decode its own frames, so one board tests itself over the cable loop. Run it after steps 3 to 5:
```
./mac_peer -n 1 -p 2 -w 52 -t 30 -s -i 20 2>&1 | tail -n 8
```
- `-n 1` is this node. `-p 2` is the peer. `-w 52` is the frame width in words (FEC 3/4). `-t 30` runs 30 s. `-i 20` sends a payload every 20 ms.
- Read the last line: `payloads received`, `lost` and `WRONG`. A pass is received above 0, lost 0 and WRONG 0.
- Measured: this run returned 1440 of 1440 payloads with the build from 26 Nov. Intervals of 10 ms or less lose almost everything (BUG-02 in `docs/BUGS.md`).

## 7. Two boards with hh-node and ping

This is the run from 25 Nov that gave 10 of 10 pings. Run steps 3 to 5 on both boards first. `hh-node` is our MAC process and creates the interface `manet0`. The `hh-node` file was copied to `/opt/radhawk_each_worker_status` on each board.

Board 1:
```
cd /opt/radhawk_each_worker_status
./hh-node --node 1 --rings /tmp/hh_rings --capacity 204 --duration 900 --summary /tmp/hhnode.sum > /tmp/hhnode.out 2> /tmp/hhnode.err &
sleep 3; cat /tmp/hhnode.err
```
It should print `node 1 up on manet0, max packet 172 bytes`. Then set the interface:
```
ip link set manet0 address 02:00:00:00:00:01 mtu 172
ip addr add 10.99.0.1/24 dev manet0
ip link set manet0 up
ip neigh add 10.99.0.2 lladdr 02:00:00:00:00:02 dev manet0 nud permanent
```
Board 2: the same, with `--node 2`, address `02:00:00:00:00:02`, `10.99.0.2/24`, and the neighbour `10.99.0.1` with `lladdr 02:00:00:00:00:01`.

Then ping from board 1:
```
ping -c 10 -W 3 10.99.0.2
```
The board's ping has no `-i` option. To watch the packets: `tcpdump -i manet0`.

`--duration 900` stops `hh-node` after 15 minutes. Use a larger number for a longer run.

## 8. Replace rh_agent on a board

Built on the machine `OPLS` (section 10). Steps, from this PC:

1. On the board, keep the old file:
```
cd /opt/radhawk_each_worker_status
cp -p rh_agent rh_agent.bak_<date>
```
2. From this PC, copy the new one to a temporary name (it asks for the board password):
```
scp <path to rh_agent> root@192.168.10.2:/opt/radhawk_each_worker_status/rh_agent.new
```
3. On the board, check it against the md5 printed on the PC, then swap:
```
md5sum rh_agent.new
mv rh_agent.new rh_agent && chmod 755 rh_agent
reboot
```
4. After the reboot do steps 2 to 5. The running `rh_agent` stays the old one until the reboot.

Current builds (md5): old `rh_agent` from the owner, sha256 `dbc99638c2fa1198dc769a0106452a023a1a3f51fc30e95396d109a0dc0f202e`. New one, md5 `0f3a816e39b738eeb627c5cf82a31b14`, sha256 `c48c8d39ec1df212c7d85273d7f788960669fe66ba715a4ff0b6c8f95565dbe6`, with the `rx_extract.c` change.

## 9. Read the radio's own counters and log

The last transmit and receive counters of `rh_agent`:
```
grep MACLINE /tmp/sca-node/radiod.log | tail -n 4
```
`tx=` is frames taken from the TX ring, `rx=` frames delivered to the RX ring, `locks=` frames found by `rx_extract`.

PL counters, read before and after a test:
```
g() { printf '%s.%s=' $1 $2; $R opencpi get $1 $2 | grep -o 'value=[^ ]*' | cut -d= -f2; }
g tx_prbs_src dbg_in_words
g tx_bit_ser dbg_in_underrun
g tx_qpsk_mod dbg_sym_count
g tx_rrc dbg_out_dropped
g rx_packer overflow
g rx_ber dbg_out_words
g rx_ber crc_ok_count
g rx_ber crc_bad_count
```
`dbg_in_words` rises by the frame words the PS sent. `dbg_in_underrun` counts words the PL re-sent because the PS had nothing, so it rises between frames.

## 10. Build rh_agent

### 10.1 On this workstation (Ubuntu 22.04)

The folders `worker_project`, `xilinx_install` and the `_mac_connect` project tree were copied from `OPLS` into `~/OPENCPI_PROJECT`. OpenCPI 2.4.7 does not recognise Ubuntu 22.04 as a host, and its Python helper module is not found. The build works when the host platform is set to `ubuntu18_04` by hand. From the project tree (the `_mac_connect` folder):
```
source ./setup-env.sh
export OCPI_TOOL_PLATFORM=ubuntu18_04 OCPI_TOOL_DIR=ubuntu18_04 OCPI_TOOL_PLATFORM_DIR=ubuntu18_04
export OCPI_TOOL_OS=linux OCPI_TOOL_OS_VERSION=18.04 OCPI_TOOL_ARCH=x86_64
export PATH=$OCPI_CDK_DIR/ubuntu18_04/bin:$PATH
export PYTHONPATH=$OCPI_CDK_DIR/ubuntu18_04/lib:$PYTHONPATH
cd applications/rh_agent
make RccPlatforms=xilinx19_2_aarch32
sha256sum target-xilinx19_2_aarch32/rh_agent
```
Measured on 26 Nov 2026: with the old `target-xilinx19_2_aarch32` folder deleted, this rebuilt everything and gave sha256 `c48c8d39ec1df212c7d85273d7f788960669fe66ba715a4ff0b6c8f95565dbe6`, the same as the build on `OPLS`. Only `rh_agent` was built here. The ARM `ocpidev` command itself does not run on Ubuntu 22.04 (its Python module is missing without the settings above), and nothing else was tried.

### 10.2 On OPLS

In the project tree under `~/OPENCPI_PROJECT`:
```
source ./setup-env.sh
cd applications/rh_agent
make RccPlatforms=xilinx19_2_aarch32
file target-xilinx19_2_aarch32/rh_agent
sha256sum target-xilinx19_2_aarch32/rh_agent
```
- Run `make` by hand. `deploy/mac_line_20261125/make.sh` hides the build output.
- `file` must say ELF 32-bit, ARM.
- Check the source first: `grep -n RXX_FC_MORE_DATA api/rx_extract.c` must print two lines.
- `make.sh` overwrites `rh_agent`, `mac_daemon` and `mac_peer` in `deploy/mac_line_20261125`. Copy the old files first.
- Host test without hardware: `sh test/mac_line/run.sh`, from a short folder path (ring file paths are limited to 107 characters).

## 11. Traps

- Put `-c` last in any `radiod` command line.
- Use `qpsk_voice_fec_app.xml` and `tx_source 1`. Use PS-PRBS only, never fabric PRBS.
- `mac_daemon` runs after every boot. Stop it before `mac_peer` or `hh-node`.
- The boards have no real-time clock. Their dates read February 2026.
- Busybox `ping` has no `-i`.
- Both boards have the same Ethernet address, so one board at a time on the cable.
- A running binary cannot be written in place. Copy to `.new`, then `mv`.

## 12. Known problem

Heavy traffic is lost on one board in loopback (BUG-02 in `docs/BUGS.md`). The more-data bit that the scheduler sets when packets are still queued may be the cause. It is not proven. Light traffic and ping work.
