/*
 * Topology Manager (Doc 1 §5; SCA class: Resource).
 *
 * Owns a derived, eventually-consistent graph view for management and telemetry.
 * It is A READ MODEL, never a dependency of the forwarding path and never in the
 * routing engine's decision path (Doc 1 §3, §5). It only reads from the Neighbor
 * Manager; it never sits between Neighbor Manager and Routing Engine.
 *
 * It is built from the same events the routing engine already consumes
 * (NeighborUp/Down, LinkStateChanged, route install/withdraw) rather than a
 * second flood.
 *
 * Detects partition (HTI-12) and merge (HTI-13):
 *  - Partition: "flagged when an entire neighbor branch, not just one
 *    destination, becomes unreachable" (Doc 1 §5).
 *  - Merge: renewed beacon exchange across a previously-severed edge, with a
 *    hold-down before newly-merged routes are trusted as primary (Doc 1 §8).
 */
#ifndef HHSDR_MANET_TOPOLOGY_H
#define HHSDR_MANET_TOPOLOGY_H

#include "hhsdr/core/clock.h"
#include "hhsdr/core/config.h"
#include "hhsdr/core/dispatcher.h"

#define HH_TOPO_MAX_NODES 128
#define HH_TOPO_MAX_EDGES 256

typedef struct {
    hh_node_id_t    id;
    bool            used;
    bool            reachable;       /* has a live route or is a neighbor    */
    bool            is_neighbor;     /* directly adjacent to us              */
    uint8_t         hop_count;
    hh_time_ms_t    last_seen;
    hh_link_state_t link_state;      /* annotation only, never a route input */
} hh_topo_node_t;

typedef struct {
    hh_node_id_t    a, b;            /* a is always this node or a next hop  */
    bool            used;
    hh_link_state_t quality;         /* edge-quality annotation for operators */
    hh_time_ms_t    last_updated;
} hh_topo_edge_t;

typedef struct {
    const hh_config_t *cfg;
    const hh_clock_t  *clock;
    hh_dispatcher_t   *bus;

    hh_topo_node_t nodes[HH_TOPO_MAX_NODES];
    hh_topo_edge_t edges[HH_TOPO_MAX_EDGES];
    size_t         node_count;
    size_t         edge_count;

    /* Partition/merge bookkeeping. */
    bool         partitioned;
    hh_node_id_t partition_branch;
    hh_time_ms_t partition_at;
    hh_time_ms_t merge_hold_down_until;

    uint64_t partitions_detected;
    uint64_t merges_detected;
    uint32_t rebuilds;
} hh_topology_t;

hh_status_t hh_topology_init(hh_topology_t *t, const hh_config_t *cfg,
                             const hh_clock_t *clock, hh_dispatcher_t *bus);

/* Event consumption — all read-only with respect to other components. */
void hh_topology_on_neighbor_up(hh_topology_t *t, hh_node_id_t id, hh_time_ms_t now);
void hh_topology_on_neighbor_down(hh_topology_t *t, hh_node_id_t id, hh_time_ms_t now);
void hh_topology_on_link_state(hh_topology_t *t, const hh_ev_link_state_t *ev,
                               hh_time_ms_t now);
void hh_topology_on_route_installed(hh_topology_t *t, hh_node_id_t dst,
                                    hh_node_id_t next_hop, uint8_t hops,
                                    hh_time_ms_t now);
void hh_topology_on_route_withdrawn(hh_topology_t *t, hh_node_id_t dst,
                                    hh_time_ms_t now);

/* Connectivity check; publishes PartitionDetected / NetworkMerged as warranted. */
void hh_topology_evaluate(hh_topology_t *t, hh_time_ms_t now);

/* Queries for the management plane. */
size_t hh_topology_node_count(const hh_topology_t *t);
size_t hh_topology_reachable_count(const hh_topology_t *t);
bool   hh_topology_is_partitioned(const hh_topology_t *t);
const hh_topo_node_t *hh_topology_node(const hh_topology_t *t, hh_node_id_t id);
bool   hh_topology_in_merge_holddown(const hh_topology_t *t, hh_time_ms_t now);

#endif /* HHSDR_MANET_TOPOLOGY_H */
