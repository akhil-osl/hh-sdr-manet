#!/bin/sh
# ocpi-selftest.sh — prove atp-ocpi.sh judges correctly, on the host.
#
#   ocpi-selftest.sh [work-dir]
#
# The board is replaced by fixtures: generated dma_stream logs, a fake
# /sys FPGA state file and /proc, and PATH shims for ps, dmesg, ocpidriver
# and sleep. Every case is run twice: with the host tools, and with BusyBox's
# awk, grep, sed and friends first on PATH, because the board runs BusyBox and
# BusyBox awk is not GNU awk.
#
# Each case states the result the specification demands. A script that can
# only pass proves nothing, so most cases are built to fail or to be refused.
#
# Needs jq (to check every record is valid JSON) and busybox. Exits 77
# (skipped) without them.

set -u

HERE=$(cd "$(dirname "$0")/.." && pwd)
ATP=$HERE/atp-ocpi.sh
WORK=${1:-$(mktemp -d)}
mkdir -p "$WORK"

for t in jq busybox; do
    command -v "$t" >/dev/null 2>&1 || { echo "SKIP: $t not installed"; exit 77; }
done

FAILS=0 RUNS=0
export FAKE_SLEPT="$WORK/slept"

# --- fixtures ---------------------------------------------------------------

SHIMS=$WORK/shims BB=$WORK/bb
mkdir -p "$SHIMS" "$BB"

cat >"$SHIMS/ps" <<'EOF'
#!/bin/sh
echo "  PID USER       VSZ STAT COMMAND"
i=0
while [ "$i" -lt "${FAKE_DMA:-0}" ]; do
    echo " 90$i root     12345 S    ./dma_stream qpsk_dma_null_app.xml 0"
    i=$((i + 1))
done
EOF
# Once the sleep shim has run, dmesg shows FAKE_DMESG_LATER: a kernel error
# that appeared during the watch.
cat >"$SHIMS/dmesg" <<'EOF'
#!/bin/sh
if [ -n "${FAKE_DMESG_LATER:-}" ] && [ -e "$FAKE_SLEPT" ]; then
    cat "$FAKE_DMESG_LATER"
else
    cat "${FAKE_DMESG:-/dev/null}"
fi
EOF
cat >"$SHIMS/ocpidriver" <<'EOF'
#!/bin/sh
if [ "${FAKE_DRIVER:-loaded}" = loaded ]; then
    echo "The driver module is currently loaded."
else
    echo "Driver not loaded; loading it."
fi
EOF
# watch sleeps while the stream writes. The shim plays the stream instead:
# it appends the lines in $FAKE_APPEND to the log, then returns at once.
cat >"$SHIMS/sleep" <<'EOF'
#!/bin/sh
[ -n "${FAKE_APPEND:-}" ] && cat "$FAKE_APPEND" >>"$FAKE_LOG"
touch "$FAKE_SLEPT"
exit 0
EOF
chmod +x "$SHIMS"/*

for a in awk grep sed head tail wc cat cp mkdir date uname dirname tr; do
    ln -sf "$(command -v busybox)" "$BB/$a"
done

# gen OUT START N STEP TXRATE LAG BITRATE [ERR_AT] [NOSYNC_AT] [TAG_LAST]
# N counter lines STEP seconds apart from time-of-day START (seconds).
# TXRATE words/s in; rx trails by LAG words; bits grow at BITRATE.
# ERR_AT: line from which errs=5. NOSYNC_AT: that line reads in_sync=false.
gen() {
    awk -v start="$2" -v n="$3" -v step="$4" -v rate="$5" -v lag="$6" \
        -v br="$7" -v errat="${8:-0}" -v nosync="${9:-0}" 'BEGIN {
        for (i = 1; i <= n; i++) {
            t = (start + (i - 1) * step) % 86400
            e = (i - 1) * step
            tx = 1000000 + rate * e; rx = tx - lag
            errs = (errat && i >= errat) ? 5 : 0
            sync = (nosync == i) ? "false" : "true"
            tag = (i == n) ? "FINAL" : "live "
            printf "%02d:%02d:%02d %s tx.in_words=%.0f tx.underrun=0 tx.out_dropped=0 | rx.out_words=%.0f rx.overflow=0 in_sync=%s bits=%.0f errs=%d | src=%.0f B sink=%.0f B\n", \
                int(t / 3600), int(t % 3600 / 60), t % 60, tag, tx, rx, sync, br * e, errs, tx * 4, rx * 4
        }
    }' >"$1"
}

L=$WORK/logs
mkdir -p "$L"
# Healthy stream: 473 kword/s, 10 s lines, gap 1000 words.
gen "$L/good.log"     36000  7 10 473000   1000 15360000
# Gap above the 200 000 limit.
gen "$L/biggap.log"   36000  7 10 473000 250000 15360000
# 300 kword/s: below the 400 kword/s floor.
gen "$L/slow.log"     36000  7 10 300000   1000 15360000
# BER: 61 lines = 600 s at 15.36 Mbit/s = 9.216 Gbit.
gen "$L/ber.log"      36000 61 10 473000   1000 15360000
gen "$L/ber-errs.log" 36000 61 10 473000   1000 15360000 40
gen "$L/ber-sync.log" 36000 61 10 473000   1000 15360000 0 30
gen "$L/ber-short.log" 36000 31 10 473000  1000 15360000
# Crosses midnight: 23:59:40 to 00:00:20 is 40 s.
gen "$L/midnight.log" 86380  5 10 473000   1000 15360000
gen "$L/one.log"      36000  1 10 473000   1000 15360000
# Application restarted inside the log: counters go back down.
{ cat "$L/good.log"; gen "$L/tmp.log" 36100 3 10 473000 1000 15360000; cat "$L/tmp.log"; } >"$L/reset.log"
# A required counter unreadable ("?").
sed '3s/rx.out_words=[0-9]*/rx.out_words=?/' "$L/good.log" >"$L/unreadable.log"
# Log with other dma_stream chatter mixed in, which must be ignored.
{ echo "10:00:00 DSP qpsk_tx.mod_select -> 1"; cat "$L/good.log"; echo "10:01:05 dma_stream: stopped after 65 s"; } >"$L/chatter.log"

B=$WORK/board
mkdir -p "$B/proc"
echo operating >"$B/fpga_state"
echo unknown >"$B/fpga_state_bad"
cat >"$B/proc/meminfo" <<'EOF'
MemTotal:         119540 kB
MemFree:           80000 kB
EOF
cat >"$B/proc/iomem" <<'EOF'
00000000-0fffffff : System RAM
  00008000-00afffff : Kernel code
EOF
sed 's/0fffffff/1fffffff/' "$B/proc/iomem" >"$B/iomem_bad"
printf '[    0.000000] Booting Linux\n[    5.100000] opencpi: loaded\n' >"$B/dmesg_clean"
{ cat "$B/dmesg_clean"; echo "[  900.000000] Internal error: Oops: 17 [#1] SMP ARM"; } >"$B/dmesg_oops"
echo "OCPI( 2:123.456): Allocation failure: unable to allocate 104857600 bytes" >"$B/console_alloc"
echo "all workers started" >"$B/console_ok"

# --- runner -----------------------------------------------------------------

# expect NAME WANT-EXIT [WANT-RESULT] -- atp-ocpi.sh args...
# Runs in both tool modes. Checks exit code, that the record is valid JSON,
# and that its result matches the exit code.
expect() {
    name=$1 want=$2; shift 2
    [ "$1" = -- ] && shift
    for mode in host busybox; do
        RUNS=$((RUNS + 1))
        out=$WORK/ev/$name.$mode
        rm -rf "$out"; mkdir -p "$out"; rm -f "$FAKE_SLEPT"
        if [ $mode = busybox ]; then p=$SHIMS:$BB:$PATH; else p=$SHIMS:$PATH; fi
        PATH=$p OCPI_FPGA_STATE_FILE=${FPGA:-$B/fpga_state} \
            OCPI_PROC_DIR=${PROCD:-$B/proc} \
            sh "$ATP" "$@" -o "$out" >"$out/stdout" 2>"$out/stderr"
        got=$?
        rec=$(ls "$out"/*.json 2>/dev/null | head -n 1)
        res=""
        if [ -n "$rec" ]; then
            res=$(jq -r .result "$rec" 2>/dev/null) || res="INVALID-JSON"
        fi
        if [ "$got" != "$want" ]; then
            echo "FAIL $name [$mode]: exit $got, want $want ($(jq -r .reason "$rec" 2>/dev/null || cat "$out/stderr"))"
            FAILS=$((FAILS + 1))
        elif [ "$want" != 2 ] && [ "$res" = "INVALID-JSON" -o -z "$res" ]; then
            echo "FAIL $name [$mode]: no valid evidence record"
            FAILS=$((FAILS + 1))
        else
            echo "ok   $name [$mode]: exit $got${res:+ ($res)}"
        fi
    done
}

# jqcheck NAME FILTER — FILTER must be true on the busybox-mode record.
jqcheck() {
    RUNS=$((RUNS + 1))
    rec=$(ls "$WORK/ev/$1.busybox"/*.json | head -n 1)
    if jq -e "$2" "$rec" >/dev/null 2>&1; then
        echo "ok   $1: $2"
    else
        echo "FAIL $1: $2 is not true"; FAILS=$((FAILS + 1))
    fi
}

# --- cases: analyze ---------------------------------------------------------

echo "== analyze"
# Operator TC-9: pass needs the gap-growth limit, which the spec does not give.
expect opr09-nolimit 5 -- analyze --case opr-09 --log "$L/good.log"
expect opr09-pass    0 -- analyze --case opr-09 --log "$L/good.log" --max-gap-growth 1000
expect opr09-biggap  1 -- analyze --case opr-09 --log "$L/biggap.log" --max-gap-growth 1000
expect apl06-pass    0 -- analyze --case apl-06 --log "$L/chatter.log" --max-gap-growth 1000
jqcheck apl06-pass '.metrics.lines == 7 and .metrics.gap_max == 1000'

expect apl07-pass    0 -- analyze --case apl-07 --log "$L/good.log" --sink /dev/null
expect apl07-nosink  5 -- analyze --case apl-07 --log "$L/good.log"
expect apl07-slow    1 -- analyze --case apl-07 --log "$L/slow.log" --sink /dev/null
jqcheck apl07-pass '.metrics.tx_words_per_s == 473000 and .params.sink == "/dev/null"'

expect apl15-nolimits 5 -- analyze --case apl-15 --log "$L/good.log"
expect apl15-pass     0 -- analyze --case apl-15 --log "$L/good.log" --min-duration-s 60 \
    --min-words-per-s 400000 --max-loss-delta 0 --max-gap-growth 0
expect apl15-short    1 -- analyze --case apl-15 --log "$L/good.log" --min-duration-s 3600 \
    --min-words-per-s 400000 --max-loss-delta 0 --max-gap-growth 0

expect ber-pass   0 -- analyze --case ber-01 --log "$L/ber.log"
jqcheck ber-pass '.metrics.bits_last == 9216000000 and .metrics.duration_s == 600 and .metrics.ber_upper_95 != null'
expect ber-errs   1 -- analyze --case ber-01 --log "$L/ber-errs.log"
expect ber-sync   1 -- analyze --case ber-01 --log "$L/ber-sync.log"
expect ber-short  1 -- analyze --case ber-01 --log "$L/ber-short.log"

expect midnight   0 -- analyze --case apl-06 --log "$L/midnight.log" --max-gap-growth 0
jqcheck midnight '.metrics.duration_s == 40'
expect one-line   3 -- analyze --case apl-06 --log "$L/one.log"
expect reset      3 -- analyze --case apl-06 --log "$L/reset.log" --max-gap-growth 0
expect unreadable 3 -- analyze --case apl-06 --log "$L/unreadable.log" --max-gap-growth 0
expect nolog      3 -- analyze --case apl-06 --log "$L/does-not-exist.log"

# JSON escaping: quote, backslash and a tab must survive into valid JSON.
TAB=$(printf '\t')
expect escape 0 -- analyze --case apl-07 --log "$L/good.log" --sink "a\"b\\c${TAB}d"
jqcheck escape ".params.sink == \"a\\\"b\\\\c\\td\""

# --- cases: preflight / postcheck -------------------------------------------

echo "== preflight, postcheck"
export FAKE_DMESG=$B/dmesg_clean FAKE_DRIVER=loaded FAKE_DMA=0
# MemTotal is "about 119 540 kB" with no tolerance: unjudged unless given one.
expect pre-notol   5 -- preflight
expect pre-pass    0 -- preflight --memtotal-kb 119540 --memtotal-tol-kb 500
jqcheck pre-pass '.metrics.fpga_state == "operating" and .metrics.driver == "loaded"'
FPGA=$B/fpga_state_bad; expect pre-fpga 1 -- preflight --memtotal-kb 119540 --memtotal-tol-kb 500; FPGA=
FAKE_DRIVER=no; expect pre-driver 1 -- preflight --memtotal-kb 119540 --memtotal-tol-kb 500; FAKE_DRIVER=loaded
FAKE_DMA=1; expect pre-leftover 1 -- preflight --memtotal-kb 119540 --memtotal-tol-kb 500; FAKE_DMA=0
FAKE_DMESG=$B/dmesg_oops; expect pre-oops 1 -- preflight --memtotal-kb 119540 --memtotal-tol-kb 500; FAKE_DMESG=$B/dmesg_clean
expect pre-alloc 1 -- preflight --memtotal-kb 119540 --memtotal-tol-kb 500 --console "$B/console_alloc"
expect pre-console-ok 0 -- preflight --memtotal-kb 119540 --memtotal-tol-kb 500 --console "$B/console_ok"
expect pre-memtotal 1 -- preflight --memtotal-kb 125000 --memtotal-tol-kb 500
mkdir -p "$B/proc_badram"; cp "$B/proc/meminfo" "$B/proc_badram/"; cp "$B/iomem_bad" "$B/proc_badram/iomem"
PROCD=$B/proc_badram; expect pre-ram 1 -- preflight --memtotal-kb 119540 --memtotal-tol-kb 500; PROCD=
# No ocpidriver on PATH: the check broke, it did not find a fault.
mv "$SHIMS/ocpidriver" "$SHIMS/ocpidriver.off"
expect pre-noenv 3 -- preflight --memtotal-kb 119540 --memtotal-tol-kb 500
mv "$SHIMS/ocpidriver.off" "$SHIMS/ocpidriver"

expect post-pass 0 -- postcheck
FAKE_DMA=1; expect post-leftover 1 -- postcheck; FAKE_DMA=0
expect post-alloc 1 -- postcheck --console "$B/console_alloc"

# --- cases: watch -----------------------------------------------------------

echo "== watch"
# The log already holds old lines; only lines written during the watch count.
W=$WORK/watch
mkdir -p "$W"
gen "$W/old.log" 30000 5 10 100 999999 0
gen "$W/new.log" 36000 13 10 473000 1000 15360000
watch_run() {  # NAME WANT APPEND-FILE [extra args]
    n=$1 w=$2 app=$3; shift 3
    cp "$W/old.log" "$W/live.log"
    export FAKE_LOG=$W/live.log FAKE_APPEND=$app FAKE_DMA=1
    expect "$n" "$w" -- watch --log "$W/live.log" "$@"
    FAKE_APPEND="" FAKE_DMA=0
}
watch_run watch-apl06 0 "$W/new.log" --case apl-06 --duration 120 --max-gap-growth 0
jqcheck watch-apl06 '.metrics.lines == 13'
watch_run watch-apl16 0 "$W/new.log" --case apl-16 --duration 120
{ head -n 5 "$W/new.log"; echo "10:00:50 dma_stream: application finished on its own"; } >"$W/exit.log"
watch_run watch-apl16-exit 1 "$W/exit.log" --case apl-16 --duration 120
head -n 3 "$W/new.log" >"$W/hang.log"
watch_run watch-apl16-hang 1 "$W/hang.log" --case apl-16 --duration 120
export FAKE_DMESG_LATER=$B/dmesg_oops
watch_run watch-apl16-oops 1 "$W/new.log" --case apl-16 --duration 120
unset FAKE_DMESG_LATER

# --- usage errors -----------------------------------------------------------

echo "== usage"
expect usage-nocase  2 -- analyze --log "$L/good.log"
expect usage-badcase 2 -- analyze --case nope --log "$L/good.log"
expect usage-apl16   2 -- analyze --case apl-16 --log "$L/good.log"
expect usage-nonnum  2 -- analyze --case apl-06 --log "$L/good.log" --max-gap-growth lots
expect usage-memtol  2 -- preflight --memtotal-kb 119540

echo "$((RUNS - FAILS))/$RUNS checks passed"
[ "$FAILS" = 0 ]
