# F01-AK-2: OLSRd2 mesh emulation in Docker

Runs OLSRd2 (OLSRv2) on several Linux nodes, one Docker container per node, on
emulated links. It shows that a two-hop route forms, heals after a link break
and carries traffic, before any radio exists.

All commands run from this folder:

```bash
cd tools/f01-ak2
```

If your user is not yet in the `docker` group for this login, wrap each
command in `sg docker -c '...'`.

## Files

| File | What it is |
|---|---|
| `Dockerfile` | Node image: OLSRd2 built from OONF v0.15.1, plus ping, traceroute, iperf3, tcpdump, nft, netcat and GStreamer |
| `olsrd2.conf` | OLSRd2 config used by every node. Timers are left at their defaults |
| `mesh.sh` | Starts and stops the mesh, cuts and restores links, applies waveform profiles |
| `profiles/*.env` | Waveform profiles: RATE, DELAY, JITTER, LOSS, MTU |
| `topologies/*.txt` | Node pairs that cannot hear each other |
| `maoi_bridge.py` | Sends the mesh's OLSRd2 links to the MA-OI topology view |

## How the mesh works

- **Nodes.** Each node is a container (`f01-A`, `f01-B`, ...) with its own
  network stack, routing table and OLSRd2 process. All nodes come from one
  image.
- **Channel.** All nodes sit on one internal Docker network, `f01ak2`
  (10.99.0.0/24), like one radio channel. It has no route to the outside.
- **Addresses.** Node A is 10.99.0.11, B is 10.99.0.12, C is 10.99.0.13, and
  so on.
- **Waveform.** The profile is applied with `tc netem` (rate, delay, jitter,
  loss) and the interface MTU on every node. Each node shapes what it sends,
  so every link is shaped the same in both directions.
- **Links.** Each node has an nftables filter on `eth0` ingress that drops
  frames from MAC addresses in its `blocked` set. Cutting a link adds each
  node's MAC address to the other's set, so neither hears the other: 100%
  loss on that link only, both ways. Nothing tells OLSRd2 the link is gone; it
  finds out when HELLOs stop, as with a radio going out of range.
- **Routing.** Each node forwards IP. The default route is removed, so
  traffic between nodes follows OLSRd2's routes only. ICMP redirects and
  reverse-path filtering are off, so a relay node forwards on the same
  interface without telling the sender to go direct.

OLSRd2 default timers: HELLO every 2 s, valid for 20 s; TC every 5 s, valid
for 300 s. A cut link is noticed when HELLO validity runs out, so route
removal takes about 20 s. Record these values with any repair-time result.

## 1. Build the node image

The build clones OONF from GitHub and builds `olsrd2_static`. No OONF source
is kept in this repo.

The Dockerfile copies the host's CA bundle into the image so HTTPS works from
inside the build. It is not kept in git; copy it first:

```bash
cp /etc/ssl/certs/ca-certificates.crt .
./mesh.sh build 2>&1 | tee build.log | tail -20
```

Check the OONF version in the image:

```bash
docker run --rm f01ak2-olsrd2 cat /etc/oonf-version
```

Expected: `v0.15.1-0-gbffb88b0` and the full commit hash. The build uses
`-fcommon` because v0.15.1 does not link on GCC 10 or later without it.

To build another OONF version:

```bash
docker build --build-arg OONF_COMMIT=<tag or hash> -t f01ak2-olsrd2 .
```

## 2. Start the mesh

```bash
./mesh.sh up 3 chain default
```

Arguments:

- **Node count:** 2 to 26. Nodes are named A, B, C, ...
- **Topology:** a file in `topologies/`. `chain` blocks A-C, so A reaches C
  only through B. `full` blocks nothing.
- **Profile:** a file in `profiles/`.

`up` creates the network and the containers, applies the profile, cuts the
topology's blocked pairs, and starts OLSRd2 on every node. Each step prints a
UTC timestamp.

## 3. Wait for routes, then check them

Give OLSRd2 about 30 s, then:

```bash
./mesh.sh status
```

For each node, `status` prints its IP and MAC, its routing table, its netem
settings and the MAC addresses it blocks.

Expected with `chain`:

- A has `10.99.0.13 via 10.99.0.12`.
- C has `10.99.0.11 via 10.99.0.12`.
- A blocks C's MAC, and C blocks A's.

## 4. Prove the two-hop route A to B to C

```bash
./mesh.sh exec A ip route get 10.99.0.13
./mesh.sh exec A traceroute -n 10.99.0.13
./mesh.sh exec A ping -c 5 10.99.0.13
./mesh.sh info A "nhdpinfo link"
./mesh.sh info A "olsrv2info route"
```

Expected:

- `ip route get` says `via 10.99.0.12`.
- traceroute shows two hops: 10.99.0.12, then 10.99.0.13.
- ping gets replies. The round trip is about 4 x the profile delay: two
  hops, each crossed once in each direction.
- `nhdpinfo link` lists only B as a neighbour of A.
- `olsrv2info route` lists C with next hop 10.99.0.12.

Other OLSRd2 tables: `nhdpinfo neighbor`, `olsrv2info node`,
`olsrv2info edge`.

## 5. Check the links are symmetric

```bash
./mesh.sh exec A ping -c 10 -q 10.99.0.12
./mesh.sh exec B ping -c 10 -q 10.99.0.11
./mesh.sh exec B ping -c 10 -q 10.99.0.13
./mesh.sh exec C ping -c 10 -q 10.99.0.12
```

Expected: all four average round trips are close, about 2 x the profile
delay.

## 6. Break B-C and restore it

Terminal 1, continuous ping with timestamps:

```bash
./mesh.sh exec A ping -D -i 0.2 10.99.0.13
```

Terminal 2:

```bash
./mesh.sh link-down B C
sleep 40
./mesh.sh exec A ip route
./mesh.sh link-up B C
sleep 30
./mesh.sh exec A ip route
```

Expected:

- After `link-down`, pings stop. While B still holds its route to C, A gets
  "Destination Host Unreachable" from B.
- Within about 20 s, A's route to 10.99.0.13 disappears.
- After `link-up`, the route comes back and pings resume.

The times of OLSRd2's route changes, in UTC, to compare with the
`link-down` and `link-up` timestamps:

```bash
./mesh.sh exec A grep -E "Set route|remove route" /var/log/olsrd2.log
```

`link-up` on a link that is not cut prints an nft error. That is expected.

## 7. Change the waveform while running

```bash
./mesh.sh profile degraded
./mesh.sh exec A ping -c 10 -q 10.99.0.13
./mesh.sh profile default
```

`profile NAME B C` applies it to listed nodes only. To add a profile, copy
`profiles/default.env` and change RATE, DELAY, JITTER, LOSS and MTU. The
values in `default.env` are placeholders: delay, jitter and loss are not
defined for the waveform yet.

## 8. Show the links in MA-OI

`maoi_bridge.py` reads each node's OLSRd2 neighbours and routes once a
second. It sends one JSON line per node on TCP 127.0.0.1:5566, in the format
MA-OI's live MANET source reads.

Terminal 1, the bridge (the mesh must be up):

```bash
./maoi_bridge.py
```

Terminal 2, MA-OI, with `network.network_source: live_socket` in its
`config/settings.yaml`:

```bash
cd ~/projects/MA-OI-part1
.venv/bin/python -m src.main
```

What MA-OI shows:

- **Nodes.** A, B, C appear as NODE-1, NODE-2, NODE-3. MA-OI places them at
  random map positions.
- **Links.** A link is healthy when OLSRd2 reports it `symmetric`, and
  degraded for `heard`, `pending` or `lost`.
- **Cut-off node.** A node with no neighbours is shown OFFLINE.
- **Counters.** OLSRd2 has no value for MA-OI's beacon and packet counters,
  so they are sent as 0.
- **Messaging.** Messages sent from MA-OI are rejected.

The bridge also prints each node's links and route count to its terminal.
After `./mesh.sh link-down B C`, the B-C link goes in about 20 s and C shows
OFFLINE. After `link-up` it comes back.

## 9. Stop

```bash
./mesh.sh down
```

This removes the containers and the network. The image stays, so the next
`up` is quick.

## mesh.sh commands

| Command | What it does |
|---|---|
| `build` | Build the node image `f01ak2-olsrd2` |
| `up [count] [topology] [profile]` | Start nodes. Defaults: `3 chain default` |
| `down` | Remove all nodes and the network |
| `link-down X Y` / `link-up X Y` | Cut or restore the link between X and Y |
| `profile NAME [nodes...]` | Apply a waveform profile, to all nodes by default |
| `info NODE "CMD"` | Run an OLSRd2 telnet command, for example `"nhdpinfo link"` |
| `exec NODE cmd...` | Run a command in a node |
| `status` | Routes, netem and blocked MAC addresses per node |
| `nodes` | List running nodes |
| `ip NODE` | Print a node's IP address |

Environment variables: `DOCKER` (docker command), `IMAGE` (default
`f01ak2-olsrd2`), `NET` (default `f01ak2`).

## Not covered yet

- Measured repair time over repeated runs.
- iperf3 throughput, RTP audio stream, and the evidence pack for the F01
  review.
