#!/bin/sh
# atp-ocpi.sh — judge the OSL QPSK SDR data path against OSL-SQA-TCS-002-EX1
# (Operator, Application and BER Test Cases) and write JSON evidence.
#
# Runs ON THE BOARD under BusyBox sh, from the OpenCPI project's deploy
# directory, after `source .../opencpi-setup.sh -s` (section 6.1 of the parent
# specification). Needs only sh, awk, grep, sed, ps, dmesg, sleep: the board
# image has no jq and no timeout.
#
# This script JUDGES; it never starts, stops or reconfigures the radio.
# Starting and stopping stay with the operator and the project's own
# board_stream.sh, because the bring-up order is hardware-critical (touching
# the PL before ad9361_init hangs the CPU) and a test script must not own it.
# See README.md in this directory for which test case uses which sub-command.
#
# Limits are the ones written in the test specification. Where the
# specification names a criterion but gives no number ("not growing",
# "the agreed duration"), there is no default: the criterion is measured and
# reported as unjudged until the caller passes the agreed number.

set -u

ATP_PROG=atp-ocpi
HERE=$(cd "$(dirname "$0")" && pwd)
. "$HERE/lib/ocpi.sh"
ORACLE_AWK=$HERE/lib/oracle.awk

# Board paths. Overridable so the self-test can point them at fixtures.
FPGA_STATE_FILE=${OCPI_FPGA_STATE_FILE:-/sys/class/fpga_manager/fpga0/state}
PROC_DIR=${OCPI_PROC_DIR:-/proc}

# Kernel errors, as the project's own board_stream.sh status counts them.
DMESG_PATTERN='oops|bad page'
# The DMA block the data app reserves (TC-10: an allocation failure of this
# size means a previous run still holds it).
DMA_BLOCK_BYTES=104857600

usage() {
    cat <<'EOF'
usage: atp-ocpi.sh preflight [options]
       atp-ocpi.sh postcheck [options]
       atp-ocpi.sh analyze   --case CASE --log FILE [options]
       atp-ocpi.sh watch     --case CASE --duration S [--log FILE] [options]

sub-commands
  preflight   read-only state before start-up: FPGA manager, OpenCPI driver,
              no dma_stream left over, System RAM range, MemTotal, dmesg
              (Operator TC-1 steps 1-3, Application TC-1)
  postcheck   read-only state after stop: no dma_stream left, DMA block not
              reported held, no kernel error
              (Operator TC-2, TC-8; Application TC-3)
  analyze     judge an existing dma_stream log for CASE
  watch       wait S seconds on a running stream, keep the counter lines
              written meanwhile, then judge them for CASE

cases (analyze, watch)
  opr-09      Operator TC-9    both word counters advance; gap below 200 000
  apl-06      Application TC-6 same criteria, both paths together
  apl-07      Application TC-7 >= 400 000 words/s each way; past 25 Mbit;
                               the sink recorded (--sink)
  apl-15      Application TC-15 long run; every limit must be agreed and given
  apl-16      Application TC-16 no exit, no hang, no kernel error (watch only)
  ber-01      BER TC-1         in_sync throughout; err_count 0;
                               bit_count >= 9.2 Gbit; >= 600 s

options
  -o DIR                 evidence directory (default: evidence)
  --log FILE             dma_stream log (default: /tmp/dma_stream.log)
  --console FILE         ocpirun console log to search for the DMA
                         allocation failure (preflight, postcheck)
  --sink TEXT            where the received data went (apl-07)
  --max-gap-growth N     words the gap may grow over the log (opr-09, apl-06,
                         apl-15); none in the specification, so unjudged
                         without it
  --min-duration-s S     the agreed duration (apl-15; ber-01 default 600)
  --max-loss-delta N     loss counter increase allowed (apl-15)
  --min-words-per-s N    throughput that must hold (apl-15)
  --memtotal-kb N        expected MemTotal, with --memtotal-tol-kb N
                         (the specification says "about 119 540 kB")
  --dmesg-pattern ERE    what counts as a kernel error
                         (default: 'oops|bad page', case-insensitive)
  -h                     this help

exit: 0 pass, 1 fail, 2 usage, 3 error, 4 blocked, 5 unjudged
EOF
}

# --- argument parsing -------------------------------------------------------

[ $# -ge 1 ] || { usage >&2; exit 2; }
CMD=$1; shift
case $CMD in
    -h|--help) usage; exit 0 ;;
    preflight|postcheck|analyze|watch) ;;
    *) ocpi_die "unknown sub-command '$CMD' (try -h)" ;;
esac

OUT=evidence LOG=/tmp/dma_stream.log CONSOLE="" CASE="" DURATION="" SINK=""
MAX_GAP_GROWTH="" MIN_DURATION="" MAX_LOSS="" MIN_WPS="" MEMTOTAL="" MEMTOL=""

need_arg() { [ $# -ge 2 ] || ocpi_die "$1 needs a value"; }
while [ $# -gt 0 ]; do
    case $1 in
        -o)                 need_arg "$@"; OUT=$2; shift ;;
        --log)              need_arg "$@"; LOG=$2; shift ;;
        --console)          need_arg "$@"; CONSOLE=$2; shift ;;
        --case)             need_arg "$@"; CASE=$2; shift ;;
        --duration)         need_arg "$@"; DURATION=$2; shift ;;
        --sink)             need_arg "$@"; SINK=$2; shift ;;
        --max-gap-growth)   need_arg "$@"; MAX_GAP_GROWTH=$2; shift ;;
        --min-duration-s)   need_arg "$@"; MIN_DURATION=$2; shift ;;
        --max-loss-delta)   need_arg "$@"; MAX_LOSS=$2; shift ;;
        --min-words-per-s)  need_arg "$@"; MIN_WPS=$2; shift ;;
        --memtotal-kb)      need_arg "$@"; MEMTOTAL=$2; shift ;;
        --memtotal-tol-kb)  need_arg "$@"; MEMTOL=$2; shift ;;
        --dmesg-pattern)    need_arg "$@"; DMESG_PATTERN=$2; shift ;;
        -h|--help)          usage; exit 0 ;;
        *) ocpi_die "unknown option '$1' (try -h)" ;;
    esac
    shift
done

for n in "$MAX_GAP_GROWTH" "$MIN_DURATION" "$MAX_LOSS" "$MIN_WPS" "$MEMTOTAL" "$MEMTOL" "$DURATION"; do
    [ -z "$n" ] || ocpi_is_number "$n" || ocpi_die "'$n' is not a number"
done
if [ -n "$MEMTOTAL" ] || [ -n "$MEMTOL" ]; then
    [ -n "$MEMTOTAL" ] && [ -n "$MEMTOL" ] || ocpi_die "--memtotal-kb and --memtotal-tol-kb go together"
fi

case $CMD in
    analyze|watch)
        [ -n "$CASE" ] || ocpi_die "$CMD needs --case"
        case $CASE in
            opr-09|apl-06|apl-07|apl-15|ber-01) ;;
            apl-16) [ "$CMD" = watch ] || ocpi_die "apl-16 needs a live run: use watch" ;;
            *) ocpi_die "unknown case '$CASE' (try -h)" ;;
        esac
        ;;
esac
if [ "$CMD" = watch ]; then
    [ -n "$DURATION" ] || ocpi_die "watch needs --duration"
fi

mkdir -p "$OUT/raw" || ocpi_die "cannot create $OUT/raw"
STARTED=$(ocpi_now_utc)
PARAMS="" METRICS="" ARTS=""
param()  { PARAMS="${PARAMS}$1=$2
"; }
metric() { METRICS="${METRICS}$1=$2
"; }
art()    { ARTS="${ARTS}$1
"; }

finish() {  # CHECK
    ocpi_verdict
    ocpi_record "$OUT/$1.json" "$1" "$RESULT" "$REASON" "$STARTED" \
        "$PARAMS" "$METRICS" "$V_THRESH" "$ARTS"
    ocpi_exit_for "$RESULT"
    exit $?
}

# --- board probes -----------------------------------------------------------

dma_stream_count() {
    ps w 2>/dev/null | grep -c "[d]ma_stream"
}

# dmesg_errors FILE — count kernel-error lines in a saved dmesg.
dmesg_errors() {
    grep -Eic "$DMESG_PATTERN" "$1" 2>/dev/null
}

# alloc_failures FILE... — lines reporting the DMA block allocation failure.
alloc_failures() {
    cat "$@" 2>/dev/null | grep -i "$DMA_BLOCK_BYTES" | grep -Eic 'fail|unable|cannot'
}

save_dmesg() {  # NAME
    if dmesg >"$OUT/raw/$1" 2>&1; then
        art "raw/$1"
        return 0
    fi
    ocpi_err "dmesg could not be read"
    return 1
}

# kernel_checks NAME — dmesg errors and DMA block failures; shared by
# preflight and postcheck.
kernel_checks() {
    save_dmesg "$1" || return
    kerr=$(dmesg_errors "$OUT/raw/$1")
    metric kernel_error_lines "$kerr"
    ocpi_limit kernel_error_lines "$kerr" max 0
    param dmesg_pattern "$DMESG_PATTERN"
    files="$OUT/raw/$1"
    if [ -n "$CONSOLE" ]; then
        if [ -r "$CONSOLE" ]; then
            files="$files $CONSOLE"; art "$CONSOLE"
        else
            ocpi_err "console log $CONSOLE not readable"
        fi
    fi
    # shellcheck disable=SC2086
    af=$(alloc_failures $files)
    metric dma_alloc_failure_lines "$af"
    ocpi_limit dma_alloc_failure_lines "$af" max 0
}

# --- preflight --------------------------------------------------------------

do_preflight() {
    ocpi_verdict_reset
    param tc "Operator TC-1 steps 1-3; Application TC-1"

    if [ -r "$FPGA_STATE_FILE" ]; then
        st=$(cat "$FPGA_STATE_FILE")
        metric fpga_state "$st"
        ocpi_thresh "fpga_state=operating"
        [ "$st" = operating ] || ocpi_fail "fpga_state=$st (want operating)"
    else
        ocpi_err "cannot read $FPGA_STATE_FILE"
    fi

    # "The driver module is currently loaded." is ocpi_linux_driver's exact
    # status text when /sys/module/opencpi exists; any other text is not
    # loaded. Matching "loaded" alone would also match "not loaded".
    if command -v ocpidriver >/dev/null 2>&1; then
        ocpidriver status >"$OUT/raw/ocpidriver-status.txt" 2>&1
        art raw/ocpidriver-status.txt
        ocpi_thresh "driver=loaded"
        if grep -q "driver module is currently loaded" "$OUT/raw/ocpidriver-status.txt"; then
            metric driver loaded
        else
            metric driver not_loaded
            ocpi_fail "OpenCPI driver not loaded"
        fi
    else
        ocpi_err "ocpidriver not on PATH (source opencpi-setup.sh first)"
    fi

    n=$(dma_stream_count)
    metric dma_stream_processes "$n"
    ocpi_limit dma_stream_processes "$n" max 0

    if [ -r "$PROC_DIR/iomem" ]; then
        if grep -Eq '^ *0+-0*fffffff : System RAM' "$PROC_DIR/iomem"; then
            metric system_ram_0_0fffffff true
        else
            metric system_ram_0_0fffffff false
            ocpi_fail "System RAM is not 0-0fffffff"
        fi
        ocpi_thresh "system_ram=0-0fffffff"
    else
        ocpi_err "cannot read $PROC_DIR/iomem"
    fi

    mt=$(awk '/^MemTotal:/ { print $2 }' "$PROC_DIR/meminfo" 2>/dev/null)
    if [ -n "$mt" ]; then
        metric memtotal_kb "$mt"
        if [ -n "$MEMTOTAL" ]; then
            ocpi_limit memtotal_kb "$mt" min "$(awk -v a="$MEMTOTAL" -v b="$MEMTOL" 'BEGIN{print a-b}')"
            ocpi_limit memtotal_kb "$mt" max "$(awk -v a="$MEMTOTAL" -v b="$MEMTOL" 'BEGIN{print a+b}')"
        else
            ocpi_unj "memtotal_kb=$mt: the specification says 'about 119 540 kB' with no tolerance"
        fi
    else
        ocpi_err "cannot read MemTotal from $PROC_DIR/meminfo"
    fi

    kernel_checks dmesg-preflight.txt
    param note "ad9361_init and the application launch are operator steps (board_stream.sh start); this check does not run them"
    finish ocpi.preflight
}

# --- postcheck --------------------------------------------------------------

do_postcheck() {
    ocpi_verdict_reset
    param tc "Operator TC-2, TC-8; Application TC-3"
    n=$(dma_stream_count)
    metric dma_stream_processes "$n"
    ocpi_limit dma_stream_processes "$n" max 0
    kernel_checks dmesg-postcheck.txt
    finish ocpi.postcheck
}

# --- analyze ----------------------------------------------------------------

# get KEY — a value from the oracle summary in $SUMMARY.
get() {
    printf '%s\n' "$SUMMARY" | sed -n "s/^$1=//p" | head -n 1
}

# require KEY... — every listed counter must have been readable on every line.
require() {
    for k in "$@"; do
        b=$(get "bad_$k")
        [ "${b:-0}" = 0 ] || ocpi_err "$k unreadable on $b line(s) (getProperty failed)"
    done
}

judge_log() {  # FILE
    ocpi_verdict_reset
    [ -r "$1" ] || { ocpi_err "log $1 not readable"; return; }
    SUMMARY=$(awk -f "$ORACLE_AWK" "$1") || { ocpi_err "could not parse $1"; return; }
    OLDIFS=$IFS; IFS='
'
    for kv in $SUMMARY; do METRICS="${METRICS}$kv
"; done
    IFS=$OLDIFS

    lines=$(get lines)
    if [ "${lines:-0}" -lt 2 ]; then
        ocpi_err "$lines counter line(s) in the log; at least 2 are needed"
        return
    fi
    if [ "$(get counter_reset)" = 1 ]; then
        ocpi_err "a counter went down: the application restarted inside this log"
        return
    fi

    case $CASE in
        opr-09|apl-06)
            if [ "$CASE" = opr-09 ]; then param tc "Operator TC-9"; else param tc "Application TC-6"; fi
            require tx_in_words rx_out_words
            ocpi_limit tx_in_words_delta "$(get tx_in_words_delta)" min 1
            ocpi_limit rx_out_words_delta "$(get rx_out_words_delta)" min 1
            ocpi_limit gap_max "$(get gap_max)" below 200000
            ocpi_limit gap_growth "$(get gap_growth)" max "$MAX_GAP_GROWTH"
            ;;
        apl-07)
            param tc "Application TC-7"
            require tx_in_words rx_out_words src
            ocpi_limit tx_words_per_s "$(get tx_words_per_s)" min 400000
            ocpi_limit rx_words_per_s "$(get rx_words_per_s)" min 400000
            # "Run past 25 Mbit": bits read from the payload source, so no
            # word size is assumed.
            ocpi_limit src_bits_delta "$(get src_bits_delta)" min 25000000
            if [ -n "$SINK" ]; then param sink "$SINK"; else ocpi_unj "the sink is not recorded (--sink)"; fi
            ;;
        apl-15)
            param tc "Application TC-15"
            require tx_in_words rx_out_words tx_underrun tx_out_dropped rx_overflow
            ocpi_limit duration_s "$(get duration_s)" min "$MIN_DURATION"
            ocpi_limit tx_words_per_s "$(get tx_words_per_s)" min "$MIN_WPS"
            ocpi_limit rx_words_per_s "$(get rx_words_per_s)" min "$MIN_WPS"
            ocpi_limit tx_underrun_delta "$(get tx_underrun_delta)" max "$MAX_LOSS"
            ocpi_limit tx_out_dropped_delta "$(get tx_out_dropped_delta)" max "$MAX_LOSS"
            ocpi_limit rx_overflow_delta "$(get rx_overflow_delta)" max "$MAX_LOSS"
            ocpi_limit gap_growth "$(get gap_growth)" max "$MAX_GAP_GROWTH"
            param interval_s 10
            ;;
        ber-01)
            param tc "BER TC-1"
            param note "src_sel=false (PRBS-23) is not visible in the log; the operator confirms it"
            require bits errs
            ocpi_limit in_sync_unreadable_lines "$(get in_sync_unreadable_lines)" max 0
            ocpi_limit in_sync_false_lines "$(get in_sync_false_lines)" max 0
            ocpi_limit errs_last "$(get errs_last)" max 0
            ocpi_limit bits_last "$(get bits_last)" min 9200000000
            ocpi_limit duration_s "$(get duration_s)" min "${MIN_DURATION:-600}"
            ;;
    esac
}

do_analyze() {
    param log "$LOG"
    cp "$LOG" "$OUT/raw/ocpi.$CASE.log" 2>/dev/null && art "raw/ocpi.$CASE.log"
    judge_log "$LOG"
    finish "ocpi.$CASE"
}

# --- watch ------------------------------------------------------------------

do_watch() {
    param log "$LOG"
    param duration_s "$DURATION"
    [ -r "$LOG" ] || { ocpi_verdict_reset; ocpi_err "log $LOG not readable (is the stream running?)"; finish "ocpi.$CASE"; }

    p0=$(dma_stream_count)
    k0=""
    if [ "$CASE" = apl-16 ] && dmesg >"$OUT/raw/dmesg-watch-start.txt" 2>/dev/null; then
        k0=$(dmesg_errors "$OUT/raw/dmesg-watch-start.txt")
    fi
    n0=$(wc -l <"$LOG")

    ocpi_log "watching $LOG for ${DURATION}s ($CASE)"
    sleep "$DURATION"

    n1=$(wc -l <"$LOG")
    part=$OUT/raw/ocpi.$CASE.log
    if [ "$n1" -lt "$n0" ]; then
        ocpi_verdict_reset
        ocpi_err "log shrank from $n0 to $n1 lines: it was restarted during the watch"
        finish "ocpi.$CASE"
    fi
    tail -n "+$((n0 + 1))" "$LOG" >"$part"
    art "raw/ocpi.$CASE.log"

    if [ "$CASE" = apl-16 ]; then
        ocpi_verdict_reset
        param tc "Application TC-16"
        art raw/dmesg-watch-start.txt
        p1=$(dma_stream_count)
        metric dma_stream_processes_start "$p0"
        metric dma_stream_processes_end "$p1"
        ocpi_limit dma_stream_processes_start "$p0" min 1
        ocpi_limit dma_stream_processes_end "$p1" min 1
        ex=$(grep -c "finished on its own" "$part")
        metric unexpected_exit_lines "$ex"
        ocpi_limit unexpected_exit_lines "$ex" max 0
        # A hang shows as counter lines that stop arriving. dma_stream prints
        # one every 10 s, so a live process yields at least duration/10 - 1.
        got=$(grep -Ec '^[0-9:]+ +(live|FINAL) +tx\.in_words=' "$part")
        metric counter_lines "$got"
        ocpi_limit counter_lines "$got" min "$((${DURATION%.*} / 10 - 1))"
        if [ -n "$k0" ] && save_dmesg dmesg-watch-end.txt; then
            k1=$(dmesg_errors "$OUT/raw/dmesg-watch-end.txt")
            metric kernel_error_lines_new "$((k1 - k0))"
            ocpi_limit kernel_error_lines_new "$((k1 - k0))" max 0
        else
            ocpi_err "dmesg could not be read"
        fi
        param dmesg_pattern "$DMESG_PATTERN"
        finish ocpi.apl-16
    fi

    judge_log "$part"
    finish "ocpi.$CASE"
}

case $CMD in
    preflight) do_preflight ;;
    postcheck) do_postcheck ;;
    analyze)   do_analyze ;;
    watch)     do_watch ;;
esac
