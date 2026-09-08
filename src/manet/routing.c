#include "hhsdr/manet/routing.h"
#include "hhsdr/core/log.h"
#include "hhsdr/core/seq.h"
#include <string.h>

#define COMP "routing"

hh_status_t hh_routing_init(hh_routing_t *r, const hh_config_t *cfg,
                            const hh_clock_t *clock, hh_dispatcher_t *bus,
                            const hh_neighbor_mgr_t *neighbors,
                            const hh_link_health_t *link_health)
{
    if (!r || !cfg || !clock || !bus) return HH_ERR_INVAL;
    memset(r, 0, sizeof *r);
    r->cfg = cfg;
    r->clock = clock;
    r->bus = bus;
    r->neighbors = neighbors;
    r->link_health = link_health;
    hh_route_publisher_init(&r->publisher);
    return HH_OK;
}

static hh_route_t *find(hh_routing_t *r, hh_node_id_t dst)
{
    for (size_t i = 0; i < HH_ROUTING_MAX; i++)
        if (r->routes[i].used && r->routes[i].destination == dst) return &r->routes[i];
    return NULL;
}

static hh_route_t *alloc(hh_routing_t *r)
{
    size_t limit = r->cfg->max_routes < HH_ROUTING_MAX ? r->cfg->max_routes : HH_ROUTING_MAX;
    if (r->count >= limit) return NULL;
    for (size_t i = 0; i < HH_ROUTING_MAX; i++)
        if (!r->routes[i].used) { r->count++; return &r->routes[i]; }
    return NULL;
}

/*
 * Rebuild and atomically publish the snapshot the forwarder reads.
 * Called after any mutation; the cost stays off the fast path entirely.
 */
static void republish(hh_routing_t *r)
{
    hh_route_snapshot_t *d = hh_route_publisher_begin(&r->publisher);
    size_t n = 0;

    if (!d) return;
    for (size_t i = 0; i < HH_ROUTING_MAX && n < HH_MAX_ROUTES; i++) {
        const hh_route_t *src = &r->routes[i];
        hh_route_entry_t *e;
        if (!src->used) continue;
        e = &d->entries[n++];
        e->destination   = src->destination;
        e->next_hop      = src->next_hop;
        e->metric        = src->metric;
        e->sequence_no   = src->sequence_no;
        e->hop_count     = src->hop_count;
        e->valid         = src->valid;
        e->alt_next_hop  = src->alt_next_hop;
        e->alt_metric    = src->alt_metric;
        e->alt_hop_count = src->alt_hop_count;
        e->has_alt       = src->has_alt;
    }
    d->count = n;
    hh_route_publisher_commit(&r->publisher, d);
}

static void publish_installed(hh_routing_t *r, const hh_route_t *rt, hh_time_ms_t now)
{
    hh_event_t ev;
    memset(&ev, 0, sizeof ev);
    ev.type = HH_EV_ROUTE_INSTALLED;
    ev.timestamp = now;
    ev.u.route_installed.destination = rt->destination;
    ev.u.route_installed.next_hop    = rt->next_hop;
    ev.u.route_installed.metric      = rt->metric;
    ev.u.route_installed.sequence_no = rt->sequence_no;
    ev.u.route_installed.hop_count   = rt->hop_count;
    hh_dispatcher_publish(r->bus, &ev);
}

static void publish_withdrawn(hh_routing_t *r, hh_node_id_t dst, hh_node_id_t nh,
                              hh_withdraw_reason_t reason, hh_time_ms_t now)
{
    hh_event_t ev;
    memset(&ev, 0, sizeof ev);
    ev.type = HH_EV_ROUTE_WITHDRAWN;
    ev.timestamp = now;
    ev.u.route_withdrawn.destination = dst;
    ev.u.route_withdrawn.next_hop    = nh;
    ev.u.route_withdrawn.reason      = reason;
    hh_dispatcher_publish(r->bus, &ev);
}

/*
 * Composite metric. Lower is better.
 *
 * Terms: link quality from the Link Health score (which already fuses RSSI,
 * SNR, PER, retransmits and latency), a bounded hop-count penalty, and a route
 * stability/age term that damps flapping by preferring a proven route over a
 * marginally better just-appeared one. Hop count is deliberately a penalty
 * term, never the primary metric.
 */
static float composite_metric(const hh_routing_t *r, hh_node_id_t next_hop,
                              uint8_t hop_count, float upstream_metric,
                              hh_time_ms_t now)
{
    const hh_config_t *c = r->cfg;
    float quality = 1.0f;
    float cost;

    if (r->link_health) {
        const hh_link_t *l = hh_link_health_get(r->link_health, next_hop);
        if (l) {
            quality = l->score;
            /* A Degraded link loses the comparison to a healthier alternate
             * before it fails outright — the "prefer proactive route
             * replacement" path, handled entirely inside the metric. */
            if (l->state == HH_LINK_DEGRADED)           quality *= 0.5f;
            else if (l->state == HH_LINK_SUSPECTED_FAILURE) quality *= 0.2f;
            else if (l->state == HH_LINK_FAILED)        quality = 0.01f;
            else if (l->state == HH_LINK_RECOVERING)    quality *= 0.6f;
        }
    }
    if (quality < 0.01f) quality = 0.01f;

    cost  = c->metric_w_quality * (1.0f / quality);
    cost += c->metric_w_hop * (float)hop_count;
    cost += upstream_metric;                   /* accumulated path cost */

    /* Dampening penalty for a next-hop that has flapped repeatedly. */
    if (hh_routing_is_damped(r, next_hop, now)) cost *= 3.0f;

    return cost;
}

/*
 *  selection rule, generalized from the prototype's hop-count-only
 * _should_replace_route: strictly fresher sequence always wins; at equal
 * sequence, the better composite metric wins.
 */
static bool should_replace(const hh_route_t *cur, hh_seq_t seq, float metric,
                           float cur_metric_now, hh_time_ms_t now)
{
    if (!cur->used || !cur->valid) return true;
    if (hh_seq_gt(seq, cur->sequence_no)) return true;
    if (hh_seq_lt(seq, cur->sequence_no)) return false;   /* stale */

    /* Equal sequence: a hold-down window protects a freshly installed route
     * from being replaced again immediately. */
    if (now < cur->hold_down_until) return false;

    /* Compare against the incumbent's metric recomputed at CURRENT link health,
     * not the value frozen at install time. the architecture requires a degraded link to
     * "lose the comparison to a healthier alternate on the next update"; that
     * cannot happen if the incumbent is still judged by how good its link was
     * when the route was installed. */
    return metric < cur_metric_now;
}

hh_status_t hh_routing_offer(hh_routing_t *r, hh_node_id_t destination,
                             hh_node_id_t via_neighbor, hh_seq_t seq,
                             uint8_t hop_count, float neighbor_metric,
                             hh_time_ms_t now)
{
    hh_route_t *rt;
    float metric;
    bool is_new = false;

    if (!r || destination == HH_NODE_ID_INVALID) return HH_ERR_INVAL;
    if (destination == r->cfg->node_id) return HH_ERR_INVAL;   /* never route to self */

    /* Poisoned reverse: the sender is telling us this destination is
     * unreachable through them. Withdraw the route we hold via that neighbor
     * rather than treating it as a very long path. */
    if (hop_count >= (uint8_t)HH_HOP_INFINITY) {
        hh_route_t *victim = find(r, destination);
        if (victim && victim->valid && victim->next_hop == via_neighbor) {
            victim->valid = false;
            victim->invalidated_at = now;
            victim->has_alt = false;
            r->withdrawals++;
            HH_LOGI(COMP, "route_withdrawn", "dst=%u next_hop=%u reason=poisoned_reverse",
                    destination, via_neighbor);
            publish_withdrawn(r, destination, via_neighbor, HH_WITHDRAW_EXPLICIT, now);
            republish(r);
        }
        return HH_ERR_AGAIN;
    }
    if (hop_count >= r->cfg->max_hop_count) return HH_ERR_INVAL;

    /* A route is only as valid as its next-hop neighbor entry. */
    if (r->neighbors && !hh_neighbor_get(r->neighbors, via_neighbor))
        return HH_ERR_NOTFOUND;

    metric = composite_metric(r, via_neighbor, hop_count, neighbor_metric, now);

    rt = find(r, destination);
    if (!rt) {
        rt = alloc(r);
        if (!rt) return HH_ERR_NOMEM;
        memset(rt, 0, sizeof *rt);
        rt->used = true;
        rt->destination = destination;
        is_new = true;
    } else if (!should_replace(rt, seq, metric,
                               composite_metric(r, rt->next_hop, rt->hop_count,
                                                0.0f, now), now)) {
        /* Not better — but if it is a distinct viable next hop, keep it as the
         * warm standby so Self-Healing can switch without a recompute. */
        if (rt->next_hop != via_neighbor && (!rt->has_alt || metric < rt->alt_metric)) {
            rt->alt_next_hop  = via_neighbor;
            rt->alt_metric    = metric;
            rt->alt_hop_count = hop_count;
            rt->has_alt       = true;
            republish(r);
        }
        r->rejected_stale++;
        return HH_ERR_AGAIN;
    } else if (rt->valid && rt->next_hop != via_neighbor) {
        /* The outgoing primary becomes the standby. */
        rt->alt_next_hop  = rt->next_hop;
        rt->alt_metric    = rt->metric;
        rt->alt_hop_count = rt->hop_count;
        rt->has_alt       = true;
        r->replacements++;
    }

    rt->next_hop     = via_neighbor;
    rt->learned_from = via_neighbor;
    rt->metric       = metric;
    rt->sequence_no  = seq;
    rt->hop_count    = hop_count;
    rt->valid        = true;
    rt->installed_at = now;
    rt->last_used    = now;
    rt->invalidated_at = 0;
    rt->hold_down_until = now + r->cfg->hold_down_ms;

    r->installs++;
    HH_LOGI(COMP, "route_installed", "dst=%u next_hop=%u metric=%.3f seq=%u hops=%u new=%d",
            destination, via_neighbor, (double)metric, seq, hop_count, (int)is_new);
    republish(r);
    publish_installed(r, rt, now);
    return HH_OK;
}

hh_status_t hh_routing_on_neighbor_up(hh_routing_t *r, hh_node_id_t id, hh_time_ms_t now)
{
    hh_status_t st;
    hh_route_t *rt;

    if (!r) return HH_ERR_INVAL;
    /* A direct neighbor is a one-hop destination reachable via itself. Our own
     * sequence advances so this beats any stale multi-hop route to it. */
    r->own_seq++;
    st = hh_routing_offer(r, id, id, r->own_seq, 1, 0.0f, now);

    /* A directly-observed route was not learned from an advertisement, so it is
     * exempt from split horizon: we may tell every neighbor about it. */
    rt = find(r, id);
    if (rt && rt->next_hop == id) rt->learned_from = HH_NODE_ID_INVALID;
    return st;
}

size_t hh_routing_invalidate_via(hh_routing_t *r, hh_node_id_t next_hop,
                                 hh_withdraw_reason_t reason, hh_time_ms_t now)
{
    size_t affected = 0;

    if (!r) return 0;
    for (size_t i = 0; i < HH_ROUTING_MAX; i++) {
        hh_route_t *rt = &r->routes[i];
        if (!rt->used || !rt->valid || rt->next_hop != next_hop) continue;

        /* Promote the warm alternate immediately if it does not also depend on
         * the failed next hop — this is the "switch without full recompute"
         * path. */
        if (rt->has_alt && rt->alt_next_hop != next_hop &&
            (!r->neighbors || hh_neighbor_get(r->neighbors, rt->alt_next_hop))) {
            HH_LOGI(COMP, "route_switched_to_alternate",
                    "dst=%u failed_next_hop=%u new_next_hop=%u metric=%.3f",
                    rt->destination, next_hop, rt->alt_next_hop, (double)rt->alt_metric);
            rt->next_hop  = rt->alt_next_hop;
            rt->metric    = rt->alt_metric;
            rt->hop_count = rt->alt_hop_count;
            rt->has_alt   = false;
            rt->installed_at = now;
            rt->hold_down_until = now + r->cfg->hold_down_ms;
            publish_installed(r, rt, now);
        } else {
            /* Two-phase invalidation: mark invalid now so forwarding stops
             * immediately, delete after the grace window. */
            rt->valid = false;
            rt->invalidated_at = now;
            rt->has_alt = false;
            r->withdrawals++;
            HH_LOGI(COMP, "route_withdrawn", "dst=%u next_hop=%u reason=%s",
                    rt->destination, next_hop, hh_withdraw_reason_str(reason));
            publish_withdrawn(r, rt->destination, next_hop, reason, now);
        }
        affected++;
    }
    if (affected) republish(r);
    return affected;
}

size_t hh_routing_tick(hh_routing_t *r, hh_time_ms_t now)
{
    size_t changed = 0;

    if (!r) return 0;
    for (size_t i = 0; i < HH_ROUTING_MAX; i++) {
        hh_route_t *rt = &r->routes[i];
        if (!rt->used) continue;

        if (rt->valid) {
            /* Refresh the stored metric so telemetry, advertisements, and the
             * published snapshot reflect the link's CURRENT health rather than
             * its health at install time. */
            rt->metric = composite_metric(r, rt->next_hop, rt->hop_count, 0.0f, now);

            /*
             * A route whose next hop is still a live neighbor is being actively
             * maintained by the proactive update stream, so it is not idle even
             * when no data traffic uses it. Ageing it out here would expire
             * perfectly good routes on a quiet network -- which a multi-node
             * scenario caught: a merely Degraded link lost its route because
             * nothing had sent packets over it.
             *
             * ACTIVE_ROUTE_TIMEOUT still applies to routes whose next hop has
             * gone, which is what the timer is actually for.
             */
            if (r->neighbors && hh_neighbor_get(r->neighbors, rt->next_hop)) {
                rt->last_used = now;
            }

            /* Expire on inactivity (ACTIVE_ROUTE_TIMEOUT analogue). */
            if (now - rt->last_used > r->cfg->route_active_timeout_ms) {
                rt->valid = false;
                rt->invalidated_at = now;
                r->withdrawals++;
                HH_LOGI(COMP, "route_withdrawn", "dst=%u next_hop=%u reason=expired",
                        rt->destination, rt->next_hop);
                publish_withdrawn(r, rt->destination, rt->next_hop,
                                  HH_WITHDRAW_EXPIRED, now);
                changed++;
            }
        } else if (rt->invalidated_at &&
                   now - rt->invalidated_at > r->cfg->route_delete_period_ms) {
            /* Grace window elapsed: now actually delete (DELETE_PERIOD). */
            HH_LOGD(COMP, "route_deleted", "dst=%u", rt->destination);
            memset(rt, 0, sizeof *rt);
            r->count--;
            changed++;
        }
    }
    if (changed) republish(r);
    return changed;
}

size_t hh_routing_build_update_for(const hh_routing_t *r, hh_node_id_t to,
                                   hh_route_update_t *out)
{
    size_t n = 0;
    if (!r || !out) return 0;

    memset(out, 0, sizeof *out);
    out->sender = r->cfg->node_id;
    for (size_t i = 0; i < HH_ROUTING_MAX && n < HH_ROUTE_UPDATE_MAX_ENTRIES; i++) {
        const hh_route_t *rt = &r->routes[i];
        bool poison;
        if (!rt->used) continue;

        /* Never tell a node about a route to itself: it knows whether it
         * exists, and our view of it is at best one hop stale. */
        if (to != HH_NODE_ID_INVALID && rt->destination == to) continue;

        /* A route that was never installed carries no information. */
        if (!rt->valid && rt->invalidated_at == 0) continue;

        /* Split horizon with poisoned reverse. Advertising a route back to the
         * neighbor it was learned from lets the two of them believe each other
         * still has a path after the real one breaks, and their hop counts climb
         * together -- a count-to-infinity loop, which a multi-node scenario
         * exposed. Sending an infinite hop count actively withdraws the route,
         * converging faster than falling silent would.
         *
         * A directly-connected neighbor has learned_from == INVALID, so a
         * direct route is never poisoned by this rule. */
        poison = (!rt->valid) ||
                 (to != HH_NODE_ID_INVALID &&
                  rt->learned_from != HH_NODE_ID_INVALID &&
                  rt->learned_from == to);

        out->entries[n].originator  = rt->destination;
        out->entries[n].sequence_no = rt->sequence_no;
        out->entries[n].hop_count   = poison ? (uint8_t)HH_HOP_INFINITY : rt->hop_count;
        /* Advertise a normalized quality, not our raw cost: the receiver
         * recomputes cost against its own link to us. */
        out->entries[n].metric = rt->metric > 1.0f ? 1.0f : rt->metric;
        n++;
    }
    out->count = (uint8_t)n;
    return n;
}

size_t hh_routing_build_update(const hh_routing_t *r, hh_route_update_t *out)
{
    return hh_routing_build_update_for(r, HH_NODE_ID_INVALID, out);
}

void hh_routing_damp(hh_routing_t *r, hh_node_id_t next_hop, hh_time_ms_t now)
{
    if (!r) return;
    for (size_t i = 0; i < 32; i++) {
        if (r->damped[i].used && r->damped[i].id == next_hop) {
            r->damped[i].until = now + r->cfg->dampening_penalty_ms;
            return;
        }
    }
    for (size_t i = 0; i < 32; i++) {
        if (!r->damped[i].used) {
            r->damped[i].used  = true;
            r->damped[i].id    = next_hop;
            r->damped[i].until = now + r->cfg->dampening_penalty_ms;
            HH_LOGI(COMP, "next_hop_damped", "next_hop=%u until_ms=%llu",
                    next_hop, (unsigned long long)r->damped[i].until);
            return;
        }
    }
}

bool hh_routing_is_damped(const hh_routing_t *r, hh_node_id_t next_hop, hh_time_ms_t now)
{
    if (!r) return false;
    for (size_t i = 0; i < 32; i++)
        if (r->damped[i].used && r->damped[i].id == next_hop)
            return now < r->damped[i].until;
    return false;
}

const hh_route_t *hh_routing_get(const hh_routing_t *r, hh_node_id_t dst)
{
    if (!r) return NULL;
    for (size_t i = 0; i < HH_ROUTING_MAX; i++)
        if (r->routes[i].used && r->routes[i].destination == dst) return &r->routes[i];
    return NULL;
}

const hh_route_snapshot_t *hh_routing_snapshot(const hh_routing_t *r)
{
    return r ? hh_route_publisher_current(&r->publisher) : NULL;
}

size_t hh_routing_count(const hh_routing_t *r) { return r ? r->count : 0; }

void hh_routing_mark_used(hh_routing_t *r, hh_node_id_t dst, hh_time_ms_t now)
{
    hh_route_t *rt;
    if (!r) return;
    rt = find(r, dst);
    if (rt) rt->last_used = now;
}
