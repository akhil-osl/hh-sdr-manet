#include "hhsdr/manet/topology.h"
#include "hhsdr/core/log.h"
#include <string.h>

#define COMP "topology"

hh_status_t hh_topology_init(hh_topology_t *t, const hh_config_t *cfg,
                             const hh_clock_t *clock, hh_dispatcher_t *bus)
{
    if (!t || !cfg || !clock || !bus) return HH_ERR_INVAL;
    memset(t, 0, sizeof *t);
    t->cfg = cfg;
    t->clock = clock;
    t->bus = bus;
    return HH_OK;
}

static hh_topo_node_t *node_slot(hh_topology_t *t, hh_node_id_t id)
{
    hh_topo_node_t *freeslot = NULL;
    for (size_t i = 0; i < HH_TOPO_MAX_NODES; i++) {
        if (t->nodes[i].used && t->nodes[i].id == id) return &t->nodes[i];
        if (!t->nodes[i].used && !freeslot) freeslot = &t->nodes[i];
    }
    if (freeslot) {
        memset(freeslot, 0, sizeof *freeslot);
        freeslot->used = true;
        freeslot->id = id;
        t->node_count++;
    }
    return freeslot;
}

static hh_topo_edge_t *edge_slot(hh_topology_t *t, hh_node_id_t a, hh_node_id_t b)
{
    hh_topo_edge_t *freeslot = NULL;
    for (size_t i = 0; i < HH_TOPO_MAX_EDGES; i++) {
        if (t->edges[i].used &&
            ((t->edges[i].a == a && t->edges[i].b == b) ||
             (t->edges[i].a == b && t->edges[i].b == a))) return &t->edges[i];
        if (!t->edges[i].used && !freeslot) freeslot = &t->edges[i];
    }
    if (freeslot) {
        memset(freeslot, 0, sizeof *freeslot);
        freeslot->used = true;
        freeslot->a = a;
        freeslot->b = b;
        t->edge_count++;
    }
    return freeslot;
}

void hh_topology_on_neighbor_up(hh_topology_t *t, hh_node_id_t id, hh_time_ms_t now)
{
    hh_topo_node_t *n;
    hh_topo_edge_t *e;
    bool was_partitioned;

    if (!t) return;
    n = node_slot(t, id);
    if (!n) return;

    n->is_neighbor = true;
    n->reachable   = true;
    n->hop_count   = 1;
    n->last_seen   = now;
    n->link_state  = HH_LINK_HEALTHY;

    e = edge_slot(t, t->cfg->node_id, id);
    if (e) { e->quality = HH_LINK_HEALTHY; e->last_updated = now; }
    t->rebuilds++;

    /* Merge detection (HTI-13): renewed contact while partitioned means a
     * previously severed edge has reconnected. */
    was_partitioned = t->partitioned;
    if (was_partitioned) {
        hh_event_t ev;
        t->partitioned = false;
        t->merges_detected++;
        /* Hold-down before merged routes may be trusted as primary, giving
         * sequence-number freshness time to settle across both halves. */
        t->merge_hold_down_until = now + t->cfg->merge_hold_down_ms;

        HH_LOGI(COMP, "network_merged", "rejoined_neighbor=%u hold_down_until=%llu",
                id, (unsigned long long)t->merge_hold_down_until);

        memset(&ev, 0, sizeof ev);
        ev.type = HH_EV_NETWORK_MERGED;
        ev.timestamp = now;
        ev.u.merged.rejoined_neighbor = id;
        ev.u.merged.hold_down_until   = t->merge_hold_down_until;
        hh_dispatcher_publish(t->bus, &ev);
    }
    HH_LOGD(COMP, "node_added", "node=%u hops=1 neighbor=1", id);
}

void hh_topology_on_neighbor_down(hh_topology_t *t, hh_node_id_t id, hh_time_ms_t now)
{
    hh_topo_node_t *n;
    if (!t) return;
    n = node_slot(t, id);
    if (!n) return;

    n->is_neighbor = false;
    /* reachable is documented as "has a live route OR is a neighbor" — losing
     * direct adjacency must not clobber a multi-hop route learned via a
     * different next hop. hop_count > 1 can only have been set by
     * hh_topology_on_route_installed (a direct neighbor is always hop_count
     * == 1), so it is exactly the signal that such a route is still current;
     * hh_topology_on_route_withdrawn is symmetric with this same check. */
    if (n->hop_count <= 1) n->reachable = false;
    n->link_state  = HH_LINK_FAILED;
    n->last_seen   = now;

    for (size_t i = 0; i < HH_TOPO_MAX_EDGES; i++) {
        hh_topo_edge_t *e = &t->edges[i];
        if (e->used && (e->a == id || e->b == id)) {
            e->quality = HH_LINK_FAILED;
            e->last_updated = now;
        }
    }
    t->rebuilds++;
    HH_LOGD(COMP, "node_unreachable", "node=%u", id);
}

void hh_topology_on_link_state(hh_topology_t *t, const hh_ev_link_state_t *ev,
                               hh_time_ms_t now)
{
    hh_topo_node_t *n;
    hh_topo_edge_t *e;

    if (!t || !ev) return;
    /* Edge-quality annotation for operator visibility only — never a routing
     * metric, which flows directly from Link Health to the Routing Engine. */
    n = node_slot(t, ev->neighbor);
    if (n) n->link_state = ev->new_state;
    e = edge_slot(t, t->cfg->node_id, ev->neighbor);
    if (e) { e->quality = ev->new_state; e->last_updated = now; }
}

void hh_topology_on_route_installed(hh_topology_t *t, hh_node_id_t dst,
                                    hh_node_id_t next_hop, uint8_t hops,
                                    hh_time_ms_t now)
{
    hh_topo_node_t *n;
    hh_topo_edge_t *e;

    if (!t) return;
    n = node_slot(t, dst);
    if (!n) return;
    n->reachable = true;
    n->hop_count = hops;
    n->last_seen = now;

    if (next_hop != dst) {
        e = edge_slot(t, next_hop, dst);
        if (e) e->last_updated = now;
    }
    t->rebuilds++;
}

void hh_topology_on_route_withdrawn(hh_topology_t *t, hh_node_id_t dst, hh_time_ms_t now)
{
    hh_topo_node_t *n;
    if (!t) return;
    n = node_slot(t, dst);
    if (!n) return;
    /* A neighbor stays reachable even if a multi-hop route to it withdraws. */
    if (!n->is_neighbor) n->reachable = false;
    n->last_seen = now;
    t->rebuilds++;
}

void hh_topology_evaluate(hh_topology_t *t, hh_time_ms_t now)
{
    size_t known = 0, reachable = 0, neighbors = 0;
    hh_node_id_t lost_branch = HH_NODE_ID_INVALID;

    if (!t) return;

    for (size_t i = 0; i < HH_TOPO_MAX_NODES; i++) {
        const hh_topo_node_t *n = &t->nodes[i];
        if (!n->used) continue;
        known++;
        if (n->reachable) reachable++;
        else if (lost_branch == HH_NODE_ID_INVALID) lost_branch = n->id;
        if (n->is_neighbor) neighbors++;
    }

    /*
     * Partition (HTI-12): an entire branch is gone, not just one destination.
     * The check requires that we knew of nodes, that none are now reachable,
     * and that we have no neighbors at all — a node with a live neighbor is
     * still inside a functioning partition, however small.
     */
    if (!t->partitioned && known > 0 && reachable == 0 && neighbors == 0) {
        hh_event_t ev;
        t->partitioned = true;
        t->partition_branch = lost_branch;
        t->partition_at = now;
        t->partitions_detected++;

        HH_LOGW(COMP, "partition_detected", "branch_root=%u unreachable=%zu known=%zu",
                lost_branch, known - reachable, known);

        memset(&ev, 0, sizeof ev);
        ev.type = HH_EV_PARTITION_DETECTED;
        ev.timestamp = now;
        ev.u.partition.branch_root       = lost_branch;
        ev.u.partition.unreachable_count = (uint32_t)(known - reachable);
        ev.u.partition.detected_at       = now;
        hh_dispatcher_publish(t->bus, &ev);
    }
}

size_t hh_topology_node_count(const hh_topology_t *t) { return t ? t->node_count : 0; }

size_t hh_topology_reachable_count(const hh_topology_t *t)
{
    size_t n = 0;
    if (!t) return 0;
    for (size_t i = 0; i < HH_TOPO_MAX_NODES; i++)
        if (t->nodes[i].used && t->nodes[i].reachable) n++;
    return n;
}

bool hh_topology_is_partitioned(const hh_topology_t *t) { return t ? t->partitioned : false; }

const hh_topo_node_t *hh_topology_node(const hh_topology_t *t, hh_node_id_t id)
{
    if (!t) return NULL;
    for (size_t i = 0; i < HH_TOPO_MAX_NODES; i++)
        if (t->nodes[i].used && t->nodes[i].id == id) return &t->nodes[i];
    return NULL;
}

bool hh_topology_in_merge_holddown(const hh_topology_t *t, hh_time_ms_t now)
{
    return t && now < t->merge_hold_down_until;
}
