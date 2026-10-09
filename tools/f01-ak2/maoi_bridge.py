#!/usr/bin/env python3
"""Serve the Docker mesh's OLSRd2 links to MA-OI's live topology view.

Each second, reads `nhdpinfo link` and `olsrv2info route` from every node and
sends one JSON line per node on TCP 5566, in the format MA-OI's
LiveSocketSource expects. Fields OLSRd2 has no value for are sent as 0.
"""
import argparse
import json
import os
import select
import socket
import subprocess
import time

DOCKER = os.environ.get("DOCKER", "docker")
PREFIX = "f01-"
NET_PREFIX = "10.99.0."
LINK_STATES = {"symmetric": "good", "heard": "degraded", "pending": "degraded", "lost": "lost"}


def nodes():
    out = subprocess.run([DOCKER, "ps", "--filter", f"name=^{PREFIX}", "--format", "{{.Names}}"],
                         capture_output=True, text=True, check=True).stdout
    return sorted(n[len(PREFIX):] for n in out.split())


def node_id(ip):
    return int(ip.rsplit(".", 1)[1]) - 10


def telnet(name, cmd):
    r = subprocess.run([DOCKER, "exec", PREFIX + name, "sh", "-c",
                        f"printf '%s\\nquit\\n' '{cmd}' | nc -N -w2 127.0.0.1 2009"],
                       capture_output=True, text=True, timeout=5)
    return [line.split() for line in r.stdout.splitlines() if line.strip()] if r.returncode == 0 else None


def status(name):
    nid = ord(name) - 64
    links = telnet(name, "nhdpinfo link")
    routes = telnet(name, "olsrv2info route")
    up = links is not None and routes is not None
    neighbors = {}
    for f in links or []:
        if len(f) > 1 and f[1].startswith(NET_PREFIX):
            state = next((LINK_STATES[t] for t in f if t in LINK_STATES), "degraded")
            neighbors[node_id(f[1])] = state
    route_count = sum(1 for f in routes or [] if f and f[0].startswith(NET_PREFIX))
    zero = ("beacon_interval_ms", "beacons_sent", "beacons_rx_accepted", "neighbor_ups",
            "neighbor_downs", "link_transitions", "failures_confirmed", "recoveries_started",
            "recoveries_completed", "routes_installed", "routes_withdrawn", "packets_forwarded",
            "packets_delivered_local", "packets_dropped_no_route", "events_dropped", "radio_channel")
    msg = {k: 0 for k in zero}
    msg.update({
        "node_id": nid,
        "state": "RUN" if up else "DOWN",
        "neighbor_count": len(neighbors),
        "route_count": route_count,
        "topology_nodes": route_count + 1,
        "reachable_nodes": route_count + 1,
        "partitioned": not neighbors,
        "radio_available": up,
        "radio_operational": up,
        "neighbors": [{"id": i, "link_state": s} for i, s in sorted(neighbors.items())],
    })
    return msg


def reply_to_commands(conn, buf):
    data = conn.recv(4096)
    if not data:
        raise OSError("client closed")
    buf += data.decode(errors="replace")
    while "\n" in buf:
        line, buf = buf.split("\n", 1)
        try:
            cmd = json.loads(line)
        except json.JSONDecodeError:
            continue
        if cmd.get("cmd") == "send":
            conn.sendall((json.dumps({"type": "send_ack", "msg_id": cmd.get("msg_id", ""),
                                      "status": "rejected",
                                      "reason": "messaging not supported by maoi_bridge"}) + "\n").encode())
    return buf


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=5566)
    ap.add_argument("--interval", type=float, default=1.0)
    args = ap.parse_args()

    srv = socket.create_server((args.host, args.port), reuse_port=True)
    print(f"maoi_bridge listening on {args.host}:{args.port}", flush=True)
    clients = {}
    while True:
        start = time.monotonic()
        ready, _, _ = select.select([srv, *clients], [], [], 0)
        for s in ready:
            if s is srv:
                conn, addr = srv.accept()
                clients[conn] = ""
                print(f"client connected {addr[0]}:{addr[1]}", flush=True)
            else:
                try:
                    clients[s] = reply_to_commands(s, clients[s])
                except OSError:
                    clients.pop(s).close()
        if clients:
            lines = []
            for name in nodes():
                st = status(name)
                lines.append(json.dumps(st) + "\n")
                print(f"{time.strftime('%H:%M:%S', time.gmtime())} {name} "
                      f"links={[(n['id'], n['link_state']) for n in st['neighbors']]} "
                      f"routes={st['route_count']}", flush=True)
            data = "".join(lines).encode()
            for c in list(clients):
                try:
                    c.sendall(data)
                except OSError:
                    clients.pop(c).close()
        time.sleep(max(0.0, args.interval - (time.monotonic() - start)))


if __name__ == "__main__":
    main()
