#!/bin/sh
# run-all.sh - run the two radio steps in order on this node.
# Usage: TEST_IFACE=... TEST_PEER=... run-all.sh
# Preconditions: routing daemon running on both nodes, iperf3 -s on the peer.
# Stops at the first failed step so a missing link is not hidden by later steps.

set -u
. "$(dirname -- "$0")/common.sh"
test_init

d=$TEST_DIR
"$d/01-link-setup.sh" || exit 1
"$d/02-route-discovery.sh" || exit 1
"$d/04-log-routing.sh" 40 5 &
logpid=$!
"$d/03-traffic.sh"
rc=$?
wait "$logpid"
test_log "run-all exit=$rc evidence=$TEST_OUT"
exit "$rc"
