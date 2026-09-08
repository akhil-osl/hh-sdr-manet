/*
 * Integration tests: the wiring between components.
 *
 * Each test drives one component's real output and asserts the downstream
 * component reacted, exercising the event-bus path rather than calling the
 * consumer directly. Two-node scenarios use the full node assembly, so the
 * radio abstraction -> networking stack path is covered end to end.
 */
#include "hh_test.h"
#include "netsim.h"
#include <string.h>

/* ---- Discovery -> Neighbor Manager ---- */
static void test_discovery_to_neighbor(void)
{
    netsim_t s;
    netsim_init(&s, 1);
    netsim_add_node(&s, 1);
    netsim_add_node(&s, 2);
    netsim_link_up(&s, 1, 2, -55.0f);
    HH_ASSERT_OK(netsim_start_all(&s));

    netsim_run(&s, 1000, 10);

    /* A beacon transmitted by node 2 travelled through the mock radio, through
     * Discovery validation, and into node 1's neighbor table. */
    HH_ASSERT(netsim_is_neighbor(&s, 1, 2));
    HH_ASSERT(netsim_is_neighbor(&s, 2, 1));
}

/* ---- Neighbor Manager -> Link Health ---- */
static void test_neighbor_to_link_health(void)
{
    netsim_t s;
    sim_node_t *n1;
    netsim_init(&s, 2);
    netsim_add_node(&s, 1);
    netsim_add_node(&s, 2);
    netsim_link_up(&s, 1, 2, -55.0f);
    netsim_start_all(&s);
    netsim_run(&s, 1000, 10);

    n1 = netsim_node(&s, 1);
    /* NeighborUp caused Link Health to begin tracking the link. */
    HH_ASSERT(hh_link_health_get(&n1->node.link_health, 2) != NULL);
    HH_ASSERT_EQ_INT(hh_link_health_state(&n1->node.link_health, 2), HH_LINK_HEALTHY);
}

/* ---- Neighbor Manager -> Routing ---- */
static void test_neighbor_to_routing(void)
{
    netsim_t s;
    netsim_init(&s, 3);
    netsim_add_node(&s, 1);
    netsim_add_node(&s, 2);
    netsim_link_up(&s, 1, 2, -55.0f);
    netsim_start_all(&s);
    netsim_run(&s, 1000, 10);

    /* A one-hop route was installed from the NeighborUp event. */
    HH_ASSERT(netsim_has_route(&s, 1, 2));
    HH_ASSERT_EQ_INT(netsim_next_hop(&s, 1, 2), 2);
}

/* ---- Link Health -> Failure Detection -> Self-Healing -> Routing ---- */
static void test_link_health_through_recovery_to_routing(void)
{
    netsim_t s;
    sim_node_t *n1;
    netsim_init(&s, 4);
    netsim_add_node(&s, 1);
    netsim_add_node(&s, 2);
    netsim_link_up(&s, 1, 2, -55.0f);
    netsim_start_all(&s);
    netsim_run(&s, 1000, 10);
    HH_ASSERT(netsim_has_route(&s, 1, 2));

    /* Sever the link: link health degrades, the failure detector confirms,
     * self-healing runs, and routing invalidates. */
    netsim_link_down(&s, 1, 2);
    netsim_run(&s, 4000, 10);

    n1 = netsim_node(&s, 1);
    HH_ASSERT(!netsim_has_route(&s, 1, 2));
    HH_ASSERT(n1->node.failure_detector.confirmations > 0 ||
              n1->node.neighbors.downs > 0);
}

/* ---- Routing -> Packet Forwarder ---- */
static void test_routing_to_forwarder(void)
{
    netsim_t s;
    sim_node_t *n1, *n2;
    uint8_t payload[] = { 0xAA, 0xBB };

    netsim_init(&s, 5);
    netsim_add_node(&s, 1);
    netsim_add_node(&s, 2);
    netsim_link_up(&s, 1, 2, -55.0f);
    netsim_start_all(&s);
    netsim_run(&s, 1000, 10);

    n1 = netsim_node(&s, 1);
    n2 = netsim_node(&s, 2);
    /* The forwarder used the route the routing engine published. */
    HH_ASSERT_OK(hh_node_send(&n1->node, 2, payload, sizeof payload, s.vc.now));
    HH_ASSERT_EQ_INT(n1->node.forwarder.forwarded, 1);

    netsim_run(&s, 200, 10);
    HH_ASSERT_EQ_INT(n2->node.forwarder.delivered_local, 1);
}

/* ---- Radio abstraction -> networking stack ---- */
static void test_radio_abstraction_drives_whole_stack(void)
{
    netsim_t s;
    sim_node_t *n1;
    netsim_init(&s, 6);
    netsim_add_node(&s, 1);
    netsim_add_node(&s, 2);
    netsim_link_up(&s, 1, 2, -55.0f);
    netsim_start_all(&s);
    netsim_run(&s, 1000, 10);

    n1 = netsim_node(&s, 1);
    /* Everything above was driven purely by frames crossing the abstract radio
     * interface: beacons out, beacons plus metrics in. */
    HH_ASSERT(n1->node.discovery.beacons_sent > 0);
    HH_ASSERT(n1->node.discovery.beacons_rx_accepted > 0);
    HH_ASSERT(n1->mock.tx_total > 0);
    HH_ASSERT(hh_neighbor_count(&n1->node.neighbors) == 1);
}

/* ---- Routing -> Topology (read model stays in step) ---- */
static void test_routing_to_topology(void)
{
    netsim_t s;
    sim_node_t *n1;
    netsim_init(&s, 7);
    netsim_add_node(&s, 1);
    netsim_add_node(&s, 2);
    netsim_add_node(&s, 3);
    netsim_link_up(&s, 1, 2, -55.0f);
    netsim_link_up(&s, 2, 3, -55.0f);
    netsim_start_all(&s);
    netsim_run(&s, 2500, 10);

    n1 = netsim_node(&s, 1);
    /* Topology learned node 3 from the route install event, not a second flood. */
    HH_ASSERT(hh_topology_node(&n1->node.topology, 3) != NULL);
    HH_ASSERT(hh_topology_node(&n1->node.topology, 3)->reachable);
    /* And it correctly distinguishes a neighbor from a multi-hop destination. */
    HH_ASSERT(hh_topology_node(&n1->node.topology, 2)->is_neighbor);
    HH_ASSERT(!hh_topology_node(&n1->node.topology, 3)->is_neighbor);
}

/* ---- Lifecycle ordering (SCA) ---- */
static void test_lifecycle_ordering_enforced(void)
{
    netsim_t s;
    sim_node_t *n;
    hh_config_t cfg;

    netsim_init(&s, 8);
    hh_config_defaults(&cfg);
    cfg.node_id = 1;
    cfg.log_level = HH_LOG_ERROR;
    netsim_add_node_cfg(&s, 1, &cfg);
    n = netsim_node(&s, 1);

    /* init() leaves it initialized; start() before configure() is rejected. */
    HH_ASSERT_EQ_INT(n->node.state, HH_NODE_INITIALIZED);
    HH_ASSERT_ERR(hh_node_start(&n->node), HH_ERR_STATE);

    HH_ASSERT_OK(hh_node_configure(&n->node, NULL));
    HH_ASSERT_EQ_INT(n->node.state, HH_NODE_CONFIGURED);
    HH_ASSERT_OK(hh_node_start(&n->node));
    HH_ASSERT_EQ_INT(n->node.state, HH_NODE_RUNNING);

    /* Ticking is only valid while running. */
    HH_ASSERT_OK(hh_node_tick(&n->node, s.vc.now));
    HH_ASSERT_OK(hh_node_stop(&n->node));
    HH_ASSERT_ERR(hh_node_tick(&n->node, s.vc.now), HH_ERR_STATE);

    /* stop() preserves configuration, so a restart needs no reconfigure. */
    HH_ASSERT_OK(hh_node_start(&n->node));
    HH_ASSERT_OK(hh_node_release(&n->node));
    HH_ASSERT_EQ_INT(n->node.state, HH_NODE_RELEASED);
}

/* ---- Adaptive cadence feedback loop (HTI-16) ---- */
static void test_link_health_drives_discovery_cadence(void)
{
    netsim_t s;
    sim_node_t *n1;
    uint32_t steady_interval;

    netsim_init(&s, 9);
    netsim_add_node(&s, 1);
    netsim_add_node(&s, 2);
    netsim_link_up(&s, 1, 2, -55.0f);
    netsim_start_all(&s);
    netsim_run(&s, 1000, 10);

    n1 = netsim_node(&s, 1);
    steady_interval = hh_discovery_interval(&n1->node.discovery);

    /* Degrade the link; instability must tighten the beacon cadence. */
    netsim_link_set_quality(&s, 1, 2, -93.0f, 4.0f, 0.4f);
    netsim_run(&s, 1500, 10);
    HH_ASSERT_MSG(hh_discovery_interval(&n1->node.discovery) < steady_interval,
                  "cadence %u did not tighten from %u",
                  hh_discovery_interval(&n1->node.discovery), steady_interval);
}

/*
 * The full pipeline must actually run on a link failure:
 * detection -> classification -> confirmation -> recovery.
 *
 * This exists because a demonstration run showed routes rerouting correctly
 * while the recovery counters stayed at zero: neighbor expiry was deleting the
 * neighbor before the link-health machine reached Failed, so the Failure
 * Detector never confirmed and Self-Healing never ran. Rerouting still happened
 * via the expiry cascade, which is why no existing test caught it.
 */
static void test_link_failure_drives_full_recovery_pipeline(void)
{
    netsim_t s;
    sim_node_t *n1;

    netsim_init(&s, 10);
    for (hh_node_id_t i = 1; i <= 4; i++) netsim_add_node(&s, i);
    netsim_link_up(&s, 1, 2, -50.0f);
    netsim_link_up(&s, 1, 3, -50.0f);
    netsim_link_up(&s, 2, 4, -50.0f);
    netsim_link_up(&s, 3, 4, -50.0f);
    netsim_start_all(&s);
    netsim_run(&s, 3000, 10);

    n1 = netsim_node(&s, 1);
    HH_ASSERT(netsim_has_route(&s, 1, 4));
    HH_ASSERT_EQ_INT(n1->node.failure_detector.confirmations, 0);

    /* Break the link node 1 is actually using. */
    netsim_link_down(&s, 1, netsim_next_hop(&s, 1, 4));
    netsim_run(&s, 5000, 10);

    /* Every stage of the pipeline must have run, not just the end result. */
    HH_ASSERT_MSG(n1->node.link_health.transitions >= 3,
        "link health did not walk Healthy->Degraded->Suspected->Failed (%llu transitions)",
        (unsigned long long)n1->node.link_health.transitions);
    HH_ASSERT_MSG(n1->node.failure_detector.confirmations >= 1,
        "failure was never confirmed; recovery pipeline was bypassed");
    HH_ASSERT_MSG(n1->node.self_healing.recoveries_started >= 1,
        "self-healing never ran despite a confirmed failure");

    /* And connectivity survived over the alternate path. */
    HH_ASSERT_MSG(netsim_has_route(&s, 1, 4), "lost connectivity after recovery");
}

HH_TEST_MAIN_BEGIN("integration")
    HH_RUN(test_discovery_to_neighbor);
    HH_RUN(test_neighbor_to_link_health);
    HH_RUN(test_neighbor_to_routing);
    HH_RUN(test_link_health_through_recovery_to_routing);
    HH_RUN(test_link_failure_drives_full_recovery_pipeline);
    HH_RUN(test_routing_to_forwarder);
    HH_RUN(test_radio_abstraction_drives_whole_stack);
    HH_RUN(test_routing_to_topology);
    HH_RUN(test_lifecycle_ordering_enforced);
    HH_RUN(test_link_health_drives_discovery_cadence);
HH_TEST_MAIN_END()
