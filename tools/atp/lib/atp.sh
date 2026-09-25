# shellcheck shell=sh
#
# atp.sh — shared helpers for the ATP/BIT scripts. Sourced, never executed.
#
# POSIX sh on purpose: these scripts must run on the PetaLinux target, where
# bash is not guaranteed. The only non-POSIX tool this library needs is jq,
# used to build evidence records — hand-escaping JSON in shell is the kind of
# thing that is correct until a device name or an error string contains a
# quote.
#
# Evidence record (schema "hh-atp-evidence/1"), one JSON object per check:
#
#   schema      "hh-atp-evidence/1"
#   check       dotted check name, e.g. "iperf3.tcp"
#   result      pass | fail | blocked | error | unjudged
#   reason      one line explaining the result
#   started_utc / finished_utc   ISO-8601, from the host system clock
#   time_source "system" — NOT the radio PHC: the time plane does not exist
#               yet (unknown.md U-07), so evidence must not imply PHC time
#   host        uname -n of the machine that ran the check
#   params      what was asked for (peer, duration, rate, ...)
#   metrics     what was measured
#   thresholds  the limits applied; empty object when none were given
#   artifacts   paths of raw tool output; relative to the evidence dir when
#               the script wrote them, as given when the caller supplied them
#
# Result semantics — the part that matters for acceptance:
#   pass      measured, and inside every threshold given
#   fail      measured, and outside at least one threshold
#   unjudged  measured, but no threshold was given. NOT a pass: ATP limits are
#             owned by the ATP document, and a script must not invent them
#   blocked   could not be measured because a capability does not exist yet;
#             reason cites the unknown.md entry. NOT a pass
#   error     the check itself broke (tool missing, peer unreachable, ...)

ATP_SCHEMA="hh-atp-evidence/1"

atp_die() {
    printf '%s: %s\n' "${ATP_PROG:-atp}" "$*" >&2
    exit 2
}

atp_log() {
    printf '%s: %s\n' "${ATP_PROG:-atp}" "$*" >&2
}

atp_now_utc() {
    date -u +%Y-%m-%dT%H:%M:%SZ
}

# atp_have TOOL — true when TOOL is on PATH.
atp_have() {
    command -v "$1" >/dev/null 2>&1
}

# jq is needed to write any record at all, so its absence cannot itself be
# reported as a record. Fail loudly instead.
atp_require_jq() {
    atp_have jq || atp_die "jq is required to write evidence records"
}

# atp_is_number VALUE — true for a plain decimal number (optionally signed,
# optionally fractional). Used to reject garbage thresholds up front rather
# than letting awk silently treat them as 0.
atp_is_number() {
    printf '%s' "$1" | grep -Eq '^-?[0-9]+(\.[0-9]+)?$'
}

# atp_exceeds VALUE OP LIMIT — true when VALUE violates LIMIT.
#   OP "min": violated when VALUE <  LIMIT
#   OP "max": violated when VALUE >  LIMIT
# Floating-point compare in awk, since sh arithmetic is integer-only.
atp_exceeds() {
    awk -v v="$1" -v op="$2" -v l="$3" 'BEGIN {
        if (op == "min") exit !(v + 0 <  l + 0);
        if (op == "max") exit !(v + 0 >  l + 0);
        exit 1
    }'
}

# atp_record OUTFILE CHECK RESULT REASON STARTED PARAMS METRICS THRESHOLDS ARTIFACTS
#
# PARAMS, METRICS and THRESHOLDS are JSON objects; ARTIFACTS is a JSON array.
# Writes the record to OUTFILE and echoes it to stdout, so a caller can both
# keep the evidence and pipe it onward.
atp_record() {
    _out=$1 _check=$2 _result=$3 _reason=$4 _started=$5
    _params=$6 _metrics=$7 _thresholds=$8 _artifacts=$9

    jq -n \
        --arg schema "$ATP_SCHEMA" \
        --arg check "$_check" \
        --arg result "$_result" \
        --arg reason "$_reason" \
        --arg started "$_started" \
        --arg finished "$(atp_now_utc)" \
        --arg host "$(uname -n)" \
        --argjson params "$_params" \
        --argjson metrics "$_metrics" \
        --argjson thresholds "$_thresholds" \
        --argjson artifacts "$_artifacts" \
        '{schema: $schema, check: $check, result: $result, reason: $reason,
          started_utc: $started, finished_utc: $finished,
          time_source: "system", host: $host,
          params: $params, metrics: $metrics, thresholds: $thresholds,
          artifacts: $artifacts}' >"$_out" || atp_die "could not write $_out"
    cat "$_out"
}

# atp_exit_for RESULT — process exit status for a result, so a caller (cron,
# systemd, CI) can act on the outcome without parsing JSON.
#   0 pass   1 fail   3 error   4 blocked   5 unjudged
# 2 is reserved for usage errors (atp_die), matching radioctl's convention of
# keeping "you called me wrong" distinct from "the thing under test is wrong".
atp_exit_for() {
    case $1 in
        pass)     return 0 ;;
        fail)     return 1 ;;
        error)    return 3 ;;
        blocked)  return 4 ;;
        unjudged) return 5 ;;
        *)        return 3 ;;
    esac
}
