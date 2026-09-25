#!/bin/sh
#
# atp-radioctl.sh — control-plane checks through the public radioctl CLI.
#
# The architecture requires test automation to drive the radio through
# radioctl rather than through private access, so this script uses nothing
# else: it relies only on radioctl's documented exit codes (radioctl/README.md)
# and its key=value output.
#
# Two modes, because they carry very different risk:
#
#   health     READ-ONLY. `status` and `stats`; judges that radiod answers,
#              is in the expected state and reports the radio operational.
#              Safe on a radio in service — this is the BIT check.
#
#   lifecycle  INTRUSIVE. Walks init -> configure -> start -> set-channel ->
#              inject/clear fault -> stop, plus a deliberately illegal command,
#              and checks every response. ATP only. It refuses to run unless
#              radiod is freshly started (state "created"), so it can never
#              reconfigure a radio that is already doing something.
#
# The expectations are radiod's documented behaviour (lifecycle order, ESTATE
# rejection, hw_fault -> faulted), not invented limits. Only the counter
# thresholds are caller-supplied.

set -u

ATP_PROG=atp-radioctl
ATP_DIR=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
. "$ATP_DIR/lib/atp.sh"

usage() {
    cat <<EOF
usage: $ATP_PROG health    -o DIR [options]
       $ATP_PROG lifecycle -o DIR --node-id N [options]

  -o DIR                evidence directory, created if absent (required)
  -s SOCK               radiod socket path (default: radioctl's own default)
  -R PATH               radioctl binary (default: radioctl on PATH)
  -T SECONDS            per-command deadline (default 5); radioctl has none
                        of its own, so a hung radiod would hang the check

  health:
  --expect-state S      state radiod must be in (default running)
  --max-tx-errors N     thresholds on the cumulative counters reported by
  --max-rx-errors N     'radioctl stats' (cumulative since radiod started)
  --max-rejected N

  lifecycle:
  --node-id N           node id to configure (required)
  --channel N           channel after configure (default 0)
  --set-channel N       channel for the set-channel step (default 1)

exit: 0 pass, 1 fail, 2 usage, 3 error, 5 unjudged
EOF
}

mode=${1-}
[ $# -gt 0 ] && shift
case $mode in
    health|lifecycle) ;;
    -h|--help) usage; exit 0 ;;
    *) usage >&2; atp_die "mode must be health or lifecycle" ;;
esac

outdir='' sock='' rctl=radioctl deadline=5
expect_state=running max_tx='' max_rx='' max_rej=''
node_id='' channel=0 set_channel=1

while [ $# -gt 0 ]; do
    case $1 in
        -o) outdir=${2-}; shift ;;
        -s) sock=${2-}; shift ;;
        -R) rctl=${2-}; shift ;;
        -T) deadline=${2-}; shift ;;
        --expect-state) expect_state=${2-}; shift ;;
        --max-tx-errors) max_tx=${2-}; shift ;;
        --max-rx-errors) max_rx=${2-}; shift ;;
        --max-rejected) max_rej=${2-}; shift ;;
        --node-id) node_id=${2-}; shift ;;
        --channel) channel=${2-}; shift ;;
        --set-channel) set_channel=${2-}; shift ;;
        -h|--help) usage; exit 0 ;;
        *) usage >&2; atp_die "unknown argument: $1" ;;
    esac
    shift
done

[ -n "$outdir" ] || { usage >&2; atp_die "-o EVIDENCE_DIR is required"; }
atp_is_number "$deadline" || atp_die "-T must be a number"
for t in "$max_tx" "$max_rx" "$max_rej"; do
    [ -z "$t" ] || atp_is_number "$t" || atp_die "threshold is not a number: $t"
done
if [ "$mode" = lifecycle ]; then
    atp_is_number "$node_id"     || atp_die "lifecycle needs --node-id N"
    atp_is_number "$channel"     || atp_die "--channel must be a number"
    atp_is_number "$set_channel" || atp_die "--set-channel must be a number"
fi

atp_require_jq
mkdir -p "$outdir/raw" || atp_die "cannot create $outdir/raw"

check="radioctl.$mode"
record="$outdir/$check.json"
log_rel="raw/$check.log"
log="$outdir/$log_rel"
: >"$log"
started=$(atp_now_utc)
artifacts=$(jq -n --arg l "$log_rel" '[$l]')

params=$(jq -n --arg mode "$mode" --arg sock "$sock" --arg rctl "$rctl" \
    --argjson deadline "$deadline" --arg state "$expect_state" \
    --arg node "$node_id" --arg ch "$channel" --arg sch "$set_channel" \
    'if $mode == "health"
     then {mode: $mode, socket: $sock, radioctl: $rctl, deadline_s: $deadline,
           expect_state: $state}
     else {mode: $mode, socket: $sock, radioctl: $rctl, deadline_s: $deadline,
           node_id: ($node | tonumber), channel: ($ch | tonumber),
           set_channel: ($sch | tonumber)} end')
thresholds=$(jq -n --arg a "$max_tx" --arg b "$max_rx" --arg c "$max_rej" \
    '{max_tx_errors: $a, max_rx_errors: $b, max_rejected: $c}
     | with_entries(select(.value != "") | .value |= tonumber)')

if ! atp_have "$rctl" && [ ! -x "$rctl" ]; then
    atp_record "$record" "$check" error "radioctl not found: $rctl" "$started" \
        "$params" '{}' "$thresholds" "$artifacts"
    atp_exit_for error; exit $?
fi

# rc CMD [ARGS...] — run one radioctl command. Sets $rc_exit and $rc_kv (the
# key=value output as a JSON object; numbers become numbers) and appends the
# exchange to the raw log, which is the artifact an auditor reads.
rc() {
    if [ -n "$sock" ]; then set -- "$rctl" -s "$sock" "$@"; else set -- "$rctl" "$@"; fi
    if atp_have timeout; then
        rc_out=$(timeout "$deadline" "$@" 2>"$log.stderr")
    else
        rc_out=$("$@" 2>"$log.stderr")
    fi
    rc_exit=$?
    rc_err=$(head -n 1 "$log.stderr")
    {
        printf '$ %s\n' "$*"
        [ -n "$rc_out" ] && printf '%s\n' "$rc_out"
        sed 's/^/stderr: /' "$log.stderr"
        printf 'exit=%s\n\n' "$rc_exit"
    } >>"$log"
    rc_kv=$(printf '%s\n' "$rc_out" | jq -R -n '
        [inputs | select(test("^[a-z_]+="))
         | capture("^(?<k>[a-z_]+)=(?<v>.*)$")
         | {(.k): (.v | tonumber? // .)}] | add // {}')
}

# What an exit code means, per radioctl/README.md.
explain() {
    case $1 in
        0)   echo "ok" ;;
        2)   echo "usage error" ;;
        3)   echo "radiod unreachable" ;;
        4)   echo "rejected by radiod" ;;
        124) echo "no answer within ${deadline}s" ;;
        *)   echo "exit $1" ;;
    esac
}

# ---- health ---------------------------------------------------------------

if [ "$mode" = health ]; then
    rc status
    st_exit=$rc_exit st_kv=$rc_kv st_err=$rc_err
    rc stats
    sa_exit=$rc_exit sa_kv=$rc_kv sa_err=$rc_err
    metrics=$(jq -n --argjson se "$st_exit" --argjson st "$st_kv" \
        --argjson ae "$sa_exit" --argjson sa "$sa_kv" \
        '{status_exit: $se, stats_exit: $ae, status: $st, stats: $sa}')

    # radiod being unreachable or silent is a finding about the radio, not a
    # broken check: exactly the distinction radioctl keeps exits 3 and 4 for.
    problems=''
    add() { problems="$problems${problems:+; }$1"; }
    [ "$st_exit" = 0 ] || add "status: $(explain "$st_exit")${st_err:+ ($st_err)}"
    [ "$sa_exit" = 0 ] || add "stats: $(explain "$sa_exit")${sa_err:+ ($sa_err)}"
    if [ "$st_exit" = 0 ]; then
        state=$(printf '%s' "$st_kv" | jq -r '.state // empty')
        oper=$(printf '%s' "$st_kv" | jq -r '.operational // empty')
        [ "$state" = "$expect_state" ] || add "state=${state:-missing} (expected $expect_state)"
        [ "$oper" = 1 ] || add "operational=${oper:-missing}"
    fi
    if [ "$sa_exit" = 0 ]; then
        for pair in "tx_errors:$max_tx" "rx_errors:$max_rx" "requests_rejected:$max_rej"; do
            key=${pair%%:*} lim=${pair#*:}
            [ -n "$lim" ] || continue
            v=$(printf '%s' "$sa_kv" | jq -r ".$key // empty")
            if [ -z "$v" ]; then add "$key not reported"
            elif atp_exceeds "$v" max "$lim"; then add "$key=$v (max $lim)"; fi
        done
    fi

    if [ -n "$problems" ]; then
        result=fail reason=$problems
    else
        result=pass reason="radiod answers, state=$expect_state, operational"
    fi
    atp_record "$record" "$check" "$result" "$reason" "$started" \
        "$params" "$metrics" "$thresholds" "$artifacts"
    atp_exit_for "$result"; exit $?
fi

# ---- lifecycle ------------------------------------------------------------

steps='[]'
failed=''

# step NAME WANT_EXIT JQ_EXPR CMD [ARGS...] — run a command and record it as
# one step. JQ_EXPR is evaluated against the parsed output and must be true;
# pass "true" when only the exit code matters.
step() {
    s_name=$1 s_want=$2 s_expr=$3
    shift 3
    rc "$@"
    s_ok=false
    if [ "$rc_exit" = "$s_want" ] && printf '%s' "$rc_kv" | jq -e "$s_expr" >/dev/null 2>&1; then
        s_ok=true
    else
        failed="$failed${failed:+; }$s_name ($(explain "$rc_exit"), expected $(explain "$s_want"); got $(printf '%s' "$rc_kv" | jq -c .))"
    fi
    steps=$(printf '%s' "$steps" | jq --arg n "$s_name" --arg cmd "$*" \
        --argjson want "$s_want" --argjson got "$rc_exit" --arg expr "$s_expr" \
        --argjson out "$rc_kv" --argjson ok "$s_ok" \
        '. + [{step: $n, command: $cmd, expected_exit: $want, exit: $got,
               expect: $expr, output: $out, ok: $ok}]')
}

# Precondition: never disturb a radio that is already configured or running.
# radiod rejects `status` in state "created" (ESTATE, exit 4) and answers it in
# every state after init, so an answered status means the radio is in use.
# The first legal-only-from-created step (init) then confirms the state.
rc status
if [ "$rc_exit" != 4 ]; then
    if [ "$rc_exit" = 0 ]; then
        why="radiod is in state $(printf '%s' "$rc_kv" | jq -r '.state // "unknown"'), not created"
    else
        why="radiod $(explain "$rc_exit")"
    fi
    atp_record "$record" "$check" error \
        "lifecycle refused: $why; it runs only against a freshly started radiod, so it never reconfigures a radio in use" \
        "$started" "$params" "$(jq -n --argjson s "$rc_kv" '{precondition_status: $s}')" \
        "$thresholds" "$artifacts"
    atp_exit_for error; exit $?
fi

step start-before-init 4 'true'                    start
step init             0 '.state == "initialized"'  init
step configure        0 '.state == "configured"'   configure "$node_id" "$channel"
step start            0 '.state == "running"'      start
step status-running   0 '.state == "running" and .operational == 1' status
# radioctl prints only `state` for set-channel, so the new channel is read
# back with a separate status.
step set-channel      0 '.state == "running"'      set-channel "$set_channel"
step status-channel   0 ".channel == $set_channel" status
step inject-hw-fault  0 '.state == "faulted"'      inject-fault hw_fault
step clear-fault      0 '.state == "running"'      clear-fault
step stats            0 '.requests_rejected >= 1'  stats
step stop             0 '.state == "stopped"'      stop

metrics=$(jq -n --argjson steps "$steps" \
    '{steps_total: ($steps | length), steps_ok: ($steps | map(select(.ok)) | length),
      steps: $steps}')
if [ -n "$failed" ]; then
    result=fail reason="step failed: $failed"
else
    result=pass reason="all lifecycle steps behaved as documented"
fi
atp_record "$record" "$check" "$result" "$reason" "$started" \
    "$params" "$metrics" "$thresholds" "$artifacts"
atp_exit_for "$result"; exit $?
