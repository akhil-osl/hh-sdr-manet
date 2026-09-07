/*
 * Routing Engine tests (Doc 1 §6, §8, §10).
 *
 * Covers the selection rule, two-phase invalidation, alternate-route promotion,
 * route aging, dampening, and the snapshot contract the forwarder depends on.
 */
#include "hhsdr/manet/routing.h"
#include "hh_test.h"
#include "vclock.h"
#include <string.h>

typedef struct {
    hh_config_t       cfg;
    vclock_t          vc;
    hh_dispatcher_t   bus;
    hh_neighbor_mgr_t nm;
    hh_link_health_t  lh;
    hh_routing_t      rt;
} fix_t;

typedef struct {
    int installs, withdrawals;
    hh_ev_route_installed_t last_install;
    hh_ev_route_withdrawn_t last_withdraw;
} obs_t;

static void observe(const hh_event_t *ev, void *ctx)
{
    obs_t *o = ctx;
    if (ev->type == HH_EV_ROUTE_INSTALLED) { o->installs++; o->last_install = ev->u.route_installed; }
    else if (ev->type == HH_EV_ROUTE_WITHDRAWN) { o->withdrawals++; o->last_withdraw = ev->u.route_withdrawn; }
}

/* Register a neighbor in both the neighbor table and link health, since routing
 * validates next hops against the authoritative one-hop table. */
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
}

static void fix_init(fix_t *f, obs_t *o)
{
    memset(f, 0, sizeof *f);
    memset(o, 0, sizeof *o);
    hh_config_defaults(&f->cfg);
    f->cfg.node_id = 1;
    vclock_init(&f->vc, 10000);
    hh_dispatcher_init(&f->bus);
    hh_dispatcher_subscribe(&f->bus, "obs",
        HH_EV_MASK(HH_EV_ROUTE_INSTALLED) | HH_EV_MASK(HH_EV_ROUTE_WITHDRAWN),
        observe, o);
    hh_neighbor_init(&f->nm, &f->cfg, &f->vc.clock, &f->bus);
    hh_link_health_init(&f->lh, &f->cfg, &f->vc.clock, &f->bus);
    hh_routing_init(&f->rt, &f->cfg, &f->vc.clock, &f->bus, &f->nm, &f->lh);
}

static void test_neighbor_up_installs_one_hop_route(void)
{
    fix_t f; obs_t o; fix_init(&f, &o);
    add_neighbor(&f, 2, -50.0f);

    HH_ASSERT_OK(hh_routing_on_neighbor_up(&f.rt, 2, f.vc.now));
    const hh_route_t *r = hh_routing_get(&f.rt, 2);
    HH_ASSERT(r != NULL);
    HH_ASSERT_EQ_INT(r->next_hop, 2);
    HH_ASSERT_EQ_INT(r->hop_count, 1);
    HH_ASSERT(r->valid);

    hh_dispatcher_drain(&f.bus);
    HH_ASSERT_EQ_INT(o.installs, 1);
    HH_ASSERT_EQ_INT(o.last_install.destination, 2);
}

static void test_route_rejected_without_valid_next_hop_neighbor(void)
{
    fix_t f; obs_t o; fix_init(&f, &o);
    /* Doc 1 §6: "a route is only as valid as its next-hop neighbor entry". */
    HH_ASSERT_ERR(hh_routing_offer(&f.rt, 5, 2, 10, 2, 0.1f, f.vc.now), HH_ERR_NOTFOUND);
    HH_ASSERT_EQ_INT(hh_routing_count(&f.rt), 0);
}

static void test_fresher_sequence_always_wins(void)
{
    fix_t f; obs_t o; fix_init(&f, &o);
    add_neighbor(&f, 2, -50.0f);
    add_neighbor(&f, 3, -90.0f);   /* deliberately a worse link */

    /* Install via the good neighbor first. */
    HH_ASSERT_OK(hh_routing_offer(&f.rt, 9, 2, 10, 2, 0.1f, f.vc.now));
    HH_ASSERT_EQ_INT(hh_routing_get(&f.rt, 9)->next_hop, 2);

    /* A strictly fresher sequence wins even though the metric is worse. */
    HH_ASSERT_OK(hh_routing_offer(&f.rt, 9, 3, 11, 4, 0.9f, f.vc.now));
    HH_ASSERT_EQ_INT(hh_routing_get(&f.rt, 9)->next_hop, 3);
    HH_ASSERT_EQ_INT(hh_routing_get(&f.rt, 9)->sequence_no, 11);
}

static void test_stale_sequence_rejected(void)
{
    fix_t f; obs_t o; fix_init(&f, &o);
    add_neighbor(&f, 2, -50.0f);
    add_neighbor(&f, 3, -50.0f);

    hh_routing_offer(&f.rt, 9, 2, 20, 2, 0.1f, f.vc.now);
    /* An older sequence must never replace a newer route. */
    HH_ASSERT_ERR(hh_routing_offer(&f.rt, 9, 3, 19, 1, 0.0f, f.vc.now), HH_ERR_AGAIN);
    HH_ASSERT_EQ_INT(hh_routing_get(&f.rt, 9)->next_hop, 2);
    HH_ASSERT_EQ_INT(f.rt.rejected_stale, 1);
}

static void test_equal_sequence_better_metric_wins_after_holddown(void)
{
    fix_t f; obs_t o; fix_init(&f, &o);
    add_neighbor(&f, 2, -90.0f);   /* poor link  */
    add_neighbor(&f, 3, -45.0f);   /* good link  */

    hh_routing_offer(&f.rt, 9, 2, 20, 3, 0.5f, f.vc.now);
    HH_ASSERT_EQ_INT(hh_routing_get(&f.rt, 9)->next_hop, 2);

    /* Inside the hold-down window a fresh route is protected from replacement
     * at equal sequence (Doc 1 §8). */
    HH_ASSERT_ERR(hh_routing_offer(&f.rt, 9, 3, 20, 2, 0.0f, f.vc.now), HH_ERR_AGAIN);
    HH_ASSERT_EQ_INT(hh_routing_get(&f.rt, 9)->next_hop, 2);

    /* Once it expires, the better composite metric wins at equal sequence. */
    vclock_advance(&f.vc, f.cfg.hold_down_ms + 1);
    HH_ASSERT_OK(hh_routing_offer(&f.rt, 9, 3, 20, 2, 0.0f, f.vc.now));
    HH_ASSERT_EQ_INT(hh_routing_get(&f.rt, 9)->next_hop, 3);
}

static void test_second_best_kept_as_warm_alternate(void)
{
    fix_t f; obs_t o; fix_init(&f, &o);
    add_neighbor(&f, 2, -45.0f);
    add_neighbor(&f, 3, -60.0f);

    hh_routing_offer(&f.rt, 9, 2, 20, 2, 0.0f, f.vc.now);
    /* A losing but viable next hop is retained as the warm standby. */
    hh_routing_offer(&f.rt, 9, 3, 20, 3, 0.2f, f.vc.now);

    const hh_route_t *r = hh_routing_get(&f.rt, 9);
    HH_ASSERT(r->has_alt);
    HH_ASSERT_EQ_INT(r->alt_next_hop, 3);
    HH_ASSERT_EQ_INT(r->next_hop, 2);
}

static void test_invalidation_promotes_alternate_without_recompute(void)
{
    fix_t f; obs_t o; fix_init(&f, &o);
    add_neighbor(&f, 2, -45.0f);
    add_neighbor(&f, 3, -60.0f);

    hh_routing_offer(&f.rt, 9, 2, 20, 2, 0.0f, f.vc.now);
    hh_routing_offer(&f.rt, 9, 3, 20, 3, 0.2f, f.vc.now);

    /* The failed next hop's routes switch to the warm alternate immediately. */
    HH_ASSERT_EQ_INT(hh_routing_invalidate_via(&f.rt, 2, HH_WITHDRAW_FAILURE_CASCADE,
                                               f.vc.now), 1);
    const hh_route_t *r = hh_routing_get(&f.rt, 9);
    HH_ASSERT(r->valid);                 /* still reachable */
    HH_ASSERT_EQ_INT(r->next_hop, 3);
    HH_ASSERT(!r->has_alt);              /* standby consumed */
}

static void test_invalidation_cascade_marks_invalid_when_no_alternate(void)
{
    fix_t f; obs_t o; fix_init(&f, &o);
    add_neighbor(&f, 2, -45.0f);

    /* Several destinations all reached through neighbor 2. */
    hh_routing_offer(&f.rt, 9,  2, 20, 2, 0.0f, f.vc.now);
    hh_routing_offer(&f.rt, 10, 2, 20, 3, 0.0f, f.vc.now);
    hh_routing_offer(&f.rt, 11, 2, 20, 4, 0.0f, f.vc.now);

    /* The cascade covers every route whose next hop is the failed neighbor. */
    HH_ASSERT_EQ_INT(hh_routing_invalidate_via(&f.rt, 2, HH_WITHDRAW_FAILURE_CASCADE,
                                               f.vc.now), 3);
    HH_ASSERT(!hh_routing_get(&f.rt, 9)->valid);
    HH_ASSERT(!hh_routing_get(&f.rt, 10)->valid);
    HH_ASSERT(!hh_routing_get(&f.rt, 11)->valid);

    hh_dispatcher_drain(&f.bus);
    HH_ASSERT_EQ_INT(o.withdrawals, 3);
    HH_ASSERT_EQ_INT(o.last_withdraw.reason, HH_WITHDRAW_FAILURE_CASCADE);
}

static void test_two_phase_invalidation_deletes_after_grace_window(void)
{
    fix_t f; obs_t o; fix_init(&f, &o);
    add_neighbor(&f, 2, -45.0f);
    hh_routing_offer(&f.rt, 9, 2, 20, 2, 0.0f, f.vc.now);

    hh_routing_invalidate_via(&f.rt, 2, HH_WITHDRAW_FAILURE_CASCADE, f.vc.now);
    /* Phase 1: invalid immediately, but the entry still exists. */
    HH_ASSERT(hh_routing_get(&f.rt, 9) != NULL);
    HH_ASSERT(!hh_routing_get(&f.rt, 9)->valid);
    HH_ASSERT_EQ_INT(hh_routing_count(&f.rt), 1);

    /* Still present partway through the grace window. */
    vclock_advance(&f.vc, f.cfg.route_delete_period_ms / 2);
    hh_routing_tick(&f.rt, f.vc.now);
    HH_ASSERT(hh_routing_get(&f.rt, 9) != NULL);

    /* Phase 2: deleted after DELETE_PERIOD elapses. */
    vclock_advance(&f.vc, f.cfg.route_delete_period_ms);
    hh_routing_tick(&f.rt, f.vc.now);
    HH_ASSERT(hh_routing_get(&f.rt, 9) == NULL);
    HH_ASSERT_EQ_INT(hh_routing_count(&f.rt), 0);
}

static void test_live_next_hop_keeps_route_from_ageing_out(void)
{
    fix_t f; obs_t o; fix_init(&f, &o);
    add_neighbor(&f, 2, -45.0f);
    hh_routing_offer(&f.rt, 9, 2, 20, 2, 0.0f, f.vc.now);

    /* While the next hop remains a live neighbor the proactive update stream is
     * maintaining this route, so it is not idle even with no data traffic over
     * it. Ageing it out here would expire good routes on a quiet network. */
    for (int i = 0; i < 5; i++) {
        vclock_advance(&f.vc, f.cfg.route_active_timeout_ms);
        hh_routing_tick(&f.rt, f.vc.now);
    }
    HH_ASSERT_MSG(hh_routing_get(&f.rt, 9)->valid,
                  "route aged out despite its next hop still being a neighbor");
    hh_dispatcher_drain(&f.bus);
    HH_ASSERT_EQ_INT(o.withdrawals, 0);
}

static void test_route_expires_once_next_hop_is_gone(void)
{
    fix_t f; obs_t o; fix_init(&f, &o);
    add_neighbor(&f, 2, -45.0f);
    hh_routing_offer(&f.rt, 9, 2, 20, 2, 0.0f, f.vc.now);

    /* Remove the neighbor without cascading invalidation, leaving the route
     * stranded. ACTIVE_ROUTE_TIMEOUT exists for exactly this case. */
    hh_neighbor_remove(&f.nm, 2, f.vc.now);
    vclock_advance(&f.vc, f.cfg.route_active_timeout_ms + 1);
    hh_routing_tick(&f.rt, f.vc.now);

    HH_ASSERT(!hh_routing_get(&f.rt, 9)->valid);
    hh_dispatcher_drain(&f.bus);
    HH_ASSERT_EQ_INT(o.last_withdraw.reason, HH_WITHDRAW_EXPIRED);
}

static void test_degraded_link_loses_to_healthy_alternate_via_metric(void)
{
    fix_t f; obs_t o; fix_init(&f, &o);
    hh_link_sample_t bad;

    add_neighbor(&f, 2, -50.0f);
    add_neighbor(&f, 3, -50.0f);
    hh_routing_offer(&f.rt, 9, 2, 20, 2, 0.0f, f.vc.now);

    /* Drive neighbor 2's link into Degraded. */
    memset(&bad, 0, sizeof bad);
    bad.neighbor_id = 2; bad.rssi = -92.0f; bad.snr = 18.0f; bad.per = 0.05f;
    for (int i = 0; i < 20; i++) hh_link_health_on_sample(&f.lh, &bad, f.vc.now);
    HH_ASSERT_EQ_INT(hh_link_health_state(&f.lh, 2), HH_LINK_DEGRADED);

    /* Doc 1 §8: degradation is handled inside the metric — the degraded link
     * simply loses the comparison, with no invalidation and no event storm. */
    vclock_advance(&f.vc, f.cfg.hold_down_ms + 1);
    HH_ASSERT_OK(hh_routing_offer(&f.rt, 9, 3, 20, 2, 0.0f, f.vc.now));
    HH_ASSERT_EQ_INT(hh_routing_get(&f.rt, 9)->next_hop, 3);
    hh_dispatcher_drain(&f.bus);
    HH_ASSERT_EQ_INT(o.withdrawals, 0);   /* no invalidation occurred */
}

static void test_dampened_next_hop_is_penalised(void)
{
    fix_t f; obs_t o; fix_init(&f, &o);
    add_neighbor(&f, 2, -50.0f);
    add_neighbor(&f, 3, -50.0f);

    HH_ASSERT(!hh_routing_is_damped(&f.rt, 2, f.vc.now));
    hh_routing_damp(&f.rt, 2, f.vc.now);
    HH_ASSERT(hh_routing_is_damped(&f.rt, 2, f.vc.now));

    /* With identical links, the damped next hop must lose. */
    hh_routing_offer(&f.rt, 9, 2, 20, 2, 0.0f, f.vc.now);
    vclock_advance(&f.vc, f.cfg.hold_down_ms + 1);
    HH_ASSERT_OK(hh_routing_offer(&f.rt, 9, 3, 20, 2, 0.0f, f.vc.now));
    HH_ASSERT_EQ_INT(hh_routing_get(&f.rt, 9)->next_hop, 3);

    /* The penalty lifts when the cooldown expires. */
    vclock_advance(&f.vc, f.cfg.dampening_penalty_ms + 1);
    HH_ASSERT(!hh_routing_is_damped(&f.rt, 2, f.vc.now));
}

static void test_hop_count_limit_and_self_route_rejected(void)
{
    fix_t f; obs_t o; fix_init(&f, &o);
    add_neighbor(&f, 2, -50.0f);

    /* Never install a route to ourselves. */
    HH_ASSERT_ERR(hh_routing_offer(&f.rt, 1, 2, 20, 2, 0.0f, f.vc.now), HH_ERR_INVAL);
    /* Bounded hop count stops a count-to-infinity walk. */
    HH_ASSERT_ERR(hh_routing_offer(&f.rt, 9, 2, 20, f.cfg.max_hop_count, 0.0f, f.vc.now),
                  HH_ERR_INVAL);
}

static void test_snapshot_is_versioned_and_reflects_table(void)
{
    fix_t f; obs_t o; fix_init(&f, &o);
    const hh_route_snapshot_t *s0, *s1;
    uint32_t v0;

    add_neighbor(&f, 2, -50.0f);
    s0 = hh_routing_snapshot(&f.rt);
    HH_ASSERT(s0 != NULL);
    v0 = s0->version;

    hh_routing_offer(&f.rt, 9, 2, 20, 2, 0.0f, f.vc.now);
    s1 = hh_routing_snapshot(&f.rt);
    /* Every publish advances the version, so a reader can detect staleness. */
    HH_ASSERT(s1->version > v0);
    HH_ASSERT_EQ_INT(s1->count, 1);
    HH_ASSERT_EQ_INT(hh_route_lookup(s1, 9)->next_hop, 2);
}

static void test_snapshot_lookup_excludes_invalid_routes(void)
{
    fix_t f; obs_t o; fix_init(&f, &o);
    add_neighbor(&f, 2, -50.0f);
    hh_routing_offer(&f.rt, 9, 2, 20, 2, 0.0f, f.vc.now);
    HH_ASSERT(hh_route_lookup(hh_routing_snapshot(&f.rt), 9) != NULL);

    /* Once invalidated, forwarding must stop finding it immediately, before
     * the entry is actually deleted. */
    hh_routing_invalidate_via(&f.rt, 2, HH_WITHDRAW_FAILURE_CASCADE, f.vc.now);
    HH_ASSERT(hh_route_lookup(hh_routing_snapshot(&f.rt), 9) == NULL);
    HH_ASSERT(hh_route_lookup(hh_routing_snapshot(&f.rt), 999) == NULL);
}

static void test_older_snapshot_stays_consistent_after_republish(void)
{
    fix_t f; obs_t o; fix_init(&f, &o);
    const hh_route_snapshot_t *held;

    add_neighbor(&f, 2, -50.0f);
    hh_routing_offer(&f.rt, 9, 2, 20, 2, 0.0f, f.vc.now);

    /* A reader holding a snapshot must keep a coherent view even as the writer
     * publishes a new one — the fast-path invariant from Doc 1 §10. */
    held = hh_routing_snapshot(&f.rt);
    HH_ASSERT_EQ_INT(held->count, 1);

    hh_routing_offer(&f.rt, 10, 2, 20, 2, 0.0f, f.vc.now);
    HH_ASSERT_EQ_INT(held->count, 1);                       /* unchanged */
    HH_ASSERT_EQ_INT(hh_routing_snapshot(&f.rt)->count, 2); /* new one has both */
}

static void test_build_update_advertises_valid_routes_only(void)
{
    fix_t f; obs_t o; fix_init(&f, &o);
    hh_route_update_t u;

    add_neighbor(&f, 2, -50.0f);
    hh_routing_offer(&f.rt, 9,  2, 20, 2, 0.0f, f.vc.now);
    hh_routing_offer(&f.rt, 10, 2, 20, 3, 0.0f, f.vc.now);
    HH_ASSERT_EQ_INT(hh_routing_build_update(&f.rt, &u), 2);
    HH_ASSERT_EQ_INT(u.sender, 1);

    /* An invalidated route is advertised with an infinite hop count rather than
     * being dropped from the update: actively withdrawing it converges faster
     * than falling silent and waiting for neighbors to time it out. */
    hh_routing_invalidate_via(&f.rt, 2, HH_WITHDRAW_FAILURE_CASCADE, f.vc.now);
    HH_ASSERT_EQ_INT(hh_routing_build_update(&f.rt, &u), 2);
    for (uint8_t i = 0; i < u.count; i++)
        HH_ASSERT_EQ_INT(u.entries[i].hop_count, HH_HOP_INFINITY);
}

static void test_split_horizon_poisons_route_back_to_its_source(void)
{
    fix_t f; obs_t o; fix_init(&f, &o);
    hh_route_update_t u;
    bool found = false;

    add_neighbor(&f, 2, -45.0f);
    add_neighbor(&f, 3, -45.0f);
    /* Destination 9 was learned from neighbor 2. */
    hh_routing_offer(&f.rt, 9, 2, 20, 2, 0.0f, f.vc.now);

    /* Advertised back to node 2, it must be poisoned: telling 2 that we can
     * reach 9 through it is what creates a count-to-infinity loop. */
    hh_routing_build_update_for(&f.rt, 2, &u);
    for (uint8_t i = 0; i < u.count; i++) {
        if (u.entries[i].originator == 9) {
            found = true;
            HH_ASSERT_EQ_INT(u.entries[i].hop_count, HH_HOP_INFINITY);
        }
    }
    HH_ASSERT(found);

    /* To any other neighbor it is advertised normally. */
    found = false;
    hh_routing_build_update_for(&f.rt, 3, &u);
    for (uint8_t i = 0; i < u.count; i++) {
        if (u.entries[i].originator == 9) {
            found = true;
            HH_ASSERT_EQ_INT(u.entries[i].hop_count, 2);
        }
    }
    HH_ASSERT(found);
}

static void test_direct_neighbor_route_is_exempt_from_split_horizon(void)
{
    fix_t f; obs_t o; fix_init(&f, &o);
    hh_route_update_t u;
    bool found = false;

    add_neighbor(&f, 2, -45.0f);
    add_neighbor(&f, 3, -45.0f);
    hh_routing_on_neighbor_up(&f.rt, 2, f.vc.now);

    /* Our route to 2 comes from direct observation, not from an advertisement,
     * so it must still be advertised to every other neighbor. */
    hh_routing_build_update_for(&f.rt, 3, &u);
    for (uint8_t i = 0; i < u.count; i++)
        if (u.entries[i].originator == 2) {
            found = true;
            HH_ASSERT_EQ_INT(u.entries[i].hop_count, 1);
        }
    HH_ASSERT(found);
}

static void test_poisoned_reverse_withdraws_route(void)
{
    fix_t f; obs_t o; fix_init(&f, &o);
    add_neighbor(&f, 2, -45.0f);
    hh_routing_offer(&f.rt, 9, 2, 20, 2, 0.0f, f.vc.now);
    HH_ASSERT(hh_routing_get(&f.rt, 9)->valid);

    /* Neighbor 2 reports the destination unreachable. That withdraws our route
     * rather than being read as a merely very long path. */
    HH_ASSERT_ERR(hh_routing_offer(&f.rt, 9, 2, 21, HH_HOP_INFINITY, 0.0f, f.vc.now),
                  HH_ERR_AGAIN);
    HH_ASSERT(!hh_routing_get(&f.rt, 9)->valid);
    hh_dispatcher_drain(&f.bus);
    HH_ASSERT_EQ_INT(o.withdrawals, 1);
}

HH_TEST_MAIN_BEGIN("routing")
    HH_RUN(test_neighbor_up_installs_one_hop_route);
    HH_RUN(test_route_rejected_without_valid_next_hop_neighbor);
    HH_RUN(test_fresher_sequence_always_wins);
    HH_RUN(test_stale_sequence_rejected);
    HH_RUN(test_equal_sequence_better_metric_wins_after_holddown);
    HH_RUN(test_second_best_kept_as_warm_alternate);
    HH_RUN(test_invalidation_promotes_alternate_without_recompute);
    HH_RUN(test_invalidation_cascade_marks_invalid_when_no_alternate);
    HH_RUN(test_two_phase_invalidation_deletes_after_grace_window);
    HH_RUN(test_live_next_hop_keeps_route_from_ageing_out);
    HH_RUN(test_route_expires_once_next_hop_is_gone);
    HH_RUN(test_degraded_link_loses_to_healthy_alternate_via_metric);
    HH_RUN(test_dampened_next_hop_is_penalised);
    HH_RUN(test_hop_count_limit_and_self_route_rejected);
    HH_RUN(test_snapshot_is_versioned_and_reflects_table);
    HH_RUN(test_snapshot_lookup_excludes_invalid_routes);
    HH_RUN(test_older_snapshot_stays_consistent_after_republish);
    HH_RUN(test_build_update_advertises_valid_routes_only);
    HH_RUN(test_split_horizon_poisons_route_back_to_its_source);
    HH_RUN(test_direct_neighbor_route_is_exempt_from_split_horizon);
    HH_RUN(test_poisoned_reverse_withdraws_route);
HH_TEST_MAIN_END()
