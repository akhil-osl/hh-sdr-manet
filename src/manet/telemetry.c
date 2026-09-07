#include "hhsdr/manet/telemetry.h"
#include "hhsdr/radio/hw_adapter.h"
#include <string.h>

void hh_telemetry_node_status(const hh_node_t *n, hh_node_status_t *out)
{
    hh_radio_status_t rs;
    hh_status_t rst;

    if (!n || !out) return;
    memset(out, 0, sizeof *out);

    out->node_id        = n->cfg.node_id;
    out->state          = hh_node_state_str(n->state);
    out->neighbor_count = (uint32_t)hh_neighbor_count(&n->neighbors);
    out->route_count    = (uint32_t)hh_routing_count(&n->routing);
    out->topology_nodes = (uint32_t)hh_topology_node_count(&n->topology);
    out->reachable_nodes= (uint32_t)hh_topology_reachable_count(&n->topology);
    out->partitioned    = hh_topology_is_partitioned(&n->topology);
    out->beacon_interval_ms = hh_discovery_interval(&n->discovery);

    out->beacons_sent          = n->discovery.beacons_sent;
    out->beacons_rx_accepted   = n->discovery.beacons_rx_accepted;
    out->beacons_rx_duplicate  = n->discovery.beacons_rx_duplicate;
    out->beacons_rx_malformed  = n->discovery.beacons_rx_malformed;
    out->neighbor_ups          = n->neighbors.ups;
    out->neighbor_downs        = n->neighbors.downs;
    out->link_transitions      = n->link_health.transitions;
    out->failures_confirmed    = n->failure_detector.confirmations;
    out->recoveries_started    = n->self_healing.recoveries_started;
    out->recoveries_completed  = n->self_healing.recoveries_completed;
    out->routes_installed      = n->routing.installs;
    out->routes_withdrawn      = n->routing.withdrawals;
    out->packets_forwarded     = n->forwarder.forwarded;
    out->packets_delivered_local = n->forwarder.delivered_local;
    out->packets_dropped_no_route = n->forwarder.dropped_no_route;
    out->events_dropped        = hh_dispatcher_dropped(&n->bus);

    /* Radio status is read through the abstraction, so a stub adapter reports
     * "unavailable" rather than the telemetry inventing a plausible reading. */
    rst = hh_radio_get_status(n->radio, &rs);
    if (rst == HH_OK) {
        out->radio_available   = true;
        out->radio_operational = rs.operational;
        out->radio_channel     = rs.channel;
    } else {
        out->radio_available   = false;
        out->radio_operational = false;
    }
}

size_t hh_telemetry_format_status(const hh_node_status_t *st, char *buf, size_t cap)
{
    int n;
    if (!st || !buf || cap == 0) return 0;
    n = snprintf(buf, cap,
        "node=%u state=%s neighbors=%u routes=%u topo_nodes=%u reachable=%u "
        "partitioned=%d beacon_ms=%u "
        "beacons_tx=%llu beacons_rx=%llu beacons_dup=%llu beacons_bad=%llu "
        "nbr_up=%llu nbr_down=%llu link_transitions=%llu failures=%llu "
        "recov_started=%llu recov_done=%llu routes_installed=%llu routes_withdrawn=%llu "
        "fwd=%llu delivered=%llu drop_no_route=%llu events_dropped=%llu "
        "radio_available=%d radio_operational=%d radio_channel=%u",
        st->node_id, st->state, st->neighbor_count, st->route_count,
        st->topology_nodes, st->reachable_nodes, (int)st->partitioned,
        st->beacon_interval_ms,
        (unsigned long long)st->beacons_sent, (unsigned long long)st->beacons_rx_accepted,
        (unsigned long long)st->beacons_rx_duplicate,
        (unsigned long long)st->beacons_rx_malformed,
        (unsigned long long)st->neighbor_ups, (unsigned long long)st->neighbor_downs,
        (unsigned long long)st->link_transitions,
        (unsigned long long)st->failures_confirmed,
        (unsigned long long)st->recoveries_started,
        (unsigned long long)st->recoveries_completed,
        (unsigned long long)st->routes_installed,
        (unsigned long long)st->routes_withdrawn,
        (unsigned long long)st->packets_forwarded,
        (unsigned long long)st->packets_delivered_local,
        (unsigned long long)st->packets_dropped_no_route,
        (unsigned long long)st->events_dropped,
        (int)st->radio_available, (int)st->radio_operational, st->radio_channel);
    return n < 0 ? 0 : (size_t)n;
}

size_t hh_telemetry_format_neighbors(const hh_node_t *n, char *buf, size_t cap)
{
    size_t used = 0;
    hh_node_id_t ids[HH_MAX_NEIGHBORS];
    size_t count;

    if (!n || !buf || cap == 0) return 0;
    buf[0] = '\0';
    count = hh_neighbor_list(&n->neighbors, ids, HH_MAX_NEIGHBORS);

    for (size_t i = 0; i < count && used < cap; i++) {
        const hh_neighbor_t *nb = hh_neighbor_get(&n->neighbors, ids[i]);
        const hh_link_t *l;
        int w;
        if (!nb) continue;
        l = hh_link_health_get(&n->link_health, ids[i]);
        w = snprintf(buf + used, cap - used,
            "neighbor=%u last_heard=%llu interval_ms=%u beacons=%u missed=%u "
            "rssi=%.1f snr=%.1f state=%s score=%.3f routing_capable=%d\n",
            nb->id, (unsigned long long)nb->last_heard, nb->observed_interval_ms,
            nb->beacons_received, nb->beacons_missed,
            (double)nb->last_sample.rssi, (double)nb->last_sample.snr,
            l ? hh_link_state_str(l->state) : "untracked",
            l ? (double)l->score : 0.0, (int)nb->routing_capable);
        if (w < 0) break;
        used += (size_t)w;
    }
    return used;
}

size_t hh_telemetry_format_routes(const hh_node_t *n, char *buf, size_t cap)
{
    const hh_route_snapshot_t *snap;
    size_t used = 0;

    if (!n || !buf || cap == 0) return 0;
    buf[0] = '\0';
    /* Reads the published snapshot: the same lock-free pointer the forwarder
     * uses, so telemetry adds no contention to the fast path. */
    snap = hh_routing_snapshot(&n->routing);
    if (!snap) return 0;

    for (size_t i = 0; i < snap->count && used < cap; i++) {
        const hh_route_entry_t *e = &snap->entries[i];
        int w = snprintf(buf + used, cap - used,
            "dst=%u next_hop=%u hops=%u metric=%.3f seq=%u valid=%d "
            "alt_next_hop=%u has_alt=%d snapshot_version=%u\n",
            e->destination, e->next_hop, e->hop_count, (double)e->metric,
            e->sequence_no, (int)e->valid, e->alt_next_hop, (int)e->has_alt,
            snap->version);
        if (w < 0) break;
        used += (size_t)w;
    }
    return used;
}

size_t hh_telemetry_format_topology(const hh_node_t *n, char *buf, size_t cap)
{
    size_t used = 0;

    if (!n || !buf || cap == 0) return 0;
    buf[0] = '\0';
    for (size_t i = 0; i < HH_TOPO_MAX_NODES && used < cap; i++) {
        const hh_topo_node_t *tn = &n->topology.nodes[i];
        int w;
        if (!tn->used) continue;
        w = snprintf(buf + used, cap - used,
            "node=%u reachable=%d neighbor=%d hops=%u link_state=%s last_seen=%llu\n",
            tn->id, (int)tn->reachable, (int)tn->is_neighbor, tn->hop_count,
            hh_link_state_str(tn->link_state), (unsigned long long)tn->last_seen);
        if (w < 0) break;
        used += (size_t)w;
    }
    return used;
}

void hh_telemetry_dump(const hh_node_t *n, FILE *out)
{
    hh_node_status_t st;
    char buf[4096];

    if (!n || !out) return;

    hh_telemetry_node_status(n, &st);
    hh_telemetry_format_status(&st, buf, sizeof buf);
    fprintf(out, "[status] %s\n", buf);

    if (hh_telemetry_format_neighbors(n, buf, sizeof buf)) fprintf(out, "%s", buf);
    if (hh_telemetry_format_routes(n, buf, sizeof buf))    fprintf(out, "%s", buf);
    if (hh_telemetry_format_topology(n, buf, sizeof buf))  fprintf(out, "%s", buf);

    if (!st.radio_available) {
        /* Never let a missing hardware backend look like a healthy node. */
        fprintf(out, "[warning] radio adapter reports no hardware backend "
                     "(FPGA/PL API unavailable)\n");
    }
}
