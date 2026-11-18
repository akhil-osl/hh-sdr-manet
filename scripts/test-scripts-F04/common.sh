#!/bin/sh
# common.sh - shared settings for the two radio routing test scripts.
# Source it; do not run it. All values come from the environment.
#
#   TEST_IFACE   radio-facing interface on this node (required)
#   TEST_PEER    IP address of the other node (required)
#   TEST_OUT     evidence directory (default ./test-evidence)
#   TEST_OLSRD2  olsrd2 binary name (default olsrd2)
#   TEST_TELNET  olsrd2 telnet port (default 2009)

TEST_DIR=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
TEST_REPO=$(CDPATH='' cd -- "$TEST_DIR/../.." && pwd)
TEST_IFACE=${TEST_IFACE-}
TEST_PEER=${TEST_PEER-}
TEST_OUT=${TEST_OUT:-./test-evidence}
TEST_OLSRD2=${TEST_OLSRD2:-olsrd2}
TEST_TELNET=${TEST_TELNET:-2009}

# Print a timestamped line to stdout and to the step log.
test_log() {
    printf '%s %s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)" "$*" | tee -a "$TEST_OUT/steps.log"
}

# Stop with exit 3 (error) and a message.
test_die() {
    printf '%s: %s\n' "${0##*/}" "$*" >&2
    exit 3
}

# Check required variables and create the evidence directory.
test_init() {
    [ -n "$TEST_IFACE" ] || test_die "TEST_IFACE not set"
    [ -n "$TEST_PEER" ] || test_die "TEST_PEER not set"
    mkdir -p "$TEST_OUT" || test_die "cannot create $TEST_OUT"
}

# Ask the olsrd2 telnet interface a question; prints nothing if unreachable.
# Command names depend on the olsrd2 build on the target; see README.md.
test_olsr_query() {
    command -v nc >/dev/null 2>&1 || return 1
    printf '%s\n' "$1" | nc -w 2 127.0.0.1 "$TEST_TELNET" 2>/dev/null
}
