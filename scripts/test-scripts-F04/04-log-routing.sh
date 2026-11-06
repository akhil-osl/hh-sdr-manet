#!/bin/sh
# 04-log-routing.sh - step 4: log routing state while traffic runs.
# Usage: 04-log-routing.sh [duration_s] [interval_s]   (defaults 60 and 5)
# Each interval it appends to the evidence directory:
#   04-routes.log     ip route table
#   04-links.log      olsrd2 link metrics with valid flags (raw telnet answer)
# At the end it counts routing control packets on UDP port 269 (OLSRv2 manet
# port) and writes control packets per second for this node to 04-control.txt.
# Run it in the background while 03-traffic.sh runs.
# Exit: 0 done, 3 error. Missing optional tools are logged, not hidden.

set -u
. "$(dirname -- "$0")/common.sh"
test_init

dur=${1:-60}
interval=${2:-5}
test_log "step=log-routing duration_s=$dur interval_s=$interval"

# Control packet count runs in the background for the whole window.
if command -v tcpdump >/dev/null 2>&1; then
    timeout "$dur" tcpdump -i "$TEST_IFACE" -n -q udp port 269 >"$TEST_OUT/04-control.pcap.txt" 2>/dev/null &
    tpid=$!
else
    test_log "control_count=skipped (tcpdump not installed)"
    tpid=''
fi

t=0
while [ "$t" -lt "$dur" ]; do
    stamp=$(date -u +%Y-%m-%dT%H:%M:%SZ)
    { echo "== $stamp"; ip route show; } >>"$TEST_OUT/04-routes.log" 2>&1
    if out=$(test_olsr_query "/nhdpinfo link"); then
        { echo "== $stamp"; echo "$out"; } >>"$TEST_OUT/04-links.log"
    else
        echo "== $stamp olsrd2 telnet unavailable" >>"$TEST_OUT/04-links.log"
    fi
    sleep "$interval"
    t=$((t + interval))
done

if [ -n "$tpid" ]; then
    wait "$tpid" 2>/dev/null
    n=$(wc -l <"$TEST_OUT/04-control.pcap.txt")
    printf 'packets=%s window_s=%s per_second=%s\n' "$n" "$dur" \
        "$(awk -v n="$n" -v d="$dur" 'BEGIN { printf "%.2f", n / d }')" |
        tee "$TEST_OUT/04-control.txt"
    test_log "control_packets=$n window_s=$dur"
fi
test_log "log-routing=done"
