#include "hhsdr/manet/neighbor.h"
#include "hhsdr/core/log.h"
#include "hhsdr/core/seq.h"
#include <string.h>

#define COMP "neighbor"

hh_status_t hh_neighbor_init(hh_neighbor_mgr_t *nm, const hh_config_t *cfg,
                             const hh_clock_t *clock, hh_dispatcher_t *bus)
{
    if (!nm || !cfg || !clock || !bus) return HH_ERR_INVAL;
    memset(nm, 0, sizeof *nm);
    nm->cfg = cfg;
    nm->clock = clock;
    nm->bus = bus;
    return HH_OK;
}

static hh_neighbor_t *find(hh_neighbor_mgr_t *nm, hh_node_id_t id)
{
    for (size_t i = 0; i < HH_MAX_NEIGHBORS; i++)
        if (nm->table[i].used && nm->table[i].id == id) return &nm->table[i];
    return NULL;
}

static void publish_neighbor_event(hh_neighbor_mgr_t *nm, hh_event_type_t type,
                                   const hh_neighbor_t *n, hh_time_ms_t now)
{
    hh_event_t ev;
    memset(&ev, 0, sizeof ev);
    ev.type = type;
    ev.timestamp = now;
    ev.u.neighbor.neighbor_id  = n->id;
    ev.u.neighbor.timestamp    = now;
    ev.u.neighbor.capabilities = n->capabilities;
    ev.u.neighbor.radio_caps   = n->radio_caps;
    hh_dispatcher_publish(nm->bus, &ev);
}

/* LRU eviction: the least recently heard entry loses its slot. Bounded table
 * size is a hard requirement on the embedded target (Doc 1 §4). */
static hh_neighbor_t *evict_lru(hh_neighbor_mgr_t *nm, hh_time_ms_t now)
{
    hh_neighbor_t *victim = NULL;
    for (size_t i = 0; i < HH_MAX_NEIGHBORS; i++) {
        if (!nm->table[i].used) continue;
        if (!victim || nm->table[i].last_heard < victim->last_heard)
            victim = &nm->table[i];
    }
    if (victim) {
        HH_LOGW(COMP, "evicted", "neighbor=%u reason=table_full last_heard=%llu",
                victim->id, (unsigned long long)victim->last_heard);
        nm->evictions++;
        publish_neighbor_event(nm, HH_EV_NEIGHBOR_DOWN, victim, now);
        memset(victim, 0, sizeof *victim);
        nm->count--;
    }
    return victim;
}

static hh_neighbor_t *alloc_slot(hh_neighbor_mgr_t *nm, hh_time_ms_t now)
{
    size_t limit = nm->cfg->max_neighbors < HH_MAX_NEIGHBORS
                 ? nm->cfg->max_neighbors : HH_MAX_NEIGHBORS;
    if (nm->count >= limit) {
        if (!evict_lru(nm, now)) return NULL;
    }
    for (size_t i = 0; i < HH_MAX_NEIGHBORS; i++)
        if (!nm->table[i].used) return &nm->table[i];
    return NULL;
}

hh_status_t hh_neighbor_on_beacon(hh_neighbor_mgr_t *nm, const hh_beacon_t *b,
                                  const hh_link_sample_t *sample, hh_time_ms_t now)
{
    hh_neighbor_t *n;
    bool is_new = false;
    uint32_t changed = 0;

    if (!nm || !b || !sample) return HH_ERR_INVAL;
    if (b->node_id == HH_NODE_ID_INVALID || b->node_id == nm->cfg->node_id)
        return HH_ERR_INVAL;

    n = find(nm, b->node_id);
    if (!n) {
        n = alloc_slot(nm, now);
        if (!n) return HH_ERR_NOMEM;
        memset(n, 0, sizeof *n);
        n->used        = true;
        n->id          = b->node_id;
        n->first_heard = now;
        n->last_seq    = b->sequence_no;
        nm->count++;
        is_new = true;
    } else {
        /* Estimate the sender's cadence from observed arrivals, so expiry can
         * be by cadence rather than a fixed wall-clock timeout (Doc 1 §4). */
        if (now > n->last_heard) {
            uint32_t gap = (uint32_t)(now - n->last_heard);
            n->observed_interval_ms = n->observed_interval_ms
                ? (n->observed_interval_ms * 3 + gap) / 4   /* smooth */
                : gap;
        }
        /* Sequence gap indicates beacons lost in flight — a link-health signal. */
        if (hh_seq_gt(b->sequence_no, n->last_seq)) {
            uint32_t adv = b->sequence_no - n->last_seq;
            if (adv > 1) n->beacons_missed += adv - 1;
        }
        n->last_seq = b->sequence_no;

        if (n->capabilities != b->capabilities) changed |= HH_NBR_ATTR_CAPABILITIES;
        if (n->radio_caps   != b->radio_caps)   changed |= HH_NBR_ATTR_RADIO_CAPS;
    }

    n->last_heard       = now;
    n->beacons_received++;
    n->capabilities     = b->capabilities;
    n->radio_caps       = b->radio_caps;
    n->routing_capable  = b->routing_capable;
    n->position_valid   = b->position_valid;
    n->position_x       = b->position_x;
    n->position_y       = b->position_y;
    n->position_z       = b->position_z;
    n->power_valid      = b->power_valid;
    n->power_battery    = b->power_battery;
    n->last_sample      = *sample;

    if (is_new) {
        nm->ups++;
        HH_LOGI(COMP, "neighbor_up", "neighbor=%u seq=%u routing_capable=%d rssi=%.1f",
                n->id, b->sequence_no, (int)n->routing_capable, (double)sample->rssi);
        publish_neighbor_event(nm, HH_EV_NEIGHBOR_UP, n, now);
    } else if (changed) {
        hh_event_t ev;
        nm->changes++;
        HH_LOGI(COMP, "neighbor_changed", "neighbor=%u attrs=0x%x", n->id, changed);
        memset(&ev, 0, sizeof ev);
        ev.type = HH_EV_NEIGHBOR_CHANGED;
        ev.timestamp = now;
        ev.u.neighbor_changed.neighbor_id        = n->id;
        ev.u.neighbor_changed.changed_attributes = changed;
        hh_dispatcher_publish(nm->bus, &ev);
    }
    return HH_OK;
}

hh_status_t hh_neighbor_record_sample(hh_neighbor_mgr_t *nm, const hh_link_sample_t *s,
                                      hh_time_ms_t now)
{
    hh_neighbor_t *n;
    hh_event_t ev;

    if (!nm || !s) return HH_ERR_INVAL;
    n = find(nm, s->neighbor_id);
    if (!n) return HH_ERR_NOTFOUND;

    n->last_sample = *s;
    (void)now;

    memset(&ev, 0, sizeof ev);
    ev.type = HH_EV_NEIGHBOR_CHANGED;
    ev.timestamp = now;
    ev.u.neighbor_changed.neighbor_id        = n->id;
    ev.u.neighbor_changed.changed_attributes = HH_NBR_ATTR_METRICS;
    hh_dispatcher_publish(nm->bus, &ev);
    return HH_OK;
}

size_t hh_neighbor_tick(hh_neighbor_mgr_t *nm, hh_time_ms_t now)
{
    size_t expired = 0;

    if (!nm) return 0;
    for (size_t i = 0; i < HH_MAX_NEIGHBORS; i++) {
        hh_neighbor_t *n = &nm->table[i];
        uint32_t cadence, deadline;
        if (!n->used) continue;

        /* Expiry by cadence: allowed_loss missed beacons at the interval that
         * neighbor was observed using, falling back to our own configured
         * interval before we have seen enough of theirs. */
        cadence = n->observed_interval_ms ? n->observed_interval_ms
                                          : nm->cfg->beacon_interval_ms;
        deadline = cadence * (nm->cfg->neighbor_allowed_loss + 1);

        if (now > n->last_heard && (now - n->last_heard) > deadline) {
            hh_node_id_t id = n->id;
            HH_LOGI(COMP, "neighbor_down", "neighbor=%u reason=expired "
                    "silent_ms=%llu deadline_ms=%u", id,
                    (unsigned long long)(now - n->last_heard), deadline);
            nm->downs++;
            publish_neighbor_event(nm, HH_EV_NEIGHBOR_DOWN, n, now);
            memset(n, 0, sizeof *n);
            nm->count--;
            expired++;
        }
    }
    return expired;
}

hh_status_t hh_neighbor_remove(hh_neighbor_mgr_t *nm, hh_node_id_t id, hh_time_ms_t now)
{
    hh_neighbor_t *n;
    if (!nm) return HH_ERR_INVAL;
    n = find(nm, id);
    if (!n) return HH_ERR_NOTFOUND;

    HH_LOGI(COMP, "neighbor_down", "neighbor=%u reason=removed", id);
    nm->downs++;
    publish_neighbor_event(nm, HH_EV_NEIGHBOR_DOWN, n, now);
    memset(n, 0, sizeof *n);
    nm->count--;
    return HH_OK;
}

const hh_neighbor_t *hh_neighbor_get(const hh_neighbor_mgr_t *nm, hh_node_id_t id)
{
    if (!nm) return NULL;
    for (size_t i = 0; i < HH_MAX_NEIGHBORS; i++)
        if (nm->table[i].used && nm->table[i].id == id) return &nm->table[i];
    return NULL;
}

size_t hh_neighbor_count(const hh_neighbor_mgr_t *nm) { return nm ? nm->count : 0; }

size_t hh_neighbor_list(const hh_neighbor_mgr_t *nm, hh_node_id_t *out, size_t cap)
{
    size_t n = 0;
    if (!nm || !out) return 0;
    for (size_t i = 0; i < HH_MAX_NEIGHBORS && n < cap; i++)
        if (nm->table[i].used) out[n++] = nm->table[i].id;
    return n;
}
