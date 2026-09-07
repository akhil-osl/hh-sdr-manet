/*
 * Self-healing demonstration — prints the network state as it converges,
 * breaks, and repairs itself. Uses the same production stack the tests drive.
 *
 * Topology:      2
 *              /   \
 *      1 ---- +     + ---- 4
 *              \   /
 *                3
 * Node 1 can reach node 4 via either node 2 or node 3.
 */
#include "netsim.h"
#include <stdio.h>

static void banner(const char *title)
{
    printf("\n============================================================\n");
    printf("  %s\n", title);
    printf("============================================================\n");
}

static void show(netsim_t *s, const char *when)
{
    hh_node_id_t hop = netsim_next_hop(s, 1, 4);
    sim_node_t *n1 = netsim_node(s, 1);

    printf("\n[t=%-6llu] %s\n", (unsigned long long)s->vc.now, when);
    printf("  node 1 neighbors : ");
    for (hh_node_id_t id = 2; id <= 4; id++)
        if (netsim_is_neighbor(s, 1, id)) {
            const hh_link_t *l = hh_link_health_get(&n1->node.link_health, id);
            printf("%u(%s) ", id, l ? hh_link_state_str(l->state) : "?");
        }
    printf("\n");
    printf("  route 1 -> 4     : %s",
           netsim_has_route(s, 1, 4) ? "REACHABLE" : "*** UNREACHABLE ***");
    if (hop != HH_NODE_ID_INVALID) printf("  via next-hop %u", hop);
    printf("\n");
    printf("  recoveries       : started=%llu completed=%llu\n",
           (unsigned long long)n1->node.self_healing.recoveries_started,
           (unsigned long long)n1->node.self_healing.recoveries_completed);
}

int main(void)
{
    netsim_t s;
    hh_node_id_t broken;

    netsim_init(&s, 42);
    for (hh_node_id_t i = 1; i <= 4; i++) netsim_add_node(&s, i);
    netsim_link_up(&s, 1, 2, -50.0f);
    netsim_link_up(&s, 1, 3, -50.0f);
    netsim_link_up(&s, 2, 4, -50.0f);
    netsim_link_up(&s, 3, 4, -50.0f);
    netsim_start_all(&s);

    banner("1. STARTUP - nodes discover each other over the radio interface");
    show(&s, "immediately after start (nothing discovered yet)");
    netsim_run(&s, 3000, 10);
    show(&s, "after discovery and routing converged");

    broken = netsim_next_hop(&s, 1, 4);
    banner("2. FAILURE - break the link node 1 is currently using");
    printf("\n  Taking down link 1 <-> %u (the active next hop)\n", broken);
    netsim_link_down(&s, 1, broken);
    netsim_run(&s, 300, 10);
    show(&s, "shortly after the break (detection in progress)");

    banner("3. SELF-HEALING - traffic reroutes over the surviving path");
    netsim_run(&s, 4000, 10);
    show(&s, "after self-healing completed");

    banner("4. RECOVERY - restore the link and let the network stabilize");
    netsim_link_up(&s, 1, broken, -50.0f);
    netsim_run(&s, 4000, 10);
    show(&s, "after the repaired link is rediscovered");

    banner("5. TOTAL ISOLATION - cut node 1 off completely (partition)");
    netsim_link_down(&s, 1, 2);
    netsim_link_down(&s, 1, 3);
    netsim_run(&s, 5000, 10);
    show(&s, "node 1 is now isolated");
    printf("  partition flagged: %s\n",
           hh_topology_is_partitioned(&netsim_node(&s, 1)->node.topology)
               ? "YES" : "no");

    banner("6. MERGE - reconnect and confirm the network rejoins");
    netsim_link_up(&s, 1, 2, -50.0f);
    netsim_run(&s, 6000, 10);
    show(&s, "after the partition merged");

    printf("\n");
    return netsim_has_route(&s, 1, 4) ? 0 : 1;
}
