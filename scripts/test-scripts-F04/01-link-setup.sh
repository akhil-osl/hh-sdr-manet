#!/bin/sh
# 01-link-setup.sh - step 1: check the radio link is up and record the MTU.
# Exit: 0 link ok, 1 link not ok, 3 error.
# Records the routing protocol and version, interface state, address and MTU.

set -u
. "$(dirname -- "$0")/common.sh"
test_init

test_log "step=link-setup iface=$TEST_IFACE peer=$TEST_PEER"

ip link show "$TEST_IFACE" >"$TEST_OUT/01-link.txt" 2>&1 || test_die "no interface $TEST_IFACE"
ip -4 addr show "$TEST_IFACE" >>"$TEST_OUT/01-link.txt" 2>&1

state=$(cat "/sys/class/net/$TEST_IFACE/operstate" 2>/dev/null || echo unknown)
mtu=$(cat "/sys/class/net/$TEST_IFACE/mtu" 2>/dev/null || echo unknown)
ver=$("$TEST_OLSRD2" --version 2>&1 | head -n 1)
[ -n "$ver" ] || ver=unknown

test_log "routing_protocol=olsrd2 routing_version=\"$ver\""
test_log "iface_state=$state mtu=$mtu"

# Link check: the peer must answer a ping at least once.
if ping -c 3 -W 2 -I "$TEST_IFACE" "$TEST_PEER" >"$TEST_OUT/01-ping.txt" 2>&1; then
    test_log "link=up"
    exit 0
fi
test_log "link=down (no ping reply from $TEST_PEER)"
exit 1
