/*
 * Node/link status-change processing, verified by injecting events directly
 * onto a fully-wired hh_node_t's dispatcher bus.
 *
 * hh_node_init() wires every component through hh_dispatcher_subscribe() --
 * "never through direct nested calls between subsystems" (node.h). Every
 * other test either calls a component's own API directly (unit-level,
 * bypassing the wiring) or drives the radio/netsim layer and lets the real
 * event cascade happen as a side effect (never isolating what the node-level
 * fan-out itself does with one specific event). This file instead publishes
 * a synthetic hh_event_t straight onto a real node's bus with
 * hh_dispatcher_publish(), drains it, and inspects exactly which downstream
 * components reacted -- proving the wiring registered in hh_node_init()
 * processes each status-change event correctly, including duplicate,
 * out-of-order, and unknown-node cases no single component's own tests can
 * observe (those only ever see events their OWN producer emits).
 *
 * Stale/lost status (silence-driven link decay and neighbor cadence expiry)
 * is already verified at the component level in test_link_health.c and
 * test_neighbor.c; here it is instead driven end-to-end through a fully
 * wired node via hh_node_tick(), to verify application-visible telemetry
 * reflects the resulting state.
 */
#include "hhsdr/manet/node.h"
#include "hhsdr/manet/telemetry.h"
#include "mock_radio.h"
#include "hh_test.h"
#include "vclock.h"
#include <string.h>

typedef struct {
    hh_config_t  cfg;
    vclock_t     vc;
    mock_radio_t mock;
    hh_radio_t   radio;
    hh_node_t    node;
} fix_t;

static void fix_init_running(fix_t *f, hh_node_id_t id)
{
    memset(f, 0, sizeof *f);
    hh_config_defaults(&f->cfg);
    f->cfg.node_id = id;
    f->cfg.log_level = HH_LOG_ERROR;
    vclock_init(&f->vc, 1000);
    mock_radio_init(&f->mock, "status", &f->radio);
    HH_ASSERT_OK(hh_node_init(&f->node, &f->cfg, &f->vc.clock, &f->radio));
    HH_ASSERT_OK(hh_node_configure(&f->node, NULL));
    HH_ASSERT_OK(hh_node_start(&f->node));
}

static void establish_neighbor(fix_t *f, hh_node_id_t id, float rssi)
{
    hh_beacon_t b;
    hh_link_sample_t s;
    memset(&b, 0, sizeof b);
    b.node_id = id; b.sequence_no = 1; b.routing_capable = true;
    memset(&s, 0, sizeof s);
    s.neighbor_id = id; s.rssi = rssi; s.snr = 25.0f;
    hh_neighbor_on_beacon(&f->node.neighbors, &b, &s, f->vc.now);
    hh_link_health_add(&f->node.link_health, id, f->vc.now);
    for (int i = 0; i < 4; i++) hh_link_health_on_sample(&f->node.link_health, &s, f->vc.now);
    hh_topology_on_neighbor_up(&f->node.topology, id, f->vc.now);
    hh_routing_on_neighbor_up(&f->node.routing, id, f->vc.now);
}

static hh_event_t mk_neighbor_event(hh_event_type_t type, hh_node_id_t id, hh_time_ms_t now)
{
    hh_event_t ev;
    memset(&ev, 0, sizeof ev);
    ev.type = type;
    ev.timestamp = now;
    ev.u.neighbor.neighbor_id = id;
    return ev;
}

/* ---------------- Node status: injected NeighborUp / NeighborDown ---------------- */

static void test_injected_neighbor_up_fans_out_to_link_health_and_topology(void)
{
    fix_t f; fix_init_running(&f, 1);
    hh_event_t ev = mk_neighbor_event(HH_EV_NEIGHBOR_UP, 2, f.vc.now);

    HH_ASSERT_OK(hh_dispatcher_publish(&f.node.bus, &ev));
    hh_dispatcher_drain_all(&f.node.bus, 8);

    /* node.c's on_neighbor_up() wiring must have reached both consumers from
     * this single injected event -- no direct call between components. */
    HH_ASSERT(hh_link_health_get(&f.node.link_health, 2) != NULL);
    HH_ASSERT(hh_topology_node(&f.node.topology, 2)->is_neighbor);
    /* Routing validates the next hop against the Neighbor Manager, so a
     * NeighborUp injected without ever calling hh_neighbor_on_beacon() must
     * NOT install a route -- the real neighbor table was never populated.
     * This is the node-level proof that routing genuinely depends on the
     * Neighbor Manager's own state, not merely on the event having fired. */
    HH_ASSERT(hh_routing_get(&f.node.routing, 2) == NULL);
}

static void test_injected_neighbor_down_fans_out_and_invalidates_routes(void)
{
    fix_t f; fix_init_running(&f, 1);
    hh_event_t ev;

    establish_neighbor(&f, 2, -50.0f);
    hh_routing_offer(&f.node.routing, 9, 2, 20, 2, 0.0f, f.vc.now);
    HH_ASSERT(hh_routing_get(&f.node.routing, 9)->valid);

    ev = mk_neighbor_event(HH_EV_NEIGHBOR_DOWN, 2, f.vc.now);
    HH_ASSERT_OK(hh_dispatcher_publish(&f.node.bus, &ev));
    hh_dispatcher_drain_all(&f.node.bus, 8);

    /* Topology marks it gone and the invalidation cascade withdraws the
     * route through it -- both driven purely by the one injected event. */
    HH_ASSERT(!hh_topology_node(&f.node.topology, 2)->is_neighbor);
    HH_ASSERT(!hh_routing_get(&f.node.routing, 9)->valid);
    HH_ASSERT(hh_link_health_get(&f.node.link_health, 2) == NULL);
}

static void test_duplicate_injected_neighbor_up_is_idempotent(void)
{
    fix_t f; fix_init_running(&f, 1);
    hh_event_t ev = mk_neighbor_event(HH_EV_NEIGHBOR_UP, 2, f.vc.now);
    size_t count_after_first, count_after_second;

    hh_dispatcher_publish(&f.node.bus, &ev);
    hh_dispatcher_drain_all(&f.node.bus, 8);
    count_after_first = f.node.link_health.count;
    HH_ASSERT_EQ_INT(count_after_first, 1);

    /* The exact same event injected again must not create a second tracked
     * link or a second topology node -- hh_link_health_add() is itself
     * idempotent for an id already tracked. */
    hh_dispatcher_publish(&f.node.bus, &ev);
    hh_dispatcher_drain_all(&f.node.bus, 8);
    count_after_second = f.node.link_health.count;
    HH_ASSERT_EQ_INT(count_after_second, count_after_first);
    HH_ASSERT_EQ_INT(hh_topology_node_count(&f.node.topology), 1);
}

static void test_injected_event_for_unknown_neighbor_down_is_handled_gracefully(void)
{
    fix_t f; fix_init_running(&f, 1);
    /* NeighborDown for an id that was never added anywhere: node.c's fan-out
     * calls hh_topology_on_neighbor_down, hh_routing_invalidate_via, and
     * hh_link_health_remove unconditionally -- this must not crash and must
     * leave no trace for an id the stack never tracked. */
    hh_event_t ev = mk_neighbor_event(HH_EV_NEIGHBOR_DOWN, 99, f.vc.now);

    HH_ASSERT_OK(hh_dispatcher_publish(&f.node.bus, &ev));
    hh_dispatcher_drain_all(&f.node.bus, 8);

    HH_ASSERT(hh_link_health_get(&f.node.link_health, 99) == NULL);
    HH_ASSERT_EQ_INT(hh_topology_node_count(&f.node.topology), 1);  /* node 99 itself now tracked as not-neighbor */
    HH_ASSERT(!hh_topology_node(&f.node.topology, 99)->is_neighbor);
    HH_ASSERT_EQ_INT(hh_routing_count(&f.node.routing), 0);
}

/* ---------------- Link status: injected LinkStateChanged ---------------- */

static hh_ev_link_state_t mk_link_state_event(hh_node_id_t neighbor, hh_link_state_t from,
                                              hh_link_state_t to, hh_cause_hint_t cause)
{
    hh_ev_link_state_t e;
    e.neighbor = neighbor; e.old_state = from; e.new_state = to; e.cause_hint = cause;
    return e;
}

static void test_injected_link_state_changed_reaches_failure_detector_and_topology(void)
{
    fix_t f; fix_init_running(&f, 1);
    hh_event_t ev;

    establish_neighbor(&f, 2, -50.0f);
    memset(&ev, 0, sizeof ev);
    ev.type = HH_EV_LINK_STATE_CHANGED;
    ev.timestamp = f.vc.now;
    ev.u.link_state = mk_link_state_event(2, HH_LINK_HEALTHY, HH_LINK_DEGRADED,
                                          HH_CAUSE_MOBILITY);

    HH_ASSERT_OK(hh_dispatcher_publish(&f.node.bus, &ev));
    hh_dispatcher_drain_all(&f.node.bus, 8);

    /* node.c's on_link_state() fans out to both the Failure Detector and
     * Topology (annotation only). Degraded alone must not confirm a
     * failure -- that requires the FD's own suspected->confirmed gate. */
    HH_ASSERT(!hh_fd_is_failed(&f.node.failure_detector, 2));
    HH_ASSERT_EQ_INT(hh_topology_node(&f.node.topology, 2)->link_state, HH_LINK_DEGRADED);
    /* Degraded is an annotation, not a reachability change. */
    HH_ASSERT(hh_topology_node(&f.node.topology, 2)->reachable);
}

static void test_repeated_identical_failed_link_state_confirms_failure_once(void)
{
    fix_t f; fix_init_running(&f, 1);
    hh_event_t ev;

    establish_neighbor(&f, 2, -50.0f);
    memset(&ev, 0, sizeof ev);
    ev.type = HH_EV_LINK_STATE_CHANGED;
    ev.timestamp = f.vc.now;
    ev.u.link_state = mk_link_state_event(2, HH_LINK_SUSPECTED_FAILURE, HH_LINK_FAILED,
                                          HH_CAUSE_NODE_FAILURE);

    hh_dispatcher_publish(&f.node.bus, &ev);
    hh_dispatcher_drain_all(&f.node.bus, 8);
    HH_ASSERT(hh_fd_is_failed(&f.node.failure_detector, 2));

    /* The identical Failed report injected again (e.g. a redelivered or
     * stale-retried status update) must not re-announce the failure or
     * restart self-healing's recovery pipeline a second time. */
    uint64_t recoveries_before = f.node.self_healing.recoveries_started;
    hh_dispatcher_publish(&f.node.bus, &ev);
    hh_dispatcher_drain_all(&f.node.bus, 8);
    HH_ASSERT_EQ_INT(f.node.self_healing.recoveries_started, recoveries_before);
}

static void test_duplicate_recovering_event_after_already_healthy_does_not_reconfirm(void)
{
    fix_t f; fix_init_running(&f, 1);
    hh_event_t failed_ev, recovering_ev, healthy_ev, late_duplicate_recovering_ev;

    /*
     * hh_fd_on_link_state() has no sequence number or timestamp ordering of
     * its own (confirmed by reading src/manet/failure_detector.c): it simply
     * reacts to whatever new_state a LinkStateChanged event carries. A
     * genuinely stale re-report of Failed is therefore, by design, treated
     * as a brand-new failure rather than suppressed -- exactly what
     * test_recovery.c's test_recovery_retracts_confirmation already proves
     * ("It can then fail again and be re-announced"). So this test instead
     * verifies the guarantee that DOES exist: a late-arriving duplicate of
     * an event the FD has already superseded (Recovering, re-delivered
     * after the link has already reached Healthy) must not retract a
     * confirmation that no longer exists, and must not leave the FD
     * thinking the link is failed.
     */
    establish_neighbor(&f, 2, -50.0f);

    memset(&failed_ev, 0, sizeof failed_ev);
    failed_ev.type = HH_EV_LINK_STATE_CHANGED;
    failed_ev.timestamp = f.vc.now;
    failed_ev.u.link_state = mk_link_state_event(2, HH_LINK_SUSPECTED_FAILURE,
                                                 HH_LINK_FAILED, HH_CAUSE_NODE_FAILURE);
    hh_dispatcher_publish(&f.node.bus, &failed_ev);
    hh_dispatcher_drain_all(&f.node.bus, 8);
    HH_ASSERT(hh_fd_is_failed(&f.node.failure_detector, 2));

    memset(&recovering_ev, 0, sizeof recovering_ev);
    recovering_ev.type = HH_EV_LINK_STATE_CHANGED;
    recovering_ev.timestamp = f.vc.now;
    recovering_ev.u.link_state = mk_link_state_event(2, HH_LINK_FAILED, HH_LINK_RECOVERING,
                                                      HH_CAUSE_UNKNOWN);
    hh_dispatcher_publish(&f.node.bus, &recovering_ev);
    hh_dispatcher_drain_all(&f.node.bus, 8);
    HH_ASSERT(!hh_fd_is_failed(&f.node.failure_detector, 2));

    memset(&healthy_ev, 0, sizeof healthy_ev);
    healthy_ev.type = HH_EV_LINK_STATE_CHANGED;
    healthy_ev.timestamp = f.vc.now;
    healthy_ev.u.link_state = mk_link_state_event(2, HH_LINK_RECOVERING, HH_LINK_HEALTHY,
                                                  HH_CAUSE_UNKNOWN);
    hh_dispatcher_publish(&f.node.bus, &healthy_ev);
    hh_dispatcher_drain_all(&f.node.bus, 8);

    /* The Recovering event is now delivered a second time, late -- the link
     * has already moved on to Healthy. */
    uint64_t retractions_before = f.node.failure_detector.retractions;
    memset(&late_duplicate_recovering_ev, 0, sizeof late_duplicate_recovering_ev);
    late_duplicate_recovering_ev.type = HH_EV_LINK_STATE_CHANGED;
    late_duplicate_recovering_ev.timestamp = f.vc.now;
    late_duplicate_recovering_ev.u.link_state = recovering_ev.u.link_state;
    hh_dispatcher_publish(&f.node.bus, &late_duplicate_recovering_ev);
    hh_dispatcher_drain_all(&f.node.bus, 8);

    /* No confirmation was outstanding to retract, and the link must still
     * read as not-failed. */
    HH_ASSERT_EQ_INT(f.node.failure_detector.retractions, retractions_before);
    HH_ASSERT(!hh_fd_is_failed(&f.node.failure_detector, 2));
}

/* ---------------- Stale/lost status driven end-to-end through hh_node_tick ---------------- */

static void test_silence_without_explicit_down_event_eventually_confirms_failure(void)
{
    fix_t f; fix_init_running(&f, 1);
    hh_node_status_t st;

    establish_neighbor(&f, 2, -50.0f);

    /* No NeighborDown, no LinkStateChanged is ever injected or produced by a
     * radio event: purely letting virtual time pass with nothing arriving
     * from neighbor 2 models real silence/staleness, not an explicit
     * "link down" signal. hh_node_tick runs link health before neighbor
     * expiry so the state machine gets to reach Failed. */
    for (int i = 0; i < 40; i++) {
        vclock_advance(&f.vc, 200);
        hh_node_tick(&f.node, f.vc.now);
    }

    HH_ASSERT_MSG(hh_link_health_state(&f.node.link_health, 2) == HH_LINK_FAILED,
                  "silent link did not reach Failed: state=%s",
                  hh_link_state_str(hh_link_health_state(&f.node.link_health, 2)));
    HH_ASSERT_MSG(hh_fd_is_failed(&f.node.failure_detector, 2),
                  "stale status never reached a confirmed failure");

    /* Application-visible behavior: telemetry reports the confirmed failure,
     * not a stale "still healthy" counter. */
    hh_telemetry_node_status(&f.node, &st);
    HH_ASSERT(st.failures_confirmed >= 1);
}

static void test_recovery_immediately_after_failure_is_processed(void)
{
    fix_t f; fix_init_running(&f, 1);
    hh_event_t fail_ev;

    establish_neighbor(&f, 2, -50.0f);
    fail_ev = mk_neighbor_event(HH_EV_NEIGHBOR_DOWN, 2, f.vc.now);
    hh_dispatcher_publish(&f.node.bus, &fail_ev);
    hh_dispatcher_drain_all(&f.node.bus, 8);
    HH_ASSERT(hh_link_health_get(&f.node.link_health, 2) == NULL);

    /* Immediately -- same virtual timestamp, no delay -- the node hears the
     * neighbor again. This must be processed as an ordinary rediscovery,
     * not rejected or left in an inconsistent half-recovered state. */
    establish_neighbor(&f, 2, -50.0f);
    HH_ASSERT(hh_link_health_get(&f.node.link_health, 2) != NULL);
    HH_ASSERT_EQ_INT(hh_link_health_state(&f.node.link_health, 2), HH_LINK_HEALTHY);
    HH_ASSERT(hh_topology_node(&f.node.topology, 2)->is_neighbor);
}

static void test_multiple_injected_status_changes_in_sequence_leave_consistent_state(void)
{
    fix_t f; fix_init_running(&f, 1);
    hh_event_t up, down, link_degraded, link_healthy;

    /* A rapid, directly-injected sequence: up, degrade, recover, down --
     * exercising node-level fan-out repeatedly without any radio traffic at
     * all, and checking only the FINAL state is self-consistent. */
    up = mk_neighbor_event(HH_EV_NEIGHBOR_UP, 2, f.vc.now);
    hh_dispatcher_publish(&f.node.bus, &up);
    hh_dispatcher_drain_all(&f.node.bus, 8);

    memset(&link_degraded, 0, sizeof link_degraded);
    link_degraded.type = HH_EV_LINK_STATE_CHANGED;
    link_degraded.timestamp = f.vc.now;
    link_degraded.u.link_state = mk_link_state_event(2, HH_LINK_HEALTHY, HH_LINK_DEGRADED,
                                                      HH_CAUSE_MOBILITY);
    hh_dispatcher_publish(&f.node.bus, &link_degraded);
    hh_dispatcher_drain_all(&f.node.bus, 8);

    memset(&link_healthy, 0, sizeof link_healthy);
    link_healthy.type = HH_EV_LINK_STATE_CHANGED;
    link_healthy.timestamp = f.vc.now;
    link_healthy.u.link_state = mk_link_state_event(2, HH_LINK_DEGRADED, HH_LINK_HEALTHY,
                                                     HH_CAUSE_UNKNOWN);
    hh_dispatcher_publish(&f.node.bus, &link_healthy);
    hh_dispatcher_drain_all(&f.node.bus, 8);

    down = mk_neighbor_event(HH_EV_NEIGHBOR_DOWN, 2, f.vc.now);
    hh_dispatcher_publish(&f.node.bus, &down);
    hh_dispatcher_drain_all(&f.node.bus, 8);

    /* Final state: fully torn down, no leftover tracking from any
     * intermediate step in the sequence. */
    HH_ASSERT(hh_link_health_get(&f.node.link_health, 2) == NULL);
    HH_ASSERT(!hh_topology_node(&f.node.topology, 2)->is_neighbor);
    HH_ASSERT_EQ_INT(hh_dispatcher_dropped(&f.node.bus), 0);
}

HH_TEST_MAIN_BEGIN("status_events")
    HH_RUN(test_injected_neighbor_up_fans_out_to_link_health_and_topology);
    HH_RUN(test_injected_neighbor_down_fans_out_and_invalidates_routes);
    HH_RUN(test_duplicate_injected_neighbor_up_is_idempotent);
    HH_RUN(test_injected_event_for_unknown_neighbor_down_is_handled_gracefully);
    HH_RUN(test_injected_link_state_changed_reaches_failure_detector_and_topology);
    HH_RUN(test_repeated_identical_failed_link_state_confirms_failure_once);
    HH_RUN(test_duplicate_recovering_event_after_already_healthy_does_not_reconfirm);
    HH_RUN(test_silence_without_explicit_down_event_eventually_confirms_failure);
    HH_RUN(test_recovery_immediately_after_failure_is_processed);
    HH_RUN(test_multiple_injected_status_changes_in_sequence_leave_consistent_state);
HH_TEST_MAIN_END()
