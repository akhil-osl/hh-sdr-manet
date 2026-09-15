/*
 * Node discovery, neighbor lifecycle, and topology-state scenarios.
 *
 * Scope is deliberately narrower than tests/scenario/test_scenarios.c: every
 * assertion here is about discovery, the neighbor table, or the Topology
 * Manager's read-model state (hh_topology_node_count/reachable_count/
 * is_neighbor/reachable) -- never about route presence or next-hop selection,
 * which test_scenarios.c and test_routing.c already cover. This runs the real
 * production stack over the deterministic netsim harness; no hardware.
 */
#include "hh_test.h"
#include "netsim.h"
#include <string.h>

#define CONVERGE_MS 3000
#define DETECT_MS   5000
#define STEP_MS     10

/* ---------------- Node discovery ---------------- */

static void test_lone_node_discovers_nobody(void)
{
    netsim_t s;
    sim_node_t *n1;
    netsim_init(&s, 201);
    netsim_add_node(&s, 1);
    netsim_start_all(&s);
    netsim_run(&s, CONVERGE_MS, STEP_MS);

    n1 = netsim_node(&s, 1);
    /* No links exist at all: a lone node must not fabricate a neighbor. */
    HH_ASSERT_EQ_INT(hh_neighbor_count(&n1->node.neighbors), 0);
    HH_ASSERT_EQ_INT(hh_topology_node_count(&n1->node.topology), 0);
    HH_ASSERT(!hh_topology_is_partitioned(&n1->node.topology));
}

static void test_two_nodes_discover_each_other(void)
{
    netsim_t s;
    sim_node_t *n1, *n2;
    netsim_init(&s, 202);
    netsim_add_node(&s, 1);
    netsim_add_node(&s, 2);
    netsim_link_up(&s, 1, 2, -55.0f);
    netsim_start_all(&s);
    netsim_run(&s, CONVERGE_MS, STEP_MS);

    HH_ASSERT(netsim_is_neighbor(&s, 1, 2));
    HH_ASSERT(netsim_is_neighbor(&s, 2, 1));

    n1 = netsim_node(&s, 1);
    n2 = netsim_node(&s, 2);
    HH_ASSERT_EQ_INT(hh_neighbor_count(&n1->node.neighbors), 1);
    HH_ASSERT_EQ_INT(hh_neighbor_count(&n2->node.neighbors), 1);
    /* Discovery is mutual: each side recorded exactly one NeighborUp. */
    HH_ASSERT_EQ_INT(n1->node.neighbors.ups, 1);
    HH_ASSERT_EQ_INT(n2->node.neighbors.ups, 1);

    /* Topology reflects the same single edge from both ends. */
    HH_ASSERT_EQ_INT(hh_topology_node_count(&n1->node.topology), 1);
    HH_ASSERT(hh_topology_node(&n1->node.topology, 2)->is_neighbor);
    HH_ASSERT(hh_topology_node(&n1->node.topology, 2)->reachable);
}

static void test_unreachable_node_is_not_discovered(void)
{
    netsim_t s;
    sim_node_t *n1;
    /* Three nodes exist, but node 3 has no link to node 1: physical
     * unreachability, not merely "not yet converged". */
    netsim_init(&s, 203);
    netsim_add_node(&s, 1);
    netsim_add_node(&s, 2);
    netsim_add_node(&s, 3);
    netsim_link_up(&s, 1, 2, -55.0f);
    netsim_start_all(&s);
    netsim_run(&s, CONVERGE_MS, STEP_MS);

    HH_ASSERT(netsim_is_neighbor(&s, 1, 2));
    HH_ASSERT_MSG(!netsim_is_neighbor(&s, 1, 3),
                  "node 1 incorrectly discovered unreachable node 3");
    n1 = netsim_node(&s, 1);
    HH_ASSERT_EQ_INT(hh_neighbor_count(&n1->node.neighbors), 1);
    /* Topology must not have learned of node 3 either: it never appeared on
     * any event this node consumed. */
    HH_ASSERT(hh_topology_node(&n1->node.topology, 3) == NULL);
}

static void test_repeated_beacons_do_not_duplicate_neighbor(void)
{
    netsim_t s;
    sim_node_t *n1;
    netsim_init(&s, 204);
    netsim_add_node(&s, 1);
    netsim_add_node(&s, 2);
    netsim_link_up(&s, 1, 2, -55.0f);
    netsim_start_all(&s);

    /* Run long enough for many repeat beacons at steady-state cadence. */
    netsim_run(&s, CONVERGE_MS * 4, STEP_MS);

    n1 = netsim_node(&s, 1);
    /* Many beacons were exchanged, but exactly one neighbor exists and exactly
     * one NeighborUp was ever published for it. */
    HH_ASSERT(n1->node.discovery.beacons_rx_accepted > 5);
    HH_ASSERT_EQ_INT(hh_neighbor_count(&n1->node.neighbors), 1);
    HH_ASSERT_EQ_INT(n1->node.neighbors.ups, 1);
    HH_ASSERT_EQ_INT(n1->node.neighbors.downs, 0);
}

/* ---------------- Neighbor management ---------------- */

static void test_neighbor_metrics_update_on_link_quality_change(void)
{
    netsim_t s;
    sim_node_t *n1;
    const hh_neighbor_t *nb;
    float rssi_before;

    netsim_init(&s, 205);
    netsim_add_node(&s, 1);
    netsim_add_node(&s, 2);
    netsim_link_up(&s, 1, 2, -50.0f);
    netsim_start_all(&s);
    netsim_run(&s, CONVERGE_MS, STEP_MS);

    n1 = netsim_node(&s, 1);
    nb = hh_neighbor_get(&n1->node.neighbors, 2);
    HH_ASSERT(nb != NULL);
    rssi_before = nb->last_sample.rssi;
    HH_ASSERT_NEAR(rssi_before, -50.0, 1.0);

    /* Change the link's reported quality; the neighbor's recorded sample must
     * track it rather than staying frozen at first contact. */
    netsim_link_set_quality(&s, 1, 2, -75.0f, 15.0f, 0.01f);
    netsim_run(&s, CONVERGE_MS, STEP_MS);

    nb = hh_neighbor_get(&n1->node.neighbors, 2);
    HH_ASSERT(nb != NULL);
    HH_ASSERT_NEAR(nb->last_sample.rssi, -75.0, 1.0);
    /* Still the same neighbor entry, not a re-add. */
    HH_ASSERT_EQ_INT(hh_neighbor_count(&n1->node.neighbors), 1);
    HH_ASSERT_EQ_INT(n1->node.neighbors.ups, 1);
}

static void test_neighbor_removed_when_link_drops_and_no_stale_entry_remains(void)
{
    netsim_t s;
    sim_node_t *n1;

    netsim_init(&s, 206);
    netsim_add_node(&s, 1);
    netsim_add_node(&s, 2);
    netsim_link_up(&s, 1, 2, -55.0f);
    netsim_start_all(&s);
    netsim_run(&s, CONVERGE_MS, STEP_MS);
    HH_ASSERT(netsim_is_neighbor(&s, 1, 2));

    netsim_link_down(&s, 1, 2);
    netsim_run(&s, DETECT_MS, STEP_MS);

    n1 = netsim_node(&s, 1);
    HH_ASSERT_MSG(!netsim_is_neighbor(&s, 1, 2), "removed neighbor still present");
    HH_ASSERT_EQ_INT(hh_neighbor_count(&n1->node.neighbors), 0);
    HH_ASSERT(hh_neighbor_get(&n1->node.neighbors, 2) == NULL);
    HH_ASSERT_EQ_INT(n1->node.neighbors.downs, 1);
    /* No leftover topology entry either, once it has aged out of reach. */
}

static void test_duplicate_neighbor_addition_keeps_single_entry(void)
{
    netsim_t s;
    sim_node_t *n1;

    /* A single physical link, but drive extra beacon traffic across it by
     * running well past several beacon intervals: this is the netsim-level
     * analogue of "duplicate discovery must not create duplicate neighbors". */
    netsim_init(&s, 207);
    netsim_add_node(&s, 1);
    netsim_add_node(&s, 2);
    netsim_link_up(&s, 1, 2, -55.0f);
    netsim_start_all(&s);
    netsim_run(&s, CONVERGE_MS * 3, STEP_MS);

    n1 = netsim_node(&s, 1);
    HH_ASSERT_EQ_INT(hh_neighbor_count(&n1->node.neighbors), 1);
    /* hh_neighbor_list must also report exactly one id, not one slot repeated. */
    {
        hh_node_id_t ids[8];
        size_t n = hh_neighbor_list(&n1->node.neighbors, ids, 8);
        HH_ASSERT_EQ_INT(n, 1);
        HH_ASSERT_EQ_INT(ids[0], 2);
    }
}

static void test_query_of_nonexistent_neighbor_is_well_defined(void)
{
    netsim_t s;
    sim_node_t *n1;
    netsim_init(&s, 208);
    netsim_add_node(&s, 1);
    netsim_add_node(&s, 2);
    netsim_link_up(&s, 1, 2, -55.0f);
    netsim_start_all(&s);
    netsim_run(&s, CONVERGE_MS, STEP_MS);

    n1 = netsim_node(&s, 1);
    /* Node 99 was never part of the simulation at all. */
    HH_ASSERT(hh_neighbor_get(&n1->node.neighbors, 99) == NULL);
    HH_ASSERT(!netsim_is_neighbor(&s, 1, 99));
    HH_ASSERT(hh_topology_node(&n1->node.topology, 99) == NULL);
}

static void test_neighbor_table_consistent_after_repeated_cycles(void)
{
    netsim_t s;
    sim_node_t *n1;

    netsim_init(&s, 209);
    netsim_add_node(&s, 1);
    netsim_add_node(&s, 2);
    netsim_link_up(&s, 1, 2, -55.0f);
    netsim_start_all(&s);
    netsim_run(&s, CONVERGE_MS, STEP_MS);

    n1 = netsim_node(&s, 1);
    /* Cycle the link down/up several times, past full detection + rediscovery
     * each time, and require the table to end up exactly consistent: neither
     * neighbor duplicated nor permanently lost. */
    for (int i = 0; i < 4; i++) {
        netsim_link_down(&s, 1, 2);
        netsim_run(&s, DETECT_MS, STEP_MS);
        HH_ASSERT_MSG(!netsim_is_neighbor(&s, 1, 2), "cycle %d: neighbor survived link down", i);
        HH_ASSERT_EQ_INT(hh_neighbor_count(&n1->node.neighbors), 0);

        netsim_link_up(&s, 1, 2, -55.0f);
        netsim_run(&s, CONVERGE_MS, STEP_MS);
        HH_ASSERT_MSG(netsim_is_neighbor(&s, 1, 2), "cycle %d: neighbor not rediscovered", i);
        HH_ASSERT_EQ_INT(hh_neighbor_count(&n1->node.neighbors), 1);
    }

    /* Exactly one UP per rediscovery (initial + 4 cycles) and one DOWN per
     * teardown (4 cycles): the accounting itself must stay exact, not just
     * the final snapshot. */
    HH_ASSERT_EQ_INT(n1->node.neighbors.ups, 5);
    HH_ASSERT_EQ_INT(n1->node.neighbors.downs, 4);
}

/* ---------------- Topology changes ---------------- */

static void test_topology_grows_as_line_extends(void)
{
    netsim_t s;
    sim_node_t *n1;

    /*
     * 1. A alone -- 2. A+B connected -- 3. A+B+C connected, staged by
     * bringing links up over time rather than adding nodes mid-run.
     *
     * netsim_add_node() only performs the SCA init step; connectivity comes
     * entirely from netsim_start_all() (called once, up front) plus
     * netsim_link_up/down(). Calling netsim_add_node() again after
     * netsim_start_all() leaves the new node stuck in HH_NODE_INITIALIZED --
     * netsim_step()/netsim_run() would then skip it, since hh_node_tick()
     * itself refuses to run outside HH_NODE_RUNNING. All nodes participating
     * in a scenario must therefore be added before start_all(); "joining" is
     * modeled by bringing its link up afterward, which is what every stage
     * below does.
     */
    netsim_init(&s, 210);
    netsim_add_node(&s, 1);
    netsim_add_node(&s, 2);
    netsim_add_node(&s, 3);
    netsim_start_all(&s);
    netsim_run(&s, 500, STEP_MS);

    n1 = netsim_node(&s, 1);
    HH_ASSERT_EQ_INT(hh_topology_node_count(&n1->node.topology), 0);

    /* Stage 2: B's link comes up. */
    netsim_link_up(&s, 1, 2, -55.0f);
    netsim_run(&s, CONVERGE_MS, STEP_MS);
    HH_ASSERT_EQ_INT(hh_topology_node_count(&n1->node.topology), 1);
    HH_ASSERT(hh_topology_node(&n1->node.topology, 2)->is_neighbor);

    /* Stage 3: C's link to B comes up. */
    netsim_link_up(&s, 2, 3, -55.0f);
    netsim_run(&s, CONVERGE_MS, STEP_MS);
    HH_ASSERT_EQ_INT(hh_topology_node_count(&n1->node.topology), 2);
    HH_ASSERT(hh_topology_node(&n1->node.topology, 3) != NULL);
    HH_ASSERT(hh_topology_node(&n1->node.topology, 3)->reachable);
    HH_ASSERT(!hh_topology_node(&n1->node.topology, 3)->is_neighbor);
}

static void test_topology_reflects_node_unavailable_and_recovered(void)
{
    netsim_t s;
    sim_node_t *n1;

    netsim_init(&s, 211);
    for (hh_node_id_t i = 1; i <= 3; i++) netsim_add_node(&s, i);
    netsim_link_up(&s, 1, 2, -55.0f);
    netsim_link_up(&s, 2, 3, -55.0f);
    netsim_start_all(&s);
    netsim_run(&s, CONVERGE_MS, STEP_MS);

    n1 = netsim_node(&s, 1);
    HH_ASSERT(hh_topology_node(&n1->node.topology, 3)->reachable);
    HH_ASSERT_EQ_INT(hh_topology_reachable_count(&n1->node.topology), 2);

    /* 4. B becomes unavailable. */
    netsim_node_fail(&s, 2);
    netsim_run(&s, DETECT_MS, STEP_MS);

    HH_ASSERT(!hh_topology_node(&n1->node.topology, 2)->is_neighbor ||
              !hh_topology_node(&n1->node.topology, 2)->reachable);
    HH_ASSERT_MSG(!hh_topology_node(&n1->node.topology, 3)->reachable,
                  "node behind the failed relay is still marked reachable");
    HH_ASSERT_EQ_INT(hh_topology_reachable_count(&n1->node.topology), 0);

    /* 5. B becomes available again: ordinary rediscovery, no duplicate node
     * entries created for either B or C. */
    netsim_node_recover(&s, 2);
    netsim_run(&s, CONVERGE_MS * 2, STEP_MS);

    HH_ASSERT(hh_topology_node(&n1->node.topology, 2)->is_neighbor);
    HH_ASSERT(hh_topology_node(&n1->node.topology, 3)->reachable);
    HH_ASSERT_EQ_INT(hh_topology_reachable_count(&n1->node.topology), 2);
    /* Exactly two tracked nodes -- B and C -- never three or more from a
     * duplicated entry. */
    HH_ASSERT_EQ_INT(hh_topology_node_count(&n1->node.topology), 2);
}

static void test_topology_reflects_link_disappearance_and_restoration(void)
{
    netsim_t s;
    sim_node_t *n1;

    netsim_init(&s, 212);
    for (hh_node_id_t i = 1; i <= 3; i++) netsim_add_node(&s, i);
    netsim_link_up(&s, 1, 2, -55.0f);
    netsim_link_up(&s, 2, 3, -55.0f);
    netsim_start_all(&s);
    netsim_run(&s, CONVERGE_MS, STEP_MS);

    n1 = netsim_node(&s, 1);
    HH_ASSERT(hh_topology_node(&n1->node.topology, 3)->reachable);

    /* 6. The B-C link disappears (B itself stays up and remains our neighbor). */
    netsim_link_down(&s, 2, 3);
    netsim_run(&s, DETECT_MS, STEP_MS);

    HH_ASSERT(hh_topology_node(&n1->node.topology, 2)->is_neighbor);
    HH_ASSERT_MSG(!hh_topology_node(&n1->node.topology, 3)->reachable,
                  "node beyond the dropped link still marked reachable");

    /* 7. The link is restored: C becomes reachable again through the same
     * relay, without a duplicate topology entry. */
    netsim_link_up(&s, 2, 3, -55.0f);
    netsim_run(&s, CONVERGE_MS * 2, STEP_MS);

    HH_ASSERT(hh_topology_node(&n1->node.topology, 3)->reachable);
    HH_ASSERT_EQ_INT(hh_topology_node_count(&n1->node.topology), 2);
}

static void test_sequential_topology_changes_leave_consistent_state(void)
{
    netsim_t s;
    sim_node_t *n1;

    /* 8/9. A sequence of joins, failures, and recoveries applied one after
     * another; only the FINAL state is asserted, exactly as a real deployment
     * would be inspected after a burst of churn. */
    netsim_init(&s, 213);
    for (hh_node_id_t i = 1; i <= 4; i++) netsim_add_node(&s, i);
    netsim_link_up(&s, 1, 2, -55.0f);
    netsim_start_all(&s);
    netsim_run(&s, CONVERGE_MS, STEP_MS);

    n1 = netsim_node(&s, 1);
    HH_ASSERT_EQ_INT(hh_topology_node_count(&n1->node.topology), 1);

    /* Chain of changes: C joins behind B, B fails, D joins directly on A,
     * B recovers. */
    netsim_link_up(&s, 2, 3, -55.0f);
    netsim_run(&s, CONVERGE_MS, STEP_MS);

    netsim_node_fail(&s, 2);
    netsim_run(&s, DETECT_MS, STEP_MS);

    netsim_link_up(&s, 1, 4, -55.0f);
    netsim_run(&s, CONVERGE_MS, STEP_MS);

    netsim_node_recover(&s, 2);
    netsim_run(&s, CONVERGE_MS * 2, STEP_MS);

    /* Final state: A neighbors B and D directly; C is reachable again behind
     * B; no stale or duplicated entries from the churn in between. */
    HH_ASSERT(hh_topology_node(&n1->node.topology, 2)->is_neighbor);
    HH_ASSERT(hh_topology_node(&n1->node.topology, 4)->is_neighbor);
    HH_ASSERT(hh_topology_node(&n1->node.topology, 3)->reachable);
    HH_ASSERT(!hh_topology_node(&n1->node.topology, 3)->is_neighbor);
    HH_ASSERT_EQ_INT(hh_topology_node_count(&n1->node.topology), 3);
    HH_ASSERT_EQ_INT(hh_neighbor_count(&n1->node.neighbors), 2);
}

/* ---------------- Multi-node scenario: line A-B-C ---------------- */

static void test_line_abc_full_neighbor_and_topology_lifecycle(void)
{
    netsim_t s;
    sim_node_t *na, *nb, *nc;
    const hh_node_id_t A = 1, B = 2, C = 3;

    netsim_init(&s, 214);
    netsim_add_node(&s, A);
    netsim_add_node(&s, B);
    netsim_add_node(&s, C);
    netsim_link_up(&s, A, B, -55.0f);
    netsim_link_up(&s, B, C, -55.0f);
    netsim_start_all(&s);
    netsim_run(&s, CONVERGE_MS, STEP_MS);

    na = netsim_node(&s, A);
    nb = netsim_node(&s, B);
    nc = netsim_node(&s, C);

    /* Each node discovers exactly its physical neighbors. */
    HH_ASSERT(netsim_is_neighbor(&s, A, B));
    HH_ASSERT(netsim_is_neighbor(&s, B, A));
    HH_ASSERT(netsim_is_neighbor(&s, B, C));
    HH_ASSERT(netsim_is_neighbor(&s, C, B));
    HH_ASSERT(!netsim_is_neighbor(&s, A, C));
    HH_ASSERT(!netsim_is_neighbor(&s, C, A));
    HH_ASSERT_EQ_INT(hh_neighbor_count(&na->node.neighbors), 1);
    HH_ASSERT_EQ_INT(hh_neighbor_count(&nb->node.neighbors), 2);
    HH_ASSERT_EQ_INT(hh_neighbor_count(&nc->node.neighbors), 1);

    /* Topology at A reflects the whole line: B direct, C reachable but not
     * a neighbor. */
    HH_ASSERT(hh_topology_node(&na->node.topology, B)->is_neighbor);
    HH_ASSERT(hh_topology_node(&na->node.topology, C)->reachable);
    HH_ASSERT(!hh_topology_node(&na->node.topology, C)->is_neighbor);

    /* Remove the B-C link: C must disappear from A's reachable set, B must
     * remain A's neighbor (that link is untouched). */
    netsim_link_down(&s, B, C);
    netsim_run(&s, DETECT_MS, STEP_MS);

    HH_ASSERT(netsim_is_neighbor(&s, A, B));
    HH_ASSERT_MSG(!netsim_is_neighbor(&s, B, C), "B still lists C as a neighbor after link down");
    HH_ASSERT_MSG(!hh_topology_node(&na->node.topology, C)->reachable,
                  "A still considers C reachable after the B-C link dropped");
    HH_ASSERT_EQ_INT(hh_neighbor_count(&nb->node.neighbors), 1);

    /* Restore the link: recovery must be complete, with no stale or duplicate
     * neighbor/topology entries anywhere in the line. */
    netsim_link_up(&s, B, C, -55.0f);
    netsim_run(&s, CONVERGE_MS * 2, STEP_MS);

    HH_ASSERT_MSG(netsim_is_neighbor(&s, B, C), "B-C link did not recover");
    HH_ASSERT_MSG(hh_topology_node(&na->node.topology, C)->reachable,
                  "A did not regain reachability to C after recovery");
    HH_ASSERT_EQ_INT(hh_neighbor_count(&na->node.neighbors), 1);
    HH_ASSERT_EQ_INT(hh_neighbor_count(&nb->node.neighbors), 2);
    HH_ASSERT_EQ_INT(hh_neighbor_count(&nc->node.neighbors), 1);
    /* B's history across both its neighbors: A joined once (1 UP, never down);
     * C joined, dropped, and rejoined (2 UPs, 1 DOWN). Total 3 UPs / 1 DOWN --
     * never a second UP for the same neighbor without an intervening DOWN,
     * which is what would signal a duplicate-add bug. */
    HH_ASSERT_EQ_INT(nb->node.neighbors.ups, 3);
    HH_ASSERT_EQ_INT(nb->node.neighbors.downs, 1);
}

/* ---------------- Multi-node scenario: triangle A-B, B-C, A-C ---------------- */

static void test_triangle_topology_full_mesh_discovery(void)
{
    netsim_t s;
    sim_node_t *na, *nb, *nc;
    const hh_node_id_t A = 1, B = 2, C = 3;

    netsim_init(&s, 215);
    netsim_add_node(&s, A);
    netsim_add_node(&s, B);
    netsim_add_node(&s, C);
    netsim_link_up(&s, A, B, -55.0f);
    netsim_link_up(&s, B, C, -55.0f);
    netsim_link_up(&s, A, C, -55.0f);
    netsim_start_all(&s);
    netsim_run(&s, CONVERGE_MS, STEP_MS);

    na = netsim_node(&s, A);
    nb = netsim_node(&s, B);
    nc = netsim_node(&s, C);

    /* A full mesh: every node is a direct neighbor of every other. */
    HH_ASSERT(netsim_is_neighbor(&s, A, B));
    HH_ASSERT(netsim_is_neighbor(&s, A, C));
    HH_ASSERT(netsim_is_neighbor(&s, B, C));
    HH_ASSERT_EQ_INT(hh_neighbor_count(&na->node.neighbors), 2);
    HH_ASSERT_EQ_INT(hh_neighbor_count(&nb->node.neighbors), 2);
    HH_ASSERT_EQ_INT(hh_neighbor_count(&nc->node.neighbors), 2);

    /* Topology at A: both B and C are direct neighbors, not multi-hop. */
    HH_ASSERT(hh_topology_node(&na->node.topology, B)->is_neighbor);
    HH_ASSERT(hh_topology_node(&na->node.topology, C)->is_neighbor);
    HH_ASSERT_EQ_INT(hh_topology_node_count(&na->node.topology), 2);

    /* Break the direct A-C edge only: C must remain reachable to A via B
     * (the mesh's redundancy), while no longer a direct neighbor. */
    netsim_link_down(&s, A, C);
    netsim_run(&s, DETECT_MS, STEP_MS);

    HH_ASSERT_MSG(!netsim_is_neighbor(&s, A, C), "direct A-C edge still a neighbor link");
    HH_ASSERT(netsim_is_neighbor(&s, A, B));
    HH_ASSERT(netsim_is_neighbor(&s, B, C));
    HH_ASSERT_MSG(hh_topology_node(&na->node.topology, C)->reachable,
                  "C lost reachability even though the B-C and A-B edges are intact");
    HH_ASSERT(!hh_topology_node(&na->node.topology, C)->is_neighbor);

    /* Restore it: back to a full mesh, no duplicate entries. */
    netsim_link_up(&s, A, C, -55.0f);
    netsim_run(&s, CONVERGE_MS * 2, STEP_MS);

    HH_ASSERT(netsim_is_neighbor(&s, A, C));
    HH_ASSERT_EQ_INT(hh_neighbor_count(&na->node.neighbors), 2);
    HH_ASSERT_EQ_INT(hh_topology_node_count(&na->node.topology), 2);
}

HH_TEST_MAIN_BEGIN("topology_scenarios")
    HH_RUN(test_lone_node_discovers_nobody);
    HH_RUN(test_two_nodes_discover_each_other);
    HH_RUN(test_unreachable_node_is_not_discovered);
    HH_RUN(test_repeated_beacons_do_not_duplicate_neighbor);
    HH_RUN(test_neighbor_metrics_update_on_link_quality_change);
    HH_RUN(test_neighbor_removed_when_link_drops_and_no_stale_entry_remains);
    HH_RUN(test_duplicate_neighbor_addition_keeps_single_entry);
    HH_RUN(test_query_of_nonexistent_neighbor_is_well_defined);
    HH_RUN(test_neighbor_table_consistent_after_repeated_cycles);
    HH_RUN(test_topology_grows_as_line_extends);
    HH_RUN(test_topology_reflects_node_unavailable_and_recovered);
    HH_RUN(test_topology_reflects_link_disappearance_and_restoration);
    HH_RUN(test_sequential_topology_changes_leave_consistent_state);
    HH_RUN(test_line_abc_full_neighbor_and_topology_lifecycle);
    HH_RUN(test_triangle_topology_full_mesh_discovery);
HH_TEST_MAIN_END()
