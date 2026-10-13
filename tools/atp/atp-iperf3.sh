#!/bin/sh
#
# atp-iperf3.sh — throughput check across the MANET using iperf3.
#
# Runs one iperf3 client against an iperf3 server already listening on PEER,
# keeps iperf3's own JSON output as the raw artifact, and writes one evidence
# record (see lib/atp.sh for the schema and result semantics).
#
# Hardware-independent by construction: it only needs an IP route to PEER.
# Today that route is a network namespace (selftest/netns-selftest.sh); on the
# radio it is whatever carries IP across the mesh (manet0 in the target
# architecture — not yet built, unknown.md U-06/U-15). Nothing here changes
# between the two; only the peer address does.
#
# The server side is deliberately not started from here: on the deployed radio
# each node runs `iperf3 -s` as a service, and this script must not assume it
# can reach into another node to start one.

set -u

ATP_PROG=atp-iperf3
ATP_DIR=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
. "$ATP_DIR/lib/atp.sh"

usage() {
    cat <<EOF
usage: $ATP_PROG -c PEER -o EVIDENCE_DIR [options]

  -c PEER               iperf3 server address (required)
  -o DIR                evidence directory, created if absent (required)
  -p PORT               server port (default 5201)
  -t SECONDS            test duration (default 10)
  -u                    UDP instead of TCP
  -b RATE               target bitrate, iperf3 syntax e.g. 2M (UDP: default 1M)
  -B ADDR               bind to local address
  -R                    reverse: server sends, client receives
  -n NAME               run name, used in the check and file names (default tcp/udp)

  thresholds (omit and the result is "unjudged", never "pass"):
  --min-bps N           minimum received throughput, bits/s
  --max-retransmits N   TCP only: maximum sender retransmits
  --max-loss-pct N      UDP only: maximum datagram loss, percent
  --max-jitter-ms N     UDP only: maximum jitter, milliseconds

exit: 0 pass, 1 fail, 2 usage, 3 error, 5 unjudged
EOF
}

peer='' outdir='' port=5201 secs=10 udp=0 rate='' bind='' reverse=0 name=''
min_bps='' max_retr='' max_loss='' max_jitter=''

while [ $# -gt 0 ]; do
    case $1 in
        -c) peer=${2-}; shift ;;
        -o) outdir=${2-}; shift ;;
        -p) port=${2-}; shift ;;
        -t) secs=${2-}; shift ;;
        -u) udp=1 ;;
        -b) rate=${2-}; shift ;;
        -B) bind=${2-}; shift ;;
        -R) reverse=1 ;;
        -n) name=${2-}; shift ;;
        --min-bps) min_bps=${2-}; shift ;;
        --max-retransmits) max_retr=${2-}; shift ;;
        --max-loss-pct) max_loss=${2-}; shift ;;
        --max-jitter-ms) max_jitter=${2-}; shift ;;
        -h|--help) usage; exit 0 ;;
        *) usage >&2; atp_die "unknown argument: $1" ;;
    esac
    shift
done

[ -n "$peer" ]   || { usage >&2; atp_die "-c PEER is required"; }
[ -n "$outdir" ] || { usage >&2; atp_die "-o EVIDENCE_DIR is required"; }
atp_is_number "$port" || atp_die "-p must be a number"
atp_is_number "$secs" || atp_die "-t must be a number"
for t in "$min_bps" "$max_retr" "$max_loss" "$max_jitter"; do
    [ -z "$t" ] || atp_is_number "$t" || atp_die "threshold is not a number: $t"
done
# A threshold that cannot apply to the chosen protocol is a mistake in the
# test definition; refusing it beats silently ignoring it.
if [ "$udp" = 1 ]; then
    [ -z "$max_retr" ] || atp_die "--max-retransmits applies to TCP only"
    proto=udp
else
    [ -z "$max_loss$max_jitter" ] || atp_die "--max-loss-pct/--max-jitter-ms apply to UDP only"
    proto=tcp
fi
[ -n "$name" ] || name=$proto

atp_require_jq
mkdir -p "$outdir/raw" || atp_die "cannot create $outdir/raw"

check="iperf3.$name"
record="$outdir/$check.json"
raw_rel="raw/$check.iperf3.json"
raw="$outdir/$raw_rel"
started=$(atp_now_utc)

params=$(jq -n --arg peer "$peer" --argjson port "$port" --argjson secs "$secs" \
    --arg proto "$proto" --arg rate "$rate" --arg bind "$bind" \
    --argjson reverse "$reverse" \
    '{peer: $peer, port: $port, duration_s: $secs, protocol: $proto,
      target_rate: $rate, bind: $bind, reverse: ($reverse == 1)}')
thresholds=$(jq -n --arg a "$min_bps" --arg b "$max_retr" --arg c "$max_loss" --arg d "$max_jitter" \
    '{min_bps: $a, max_retransmits: $b, max_loss_pct: $c, max_jitter_ms: $d}
     | with_entries(select(.value != "") | .value |= tonumber)')

if ! atp_have iperf3; then
    atp_record "$record" "$check" error "iperf3 not found on PATH" "$started" \
        "$params" '{}' "$thresholds" '[]'
    atp_exit_for error; exit $?
fi

set -- -c "$peer" -p "$port" -t "$secs" -J
[ "$udp" = 1 ] && set -- "$@" -u -b "${rate:-1M}"
[ "$udp" = 0 ] && [ -n "$rate" ] && set -- "$@" -b "$rate"
[ -n "$bind" ] && set -- "$@" -B "$bind"
[ "$reverse" = 1 ] && set -- "$@" -R

# iperf3 has no overall deadline of its own; a peer that accepts the control
# connection and then stalls would hang a BIT run forever.
if atp_have timeout; then
    timeout $((secs + 30)) iperf3 "$@" >"$raw" 2>"$raw.stderr"
else
    iperf3 "$@" >"$raw" 2>"$raw.stderr"
fi
rc=$?

artifacts=$(jq -n --arg r "$raw_rel" '[$r]')

# iperf3 -J reports its own failures as {"error": "..."}; keep that message,
# it is far more useful in evidence than an exit code.
if [ $rc -ne 0 ] || ! jq -e 'has("end") and (has("error") | not)' "$raw" >/dev/null 2>&1; then
    why=$(jq -r '.error // empty' "$raw" 2>/dev/null)
    [ -n "$why" ] || why=$(head -n 1 "$raw.stderr" 2>/dev/null)
    [ -n "$why" ] || why="iperf3 exited $rc"
    atp_record "$record" "$check" error "iperf3 failed: $why" "$started" \
        "$params" '{}' "$thresholds" "$artifacts"
    atp_exit_for error; exit $?
fi

if [ "$udp" = 1 ]; then
    # For UDP, loss and jitter are measured by the receiving end and returned
    # to the client in .end.sum.
    metrics=$(jq '{bits_per_second: .end.sum.bits_per_second,
                   bytes: .end.sum.bytes,
                   packets: .end.sum.packets,
                   lost_packets: .end.sum.lost_packets,
                   lost_percent: .end.sum.lost_percent,
                   jitter_ms: .end.sum.jitter_ms}' "$raw")
else
    metrics=$(jq '{bits_per_second: .end.sum_received.bits_per_second,
                   bytes: .end.sum_received.bytes,
                   sender_bits_per_second: .end.sum_sent.bits_per_second,
                   retransmits: .end.sum_sent.retransmits}' "$raw")
fi

get() { printf '%s' "$metrics" | jq -r ".$1 // empty"; }

result=unjudged
reason="measured; no threshold given"
violations=''
judge() {  # judge LABEL VALUE OP LIMIT
    [ -n "$4" ] || return 0
    [ "$result" = unjudged ] && result=pass
    if [ -z "$2" ]; then
        violations="$violations${violations:+; }$1 not reported by iperf3"
    elif atp_exceeds "$2" "$3" "$4"; then
        violations="$violations${violations:+; }$1=$2 ($3 $4)"
    fi
}
judge bits_per_second "$(get bits_per_second)" min "$min_bps"
judge retransmits     "$(get retransmits)"     max "$max_retr"
judge lost_percent    "$(get lost_percent)"    max "$max_loss"
judge jitter_ms       "$(get jitter_ms)"       max "$max_jitter"

if [ -n "$violations" ]; then
    result=fail reason="threshold violated: $violations"
elif [ "$result" = pass ]; then
    reason="all thresholds met"
fi

atp_record "$record" "$check" "$result" "$reason" "$started" \
    "$params" "$metrics" "$thresholds" "$artifacts"
atp_exit_for "$result"; exit $?
