/*
 * Neighbor Manager (Doc 1 §3, §4; SCA class: Resource).
 *
 * THE authoritative source of one-hop neighbor information. It is the sole
 * writer of the Neighbor Table; every other component reads snapshots and
 * receives NeighborUp/Down/Changed events (HTI-06). Topology Manager never sits
 * between it and the Routing Engine.
 *
 * It owns liveness, capability, and the per-neighbor raw link samples. It does
 * NOT own multi-hop reachability or route metrics (Routing Engine), and it does
 * not decide link health (Link Health Monitor) — it only records what arrived.
 *
 * Expiry is by cadence, not wall-clock: a neighbor is expired after
 * neighbor_allowed_loss missed beacons at the interval that neighbor was last
 * observed to use, which is what Doc 1 §4 specifies. The table is bounded with
 * LRU eviction (MAX_NEIGHBOUR_TABLE_ENTRIES lineage).
 */
#ifndef HHSDR_MANET_NEIGHBOR_H
#define HHSDR_MANET_NEIGHBOR_H

#include "hhsdr/core/clock.h"
#include "hhsdr/core/config.h"
#include "hhsdr/core/dispatcher.h"

#define HH_MAX_NEIGHBORS 64

typedef struct {
    hh_node_id_t id;
    bool         used;

    /* Liveness */
    hh_seq_t     last_seq;
    hh_time_ms_t last_heard;
    hh_time_ms_t first_heard;
    uint32_t     observed_interval_ms;  /* inter-beacon gap, for cadence expiry */
    uint32_t     beacons_received;
    uint32_t     beacons_missed;        /* estimated from sequence gaps         */

    /* Capability, copied from the validating beacon so consumers need not
     * re-parse it (HTI-06 field list). */
    uint32_t capabilities;
    uint32_t radio_caps;
    bool     routing_capable;
    bool     position_valid;
    float    position_x, position_y, position_z;
    bool     power_valid;
    float    power_battery;

    /* Most recent raw link sample. Owned here; the Link Health Monitor fuses
     * it but does not store the raw values. */
    hh_link_sample_t last_sample;
} hh_neighbor_t;

typedef struct {
    const hh_config_t *cfg;
    const hh_clock_t  *clock;
    hh_dispatcher_t   *bus;

    hh_neighbor_t table[HH_MAX_NEIGHBORS];
    size_t        count;

    uint64_t ups;
    uint64_t downs;
    uint64_t changes;
    uint64_t evictions;
} hh_neighbor_mgr_t;

hh_status_t hh_neighbor_init(hh_neighbor_mgr_t *nm, const hh_config_t *cfg,
                             const hh_clock_t *clock, hh_dispatcher_t *bus);

/* Consume a validated beacon (HH_EV_BEACON_RX). Adds or refreshes the entry and
 * publishes NeighborUp or NeighborChanged as appropriate. */
hh_status_t hh_neighbor_on_beacon(hh_neighbor_mgr_t *nm, const hh_beacon_t *b,
                                  const hh_link_sample_t *sample, hh_time_ms_t now);

/* Age out neighbors whose expected beacons have not arrived. Publishes
 * NeighborDown for each. Returns how many expired. */
size_t hh_neighbor_tick(hh_neighbor_mgr_t *nm, hh_time_ms_t now);

/* Read-only accessors. Callers must not retain the pointer across a mutation. */
const hh_neighbor_t *hh_neighbor_get(const hh_neighbor_mgr_t *nm, hh_node_id_t id);
size_t hh_neighbor_count(const hh_neighbor_mgr_t *nm);

/* Snapshot ids into out[]; returns how many were written. */
size_t hh_neighbor_list(const hh_neighbor_mgr_t *nm, hh_node_id_t *out, size_t cap);

/* Force-remove a neighbor (used when a failure is confirmed elsewhere).
 * Publishes NeighborDown. */
hh_status_t hh_neighbor_remove(hh_neighbor_mgr_t *nm, hh_node_id_t id, hh_time_ms_t now);

/* Record a link sample arriving outside a beacon (HTI-05 periodic path). */
hh_status_t hh_neighbor_record_sample(hh_neighbor_mgr_t *nm, const hh_link_sample_t *s,
                                      hh_time_ms_t now);

#endif /* HHSDR_MANET_NEIGHBOR_H */
