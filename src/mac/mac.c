#include "hhsdr/mac/mac.h"
#include "hhsdr/core/log.h"
#include <string.h>

#define COMP "mac"

static const uint16_t default_depth[HH_MAC_CLASS_COUNT] = { 16, 8, 8, 32, 64 };

void hh_mac_config_default(hh_mac_config_t *cfg, hh_node_id_t node_id,
                           const hh_clock_t *clock)
{
    if (!cfg) return;
    memset(cfg, 0, sizeof *cfg);
    cfg->node_id = node_id;
    cfg->clock = clock;
    cfg->max_payload = 1280;
    for (int k = 0; k < HH_MAC_CLASS_COUNT; k++) {
        cfg->queue_depth[k] = default_depth[k];
        cfg->hold_depth[k] = 16;
    }
    cfg->report_period_sf = 1;
}

static bool is_group(hh_node_id_t id)
{
    return id >= HH_MAC_GROUP_FIRST && id <= HH_MAC_GROUP_LAST;
}

static bool cfg_valid(const hh_mac_config_t *c)
{
    unsigned tx = 0, rx = 0;

    if (!c->clock) return false;
    if (c->node_id == HH_MAC_BROADCAST || c->node_id == HH_MAC_ID_RESERVED ||
        is_group(c->node_id)) return false;
    if (c->max_payload == 0 || c->max_payload > HH_MAC_PAYLOAD_MAX) return false;
    for (int k = 0; k < HH_MAC_CLASS_COUNT; k++) {
        if (c->queue_depth[k] == 0 || c->hold_depth[k] == 0) return false;
        tx += c->queue_depth[k];
        rx += c->hold_depth[k];
    }
    if (tx > HH_MAC_TX_POOL_CAP || rx > HH_MAC_RX_POOL_CAP) return false;
    if (c->ctl_cap_pdus_per_s && c->ctl_burst_pdus == 0) return false;
    if (c->ctl_cap_bytes_per_s && c->ctl_burst_bytes < c->max_payload) return false;
    return true;
}

static bool can_send(hh_mac_state_t s)
{
    return s == HH_MAC_STEADY || s == HH_MAC_HOLDOVER;
}

/* DP-9 option (a): deliver in every receiving state. */
static bool can_deliver(hh_mac_state_t s)
{
    return s != HH_MAC_OFF && s != HH_MAC_WAIT_TIME && s != HH_MAC_SAFE;
}

static hh_time_ms_t now(const hh_mac_t *mac)
{
    return hh_now(mac->cfg.clock);
}

static uint16_t ring_pop(hh_mac_ring_t *r)
{
    uint16_t i = r->idx[r->head];
    r->head = (uint16_t)((r->head + 1) % HH_MAC_TX_POOL_CAP);
    r->count--;
    return i;
}

static void ring_push(hh_mac_ring_t *r, uint16_t i)
{
    r->idx[(r->head + r->count) % HH_MAC_TX_POOL_CAP] = i;
    r->count++;
}

static uint16_t ring_peek(const hh_mac_ring_t *r)
{
    return r->idx[r->head];
}

static void pools_reset(hh_mac_t *mac)
{
    for (int k = 0; k < HH_MAC_CLASS_COUNT; k++) {
        mac->txq[k].head = mac->txq[k].count = 0;
        mac->hold[k].head = mac->hold[k].count = 0;
    }
    for (uint16_t i = 0; i < HH_MAC_TX_POOL_CAP; i++) mac->tx_free[i] = i;
    mac->tx_free_count = HH_MAC_TX_POOL_CAP;
    for (uint16_t i = 0; i < HH_MAC_RX_POOL_CAP; i++) mac->rx_free[i] = i;
    mac->rx_free_count = HH_MAC_RX_POOL_CAP;
    mac->ev_head = mac->ev_count = 0;
}

static void tx_release(hh_mac_t *mac, uint16_t i)
{
    mac->tx_free[mac->tx_free_count++] = i;
}

static void rx_release(hh_mac_t *mac, uint16_t i)
{
    mac->rx_free[mac->rx_free_count++] = i;
}

/* Deliver held frames and events, oldest first, until the network layer
 * refuses one. */
static void flush(hh_mac_t *mac)
{
    if (!mac->opened || !mac->cfg.receive || mac->delivering) return;
    mac->delivering = true;
    for (;;) {
        int best = -1;            /* class, or HH_MAC_CLASS_COUNT for an event */
        uint32_t best_seq = 0;
        hh_mac_indication_t ind;

        for (int k = 0; k < HH_MAC_CLASS_COUNT; k++) {
            if (!mac->hold[k].count) continue;
            uint32_t s = mac->rx_pool[ring_peek(&mac->hold[k])].seq;
            if (best < 0 || (int32_t)(s - best_seq) < 0) { best = k; best_seq = s; }
        }
        if (mac->ev_count) {
            uint32_t s = mac->events[mac->ev_head].seq;
            if (best < 0 || (int32_t)(s - best_seq) < 0) best = HH_MAC_CLASS_COUNT;
        }
        if (best < 0) break;

        memset(&ind, 0, sizeof ind);
        if (best == HH_MAC_CLASS_COUNT) {
            ind.type = HH_MAC_IND_EVENT;
            ind.event = &mac->events[mac->ev_head].ev;
        } else {
            ind.type = HH_MAC_IND_FRAME;
            ind.frame = &mac->rx_pool[ring_peek(&mac->hold[best])].f;
        }
        if (!mac->cfg.receive(mac->cfg.receive_ctx, &ind)) {
            mac->st.delivery_refused++;
            break;
        }
        if (best == HH_MAC_CLASS_COUNT) {
            mac->ev_head = (uint16_t)((mac->ev_head + 1) % HH_MAC_EVENT_CAP);
            mac->ev_count--;
        } else {
            rx_release(mac, ring_pop(&mac->hold[best]));
            mac->st.frames_rx++;
        }
    }
    mac->delivering = false;
}

static void raise_event(hh_mac_t *mac, hh_mac_event_kind_t kind, hh_node_id_t node,
                        uint32_t detail, const hh_mac_link_report_t *report)
{
    hh_mac_ev_entry_t *e;

    if (mac->ev_count == HH_MAC_EVENT_CAP) {
        mac->ev_head = (uint16_t)((mac->ev_head + 1) % HH_MAC_EVENT_CAP);
        mac->ev_count--;
        mac->st.event_overflow++;
    }
    e = &mac->events[(mac->ev_head + mac->ev_count) % HH_MAC_EVENT_CAP];
    memset(e, 0, sizeof *e);
    e->ev.kind = kind;
    e->ev.node = node;
    e->ev.superframe = mac->superframe;
    e->ev.detail = detail;
    if (report) e->ev.report = *report;
    e->seq = mac->seq++;
    mac->ev_count++;
    flush(mac);
}

hh_mac_result_t hh_mac_init(hh_mac_t *mac, const hh_mac_config_t *cfg)
{
    if (!mac) return HH_MAC_INVALID_ARGUMENT;
    memset(mac, 0, sizeof *mac);
    if (!cfg) return HH_MAC_INVALID_ARGUMENT;
    if (!cfg_valid(cfg)) {
        HH_LOGE(COMP, "init_failed", "reason=invalid_config node=%u", cfg->node_id);
        return HH_MAC_INVALID_CONFIG;
    }
    mac->cfg = *cfg;
    mac->state = HH_MAC_OFF;
    mac->max_payload = cfg->max_payload;
    pools_reset(mac);
    mac->initialized = true;
    HH_LOGI(COMP, "initialized",
            "node=%u max_payload=%u depth=%u/%u/%u/%u/%u min_share_n=%u",
            cfg->node_id, cfg->max_payload,
            cfg->queue_depth[0], cfg->queue_depth[1], cfg->queue_depth[2],
            cfg->queue_depth[3], cfg->queue_depth[4], cfg->min_share_n);
    return HH_MAC_OK;
}

hh_mac_result_t hh_mac_open(hh_mac_t *mac)
{
    if (!mac) return HH_MAC_INVALID_ARGUMENT;
    if (!mac->initialized) return HH_MAC_INVALID_CONFIG;
    if (mac->opened) return HH_MAC_OK;
    mac->opened = true;
    mac->state = HH_MAC_WAIT_TIME;
    mac->state_since = now(mac);
    mac->ctl_pdu_tokens = (uint64_t)mac->cfg.ctl_burst_pdus * 1000u;
    mac->ctl_byte_tokens = (uint64_t)mac->cfg.ctl_burst_bytes * 1000u;
    mac->ctl_refill_at = mac->state_since;
    HH_LOGI(COMP, "opened", "node=%u state=%s", mac->cfg.node_id, hh_mac_state_str(mac->state));
    return HH_MAC_OK;
}

hh_mac_result_t hh_mac_close(hh_mac_t *mac)
{
    if (!mac || !mac->initialized || !mac->opened) return HH_MAC_OK;
    for (int k = 0; k < HH_MAC_CLASS_COUNT; k++) {
        mac->st.close_discards[k] += mac->txq[k].count;
        mac->st.hold_close_discards += mac->hold[k].count;
    }
    pools_reset(mac);
    mac->slots_without_user = 0;
    mac->inhibit = HH_MAC_INHIBIT_NONE;
    mac->opened = false;
    mac->state = HH_MAC_OFF;
    HH_LOGI(COMP, "closed", "node=%u discarded_tx=%u/%u/%u/%u/%u",
            mac->cfg.node_id, mac->st.close_discards[0], mac->st.close_discards[1],
            mac->st.close_discards[2], mac->st.close_discards[3], mac->st.close_discards[4]);
    return HH_MAC_OK;
}

static bool class_valid(hh_frame_kind_t kind, hh_mac_class_t cls, hh_mac_class_t *out)
{
    if ((unsigned)cls >= HH_MAC_CLASS_COUNT) return false;
    switch (kind) {
    case HH_FRAME_BEACON:
    case HH_FRAME_ROUTING: *out = HH_MAC_CLASS_CONTROL; return true;
    case HH_FRAME_DATA:
        if (cls == HH_MAC_CLASS_CONTROL) return false;
        *out = cls;
        return true;
    }
    return false;
}

hh_mac_result_t hh_mac_send(hh_mac_t *mac, hh_frame_kind_t kind, hh_mac_class_t cls,
                            hh_node_id_t dst, uint16_t lifetime_ms,
                            const uint8_t *payload, uint16_t len)
{
    hh_mac_class_t c;
    hh_mac_ring_t *q;
    hh_mac_tx_entry_t *e;
    uint16_t i;

    if (!mac || !mac->initialized || !mac->opened) return HH_MAC_NOT_OPEN;
    if (!can_send(mac->state)) {
        mac->st.refused_state++;
        return HH_MAC_WRONG_STATE;
    }
    if (mac->inhibit != HH_MAC_INHIBIT_NONE) {
        mac->st.refused_inhibited++;
        return HH_MAC_TX_INHIBITED;
    }
    if (len > mac->max_payload) {
        mac->st.refused_too_large++;
        HH_LOGD(COMP, "send_refused", "reason=too_large len=%u max=%u", len, mac->max_payload);
        return HH_MAC_TOO_LARGE;
    }
    /* TODO: decide which destination ids are invalid (DP-11); none are refused now. */
    if (!class_valid(kind, cls, &c) || (!payload && len)) {
        mac->st.refused_invalid++;
        return HH_MAC_INVALID_ARGUMENT;
    }

    q = &mac->txq[c];
    if (q->count >= mac->cfg.queue_depth[c]) {
        if (c != HH_MAC_CLASS_VOICE && c != HH_MAC_CLASS_VIDEO) {
            mac->st.queue_full[c]++;
            HH_LOGD(COMP, "send_refused", "reason=queue_full class=%s depth=%u",
                    hh_mac_class_str(c), q->count);
            return HH_MAC_TRY_AGAIN;
        }
        tx_release(mac, ring_pop(q));
        mac->st.drop_oldest[c]++;
    }

    i = mac->tx_free[--mac->tx_free_count];
    e = &mac->tx_pool[i];
    e->f.kind = kind;
    e->f.cls = c;
    e->f.src = mac->cfg.node_id;
    e->f.dst = dst;
    e->f.len = len;
    if (len) memcpy(e->f.payload, payload, len);
    e->expires = lifetime_ms ? now(mac) + lifetime_ms : 0;
    ring_push(q, i);
    return HH_MAC_ACCEPTED;
}

hh_mac_result_t hh_mac_status(const hh_mac_t *mac, hh_mac_status_t *out)
{
    if (!mac || !out) return HH_MAC_INVALID_ARGUMENT;
    if (!mac->initialized || !mac->opened) return HH_MAC_NOT_OPEN;
    *out = mac->st;
    out->state = mac->state;
    switch (mac->state) {
    case HH_MAC_STEADY:
    case HH_MAC_HOLDOVER: out->operational = true;  out->operational_known = true; break;
    case HH_MAC_OFF:
    case HH_MAC_SAFE:     out->operational = false; out->operational_known = true; break;
    default:              out->operational = false; out->operational_known = false; break;
    }
    out->time_in_holdover_ms = mac->state == HH_MAC_HOLDOVER
        ? (uint32_t)(now(mac) - mac->state_since) : 0;
    out->tx_inhibit_reason = mac->inhibit;
    out->max_payload = mac->max_payload;
    out->node_id = mac->cfg.node_id;
    for (int k = 0; k < HH_MAC_CLASS_COUNT; k++) {
        out->queue_depth[k] = mac->txq[k].count;
        out->hold_depth[k] = mac->hold[k].count;
    }
    return HH_MAC_OK;
}

static void fill_report(const hh_mac_neighbor_t *n, hh_mac_link_report_t *out)
{
    /* Only last heard and receive count are measured; every valid bit stays clear. */
    memset(out, 0, sizeof *out);
    out->neighbor = n->id;
    out->last_heard_superframe = n->last_heard_superframe;
    out->rx_count = n->rx_count;
}

hh_mac_result_t hh_mac_link_metrics(const hh_mac_t *mac, hh_node_id_t neighbor,
                                    hh_mac_link_report_t *out)
{
    if (!mac || !out) return HH_MAC_INVALID_ARGUMENT;
    if (!mac->initialized || !mac->opened) return HH_MAC_NOT_OPEN;
    for (int i = 0; i < HH_MAC_NEIGHBOR_CAP; i++) {
        const hh_mac_neighbor_t *n = &mac->neighbors[i];
        if (n->used && n->id == neighbor) {
            fill_report(n, out);
            return HH_MAC_OK;
        }
    }
    return HH_MAC_NOT_FOUND;
}

hh_mac_result_t hh_mac_net_preset_select(hh_mac_t *mac, uint32_t preset)
{
    (void)preset;
    if (!mac || !mac->initialized || !mac->opened) return HH_MAC_NOT_OPEN;
    return HH_MAC_UNSUPPORTED;   /* TODO: preset meaning and encoding (OI-4) */
}

void hh_mac_flush(hh_mac_t *mac)
{
    if (mac) flush(mac);
}

/* Token bucket in thousandths, so a rate per second refills per millisecond. */
static void refill_control(hh_mac_t *mac)
{
    hh_time_ms_t t = now(mac);
    uint64_t dt = t - mac->ctl_refill_at;
    uint64_t cap;

    mac->ctl_refill_at = t;
    if (mac->cfg.ctl_cap_pdus_per_s) {
        cap = (uint64_t)mac->cfg.ctl_burst_pdus * 1000u;
        mac->ctl_pdu_tokens += dt * mac->cfg.ctl_cap_pdus_per_s;
        if (mac->ctl_pdu_tokens > cap) mac->ctl_pdu_tokens = cap;
    }
    if (mac->cfg.ctl_cap_bytes_per_s) {
        cap = (uint64_t)mac->cfg.ctl_burst_bytes * 1000u;
        mac->ctl_byte_tokens += dt * mac->cfg.ctl_cap_bytes_per_s;
        if (mac->ctl_byte_tokens > cap) mac->ctl_byte_tokens = cap;
    }
}

static bool control_allowed(const hh_mac_t *mac, uint16_t len)
{
    if (mac->cfg.ctl_cap_pdus_per_s && mac->ctl_pdu_tokens < 1000u) return false;
    if (mac->cfg.ctl_cap_bytes_per_s && mac->ctl_byte_tokens < (uint64_t)len * 1000u) return false;
    return true;
}

static void control_spend(hh_mac_t *mac, uint16_t len)
{
    if (mac->cfg.ctl_cap_pdus_per_s) mac->ctl_pdu_tokens -= 1000u;
    if (mac->cfg.ctl_cap_bytes_per_s) mac->ctl_byte_tokens -= (uint64_t)len * 1000u;
}

static void drop_expired(hh_mac_t *mac, hh_time_ms_t t)
{
    for (int k = 0; k < HH_MAC_CLASS_COUNT; k++) {
        hh_mac_ring_t *q = &mac->txq[k];
        while (q->count) {
            const hh_mac_tx_entry_t *e = &mac->tx_pool[ring_peek(q)];
            if (!e->expires || t < e->expires) break;
            tx_release(mac, ring_pop(q));
            mac->st.lifetime_expired[k]++;
        }
    }
}

bool hh_mac_next_pdu(hh_mac_t *mac, hh_mac_tx_frame_t *out)
{
    int user = -1;
    int pick = -1;
    bool share_due;
    hh_mac_ring_t *ctl;
    uint16_t i;

    if (!mac || !out || !mac->opened) return false;
    /* TODO: fate of queued frames outside STEADY and HOLDOVER or while inhibited (DP-3); kept for now. */
    if (!can_send(mac->state) || mac->inhibit != HH_MAC_INHIBIT_NONE) return false;

    drop_expired(mac, now(mac));
    refill_control(mac);

    for (int k = HH_MAC_CLASS_EMERGENCY; k < HH_MAC_CLASS_COUNT; k++)
        if (mac->txq[k].count) { user = k; break; }

    ctl = &mac->txq[HH_MAC_CLASS_CONTROL];
    share_due = user >= 0 && mac->cfg.min_share_n &&
                mac->slots_without_user + 1u >= mac->cfg.min_share_n;
    if (!share_due && ctl->count &&
        control_allowed(mac, mac->tx_pool[ring_peek(ctl)].f.len))
        pick = HH_MAC_CLASS_CONTROL;
    else if (user >= 0)
        pick = user;
    if (pick < 0) return false;

    i = ring_pop(&mac->txq[pick]);
    *out = mac->tx_pool[i].f;
    tx_release(mac, i);
    if (pick == HH_MAC_CLASS_CONTROL) {
        control_spend(mac, out->len);
        mac->slots_without_user = user >= 0 ? (uint16_t)(mac->slots_without_user + 1) : 0;
    } else {
        mac->slots_without_user = 0;
    }
    mac->st.frames_tx++;
    return true;
}

static hh_mac_neighbor_t *neighbor_find(hh_mac_t *mac, hh_node_id_t id, bool *added)
{
    hh_mac_neighbor_t *free_slot = NULL;

    *added = false;
    for (int i = 0; i < HH_MAC_NEIGHBOR_CAP; i++) {
        hh_mac_neighbor_t *n = &mac->neighbors[i];
        if (n->used && n->id == id) return n;
        if (!n->used && !free_slot) free_slot = n;
    }
    if (!free_slot) {
        mac->st.neighbor_table_full++;
        return NULL;
    }
    memset(free_slot, 0, sizeof *free_slot);
    free_slot->used = true;
    free_slot->id = id;
    *added = true;
    return free_slot;
}

void hh_mac_on_rx(hh_mac_t *mac, const hh_mac_rx_pdu_t *pdu)
{
    hh_mac_neighbor_t *n;
    hh_mac_ring_t *h;
    hh_mac_rx_entry_t *e;
    bool added;
    uint16_t i;

    if (!mac || !pdu || !mac->opened) return;
    if (pdu->len > HH_MAC_PAYLOAD_MAX || (!pdu->payload && pdu->len) ||
        (unsigned)pdu->cls >= HH_MAC_CLASS_COUNT) return;

    /* Every CRC-good PDU updates the neighbour record, before the address filter. */
    n = neighbor_find(mac, pdu->src, &added);
    if (n) {
        n->rx_count++;
        n->last_heard_superframe = pdu->superframe;
        /* TODO: also raise it when a silent neighbour is heard again (OI-3). */
        if (added) raise_event(mac, HH_MAC_EV_NEIGHBOUR_HEARD, pdu->src, 0, NULL);
    }

    if (pdu->dst != mac->cfg.node_id && pdu->dst != HH_MAC_BROADCAST && !is_group(pdu->dst)) {
        mac->st.rx_not_addressed++;
        return;
    }
    if (!can_deliver(mac->state)) {
        mac->st.rx_not_delivered_state++;
        return;
    }

    h = &mac->hold[pdu->cls];
    if (h->count >= mac->cfg.hold_depth[pdu->cls]) {
        rx_release(mac, ring_pop(h));
        mac->st.hold_overflow[pdu->cls]++;
    }
    i = mac->rx_free[--mac->rx_free_count];
    e = &mac->rx_pool[i];
    e->f.kind = pdu->kind;
    e->f.cls = pdu->cls;
    e->f.src = pdu->src;
    e->f.dst = pdu->dst;
    e->f.superframe = pdu->superframe;
    e->f.metrics = pdu->metrics;
    e->f.len = pdu->len;
    if (pdu->len) memcpy(e->f.payload, pdu->payload, pdu->len);
    e->seq = mac->seq++;
    ring_push(h, i);
    flush(mac);
}

void hh_mac_on_crc_fail(hh_mac_t *mac)
{
    if (mac && mac->opened) mac->st.rx_crc_fail++;
}

void hh_mac_on_tx_underrun(hh_mac_t *mac)
{
    if (mac && mac->opened) mac->st.tx_underrun++;
}

void hh_mac_on_tx_inhibited_slot(hh_mac_t *mac)
{
    if (mac && mac->opened) mac->st.tx_inhibit++;
}

void hh_mac_on_superframe(hh_mac_t *mac, uint32_t superframe)
{
    if (!mac || !mac->opened) return;
    mac->superframe = superframe;
    if (!mac->cfg.report_period_sf || ++mac->sf_since_report < mac->cfg.report_period_sf)
        return;
    mac->sf_since_report = 0;
    for (int i = 0; i < HH_MAC_NEIGHBOR_CAP; i++) {
        hh_mac_link_report_t r;
        if (!mac->neighbors[i].used) continue;
        fill_report(&mac->neighbors[i], &r);
        raise_event(mac, HH_MAC_EV_LINK_REPORT, r.neighbor, 0, &r);
    }
}

void hh_mac_on_break_in(hh_mac_t *mac, hh_node_id_t node, bool on)
{
    if (!mac || !mac->opened) return;
    raise_event(mac, on ? HH_MAC_EV_BREAK_IN_ON : HH_MAC_EV_BREAK_IN_OFF, node, 0, NULL);
}

void hh_mac_on_entry_failed(hh_mac_t *mac, hh_mac_entry_fail_t reason)
{
    if (!mac || !mac->opened) return;
    HH_LOGW(COMP, "entry_failed", "node=%u reason=%d", mac->cfg.node_id, (int)reason);
    raise_event(mac, HH_MAC_EV_ENTRY_FAILED, mac->cfg.node_id, (uint32_t)reason, NULL);
}

void hh_mac_set_state(hh_mac_t *mac, hh_mac_state_t state)
{
    if (!mac || !mac->opened || mac->state == state) return;
    HH_LOGI(COMP, "state", "node=%u from=%s to=%s", mac->cfg.node_id,
            hh_mac_state_str(mac->state), hh_mac_state_str(state));
    mac->state = state;
    mac->state_since = now(mac);
    if (!can_send(state)) mac->slots_without_user = 0;
    raise_event(mac, HH_MAC_EV_STATE_CHANGED, mac->cfg.node_id, (uint32_t)state, NULL);
}

void hh_mac_on_reset(hh_mac_t *mac)
{
    if (!mac || !mac->opened) return;
    mac->st.restart_count++;
    HH_LOGW(COMP, "reset", "node=%u restart_count=%u", mac->cfg.node_id, mac->st.restart_count);
    /* TODO: fate of queued frames on a MAC reset (DP-3); kept for now. */
    hh_mac_set_state(mac, HH_MAC_WAIT_TIME);
}

void hh_mac_set_tx_inhibit(hh_mac_t *mac, hh_mac_inhibit_t reason)
{
    hh_mac_inhibit_t was;

    if (!mac || !mac->opened || mac->inhibit == reason) return;
    was = mac->inhibit;
    mac->inhibit = reason;
    HH_LOGI(COMP, "tx_inhibit", "node=%u reason=%d", mac->cfg.node_id, (int)reason);
    if (reason == HH_MAC_INHIBIT_NONE)
        raise_event(mac, HH_MAC_EV_TX_INHIBIT_OFF, mac->cfg.node_id, (uint32_t)was, NULL);
    else
        raise_event(mac, HH_MAC_EV_TX_INHIBIT_ON, mac->cfg.node_id, (uint32_t)reason, NULL);
}

void hh_mac_set_own_slots(hh_mac_t *mac, uint16_t own_slots_per_frame)
{
    if (!mac || !mac->opened) return;
    if (mac->st.own_slots_valid && mac->st.own_slots_per_frame == own_slots_per_frame) return;
    mac->st.own_slots_per_frame = own_slots_per_frame;
    mac->st.own_slots_valid = true;
    raise_event(mac, HH_MAC_EV_OWN_SLOTS_CHANGED, mac->cfg.node_id, own_slots_per_frame, NULL);
}

void hh_mac_set_time_source(hh_mac_t *mac, hh_mac_time_source_t src)
{
    if (!mac || !mac->opened) return;
    mac->st.time_source = src;
    mac->st.time_source_valid = true;
}

hh_mac_result_t hh_mac_set_max_payload(hh_mac_t *mac, uint16_t max_payload)
{
    if (!mac || !mac->initialized || !mac->opened) return HH_MAC_NOT_OPEN;
    if (max_payload == 0 || max_payload > HH_MAC_PAYLOAD_MAX) return HH_MAC_INVALID_ARGUMENT;
    if (max_payload == mac->max_payload) return HH_MAC_OK;
    mac->max_payload = max_payload;
    raise_event(mac, HH_MAC_EV_PARAMETERS_CHANGED, mac->cfg.node_id, max_payload, NULL);
    return HH_MAC_OK;
}

const char *hh_mac_result_str(hh_mac_result_t r)
{
    switch (r) {
    case HH_MAC_OK:               return "OK";
    case HH_MAC_ACCEPTED:         return "ACCEPTED";
    case HH_MAC_TRY_AGAIN:        return "TRY_AGAIN";
    case HH_MAC_WRONG_STATE:      return "WRONG_STATE";
    case HH_MAC_TX_INHIBITED:     return "TX_INHIBITED";
    case HH_MAC_TOO_LARGE:        return "TOO_LARGE";
    case HH_MAC_INVALID_ARGUMENT: return "INVALID_ARGUMENT";
    case HH_MAC_INVALID_CONFIG:   return "INVALID_CONFIG";
    case HH_MAC_NOT_OPEN:         return "NOT_OPEN";
    case HH_MAC_NOT_FOUND:        return "NOT_FOUND";
    case HH_MAC_UNSUPPORTED:      return "UNSUPPORTED";
    case HH_MAC_FAULT:            return "FAULT";
    }
    return "?";
}

const char *hh_mac_state_str(hh_mac_state_t s)
{
    switch (s) {
    case HH_MAC_OFF:       return "OFF";
    case HH_MAC_WAIT_TIME: return "WAIT_TIME";
    case HH_MAC_LISTEN:    return "LISTEN";
    case HH_MAC_ENTRY:     return "ENTRY";
    case HH_MAC_STEADY:    return "STEADY";
    case HH_MAC_HOLDOVER:  return "HOLDOVER";
    case HH_MAC_LOST:      return "LOST";
    case HH_MAC_SAFE:      return "SAFE";
    }
    return "?";
}

const char *hh_mac_class_str(hh_mac_class_t c)
{
    switch (c) {
    case HH_MAC_CLASS_CONTROL:   return "control";
    case HH_MAC_CLASS_EMERGENCY: return "emergency";
    case HH_MAC_CLASS_VOICE:     return "voice";
    case HH_MAC_CLASS_VIDEO:     return "video";
    case HH_MAC_CLASS_DATA:      return "data";
    case HH_MAC_CLASS_COUNT:     break;
    }
    return "?";
}
