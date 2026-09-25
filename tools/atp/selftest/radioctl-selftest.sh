#!/bin/sh
#
# radioctl-selftest.sh — prove atp-radioctl.sh judges the control plane
# correctly, against the REAL radiod and radioctl binaries.
#
# Drives one radiod through the states a deployed radio can be found in, and
# checks the result AND exit code of every check:
#
#   fresh radiod           health  -> fail   (not running yet)
#                          lifecycle -> pass (every documented transition)
#   after lifecycle        lifecycle -> error (refuses: radio already used)
#   started                health  -> pass
#                          health --max-rejected 0 -> fail (counter threshold)
#   hw_fault injected      health  -> fail   (faulted, not operational)
#   radiod gone            health  -> fail   (unreachable is a finding)
#   no radioctl binary     health  -> error  (the check itself cannot run)
#
# Runs in well under a second and is registered with ctest. Exits 77 (skipped)
# when jq is unavailable.
#
# usage: radioctl-selftest.sh PATH/TO/radiod PATH/TO/radioctl [WORKDIR]

set -u

atp=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)/atp-radioctl.sh
radiod=${1-} radioctl=${2-}
[ -x "$radiod" ] && [ -x "$radioctl" ] || {
    echo "usage: $0 PATH/TO/radiod PATH/TO/radioctl [WORKDIR]" >&2
    exit 2
}
command -v jq >/dev/null 2>&1 || { echo "radioctl-selftest: SKIP — jq not found" >&2; exit 77; }

work=${3:-$(mktemp -d "${TMPDIR:-/tmp}/hh-atp-radioctl.XXXXXX")}
mkdir -p "$work" || exit 3
# The socket lives in TMPDIR, not WORKDIR: a UNIX socket path is limited to
# 108 bytes, and a build directory can be nested deeper than that allows.
sock=$(mktemp -u "${TMPDIR:-/tmp}/hh-atp-radiod.XXXXXX")

"$radiod" -s "$sock" -v warn 2>"$work/radiod.log" &
daemon=$!
trap 'kill -KILL $daemon 2>/dev/null; rm -f "$sock"' EXIT

# Wait for the socket rather than sleeping a fixed time.
i=0
while [ ! -S "$sock" ] && [ $i -lt 50 ]; do sleep 0.05; i=$((i + 1)); done
[ -S "$sock" ] || { echo "radioctl-selftest: radiod did not create $sock" >&2; exit 3; }

fails=0 total=0
expect() {  # expect NAME WANT_EXIT WANT_RESULT ARGS...
    name=$1 want_rc=$2 want_result=$3
    shift 3
    total=$((total + 1))
    out=$("$atp" "$@" -o "$work/$name" 2>"$work/$name.stderr")
    rc=$?
    got=$(printf '%s' "$out" | jq -r '.result // "none"' 2>/dev/null)
    if [ "$rc" = "$want_rc" ] && [ "$got" = "$want_result" ]; then
        printf 'ok    %-22s %s\n' "$name" "$got"
    else
        fails=$((fails + 1))
        printf 'FAIL  %-22s want %s/exit %s, got %s/exit %s: %s\n' "$name" \
            "$want_result" "$want_rc" "${got:-none}" "$rc" \
            "$(printf '%s' "$out" | jq -r '.reason // empty' 2>/dev/null)"
        sed 's/^/        /' "$work/$name.stderr"
    fi
}

# Deliberately unquoted where used: a word list of common arguments.
R="-R $radioctl -s $sock -T 5"
expect health-fresh       1 fail     health $R
expect lifecycle          0 pass     lifecycle $R --node-id 42 --channel 7 --set-channel 11
expect lifecycle-refused  3 error    lifecycle $R --node-id 42
"$radioctl" -s "$sock" start >/dev/null
expect health-running     0 pass     health $R
expect health-rejected    1 fail     health $R --max-rejected 0
"$radioctl" -s "$sock" inject-fault hw_fault >/dev/null
expect health-faulted     1 fail     health $R
"$radioctl" -s "$sock" shutdown >/dev/null
wait $daemon 2>/dev/null
expect health-unreachable 1 fail     health $R
expect radioctl-missing   3 error    health -R "$work/no-such-radioctl" -s "$sock"

echo "== $((total - fails))/$total passed =="
[ "$fails" -eq 0 ]
