/*
 * Route calculation, installation, update, and removal -- verified end to end
 * through the real discovery/beacon/route-update wire pipeline in netsim.
 *
 * tests/unit/test_routing.c already exercises hh_routing_offer() and friends
 * directly and exhaustively (selection rule, two-phase invalidation, warm
 * alternates, split horizon, dampening). What it cannot show is that the SAME
 * guarantees hold once routes are learned the real way: from beacons through
 * Discovery and Neighbor Manager, and from HH_FRAME_ROUTING updates built by
 * hh_routing_build_update_for() and decoded by handle_route_update() in
 * node.c. tests/scenario/test_scenarios.c already runs many such multi-node
 * scenarios, but every assertion there goes through the thin
 * netsim_has_route()/netsim_next_hop() helpers -- never hop_count, metric,
 * alt_next_hop/has_alt, or exact route-table size. This file closes that gap:
 * every test here reads the real hh_route_t fields via hh_routing_get(), or
 * the published hh_route_snapshot_t via hh_routing_snapshot(), on the actual
 * sim_node_t.node.routing of a real hh_node_t.
 *
 * No hardware. Deterministic virtual time and seeded loss, same as every
 * other scenario test in this suite.
 */
#include "hh_test.h"
#include "netsim.h"
#include <string.h>

#define CONVERGE_MS 3000
#define DETECT_MS   5000
#define STEP_MS     10

/* ---------------- Route calculation ---------------- */

static void test_direct_route_fields_are_correct(void)
{
    netsim_t s;
    sim_node_t *na;
    const hh_route_t *r;

    netsim_init(&s, 301);
    netsim_add_node(&s, 1);
    netsim_add_node(&s, 2);
    netsim_link_up(&s, 1, 2, -50.0f);
    netsim_start_all(&s);
    netsim_run(&s, CONVERGE_MS, STEP_MS);

    na = netsim_node(&s, 1);
    r = hh_routing_get(&na->node.routing, 2);
    HH_ASSERT(r != NULL);
    HH_ASSERT_EQ_INT(r->destination, 2);
    HH_ASSERT_EQ_INT(r->next_hop, 2);
    HH_ASSERT_EQ_INT(r->hop_count, 1);
    HH_ASSERT(r->valid);
    /* A direct neighbor's own route is not learned from an advertisement. */
    HH_ASSERT_EQ_INT(r->learned_from, HH_NODE_ID_INVALID);

    /* The published snapshot the forwarder actually reads agrees. */
    {
        const hh_route_snapshot_t *snap = hh_routing_snapshot(&na->node.routing);
        const hh_route_entry_t *e = hh_route_lookup(snap, 2);
        HH_ASSERT(e != NULL);
        HH_ASSERT_EQ_INT(e->next_hop, 2);
        HH_ASSERT_EQ_INT(e->hop_count, 1);
        HH_ASSERT_EQ_INT(snap->count, 1);
    }
}

static void test_multi_hop_route_fields_are_correct(void)
{
    netsim_t s;
    sim_node_t *na;
    const hh_route_t *r;

    /* A -- B -- C. */
    netsim_init(&s, 302);
    netsim_add_node(&s, 1);
    netsim_add_node(&s, 2);
    netsim_add_node(&s, 3);
    netsim_link_up(&s, 1, 2, -50.0f);
    netsim_link_up(&s, 2, 3, -50.0f);
    netsim_start_all(&s);
    netsim_run(&s, CONVERGE_MS, STEP_MS);

    na = netsim_node(&s, 1);
    r = hh_routing_get(&na->node.routing, 3);
    HH_ASSERT(r != NULL);
    HH_ASSERT_EQ_INT(r->destination, 3);
    /* Calculated through B, not directly to C. */
    HH_ASSERT_EQ_INT(r->next_hop, 2);
    HH_ASSERT_EQ_INT(r->hop_count, 2);
    HH_ASSERT(r->valid);
    /* Learned from an advertisement relayed by B, matching the next hop. */
    HH_ASSERT_EQ_INT(r->learned_from, 2);

    /* A's route table holds exactly two entries: B (direct) and C (via B). */
    HH_ASSERT_EQ_INT(hh_routing_count(&na->node.routing), 2);
}

static void test_metric_prefers_better_quality_path_over_equal_hop_alternate(void)
{
    netsim_t s;
    sim_node_t *na;
    const hh_route_t *r;

    /*
     *       B (good link both sides)
     *      / \
     * A --     --- D
     *      \ /
     *       C (poor link both sides)
     *
     * Both arms are 2 hops; the composite metric's dominant term is link
     * quality (metric_w_quality=1.0 vs metric_w_hop=0.15, hh_config.c
     * defaults), so A must prefer the path through the healthier arm B.
     */
    netsim_init(&s, 303);
    for (hh_node_id_t i = 1; i <= 4; i++) netsim_add_node(&s, i);
    netsim_link_up(&s, 1, 2, -45.0f);    /* A-B: strong    */
    netsim_link_up(&s, 2, 4, -45.0f);    /* B-D: strong    */
    netsim_link_up(&s, 1, 3, -90.0f);    /* A-C: weak      */
    netsim_link_up(&s, 3, 4, -90.0f);    /* C-D: weak      */
    netsim_start_all(&s);
    netsim_run(&s, CONVERGE_MS * 2, STEP_MS);

    na = netsim_node(&s, 1);
    r = hh_routing_get(&na->node.routing, 4);
    HH_ASSERT(r != NULL);
    HH_ASSERT(r->valid);
    HH_ASSERT_EQ_INT(r->hop_count, 2);
    HH_ASSERT_MSG(r->next_hop == 2,
                  "expected the better-quality arm (via 2), got next_hop=%u metric=%.4f",
                  r->next_hop, (double)r->metric);

    /* The weaker arm survives as the warm standby, not discarded outright. */
    HH_ASSERT(r->has_alt);
    HH_ASSERT_EQ_INT(r->alt_next_hop, 3);
}

/* ---------------- Route installation ---------------- */

static void test_route_install_does_not_disturb_unrelated_routes(void)
{
    netsim_t s;
    sim_node_t *na;
    const hh_route_t *r_to_b;
    uint64_t installs_to_b_before;

    /* A has an established direct route to B. A third node C then joins
     * behind B; installing the new route to C must not re-install or alter
     * the existing, unrelated route to B. */
    netsim_init(&s, 304);
    netsim_add_node(&s, 1);
    netsim_add_node(&s, 2);
    netsim_add_node(&s, 3);
    netsim_link_up(&s, 1, 2, -50.0f);
    netsim_start_all(&s);
    netsim_run(&s, CONVERGE_MS, STEP_MS);

    na = netsim_node(&s, 1);
    r_to_b = hh_routing_get(&na->node.routing, 2);
    HH_ASSERT(r_to_b != NULL);
    HH_ASSERT_EQ_INT(r_to_b->next_hop, 2);
    installs_to_b_before = na->node.routing.installs;

    netsim_link_up(&s, 2, 3, -50.0f);
    netsim_run(&s, CONVERGE_MS, STEP_MS);

    HH_ASSERT(hh_routing_get(&na->node.routing, 3) != NULL);
    /* The existing route to B is exactly as it was: same next hop, still
     * valid, table grew by exactly one entry. */
    r_to_b = hh_routing_get(&na->node.routing, 2);
    HH_ASSERT(r_to_b != NULL);
    HH_ASSERT(r_to_b->valid);
    HH_ASSERT_EQ_INT(r_to_b->next_hop, 2);
    HH_ASSERT_EQ_INT(hh_routing_count(&na->node.routing), 2);
    /* installs is a cumulative counter across all destinations, so it is
     * allowed to grow from C's install, but the table size proves B's entry
     * was not torn down and rebuilt as a distinct event. */
    HH_ASSERT(na->node.routing.installs > installs_to_b_before);
}

static void test_repeated_identical_route_updates_do_not_duplicate_entries(void)
{
    netsim_t s;
    sim_node_t *na;

    /*
     * A direct neighbor's route comes from hh_routing_on_neighbor_up(), not
     * from the HH_FRAME_ROUTING advertisement stream -- and with only two
     * nodes, B has nothing else to advertise to A (split horizon/self
     * filtering leave the update empty). A third node C, reached only
     * through B's advertisements, is what actually exercises the repeated-
     * identical-update path this test means to cover.
     *
     * Letting this converged three-node network run for a long time resends
     * many identical periodic route updates for C
     * (hh_routing_build_update_for). None of them may duplicate the
     * route-table entry for C.
     */
    netsim_init(&s, 305);
    netsim_add_node(&s, 1);
    netsim_add_node(&s, 2);
    netsim_add_node(&s, 3);
    netsim_link_up(&s, 1, 2, -50.0f);
    netsim_link_up(&s, 2, 3, -50.0f);
    netsim_start_all(&s);
    netsim_run(&s, CONVERGE_MS * 4, STEP_MS);

    na = netsim_node(&s, 1);
    HH_ASSERT(na->node.route_updates_rx > 5);
    HH_ASSERT_EQ_INT(hh_routing_count(&na->node.routing), 2);
    HH_ASSERT_EQ_INT(hh_routing_snapshot(&na->node.routing)->count, 2);
    HH_ASSERT_EQ_INT(hh_routing_get(&na->node.routing, 3)->next_hop, 2);
}

/* ---------------- Route update on topology change ---------------- */

static void test_route_updates_when_redundant_path_introduced(void)
{
    netsim_t s;
    sim_node_t *na;
    const hh_route_t *r;
    hh_node_id_t hop_via_line;

    /*
     * Initial:  A -- B -- C        (route A->C: next hop B, hop_count 2)
     * Then add: A -- D -- C        (a second, independent 2-hop arm)
     *
     * Verify routing reacts to the new path appearing (it remains valid to
     * either keep the incumbent at equal metric/hop-count, per the
     * freshness-then-metric selection rule already verified in
     * test_routing.c) and that the obsolete single-path state -- no
     * alternate recorded -- is updated to reflect the new alternate.
     */
    netsim_init(&s, 306);
    for (hh_node_id_t i = 1; i <= 4; i++) netsim_add_node(&s, i);
    netsim_link_up(&s, 1, 2, -50.0f);   /* A-B */
    netsim_link_up(&s, 2, 3, -50.0f);   /* B-C */
    netsim_start_all(&s);
    netsim_run(&s, CONVERGE_MS, STEP_MS);

    na = netsim_node(&s, 1);
    r = hh_routing_get(&na->node.routing, 3);
    HH_ASSERT(r != NULL);
    HH_ASSERT_EQ_INT(r->hop_count, 2);
    hop_via_line = r->next_hop;
    HH_ASSERT_EQ_INT(hop_via_line, 2);
    /* No alternate exists yet: only one path to C is known. */
    HH_ASSERT(!r->has_alt);

    /* Introduce the redundant arm A-D-C. */
    netsim_link_up(&s, 1, 4, -50.0f);   /* A-D */
    netsim_link_up(&s, 4, 3, -50.0f);   /* D-C */
    netsim_run(&s, CONVERGE_MS * 2, STEP_MS);

    r = hh_routing_get(&na->node.routing, 3);
    HH_ASSERT(r != NULL);
    HH_ASSERT(r->valid);
    /* The route to C is still exactly 2 hops (neither arm is shorter) and the
     * obsolete "no alternate" state has been updated: the second arm is now
     * recorded as a warm standby rather than being silently dropped. */
    HH_ASSERT_EQ_INT(r->hop_count, 2);
    HH_ASSERT(r->has_alt);
    HH_ASSERT_MSG(r->alt_next_hop == 2 || r->alt_next_hop == 4,
                  "alt_next_hop=%u is neither known arm", r->alt_next_hop);
    HH_ASSERT(r->alt_next_hop != r->next_hop);
}

/* ---------------- Route removal / withdrawal ---------------- */

static void test_route_withdrawn_leaves_no_stale_next_hop(void)
{
    netsim_t s;
    sim_node_t *na;

    /* A -- B -- C. Break B-C: A's route to C must be fully gone, not merely
     * marked and forgotten about -- hh_routing_get must return NULL, not a
     * stale/invalid entry callers might misread. */
    netsim_init(&s, 307);
    netsim_add_node(&s, 1);
    netsim_add_node(&s, 2);
    netsim_add_node(&s, 3);
    netsim_link_up(&s, 1, 2, -50.0f);
    netsim_link_up(&s, 2, 3, -50.0f);
    netsim_start_all(&s);
    netsim_run(&s, CONVERGE_MS, STEP_MS);

    na = netsim_node(&s, 1);
    HH_ASSERT(hh_routing_get(&na->node.routing, 3) != NULL);

    netsim_link_down(&s, 2, 3);
    netsim_run(&s, DETECT_MS, STEP_MS);

    /* Phase 1 (immediate): the snapshot forwarding reads must exclude it
     * at once, even before the grace-window delete. */
    HH_ASSERT(hh_route_lookup(hh_routing_snapshot(&na->node.routing), 3) == NULL);

    /* Phase 2 (after the grace window + a tick): the entry itself is gone,
     * not just marked invalid -- no stale next-hop bookkeeping lingers. */
    netsim_run(&s, na->node.cfg.route_delete_period_ms + 1000, STEP_MS);
    HH_ASSERT(hh_routing_get(&na->node.routing, 3) == NULL);

    /* The unrelated direct route to B is completely unaffected. */
    const hh_route_t *r_to_b = hh_routing_get(&na->node.routing, 2);
    HH_ASSERT(r_to_b != NULL);
    HH_ASSERT(r_to_b->valid);
    HH_ASSERT_EQ_INT(r_to_b->next_hop, 2);
}

static void test_route_withdrawal_does_not_affect_other_destinations(void)
{
    netsim_t s;
    sim_node_t *na;

    /* A has routes to both B (direct) and D (via B, a 2-hop fan-out). Losing
     * the B-C edge (C unrelated to either) must not disturb either route. */
    netsim_init(&s, 308);
    for (hh_node_id_t i = 1; i <= 4; i++) netsim_add_node(&s, i);
    netsim_link_up(&s, 1, 2, -50.0f);   /* A-B */
    netsim_link_up(&s, 2, 3, -50.0f);   /* B-C */
    netsim_link_up(&s, 2, 4, -50.0f);   /* B-D */
    netsim_start_all(&s);
    netsim_run(&s, CONVERGE_MS, STEP_MS);

    na = netsim_node(&s, 1);
    HH_ASSERT(hh_routing_get(&na->node.routing, 2) != NULL);
    HH_ASSERT(hh_routing_get(&na->node.routing, 3) != NULL);
    HH_ASSERT(hh_routing_get(&na->node.routing, 4) != NULL);

    netsim_link_down(&s, 2, 3);
    netsim_run(&s, DETECT_MS, STEP_MS);

    HH_ASSERT(hh_routing_get(&na->node.routing, 3) == NULL);
    /* B and D, unaffected by C's edge dropping, remain valid and unchanged. */
    const hh_route_t *r_b = hh_routing_get(&na->node.routing, 2);
    const hh_route_t *r_d = hh_routing_get(&na->node.routing, 4);
    HH_ASSERT(r_b != NULL && r_b->valid && r_b->next_hop == 2);
    HH_ASSERT(r_d != NULL && r_d->valid && r_d->next_hop == 2 && r_d->hop_count == 2);
}

/* ---------------- Route-change handling: representative transitions ---------------- */

static void test_current_next_hop_unavailable_triggers_replacement(void)
{
    netsim_t s;
    sim_node_t *na;
    hh_node_id_t original_hop;

    netsim_init(&s, 309);
    for (hh_node_id_t i = 1; i <= 4; i++) netsim_add_node(&s, i);
    netsim_link_up(&s, 1, 2, -45.0f);
    netsim_link_up(&s, 1, 3, -45.0f);
    netsim_link_up(&s, 2, 4, -45.0f);
    netsim_link_up(&s, 3, 4, -45.0f);
    netsim_start_all(&s);
    netsim_run(&s, CONVERGE_MS, STEP_MS);

    na = netsim_node(&s, 1);
    original_hop = hh_routing_get(&na->node.routing, 4)->next_hop;

    /* The current next hop itself becomes unreachable. */
    netsim_node_fail(&s, original_hop);
    netsim_run(&s, DETECT_MS, STEP_MS);

    const hh_route_t *r = hh_routing_get(&na->node.routing, 4);
    HH_ASSERT(r != NULL);
    HH_ASSERT(r->valid);
    HH_ASSERT_MSG(r->next_hop != original_hop,
                  "route replacement did not occur: still via failed next hop %u",
                  original_hop);
}

static void test_route_withdrawn_when_no_alternate_exists(void)
{
    netsim_t s;
    sim_node_t *na;

    /* Pure line, no redundancy: when the only path's relay fails, the route
     * must be withdrawn outright, not left dangling or half-updated. */
    netsim_init(&s, 310);
    netsim_add_node(&s, 1);
    netsim_add_node(&s, 2);
    netsim_add_node(&s, 3);
    netsim_link_up(&s, 1, 2, -50.0f);
    netsim_link_up(&s, 2, 3, -50.0f);
    netsim_start_all(&s);
    netsim_run(&s, CONVERGE_MS, STEP_MS);

    na = netsim_node(&s, 1);
    HH_ASSERT(hh_routing_get(&na->node.routing, 3) != NULL);

    netsim_node_fail(&s, 2);
    netsim_run(&s, DETECT_MS, STEP_MS);

    HH_ASSERT(hh_routing_get(&na->node.routing, 3) == NULL);
    HH_ASSERT(hh_routing_get(&na->node.routing, 2) == NULL);
    HH_ASSERT_EQ_INT(hh_routing_count(&na->node.routing), 0);
}

/* ---------------- Failure and recovery scenarios ---------------- */

/* Scenario A: link failure and recovery on A--B--C, route-field level. */
static void test_scenario_a_link_failure_and_recovery(void)
{
    netsim_t s;
    sim_node_t *na;
    const hh_route_t *r;

    netsim_init(&s, 311);
    netsim_add_node(&s, 1);
    netsim_add_node(&s, 2);
    netsim_add_node(&s, 3);
    netsim_link_up(&s, 1, 2, -50.0f);
    netsim_link_up(&s, 2, 3, -50.0f);
    netsim_start_all(&s);
    netsim_run(&s, CONVERGE_MS, STEP_MS);

    r = hh_routing_get(&netsim_node(&s, 1)->node.routing, 3);
    HH_ASSERT(r != NULL && r->valid && r->next_hop == 2 && r->hop_count == 2);

    netsim_link_down(&s, 2, 3);
    netsim_run(&s, DETECT_MS, STEP_MS);

    na = netsim_node(&s, 1);
    HH_ASSERT(hh_routing_get(&na->node.routing, 3) == NULL);

    netsim_link_up(&s, 2, 3, -50.0f);
    netsim_run(&s, CONVERGE_MS * 2, STEP_MS);

    r = hh_routing_get(&na->node.routing, 3);
    HH_ASSERT(r != NULL);
    HH_ASSERT(r->valid);
    HH_ASSERT_EQ_INT(r->next_hop, 2);
    HH_ASSERT_EQ_INT(r->hop_count, 2);
}

/* Scenario B: node failure and recovery on A--B--C, route-field level. */
static void test_scenario_b_node_failure_and_recovery(void)
{
    netsim_t s;
    sim_node_t *na;
    const hh_route_t *r;

    netsim_init(&s, 312);
    netsim_add_node(&s, 1);
    netsim_add_node(&s, 2);
    netsim_add_node(&s, 3);
    netsim_link_up(&s, 1, 2, -50.0f);
    netsim_link_up(&s, 2, 3, -50.0f);
    netsim_start_all(&s);
    netsim_run(&s, CONVERGE_MS, STEP_MS);

    HH_ASSERT(hh_routing_get(&netsim_node(&s, 1)->node.routing, 3) != NULL);

    netsim_node_fail(&s, 2);
    netsim_run(&s, DETECT_MS, STEP_MS);

    na = netsim_node(&s, 1);
    /* Both the relay's own route and everything behind it are gone. */
    HH_ASSERT(hh_routing_get(&na->node.routing, 2) == NULL);
    HH_ASSERT(hh_routing_get(&na->node.routing, 3) == NULL);
    HH_ASSERT_EQ_INT(hh_routing_count(&na->node.routing), 0);

    netsim_node_recover(&s, 2);
    netsim_run(&s, CONVERGE_MS * 2, STEP_MS);

    r = hh_routing_get(&na->node.routing, 2);
    HH_ASSERT(r != NULL && r->valid && r->next_hop == 2 && r->hop_count == 1);
    r = hh_routing_get(&na->node.routing, 3);
    HH_ASSERT(r != NULL && r->valid && r->next_hop == 2 && r->hop_count == 2);
}

/* Scenario C: alternate path. B/D diamond; remove one arm, verify the
 * remaining arm keeps the route alive at the field level. */
static void test_scenario_c_alternate_path_survives_one_arm_removed(void)
{
    netsim_t s;
    sim_node_t *na;
    hh_node_id_t first_hop;
    const hh_route_t *r;

    netsim_init(&s, 313);
    for (hh_node_id_t i = 1; i <= 4; i++) netsim_add_node(&s, i);
    netsim_link_up(&s, 1, 2, -50.0f);   /* A-B */
    netsim_link_up(&s, 1, 4, -50.0f);   /* A-D */
    netsim_link_up(&s, 2, 3, -50.0f);   /* B-C */
    netsim_link_up(&s, 4, 3, -50.0f);   /* D-C */
    netsim_start_all(&s);
    netsim_run(&s, CONVERGE_MS, STEP_MS);

    na = netsim_node(&s, 1);
    r = hh_routing_get(&na->node.routing, 3);
    HH_ASSERT(r != NULL && r->valid);
    HH_ASSERT_EQ_INT(r->hop_count, 2);
    first_hop = r->next_hop;
    HH_ASSERT(first_hop == 2 || first_hop == 4);

    /* Remove the arm currently in use. */
    netsim_link_down(&s, 1, first_hop);
    netsim_run(&s, DETECT_MS, STEP_MS);

    r = hh_routing_get(&na->node.routing, 3);
    HH_ASSERT_MSG(r != NULL && r->valid,
                  "route to C withdrawn even though the other arm is intact");
    HH_ASSERT_MSG(r->next_hop != first_hop,
                  "still via the removed arm's next hop %u", first_hop);
    HH_ASSERT_EQ_INT(r->hop_count, 2);
}

HH_TEST_MAIN_BEGIN("route_verification")
    HH_RUN(test_direct_route_fields_are_correct);
    HH_RUN(test_multi_hop_route_fields_are_correct);
    HH_RUN(test_metric_prefers_better_quality_path_over_equal_hop_alternate);
    HH_RUN(test_route_install_does_not_disturb_unrelated_routes);
    HH_RUN(test_repeated_identical_route_updates_do_not_duplicate_entries);
    HH_RUN(test_route_updates_when_redundant_path_introduced);
    HH_RUN(test_route_withdrawn_leaves_no_stale_next_hop);
    HH_RUN(test_route_withdrawal_does_not_affect_other_destinations);
    HH_RUN(test_current_next_hop_unavailable_triggers_replacement);
    HH_RUN(test_route_withdrawn_when_no_alternate_exists);
    HH_RUN(test_scenario_a_link_failure_and_recovery);
    HH_RUN(test_scenario_b_node_failure_and_recovery);
    HH_RUN(test_scenario_c_alternate_path_survives_one_arm_removed);
HH_TEST_MAIN_END()
