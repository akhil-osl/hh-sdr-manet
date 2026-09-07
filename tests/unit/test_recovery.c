/*
 * Failure Detector, Topology Manager, and Self-Healing tests (Doc 1 §5, §7, §8).
 *
 * The pipeline these implement is: detection -> classification -> confirmation
 * -> event -> topology update -> route invalidation -> alternate/rediscovery ->
 * stabilization. These tests verify each stage gates the next correctly, and
 * that suspicion alone never reaches route invalidation.
 */
#include "hhsdr/manet/failure_detector.h"
#include "hhsdr/manet/self_healing.h"
#include "hhsdr/manet/topology.h"
#include "mock_radio.h"
#include "hh_test.h"
#include "vclock.h"
#include <string.h>

typedef struct {
    hh_config_t           cfg;
    vclock_t              vc;
    hh_dispatcher_t       bus;
    hh_neighbor_mgr_t     nm;
    hh_link_health_t      lh;
    hh_routing_t          rt;
    hh_failure_detector_t fd;
    hh_topology_t         topo;
    hh_self_healing_t     sh;
    mock_radio_t          mock;
    hh_radio_t            radio;
} fix_t;

typedef struct {
    int failures, recoveries_started, recoveries_completed, partitions, merges;
    hh_ev_failure_t  last_failure;
    hh_ev_recovery_t last_recovery;
} obs_t;

static void observe(const hh_event_t *ev, void *ctx)
{
    obs_t *o = ctx;
    switch (ev->type) {
    case HH_EV_FAILURE_DETECTED:   o->failures++; o->last_failure = ev->u.failure; break;
    case HH_EV_RECOVERY_STARTED:   o->recoveries_started++; break;
    case HH_EV_RECOVERY_COMPLETED: o->recoveries_completed++; o->last_recovery = ev->u.recovery; break;
    case HH_EV_PARTITION_DETECTED: o->partitions++; break;
    case HH_EV_NETWORK_MERGED:     o->merges++; break;
    default: break;
    }
}

static void add_neighbor(fix_t *f, hh_node_id_t id, float rssi)
{
    hh_beacon_t b;
    hh_link_sample_t s;
    memset(&b, 0, sizeof b);
    b.node_id = id; b.sequence_no = 1; b.routing_capable = true;
    memset(&s, 0, sizeof s);
    s.neighbor_id = id; s.rssi = rssi; s.snr = 25.0f;
    hh_neighbor_on_beacon(&f->nm, &b, &s, f->vc.now);
    hh_link_health_add(&f->lh, id, f->vc.now);
    for (int i = 0; i < 4; i++) hh_link_health_on_sample(&f->lh, &s, f->vc.now);
    hh_topology_on_neighbor_up(&f->topo, id, f->vc.now);
}

static void fix_init(fix_t *f, obs_t *o)
{
    memset(f, 0, sizeof *f);
    memset(o, 0, sizeof *o);
    hh_config_defaults(&f->cfg);
    f->cfg.node_id = 1;
    vclock_init(&f->vc, 10000);
    hh_dispatcher_init(&f->bus);
    hh_dispatcher_subscribe(&f->bus, "obs", HH_EV_MASK_ALL, observe, o);
    mock_radio_init(&f->mock, "sh", &f->radio);
    hh_radio_open(&f->radio);
    hh_neighbor_init(&f->nm, &f->cfg, &f->vc.clock, &f->bus);
    hh_link_health_init(&f->lh, &f->cfg, &f->vc.clock, &f->bus);
    hh_routing_init(&f->rt, &f->cfg, &f->vc.clock, &f->bus, &f->nm, &f->lh);
    hh_fd_init(&f->fd, &f->cfg, &f->vc.clock, &f->bus);
    hh_topology_init(&f->topo, &f->cfg, &f->vc.clock, &f->bus);
    hh_sh_init(&f->sh, &f->cfg, &f->vc.clock, &f->bus, &f->rt, &f->topo, &f->radio);
}

static hh_ev_link_state_t link_ev(hh_node_id_t n, hh_link_state_t from,
                                  hh_link_state_t to, hh_cause_hint_t c)
{
    hh_ev_link_state_t e;
    e.neighbor = n; e.old_state = from; e.new_state = to; e.cause_hint = c;
    return e;
}

/* ---------------- Failure Detector ---------------- */

static void test_suspicion_alone_never_confirms(void)
{
    fix_t f; obs_t o; fix_init(&f, &o);
    hh_ev_link_state_t ev = link_ev(2, HH_LINK_DEGRADED, HH_LINK_SUSPECTED_FAILURE,
                                    HH_CAUSE_NODE_FAILURE);

    HH_ASSERT_OK(hh_fd_on_link_state(&f.fd, &ev, f.vc.now));
    hh_dispatcher_drain(&f.bus);
    /* Doc 1 §8: "Degradation Detected alone, without confirmation, never
     * reaches route invalidation." */
    HH_ASSERT_EQ_INT(o.failures, 0);
    HH_ASSERT(!hh_fd_is_failed(&f.fd, 2));
}

static void test_failed_state_confirms_once(void)
{
    fix_t f; obs_t o; fix_init(&f, &o);
    hh_ev_link_state_t sus = link_ev(2, HH_LINK_DEGRADED, HH_LINK_SUSPECTED_FAILURE,
                                     HH_CAUSE_NODE_FAILURE);
    hh_ev_link_state_t fail = link_ev(2, HH_LINK_SUSPECTED_FAILURE, HH_LINK_FAILED,
                                      HH_CAUSE_NODE_FAILURE);

    hh_fd_on_link_state(&f.fd, &sus, f.vc.now);
    vclock_advance(&f.vc, 2000);
    hh_fd_on_link_state(&f.fd, &fail, f.vc.now);
    hh_dispatcher_drain(&f.bus);

    HH_ASSERT_EQ_INT(o.failures, 1);
    HH_ASSERT_EQ_INT(o.last_failure.neighbor_or_node_id, 2);
    HH_ASSERT_EQ_INT(o.last_failure.cause_hint, HH_CAUSE_NODE_FAILURE);
    HH_ASSERT(hh_fd_is_failed(&f.fd, 2));

    /* A repeated Failed report must not re-announce. */
    hh_fd_on_link_state(&f.fd, &fail, f.vc.now);
    hh_dispatcher_drain(&f.bus);
    HH_ASSERT_EQ_INT(o.failures, 1);
}

static void test_recovery_retracts_confirmation(void)
{
    fix_t f; obs_t o; fix_init(&f, &o);
    hh_ev_link_state_t fail = link_ev(2, HH_LINK_SUSPECTED_FAILURE, HH_LINK_FAILED,
                                      HH_CAUSE_NODE_FAILURE);
    hh_ev_link_state_t rec = link_ev(2, HH_LINK_FAILED, HH_LINK_RECOVERING,
                                     HH_CAUSE_UNKNOWN);

    hh_fd_on_link_state(&f.fd, &fail, f.vc.now);
    HH_ASSERT(hh_fd_is_failed(&f.fd, 2));

    /* A returning node must not stay permanently marked failed. */
    hh_fd_on_link_state(&f.fd, &rec, f.vc.now);
    HH_ASSERT(!hh_fd_is_failed(&f.fd, 2));

    /* It can then fail again and be re-announced. */
    hh_fd_on_link_state(&f.fd, &fail, f.vc.now);
    hh_dispatcher_drain(&f.bus);
    HH_ASSERT_EQ_INT(o.failures, 2);
}

static void test_node_failure_requires_corroboration_count(void)
{
    fix_t f; obs_t o; fix_init(&f, &o);
    hh_ev_link_state_t fail = link_ev(7, HH_LINK_SUSPECTED_FAILURE, HH_LINK_FAILED,
                                      HH_CAUSE_NODE_FAILURE);

    hh_fd_on_link_state(&f.fd, &fail, f.vc.now);
    /* Our own observation counts as one; Doc 1 §8 requires every neighbor that
     * could see the node to confirm before it leaves the aggregate topology. */
    HH_ASSERT_EQ_INT(hh_fd_corroborations(&f.fd, 7), 1);
    hh_fd_corroborate(&f.fd, 7);
    hh_fd_corroborate(&f.fd, 7);
    HH_ASSERT_EQ_INT(hh_fd_corroborations(&f.fd, 7), 3);
}

/* ---------------- Topology ---------------- */

static void test_topology_tracks_neighbors_and_routes(void)
{
    fix_t f; obs_t o; fix_init(&f, &o);
    add_neighbor(&f, 2, -50.0f);
    hh_topology_on_route_installed(&f.topo, 9, 2, 2, f.vc.now);

    HH_ASSERT_EQ_INT(hh_topology_node_count(&f.topo), 2);
    HH_ASSERT_EQ_INT(hh_topology_reachable_count(&f.topo), 2);
    HH_ASSERT(hh_topology_node(&f.topo, 2)->is_neighbor);
    HH_ASSERT_EQ_INT(hh_topology_node(&f.topo, 9)->hop_count, 2);
    /* A multi-hop destination is not a neighbor. */
    HH_ASSERT(!hh_topology_node(&f.topo, 9)->is_neighbor);
}

static void test_link_state_is_annotation_only(void)
{
    fix_t f; obs_t o; fix_init(&f, &o);
    hh_ev_link_state_t ev = link_ev(2, HH_LINK_HEALTHY, HH_LINK_DEGRADED,
                                    HH_CAUSE_MOBILITY);
    add_neighbor(&f, 2, -50.0f);
    hh_topology_on_link_state(&f.topo, &ev, f.vc.now);

    /* The annotation is recorded for operators but must not change reachability
     * — routing metrics flow directly from Link Health to Routing. */
    HH_ASSERT_EQ_INT(hh_topology_node(&f.topo, 2)->link_state, HH_LINK_DEGRADED);
    HH_ASSERT(hh_topology_node(&f.topo, 2)->reachable);
}

static void test_partition_detected_only_when_whole_branch_lost(void)
{
    fix_t f; obs_t o; fix_init(&f, &o);
    add_neighbor(&f, 2, -50.0f);
    add_neighbor(&f, 3, -50.0f);
    hh_topology_on_route_installed(&f.topo, 9, 2, 2, f.vc.now);

    /* Losing one destination is not a partition. */
    hh_topology_on_route_withdrawn(&f.topo, 9, f.vc.now);
    hh_topology_evaluate(&f.topo, f.vc.now);
    HH_ASSERT(!hh_topology_is_partitioned(&f.topo));

    /* Losing one of two neighbors is still not a partition. */
    hh_topology_on_neighbor_down(&f.topo, 2, f.vc.now);
    hh_topology_evaluate(&f.topo, f.vc.now);
    HH_ASSERT(!hh_topology_is_partitioned(&f.topo));

    /* Losing every neighbor and every route is. */
    hh_topology_on_neighbor_down(&f.topo, 3, f.vc.now);
    hh_topology_evaluate(&f.topo, f.vc.now);
    HH_ASSERT(hh_topology_is_partitioned(&f.topo));
    hh_dispatcher_drain(&f.bus);
    HH_ASSERT_EQ_INT(o.partitions, 1);
}

static void test_merge_detected_with_holddown(void)
{
    fix_t f; obs_t o; fix_init(&f, &o);
    add_neighbor(&f, 2, -50.0f);
    hh_topology_on_neighbor_down(&f.topo, 2, f.vc.now);
    hh_topology_evaluate(&f.topo, f.vc.now);
    HH_ASSERT(hh_topology_is_partitioned(&f.topo));

    /* Renewed contact across the severed edge is a merge. */
    vclock_advance(&f.vc, 5000);
    hh_topology_on_neighbor_up(&f.topo, 2, f.vc.now);
    HH_ASSERT(!hh_topology_is_partitioned(&f.topo));
    hh_dispatcher_drain(&f.bus);
    HH_ASSERT_EQ_INT(o.merges, 1);

    /* Merged routes are held down before being trusted as primary. */
    HH_ASSERT(hh_topology_in_merge_holddown(&f.topo, f.vc.now));
    vclock_advance(&f.vc, f.cfg.merge_hold_down_ms + 1);
    HH_ASSERT(!hh_topology_in_merge_holddown(&f.topo, f.vc.now));
}

/* ---------------- Self-Healing ---------------- */

static void test_failure_triggers_invalidation_and_alternate_switch(void)
{
    fix_t f; obs_t o; fix_init(&f, &o);
    hh_ev_failure_t ev;

    add_neighbor(&f, 2, -45.0f);
    add_neighbor(&f, 3, -50.0f);
    /* Destination 9 reachable via 2, with 3 as the warm standby. */
    hh_routing_offer(&f.rt, 9, 2, 20, 2, 0.0f, f.vc.now);
    hh_routing_offer(&f.rt, 9, 3, 20, 3, 0.1f, f.vc.now);
    HH_ASSERT(hh_routing_get(&f.rt, 9)->has_alt);

    memset(&ev, 0, sizeof ev);
    ev.neighbor_or_node_id = 2;
    ev.cause_hint = HH_CAUSE_NODE_FAILURE;
    HH_ASSERT_OK(hh_sh_on_failure(&f.sh, &ev, f.vc.now));

    /* The route survived by switching to the warm alternate. */
    HH_ASSERT(hh_routing_get(&f.rt, 9)->valid);
    HH_ASSERT_EQ_INT(hh_routing_get(&f.rt, 9)->next_hop, 3);
    hh_dispatcher_drain(&f.bus);
    HH_ASSERT_EQ_INT(o.recoveries_started, 1);
}

static void test_rf_interference_prefers_channel_change_over_route_churn(void)
{
    fix_t f; obs_t o; fix_init(&f, &o);
    hh_ev_failure_t ev;
    uint32_t before_channel;

    add_neighbor(&f, 2, -50.0f);
    hh_routing_offer(&f.rt, 9, 2, 20, 2, 0.0f, f.vc.now);
    before_channel = f.mock.channel;

    memset(&ev, 0, sizeof ev);
    ev.neighbor_or_node_id = 2;
    ev.cause_hint = HH_CAUSE_RF_INTERFERENCE;
    hh_sh_on_failure(&f.sh, &ev, f.vc.now);

    /* Doc 1 §8: prefer a channel change over route churn for an RF cause. */
    HH_ASSERT(f.mock.channel != before_channel);
    HH_ASSERT_EQ_INT(f.sh.channel_changes, 1);
    /* Routes stay intact: the link may return on the new channel. */
    HH_ASSERT(hh_routing_get(&f.rt, 9)->valid);
}

static void test_channel_change_unsupported_falls_back_to_route_recovery(void)
{
    fix_t f; obs_t o; fix_init(&f, &o);
    hh_ev_failure_t ev;

    add_neighbor(&f, 2, -50.0f);
    hh_routing_offer(&f.rt, 9, 2, 20, 2, 0.0f, f.vc.now);
    f.mock.supports_channel_change = false;   /* radio cannot retune */

    memset(&ev, 0, sizeof ev);
    ev.neighbor_or_node_id = 2;
    ev.cause_hint = HH_CAUSE_RF_INTERFERENCE;
    hh_sh_on_failure(&f.sh, &ev, f.vc.now);

    /* Falls back to route-based recovery rather than failing the recovery. */
    HH_ASSERT_EQ_INT(f.sh.channel_change_failures, 1);
    HH_ASSERT(!hh_routing_get(&f.rt, 9)->valid);
}

static void test_no_alternate_enters_bounded_rediscovery(void)
{
    fix_t f; obs_t o; fix_init(&f, &o);
    hh_ev_failure_t ev;

    add_neighbor(&f, 2, -50.0f);
    hh_routing_offer(&f.rt, 9, 2, 20, 2, 0.0f, f.vc.now);

    memset(&ev, 0, sizeof ev);
    ev.neighbor_or_node_id = 2;
    ev.cause_hint = HH_CAUSE_NODE_FAILURE;
    hh_sh_on_failure(&f.sh, &ev, f.vc.now);
    HH_ASSERT(hh_sh_is_recovering(&f.sh, 2));

    /* Retries are bounded and back off, rather than retrying forever. */
    for (int i = 0; i < 20; i++) {
        vclock_advance(&f.vc, f.cfg.rediscovery_backoff_ms * 16);
        hh_sh_tick(&f.sh, f.vc.now);
    }
    HH_ASSERT(!hh_sh_is_recovering(&f.sh, 2));
    hh_dispatcher_drain(&f.bus);
    HH_ASSERT_EQ_INT(o.recoveries_completed, 1);
    HH_ASSERT(!o.last_recovery.succeeded);
}

static void test_recovery_completes_when_route_returns(void)
{
    fix_t f; obs_t o; fix_init(&f, &o);
    hh_ev_failure_t ev;
    hh_beacon_t b;
    hh_link_sample_t s;

    add_neighbor(&f, 2, -50.0f);
    hh_routing_offer(&f.rt, 9, 2, 20, 2, 0.0f, f.vc.now);

    memset(&ev, 0, sizeof ev);
    ev.neighbor_or_node_id = 2;
    ev.cause_hint = HH_CAUSE_NODE_FAILURE;
    hh_sh_on_failure(&f.sh, &ev, f.vc.now);
    HH_ASSERT(hh_sh_is_recovering(&f.sh, 2));

    /* The node comes back and a route to it is reinstalled. */
    vclock_advance(&f.vc, 1000);
    memset(&b, 0, sizeof b);
    b.node_id = 2; b.sequence_no = 50; b.routing_capable = true;
    memset(&s, 0, sizeof s);
    s.neighbor_id = 2; s.rssi = -50.0f; s.snr = 25.0f;
    hh_neighbor_on_beacon(&f.nm, &b, &s, f.vc.now);
    hh_routing_on_neighbor_up(&f.rt, 2, f.vc.now);

    HH_ASSERT_EQ_INT(hh_sh_tick(&f.sh, f.vc.now), 1);
    HH_ASSERT(!hh_sh_is_recovering(&f.sh, 2));
    hh_dispatcher_drain(&f.bus);
    HH_ASSERT(o.last_recovery.succeeded);
    /* Stabilization: reported as still under hold-down. */
    HH_ASSERT(o.last_recovery.hold_down_active);
}

static void test_duplicate_failure_does_not_restart_recovery(void)
{
    fix_t f; obs_t o; fix_init(&f, &o);
    hh_ev_failure_t ev;

    add_neighbor(&f, 2, -50.0f);
    hh_routing_offer(&f.rt, 9, 2, 20, 2, 0.0f, f.vc.now);

    memset(&ev, 0, sizeof ev);
    ev.neighbor_or_node_id = 2;
    ev.cause_hint = HH_CAUSE_NODE_FAILURE;
    HH_ASSERT_OK(hh_sh_on_failure(&f.sh, &ev, f.vc.now));
    HH_ASSERT_ERR(hh_sh_on_failure(&f.sh, &ev, f.vc.now), HH_ERR_AGAIN);
    HH_ASSERT_EQ_INT(f.sh.recoveries_started, 1);
}

static void test_partition_does_not_flush_routes(void)
{
    fix_t f; obs_t o; fix_init(&f, &o);
    hh_ev_partition_t pev;

    add_neighbor(&f, 2, -50.0f);
    hh_routing_offer(&f.rt, 9, 2, 20, 2, 0.0f, f.vc.now);

    memset(&pev, 0, sizeof pev);
    pev.branch_root = 9;
    pev.unreachable_count = 1;
    hh_sh_on_partition(&f.sh, &pev, f.vc.now);

    /* Doc 1 §8: each partition keeps operating within itself, and stale
     * cross-partition entries age out normally rather than being flushed. */
    HH_ASSERT(hh_routing_get(&f.rt, 9)->valid);
}

HH_TEST_MAIN_BEGIN("recovery")
    HH_RUN(test_suspicion_alone_never_confirms);
    HH_RUN(test_failed_state_confirms_once);
    HH_RUN(test_recovery_retracts_confirmation);
    HH_RUN(test_node_failure_requires_corroboration_count);
    HH_RUN(test_topology_tracks_neighbors_and_routes);
    HH_RUN(test_link_state_is_annotation_only);
    HH_RUN(test_partition_detected_only_when_whole_branch_lost);
    HH_RUN(test_merge_detected_with_holddown);
    HH_RUN(test_failure_triggers_invalidation_and_alternate_switch);
    HH_RUN(test_rf_interference_prefers_channel_change_over_route_churn);
    HH_RUN(test_channel_change_unsupported_falls_back_to_route_recovery);
    HH_RUN(test_no_alternate_enters_bounded_rediscovery);
    HH_RUN(test_recovery_completes_when_route_returns);
    HH_RUN(test_duplicate_failure_does_not_restart_recovery);
    HH_RUN(test_partition_does_not_flush_routes);
HH_TEST_MAIN_END()
