# shellcheck shell=sh
#
# ocpi.sh — shared helpers for atp-ocpi.sh. Sourced, never executed.
#
# These scripts run ON THE RADIO BOARD (Xilinx 2019.2 rootfs, BusyBox). That
# image has no jq and no timeout (CONTEXT.md §7), so unlike tools/atp/lib/atp.sh
# this library builds evidence records with awk. The record is the same
# schema, "hh-atp-evidence/1", so the two families of evidence can be read by
# the same consumer.
#
# Every string goes into awk through ENVIRON, never through `awk -v`: -v
# interprets backslash escapes, so a reason containing "\n" would be silently
# rewritten before it reached the JSON escaper.

ATP_SCHEMA="hh-atp-evidence/1"

ocpi_die() {
    printf '%s: %s\n' "${ATP_PROG:-atp-ocpi}" "$*" >&2
    exit 2
}

ocpi_log() {
    printf '%s: %s\n' "${ATP_PROG:-atp-ocpi}" "$*" >&2
}

ocpi_now_utc() {
    date -u +%Y-%m-%dT%H:%M:%SZ
}

# ocpi_is_number VALUE — plain decimal, optionally signed and fractional.
ocpi_is_number() {
    printf '%s' "$1" | grep -Eq '^-?[0-9]+(\.[0-9]+)?$'
}

# ocpi_violates VALUE OP LIMIT — true when VALUE violates LIMIT.
#   min:   VALUE <  LIMIT   ("at least")
#   max:   VALUE >  LIMIT   ("at most")
#   below: VALUE >= LIMIT   ("below", as in "below 200 000")
# awk, because sh arithmetic is integer-only and 32-bit on some shells, and
# bit counts here exceed 2^32.
ocpi_violates() {
    awk -v v="$1" -v op="$2" -v l="$3" 'BEGIN {
        if (op == "min")   exit !(v + 0 <  l + 0);
        if (op == "max")   exit !(v + 0 >  l + 0);
        if (op == "below") exit !(v + 0 >= l + 0);
        exit 1
    }'
}

# ocpi_record OUT CHECK RESULT REASON STARTED PARAMS METRICS THRESHOLDS ARTIFACTS
#
# PARAMS, METRICS and THRESHOLDS are newline-separated key=value lists; each
# becomes a flat JSON object. A value that is a decimal number becomes a JSON
# number, true/false become booleans, null becomes null, anything else a
# string. ARTIFACTS is a newline-separated list of paths. Writes OUT and echoes
# it to stdout, like atp_record.
ocpi_record() {
    OCPI_R_CHECK=$2 OCPI_R_RESULT=$3 OCPI_R_REASON=$4 OCPI_R_STARTED=$5 \
    OCPI_R_PARAMS=$6 OCPI_R_METRICS=$7 OCPI_R_THRESH=$8 OCPI_R_ART=$9 \
    OCPI_R_FINISHED=$(ocpi_now_utc) OCPI_R_HOST=$(uname -n) \
    OCPI_R_SCHEMA=$ATP_SCHEMA \
    awk 'function esc(s,   i, c, out) {
            gsub(/\\/, "\\\\", s); gsub(/"/, "\\\"", s)
            gsub(/\t/, "\\t", s); gsub(/\r/, "\\r", s); gsub(/\n/, "\\n", s)
            out = ""
            for (i = 1; i <= length(s); i++) {
                c = substr(s, i, 1)
                if (c < " ") c = sprintf("\\u%04x", ctl[c])
                out = out c
            }
            return out
        }
        function val(v) {
            if (v ~ /^-?[0-9]+(\.[0-9]+)?([eE][-+]?[0-9]+)?$/) return v
            if (v == "true" || v == "false" || v == "null") return v
            return "\"" esc(v) "\""
        }
        function obj(list,   n, i, a, k, eq, out, sep) {
            n = split(list, a, "\n"); out = "{"; sep = ""
            for (i = 1; i <= n; i++) {
                if (a[i] == "") continue
                eq = index(a[i], "=")
                if (eq == 0) continue
                k = substr(a[i], 1, eq - 1)
                out = out sep "\"" esc(k) "\": " val(substr(a[i], eq + 1))
                sep = ", "
            }
            return out "}"
        }
        function arr(list,   n, i, a, out, sep) {
            n = split(list, a, "\n"); out = "["; sep = ""
            for (i = 1; i <= n; i++) {
                if (a[i] == "") continue
                out = out sep "\"" esc(a[i]) "\""; sep = ", "
            }
            return out "]"
        }
        BEGIN {
            for (i = 1; i < 32; i++) ctl[sprintf("%c", i)] = i
            printf "{\"schema\": \"%s\", \"check\": \"%s\", \"result\": \"%s\", ",
                esc(ENVIRON["OCPI_R_SCHEMA"]), esc(ENVIRON["OCPI_R_CHECK"]),
                esc(ENVIRON["OCPI_R_RESULT"])
            printf "\"reason\": \"%s\", \"started_utc\": \"%s\", \"finished_utc\": \"%s\", ",
                esc(ENVIRON["OCPI_R_REASON"]), esc(ENVIRON["OCPI_R_STARTED"]),
                esc(ENVIRON["OCPI_R_FINISHED"])
            printf "\"time_source\": \"system\", \"host\": \"%s\", ", esc(ENVIRON["OCPI_R_HOST"])
            printf "\"params\": %s, \"metrics\": %s, \"thresholds\": %s, \"artifacts\": %s}\n",
                obj(ENVIRON["OCPI_R_PARAMS"]), obj(ENVIRON["OCPI_R_METRICS"]),
                obj(ENVIRON["OCPI_R_THRESH"]), arr(ENVIRON["OCPI_R_ART"])
        }' >"$1" || ocpi_die "could not write $1"
    cat "$1"
}

# ocpi_exit_for RESULT — same exit codes as tools/atp (README "Results").
ocpi_exit_for() {
    case $1 in
        pass)     return 0 ;;
        fail)     return 1 ;;
        error)    return 3 ;;
        blocked)  return 4 ;;
        unjudged) return 5 ;;
        *)        return 3 ;;
    esac
}

# --- verdict accumulator ----------------------------------------------------
#
# A check is a list of criteria. Each criterion adds to one of three lists;
# the result is the worst of them: any error -> error, else any fail -> fail,
# else any unjudged -> unjudged, else pass. "unjudged" is used when the test
# specification names a criterion but gives no number for it (for example
# "not growing"), so the script measures it and refuses to call it good.

ocpi_verdict_reset() {
    V_ERR="" V_FAIL="" V_UNJ="" V_THRESH=""
}
ocpi_err()   { V_ERR="${V_ERR:+$V_ERR; }$*"; }
ocpi_fail()  { V_FAIL="${V_FAIL:+$V_FAIL; }$*"; }
ocpi_unj()   { V_UNJ="${V_UNJ:+$V_UNJ; }$*"; }
ocpi_thresh() { V_THRESH="${V_THRESH}$1
"; }

# ocpi_limit NAME VALUE OP LIMIT — judge one numeric criterion and record the
# limit. An empty LIMIT means the specification gives none: unjudged.
ocpi_limit() {
    if [ -z "$4" ]; then
        ocpi_unj "$1=$2 has no limit"
        return
    fi
    ocpi_thresh "$3_$1=$4"
    if ! ocpi_is_number "$2"; then
        ocpi_err "$1 not measured"
    elif ocpi_violates "$2" "$3" "$4"; then
        ocpi_fail "$1=$2 ($3 $4)"
    fi
}

# ocpi_verdict — sets RESULT and REASON from the accumulated criteria.
ocpi_verdict() {
    if [ -n "$V_ERR" ]; then
        RESULT=error REASON=$V_ERR
    elif [ -n "$V_FAIL" ]; then
        RESULT=fail REASON="criterion violated: $V_FAIL"
    elif [ -n "$V_UNJ" ]; then
        RESULT=unjudged REASON="measured, not judged: $V_UNJ"
    else
        RESULT=pass REASON="all criteria met"
    fi
}
