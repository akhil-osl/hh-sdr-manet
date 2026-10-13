#!/bin/sh
#
# check-dissector.sh — validate hh_manet.lua against the real C codec.
#
# 1. hh_wire_pcapgen writes golden captures with the real encoder, and an
#    expected.json holding what the real decoder makes of each frame.
# 2. tshark decodes the same captures with hh_manet.lua.
# 3. Every frame's status and every field are compared. Scaled values are
#    compared within half a unit of their wire resolution (0.1 for
#    coordinates, 1e-4 for fractions); everything else must match exactly.
#
# Any difference means wire.c and the dissector have drifted apart, and fails.
# Exits 77 (skipped) when tshark with Lua support, or jq, is unavailable.
#
# usage: check-dissector.sh PATH/TO/hh_wire_pcapgen [WORKDIR]

set -u

here=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
gen=${1-}
[ -x "$gen" ] || { echo "usage: $0 PATH/TO/hh_wire_pcapgen [WORKDIR]" >&2; exit 2; }

for t in tshark jq; do
    command -v $t >/dev/null 2>&1 || { echo "check-dissector: SKIP — $t not found" >&2; exit 77; }
done
if ! tshark -v 2>/dev/null | grep -q 'with Lua'; then
    echo "check-dissector: SKIP — tshark built without Lua" >&2
    exit 77
fi

work=${2:-$(mktemp -d "${TMPDIR:-/tmp}/hh-dissector.XXXXXX")}
mkdir -p "$work" || exit 3
"$gen" "$work" || { echo "check-dissector: hh_wire_pcapgen failed" >&2; exit 3; }

fails=0
for f in beacons.pcap routes.pcap; do
    if ! tshark -X "lua_script:$here/hh_manet.lua" -r "$work/$f" \
            -T json --no-duplicate-keys >"$work/$f.tshark.json" 2>"$work/$f.tshark.err"; then
        echo "FAIL  $f: tshark failed:" >&2
        sed 's/^/        /' "$work/$f.tshark.err" >&2
        fails=$((fails + 1))
        continue
    fi

    jq -L "$here" 'include "hh_manet"; hh_frames' "$work/$f.tshark.json" >"$work/$f.actual.json" || {
        echo "FAIL  $f: could not normalise tshark output" >&2
        fails=$((fails + 1))
        continue
    }

    # A comparator that errors prints nothing, and nothing reads as "no
    # differences" — so its own failure must fail the check explicitly.
    if ! diffs=$(jq -r -n --arg file "$f" \
        --slurpfile exp "$work/expected.json" --slurpfile act "$work/$f.actual.json" '
        def abs: if . < 0 then -. else . end;
        def tol($k):
            if ($k | startswith("position_")) then 0.05
            elif $k == "power_battery" or $k == "metric" then 0.00005
            else 0 end;
        def same($k; $a; $b):
            if ($a | type) == "number" and ($b | type) == "number"
            then (($a - $b) | abs) <= tol($k)
            else $a == $b end;
        def cmp($path; $e; $a):
            $e | keys[] as $k
            | if $k == "entries" then
                  if ($e.entries | length) != (($a.entries // []) | length) then
                      "\($path).entries: \($e.entries | length) expected, \(($a.entries // []) | length) decoded"
                  else
                      range(0; $e.entries | length) as $i
                      | cmp("\($path).entries[\($i)]"; $e.entries[$i]; $a.entries[$i])
                  end
              elif same($k; $e[$k]; $a[$k]) | not then
                  "\($path).\($k): expected \($e[$k]), decoded \($a[$k])"
              else empty end;
        ($exp[0] | map(select(.file == $file))) as $es
        | ($act[0] | map({key: (.frame | tostring), value: .}) | from_entries) as $byframe
        | ($es | length) as $ne | ($act[0] | length) as $na
        | (if $ne != $na then "frame count: \($ne) expected, \($na) decoded" else empty end),
          ($es[] as $e
           | $byframe[$e.frame | tostring] as $a
           | if $a == null then "frame \($e.frame): not decoded"
             elif $a.status != $e.status then
                 "frame \($e.frame): status expected \($e.status), decoded \($a.status)"
             elif $e.fields then cmp("frame \($e.frame)"; $e.fields; $a.fields // {})
             else empty end)'); then
        echo "FAIL  $f: comparison itself failed" >&2
        fails=$((fails + 1))
        continue
    fi

    n=$(jq --arg file "$f" 'map(select(.file == $file)) | length' "$work/expected.json")
    if [ -n "$diffs" ]; then
        echo "FAIL  $f:"
        printf '%s\n' "$diffs" | sed 's/^/        /'
        fails=$((fails + 1))
    else
        echo "ok    $f: $n frames match the C decoder"
    fi
done

# The evidence path: atp-decode.sh must summarise the same golden capture
# exactly — 6 frames, 3 decoded ok, 3 rejected, node 42 seen twice with no gap.
if ev=$("$here/../atp-decode.sh" -r "$work/beacons.pcap" -o "$work/evidence" \
        --max-bad-frames 0 2>&1); then
    rc=0
else
    rc=$?
fi
if [ "$rc" = 1 ] && printf '%s' "$ev" | jq -e '
        .result == "fail" and .metrics.frames == 6 and .metrics.by_status.ok == 3
        and .metrics.bad_frames == 3
        and (.metrics.nodes[] | select(.node_id == 42) | .beacons == 2 and .seq_gaps == 0)' \
        >/dev/null 2>&1; then
    echo "ok    atp-decode.sh: evidence matches the golden capture"
else
    echo "FAIL  atp-decode.sh (exit $rc):"
    printf '%s\n' "$ev" | sed 's/^/        /'
    fails=$((fails + 1))
fi

[ "$fails" -eq 0 ]
