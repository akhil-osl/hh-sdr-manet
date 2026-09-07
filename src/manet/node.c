#include "hhsdr/manet/node.h"
#include "hhsdr/core/log.h"
#include "hhsdr/radio/wire.h"
#include <string.h>

#define COMP "node"

/* ---------------------------------------------------------------------------
 * Event handlers. Each is one component consuming another's published event —
 * never a direct nested call from the producer into the consumer.
 * ------------------------------------------------------------------------- */

static void on_beacon_rx(const hh_event_t *ev, void *ctx)
{
    hh_node_t *n = ctx;
    const hh_beacon_t *b = &ev->u.beacon_rx.beacon;
    const hh_link_sample_t *s = &ev->u.beacon_rx.sample;

    /* Discovery -> Neighbor Manager (validated beacon). */
    hh_neighbor_on_beacon(&n->neighbors, b, s, ev->timestamp);

    /* The beacon is also the heartbeat, so it feeds link health directly
     * (Doc 1 §4: discovery and heartbeat are one message stream). */
    hh_link_health_on_beacon(&n->link_health, b->node_id, ev->timestamp);
    hh_link_health_on_sample(&n->link_health, s, ev->timestamp);
}

static void on_neighbor_up(const hh_event_t *ev, void *ctx)
{
    hh_node_t *n = ctx;
    hh_node_id_t id = ev->u.neighbor.neighbor_id;

    /* HTI-06 fan-out. Topology and Routing update from the same event
     * independently; neither waits on the other. */
    hh_link_health_add(&n->link_health, id, ev->timestamp);
    hh_topology_on_neighbor_up(&n->topology, id, ev->timestamp);
    hh_routing_on_neighbor_up(&n->routing, id, ev->timestamp);
}

static void on_neighbor_down(const hh_event_t *ev, void *ctx)
{
    hh_node_t *n = ctx;
    hh_node_id_t id = ev->u.neighbor.neighbor_id;

    hh_topology_on_neighbor_down(&n->topology, id, ev->timestamp);
    /* Neighbor expiry cascades route invalidation (Doc 1 §6). */
    hh_routing_invalidate_via(&n->routing, id, HH_WITHDRAW_FAILURE_CASCADE,
                              ev->timestamp);
    hh_link_health_remove(&n->link_health, id);
}

static void on_link_state(const hh_event_t *ev, void *ctx)
{
    hh_node_t *n = ctx;
    /* HTI-07 fan-out: four consumers, none blocking another. */
    hh_fd_on_link_state(&n->failure_detector, &ev->u.link_state, ev->timestamp);
    hh_topology_on_link_state(&n->topology, &ev->u.link_state, ev->timestamp);
}

static void on_cadence_hint(const hh_event_t *ev, void *ctx)
{
    hh_node_t *n = ctx;
    hh_discovery_apply_cadence_hint(&n->discovery, &ev->u.cadence);
}

static void on_failure(const hh_event_t *ev, void *ctx)
{
    hh_node_t *n = ctx;
    hh_sh_on_failure(&n->self_healing, &ev->u.failure, ev->timestamp);
}

static void on_route_installed(const hh_event_t *ev, void *ctx)
{
    hh_node_t *n = ctx;
    hh_topology_on_route_installed(&n->topology, ev->u.route_installed.destination,
                                   ev->u.route_installed.next_hop,
                                   ev->u.route_installed.hop_count, ev->timestamp);
}

static void on_route_withdrawn(const hh_event_t *ev, void *ctx)
{
    hh_node_t *n = ctx;
    hh_topology_on_route_withdrawn(&n->topology, ev->u.route_withdrawn.destination,
                                   ev->timestamp);
}

static void on_partition(const hh_event_t *ev, void *ctx)
{
    hh_node_t *n = ctx;
    hh_sh_on_partition(&n->self_healing, &ev->u.partition, ev->timestamp);
}

static void on_merged(const hh_event_t *ev, void *ctx)
{
    hh_node_t *n = ctx;
    hh_sh_on_merge(&n->self_healing, &ev->u.merged, ev->timestamp);
}

/* Radio rx callback: the single ingress point for everything off the air. */
static void radio_rx(const hh_frame_t *f, const hh_link_sample_t *m, void *ctx)
{
    hh_node_t *n = ctx;
    hh_node_on_frame(n, f, m);
}

/* ---------------------------------------------------------------------------
 * Lifecycle
 * ------------------------------------------------------------------------- */

const char *hh_node_state_str(hh_node_state_t s)
{
    switch (s) {
    case HH_NODE_CREATED:     return "created";
    case HH_NODE_INITIALIZED: return "initialized";
    case HH_NODE_CONFIGURED:  return "configured";
    case HH_NODE_RUNNING:     return "running";
    case HH_NODE_STOPPED:     return "stopped";
    case HH_NODE_RELEASED:    return "released";
    }
    return "unknown";
}

hh_status_t hh_node_init(hh_node_t *n, const hh_config_t *cfg,
                         const hh_clock_t *clock, hh_radio_t *radio)
{
    hh_status_t st;

    if (!n || !cfg || !clock || !radio) return HH_ERR_INVAL;
    st = hh_config_validate(cfg);
    if (st != HH_OK) return st;

    memset(n, 0, sizeof *n);
    n->cfg   = *cfg;
    n->clock = clock;
    n->radio = radio;

    /* SCA step 2: each component reaches a known internal state. */
    hh_dispatcher_init(&n->bus);
    hh_discovery_init(&n->discovery, &n->cfg, clock, &n->bus, radio);
    hh_neighbor_init(&n->neighbors, &n->cfg, clock, &n->bus);
    hh_link_health_init(&n->link_health, &n->cfg, clock, &n->bus);
    hh_routing_init(&n->routing, &n->cfg, clock, &n->bus, &n->neighbors, &n->link_health);
    hh_fd_init(&n->failure_detector, &n->cfg, clock, &n->bus);
    hh_topology_init(&n->topology, &n->cfg, clock, &n->bus);
    hh_sh_init(&n->self_healing, &n->cfg, clock, &n->bus, &n->routing, &n->topology, radio);
    hh_forwarder_init(&n->forwarder, &n->cfg, radio, &n->routing.publisher);

    /* SCA step 3: port connection. Each subscription is one uses/provides pair. */
    hh_dispatcher_subscribe(&n->bus, "neighbor.beacon", HH_EV_MASK(HH_EV_BEACON_RX),
                            on_beacon_rx, n);
    hh_dispatcher_subscribe(&n->bus, "fanout.nbr_up", HH_EV_MASK(HH_EV_NEIGHBOR_UP),
                            on_neighbor_up, n);
    hh_dispatcher_subscribe(&n->bus, "fanout.nbr_down", HH_EV_MASK(HH_EV_NEIGHBOR_DOWN),
                            on_neighbor_down, n);
    hh_dispatcher_subscribe(&n->bus, "fanout.link_state",
                            HH_EV_MASK(HH_EV_LINK_STATE_CHANGED), on_link_state, n);
    hh_dispatcher_subscribe(&n->bus, "discovery.cadence", HH_EV_MASK(HH_EV_CADENCE_HINT),
                            on_cadence_hint, n);
    hh_dispatcher_subscribe(&n->bus, "selfheal.failure",
                            HH_EV_MASK(HH_EV_FAILURE_DETECTED), on_failure, n);
    hh_dispatcher_subscribe(&n->bus, "topo.route_installed",
                            HH_EV_MASK(HH_EV_ROUTE_INSTALLED), on_route_installed, n);
    hh_dispatcher_subscribe(&n->bus, "topo.route_withdrawn",
                            HH_EV_MASK(HH_EV_ROUTE_WITHDRAWN), on_route_withdrawn, n);
    hh_dispatcher_subscribe(&n->bus, "selfheal.partition",
                            HH_EV_MASK(HH_EV_PARTITION_DETECTED), on_partition, n);
    hh_dispatcher_subscribe(&n->bus, "selfheal.merged",
                            HH_EV_MASK(HH_EV_NETWORK_MERGED), on_merged, n);

    hh_radio_set_rx_callback(radio, radio_rx, n);

    n->state = HH_NODE_INITIALIZED;
    HH_LOGI(COMP, "initialized", "node=%u", n->cfg.node_id);
    return HH_OK;
}

hh_status_t hh_node_configure(hh_node_t *n, const hh_config_t *cfg)
{
    hh_status_t st;
    if (!n) return HH_ERR_INVAL;
    /* SCA: configure must follow wiring and precede start. */
    if (n->state != HH_NODE_INITIALIZED && n->state != HH_NODE_STOPPED)
        return HH_ERR_STATE;
    if (cfg) {
        st = hh_config_validate(cfg);
        if (st != HH_OK) return st;
        n->cfg = *cfg;
    }
    hh_log_set_level(n->cfg.log_level);
    n->state = HH_NODE_CONFIGURED;
    HH_LOGI(COMP, "configured", "node=%u beacon_ms=%u", n->cfg.node_id,
            n->cfg.beacon_interval_ms);
    return HH_OK;
}

hh_status_t hh_node_start(hh_node_t *n)
{
    hh_status_t st;
    if (!n) return HH_ERR_INVAL;
    if (n->state != HH_NODE_CONFIGURED && n->state != HH_NODE_STOPPED)
        return HH_ERR_STATE;

    st = hh_radio_open(n->radio);
    if (st != HH_OK) {
        /* Surfaced, not hidden: with the stub hardware adapter this is exactly
         * where a node reports that no radio backend exists. */
        HH_LOGE(COMP, "radio_open_failed", "node=%u status=%s",
                n->cfg.node_id, hh_status_str(st));
        return st;
    }
    hh_discovery_start(&n->discovery);
    n->state = HH_NODE_RUNNING;
    HH_LOGI(COMP, "started", "node=%u", n->cfg.node_id);
    return HH_OK;
}

hh_status_t hh_node_stop(hh_node_t *n)
{
    if (!n) return HH_ERR_INVAL;
    if (n->state != HH_NODE_RUNNING) return HH_ERR_STATE;
    hh_discovery_stop(&n->discovery);
    n->state = HH_NODE_STOPPED;
    HH_LOGI(COMP, "stopped", "node=%u", n->cfg.node_id);
    return HH_OK;
}

hh_status_t hh_node_release(hh_node_t *n)
{
    if (!n) return HH_ERR_INVAL;
    if (n->state == HH_NODE_RUNNING) hh_node_stop(n);
    hh_radio_close(n->radio);
    n->state = HH_NODE_RELEASED;
    HH_LOGI(COMP, "released", "node=%u", n->cfg.node_id);
    return HH_OK;
}

/* ---------------------------------------------------------------------------
 * Ingress and periodic work
 * ------------------------------------------------------------------------- */

static void handle_route_update(hh_node_t *n, const hh_frame_t *f, hh_time_ms_t now)
{
    hh_route_update_t u;
    if (hh_route_update_decode(f->data, f->len, &u) != HH_OK) return;
    if (u.sender == n->cfg.node_id) return;

    n->route_updates_rx++;
    for (uint8_t i = 0; i < u.count; i++) {
        const hh_route_update_entry_t *e = &u.entries[i];
        uint8_t hops;
        if (e->originator == n->cfg.node_id) continue;   /* never route to self */

        /* Distance vector: the advertised route costs one more hop through the
         * sender. An already-infinite hop count stays infinite rather than
         * wrapping to zero, so a poisoned-reverse withdrawal is not silently
         * turned back into an attractive zero-hop route. */
        hops = (e->hop_count >= (uint8_t)(HH_HOP_INFINITY - 1))
             ? (uint8_t)HH_HOP_INFINITY
             : (uint8_t)(e->hop_count + 1);

        /* Routing applies the freshness/metric rule and validates the next hop
         * against the authoritative neighbor table. */
        hh_routing_offer(&n->routing, e->originator, u.sender, e->sequence_no,
                         hops, e->metric, now);
    }
}

void hh_node_on_frame(hh_node_t *n, const hh_frame_t *f, const hh_link_sample_t *m)
{
    hh_time_ms_t now;
    if (!n || !f || !m) return;
    now = hh_now(n->clock);

    switch (f->kind) {
    case HH_FRAME_BEACON:
        hh_discovery_on_frame(&n->discovery, f, m);
        break;
    case HH_FRAME_ROUTING:
        handle_route_update(n, f, now);
        break;
    case HH_FRAME_DATA:
        hh_forwarder_forward(&n->forwarder, f, now);
        break;
    }
}

/*
 * Send one route update per neighbor.
 *
 * Split horizon is per-recipient by nature: a route learned from neighbor A
 * must not be advertised back to A, but should still be advertised to B. That
 * requires a distinct update per neighbor rather than one broadcast, so each
 * is addressed to its recipient.
 */
static void send_route_update(hh_node_t *n, hh_time_ms_t now)
{
    hh_node_id_t neighbors[HH_MAX_NEIGHBORS];
    size_t count = hh_neighbor_list(&n->neighbors, neighbors, HH_MAX_NEIGHBORS);

    for (size_t i = 0; i < count; i++) {
        hh_route_update_t u;
        hh_frame_t f;
        size_t len;

        if (hh_routing_build_update_for(&n->routing, neighbors[i], &u) == 0) continue;

        memset(&f, 0, sizeof f);
        f.kind = HH_FRAME_ROUTING;
        f.src  = n->cfg.node_id;
        f.dst  = neighbors[i];
        len = hh_route_update_encode(&u, f.data, sizeof f.data);
        if (len == 0) continue;
        f.len = (uint16_t)len;

        if (hh_radio_transmit(n->radio, &f) == HH_OK) {
            n->route_updates_sent++;
            HH_LOGD(COMP, "route_update_tx", "node=%u to=%u entries=%u t_ms=%llu",
                    n->cfg.node_id, neighbors[i], u.count, (unsigned long long)now);
        }
    }
}

hh_status_t hh_node_tick(hh_node_t *n, hh_time_ms_t now)
{
    if (!n) return HH_ERR_INVAL;
    if (n->state != HH_NODE_RUNNING) return HH_ERR_STATE;
    n->ticks++;

    /* 1. Service the radio: inbound frames enter through the rx callback. */
    hh_radio_poll(n->radio, now);
    hh_dispatcher_drain_all(&n->bus, 8);

    /* 2. Discovery beacon, if due. */
    hh_discovery_tick(&n->discovery, now);

    /* 3. Proactive route update stream (Doc 1 §6). */
    if (now - n->last_route_update_at >= n->cfg.route_update_interval_ms) {
        send_route_update(n, now);
        n->last_route_update_at = now;
    }

    /* 4. Timers: neighbor expiry, link health, route aging, recovery retries.
     * Ordered so each stage sees the previous stage's events this same tick. */
    hh_neighbor_tick(&n->neighbors, now);
    hh_dispatcher_drain_all(&n->bus, 8);

    hh_link_health_tick(&n->link_health, now);
    hh_dispatcher_drain_all(&n->bus, 8);

    hh_routing_tick(&n->routing, now);
    hh_sh_tick(&n->self_healing, now);
    hh_topology_evaluate(&n->topology, now);
    hh_dispatcher_drain_all(&n->bus, 8);

    /* 5. Retry packets buffered during a route miss. */
    hh_forwarder_flush(&n->forwarder, now, n->cfg.route_active_timeout_ms);
    return HH_OK;
}

hh_status_t hh_node_send(hh_node_t *n, hh_node_id_t dst, const uint8_t *payload,
                         uint16_t len, hh_time_ms_t now)
{
    if (!n) return HH_ERR_INVAL;
    if (n->state != HH_NODE_RUNNING) return HH_ERR_STATE;
    return hh_forwarder_send(&n->forwarder, dst, payload, len,
                             n->cfg.max_hop_count, now);
}
