/*
 * Node/link status-change and stale/lost-status scenarios, driven through the
 * real multi-node netsim pipeline.
 *
 * tests/unit/test_status_events.c injects synthetic events directly onto one
 * node's bus to isolate the node-level fan-out wiring. This file instead
 * drives status change the way it actually happens end to end -- real beacon
 * traffic, real radios, real virtual time -- and is specifically about the
 * distinction netsim_link_down() cannot express: a link that goes SILENT
 * (100% loss, the link object itself never marked down, no explicit
 * "disconnected" signal to either side) versus a link that is explicitly torn
 * down. Every other scenario test in this suite (test_scenarios.c,
 * test_topology_scenarios.c, test_route_verification.c) only ever uses
 * netsim_link_down() for failure, which is instantaneous and unambiguous --
 * it does not exercise the stale/lost timeout path at all, only the
 * already-covered "told explicitly" path.
 *
 * netsim_link_set_loss(s, a, b, 1.0f) is the right tool here: the link stays
 * "up" in the simulator's own model (so no event fires purely from the
 * simulator), but every frame across it is dropped -- exactly modeling "no
 * status received" silence rather than an explicit down signal. No hardware.
 */
#include "hh_test.h"
#include "netsim.h"
#include "hhsdr/manet/telemetry.h"
#include <string.h>

#define CONVERGE_MS 3000
#define STEP_MS     10

static void test_total_silence_is_distinct_from_explicit_link_down(void)
{
    netsim_t s;
    sim_node_t *n1;

    /* A -- B, established normally. */
    netsim_init(&s, 401);
    netsim_add_node(&s, 1);
    netsim_add_node(&s, 2);
    netsim_link_up(&s, 1, 2, -55.0f);
    netsim_start_all(&s);
    netsim_run(&s, CONVERGE_MS, STEP_MS);

    n1 = netsim_node(&s, 1);
    HH_ASSERT_EQ_INT(hh_link_health_state(&n1->node.link_health, 2), HH_LINK_HEALTHY);

    /* Go fully silent WITHOUT netsim_link_down(): the link stays "up" in the
     * simulator, every frame drops, and -- unlike every other failure test in
     * this suite -- no explicit NeighborDown/LinkStateChanged is produced by
     * anything outside the node's own timeout processing. */
    netsim_link_set_loss(&s, 1, 2, 1.0f);
    /* Run well past the neighbor's cadence-based expiry deadline and the
     * link-health hold-down, so the silence has time to be detected purely
     * from the absence of beacons. */
    netsim_run(&s, 15000, STEP_MS);

    HH_ASSERT_MSG(hh_link_health_state(&n1->node.link_health, 2) == HH_LINK_FAILED,
                  "silent link (100%% loss, never explicitly torn down) did not "
                  "reach Failed: state=%s",
                  hh_link_state_str(hh_link_health_state(&n1->node.link_health, 2)));
    /* Stale status must not persist indefinitely: the neighbor itself is
     * eventually removed, same observable end state as an explicit failure. */
    HH_ASSERT_MSG(!netsim_is_neighbor(&s, 1, 2),
                  "neighbor with total silence was never expired");
    HH_ASSERT(n1->node.failure_detector.confirmations >= 1);
}

static void test_stale_link_recovers_once_traffic_resumes(void)
{
    netsim_t s;
    sim_node_t *n1;

    netsim_init(&s, 402);
    netsim_add_node(&s, 1);
    netsim_add_node(&s, 2);
    netsim_link_up(&s, 1, 2, -55.0f);
    netsim_start_all(&s);
    netsim_run(&s, CONVERGE_MS, STEP_MS);

    netsim_link_set_loss(&s, 1, 2, 1.0f);
    netsim_run(&s, 15000, STEP_MS);

    n1 = netsim_node(&s, 1);
    HH_ASSERT(!netsim_is_neighbor(&s, 1, 2));

    /* Traffic resumes -- not a link_up(), just loss dropping back to zero on
     * the same link object, modeling signal returning rather than a
     * brand-new connection event. Recovery must be ordinary rediscovery. */
    netsim_link_set_loss(&s, 1, 2, 0.0f);
    netsim_run(&s, CONVERGE_MS * 2, STEP_MS);

    HH_ASSERT_MSG(netsim_is_neighbor(&s, 1, 2),
                  "link did not recover once traffic resumed after silence");
    HH_ASSERT_EQ_INT(hh_link_health_state(&n1->node.link_health, 2), HH_LINK_HEALTHY);
    /* Exactly one rediscovery, not a duplicate: one UP before silence, one
     * DOWN on expiry, one UP on recovery. */
    HH_ASSERT_EQ_INT(n1->node.neighbors.ups, 2);
    HH_ASSERT_EQ_INT(n1->node.neighbors.downs, 1);
}

static void test_application_telemetry_reflects_stale_to_failed_transition(void)
{
    netsim_t s;
    sim_node_t *n1;
    hh_node_status_t before, after;

    /*
     * Application-visible behavior: hh_telemetry_node_status() is the
     * management-plane's read of node health (HTI-01). A real operator
     * console or automated monitor has no other way to observe this
     * transition than through telemetry's own counters, so this is what
     * "application behavior" means for this stack -- there is no GUI/Python
     * layer in scope for this task.
     */
    netsim_init(&s, 403);
    netsim_add_node(&s, 1);
    netsim_add_node(&s, 2);
    netsim_link_up(&s, 1, 2, -55.0f);
    netsim_start_all(&s);
    netsim_run(&s, CONVERGE_MS, STEP_MS);

    n1 = netsim_node(&s, 1);
    hh_telemetry_node_status(&n1->node, &before);
    HH_ASSERT_EQ_INT(before.neighbor_count, 1);
    HH_ASSERT_EQ_INT(before.failures_confirmed, 0);

    netsim_link_set_loss(&s, 1, 2, 1.0f);
    netsim_run(&s, 15000, STEP_MS);

    hh_telemetry_node_status(&n1->node, &after);
    HH_ASSERT_MSG(after.neighbor_count == 0,
                  "telemetry still reports a stale neighbor as present");
    HH_ASSERT_MSG(after.failures_confirmed >= 1,
                  "telemetry did not reflect the confirmed failure from silence");
    HH_ASSERT(after.neighbor_downs >= 1);
    /* events_dropped staying at zero proves the dispatcher kept up with the
     * whole detection pipeline; a nonzero value would mean the application
     * view could be missing events, not just slow to update. */
    HH_ASSERT_EQ_INT(after.events_dropped, 0);
}

static void test_repeated_silence_recovery_cycles_stay_consistent(void)
{
    netsim_t s;
    sim_node_t *n1;

    /* Two full silence -> recovery cycles on the same link, verifying the
     * neighbor table and link-health tracking stay exactly consistent --
     * the stale/lost path specifically, not netsim_link_down(), repeated. */
    netsim_init(&s, 404);
    netsim_add_node(&s, 1);
    netsim_add_node(&s, 2);
    netsim_link_up(&s, 1, 2, -55.0f);
    netsim_start_all(&s);
    netsim_run(&s, CONVERGE_MS, STEP_MS);
    n1 = netsim_node(&s, 1);

    for (int cycle = 0; cycle < 2; cycle++) {
        netsim_link_set_loss(&s, 1, 2, 1.0f);
        netsim_run(&s, 15000, STEP_MS);
        HH_ASSERT_MSG(!netsim_is_neighbor(&s, 1, 2), "cycle %d: did not go stale", cycle);

        netsim_link_set_loss(&s, 1, 2, 0.0f);
        netsim_run(&s, CONVERGE_MS * 2, STEP_MS);
        HH_ASSERT_MSG(netsim_is_neighbor(&s, 1, 2), "cycle %d: did not recover", cycle);
        HH_ASSERT_EQ_INT(hh_neighbor_count(&n1->node.neighbors), 1);
    }

    HH_ASSERT_EQ_INT(n1->node.neighbors.ups, 3);     /* initial + 2 recoveries */
    HH_ASSERT_EQ_INT(n1->node.neighbors.downs, 2);   /* 2 stale expirations    */
    HH_ASSERT_EQ_INT(hh_dispatcher_dropped(&n1->node.bus), 0);
}

HH_TEST_MAIN_BEGIN("status_scenarios")
    HH_RUN(test_total_silence_is_distinct_from_explicit_link_down);
    HH_RUN(test_stale_link_recovers_once_traffic_resumes);
    HH_RUN(test_application_telemetry_reflects_stale_to_failed_transition);
    HH_RUN(test_repeated_silence_recovery_cycles_stay_consistent);
HH_TEST_MAIN_END()
