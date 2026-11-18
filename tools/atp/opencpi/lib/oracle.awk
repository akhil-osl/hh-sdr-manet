# oracle.awk — summarise dma_stream counter lines ("oracle lines").
#
# dma_stream (OpenCPI project, applications/dma_stream/dma_stream.cc) prints
# one line every 10 s while running, and one at exit:
#
#   HH:MM:SS live  tx.in_words=N tx.underrun=N tx.out_dropped=N | rx.out_words=N
#     rx.overflow=N in_sync=B bits=N errs=N | src=N B sink=N B
#
# (one line in the log; wrapped here). The tag is "live" or "FINAL". A value
# that dma_stream could not read through getProperty is printed as "?".
# Every other line in the log (DSP applies, start-up text) is ignored.
#
# Output: key=value lines, consumed by atp-ocpi.sh. Nothing is judged here;
# this file only measures, so it can be tested against fixed logs.
#
# Counters are cumulative from application start, so every figure is a delta
# between the first and the last line. A counter that goes DOWN between two
# lines means the application restarted inside the log; the deltas would then
# be meaningless, so it is reported as counter_reset=1 and the caller treats
# the whole log as an error, not as a measurement.
#
# Timestamps are local time of day with no date (the board has no RTC). The
# elapsed time is summed step by step, adding a day when a step goes
# backwards, which handles one midnight between two lines 10 s apart.

function secs(t,   a) {
    if (split(t, a, ":") != 3) return -1
    return a[1] * 3600 + a[2] * 60 + a[3]
}

# Fields whose value is a plain unsigned integer counter.
BEGIN {
    nkeys = split("tx.in_words tx.underrun tx.out_dropped rx.out_words rx.overflow bits errs src sink", K, " ")
    lines = 0; elapsed = 0; reset = 0; sync_false = 0; sync_bad = 0
    gap_max = ""; gap_first = ""; gap_last = ""
}

/^[0-9][0-9]:[0-9][0-9]:[0-9][0-9] +(live|FINAL) +tx\.in_words=/ {
    delete v
    for (i = 3; i <= NF; i++) {
        eq = index($i, "=")
        if (eq > 0) v[substr($i, 1, eq - 1)] = substr($i, eq + 1)
    }
    t = secs($1)
    if (lines > 0) {
        step = t - prev_t
        if (step < 0) step += 86400
        elapsed += step
    }
    prev_t = t
    lines++
    if ($2 == "FINAL") final_seen = 1

    for (k = 1; k <= nkeys; k++) {
        key = K[k]
        x = (key in v) ? v[key] : "?"
        if (x !~ /^[0-9]+$/) { bad[key]++; continue }
        x = x + 0
        if (!(key in first)) first[key] = x
        if ((key in last) && x < last[key]) reset = 1
        last[key] = x
    }

    s = (("in_sync" in v) ? v["in_sync"] : "?")
    if (s == "true") last_sync = "true"
    else if (s == "false") { last_sync = "false"; sync_false++ }
    else { last_sync = "unknown"; sync_bad++ }

    if (v["tx.in_words"] ~ /^[0-9]+$/ && v["rx.out_words"] ~ /^[0-9]+$/) {
        gap = v["tx.in_words"] - v["rx.out_words"]
        if (gap_first == "") gap_first = gap
        gap_last = gap
        if (gap_max == "" || gap > gap_max) gap_max = gap
    }
}

# Counter names as they appear in metrics: dots become underscores.
function mkey(key,   m) { m = key; gsub(/\./, "_", m); return m }

END {
    print "lines=" lines
    print "final_seen=" (final_seen ? "true" : "false")
    print "duration_s=" elapsed
    print "counter_reset=" reset
    for (k = 1; k <= nkeys; k++) {
        key = K[k]; m = mkey(key)
        print "bad_" m "=" (bad[key] + 0)
        if (key in first) {
            printf "%s_first=%.0f\n%s_last=%.0f\n%s_delta=%.0f\n", m, first[key], m, last[key], m, last[key] - first[key]
        }
    }
    print "in_sync_false_lines=" sync_false
    print "in_sync_unreadable_lines=" sync_bad
    if (lines > 0) print "in_sync_last=" last_sync
    if (gap_first != "") {
        printf "gap_first=%.0f\ngap_last=%.0f\ngap_max=%.0f\ngap_growth=%.0f\n", gap_first, gap_last, gap_max, gap_last - gap_first
    }
    if (elapsed > 0) {
        if ("tx.in_words" in first)  printf "tx_words_per_s=%.0f\n", (last["tx.in_words"] - first["tx.in_words"]) / elapsed
        if ("rx.out_words" in first) printf "rx_words_per_s=%.0f\n", (last["rx.out_words"] - first["rx.out_words"]) / elapsed
    }
    if ("src" in first) printf "src_bits_delta=%.0f\n", (last["src"] - first["src"]) * 8
    # BER over the whole run so far, as dma_stream's own "ber" view reports it:
    # raw, and divided by 3 because the PRBS-23 checker counts one channel
    # error up to 3 times (board_stream.sh do_ber). With zero errors the
    # rule-of-3 figure is the 95 % upper bound.
    if (("bits" in last) && ("errs" in last) && last["bits"] > 0) {
        printf "ber_raw=%.3e\n", last["errs"] / last["bits"]
        printf "ber_corrected=%.3e\n", last["errs"] / 3 / last["bits"]
        if (last["errs"] == 0) printf "ber_upper_95=%.3e\n", 3 / last["bits"]
    }
}
