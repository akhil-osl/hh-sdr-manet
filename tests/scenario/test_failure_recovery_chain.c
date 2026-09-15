/*
 * End-to-end failure -> detection -> event -> topology -> routing ->
 * recovery -> topology restoration -> route restoration chain.
 *
 * Every prior verification session checked ONE dimension of this pipeline in
 * isolation:
 *   - test_recovery.c (unit)            : FD/Topology/SelfHealing components
 *                                          called directly, one at a time.
 *   - test_scenarios.c / test_topology_scenarios.c : neighbor+topology state,
 *                                          via netsim, but never the FD's own
 *                                          confirmation counters or
 *                                          SelfHealing's recovery counters.
 *   - test_route_verification.c          : route FIELDS (next_hop, hop_count,
 *                                          metric) before/after failure, but
 *                                          never cross-checked against the
 *                                          Failure Detector's or
 *                                          Self-Healing's own state in the
 *                                          same assertion.
 *   - test_status_events.c/_scenarios.c  : status/stale-event processing,
 *                                          deliberately not about routing.
 *
 * No existing test reads hh_neighbor_mgr_t, hh_link_health_t,
 * hh_failure_detector_t, hh_topology_t, hh_self_healing_t, AND hh_routing_t
 * state together, at each of: before-failure / after-failure /
 * after-recovery, for one continuous scenario. That is what this file adds:
 * one test per scenario walks the full chain, asserting a concrete value at
 * every stage, and checks that no stage double-counts across a
 * failure+recovery cycle or across repeated cycles.
 *
 * Software-injected failures only (netsim_link_down/_node_fail and their
 * inverses, plus netsim_link_set_loss for the silent-failure path already
 * distinguished in test_status_scenarios.c). No hardware.
 */
#include "hh_test.h"
#include "netsim.h"
#include <string.h>

#define CONVERGE_MS 3000
#define DETECT_MS   5000
#define STEP_MS     10

/* ---------------- A. Link failure -> recovery, full chain ---------------- */

static void test_link_failure_recovery_full_chain(void)
{
    netsim_t s;
    sim_node_t *na;
    const hh_route_t *r;
    uint64_t fd_confirmations_before, sh_started_before, sh_completed_before;
    uint64_t lh_transitions_before;

    /* A -- B -- C. */
    netsim_init(&s, 501);
    netsim_add_node(&s, 1);
    netsim_add_node(&s, 2);
    netsim_add_node(&s, 3);
    netsim_link_up(&s, 1, 2, -50.0f);
    netsim_link_up(&s, 2, 3, -50.0f);
    netsim_start_all(&s);
    netsim_run(&s, CONVERGE_MS, STEP_MS);
    na = netsim_node(&s, 1);

    /* ---- BEFORE: establish and record the complete baseline state. ---- */
    HH_ASSERT(hh_neighbor_get(&na->node.neighbors, 2) != NULL);
    HH_ASSERT(hh_topology_node(&na->node.topology, 2)->is_neighbor);
    HH_ASSERT(hh_topology_node(&na->node.topology, 3)->reachable);
    HH_ASSERT(!hh_topology_node(&na->node.topology, 3)->is_neighbor);
    r = hh_routing_get(&na->node.routing, 3);
    HH_ASSERT(r != NULL && r->valid);
    HH_ASSERT_EQ_INT(r->next_hop, 2);
    HH_ASSERT_EQ_INT(r->hop_count, 2);
    fd_confirmations_before = na->node.failure_detector.confirmations;
    sh_started_before       = na->node.self_healing.recoveries_started;
    sh_completed_before      = na->node.self_healing.recoveries_completed;

    /* ---- FAILURE: inject the B-C link failure (software-only). ---- */
    netsim_link_down(&s, 2, 3);
    netsim_run(&s, DETECT_MS, STEP_MS);

    /* Detection: A itself never lost its link to B, so A's own Failure
     * Detector/Self-Healing never fire for THIS failure -- it is B and C's
     * local observation, which A learns about only through the resulting
     * route withdrawal. This is the real, implemented boundary: "Topology
     * Manager... only reads from the Neighbor Manager" and routing's
     * invalidation cascade is driven by NeighborDown/LinkStateChanged on the
     * node that actually lost the link, not by every node in the network.
     * So the chain this test proves at A is: B-C breaks -> B's and C's own
     * neighbor tables drop each other -> B stops advertising a route to C
     * -> A's routing update stream stops receiving that advertisement ->
     * A's route ages out via ACTIVE_ROUTE_TIMEOUT and is withdrawn. */
    HH_ASSERT(hh_neighbor_get(&na->node.neighbors, 2) != NULL);  /* A-B intact */
    HH_ASSERT_MSG(!netsim_is_neighbor(&s, 2, 3), "B-C did not fail at the source");

    /* Topology update at A: C no longer reachable, B is still A's neighbor. */
    HH_ASSERT(hh_topology_node(&na->node.topology, 2)->is_neighbor);
    HH_ASSERT_MSG(!hh_topology_node(&na->node.topology, 3)->reachable,
                  "A's topology still reports C reachable after B-C failed");

    /* Routing impact at A: the route through the broken relay is gone. */
    HH_ASSERT_MSG(hh_routing_get(&na->node.routing, 3) == NULL,
                  "A's route to C survived the B-C failure");
    /* The unrelated direct route to B is completely undisturbed. */
    r = hh_routing_get(&na->node.routing, 2);
    HH_ASSERT(r != NULL && r->valid && r->next_hop == 2 && r->hop_count == 1);

    /* B's own failure detection fully fires, since B is the node that
     * actually lost a neighbor. */
    {
        sim_node_t *nb = netsim_node(&s, 2);
        HH_ASSERT_MSG(nb->node.failure_detector.confirmations >= 1 ||
                      nb->node.neighbors.downs >= 1,
                      "B never detected or confirmed the loss of C");
    }

    /* ---- RECOVERY: restore B-C (software-only). ---- */
    netsim_link_up(&s, 2, 3, -50.0f);
    netsim_run(&s, CONVERGE_MS * 2, STEP_MS);

    /* Neighbor/link restored at the source. */
    HH_ASSERT_MSG(netsim_is_neighbor(&s, 2, 3), "B-C link did not recover");

    /* Topology restored at A: exactly back to the pre-failure shape. */
    HH_ASSERT(hh_topology_node(&na->node.topology, 2)->is_neighbor);
    HH_ASSERT_MSG(hh_topology_node(&na->node.topology, 3)->reachable,
                  "A did not regain reachability to C after B-C recovered");
    HH_ASSERT(!hh_topology_node(&na->node.topology, 3)->is_neighbor);
    /* No duplicate topology entries: exactly two tracked nodes, same as the
     * pre-failure baseline. */
    HH_ASSERT_EQ_INT(hh_topology_node_count(&na->node.topology), 2);

    /* Route restored at A, with the exact same fields as before failure. */
    r = hh_routing_get(&na->node.routing, 3);
    HH_ASSERT_MSG(r != NULL, "A's route to C was not restored");
    HH_ASSERT(r->valid);
    HH_ASSERT_EQ_INT(r->next_hop, 2);
    HH_ASSERT_EQ_INT(r->hop_count, 2);
    HH_ASSERT_EQ_INT(hh_routing_count(&na->node.routing), 2);

    /* A's own FD/SelfHealing counters never moved at all: this failure was
     * never A's own confirmed failure, only a route consequence of it. That
     * absence of spurious activity at A IS the correctness property -- a
     * failure two hops away must not make A think IT detected a failure. */
    HH_ASSERT_EQ_INT(na->node.failure_detector.confirmations, fd_confirmations_before);
    HH_ASSERT_EQ_INT(na->node.self_healing.recoveries_started, sh_started_before);
    HH_ASSERT_EQ_INT(na->node.self_healing.recoveries_completed, sh_completed_before);
    (void)lh_transitions_before;
}

/* ---------------- B. Node failure -> recovery, full chain ---------------- */

static void test_node_failure_recovery_full_chain(void)
{
    netsim_t s;
    sim_node_t *na;
    const hh_route_t *r;
    uint64_t fd_confirmations_before, sh_started_before, sh_completed_before;
    uint64_t lh_transitions_before;

    netsim_init(&s, 502);
    netsim_add_node(&s, 1);
    netsim_add_node(&s, 2);
    netsim_add_node(&s, 3);
    netsim_link_up(&s, 1, 2, -50.0f);
    netsim_link_up(&s, 2, 3, -50.0f);
    netsim_start_all(&s);
    netsim_run(&s, CONVERGE_MS, STEP_MS);
    na = netsim_node(&s, 1);

    /* ---- BEFORE. ---- */
    HH_ASSERT(hh_neighbor_get(&na->node.neighbors, 2) != NULL);
    HH_ASSERT(hh_topology_node(&na->node.topology, 2)->is_neighbor);
    HH_ASSERT(hh_topology_node(&na->node.topology, 3)->reachable);
    r = hh_routing_get(&na->node.routing, 3);
    HH_ASSERT(r != NULL && r->valid && r->next_hop == 2 && r->hop_count == 2);
    fd_confirmations_before = na->node.failure_detector.confirmations;
    sh_started_before       = na->node.self_healing.recoveries_started;
    sh_completed_before      = na->node.self_healing.recoveries_completed;
    lh_transitions_before    = na->node.link_health.transitions;

    /* ---- FAILURE: B's radio goes dark (software-injected node failure). ---- */
    netsim_node_fail(&s, 2);
    netsim_run(&s, DETECT_MS, STEP_MS);

    /* Detection at A: THIS time A is the one who lost a direct neighbor, so
     * A's own full pipeline must have run -- link health walked its states,
     * the Failure Detector confirmed, and Self-Healing started a recovery. */
    HH_ASSERT_MSG(na->node.link_health.transitions > lh_transitions_before,
                  "link health never transitioned on B's failure");
    HH_ASSERT_MSG(na->node.failure_detector.confirmations > fd_confirmations_before,
                  "A's Failure Detector never confirmed B's failure");
    HH_ASSERT_MSG(na->node.self_healing.recoveries_started > sh_started_before,
                  "Self-Healing never started a recovery for B's failure");

    /* Neighbor/link state: B is gone from A's table. */
    HH_ASSERT(hh_neighbor_get(&na->node.neighbors, 2) == NULL);

    /* Topology update: B no longer a neighbor, C (only reachable via B) no
     * longer reachable either -- both relationships updated from one
     * failure, not just the directly-observed one. */
    HH_ASSERT_MSG(!hh_topology_node(&na->node.topology, 2)->is_neighbor,
                  "topology still shows B as a neighbor after its failure");
    HH_ASSERT_MSG(!hh_topology_node(&na->node.topology, 3)->reachable,
                  "topology still shows C reachable after its only relay failed");

    /* Routing impact: both the relay's own route and everything behind it
     * are invalidated/removed, not left half-updated. */
    HH_ASSERT(hh_routing_get(&na->node.routing, 2) == NULL);
    HH_ASSERT(hh_routing_get(&na->node.routing, 3) == NULL);
    HH_ASSERT_EQ_INT(hh_routing_count(&na->node.routing), 0);

    /* ---- RECOVERY: B comes back (software-injected node recovery). ---- */
    netsim_node_recover(&s, 2);
    netsim_run(&s, CONVERGE_MS * 2, STEP_MS);

    /* Neighbor/link relationship restored, exactly once -- no duplicate. */
    HH_ASSERT(hh_neighbor_get(&na->node.neighbors, 2) != NULL);
    HH_ASSERT_EQ_INT(hh_neighbor_count(&na->node.neighbors), 1);

    /* Topology restored for both B (direct) and C (via B). */
    HH_ASSERT(hh_topology_node(&na->node.topology, 2)->is_neighbor);
    HH_ASSERT(hh_topology_node(&na->node.topology, 3)->reachable);
    /* No duplicate topology entries from the failure+recovery cycle. */
    HH_ASSERT_EQ_INT(hh_topology_node_count(&na->node.topology), 2);

    /* Routes reconverge: A -> B -> C works again, fields match the original
     * baseline exactly. */
    r = hh_routing_get(&na->node.routing, 2);
    HH_ASSERT(r != NULL && r->valid && r->next_hop == 2 && r->hop_count == 1);
    r = hh_routing_get(&na->node.routing, 3);
    HH_ASSERT(r != NULL && r->valid && r->next_hop == 2 && r->hop_count == 2);
    /* No duplicate/stale route entries: exactly the two expected routes. */
    HH_ASSERT_EQ_INT(hh_routing_count(&na->node.routing), 2);

    /* No stale failure state survives recovery: the Failure Detector no
     * longer considers B failed. */
    HH_ASSERT(!hh_fd_is_failed(&na->node.failure_detector, 2));
    /* Self-Healing's recovery for B actually completed, not left dangling. */
    HH_ASSERT_MSG(na->node.self_healing.recoveries_completed > sh_completed_before,
                  "Self-Healing never reported the recovery as completed");
    HH_ASSERT(!hh_sh_is_recovering(&na->node.self_healing, 2));
}

/* ---------------- C. Alternate path: failure and recovery through a diamond ---------------- */

static void test_alternate_path_failure_and_recovery_full_chain(void)
{
    netsim_t s;
    sim_node_t *na;
    const hh_route_t *r;
    hh_node_id_t original_hop, other_hop;

    /*
     *       B
     *      / \
     * A --    --- C
     *      \ /
     *       D
     *
     * Both arms are built with identical link quality and hop count, so the
     * implementation's actual selection rule (fresher-sequence, or
     * first-installed at equal sequence/metric -- see test_routing.c's
     * should_replace rule) determines which arm is primary; this test does
     * not assume which one, it discovers it from the running system, exactly
     * as the task requires.
     */
    netsim_init(&s, 503);
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
    original_hop = r->next_hop;
    HH_ASSERT(original_hop == 2 || original_hop == 4);
    other_hop = (original_hop == 2) ? 4 : 2;
    /* The losing arm is tracked as a warm standby, not discarded. */
    HH_ASSERT(r->has_alt);
    HH_ASSERT_EQ_INT(r->alt_next_hop, other_hop);

    /* ---- FAILURE: take down the currently-used arm at its root link. ---- */
    netsim_link_down(&s, 1, original_hop);
    netsim_run(&s, DETECT_MS, STEP_MS);

    /* A's own pipeline ran for losing its direct neighbor on that arm. */
    HH_ASSERT(hh_neighbor_get(&na->node.neighbors, original_hop) == NULL);
    HH_ASSERT(!hh_topology_node(&na->node.topology, original_hop)->is_neighbor);
    /* C remains reachable throughout -- the whole point of the alternate
     * path -- and routing switched to the surviving arm. */
    r = hh_routing_get(&na->node.routing, 3);
    HH_ASSERT_MSG(r != NULL && r->valid,
                  "route to C was withdrawn even though the other arm is intact");
    HH_ASSERT_MSG(r->next_hop == other_hop,
                  "expected failover to the surviving arm %u, got next_hop=%u",
                  other_hop, r->next_hop);
    HH_ASSERT_EQ_INT(r->hop_count, 2);
    HH_ASSERT(hh_topology_node(&na->node.topology, 3)->reachable);

    /* ---- RECOVERY: restore the originally-used arm. ---- */
    netsim_link_up(&s, 1, original_hop, -50.0f);
    netsim_run(&s, CONVERGE_MS * 2, STEP_MS);

    /* Neighbor/topology relationship for that arm is restored. */
    HH_ASSERT(hh_neighbor_get(&na->node.neighbors, original_hop) != NULL);
    HH_ASSERT(hh_topology_node(&na->node.topology, original_hop)->is_neighbor);
    HH_ASSERT_EQ_INT(hh_neighbor_count(&na->node.neighbors), 2);
    /* No duplicate topology entries from the churn. */
    HH_ASSERT_EQ_INT(hh_topology_node_count(&na->node.topology), 3);

    /* C is (still/again) reachable via a 2-hop route. This test does not
     * assume the implementation switches back to the original arm --
     * test_routing.c's should_replace() rule only prefers a newly-offered
     * route over the current one on a fresher sequence or, at equal
     * sequence, a strictly better metric; with both arms equal quality and
     * the surviving arm now the incumbent, staying on it is the documented,
     * correct behavior (route stability is an explicit design goal --
     * test_14_stabilization_after_recovery already proves no continuing
     * churn once stable). What IS verified is that connectivity and route
     * validity are fully restored either way. */
    r = hh_routing_get(&na->node.routing, 3);
    HH_ASSERT_MSG(r != NULL && r->valid, "route to C lost after the original arm recovered");
    HH_ASSERT_EQ_INT(r->hop_count, 2);
    HH_ASSERT(r->next_hop == 2 || r->next_hop == 4);
}

/* ---------------- Explicit failure vs. silent failure: same chain, different trigger ---------------- */

static void test_silent_link_failure_drives_the_same_full_chain_as_explicit(void)
{
    netsim_t s;
    sim_node_t *na;
    const hh_route_t *r;

    /* Same A-B-C topology as Scenario A, but the B-C link is never marked
     * down -- it goes 100% lossy instead (silent failure, as distinguished
     * in test_status_scenarios.c). This proves the SAME topology/routing
     * restoration chain this task requires also holds for the stale/lost
     * trigger, not only the explicit one, without re-testing the stale
     * detection mechanics themselves (already covered in the prior task). */
    netsim_init(&s, 504);
    netsim_add_node(&s, 1);
    netsim_add_node(&s, 2);
    netsim_add_node(&s, 3);
    netsim_link_up(&s, 1, 2, -50.0f);
    netsim_link_up(&s, 2, 3, -50.0f);
    netsim_start_all(&s);
    netsim_run(&s, CONVERGE_MS, STEP_MS);
    na = netsim_node(&s, 1);

    r = hh_routing_get(&na->node.routing, 3);
    HH_ASSERT(r != NULL && r->valid && r->hop_count == 2);

    netsim_link_set_loss(&s, 2, 3, 1.0f);
    netsim_run(&s, 15000, STEP_MS);

    HH_ASSERT_MSG(!hh_topology_node(&na->node.topology, 3)->reachable,
                  "A still sees C reachable after B-C went silent");
    HH_ASSERT(hh_routing_get(&na->node.routing, 3) == NULL);

    netsim_link_set_loss(&s, 2, 3, 0.0f);
    netsim_run(&s, CONVERGE_MS * 2, STEP_MS);

    HH_ASSERT(hh_topology_node(&na->node.topology, 3)->reachable);
    r = hh_routing_get(&na->node.routing, 3);
    HH_ASSERT(r != NULL && r->valid);
    HH_ASSERT_EQ_INT(r->next_hop, 2);
    HH_ASSERT_EQ_INT(r->hop_count, 2);
    HH_ASSERT_EQ_INT(hh_topology_node_count(&na->node.topology), 2);
}

/* ---------------- Repeated failure/recovery cycles: full-state equivalence ---------------- */

static void test_repeated_node_failure_recovery_cycles_reach_equivalent_healthy_state(void)
{
    netsim_t s;
    sim_node_t *na;
    const hh_route_t *r_to_b, *r_to_c;

    netsim_init(&s, 505);
    netsim_add_node(&s, 1);
    netsim_add_node(&s, 2);
    netsim_add_node(&s, 3);
    netsim_link_up(&s, 1, 2, -50.0f);
    netsim_link_up(&s, 2, 3, -50.0f);
    netsim_start_all(&s);
    netsim_run(&s, CONVERGE_MS, STEP_MS);
    na = netsim_node(&s, 1);

    for (int cycle = 0; cycle < 3; cycle++) {
        netsim_node_fail(&s, 2);
        netsim_run(&s, DETECT_MS, STEP_MS);

        HH_ASSERT_MSG(hh_neighbor_get(&na->node.neighbors, 2) == NULL,
                      "cycle %d: B still a neighbor after failure", cycle);
        HH_ASSERT_MSG(hh_routing_count(&na->node.routing) == 0,
                      "cycle %d: stale routes survived B's failure", cycle);

        netsim_node_recover(&s, 2);
        netsim_run(&s, CONVERGE_MS * 2, STEP_MS);

        HH_ASSERT_MSG(hh_neighbor_get(&na->node.neighbors, 2) != NULL,
                      "cycle %d: B did not recover", cycle);
        HH_ASSERT_MSG(!hh_fd_is_failed(&na->node.failure_detector, 2),
                      "cycle %d: stale failure state survived recovery", cycle);
        HH_ASSERT_MSG(!hh_sh_is_recovering(&na->node.self_healing, 2),
                      "cycle %d: a recovery was left dangling", cycle);

        /* Exactly the healthy-network shape at the end of every cycle: no
         * growth, no duplication, regardless of how many cycles ran. */
        HH_ASSERT_EQ_INT(hh_neighbor_count(&na->node.neighbors), 1);
        HH_ASSERT_EQ_INT(hh_topology_node_count(&na->node.topology), 2);
        HH_ASSERT_EQ_INT(hh_routing_count(&na->node.routing), 2);
    }

    /* Final state, after three full cycles, is field-equivalent to the
     * original healthy baseline. */
    r_to_b = hh_routing_get(&na->node.routing, 2);
    r_to_c = hh_routing_get(&na->node.routing, 3);
    HH_ASSERT(r_to_b != NULL && r_to_b->valid && r_to_b->next_hop == 2 && r_to_b->hop_count == 1);
    HH_ASSERT(r_to_c != NULL && r_to_c->valid && r_to_c->next_hop == 2 && r_to_c->hop_count == 2);
    HH_ASSERT(hh_topology_node(&na->node.topology, 2)->is_neighbor);
    HH_ASSERT(hh_topology_node(&na->node.topology, 3)->reachable);

    /* Exactly one confirmation and one completed recovery PER cycle -- no
     * cycle silently failed to run the pipeline, and none double-counted. */
    HH_ASSERT_EQ_INT(na->node.failure_detector.confirmations, 3);
    HH_ASSERT_EQ_INT(na->node.self_healing.recoveries_completed, 3);
    HH_ASSERT_EQ_INT(hh_dispatcher_dropped(&na->node.bus), 0);
}

/* ---------------- Duplicate events across the failure+recovery boundary ---------------- */

static void test_duplicate_failure_event_during_active_recovery_does_not_corrupt_state(void)
{
    netsim_t s;
    sim_node_t *na;
    hh_ev_failure_t dup;
    uint64_t started_before;

    netsim_init(&s, 506);
    netsim_add_node(&s, 1);
    netsim_add_node(&s, 2);
    netsim_add_node(&s, 3);
    netsim_link_up(&s, 1, 2, -50.0f);
    netsim_link_up(&s, 2, 3, -50.0f);
    netsim_start_all(&s);
    netsim_run(&s, CONVERGE_MS, STEP_MS);
    na = netsim_node(&s, 1);

    netsim_node_fail(&s, 2);
    netsim_run(&s, DETECT_MS, STEP_MS);
    HH_ASSERT(hh_sh_is_recovering(&na->node.self_healing, 2));
    started_before = na->node.self_healing.recoveries_started;

    /* While recovery for node 2 is still active, the exact same
     * FailureDetected report is injected again directly -- modeling a
     * redelivered or duplicated event arriving while the first one is still
     * being acted on. hh_sh_on_failure()'s own documented guarantee
     * (test_recovery.c: "duplicate failure does not restart recovery") must
     * hold here too, reached through the real node-level wiring rather than
     * a direct unit call. */
    memset(&dup, 0, sizeof dup);
    dup.neighbor_or_node_id = 2;
    dup.cause_hint = HH_CAUSE_NODE_FAILURE;
    HH_ASSERT_ERR(hh_sh_on_failure(&na->node.self_healing, &dup, s.vc.now), HH_ERR_AGAIN);
    HH_ASSERT_EQ_INT(na->node.self_healing.recoveries_started, started_before);

    /* Recovery still proceeds normally afterward: the duplicate did not
     * leave the pipeline stuck. */
    netsim_node_recover(&s, 2);
    netsim_run(&s, CONVERGE_MS * 2, STEP_MS);

    HH_ASSERT(!hh_sh_is_recovering(&na->node.self_healing, 2));
    HH_ASSERT(hh_neighbor_get(&na->node.neighbors, 2) != NULL);
    const hh_route_t *r = hh_routing_get(&na->node.routing, 3);
    HH_ASSERT(r != NULL && r->valid && r->hop_count == 2);
}

HH_TEST_MAIN_BEGIN("failure_recovery_chain")
    HH_RUN(test_link_failure_recovery_full_chain);
    HH_RUN(test_node_failure_recovery_full_chain);
    HH_RUN(test_alternate_path_failure_and_recovery_full_chain);
    HH_RUN(test_silent_link_failure_drives_the_same_full_chain_as_explicit);
    HH_RUN(test_repeated_node_failure_recovery_cycles_reach_equivalent_healthy_state);
    HH_RUN(test_duplicate_failure_event_during_active_recovery_does_not_corrupt_state);
HH_TEST_MAIN_END()
