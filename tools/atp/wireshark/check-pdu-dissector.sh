#!/bin/sh
#
# check-pdu-dissector.sh - validate hh_pdu_v2.lua against the C codec.
#
# 1. The worked examples A to E of MAC frame specification v2 (and vector A with
#    a damaged CRC) are written as a capture with text2pcap and decoded.
# 2. hh-slot-sim writes a capture of PDUs made by the real encoder. Every PDU
#    must decode with status ok, and the PDU and message counts must equal the
#    simulator's own log.
#
# Exits 77 (skipped) when tshark with Lua support or text2pcap is unavailable.
#
# usage: check-pdu-dissector.sh PATH/TO/hh-slot-sim [WORKDIR]

set -u

here=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
sim=${1-}
[ -x "$sim" ] || { echo "usage: $0 PATH/TO/hh-slot-sim [WORKDIR]" >&2; exit 2; }

for t in tshark text2pcap; do
    command -v $t >/dev/null 2>&1 || { echo "check-pdu-dissector: SKIP - $t not found" >&2; exit 77; }
done
if ! tshark -v 2>/dev/null | grep -q 'with Lua'; then
    echo "check-pdu-dissector: SKIP - tshark built without Lua" >&2
    exit 77
fi

work=${2:-$(mktemp -d "${TMPDIR:-/tmp}/hh-pdu-dissector.XXXXXX")}
mkdir -p "$work" || exit 3
lua="$here/hh_pdu_v2.lua"
fields="-e hhpdu.seq -e hhpdu.src -e hhpdu.messages -e hhpdu.status -e hhpdu.slots"
fails=0

cat >"$work/vectors.txt" <<'HEX'
000000 41 A8 21 10 00 FF FF 01 00 10 00 00 00 00 03 20 00 00 F0 02 00 05 A0 07 48 45 4C 4C 4F 5F 1A D6 46

000000 41 A8 40 10 00 FF FF 02 00 18 00 00 00 01 03 00 02 00 01 00 E8 00 00 00 80 0A 29 E0 AC

000000 41 A8 22 10 00 FF FF 01 00 10 10 00 00 00 03 20 00 00 80 A4 7F 01 01 00 BC A4 7F 01 00 04 02 20 02 00 9C EC EA E6

000000 41 A8 01 10 00 FF FF 03 00 14 00 00 00 00 00 20 FF FF 0B 00 01 E7 91 3C 5A 01 01 03 DE FD 0F E2 55 5B 6A

000000 41 A8 23 10 00 FF FF 01 00 10 00 00 00 00 03 20 00 00 20 03 00 0F 00 02 E7 91 3C 5A BE A4 7F 01 03 00 40 00 4E 01 92 EB 30 E7

000000 41 A8 21 10 00 FF FF 01 00 10 00 00 00 00 03 20 00 00 F0 02 00 05 A0 07 48 45 4C 4C 4F 5F 1A D6 47
HEX

text2pcap -q -l 149 "$work/vectors.txt" "$work/vectors.pcap" || { echo "check-pdu-dissector: text2pcap failed" >&2; exit 3; }
# shellcheck disable=SC2086
tshark -X "lua_script:$lua" -r "$work/vectors.pcap" -T fields $fields >"$work/vectors.actual" 2>"$work/vectors.err" || {
    echo "FAIL  vectors: tshark failed:" >&2; sed 's/^/        /' "$work/vectors.err" >&2; exit 1; }

printf '33\t0x0001\t1\tok\t5\n64\t0x0002\t0\tok\t9\n34\t0x0001\t0\tok\t5\n1\t0x0003\t1\tok\t\n35\t0x0001\t1\tok\t5\n33\t0x0001\t1\tbad_crc\t5\n' >"$work/vectors.expected"
if ! diff "$work/vectors.expected" "$work/vectors.actual" >"$work/vectors.diff"; then
    echo "FAIL  spec vectors differ from the dissector (expected <, actual >):" >&2
    sed 's/^/        /' "$work/vectors.diff" >&2
    fails=$((fails + 1))
fi

"$sim" --gen-s 3 --drain-s 1 --log "$work/sim.log" --pcap "$work/sim.pcap" --summary "$work/sim.summary" >/dev/null \
    || { echo "check-pdu-dissector: hh-slot-sim failed" >&2; exit 3; }
tshark -X "lua_script:$lua" -r "$work/sim.pcap" -T fields -e hhpdu.status -e hhpdu.messages >"$work/sim.actual" 2>"$work/sim.err" || {
    echo "FAIL  sim.pcap: tshark failed:" >&2; sed 's/^/        /' "$work/sim.err" >&2; exit 1; }

want_pdus=$(grep -c 'ev=slot_tx' "$work/sim.log")
want_msgs=$(sed -n 's/.* msgs=\([0-9][0-9]*\) .*/\1/p' "$work/sim.log" | awk '{s += $1} END {print s + 0}')
got_pdus=$(wc -l <"$work/sim.actual" | tr -d ' ')
got_msgs=$(awk -F'\t' '{s += $2} END {print s + 0}' "$work/sim.actual")
bad=$(awk -F'\t' '$1 != "ok"' "$work/sim.actual" | wc -l | tr -d ' ')
if [ "$got_pdus" != "$want_pdus" ] || [ "$got_msgs" != "$want_msgs" ] || [ "$bad" != 0 ] || [ "$want_pdus" = 0 ]; then
    echo "FAIL  sim.pcap: pdus $got_pdus (want $want_pdus), messages $got_msgs (want $want_msgs), not ok: $bad" >&2
    fails=$((fails + 1))
fi

if [ "$fails" -ne 0 ]; then
    echo "check-pdu-dissector: $fails FAILED (files in $work)" >&2
    exit 1
fi
echo "check-pdu-dissector: OK (6 spec vectors, $got_pdus simulated PDUs, $got_msgs messages)"
