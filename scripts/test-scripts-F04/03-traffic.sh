#!/bin/sh
# 03-traffic.sh - step 3: ping and iperf3 to the peer.
# Usage: 03-traffic.sh [ping_count] [iperf_seconds]   (defaults 50 and 10)
# The iperf3 server must already run on the peer (iperf3 -s).
# Results are recorded, not judged: pass/fail thresholds come from the
# expected results (not written yet), so iperf3 is reported "unjudged".
# Exit: 0 both ran, 1 ping or iperf3 failed, 3 error.

set -u
. "$(dirname -- "$0")/common.sh"
test_init

count=${1:-50}
secs=${2:-10}
rc=0

test_log "step=traffic ping_count=$count iperf_s=$secs"

# Interval 0.2 s needs root on most targets; fall back to 1 s.
if ping -c "$count" -i 0.2 -I "$TEST_IFACE" "$TEST_PEER" >"$TEST_OUT/03-ping.txt" 2>&1 ||
   ping -c "$count" -I "$TEST_IFACE" "$TEST_PEER" >"$TEST_OUT/03-ping.txt" 2>&1; then
    test_log "ping=done $(grep -E 'packet loss' "$TEST_OUT/03-ping.txt")"
else
    test_log "ping=failed $(grep -E 'packet loss' "$TEST_OUT/03-ping.txt")"
    rc=1
fi

atp="$TEST_REPO/tools/atp/atp-iperf3.sh"
[ -x "$atp" ] || test_die "missing $atp"

"$atp" -c "$TEST_PEER" -o "$TEST_OUT/03-iperf-tcp" -t "$secs" -n tcp
r=$?
test_log "iperf3_tcp exit=$r (5 means unjudged)"
[ "$r" -eq 0 ] || [ "$r" -eq 5 ] || rc=1

"$atp" -c "$TEST_PEER" -o "$TEST_OUT/03-iperf-udp" -t "$secs" -u -n udp
r=$?
test_log "iperf3_udp exit=$r (5 means unjudged)"
[ "$r" -eq 0 ] || [ "$r" -eq 5 ] || rc=1

exit "$rc"
