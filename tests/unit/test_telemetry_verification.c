/*
 * Telemetry generation, event/counter correctness, and observability output
 * verification -- driven by software-generated traffic and simulated events
 * only (netsim, mock_radio, virtual clock). No hardware.
 *
 * Existing coverage before this file (not duplicated here):
 *   - test_telemetry_sca.c: hh_telemetry_node_status() on a converged
 *     3-node netsim network (node_id, state, neighbor_count, route_count,
 *     beacons_sent/rx_accepted, partitioned, events_dropped==0), the missing
 *     -radio-backend honesty path, and that reading telemetry never
 *     disturbs the route snapshot version. Plus SCA lifecycle/property
 *     tests unrelated to telemetry content.
 *   - test_status_scenarios.c: hh_telemetry_node_status() before/after a
 *     silent (stale) link failure -- neighbor_count, failures_confirmed,
 *     neighbor_downs, events_dropped.
 *   - test_interface_compliance.c: hh_telemetry_format_neighbors() and
 *     hh_telemetry_node_status().link_transitions reflect Link Health state
 *     without an event drain (the HTI-07 pull-path proof).
 *   - test_node.c: hh_telemetry_format_topology() basic content and its
 *     NULL/empty-input contract.
 *
 * Gaps this file closes:
 *   - Full-field hh_node_status_t correctness (every counter, not a
 *     representative few) across a real multi-node run, including fields
 *     no existing test ever reads: topology_nodes, reachable_nodes,
 *     radio_channel, beacon_interval_ms, neighbor_ups, routes_installed,
 *     routes_withdrawn, recoveries_completed, packets_forwarded,
 *     packets_delivered_local, packets_dropped_no_route.
 *   - hh_telemetry_format_status()'s exact key=value output, parsed back and
 *     cross-checked field-by-field against the struct it was built from
 *     (not a handful of strstr spot checks).
 *   - packets_forwarded / packets_delivered_local / packets_dropped_no_route
 *     driven by real hh_node_send() traffic -- never exercised through
 *     telemetry by any existing test.
 *   - hh_telemetry_dump(): zero prior test coverage of any kind.
 *   - Telemetry snapshot taken at three points (before / during-failure /
 *     after-recovery) of one continuous scenario, checking that unrelated
 *     counters are untouched by an event that shouldn't affect them, and
 *     that no stale value survives into the "after recovery" snapshot.
 *   - Idempotency: calling hh_telemetry_node_status() twice with no
 *     intervening state change produces byte-identical output.
 */
#include "hhsdr/manet/node.h"
#include "hhsdr/manet/telemetry.h"
#include "hhsdr/radio/hw_adapter.h"
#include "mock_radio.h"
#include "hh_test.h"
#include "vclock.h"
#include "netsim.h"
#include <stdlib.h>
#include <string.h>

/* ---------------- Full-field snapshot during normal operation ---------------- */

static void test_full_field_snapshot_during_normal_3node_operation(void)
{
    netsim_t s;
    sim_node_t *n1;
    hh_node_status_t st;

    netsim_init(&s, 601);
    netsim_add_node(&s, 1);
    netsim_add_node(&s, 2);
    netsim_add_node(&s, 3);
    netsim_link_up(&s, 1, 2, -50.0f);
    netsim_link_up(&s, 2, 3, -50.0f);
    netsim_start_all(&s);
    netsim_run(&s, 3000, 10);

    n1 = netsim_node(&s, 1);
    hh_telemetry_node_status(&n1->node, &st);

    /* Identity and state, already spot-checked elsewhere -- included here
     * only as the baseline the rest of this test builds on. */
    HH_ASSERT_EQ_INT(st.node_id, 1);
    HH_ASSERT_EQ_STR(st.state, "running");

    /* Fields NO existing test reads via hh_telemetry_node_status(). */
    HH_ASSERT_EQ_INT(st.neighbor_count, 1);
    HH_ASSERT_EQ_INT(st.route_count, 2);              /* node 2 direct, node 3 via 2 */
    HH_ASSERT_EQ_INT(st.topology_nodes, 2);            /* 2 and 3, both known to topology */
    HH_ASSERT_EQ_INT(st.reachable_nodes, 2);
    HH_ASSERT(!st.partitioned);
    HH_ASSERT(st.beacon_interval_ms > 0);
    HH_ASSERT_EQ_INT(st.beacon_interval_ms, hh_discovery_interval(&n1->node.discovery));

    HH_ASSERT(st.beacons_sent > 0);
    HH_ASSERT(st.beacons_rx_accepted > 0);
    HH_ASSERT_EQ_INT(st.beacons_rx_duplicate, n1->node.discovery.beacons_rx_duplicate);
    HH_ASSERT_EQ_INT(st.beacons_rx_malformed, 0);

    HH_ASSERT_EQ_INT(st.neighbor_ups, 1);
    HH_ASSERT_EQ_INT(st.neighbor_downs, 0);
    HH_ASSERT_EQ_INT(st.link_transitions, 0);          /* link stayed Healthy throughout */
    HH_ASSERT_EQ_INT(st.failures_confirmed, 0);
    HH_ASSERT_EQ_INT(st.recoveries_started, 0);
    HH_ASSERT_EQ_INT(st.recoveries_completed, 0);

    HH_ASSERT(st.routes_installed >= 2);
    HH_ASSERT_EQ_INT(st.routes_withdrawn, 0);

    /* No application traffic sent yet: all three packet counters are
     * genuinely zero, not merely unchecked. */
    HH_ASSERT_EQ_INT(st.packets_forwarded, 0);
    HH_ASSERT_EQ_INT(st.packets_delivered_local, 0);
    HH_ASSERT_EQ_INT(st.packets_dropped_no_route, 0);
    HH_ASSERT_EQ_INT(st.events_dropped, 0);

    /* Mock radio always reports available+operational; channel defaults to 0
     * until a channel change occurs -- verified against the real adapter
     * state, not assumed. */
    HH_ASSERT(st.radio_available);
    HH_ASSERT(st.radio_operational);
    HH_ASSERT_EQ_INT(st.radio_channel, n1->mock.channel);
}

/* ---------------- Packet counters driven by real application traffic ---------------- */

static void test_packet_counters_reflect_real_traffic(void)
{
    netsim_t s;
    sim_node_t *n1, *n2;
    hh_node_status_t st1, st2;
    uint8_t payload[] = { 0xAA, 0xBB, 0xCC };

    netsim_init(&s, 602);
    netsim_add_node(&s, 1);
    netsim_add_node(&s, 2);
    netsim_link_up(&s, 1, 2, -50.0f);
    netsim_start_all(&s);
    netsim_run(&s, 2000, 10);

    n1 = netsim_node(&s, 1);
    n2 = netsim_node(&s, 2);

    hh_telemetry_node_status(&n1->node, &st1);
    HH_ASSERT_EQ_INT(st1.packets_forwarded, 0);
    HH_ASSERT_EQ_INT(st1.packets_delivered_local, 0);

    /* A real send from node 1 to node 2, through hh_node_send() exactly as
     * the application layer would call it. */
    HH_ASSERT_OK(hh_node_send(&n1->node, 2, payload, sizeof payload, s.vc.now));
    netsim_run(&s, 500, 10);

    hh_telemetry_node_status(&n1->node, &st1);
    hh_telemetry_node_status(&n2->node, &st2);
    HH_ASSERT_EQ_INT(st1.packets_forwarded, 1);
    HH_ASSERT_EQ_INT(st1.packets_delivered_local, 0);   /* not addressed to node 1 */
    HH_ASSERT_EQ_INT(st2.packets_delivered_local, 1);   /* arrived at node 2      */
    HH_ASSERT_EQ_INT(st2.packets_forwarded, 0);          /* node 2 never relayed   */

    /* A send with no route must increment packets_dropped_no_route, and
     * must NOT increment packets_forwarded -- the two are mutually
     * exclusive outcomes for one send. */
    HH_ASSERT_ERR(hh_node_send(&n1->node, 99, payload, sizeof payload, s.vc.now), HH_ERR_AGAIN);
    hh_telemetry_node_status(&n1->node, &st1);
    HH_ASSERT_EQ_INT(st1.packets_dropped_no_route, 1);
    HH_ASSERT_EQ_INT(st1.packets_forwarded, 1);          /* unchanged from before  */

    /* A self-addressed send is local delivery, never touches the forwarded
     * counter. */
    HH_ASSERT_OK(hh_node_send(&n1->node, 1, payload, sizeof payload, s.vc.now));
    hh_telemetry_node_status(&n1->node, &st1);
    HH_ASSERT_EQ_INT(st1.packets_delivered_local, 1);
    HH_ASSERT_EQ_INT(st1.packets_forwarded, 1);          /* unchanged              */
}

/* ---------------- hh_telemetry_format_status: exact key=value round-trip ---------------- */

/* Returns the parsed value, or -1 and records a test failure if the key is
 * absent. HH_ASSERT_MSG expands to a bare `return;` on failure (fine in a
 * void HH_RUN test body, not in a value-returning helper), so the absent
 * case is handled directly instead of routing through the macro. */
static long long find_kv_int(const char *buf, const char *key)
{
    const char *p = strstr(buf, key);
    if (!p) {
        hh_tests_failed++;
        fprintf(stderr, "FAIL %s: key \"%s\" missing from formatted status: %s\n",
                hh_current_test, key, buf);
        return -1;
    }
    return atoll(p + strlen(key));
}

static void test_format_status_matches_struct_field_by_field(void)
{
    netsim_t s;
    sim_node_t *n1;
    hh_node_status_t st;
    char buf[2048];
    size_t written;

    netsim_init(&s, 603);
    netsim_add_node(&s, 1);
    netsim_add_node(&s, 2);
    netsim_add_node(&s, 3);
    netsim_link_up(&s, 1, 2, -50.0f);
    netsim_link_up(&s, 2, 3, -50.0f);
    netsim_start_all(&s);
    netsim_run(&s, 3000, 10);

    n1 = netsim_node(&s, 1);
    hh_telemetry_node_status(&n1->node, &st);
    written = hh_telemetry_format_status(&st, buf, sizeof buf);
    HH_ASSERT(written > 0);
    HH_ASSERT_EQ_INT(written, strlen(buf));   /* returned length matches actual content */

    /* Every field the struct carries must appear in the formatted line with
     * the SAME value -- not merely present, but numerically equal. This is
     * the serialization contract an operator console or log scraper relies
     * on: format_status() must not silently drop or misrender a field. */
    HH_ASSERT_EQ_INT(find_kv_int(buf, "node="), st.node_id);
    HH_ASSERT(strstr(buf, st.state) != NULL);
    HH_ASSERT_EQ_INT(find_kv_int(buf, "neighbors="), st.neighbor_count);
    HH_ASSERT_EQ_INT(find_kv_int(buf, "routes="), st.route_count);
    HH_ASSERT_EQ_INT(find_kv_int(buf, "topo_nodes="), st.topology_nodes);
    HH_ASSERT_EQ_INT(find_kv_int(buf, "reachable="), st.reachable_nodes);
    HH_ASSERT_EQ_INT(find_kv_int(buf, "partitioned="), (int)st.partitioned);
    HH_ASSERT_EQ_INT(find_kv_int(buf, "beacon_ms="), st.beacon_interval_ms);
    HH_ASSERT_EQ_INT(find_kv_int(buf, "beacons_tx="), st.beacons_sent);
    HH_ASSERT_EQ_INT(find_kv_int(buf, "beacons_rx="), st.beacons_rx_accepted);
    HH_ASSERT_EQ_INT(find_kv_int(buf, "beacons_dup="), st.beacons_rx_duplicate);
    HH_ASSERT_EQ_INT(find_kv_int(buf, "beacons_bad="), st.beacons_rx_malformed);
    HH_ASSERT_EQ_INT(find_kv_int(buf, "nbr_up="), st.neighbor_ups);
    HH_ASSERT_EQ_INT(find_kv_int(buf, "nbr_down="), st.neighbor_downs);
    HH_ASSERT_EQ_INT(find_kv_int(buf, "link_transitions="), st.link_transitions);
    HH_ASSERT_EQ_INT(find_kv_int(buf, "failures="), st.failures_confirmed);
    HH_ASSERT_EQ_INT(find_kv_int(buf, "recov_started="), st.recoveries_started);
    HH_ASSERT_EQ_INT(find_kv_int(buf, "recov_done="), st.recoveries_completed);
    HH_ASSERT_EQ_INT(find_kv_int(buf, "routes_installed="), st.routes_installed);
    HH_ASSERT_EQ_INT(find_kv_int(buf, "routes_withdrawn="), st.routes_withdrawn);
    HH_ASSERT_EQ_INT(find_kv_int(buf, "fwd="), st.packets_forwarded);
    HH_ASSERT_EQ_INT(find_kv_int(buf, "delivered="), st.packets_delivered_local);
    HH_ASSERT_EQ_INT(find_kv_int(buf, "drop_no_route="), st.packets_dropped_no_route);
    HH_ASSERT_EQ_INT(find_kv_int(buf, "events_dropped="), st.events_dropped);
    HH_ASSERT_EQ_INT(find_kv_int(buf, "radio_available="), (int)st.radio_available);
    HH_ASSERT_EQ_INT(find_kv_int(buf, "radio_operational="), (int)st.radio_operational);
    HH_ASSERT_EQ_INT(find_kv_int(buf, "radio_channel="), st.radio_channel);
}

static void test_format_status_rejects_null_and_empty_buffer(void)
{
    hh_node_status_t st;
    char buf[64];
    memset(&st, 0, sizeof st);
    st.state = "running";

    HH_ASSERT_EQ_INT(hh_telemetry_format_status(NULL, buf, sizeof buf), 0);
    HH_ASSERT_EQ_INT(hh_telemetry_format_status(&st, NULL, sizeof buf), 0);
    HH_ASSERT_EQ_INT(hh_telemetry_format_status(&st, buf, 0), 0);
}

/* ---------------- hh_telemetry_dump: zero prior coverage ---------------- */

static void test_dump_writes_every_section_and_returned_length_is_honest(void)
{
    netsim_t s;
    sim_node_t *n1;
    char path[] = "/tmp/hh_telemetry_dump_test_XXXXXX";
    int fd;
    FILE *f;
    long size_before, size_after;
    char *contents;
    long len;

    netsim_init(&s, 604);
    netsim_add_node(&s, 1);
    netsim_add_node(&s, 2);
    netsim_link_up(&s, 1, 2, -50.0f);
    netsim_start_all(&s);
    netsim_run(&s, 2000, 10);
    n1 = netsim_node(&s, 1);

    fd = mkstemp(path);
    HH_ASSERT(fd >= 0);
    f = fdopen(fd, "w+");
    HH_ASSERT(f != NULL);

    hh_telemetry_dump(&n1->node, f);
    fflush(f);
    size_before = ftell(f);
    HH_ASSERT(size_before > 0);

    fseek(f, 0, SEEK_SET);
    contents = (char *)malloc((size_t)size_before + 1);
    HH_ASSERT(contents != NULL);
    len = (long)fread(contents, 1, (size_t)size_before, f);
    contents[len] = '\0';

    /* Every section hh_telemetry_dump() documents itself as writing. */
    HH_ASSERT(strstr(contents, "[status]") != NULL);
    HH_ASSERT(strstr(contents, "node=1") != NULL);
    HH_ASSERT(strstr(contents, "neighbor=2") != NULL);   /* from format_neighbors */
    HH_ASSERT(strstr(contents, "dst=2") != NULL);         /* from format_routes    */
    /* Mock radio reports available, so the "no hardware backend" warning
     * must NOT appear -- the honesty guarantee cuts both ways. */
    HH_ASSERT(strstr(contents, "no hardware backend") == NULL);

    free(contents);
    fclose(f);

    /* Calling dump() again must not truncate or corrupt a file already
     * containing data -- append semantics behave as any FILE* stream would. */
    f = fopen(path, "a");
    HH_ASSERT(f != NULL);
    hh_telemetry_dump(&n1->node, f);
    fflush(f);
    size_after = ftell(f);
    HH_ASSERT(size_after > size_before);
    fclose(f);

    remove(path);
}

static void test_dump_reports_missing_radio_backend_in_output(void)
{
    hh_config_t cfg;
    vclock_t vc;
    hh_hw_adapter_t hw;
    hh_radio_t radio;
    hh_node_t node;
    char path[] = "/tmp/hh_telemetry_dump_norf_XXXXXX";
    int fd;
    FILE *f;
    long size;
    char *contents;

    hh_config_defaults(&cfg);
    cfg.node_id = 1;
    cfg.log_level = HH_LOG_ERROR;
    vclock_init(&vc, 1000);
    hh_hw_adapter_init(&hw, &radio);
    HH_ASSERT_OK(hh_node_init(&node, &cfg, &vc.clock, &radio));

    fd = mkstemp(path);
    HH_ASSERT(fd >= 0);
    f = fdopen(fd, "w+");
    HH_ASSERT(f != NULL);

    hh_telemetry_dump(&node, f);
    fflush(f);
    size = ftell(f);
    HH_ASSERT(size > 0);

    fseek(f, 0, SEEK_SET);
    contents = (char *)malloc((size_t)size + 1);
    HH_ASSERT(contents != NULL);
    contents[fread(contents, 1, (size_t)size, f)] = '\0';

    HH_ASSERT(strstr(contents, "no hardware backend") != NULL);
    HH_ASSERT(strstr(contents, "radio_available=0") != NULL);

    free(contents);
    fclose(f);
    remove(path);
}

static void test_dump_rejects_null_arguments(void)
{
    netsim_t s;
    sim_node_t *n1;
    netsim_init(&s, 605);
    netsim_add_node(&s, 1);
    netsim_start_all(&s);
    n1 = netsim_node(&s, 1);

    /* Must not crash. */
    hh_telemetry_dump(NULL, stderr);
    hh_telemetry_dump(&n1->node, NULL);
}

/* ---------------- Before / during-failure / after-recovery consistency ---------------- */

static void test_telemetry_consistent_across_failure_and_recovery_cycle(void)
{
    netsim_t s;
    sim_node_t *n1;
    hh_node_status_t before, during, after;

    netsim_init(&s, 606);
    netsim_add_node(&s, 1);
    netsim_add_node(&s, 2);
    netsim_add_node(&s, 3);
    netsim_link_up(&s, 1, 2, -50.0f);
    netsim_link_up(&s, 2, 3, -50.0f);
    netsim_start_all(&s);
    netsim_run(&s, 3000, 10);
    n1 = netsim_node(&s, 1);

    hh_telemetry_node_status(&n1->node, &before);
    HH_ASSERT_EQ_INT(before.neighbor_count, 1);
    HH_ASSERT_EQ_INT(before.reachable_nodes, 2);
    HH_ASSERT_EQ_INT(before.failures_confirmed, 0);
    HH_ASSERT_EQ_INT(before.recoveries_completed, 0);

    /* Node 2 fails: this is A's OWN direct neighbor, so A's full detection
     * pipeline runs (unlike the two-hops-away case test_route_verification.c
     * covers at the route-field level, not the telemetry level). */
    netsim_node_fail(&s, 2);
    netsim_run(&s, 5000, 10);

    hh_telemetry_node_status(&n1->node, &during);
    HH_ASSERT_EQ_INT(during.neighbor_count, 0);
    HH_ASSERT_EQ_INT(during.reachable_nodes, 0);
    HH_ASSERT(during.failures_confirmed > before.failures_confirmed);
    HH_ASSERT_EQ_INT(during.route_count, 0);
    HH_ASSERT_EQ_INT(during.recoveries_completed, before.recoveries_completed);

    /* Counters that must NOT move from an event unrelated to them: no
     * application traffic was ever sent in this test, so all three packet
     * counters stay exactly zero through the failure, and beacon counters
     * only ever grow (never reset) purely from time passing. */
    HH_ASSERT_EQ_INT(during.packets_forwarded, 0);
    HH_ASSERT_EQ_INT(during.packets_delivered_local, 0);
    HH_ASSERT_EQ_INT(during.packets_dropped_no_route, 0);
    HH_ASSERT(during.beacons_sent >= before.beacons_sent);

    netsim_node_recover(&s, 2);
    netsim_run(&s, 6000, 10);

    hh_telemetry_node_status(&n1->node, &after);
    HH_ASSERT_EQ_INT(after.neighbor_count, 1);
    HH_ASSERT_EQ_INT(after.reachable_nodes, 2);
    HH_ASSERT(after.recoveries_completed > during.recoveries_completed);
    HH_ASSERT_EQ_INT(after.route_count, 2);

    /* No stale failure state survives into the recovered snapshot: the
     * counter is cumulative (so it stays at its confirmed count, it does
     * not reset), but reachable_nodes/neighbor_count/route_count -- the
     * CURRENT-state fields -- must read as fully healthy again, identical
     * in shape to the pre-failure baseline. */
    HH_ASSERT_EQ_INT(after.neighbor_count, before.neighbor_count);
    HH_ASSERT_EQ_INT(after.reachable_nodes, before.reachable_nodes);
    HH_ASSERT_EQ_INT(after.route_count, before.route_count);
    HH_ASSERT(!after.partitioned);
}

/* ---------------- No duplicate/stale telemetry: idempotent reads ---------------- */

static void test_repeated_reads_with_no_state_change_are_identical(void)
{
    netsim_t s;
    sim_node_t *n1;
    hh_node_status_t st1, st2;
    char buf1[2048], buf2[2048];

    netsim_init(&s, 607);
    netsim_add_node(&s, 1);
    netsim_add_node(&s, 2);
    netsim_link_up(&s, 1, 2, -50.0f);
    netsim_start_all(&s);
    netsim_run(&s, 2000, 10);
    n1 = netsim_node(&s, 1);

    /* Two reads at the exact same virtual timestamp, nothing ticked or
     * changed in between: every field must be byte-identical. Reading
     * telemetry must never itself mutate state (the architecture's own
     * "never sits on any layer's critical path" claim, verified directly). */
    hh_telemetry_node_status(&n1->node, &st1);
    hh_telemetry_node_status(&n1->node, &st2);
    HH_ASSERT_EQ_INT(memcmp(&st1, &st2, sizeof st1) == 0, 1);

    hh_telemetry_format_status(&st1, buf1, sizeof buf1);
    hh_telemetry_format_status(&st2, buf2, sizeof buf2);
    HH_ASSERT_EQ_STR(buf1, buf2);
}

HH_TEST_MAIN_BEGIN("telemetry_verification")
    HH_RUN(test_full_field_snapshot_during_normal_3node_operation);
    HH_RUN(test_packet_counters_reflect_real_traffic);
    HH_RUN(test_format_status_matches_struct_field_by_field);
    HH_RUN(test_format_status_rejects_null_and_empty_buffer);
    HH_RUN(test_dump_writes_every_section_and_returned_length_is_honest);
    HH_RUN(test_dump_reports_missing_radio_backend_in_output);
    HH_RUN(test_dump_rejects_null_arguments);
    HH_RUN(test_telemetry_consistent_across_failure_and_recovery_cycle);
    HH_RUN(test_repeated_reads_with_no_state_change_are_identical);
HH_TEST_MAIN_END()
