#!/bin/bash
#
# slot-mesh.sh - real OLSRd2 over the slot-timed network.
#
# Each node is a network namespace running an unmodified OLSRd2. hh-slot-bridge
# creates a TAP interface in each namespace and carries the packets through the
# MAC and the TDMA slots of the simulated air (tools/sim/hh_slot_bridge.c).
# Routes form and repair through the slots; the script measures with ping and
# iperf3 and keeps the logs.
#
# Needs root. Two ways to run it:
#   in a privileged container:   slot-mesh.sh run          (see slot-mesh-docker.sh)
#   as a normal user, no Docker: slot-mesh.sh local        (user and network namespace)
#
# Settings (environment):
#   NODES=4  TOPOLOGY=line|ring|mesh  SCENARIO=line|ring-cut  CAPACITY=256
#   CONVERGE=30 (seconds before traffic)  TEST_S=30 (seconds of ping)  IPERF_S=20
#   BRIDGE=path/to/hh-slot-bridge  OLSRD2=olsrd2  OUT=output directory
# SCENARIO=ring-cut uses a ring, pings node 1 to node 3, cuts the link 1-2 for a
# while and restores it.

set -u

HERE=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
export NODES=${NODES:-4} TOPOLOGY=${TOPOLOGY:-line} SCENARIO=${SCENARIO:-line} CAPACITY=${CAPACITY:-256}
export CONVERGE=${CONVERGE:-30} TEST_S=${TEST_S:-30} IPERF_S=${IPERF_S:-20}
export BRIDGE=${BRIDGE:-$HERE/../../build/hh-slot-bridge} OLSRD2=${OLSRD2:-olsrd2}
export OUT=${OUT:-$PWD/slot-mesh-out}
BRIDGE_PID=""
PIDS=""

# Prints a message with the time, and appends it to the timeline file.
log() { echo "$(date +%s.%N) $*" | tee -a "$OUT/timeline.txt"; }

# Stops every process started here and removes the namespaces.
cleanup() {
    for p in $PIDS; do kill "$p" 2>/dev/null; done
    if [ -n "$BRIDGE_PID" ]; then kill -TERM "$BRIDGE_PID" 2>/dev/null; wait "$BRIDGE_PID" 2>/dev/null; fi
    for i in $(seq 1 "$NODES"); do ip netns del "n$i" 2>/dev/null; done
}

# Returns the link address of node $1: 02-00-00-00-hh-ll.
mac_of() { printf '02:00:00:00:%02x:%02x' $(($1 / 256)) $(($1 % 256)); }

# Starts the whole thing in a user and network namespace, so no root is needed.
run_local() {
    exec unshare -Urnm bash -c 'mount -t tmpfs tmpfs /run && exec "$0" run' "$0"
}

# Builds the namespaces, bridge and OLSRd2 nodes, runs the measurements, collects the logs.
run() {
    local i k dst cut=0 mtu
    [ -x "$BRIDGE" ] || { echo "slot-mesh: bridge not found: $BRIDGE" >&2; exit 2; }
    command -v "$OLSRD2" >/dev/null || { echo "slot-mesh: $OLSRD2 not found" >&2; exit 2; }
    mkdir -p "$OUT" || exit 3
    : >"$OUT/timeline.txt"
    trap cleanup EXIT
    dst=$NODES
    if [ "$SCENARIO" = ring-cut ]; then TOPOLOGY=ring; dst=3; cut=1; fi
    mtu=$((CAPACITY - 27))   # largest packet one PDU carries; fragmentation is excluded

    for i in $(seq 1 "$NODES"); do
        ip netns add "n$i" || { echo "slot-mesh: cannot create namespaces (need root or CAP_SYS_ADMIN)" >&2; exit 3; }
        ip -n "n$i" link set lo up
        ip netns exec "n$i" sysctl -qw net.ipv6.conf.all.disable_ipv6=1 2>/dev/null
    done

    mkfifo "$OUT/control"
    "$BRIDGE" --nodes "$NODES" --topology "$TOPOLOGY" --capacity "$CAPACITY" --control "$OUT/control" \
        --log "$OUT/slots.log" --pcap "$OUT/run.pcap" --summary "$OUT/bridge.summary" \
        >"$OUT/bridge.out" 2>"$OUT/bridge.err" &
    BRIDGE_PID=$!
    for _ in $(seq 1 50); do
        ip -n "n$NODES" link show tap0 >/dev/null 2>&1 && break
        kill -0 "$BRIDGE_PID" 2>/dev/null || { echo "slot-mesh: bridge stopped:" >&2; cat "$OUT/bridge.err" >&2; exit 3; }
        sleep 0.2
    done
    log "bridge up: $(tr '\n' ' ' <"$OUT/bridge.err")"

    for i in $(seq 1 "$NODES"); do
        ip -n "n$i" link set tap0 address "$(mac_of "$i")"
        ip -n "n$i" addr add "10.99.0.$i/24" dev tap0
        ip -n "n$i" link set tap0 mtu "$mtu" up
        for k in $(seq 1 "$NODES"); do
            [ "$k" = "$i" ] || ip -n "n$i" neigh replace "10.99.0.$k" lladdr "$(mac_of "$k")" dev tap0 nud permanent
        done
    done

    for i in $(seq 1 "$NODES"); do
        sed -e 's/interface=eth0/interface=tap0/' -e "s#/var/log/olsrd2.log#$OUT/olsrd2-n$i.log#" \
            "$HERE/olsrd2.conf" >"$OUT/olsrd2-n$i.conf"
        ip netns exec "n$i" "$OLSRD2" -l "$OUT/olsrd2-n$i.conf" >"$OUT/olsrd2-n$i.stdout" 2>&1 &
        PIDS="$PIDS $!"
        ip netns exec "n$i" ip -ts monitor route >"$OUT/routes-n$i.log" 2>&1 &
        PIDS="$PIDS $!"
    done
    log "olsrd2 started on $NODES nodes, topology=$TOPOLOGY scenario=$SCENARIO mtu=$mtu"

    sleep "$CONVERGE"
    for i in $(seq 1 "$NODES"); do ip -n "n$i" route >"$OUT/routes-after-converge-n$i.txt"; done
    log "converged wait done; routes saved"

    if [ "$cut" = 1 ]; then
        ip netns exec n1 ping -n -D -i 0.2 -w $((TEST_S * 2)) "10.99.0.$dst" >"$OUT/ping.txt" 2>&1 &
        PIDS="$PIDS $!"
        sleep $((TEST_S / 2))
        log "cut link 1-2"; echo "down 1 2" >"$OUT/control"
        sleep $((TEST_S / 2))
        log "restore link 1-2"; echo "up 1 2" >"$OUT/control"
        sleep $((TEST_S / 2 + 3))
    else
        ip netns exec n1 ping -n -D -i 0.2 -w "$TEST_S" "10.99.0.$dst" >"$OUT/ping.txt" 2>&1
        log "ping done"
    fi
    for i in $(seq 1 "$NODES"); do ip -n "n$i" route >"$OUT/routes-final-n$i.txt"; done

    if command -v iperf3 >/dev/null; then
        ip netns exec "n$dst" iperf3 -s -1 >"$OUT/iperf3-server.txt" 2>&1 &
        sleep 1
        ip netns exec n1 iperf3 -u -c "10.99.0.$dst" -b 20k -l 100 -t "$IPERF_S" --json >"$OUT/iperf3.json" 2>"$OUT/iperf3.err"
        log "iperf3 done"
    else
        log "iperf3 not installed, skipped"
    fi

    for i in $(seq 1 "$NODES"); do ip -n "n$i" -s link show tap0 >"$OUT/tap-stats-n$i.txt"; done
    log "stopping"
    cleanup
    BRIDGE_PID=""
    PIDS=""
    log "done; results in $OUT"
}

case "${1:-}" in
    run)   run ;;
    local) run_local ;;
    *)     sed -n '2,22p' "$0"; exit 2 ;;
esac
