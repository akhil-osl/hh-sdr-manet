#!/bin/sh
# 02-route-discovery.sh - step 2: time how long the route to the peer takes to appear.
# Usage: 02-route-discovery.sh [timeout_s]   (default 60)
# Run it right after the routing daemon is started on this node.
# Exit: 0 route found, 1 timeout, 3 error.
# Whole seconds only; the poll interval is 1 s.

set -u
. "$(dirname -- "$0")/common.sh"
test_init

timeout=${1:-60}
test_log "step=route-discovery peer=$TEST_PEER timeout_s=$timeout"

t=0
while [ "$t" -lt "$timeout" ]; do
    if ip route get "$TEST_PEER" 2>/dev/null | grep -q "dev $TEST_IFACE"; then
        ip route show >"$TEST_OUT/02-routes.txt" 2>&1
        test_log "route=found after_s=$t"
        exit 0
    fi
    sleep 1
    t=$((t + 1))
done

ip route show >"$TEST_OUT/02-routes.txt" 2>&1
test_log "route=missing after_s=$timeout"
exit 1
