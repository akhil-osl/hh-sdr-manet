#!/bin/sh
#
# atp-decode.sh — decode a capture with the MANET dissector and write evidence.
#
# Runs on the ANALYSIS HOST, not on the radio: it needs tshark with Lua, which
# the radio image does not carry. The radio captures (atp-capture.sh); this
# decodes afterwards, so a stored pcap can be re-analysed at any time.
#
# Reports per-status frame counts and, for beacons, per-node sequence
# continuity — a gap in a node's beacon sequence numbers is a beacon the
# capture point never saw.
#
# Only captures in the private link types of wireshark/hh_manet.lua decode as
# MANET frames today (DLT_USER0 beacons, DLT_USER1 routing updates). On the
# radio, MANET frames have no defined capture point yet (unknown.md U-16);
# frames of any other kind are counted as "not_hh", not guessed at.

set -u

ATP_PROG=atp-decode
ATP_DIR=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
. "$ATP_DIR/lib/atp.sh"

usage() {
    cat <<EOF
usage: $ATP_PROG -r PCAP -o EVIDENCE_DIR [options]

  -r PCAP               capture to decode (required)
  -o DIR                evidence directory, created if absent (required)
  -n NAME               run name, used in the check and file names (default: pcap basename)

  thresholds (omit and the result is "unjudged", never "pass"):
  --min-frames N        minimum frames decoded as MANET frames
  --max-bad-frames N    maximum frames with any status other than "ok"
  --max-seq-gaps N      maximum missing beacon sequence numbers, all nodes

exit: 0 pass, 1 fail, 2 usage, 3 error, 5 unjudged
EOF
}

pcap='' outdir='' name='' min_frames='' max_bad='' max_gaps=''

while [ $# -gt 0 ]; do
    case $1 in
        -r) pcap=${2-}; shift ;;
        -o) outdir=${2-}; shift ;;
        -n) name=${2-}; shift ;;
        --min-frames) min_frames=${2-}; shift ;;
        --max-bad-frames) max_bad=${2-}; shift ;;
        --max-seq-gaps) max_gaps=${2-}; shift ;;
        -h|--help) usage; exit 0 ;;
        *) usage >&2; atp_die "unknown argument: $1" ;;
    esac
    shift
done

[ -n "$pcap" ]   || { usage >&2; atp_die "-r PCAP is required"; }
[ -n "$outdir" ] || { usage >&2; atp_die "-o EVIDENCE_DIR is required"; }
for t in "$min_frames" "$max_bad" "$max_gaps"; do
    [ -z "$t" ] || atp_is_number "$t" || atp_die "threshold is not a number: $t"
done
[ -n "$name" ] || name=$(basename "$pcap" .pcap)

atp_require_jq
mkdir -p "$outdir/raw" || atp_die "cannot create $outdir/raw"

check="decode.$name"
record="$outdir/$check.json"
frames_rel="raw/$check.frames.json"
started=$(atp_now_utc)
lua="$ATP_DIR/wireshark/hh_manet.lua"

params=$(jq -n --arg pcap "$pcap" --arg lua "$lua" '{pcap: $pcap, dissector: $lua}')
thresholds=$(jq -n --arg a "$min_frames" --arg b "$max_bad" --arg c "$max_gaps" \
    '{min_frames: $a, max_bad_frames: $b, max_seq_gaps: $c}
     | with_entries(select(.value != "") | .value |= tonumber)')

fail_error() {
    atp_record "$record" "$check" error "$1" "$started" "$params" '{}' "$thresholds" "${2:-[]}"
    atp_exit_for error; exit $?
}

atp_have tshark || fail_error "tshark not found on PATH"
tshark -v 2>/dev/null | grep -q 'with Lua' || fail_error "tshark is built without Lua"
[ -r "$pcap" ] || fail_error "cannot read $pcap"

# The per-frame records are kept as an artifact: they are what the summary
# below was computed from, so an auditor can check any number in it.
tshark -X "lua_script:$lua" -r "$pcap" -T json --no-duplicate-keys 2>"$outdir/$frames_rel.stderr" \
    | jq -L "$ATP_DIR/wireshark" 'include "hh_manet"; hh_frames' >"$outdir/$frames_rel" \
    || fail_error "tshark/dissector failed on $pcap: $(head -n 1 "$outdir/$frames_rel.stderr")"

artifacts=$(jq -n --arg f "$frames_rel" '[$f]')

metrics=$(jq '
    # Sequence numbers are uint32 and wrap. A spread wider than half the space
    # can only be a wrap (the same serial-number rule as core/seq.h), so the
    # low side is lifted past 2^32 before counting what is missing.
    def unwrap: if (max - min) > 2147483648
                then map(if . < 2147483648 then . + 4294967296 else . end) else . end;
    def gaps: (unwrap | sort | unique) as $u
              | if ($u | length) == 0 then 0 else ($u[-1] - $u[0] + 1 - ($u | length)) end;
    { frames: length,
      manet_frames: (map(select(.proto != null)) | length),
      beacons: (map(select(.proto == "beacon")) | length),
      route_updates: (map(select(.proto == "route")) | length),
      by_status: (group_by(.status) | map({key: .[0].status, value: length}) | from_entries),
      bad_frames: (map(select(.status != "ok")) | length),
      route_entries: (map(select(.proto == "route" and .fields) | .fields.count) | add // 0),
      nodes: (map(select(.proto == "beacon" and .fields))
              | group_by(.fields.node_id)
              | map((map(.fields.sequence_no)) as $s
                    | { node_id: .[0].fields.node_id,
                        beacons: length,
                        # capture order (group_by is stable), not min/max,
                        # so a wrapped sequence still reads first -> last
                        seq_first: $s[0], seq_last: $s[-1],
                        duplicates: (($s | length) - ($s | unique | length)),
                        seq_gaps: ($s | gaps) })) }
    | .seq_gaps_total = (.nodes | map(.seq_gaps) | add // 0)' "$outdir/$frames_rel") \
    || fail_error "could not summarise decoded frames" "$artifacts"

get() { printf '%s' "$metrics" | jq -r ".$1"; }

result=unjudged
violations=''
judge() {  # judge LABEL VALUE OP LIMIT
    [ -n "$4" ] || return 0
    result=pass
    atp_exceeds "$2" "$3" "$4" && violations="$violations${violations:+; }$1=$2 ($3 $4)"
}
judge manet_frames   "$(get manet_frames)"   min "$min_frames"
judge bad_frames     "$(get bad_frames)"     max "$max_bad"
judge seq_gaps_total "$(get seq_gaps_total)" max "$max_gaps"

if [ -n "$violations" ]; then
    result=fail reason="threshold violated: $violations"
elif [ "$result" = pass ]; then
    reason="all thresholds met"
else
    reason="decoded; no threshold given"
fi

atp_record "$record" "$check" "$result" "$reason" "$started" \
    "$params" "$metrics" "$thresholds" "$artifacts"
atp_exit_for "$result"; exit $?
