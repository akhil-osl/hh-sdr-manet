#!/bin/sh
#
# netns-selftest.sh — prove the ATP traffic scripts judge correctly.
#
# Builds a two-node IP network out of Linux network namespaces joined by a
# veth pair, then runs atp-iperf3.sh, atp-mgen.sh and atp-capture.sh across it:
#
#   - on a clean link, where every check must PASS;
#   - on a link impaired with tc netem (loss, delay), where the same checks
#     must FAIL — a script that can only ever pass proves nothing;
#   - in the honesty cases: no threshold -> "unjudged", latency without
#     synchronised clocks -> "blocked", unreachable peer -> "error".
#
# The MANET stack is deliberately not in the path. What is under test here is
# the tooling, and a plain veth link is the one network whose behaviour is
# known exactly. On the radio the same scripts run unchanged against the mesh.
#
# Needs no root. It runs inside an unprivileged user namespace, mapped to the
# caller's own uid with capabilities kept (unshare -c --keep-caps) rather than
# mapped to root (unshare -r): as "root", tcpdump tries to drop privileges to
# its build-time user, and setgroups() is always denied in an unprivileged
# user namespace, so the capture would fail for a reason that has nothing to
# do with the script under test.
#
# Exits 77 — the conventional "skipped" code, which ctest honours — when a
# required tool or unprivileged user namespaces are unavailable.
#
# usage: netns-selftest.sh [EVIDENCE_DIR]   (default: a fresh mktemp dir)

set -u

ATP_DIR=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
SELF="$ATP_DIR/selftest/netns-selftest.sh"
A=10.99.0.1
B=10.99.0.2

# ---- outer: check prerequisites, then re-exec inside a user+net namespace ---

if [ "${HH_ATP_INNER-}" != 1 ]; then
    missing=''
    for t in iperf3 mgen tcpdump jq tc ip unshare nsenter timeout; do
        command -v $t >/dev/null 2>&1 || missing="$missing $t"
    done
    if [ -n "$missing" ]; then
        echo "netns-selftest: SKIP — missing:$missing" >&2
        exit 77
    fi
    if ! unshare -n -c --keep-caps true 2>/dev/null; then
        echo "netns-selftest: SKIP — unprivileged user namespaces (or unshare --keep-caps, util-linux >= 2.37) unavailable" >&2
        exit 77
    fi
    # Inside a confined snap (VS Code's integrated terminal, for one), Ubuntu's
    # AppArmor profile for tcpdump refuses signals from the sandbox: timeout(1)
    # cannot end the capture and the self-test would wait forever. Detect it
    # and skip the capture case, loudly, instead of hanging.
    label=$(cat /proc/self/attr/current 2>/dev/null)
    case $label in
        snap.*)
            HH_ATP_SKIP_CAPTURE="shell is confined by AppArmor profile '${label%% *}'; tcpdump would ignore timeout's signals and hang — run from an ordinary terminal to cover capture"
            export HH_ATP_SKIP_CAPTURE
            ;;
    esac
    ev=${1:-$(mktemp -d "${TMPDIR:-/tmp}/hh-atp-selftest.XXXXXX")}
    mkdir -p "$ev" || exit 3
    HH_ATP_INNER=1 exec unshare -n -c --keep-caps --fork "$SELF" "$ev"
fi

# ---- inner: we hold every capability in a private network namespace ("node A")

ev=$1
fails=0 total=0 skipped=0

# A second namespace ("node B") is held open by a sleeping child; commands run
# in it through nsenter. It lives inside our user namespace, so no root needed.
unshare -n sleep 600 &
nsb=$!
srv=''
# SIGKILL, not SIGTERM: background children can inherit SIGTERM as ignored
# from whatever launched the selftest, and a surviving iperf3 server keeps the
# caller's stdout open, so a pipeline reading this script would never finish.
cleanup() { kill -KILL $srv "$nsb" 2>/dev/null; }
trap cleanup EXIT
sleep 0.2
inb() { nsenter -t "$nsb" -n "$@"; }

ip link set dev lo up
ip link add name veth0 type veth peer name veth1
ip link set dev veth1 netns "$nsb"
ip addr add "$A/24" dev veth0
ip link set dev veth0 up
inb ip link set dev lo up
inb ip addr add "$B/24" dev veth1
inb ip link set dev veth1 up

# Started directly rather than through inb, so $! is the server itself and
# not a subshell wrapped around it.
nsenter -t "$nsb" -n iperf3 -s >/dev/null 2>&1 &
srv=$!
sleep 0.3

# expect NAME WANT_EXIT WANT_RESULT CMD... — run a check and compare both its
# exit status and the "result" field of the record it printed.
expect() {
    name=$1 want_rc=$2 want_result=$3
    shift 3
    total=$((total + 1))
    out=$("$@" 2>"$ev/$name.stderr")
    rc=$?
    got=$(printf '%s' "$out" | jq -r '.result // "none"' 2>/dev/null)
    if [ "$rc" = "$want_rc" ] && [ "$got" = "$want_result" ]; then
        printf 'ok    %-28s %s\n' "$name" "$got"
    else
        fails=$((fails + 1))
        printf 'FAIL  %-28s want %s/exit %s, got %s/exit %s\n' \
            "$name" "$want_result" "$want_rc" "${got:-none}" "$rc"
        sed 's/^/        /' "$ev/$name.stderr"
    fi
}

mgen_run() {  # mgen_run NAME DIR [extra analyze args...]
    n=$1 d=$2
    shift 2
    inb "$ATP_DIR/atp-mgen.sh" rx -o "$d" -n "$n" -t 4 >/dev/null 2>&1 &
    rxpid=$!
    sleep 0.5
    "$ATP_DIR/atp-mgen.sh" tx -c "$B" -o "$d" -n "$n" -t 2 -r 100 2>/dev/null
    wait "$rxpid"
    "$ATP_DIR/atp-mgen.sh" analyze -o "$d" -n "$n" \
        --tx "$d/raw/mgen.$n.tx.txt" --rx "$d/raw/mgen.$n.rx.txt" "$@"
}

echo "== clean link (expect pass) =="
clean="$ev/clean"
if [ -n "${HH_ATP_SKIP_CAPTURE-}" ]; then
    expect iperf3-tcp-clean 0 pass "$ATP_DIR/atp-iperf3.sh" -c "$B" -o "$clean" -t 2 --min-bps 1000000
    skipped=$((skipped + 1))
    printf 'SKIP  %-28s %s\n' capture-clean "$HH_ATP_SKIP_CAPTURE"
else
    # Headers only (-s 96): a veth link moves gigabits per second, and full
    # packets would write gigabytes of pcap to prove a packet count.
    "$ATP_DIR/atp-capture.sh" -i veth0 -o "$clean" -t 4 -s 96 -f "tcp port 5201" \
        --min-packets 10 >"$clean.capture.out" 2>&1 &
    cap=$!
    sleep 0.5
    expect iperf3-tcp-clean 0 pass "$ATP_DIR/atp-iperf3.sh" -c "$B" -o "$clean" -t 2 --min-bps 1000000
    wait "$cap"
    total=$((total + 1))
    if jq -e '.result == "pass" and .metrics.packets >= 10' "$clean/capture.veth0.json" >/dev/null 2>&1; then
        printf 'ok    %-28s %s\n' capture-clean pass
    else
        fails=$((fails + 1)); printf 'FAIL  %-28s see %s\n' capture-clean "$clean.capture.out"
    fi
fi
expect iperf3-udp-clean 0 pass "$ATP_DIR/atp-iperf3.sh" -c "$B" -o "$clean" -t 2 -u -b 5M \
    --max-loss-pct 1
expect mgen-clean 0 pass mgen_run udp "$clean" --clock-synced --max-loss-pct 1 --max-latency-ms 50

echo "== impaired link: 30% loss, 100 ms delay (expect fail) =="
bad="$ev/impaired"
tc qdisc add dev veth0 root netem loss 30% delay 100ms
expect iperf3-udp-loss 1 fail "$ATP_DIR/atp-iperf3.sh" -c "$B" -o "$bad" -t 2 -u -b 2M \
    --max-loss-pct 5
expect mgen-loss-latency 1 fail mgen_run udp "$bad" --clock-synced --max-loss-pct 5 --max-latency-ms 50
tc qdisc del dev veth0 root

echo "== honesty cases =="
misc="$ev/honesty"
expect iperf3-no-threshold 5 unjudged "$ATP_DIR/atp-iperf3.sh" -c "$B" -o "$misc" -t 1
expect mgen-latency-unsynced 4 blocked mgen_run udp "$misc" --max-latency-ms 50
expect iperf3-no-server 3 error "$ATP_DIR/atp-iperf3.sh" -c "$B" -p 5999 -o "$misc" -n noserver -t 1

echo "== $((total - fails))/$total passed, $skipped skipped; evidence in $ev =="
[ "$skipped" -eq 0 ] || echo "== NOT full coverage: skipped cases were not tested =="
[ "$fails" -eq 0 ]
