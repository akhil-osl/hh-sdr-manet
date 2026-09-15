/*
 * Interface-contract compliance: approved-specification field/parameter
 * checks not already covered by functional verification.
 *
 * The approved interface definition is
 * HH-SDR-MANET-Network-Topology-HWSW-Interface-Specification.md (HTI-01
 * through HTI-16). Every component header in this codebase already cites
 * the specific HTI it implements (grep "HTI-" across include/hhsdr -- every
 * hit names the interface it satisfies), and the event structures in
 * hhsdr/core/events.h were cross-checked field-by-field against each HTI's
 * documented Data table during this verification pass: HTI-05/06/07/08/09/
 * 10/11/12/13/16 all match the spec's required/optional field lists exactly.
 * That functional field-correctness (producer populates what the consumer
 * reads, with the right values, under real state transitions) is already
 * proven executably by test_routing.c, test_recovery.c, test_neighbor.c,
 * test_link_health.c, and test_status_events.c -- this file does not repeat
 * any of that.
 *
 * What those tests never exercised is the INTERFACE CONTRACT itself,
 * independent of any particular scenario:
 *
 *  1. Every component's _init() documents "NULL arguments rejected" as a
 *     precondition (visible in every _init()'s own `if (!x || ...) return
 *     HH_ERR_INVAL;` guard) but no test had ever called a single one of them
 *     with a NULL argument to prove the contract holds.
 *  2. Five enum-to-string functions (hh_status_str, hh_link_state_str,
 *     hh_cause_hint_str, hh_withdraw_reason_str, hh_recovery_strategy_str)
 *     are part of the observable interface surface (they are what appears in
 *     telemetry output and log records -- an application-facing contract),
 *     each with an exhaustive switch and an "unknown" fallback, but none had
 *     ever actually been called by a test for every value of its enum. Only
 *     hh_event_type_str had this check (test_dispatcher.c).
 *  3. HTI-07's interface-summary table (spec Sec. 13) lists Routing Engine
 *     and Telemetry as "Consumers" of LinkStateChanged, but node.c's
 *     on_link_state() only subscribes the Failure Detector and Topology
 *     Manager to that event -- Routing and Telemetry instead read Link
 *     Health's state synchronously via hh_link_health_get(), a deliberate,
 *     separately-documented design (spec Sec. 9: "the Routing Engine
 *     consumes raw metrics directly... independently of the Link Health
 *     Monitor's own state-machine verdict... a delay in one never blocks the
 *     other"). No existing test proved this pull-based path actually reflects
 *     a link-health state change without any event ever being delivered to
 *     Routing/Telemetry -- this file adds exactly that proof.
 */
#include "hhsdr/manet/discovery.h"
#include "hhsdr/manet/neighbor.h"
#include "hhsdr/manet/link_health.h"
#include "hhsdr/manet/routing.h"
#include "hhsdr/manet/failure_detector.h"
#include "hhsdr/manet/topology.h"
#include "hhsdr/manet/self_healing.h"
#include "hhsdr/manet/telemetry.h"
#include "hhsdr/dataplane/forwarder.h"
#include "hhsdr/radio/radio.h"
#include "mock_radio.h"
#include "hh_test.h"
#include "vclock.h"
#include <string.h>

/* ---------------- 1. NULL-argument rejection contract for every _init() ---------------- */

static void test_every_component_init_rejects_null_arguments(void)
{
    hh_config_t cfg;
    vclock_t vc;
    hh_dispatcher_t bus;
    mock_radio_t mock;
    hh_radio_t radio;
    hh_neighbor_mgr_t nm;
    hh_link_health_t lh;
    hh_routing_t rt;
    hh_topology_t topo;
    hh_route_publisher_t pub;

    hh_discovery_t disc;
    hh_failure_detector_t fd;
    hh_self_healing_t sh;
    hh_forwarder_t fw;

    hh_config_defaults(&cfg);
    cfg.node_id = 1;
    vclock_init(&vc, 0);
    hh_dispatcher_init(&bus);
    mock_radio_init(&mock, "n", &radio);
    hh_route_publisher_init(&pub);
    /* Fully-initialized fixtures, needed as valid arguments when probing a
     * DIFFERENT parameter of each _init -- only one argument is NULLed at a
     * time, exactly as each function's own guard checks each independently. */
    hh_neighbor_init(&nm, &cfg, &vc.clock, &bus);
    hh_link_health_init(&lh, &cfg, &vc.clock, &bus);
    hh_routing_init(&rt, &cfg, &vc.clock, &bus, &nm, &lh);
    hh_topology_init(&topo, &cfg, &vc.clock, &bus);

    /* hh_discovery_init(d, cfg, clock, bus, radio) */
    HH_ASSERT_ERR(hh_discovery_init(NULL, &cfg, &vc.clock, &bus, &radio), HH_ERR_INVAL);
    HH_ASSERT_ERR(hh_discovery_init(&disc, NULL, &vc.clock, &bus, &radio), HH_ERR_INVAL);
    HH_ASSERT_ERR(hh_discovery_init(&disc, &cfg, NULL, &bus, &radio), HH_ERR_INVAL);
    HH_ASSERT_ERR(hh_discovery_init(&disc, &cfg, &vc.clock, NULL, &radio), HH_ERR_INVAL);

    /* hh_neighbor_init(nm, cfg, clock, bus) */
    HH_ASSERT_ERR(hh_neighbor_init(NULL, &cfg, &vc.clock, &bus), HH_ERR_INVAL);
    HH_ASSERT_ERR(hh_neighbor_init(&nm, NULL, &vc.clock, &bus), HH_ERR_INVAL);
    HH_ASSERT_ERR(hh_neighbor_init(&nm, &cfg, NULL, &bus), HH_ERR_INVAL);
    HH_ASSERT_ERR(hh_neighbor_init(&nm, &cfg, &vc.clock, NULL), HH_ERR_INVAL);

    /* hh_link_health_init(lh, cfg, clock, bus) */
    HH_ASSERT_ERR(hh_link_health_init(NULL, &cfg, &vc.clock, &bus), HH_ERR_INVAL);
    HH_ASSERT_ERR(hh_link_health_init(&lh, NULL, &vc.clock, &bus), HH_ERR_INVAL);
    HH_ASSERT_ERR(hh_link_health_init(&lh, &cfg, NULL, &bus), HH_ERR_INVAL);
    HH_ASSERT_ERR(hh_link_health_init(&lh, &cfg, &vc.clock, NULL), HH_ERR_INVAL);

    /* hh_routing_init(rt, cfg, clock, bus, neighbors, link_health) */
    HH_ASSERT_ERR(hh_routing_init(NULL, &cfg, &vc.clock, &bus, &nm, &lh), HH_ERR_INVAL);
    HH_ASSERT_ERR(hh_routing_init(&rt, NULL, &vc.clock, &bus, &nm, &lh), HH_ERR_INVAL);
    HH_ASSERT_ERR(hh_routing_init(&rt, &cfg, NULL, &bus, &nm, &lh), HH_ERR_INVAL);
    HH_ASSERT_ERR(hh_routing_init(&rt, &cfg, &vc.clock, NULL, &nm, &lh), HH_ERR_INVAL);
    /* neighbors/link_health are documented read-only pointers, not required
     * non-NULL by hh_routing_init's own guard (routing.c checks only
     * r/cfg/clock/bus) -- so this intentionally does NOT assert HH_ERR_INVAL
     * for a NULL neighbors/link_health, matching the actual precondition
     * rather than an invented one. */

    /* hh_fd_init(fd, cfg, clock, bus) */
    HH_ASSERT_ERR(hh_fd_init(NULL, &cfg, &vc.clock, &bus), HH_ERR_INVAL);
    HH_ASSERT_ERR(hh_fd_init(&fd, NULL, &vc.clock, &bus), HH_ERR_INVAL);
    HH_ASSERT_ERR(hh_fd_init(&fd, &cfg, NULL, &bus), HH_ERR_INVAL);
    HH_ASSERT_ERR(hh_fd_init(&fd, &cfg, &vc.clock, NULL), HH_ERR_INVAL);

    /* hh_topology_init(t, cfg, clock, bus) */
    HH_ASSERT_ERR(hh_topology_init(NULL, &cfg, &vc.clock, &bus), HH_ERR_INVAL);
    HH_ASSERT_ERR(hh_topology_init(&topo, NULL, &vc.clock, &bus), HH_ERR_INVAL);
    HH_ASSERT_ERR(hh_topology_init(&topo, &cfg, NULL, &bus), HH_ERR_INVAL);
    HH_ASSERT_ERR(hh_topology_init(&topo, &cfg, &vc.clock, NULL), HH_ERR_INVAL);

    /* hh_sh_init(sh, cfg, clock, bus, routing, topology, radio) */
    HH_ASSERT_ERR(hh_sh_init(NULL, &cfg, &vc.clock, &bus, &rt, &topo, &radio), HH_ERR_INVAL);
    HH_ASSERT_ERR(hh_sh_init(&sh, NULL, &vc.clock, &bus, &rt, &topo, &radio), HH_ERR_INVAL);
    HH_ASSERT_ERR(hh_sh_init(&sh, &cfg, NULL, &bus, &rt, &topo, &radio), HH_ERR_INVAL);
    HH_ASSERT_ERR(hh_sh_init(&sh, &cfg, &vc.clock, NULL, &rt, &topo, &radio), HH_ERR_INVAL);

    /* hh_forwarder_init(fw, cfg, radio, routes) */
    HH_ASSERT_ERR(hh_forwarder_init(NULL, &cfg, &radio, &pub), HH_ERR_INVAL);
    HH_ASSERT_ERR(hh_forwarder_init(&fw, NULL, &radio, &pub), HH_ERR_INVAL);
    HH_ASSERT_ERR(hh_forwarder_init(&fw, &cfg, NULL, &pub), HH_ERR_INVAL);
    HH_ASSERT_ERR(hh_forwarder_init(&fw, &cfg, &radio, NULL), HH_ERR_INVAL);
}

/* ---------------- 2. Enum-to-string interface completeness ---------------- */

static void test_status_str_covers_every_value_without_collapsing_to_unknown(void)
{
    const char *seen[16];
    int n = 0;
    const hh_status_t all[] = {
        HH_OK, HH_ERR_INVAL, HH_ERR_NOMEM, HH_ERR_NOTFOUND, HH_ERR_AGAIN,
        HH_ERR_STATE, HH_ERR_UNSUPPORTED, HH_ERR_NOT_IMPLEMENTED, HH_ERR_IO
    };
    for (size_t i = 0; i < sizeof all / sizeof all[0]; i++) {
        const char *s = hh_status_str(all[i]);
        HH_ASSERT_MSG(s != NULL && s[0] != '\0', "status %d has an empty name", (int)all[i]);
        HH_ASSERT_MSG(strcmp(s, "EUNKNOWN") != 0, "status %d falls back to EUNKNOWN", (int)all[i]);
        for (int j = 0; j < n; j++)
            HH_ASSERT_MSG(strcmp(seen[j], s) != 0, "status name \"%s\" reused for a second value", s);
        seen[n++] = s;
    }
    /* A value outside the defined enum correctly falls back, rather than
     * reading out of bounds or returning NULL. */
    HH_ASSERT_EQ_STR(hh_status_str((hh_status_t)999), "EUNKNOWN");
}

static void test_link_state_str_covers_every_value_without_collapsing_to_unknown(void)
{
    const char *seen[8];
    int n = 0;
    const hh_link_state_t all[] = {
        HH_LINK_HEALTHY, HH_LINK_DEGRADED, HH_LINK_SUSPECTED_FAILURE,
        HH_LINK_FAILED, HH_LINK_RECOVERING
    };
    for (size_t i = 0; i < sizeof all / sizeof all[0]; i++) {
        const char *s = hh_link_state_str(all[i]);
        HH_ASSERT_MSG(s != NULL && s[0] != '\0', "link state %d has an empty name", (int)all[i]);
        HH_ASSERT_MSG(strcmp(s, "Unknown") != 0, "link state %d falls back to Unknown", (int)all[i]);
        for (int j = 0; j < n; j++)
            HH_ASSERT_MSG(strcmp(seen[j], s) != 0, "link state name \"%s\" reused", s);
        seen[n++] = s;
    }
    HH_ASSERT_EQ_STR(hh_link_state_str((hh_link_state_t)999), "Unknown");
}

static void test_cause_hint_str_covers_every_value_without_collapsing_to_unknown(void)
{
    const char *seen[8];
    int n = 0;
    const hh_cause_hint_t all[] = {
        HH_CAUSE_UNKNOWN, HH_CAUSE_NODE_FAILURE, HH_CAUSE_RF_INTERFERENCE,
        HH_CAUSE_MOBILITY, HH_CAUSE_OWN_RADIO_FAILURE, HH_CAUSE_ASYMMETRIC_LINK
    };
    for (size_t i = 0; i < sizeof all / sizeof all[0]; i++) {
        const char *s = hh_cause_hint_str(all[i]);
        HH_ASSERT_MSG(s != NULL && s[0] != '\0', "cause hint %d has an empty name", (int)all[i]);
        for (int j = 0; j < n; j++)
            HH_ASSERT_MSG(strcmp(seen[j], s) != 0, "cause hint name \"%s\" reused", s);
        seen[n++] = s;
    }
    /* HH_CAUSE_UNKNOWN itself legitimately maps to "unknown" -- that is the
     * documented cause, not a missing-mapping fallback, so it is excluded
     * from the "no value collapses to the fallback string" check above and
     * instead asserted explicitly here. */
    HH_ASSERT_EQ_STR(hh_cause_hint_str(HH_CAUSE_UNKNOWN), "unknown");
    HH_ASSERT_EQ_STR(hh_cause_hint_str((hh_cause_hint_t)999), "unknown");
}

static void test_withdraw_reason_str_covers_every_value(void)
{
    const char *seen[4];
    int n = 0;
    const hh_withdraw_reason_t all[] = {
        HH_WITHDRAW_EXPIRED, HH_WITHDRAW_FAILURE_CASCADE, HH_WITHDRAW_EXPLICIT
    };
    for (size_t i = 0; i < sizeof all / sizeof all[0]; i++) {
        const char *s = hh_withdraw_reason_str(all[i]);
        HH_ASSERT_MSG(s != NULL && s[0] != '\0', "withdraw reason %d has an empty name", (int)all[i]);
        HH_ASSERT_MSG(strcmp(s, "unknown") != 0, "withdraw reason %d falls back to unknown", (int)all[i]);
        for (int j = 0; j < n; j++)
            HH_ASSERT_MSG(strcmp(seen[j], s) != 0, "withdraw reason name \"%s\" reused", s);
        seen[n++] = s;
    }
    HH_ASSERT_EQ_STR(hh_withdraw_reason_str((hh_withdraw_reason_t)999), "unknown");
}

static void test_recovery_strategy_str_covers_every_value(void)
{
    const char *seen[4];
    int n = 0;
    const hh_recovery_strategy_t all[] = {
        HH_RECOVERY_ALTERNATE_ROUTE, HH_RECOVERY_REDISCOVERY, HH_RECOVERY_CHANNEL_CHANGE
    };
    for (size_t i = 0; i < sizeof all / sizeof all[0]; i++) {
        const char *s = hh_recovery_strategy_str(all[i]);
        HH_ASSERT_MSG(s != NULL && s[0] != '\0', "recovery strategy %d has an empty name", (int)all[i]);
        HH_ASSERT_MSG(strcmp(s, "unknown") != 0, "recovery strategy %d falls back to unknown", (int)all[i]);
        for (int j = 0; j < n; j++)
            HH_ASSERT_MSG(strcmp(seen[j], s) != 0, "recovery strategy name \"%s\" reused", s);
        seen[n++] = s;
    }
    HH_ASSERT_EQ_STR(hh_recovery_strategy_str((hh_recovery_strategy_t)999), "unknown");
}

/* ---------------- 3. HTI-07 pull-based consumers (Routing, Telemetry) ---------------- */

static void test_routing_and_telemetry_reflect_link_health_without_any_event_delivered(void)
{
    /*
     * Proves, executably, the design the spec documents in Sec. 9: Routing
     * and Telemetry read Link Health's state synchronously by direct
     * pointer access (hh_link_health_get), not through HH_EV_LINK_STATE_
     * CHANGED. A real hh_node_t is used (rather than loose components) so
     * Telemetry's own consumer path is exercised too. This test drives a
     * real state transition and inspects both consumers' views WITHOUT EVER
     * DRAINING THE DISPATCHER -- if either consumer secretly depended on the
     * event instead of the synchronous read, its view would stay stale
     * here, because on_link_state() would never have run.
     */
    hh_config_t cfg;
    vclock_t vc;
    mock_radio_t mock;
    hh_radio_t radio;
    hh_node_t node;
    hh_link_sample_t bad;
    hh_node_status_t st;

    hh_config_defaults(&cfg);
    cfg.node_id = 1;
    cfg.log_level = HH_LOG_ERROR;
    vclock_init(&vc, 10000);
    mock_radio_init(&mock, "iface", &radio);
    HH_ASSERT_OK(hh_node_init(&node, &cfg, &vc.clock, &radio));
    HH_ASSERT_OK(hh_node_configure(&node, NULL));
    HH_ASSERT_OK(hh_node_start(&node));

    {
        hh_beacon_t b;
        hh_link_sample_t s;
        memset(&b, 0, sizeof b);
        b.node_id = 2; b.sequence_no = 1; b.routing_capable = true;
        memset(&s, 0, sizeof s);
        s.neighbor_id = 2; s.rssi = -50.0f; s.snr = 25.0f;
        hh_neighbor_on_beacon(&node.neighbors, &b, &s, vc.now);
        hh_link_health_add(&node.link_health, 2, vc.now);
        for (int i = 0; i < 4; i++) hh_link_health_on_sample(&node.link_health, &s, vc.now);
    }
    hh_routing_on_neighbor_up(&node.routing, 2, vc.now);
    hh_routing_offer(&node.routing, 9, 2, 20, 2, 0.0f, vc.now);
    /* Drain whatever the setup above produced, to isolate what happens next. */
    hh_dispatcher_drain_all(&node.bus, 8);

    /* Drive the link into Degraded -- this DOES publish HH_EV_LINK_STATE_
     * CHANGED onto the bus, but it is deliberately never drained below. */
    memset(&bad, 0, sizeof bad);
    bad.neighbor_id = 2; bad.rssi = -92.0f; bad.snr = 18.0f; bad.per = 0.05f;
    for (int i = 0; i < 20; i++) hh_link_health_on_sample(&node.link_health, &bad, vc.now);
    HH_ASSERT_EQ_INT(hh_link_health_state(&node.link_health, 2), HH_LINK_DEGRADED);
    /* Confirm the event really is sitting there, undelivered. */
    HH_ASSERT(hh_dispatcher_pending(&node.bus) > 0);

    /* Telemetry's view reflects Degraded immediately, via hh_link_health_get
     * inside hh_telemetry_format_neighbors -- no drain has run. */
    {
        char buf[1024];
        HH_ASSERT(hh_telemetry_format_neighbors(&node, buf, sizeof buf) > 0);
        HH_ASSERT_MSG(strstr(buf, "state=Degraded") != NULL,
                      "telemetry did not reflect Degraded without an event drain: %s", buf);
    }
    hh_telemetry_node_status(&node, &st);
    HH_ASSERT_EQ_INT(st.link_transitions, node.link_health.transitions);

    /* Routing's metric computation also reads Link Health directly: a fresh,
     * strictly higher sequence offered via a second, healthy neighbor must
     * beat the now-Degraded incumbent -- only possible if routing's internal
     * composite_metric() call saw the Degraded state set above, with the
     * LinkStateChanged event still sitting undelivered throughout. */
    {
        hh_beacon_t b3; hh_link_sample_t s3;
        vclock_advance(&vc, cfg.hold_down_ms + 1);
        memset(&b3, 0, sizeof b3);
        b3.node_id = 3; b3.sequence_no = 1; b3.routing_capable = true;
        memset(&s3, 0, sizeof s3);
        s3.neighbor_id = 3; s3.rssi = -50.0f; s3.snr = 25.0f;
        hh_neighbor_on_beacon(&node.neighbors, &b3, &s3, vc.now);
        hh_link_health_add(&node.link_health, 3, vc.now);
        for (int i = 0; i < 4; i++) hh_link_health_on_sample(&node.link_health, &s3, vc.now);
        hh_routing_on_neighbor_up(&node.routing, 3, vc.now);

        HH_ASSERT_OK(hh_routing_offer(&node.routing, 9, 3, 20, 2, 0.0f, vc.now));
        HH_ASSERT_EQ_INT(hh_routing_get(&node.routing, 9)->next_hop, 3);
    }

    /* The dispatcher STILL has the original LinkStateChanged event sitting
     * undelivered throughout all of the above -- the exact proof that
     * neither Routing's nor Telemetry's reaction was event-driven. */
    HH_ASSERT(hh_dispatcher_pending(&node.bus) > 0);
}

HH_TEST_MAIN_BEGIN("interface_compliance")
    HH_RUN(test_every_component_init_rejects_null_arguments);
    HH_RUN(test_status_str_covers_every_value_without_collapsing_to_unknown);
    HH_RUN(test_link_state_str_covers_every_value_without_collapsing_to_unknown);
    HH_RUN(test_cause_hint_str_covers_every_value_without_collapsing_to_unknown);
    HH_RUN(test_withdraw_reason_str_covers_every_value);
    HH_RUN(test_recovery_strategy_str_covers_every_value);
    HH_RUN(test_routing_and_telemetry_reflect_link_health_without_any_event_delivered);
HH_TEST_MAIN_END()
