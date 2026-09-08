#include "hhsdr/manet/self_healing.h"
#include "hhsdr/core/log.h"
#include <string.h>

#define COMP "selfheal"

hh_status_t hh_sh_init(hh_self_healing_t *sh, const hh_config_t *cfg,
                       const hh_clock_t *clock, hh_dispatcher_t *bus,
                       hh_routing_t *routing, hh_topology_t *topology,
                       hh_radio_t *radio)
{
    if (!sh || !cfg || !clock || !bus || !routing) return HH_ERR_INVAL;
    memset(sh, 0, sizeof *sh);
    sh->cfg = cfg;
    sh->clock = clock;
    sh->bus = bus;
    sh->routing = routing;
    sh->topology = topology;
    sh->radio = radio;
    return HH_OK;
}

static hh_recovery_t *rec_slot(hh_self_healing_t *sh, hh_node_id_t target)
{
    hh_recovery_t *freeslot = NULL;
    for (size_t i = 0; i < HH_SH_MAX_ACTIVE; i++) {
        if (sh->active[i].used && sh->active[i].target == target) return &sh->active[i];
        if (!sh->active[i].used && !freeslot) freeslot = &sh->active[i];
    }
    if (freeslot) {
        memset(freeslot, 0, sizeof *freeslot);
        freeslot->used = true;
        freeslot->target = target;
    }
    return freeslot;
}

static void publish_recovery(hh_self_healing_t *sh, hh_event_type_t type,
                             const hh_recovery_t *r, bool succeeded,
                             bool hold_down, hh_time_ms_t now)
{
    hh_event_t ev;
    memset(&ev, 0, sizeof ev);
    ev.type = type;
    ev.timestamp = now;
    ev.u.recovery.target           = r->target;
    ev.u.recovery.strategy         = r->strategy;
    ev.u.recovery.hold_down_active = hold_down;
    ev.u.recovery.succeeded        = succeeded;
    hh_dispatcher_publish(sh->bus, &ev);
}

/*
 * RF-interference path: "where the cause-hint indicates interference
 * rather than a node problem, Self-Healing prefers a channel/frequency change
 * over route churn."
 * Returns true when the channel change was accepted.
 */
static bool try_channel_change(hh_self_healing_t *sh, hh_time_ms_t now)
{
    hh_radio_status_t st;
    hh_status_t rc;
    uint32_t next_channel;

    if (!sh->radio) return false;
    if (hh_radio_get_status(sh->radio, &st) != HH_OK) return false;

    next_channel = st.channel + 1;
    rc = hh_radio_set_channel(sh->radio, next_channel);
    if (rc == HH_OK) {
        sh->channel_changes++;
        HH_LOGI(COMP, "channel_changed", "old=%u new=%u reason=rf_interference",
                st.channel, next_channel);
        return true;
    }

    /* Channel-change failure behavior is left undefined by the interface
     * specification. This
     * stack falls back to route-based recovery and records the decision rather
     * than treating the recovery as failed. */
    sh->channel_change_failures++;
    HH_LOGW(COMP, "channel_change_failed", "status=%s fallback=route_recovery",
            hh_status_str(rc));
    (void)now;
    return false;
}

hh_status_t hh_sh_on_failure(hh_self_healing_t *sh, const hh_ev_failure_t *ev,
                             hh_time_ms_t now)
{
    hh_recovery_t *r;
    hh_node_id_t failed;
    size_t affected;
    const hh_route_t *route;

    if (!sh || !ev) return HH_ERR_INVAL;
    failed = ev->neighbor_or_node_id;

    r = rec_slot(sh, failed);
    if (!r) return HH_ERR_NOMEM;
    if (r->used && r->started_at && !r->completed) return HH_ERR_AGAIN; /* already active */

    r->started_at = now;
    r->completed  = false;
    r->retries    = 0;
    sh->recoveries_started++;

    HH_LOGI(COMP, "recovery_started", "target=%u cause=%s",
            failed, hh_cause_hint_str(ev->cause_hint));

    /* Classification decides the strategy before any route churn happens. */
    if (ev->cause_hint == HH_CAUSE_RF_INTERFERENCE && try_channel_change(sh, now)) {
        r->strategy = HH_RECOVERY_CHANNEL_CHANGE;
        publish_recovery(sh, HH_EV_RECOVERY_STARTED, r, false, false, now);
        /* Routes are left intact: the link may well return on the new channel. */
        return HH_OK;
    }

    /* Route invalidation cascade. Routing promotes a warm alternate where one
     * exists, which is the "switch without full recompute" path. */
    affected = hh_routing_invalidate_via(sh->routing, failed,
                                         HH_WITHDRAW_FAILURE_CASCADE, now);

    /* Penalise a next hop that has been flapping. */
    hh_routing_damp(sh->routing, failed, now);

    route = hh_routing_get(sh->routing, failed);
    if (route && route->valid) {
        /* An alternate path to the failed node itself survived. */
        r->strategy = HH_RECOVERY_ALTERNATE_ROUTE;
        sh->alternate_switches++;
    } else {
        r->strategy = HH_RECOVERY_REDISCOVERY;
        r->next_retry_at = now + sh->cfg->rediscovery_backoff_ms;
        sh->rediscoveries++;
    }

    HH_LOGI(COMP, "recovery_strategy", "target=%u strategy=%s routes_affected=%zu",
            failed, hh_recovery_strategy_str(r->strategy), affected);
    publish_recovery(sh, HH_EV_RECOVERY_STARTED, r, false, false, now);
    return HH_OK;
}

void hh_sh_on_partition(hh_self_healing_t *sh, const hh_ev_partition_t *ev,
                        hh_time_ms_t now)
{
    if (!sh || !ev) return;
    /* Each partition continues operating fully within itself, and no
     * cross-partition route is asserted on stale information — those entries
     * age out normally. So there is deliberately no forced flush here. */
    HH_LOGW(COMP, "partition_response", "branch_root=%u unreachable=%u "
            "action=continue_local_operation", ev->branch_root, ev->unreachable_count);
    (void)now;
}

void hh_sh_on_merge(hh_self_healing_t *sh, const hh_ev_merged_t *ev, hh_time_ms_t now)
{
    if (!sh || !ev) return;
    /* A hold-down delays trusting newly-merged routes as primary
     * until sequence-number freshness has settled across both halves. The
     * Topology Manager owns the window; recovery records that it is in force. */
    HH_LOGI(COMP, "merge_response", "rejoined_neighbor=%u hold_down_until=%llu",
            ev->rejoined_neighbor, (unsigned long long)ev->hold_down_until);
    (void)now;
}

size_t hh_sh_tick(hh_self_healing_t *sh, hh_time_ms_t now)
{
    size_t completed = 0;

    if (!sh) return 0;
    for (size_t i = 0; i < HH_SH_MAX_ACTIVE; i++) {
        hh_recovery_t *r = &sh->active[i];
        const hh_route_t *route;
        bool hold_down;

        if (!r->used || r->completed) continue;

        route = hh_routing_get(sh->routing, r->target);

        /* Connectivity restored: stabilize under hold-down before the route is
         * trusted as primary. */
        if (route && route->valid) {
            hold_down = now < route->hold_down_until;
            r->completed = true;
            sh->recoveries_completed++;
            completed++;
            HH_LOGI(COMP, "recovery_completed", "target=%u strategy=%s hold_down=%d",
                    r->target, hh_recovery_strategy_str(r->strategy), (int)hold_down);
            publish_recovery(sh, HH_EV_RECOVERY_COMPLETED, r, true, hold_down, now);
            continue;
        }

        if (r->strategy != HH_RECOVERY_REDISCOVERY) continue;
        if (now < r->next_retry_at) continue;

        if (r->retries >= sh->cfg->rediscovery_max_retries) {
            /* Bounded retry: give up rather than retrying forever. The node
             * stays discoverable through the ordinary beacon stream. */
            r->completed = true;
            completed++;
            HH_LOGW(COMP, "recovery_completed", "target=%u strategy=rediscovery "
                    "result=exhausted retries=%u", r->target, r->retries);
            publish_recovery(sh, HH_EV_RECOVERY_COMPLETED, r, false, false, now);
            continue;
        }

        /* Exponential backoff with TTL growth, mirroring _retry_discovery. */
        r->retries++;
        r->next_retry_at = now + (hh_time_ms_t)sh->cfg->rediscovery_backoff_ms * (1u << r->retries);
        HH_LOGD(COMP, "rediscovery_retry", "target=%u attempt=%u next_ms=%llu",
                r->target, r->retries, (unsigned long long)r->next_retry_at);
    }
    return completed;
}

bool hh_sh_is_recovering(const hh_self_healing_t *sh, hh_node_id_t target)
{
    if (!sh) return false;
    for (size_t i = 0; i < HH_SH_MAX_ACTIVE; i++)
        if (sh->active[i].used && sh->active[i].target == target)
            return !sh->active[i].completed;
    return false;
}

size_t hh_sh_active_count(const hh_self_healing_t *sh)
{
    size_t n = 0;
    if (!sh) return 0;
    for (size_t i = 0; i < HH_SH_MAX_ACTIVE; i++)
        if (sh->active[i].used && !sh->active[i].completed) n++;
    return n;
}
