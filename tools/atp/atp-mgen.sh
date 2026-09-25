#!/bin/sh
#
# atp-mgen.sh — packet loss, reordering and latency across the MANET using MGEN.
#
# MGEN needs a sender and a receiver on different nodes, so this script has
# three sub-commands that map onto that split:
#
#   rx       on the receiving node: listen and log for a fixed time
#   tx       on the sending node:   send a periodic UDP flow and log each send
#   analyze  anywhere both logs are available: compare them and write evidence
#
# Loss is computed against the sender's own log (MGEN "txlog"), not inferred
# from gaps in the receive log, because gaps cannot see packets lost at the
# end of a run.
#
# One-way latency is (receive time - send time) across two machines, so it is
# only meaningful when their clocks agree. On the radio that agreement is what
# the time plane is for, and the time plane does not exist yet (unknown.md
# U-07). Latency is therefore reported only when the caller asserts
# --clock-synced; asked for without it, the latency judgment is "blocked", not
# silently computed from unsynchronised clocks.

set -u

ATP_PROG=atp-mgen
ATP_DIR=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
. "$ATP_DIR/lib/atp.sh"

usage() {
    cat <<EOF
usage: $ATP_PROG rx      -o DIR [-p PORT] [-t SECONDS] [-n NAME]
       $ATP_PROG tx      -c PEER -o DIR [-p PORT] [-t SECONDS] [-r PPS] [-s BYTES] [-f FLOW] [-n NAME]
       $ATP_PROG analyze -o DIR --tx TXLOG --rx RXLOG [-f FLOW] [-n NAME] [--clock-synced] [thresholds]

  -o DIR                evidence directory, created if absent (required)
  -c PEER               receiver address (tx)
  -p PORT               UDP port (default 5000)
  -t SECONDS            rx: listen time; tx: send time (default 10)
  -r PPS                tx: packets per second (default 50)
  -s BYTES              tx: payload size (default 256)
  -f FLOW               MGEN flow id (default 1)
  -n NAME               run name, used in the check and file names (default udp)
  --tx / --rx FILE      analyze: the MGEN logs written by tx and rx
  --clock-synced        analyze: assert sender and receiver clocks agree, so
                        one-way latency is meaningful

  thresholds (omit and the result is "unjudged", never "pass"):
  --max-loss-pct N      maximum packet loss, percent
  --max-reordered N     maximum packets received out of order
  --max-latency-ms N    maximum one-way latency (needs --clock-synced)

exit (analyze): 0 pass, 1 fail, 2 usage, 3 error, 4 blocked, 5 unjudged
exit (rx/tx):   0 ok, 2 usage, 3 error
EOF
}

mode=${1-}
[ $# -gt 0 ] && shift
case $mode in
    rx|tx|analyze) ;;
    -h|--help) usage; exit 0 ;;
    *) usage >&2; atp_die "sub-command must be rx, tx or analyze" ;;
esac

peer='' outdir='' port=5000 secs=10 pps=50 size=256 flow=1 name=udp
txlog='' rxlog='' synced=0 max_loss='' max_reord='' max_lat=''

while [ $# -gt 0 ]; do
    case $1 in
        -c) peer=${2-}; shift ;;
        -o) outdir=${2-}; shift ;;
        -p) port=${2-}; shift ;;
        -t) secs=${2-}; shift ;;
        -r) pps=${2-}; shift ;;
        -s) size=${2-}; shift ;;
        -f) flow=${2-}; shift ;;
        -n) name=${2-}; shift ;;
        --tx) txlog=${2-}; shift ;;
        --rx) rxlog=${2-}; shift ;;
        --clock-synced) synced=1 ;;
        --max-loss-pct) max_loss=${2-}; shift ;;
        --max-reordered) max_reord=${2-}; shift ;;
        --max-latency-ms) max_lat=${2-}; shift ;;
        -h|--help) usage; exit 0 ;;
        *) usage >&2; atp_die "unknown argument: $1" ;;
    esac
    shift
done

[ -n "$outdir" ] || { usage >&2; atp_die "-o EVIDENCE_DIR is required"; }
for v in "$port" "$secs" "$pps" "$size" "$flow"; do
    atp_is_number "$v" || atp_die "not a number: $v"
done
for t in "$max_loss" "$max_reord" "$max_lat"; do
    [ -z "$t" ] || atp_is_number "$t" || atp_die "threshold is not a number: $t"
done
mkdir -p "$outdir/raw" || atp_die "cannot create $outdir/raw"

check="mgen.$name"

# ---- rx / tx: run MGEN and keep its log. No evidence record: a log is only
# half a measurement until analyze compares it with the other side's. --------

if [ "$mode" = rx ] || [ "$mode" = tx ]; then
    atp_have mgen    || { atp_log "mgen not found on PATH"; exit 3; }
    atp_have timeout || { atp_log "timeout not found on PATH"; exit 3; }
    log="$outdir/raw/$check.$mode.txt"

    if [ "$mode" = rx ]; then
        # MGEN runs until killed; timeout(1) ends the listen on schedule.
        timeout "$secs" mgen event "LISTEN UDP $port" output "$log" >/dev/null 2>"$log.stderr"
        rc=$?
        [ $rc -eq 0 ] || [ $rc -eq 124 ] || { atp_log "mgen rx failed (exit $rc)"; exit 3; }
    else
        [ -n "$peer" ] || { usage >&2; atp_die "tx needs -c PEER"; }
        # Source port is fixed one above the destination so a capture filter
        # for the flow is predictable in both directions.
        timeout $((secs + 10)) mgen \
            event "0.0 ON $flow UDP SRC $((port + 1)) DST $peer/$port PERIODIC [$pps $size]" \
            event "$secs OFF $flow" \
            txlog output "$log" >/dev/null 2>"$log.stderr"
        rc=$?
        [ $rc -eq 0 ] || { atp_log "mgen tx failed (exit $rc)"; exit 3; }
    fi
    atp_log "wrote $log"
    exit 0
fi

# ---- analyze --------------------------------------------------------------

[ -n "$txlog" ] && [ -n "$rxlog" ] || { usage >&2; atp_die "analyze needs --tx and --rx"; }
atp_require_jq

record="$outdir/$check.json"
started=$(atp_now_utc)
params=$(jq -n --arg tx "$txlog" --arg rx "$rxlog" --argjson flow "$flow" \
    --argjson synced "$synced" \
    '{tx_log: $tx, rx_log: $rx, flow: $flow, clock_synced: ($synced == 1)}')
thresholds=$(jq -n --arg a "$max_loss" --arg b "$max_reord" --arg c "$max_lat" \
    '{max_loss_pct: $a, max_reordered: $b, max_latency_ms: $c}
     | with_entries(select(.value != "") | .value |= tonumber)')
artifacts=$(jq -n --arg t "$txlog" --arg r "$rxlog" '[$t, $r]')

for f in "$txlog" "$rxlog"; do
    if [ ! -r "$f" ]; then
        atp_record "$record" "$check" error "cannot read MGEN log $f" "$started" \
            "$params" '{}' "$thresholds" "$artifacts"
        atp_exit_for error; exit $?
    fi
done

# MGEN log lines look like
#   06:21:43.446607 SEND proto>UDP flow>7 seq>0 srcPort>5001 dst>... size>128
#   06:19:37.650337 RECV proto>UDP flow>1 seq>0 src>... dst>... sent>06:19:37.650168 size>256 ...
# Times carry no date, so a receive that appears earlier than its send has
# crossed midnight and gets a day added.
metrics=$(awk -v flow="$flow" -v synced="$synced" '
    function field(name,   i) {
        for (i = 3; i <= NF; i++)
            if (index($i, name ">") == 1) return substr($i, length(name) + 2)
        return ""
    }
    function secs(t,   p) { split(t, p, ":"); return p[1] * 3600 + p[2] * 60 + p[3] }
    FNR == NR { if ($2 == "SEND" && field("flow") == flow) sent++; next }
    $2 == "RECV" && field("flow") == flow {
        seq = field("seq") + 0
        if (seq in seen) { dup++; next }
        seen[seq] = 1; recv++
        if (recv > 1 && seq < maxseq) reord++
        if (seq > maxseq || recv == 1) maxseq = seq
        if (synced) {
            lat = (secs($1) - secs(field("sent"))) * 1000
            if (lat < 0) lat += 86400000
            lsum += lat
            if (nlat == 0 || lat < lmin) lmin = lat
            if (nlat == 0 || lat > lmax) lmax = lat
            nlat++
        }
    }
    END {
        lost = sent - recv; if (lost < 0) lost = 0
        printf "{\"sent\":%d,\"received\":%d,\"lost\":%d,", sent, recv, lost
        printf "\"loss_pct\":%s,", (sent > 0) ? sprintf("%.4f", lost * 100 / sent) : "null"
        printf "\"duplicates\":%d,\"reordered\":%d,", dup, reord
        if (nlat > 0)
            printf "\"latency_ms\":{\"min\":%.3f,\"avg\":%.3f,\"max\":%.3f}}", lmin, lsum / nlat, lmax
        else
            printf "\"latency_ms\":null}"
    }' "$txlog" "$rxlog")

# A log that does not parse is a broken check, not a measurement.
if ! sent=$(printf '%s' "$metrics" | jq -e -r .sent 2>/dev/null); then
    atp_record "$record" "$check" error "could not parse MGEN logs" \
        "$started" "$params" '{}' "$thresholds" "$artifacts"
    atp_exit_for error; exit $?
fi
if [ "$sent" -eq 0 ]; then
    atp_record "$record" "$check" error "no SEND records for flow $flow in $txlog (was tx run with txlog?)" \
        "$started" "$params" "$metrics" "$thresholds" "$artifacts"
    atp_exit_for error; exit $?
fi

get() { printf '%s' "$metrics" | jq -r "$1 // empty"; }

result=unjudged
violations='' blocked=''
if [ -n "$max_loss" ]; then
    result=pass
    v=$(get .loss_pct)
    atp_exceeds "$v" max "$max_loss" && violations="loss_pct=$v (max $max_loss)"
fi
if [ -n "$max_reord" ]; then
    result=pass
    v=$(get .reordered)
    atp_exceeds "$v" max "$max_reord" && \
        violations="$violations${violations:+; }reordered=$v (max $max_reord)"
fi
if [ -n "$max_lat" ]; then
    result=pass
    if [ "$synced" != 1 ]; then
        blocked="latency needs synchronised clocks; not asserted (--clock-synced), and the radio time plane does not exist yet (unknown.md U-07)"
    else
        v=$(get .latency_ms.max)
        if [ -z "$v" ]; then
            violations="$violations${violations:+; }no packets received, latency unmeasurable"
        elif atp_exceeds "$v" max "$max_lat"; then
            violations="$violations${violations:+; }latency_max_ms=$v (max $max_lat)"
        fi
    fi
fi

# A violated threshold is a definite failure even if another judgment was
# blocked; a blocked judgment prevents an overall pass.
if [ -n "$violations" ]; then
    result=fail reason="threshold violated: $violations"
elif [ -n "$blocked" ]; then
    result=blocked reason=$blocked
elif [ "$result" = pass ]; then
    reason="all thresholds met"
else
    reason="measured; no threshold given"
fi

atp_record "$record" "$check" "$result" "$reason" "$started" \
    "$params" "$metrics" "$thresholds" "$artifacts"
atp_exit_for "$result"; exit $?
