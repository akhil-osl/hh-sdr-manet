/*
 * Management / Telemetry.
 *
 * Read-only export of node, neighbor, link, route, and radio state. 
 * requires that this "never sits on any layer's critical path": nothing here is
 * called from the forwarding path, and it only reads state other components
 * already own. It takes no lock the control plane can hold.
 *
 * Output is line-oriented key=value, the same shape as the structured log, so
 * an operator console or an automated test can consume it without a parser.
 * The architecture records the NodeStatus payload schema as TBD; this
 * is a concrete interim schema, not a specification contract.
 */
#ifndef HHSDR_MANET_TELEMETRY_H
#define HHSDR_MANET_TELEMETRY_H

#include "hhsdr/manet/node.h"

/* Snapshot of node health for HTI-01. */
typedef struct {
    hh_node_id_t node_id;
    const char  *state;
    uint32_t     neighbor_count;
    uint32_t     route_count;
    uint32_t     topology_nodes;
    uint32_t     reachable_nodes;
    bool         partitioned;
    uint32_t     beacon_interval_ms;

    uint64_t beacons_sent;
    uint64_t beacons_rx_accepted;
    uint64_t beacons_rx_duplicate;
    uint64_t beacons_rx_malformed;
    uint64_t neighbor_ups;
    uint64_t neighbor_downs;
    uint64_t link_transitions;
    uint64_t failures_confirmed;
    uint64_t recoveries_started;
    uint64_t recoveries_completed;
    uint64_t routes_installed;
    uint64_t routes_withdrawn;
    uint64_t packets_forwarded;
    uint64_t packets_delivered_local;
    uint64_t packets_dropped_no_route;
    uint64_t events_dropped;

    /* Hardware-interface status (HTI-02). Reported honestly: with no radio
     * backend this says so rather than showing plausible-looking counters. */
    bool     radio_operational;
    bool     radio_available;   /* false when the adapter is the TBD stub */
    uint32_t radio_channel;
} hh_node_status_t;

void hh_telemetry_node_status(const hh_node_t *n, hh_node_status_t *out);

/* Format helpers. Each returns the number of bytes written (excluding NUL). */
size_t hh_telemetry_format_status(const hh_node_status_t *st, char *buf, size_t cap);
size_t hh_telemetry_format_neighbors(const hh_node_t *n, char *buf, size_t cap);
size_t hh_telemetry_format_routes(const hh_node_t *n, char *buf, size_t cap);
size_t hh_telemetry_format_topology(const hh_node_t *n, char *buf, size_t cap);

/* Write a full report to a stream (operator console / diagnostics dump). */
void hh_telemetry_dump(const hh_node_t *n, FILE *out);

#endif /* HHSDR_MANET_TELEMETRY_H */
