#!/bin/sh
#
# atp-capture.sh — packet capture with tcpdump, as ATP/BIT evidence.
#
# Captures on one interface for a fixed time, keeps the pcap as the raw
# artifact, and writes one evidence record with packet/byte/drop counts. The
# pcap is decoded later on an analysis host (Wireshark/tshark plus
# wireshark/hh_manet.lua); the radio itself only captures.
#
# What a capture on the radio can actually see is limited by what the radio
# exposes as a network interface. User IP traffic crosses manet0 (target
# architecture; not yet built, unknown.md U-06/U-15). The MANET's own beacon and
# routing frames travel hh_radio_transmit -> DMA -> MAC and have no defined
# capture point at all (unknown.md U-16). This script captures whatever
# interface it is given and claims nothing beyond that.

set -u

ATP_PROG=atp-capture
ATP_DIR=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
. "$ATP_DIR/lib/atp.sh"

usage() {
    cat <<EOF
usage: $ATP_PROG -i IFACE -o EVIDENCE_DIR [options]

  -i IFACE              interface to capture on (required)
  -o DIR                evidence directory, created if absent (required)
  -t SECONDS            capture duration (default 10)
  -c COUNT              stop early after COUNT packets
  -f FILTER             BPF filter expression, quoted as one argument
  -s SNAPLEN            bytes kept per packet (default 262144)
  -n NAME               run name, used in the check and file names (default IFACE)

  thresholds (omit and the result is "unjudged", never "pass"):
  --min-packets N       minimum packets captured
  --max-kernel-drops N  maximum packets dropped by the kernel

exit: 0 pass, 1 fail, 2 usage, 3 error, 5 unjudged
EOF
}

iface='' outdir='' secs=10 count='' filter='' snaplen=262144 name=''
min_pkts='' max_drops=''

while [ $# -gt 0 ]; do
    case $1 in
        -i) iface=${2-}; shift ;;
        -o) outdir=${2-}; shift ;;
        -t) secs=${2-}; shift ;;
        -c) count=${2-}; shift ;;
        -f) filter=${2-}; shift ;;
        -s) snaplen=${2-}; shift ;;
        -n) name=${2-}; shift ;;
        --min-packets) min_pkts=${2-}; shift ;;
        --max-kernel-drops) max_drops=${2-}; shift ;;
        -h|--help) usage; exit 0 ;;
        *) usage >&2; atp_die "unknown argument: $1" ;;
    esac
    shift
done

[ -n "$iface" ]  || { usage >&2; atp_die "-i IFACE is required"; }
[ -n "$outdir" ] || { usage >&2; atp_die "-o EVIDENCE_DIR is required"; }
atp_is_number "$secs"    || atp_die "-t must be a number"
atp_is_number "$snaplen" || atp_die "-s must be a number"
for t in "$count" "$min_pkts" "$max_drops"; do
    [ -z "$t" ] || atp_is_number "$t" || atp_die "not a number: $t"
done
[ -n "$name" ] || name=$iface

atp_require_jq
mkdir -p "$outdir/raw" || atp_die "cannot create $outdir/raw"

check="capture.$name"
record="$outdir/$check.json"
pcap_rel="raw/$check.pcap"
pcap="$outdir/$pcap_rel"
started=$(atp_now_utc)

params=$(jq -n --arg iface "$iface" --argjson secs "$secs" --arg count "$count" \
    --arg filter "$filter" --argjson snaplen "$snaplen" \
    '{iface: $iface, duration_s: $secs, max_count: $count, filter: $filter,
      snaplen: $snaplen}')
thresholds=$(jq -n --arg a "$min_pkts" --arg b "$max_drops" \
    '{min_packets: $a, max_kernel_drops: $b}
     | with_entries(select(.value != "") | .value |= tonumber)')

for tool in tcpdump timeout; do
    if ! atp_have $tool; then
        atp_record "$record" "$check" error "$tool not found on PATH" "$started" \
            "$params" '{}' "$thresholds" '[]'
        atp_exit_for error; exit $?
    fi
done

# -U flushes each packet so a capture cut short by the timeout is still a
# complete pcap. Privilege dropping is left at tcpdump's own default: on the
# radio this runs as root and tcpdump drops to its build-time user, which is
# the behaviour an operator expects from tcpdump anywhere else.
set -- -i "$iface" -U -n -s "$snaplen" -w "$pcap"
[ -n "$count" ] && set -- "$@" -c "$count"
[ -n "$filter" ] && set -- "$@" "$filter"

timeout "$secs" tcpdump "$@" 2>"$pcap.stderr"
rc=$?
artifacts=$(jq -n --arg p "$pcap_rel" '[$p]')

# 124 is timeout(1) ending the capture on schedule — the normal case. 0 means
# tcpdump stopped by itself after -c packets. Anything else is a real failure.
if [ $rc -ne 0 ] && [ $rc -ne 124 ]; then
    why=$(grep -v -e '^listening on' -e 'packet' "$pcap.stderr" | head -n 1)
    atp_record "$record" "$check" error "tcpdump failed (exit $rc): ${why:-no message}" \
        "$started" "$params" '{}' "$thresholds" "$artifacts"
    atp_exit_for error; exit $?
fi

packets=$(tcpdump -r "$pcap" -n -q 2>/dev/null | wc -l | tr -d ' ')
bytes=$(wc -c <"$pcap" | tr -d ' ')
# tcpdump prints "N packets dropped by kernel" on exit. Absent (e.g. the line
# format differs on some build) it stays null rather than being guessed as 0.
drops=$(sed -n 's/^\([0-9][0-9]*\) packets\{0,1\} dropped by kernel.*/\1/p' "$pcap.stderr" | head -n 1)

metrics=$(jq -n --argjson p "$packets" --argjson b "$bytes" --arg d "$drops" \
    '{packets: $p, pcap_bytes: $b,
      kernel_drops: (if $d == "" then null else ($d | tonumber) end)}')

result=unjudged
reason="captured; no threshold given"
violations=''
if [ -n "$min_pkts" ]; then
    result=pass
    atp_exceeds "$packets" min "$min_pkts" && violations="packets=$packets (min $min_pkts)"
fi
if [ -n "$max_drops" ]; then
    result=pass
    if [ -z "$drops" ]; then
        violations="$violations${violations:+; }kernel_drops not reported by tcpdump"
    elif atp_exceeds "$drops" max "$max_drops"; then
        violations="$violations${violations:+; }kernel_drops=$drops (max $max_drops)"
    fi
fi

if [ -n "$violations" ]; then
    result=fail reason="threshold violated: $violations"
elif [ "$result" = pass ]; then
    reason="all thresholds met"
fi

atp_record "$record" "$check" "$result" "$reason" "$started" \
    "$params" "$metrics" "$thresholds" "$artifacts"
atp_exit_for "$result"; exit $?
