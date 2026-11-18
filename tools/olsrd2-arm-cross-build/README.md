# F01-AK-5: OLSRd2 cross-compiled for the Zynq reference board

Builds OLSRd2 (OONF v0.15.1) for the Zynq reference board, installs it
on the board, and forms a two-node route with a PC running the same version.

## Board

| Item | Value |
|---|---|
| Board | Zynq reference board (Zynq-7000, dual Cortex-A9, ARMv7 hard-float) |
| Linux | PetaLinux 2019.1, kernel 4.19.0-xilinx-v2019.1 |
| C library | glibc 2.28 (`/lib/libc-2.28.so`) |
| Tools on board | `nc`, `telnet`, `ssh`, `scp`, `dropbear`, `wget`, `tftp`. No `ldd`. |
| Network | `eth0`, set to DHCP in `/etc/network/interfaces` |

## Build method

The board's PetaLinux 2019.1 SDK is not available. The build uses the Debian 10
(buster) cross compiler instead, which has the same versions as the board:
GCC 8.3 and glibc 2.28. The binary is linked dynamically against the board's
own C library.

## Files

| File | What it is |
|---|---|
| `Dockerfile.arm` | Build image: Debian 10 with the ARM hard-float cross compiler, CMake and git |
| `arm-linux-gnueabihf.cmake` | CMake toolchain file for the cross compiler |
| `build-arm.sh` | Builds the image, then OONF v0.15.1, and writes `out/olsrd2` and `out/build.log` |
| `olsrd2-board.conf` | OLSRd2 config for board and PC. Default timers, no fixed interface. |

## 1. Build

```bash
./build-arm.sh
```

The OONF source comes from a local clone, `~/projects/OONF` by default
(`OONF_SRC=<path>` to change), checked out at tag `v0.15.1` inside the build.
The local clone is mounted read-only and is not changed.

`out/build.log` records the compiler, the target C library, the OONF tag and
hash, the CMake command, and checks on the binary. Expected:

- `file`: `ELF 32-bit LSB executable, ARM, EABI5 ... dynamically linked,
  interpreter /lib/ld-linux-armhf.so.3`
- Needed libraries: only C library parts (`libc`, `libpthread`, `librt`,
  `libdl`, `libm`), all present on the board.
- Highest GLIBC version needed: 2.28 or lower.

## 2. Network

Direct cable from the board's `eth0` to a free PC port, with fixed addresses:

```sh
# board
ip addr add 192.168.50.2/24 dev eth0
ip link set eth0 up
```

```bash
# PC (eno2 here; use the port cabled to the board)
sudo ip link set eno2 up
sudo ip addr add 192.168.50.1/24 dev eno2
ping -c 3 192.168.50.2
```

On the office LAN instead, the board gets an address by DHCP (`ifup eth0`),
and the PC side uses its LAN interface.

## 3. Install on the board

```bash
scp out/olsrd2 root@192.168.50.2:/usr/local/sbin/olsrd2
ssh root@192.168.50.2 'mkdir -p /etc/olsrd2'
scp olsrd2-board.conf root@192.168.50.2:/etc/olsrd2/olsrd2.conf
ssh root@192.168.50.2 'chmod +x /usr/local/sbin/olsrd2
  /lib/ld-linux-armhf.so.3 --list /usr/local/sbin/olsrd2
  /usr/local/sbin/olsrd2 -v'
```

The board has no `ldd`. `ld-linux-armhf.so.3 --list` gives the same output:
every library must show a path, none `not found`. `olsrd2 -v` must show
`v0.15.1-0-gbffb88b0`.

The board's root filesystem is on the SD card, so the files stay after a
reboot.

## 4. Two-node route

Board:

```sh
/usr/local/sbin/olsrd2 -l /etc/olsrd2/olsrd2.conf eth0
```

PC, the same OLSRd2 version from the F01-AK-2 image, on the host network:

```bash
docker run --rm -it --name olsr-pc --network host --cap-add NET_ADMIN --cap-add NET_RAW \
  -v "$PWD/olsrd2-board.conf:/etc/olsrd2/pc.conf:ro" \
  f01ak2-olsrd2 olsrd2 -l /etc/olsrd2/pc.conf eno2
```

After about 15 s, on the board:

```sh
printf 'nhdpinfo link\nquit\n'     | nc 127.0.0.1 2009
printf 'nhdpinfo neighbor\nquit\n' | nc 127.0.0.1 2009
printf 'olsrv2info route\nquit\n'  | nc 127.0.0.1 2009
ip route
ping -c 10 192.168.50.1
```

On the PC:

```bash
docker exec olsr-pc sh -c "printf 'nhdpinfo link\nquit\n' | nc -N 127.0.0.1 2009"
docker exec olsr-pc sh -c "printf 'olsrv2info route\nquit\n' | nc -N 127.0.0.1 2009"
ip route
ping -c 10 192.168.50.2
```

Pass:

- Each side lists the other as `symmetric` in `nhdpinfo link`.
- Each side has an OLSRd2 route to the other (`proto 100` in `ip route`).
- Ping works both ways.

If no neighbour appears:

- Check the interface name on both sides. The wrong interface gives no
  neighbours and no error.
- `sudo tcpdump -ni eno2 udp port 269` must show packets from both IPs
  every 2 s.
- Check the PC firewall (`sudo ufw status`).
