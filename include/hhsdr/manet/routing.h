/*
 * Routing Engine (Doc 1 §6; SCA class: Resource, and the SAD assembly controller).
 *
 * Proactive, distance-vector, sequence-numbered, RF-metric-weighted — Doc 1 §6's
 * recommendation. Routes are maintained by periodic sequence-numbered
 * originator-style updates bounded to one hop's neighbors, plus event-triggered
 * updates fired immediately on a confirmed link state change.
 *
 * It owns route computation, the route table, and next-hop selection. It
 * consumes link classification but does not compute it, and it never forwards a
 * packet. It is the sole writer of the published route snapshot.
 *
 * Selection rule (ported and generalized from the prototype's
 * _should_replace_route): a strictly fresher sequence number always wins; at
 * equal sequence number, the better composite metric wins. Hop count is a
 * bounded penalty term, never the primary metric.
 *
 * Invalidation is two-phase (Doc 1 §6): mark invalid immediately so forwarding
 * stops at once, delete after a grace window so a returning route is not
 * confused with a brand-new one.
 */
#ifndef HHSDR_MANET_ROUTING_H
#define HHSDR_MANET_ROUTING_H

#include "hhsdr/core/clock.h"
#include "hhsdr/core/config.h"
#include "hhsdr/core/dispatcher.h"
#include "hhsdr/manet/neighbor.h"
#include "hhsdr/manet/route_table.h"
#include "hhsdr/manet/link_health.h"
#include "hhsdr/radio/wire.h"

/* Internal working entry. The published snapshot carries only what forwarding
 * needs; this carries the bookkeeping routing itself needs. */
typedef struct {
    hh_node_id_t destination;
    hh_node_id_t next_hop;
    float        metric;
    hh_seq_t     sequence_no;
    uint8_t      hop_count;
    bool         used;
    bool         valid;

    hh_time_ms_t installed_at;
    hh_time_ms_t last_used;
    hh_time_ms_t invalidated_at;   /* start of the DELETE_PERIOD grace window */
    hh_time_ms_t hold_down_until;  /* not replaceable before this (Doc 1 §8)  */

    hh_node_id_t alt_next_hop;
    float        alt_metric;
    uint8_t      alt_hop_count;
    bool         has_alt;
} hh_route_t;

#define HH_ROUTING_MAX HH_MAX_ROUTES

typedef struct {
    const hh_config_t       *cfg;
    const hh_clock_t        *clock;
    hh_dispatcher_t         *bus;
    const hh_neighbor_mgr_t *neighbors;   /* read-only: one-hop truth */
    const hh_link_health_t  *link_health; /* read-only: link state    */

    hh_route_t           routes[HH_ROUTING_MAX];
    size_t               count;
    hh_route_publisher_t publisher;

    hh_seq_t     own_seq;              /* our originator sequence number */
    hh_time_ms_t last_update_sent;

    /* Dampening: next-hops penalised for repeated flapping (Doc 1 §8). */
    struct { hh_node_id_t id; hh_time_ms_t until; bool used; } damped[32];

    uint64_t installs;
    uint64_t withdrawals;
    uint64_t replacements;
    uint64_t rejected_stale;
} hh_routing_t;

hh_status_t hh_routing_init(hh_routing_t *r, const hh_config_t *cfg,
                            const hh_clock_t *clock, hh_dispatcher_t *bus,
                            const hh_neighbor_mgr_t *neighbors,
                            const hh_link_health_t *link_health);

/* Offer a route learned from a neighbor's update. Applies the freshness/metric
 * rule and publishes RouteInstalled when it wins. Returns HH_OK when installed,
 * HH_ERR_AGAIN when correctly rejected as not better. */
hh_status_t hh_routing_offer(hh_routing_t *r, hh_node_id_t destination,
                             hh_node_id_t via_neighbor, hh_seq_t seq,
                             uint8_t hop_count, float neighbor_metric,
                             hh_time_ms_t now);

/* A direct neighbor appeared: install the one-hop route. */
hh_status_t hh_routing_on_neighbor_up(hh_routing_t *r, hh_node_id_t id, hh_time_ms_t now);

/* Invalidation cascade (HTI-09): mark every route whose next hop is this
 * neighbor invalid, promoting a warm alternate where one exists. Returns how
 * many routes were affected. */
size_t hh_routing_invalidate_via(hh_routing_t *r, hh_node_id_t next_hop,
                                 hh_withdraw_reason_t reason, hh_time_ms_t now);

/* Age routes: expire on inactivity, delete after the grace window. */
size_t hh_routing_tick(hh_routing_t *r, hh_time_ms_t now);

/* Build the proactive update this node broadcasts (Doc 1 §6). */
size_t hh_routing_build_update(const hh_routing_t *r, hh_route_update_t *out);

/* Penalise a next-hop that has flapped repeatedly (Doc 1 §8 route dampening). */
void hh_routing_damp(hh_routing_t *r, hh_node_id_t next_hop, hh_time_ms_t now);
bool hh_routing_is_damped(const hh_routing_t *r, hh_node_id_t next_hop, hh_time_ms_t now);

/* Queries. */
const hh_route_t *hh_routing_get(const hh_routing_t *r, hh_node_id_t dst);
const hh_route_snapshot_t *hh_routing_snapshot(const hh_routing_t *r);
size_t hh_routing_count(const hh_routing_t *r);

/* Mark a route used, refreshing its activity timer (ACTIVE_ROUTE_TIMEOUT). */
void hh_routing_mark_used(hh_routing_t *r, hh_node_id_t dst, hh_time_ms_t now);

#endif /* HHSDR_MANET_ROUTING_H */
