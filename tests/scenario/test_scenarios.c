/*
 * Multi-node MANET scenarios (task §6, items 1-14).
 *
 * Every scenario runs the real production stack across virtual nodes and
 * asserts observable network behavior -- routes converging, next hops changing,
 * routes invalidating and returning -- rather than that functions executed.
 *
 * Timing is virtual and loss is seeded, so each scenario is reproducible.
 */
#include "hh_test.h"
#include "netsim.h"
#include <string.h>

/* Convergence budgets, in virtual ms. Generous relative to the configured
 * timers so a scenario asserts behavior, not a tuned race. */
#define CONVERGE_MS   3000
#define DETECT_MS     5000
#define STEP_MS       10

/* Build a line topology 1 -- 2 -- 3 ... n. */
static void build_line(netsim_t *s, size_t n, uint64_t seed)
{
    netsim_init(s, seed);
    for (hh_node_id_t i = 1; i <= (hh_node_id_t)n; i++) netsim_add_node(s, i);
    for (hh_node_id_t i = 1; i < (hh_node_id_t)n; i++)
        netsim_link_up(s, i, i + 1, -55.0f);
    netsim_start_all(s);
}

/* --- 1. Normal multi-node discovery and routing --- */
static void test_01_normal_discovery_and_routing(void)
{
    netsim_t s;
    build_line(&s, 4, 101);
    netsim_run(&s, CONVERGE_MS, STEP_MS);

    /* Each node discovers exactly its physical neighbors. */
    HH_ASSERT(netsim_is_neighbor(&s, 1, 2));
    HH_ASSERT(netsim_is_neighbor(&s, 2, 3));
    HH_ASSERT(netsim_is_neighbor(&s, 3, 4));
    HH_ASSERT_MSG(!netsim_is_neighbor(&s, 1, 3), "node 1 must not adopt a non-adjacent neighbor");

    /* And multi-hop routes form across the line, via the correct next hop. */
    HH_ASSERT_MSG(netsim_has_route(&s, 1, 3), "no route 1->3");
    HH_ASSERT_MSG(netsim_has_route(&s, 1, 4), "no route 1->4");
    HH_ASSERT_EQ_INT(netsim_next_hop(&s, 1, 3), 2);
    HH_ASSERT_EQ_INT(netsim_next_hop(&s, 1, 4), 2);
    HH_ASSERT_EQ_INT(netsim_next_hop(&s, 4, 1), 3);
}

/* --- 2. Link failure --- */
static void test_02_link_failure(void)
{
    netsim_t s;
    build_line(&s, 3, 102);
    netsim_run(&s, CONVERGE_MS, STEP_MS);
    HH_ASSERT(netsim_has_route(&s, 1, 3));

    netsim_link_down(&s, 1, 2);
    netsim_run(&s, DETECT_MS, STEP_MS);

    /* The neighbor is gone and every route through it is invalidated. */
    HH_ASSERT_MSG(!netsim_is_neighbor(&s, 1, 2), "failed neighbor still present");
    HH_ASSERT_MSG(!netsim_has_route(&s, 1, 3), "route through failed link survived");
    HH_ASSERT_MSG(!netsim_has_route(&s, 1, 2), "direct route to failed neighbor survived");
}

/* --- 3. Alternate-route selection --- */
static void test_03_alternate_route_selection(void)
{
    netsim_t s;
    hh_node_id_t first_hop;

    /* Diamond: 1 reaches 4 via either 2 or 3. */
    netsim_init(&s, 103);
    for (hh_node_id_t i = 1; i <= 4; i++) netsim_add_node(&s, i);
    netsim_link_up(&s, 1, 2, -50.0f);
    netsim_link_up(&s, 1, 3, -50.0f);
    netsim_link_up(&s, 2, 4, -50.0f);
    netsim_link_up(&s, 3, 4, -50.0f);
    netsim_start_all(&s);
    netsim_run(&s, CONVERGE_MS, STEP_MS);

    HH_ASSERT(netsim_has_route(&s, 1, 4));
    first_hop = netsim_next_hop(&s, 1, 4);
    HH_ASSERT(first_hop == 2 || first_hop == 3);

    /* Break the chosen path; traffic must reroute over the other arm. */
    netsim_link_down(&s, 1, first_hop);
    netsim_run(&s, DETECT_MS, STEP_MS);

    HH_ASSERT_MSG(netsim_has_route(&s, 1, 4),
                  "no alternate route to 4 after %u failed", first_hop);
    HH_ASSERT_MSG(netsim_next_hop(&s, 1, 4) != first_hop,
                  "still routing through the failed next hop");
}

/* --- 4. Node failure --- */
static void test_04_node_failure(void)
{
    netsim_t s;
    build_line(&s, 3, 104);
    netsim_run(&s, CONVERGE_MS, STEP_MS);
    HH_ASSERT(netsim_has_route(&s, 1, 3));

    /* Node 2's radio goes dark: from 1 and 3's local view it simply vanishes. */
    netsim_node_fail(&s, 2);
    netsim_run(&s, DETECT_MS, STEP_MS);

    HH_ASSERT_MSG(!netsim_is_neighbor(&s, 1, 2), "failed node still a neighbor of 1");
    HH_ASSERT_MSG(!netsim_is_neighbor(&s, 3, 2), "failed node still a neighbor of 3");
    /* The route it relayed is gone from both sides. */
    HH_ASSERT(!netsim_has_route(&s, 1, 3));
    HH_ASSERT(!netsim_has_route(&s, 3, 1));
}

/* --- 5. Link degradation --- */
static void test_05_link_degradation(void)
{
    netsim_t s;
    sim_node_t *n1;

    netsim_init(&s, 105);
    netsim_add_node(&s, 1);
    netsim_add_node(&s, 2);
    netsim_link_up(&s, 1, 2, -50.0f);
    netsim_start_all(&s);
    netsim_run(&s, CONVERGE_MS, STEP_MS);

    n1 = netsim_node(&s, 1);
    HH_ASSERT_EQ_INT(hh_link_health_state(&n1->node.link_health, 2), HH_LINK_HEALTHY);

    /* Degrade one signal only: the link must degrade but NOT be torn down,
     * since no single signal may move a link past Degraded (Doc 1 §7). */
    netsim_link_set_quality(&s, 1, 2, -90.0f, 22.0f, 0.02f);
    netsim_run(&s, 2000, STEP_MS);

    HH_ASSERT_EQ_INT(hh_link_health_state(&n1->node.link_health, 2), HH_LINK_DEGRADED);
    HH_ASSERT_MSG(netsim_is_neighbor(&s, 1, 2), "degraded link was wrongly torn down");
    HH_ASSERT_MSG(netsim_has_route(&s, 1, 2), "degraded link lost its route");
}

/* --- 6. Network partition --- */
static void test_06_network_partition(void)
{
    netsim_t s;
    hh_node_id_t a[] = { 1, 2 }, b[] = { 3, 4 };

    build_line(&s, 4, 106);
    netsim_run(&s, CONVERGE_MS, STEP_MS);
    HH_ASSERT(netsim_has_route(&s, 1, 4));

    /* Sever the network into {1,2} and {3,4}. */
    netsim_partition(&s, a, 2, b, 2);
    netsim_run(&s, DETECT_MS, STEP_MS);

    /* No cross-partition route survives on stale information. */
    HH_ASSERT_MSG(!netsim_has_route(&s, 1, 4), "cross-partition route survived");
    HH_ASSERT_MSG(!netsim_has_route(&s, 4, 1), "cross-partition route survived");
    /* Each partition keeps operating fully within itself. */
    HH_ASSERT_MSG(netsim_has_route(&s, 1, 2), "intra-partition route lost");
    HH_ASSERT_MSG(netsim_has_route(&s, 3, 4), "intra-partition route lost");
}

/* --- 7. Network merge / rejoin --- */
static void test_07_network_merge(void)
{
    netsim_t s;
    hh_node_id_t a[] = { 1, 2 }, b[] = { 3, 4 };

    build_line(&s, 4, 107);
    netsim_run(&s, CONVERGE_MS, STEP_MS);
    netsim_partition(&s, a, 2, b, 2);
    netsim_run(&s, DETECT_MS, STEP_MS);
    HH_ASSERT(!netsim_has_route(&s, 1, 4));

    /* Restore the severed edge: ordinary discovery rejoins the halves. */
    netsim_link_up(&s, 2, 3, -55.0f);
    netsim_run(&s, CONVERGE_MS * 2, STEP_MS);

    HH_ASSERT_MSG(netsim_is_neighbor(&s, 2, 3), "severed edge did not reconnect");
    HH_ASSERT_MSG(netsim_has_route(&s, 1, 4), "route not restored after merge");
    HH_ASSERT_MSG(netsim_has_route(&s, 4, 1), "reverse route not restored after merge");
}

/* --- 8. Node recovery --- */
static void test_08_node_recovery(void)
{
    netsim_t s;
    build_line(&s, 3, 108);
    netsim_run(&s, CONVERGE_MS, STEP_MS);

    netsim_node_fail(&s, 2);
    netsim_run(&s, DETECT_MS, STEP_MS);
    HH_ASSERT(!netsim_has_route(&s, 1, 3));

    /* The node returns; rejoin is not special-cased, it is ordinary discovery. */
    netsim_node_recover(&s, 2);
    netsim_run(&s, CONVERGE_MS * 2, STEP_MS);

    HH_ASSERT_MSG(netsim_is_neighbor(&s, 1, 2), "recovered node not rediscovered");
    HH_ASSERT_MSG(netsim_has_route(&s, 1, 3), "route not reinstalled after recovery");
}

/* --- 9. Route invalidation and reinstallation --- */
static void test_09_route_invalidation_and_reinstallation(void)
{
    netsim_t s;
    build_line(&s, 3, 109);
    netsim_run(&s, CONVERGE_MS, STEP_MS);
    HH_ASSERT(netsim_has_route(&s, 1, 3));

    netsim_link_down(&s, 2, 3);
    netsim_run(&s, DETECT_MS, STEP_MS);
    HH_ASSERT_MSG(!netsim_has_route(&s, 1, 3), "route not invalidated");

    /* Restoring the link reinstalls the route through the normal update stream. */
    netsim_link_up(&s, 2, 3, -55.0f);
    netsim_run(&s, CONVERGE_MS * 2, STEP_MS);
    HH_ASSERT_MSG(netsim_has_route(&s, 1, 3), "route not reinstalled");
    HH_ASSERT_EQ_INT(netsim_next_hop(&s, 1, 3), 2);
}

/* --- 10. Rapid repeated link failures/recoveries --- */
static void test_10_rapid_flapping(void)
{
    netsim_t s;
    sim_node_t *n1;

    build_line(&s, 3, 110);
    netsim_run(&s, CONVERGE_MS, STEP_MS);
    n1 = netsim_node(&s, 1);

    /* Flap the link repeatedly. The stack must stay coherent: bounded tables,
     * no runaway growth, and it must still converge afterwards. */
    for (int i = 0; i < 6; i++) {
        netsim_link_down(&s, 1, 2);
        netsim_run(&s, 600, STEP_MS);
        netsim_link_up(&s, 1, 2, -55.0f);
        netsim_run(&s, 600, STEP_MS);
    }
    HH_ASSERT(hh_neighbor_count(&n1->node.neighbors) <= 2);
    HH_ASSERT(hh_routing_count(&n1->node.routing) <= n1->node.cfg.max_routes);
    HH_ASSERT_EQ_INT(hh_dispatcher_dropped(&n1->node.bus), 0);

    /* Given a stable period, it reconverges. */
    netsim_run(&s, CONVERGE_MS * 2, STEP_MS);
    HH_ASSERT_MSG(netsim_is_neighbor(&s, 1, 2), "did not reconverge after flapping");
    HH_ASSERT_MSG(netsim_has_route(&s, 1, 3), "route not restored after flapping");
}

/* --- 11. Packet loss --- */
static void test_11_packet_loss(void)
{
    netsim_t s;
    build_line(&s, 3, 111);
    /* 30% loss on every link: discovery and routing must still converge, since
     * the allowed-loss count tolerates missed beacons rather than tearing the
     * neighbor down on the first miss. */
    netsim_link_set_loss(&s, 1, 2, 0.30f);
    netsim_link_set_loss(&s, 2, 3, 0.30f);
    netsim_run(&s, CONVERGE_MS * 3, STEP_MS);

    HH_ASSERT_MSG(netsim_is_neighbor(&s, 1, 2), "lossy link prevented discovery");
    HH_ASSERT_MSG(netsim_has_route(&s, 1, 3), "lossy link prevented route convergence");
    HH_ASSERT(s.frames_dropped > 0);   /* loss really was applied */
}

/* --- 12. High-latency conditions --- */
static void test_12_high_latency(void)
{
    netsim_t s;
    build_line(&s, 3, 112);
    netsim_link_set_delay(&s, 1, 2, 150);
    netsim_link_set_delay(&s, 2, 3, 150);
    netsim_run(&s, CONVERGE_MS * 3, STEP_MS);

    /* Delay must not be mistaken for loss: the links stay up and converge. */
    HH_ASSERT_MSG(netsim_is_neighbor(&s, 1, 2), "delay broke discovery");
    HH_ASSERT_MSG(netsim_has_route(&s, 1, 3), "delay broke route convergence");
}

/* --- 13. Multiple simultaneous failures --- */
static void test_13_multiple_simultaneous_failures(void)
{
    netsim_t s;
    sim_node_t *n1;

    /* Node 1 has three neighbors; a fourth node sits behind node 2. */
    netsim_init(&s, 113);
    for (hh_node_id_t i = 1; i <= 5; i++) netsim_add_node(&s, i);
    netsim_link_up(&s, 1, 2, -50.0f);
    netsim_link_up(&s, 1, 3, -50.0f);
    netsim_link_up(&s, 1, 4, -50.0f);
    netsim_link_up(&s, 2, 5, -50.0f);
    netsim_start_all(&s);
    netsim_run(&s, CONVERGE_MS, STEP_MS);

    n1 = netsim_node(&s, 1);
    HH_ASSERT_EQ_INT(hh_neighbor_count(&n1->node.neighbors), 3);
    HH_ASSERT(netsim_has_route(&s, 1, 5));

    /* Two neighbors fail at once, one by link and one by node death. */
    netsim_link_down(&s, 1, 3);
    netsim_node_fail(&s, 2);
    netsim_run(&s, DETECT_MS, STEP_MS);

    /* Both are removed, the surviving neighbor is untouched, and the route
     * that depended on the dead relay is gone. */
    HH_ASSERT(!netsim_is_neighbor(&s, 1, 3));
    HH_ASSERT(!netsim_is_neighbor(&s, 1, 2));
    HH_ASSERT_MSG(netsim_is_neighbor(&s, 1, 4), "unaffected neighbor was lost");
    HH_ASSERT_MSG(!netsim_has_route(&s, 1, 5), "route via dead relay survived");
    HH_ASSERT_EQ_INT(hh_dispatcher_dropped(&n1->node.bus), 0);
}

/* --- 14. Network stabilization after recovery --- */
static void test_14_stabilization_after_recovery(void)
{
    netsim_t s;
    sim_node_t *n1;
    uint64_t installs_before;
    hh_node_id_t hop_before;

    build_line(&s, 4, 114);
    netsim_run(&s, CONVERGE_MS, STEP_MS);

    netsim_link_down(&s, 2, 3);
    netsim_run(&s, DETECT_MS, STEP_MS);
    netsim_link_up(&s, 2, 3, -55.0f);
    netsim_run(&s, CONVERGE_MS * 2, STEP_MS);

    n1 = netsim_node(&s, 1);
    HH_ASSERT(netsim_has_route(&s, 1, 4));
    hop_before = netsim_next_hop(&s, 1, 4);
    installs_before = n1->node.routing.installs;

    /* Once stable, the network must settle: no continuing route churn and no
     * next-hop oscillation while nothing changes. */
    netsim_run(&s, CONVERGE_MS * 2, STEP_MS);
    HH_ASSERT_EQ_INT(netsim_next_hop(&s, 1, 4), hop_before);
    HH_ASSERT_MSG(n1->node.routing.installs - installs_before < 40,
                  "route churn continued after stabilization: %llu installs",
                  (unsigned long long)(n1->node.routing.installs - installs_before));
    HH_ASSERT_EQ_INT(hh_dispatcher_dropped(&n1->node.bus), 0);
}

HH_TEST_MAIN_BEGIN("scenarios")
    HH_RUN(test_01_normal_discovery_and_routing);
    HH_RUN(test_02_link_failure);
    HH_RUN(test_03_alternate_route_selection);
    HH_RUN(test_04_node_failure);
    HH_RUN(test_05_link_degradation);
    HH_RUN(test_06_network_partition);
    HH_RUN(test_07_network_merge);
    HH_RUN(test_08_node_recovery);
    HH_RUN(test_09_route_invalidation_and_reinstallation);
    HH_RUN(test_10_rapid_flapping);
    HH_RUN(test_11_packet_loss);
    HH_RUN(test_12_high_latency);
    HH_RUN(test_13_multiple_simultaneous_failures);
    HH_RUN(test_14_stabilization_after_recovery);
HH_TEST_MAIN_END()
