#include "hhsdr/mac/mac.h"
#include "hhsdr/core/log.h"
#include <string.h>

#define COMP "mac"

void hh_mac_config_default(hh_mac_config_t *cfg, hh_node_id_t node_id)
{
    if (!cfg) return;
    memset(cfg, 0, sizeof *cfg);
    cfg->node_id = node_id;
    cfg->max_payload = HH_MAC_PAYLOAD_MAX;
    cfg->queue_depth[HH_MAC_CLASS_CONTROL] = 16;
    cfg->queue_depth[HH_MAC_CLASS_DATA] = 64;
    cfg->rx_buffer_depth = 16;
    cfg->data_share_n = 0;
    cfg->rx_deliver_all_states = false;
}

hh_mac_class_t hh_mac_class_of(hh_frame_kind_t kind)
{
    switch (kind) {
    case HH_FRAME_BEACON:
    case HH_FRAME_ROUTING: return HH_MAC_CLASS_CONTROL;
    case HH_FRAME_DATA:    return HH_MAC_CLASS_DATA;
    }
    return HH_MAC_CLASS_COUNT;
}

static bool cfg_valid(const hh_mac_config_t *c)
{
    if (c->node_id == HH_NODE_ID_INVALID) return false;
    if (c->max_payload == 0 || c->max_payload > HH_MAC_PAYLOAD_MAX) return false;
    for (int k = 0; k < HH_MAC_CLASS_COUNT; k++)
        if (c->queue_depth[k] == 0 || c->queue_depth[k] > HH_MAC_QUEUE_CAP) return false;
    if (c->rx_buffer_depth == 0 || c->rx_buffer_depth > HH_MAC_RXBUF_CAP) return false;
    return true;
}

static bool can_send(hh_mac_state_t s)
{
    return s == HH_MAC_STEADY || s == HH_MAC_HOLDOVER;
}

hh_mac_result_t hh_mac_init(hh_mac_t *mac, const hh_mac_config_t *cfg)
{
    if (!mac || !cfg) return HH_MAC_INVALID_ARGUMENT;
    if (!cfg_valid(cfg)) {
        HH_LOGE(COMP, "init_failed", "reason=invalid_config node=%u", cfg->node_id);
        return HH_MAC_INVALID_CONFIG;
    }
    memset(mac, 0, sizeof *mac);
    mac->cfg = *cfg;
    mac->state = HH_MAC_OFF;
    mac->initialized = true;
    HH_LOGI(COMP, "initialized",
            "node=%u depth_control=%u depth_data=%u rx_depth=%u data_share_n=%u",
            cfg->node_id, cfg->queue_depth[HH_MAC_CLASS_CONTROL],
            cfg->queue_depth[HH_MAC_CLASS_DATA], cfg->rx_buffer_depth, cfg->data_share_n);
    return HH_MAC_OK;
}

hh_mac_result_t hh_mac_open(hh_mac_t *mac)
{
    if (!mac || !mac->initialized) return HH_MAC_INVALID_ARGUMENT;
    if (mac->opened) return HH_MAC_OK;   /* TODO: decide behaviour of Open when already open */
    mac->opened = true;
    mac->state = HH_MAC_WAIT_TIME;
    HH_LOGI(COMP, "opened", "node=%u state=%s", mac->cfg.node_id, hh_mac_state_str(mac->state));
    return HH_MAC_OK;
}

hh_mac_result_t hh_mac_close(hh_mac_t *mac)
{
    if (!mac || !mac->initialized) return HH_MAC_INVALID_ARGUMENT;
    if (!mac->opened) return HH_MAC_OK;
    /* Queued frames are discarded and counted. */
    for (int k = 0; k < HH_MAC_CLASS_COUNT; k++) {
        mac->st.close_discards[k] += mac->queue[k].count;
        mac->queue[k].head = 0;
        mac->queue[k].count = 0;
    }
    mac->rx_head = 0;
    mac->rx_count = 0;
    mac->control_run = 0;
    mac->opened = false;
    mac->state = HH_MAC_OFF;
    HH_LOGI(COMP, "closed", "node=%u discarded_control=%llu discarded_data=%llu",
            mac->cfg.node_id,
            (unsigned long long)mac->st.close_discards[HH_MAC_CLASS_CONTROL],
            (unsigned long long)mac->st.close_discards[HH_MAC_CLASS_DATA]);
    return HH_MAC_OK;
}

hh_mac_result_t hh_mac_send(hh_mac_t *mac, hh_frame_kind_t kind, hh_node_id_t dst,
                            const uint8_t *payload, uint16_t len)
{
    hh_mac_class_t cls;
    hh_mac_queue_t *q;
    hh_mac_tx_frame_t *f;

    if (!mac || !mac->initialized) return HH_MAC_INVALID_ARGUMENT;
    if (!mac->opened) return HH_MAC_NOT_OPEN;
    if (!can_send(mac->state)) {
        mac->st.refused_state++;
        return HH_MAC_WRONG_STATE;
    }
    if (len > mac->cfg.max_payload) {
        mac->st.refused_too_large++;
        HH_LOGD(COMP, "send_refused", "reason=too_large len=%u max=%u", len, mac->cfg.max_payload);
        return HH_MAC_TOO_LARGE;
    }
    cls = hh_mac_class_of(kind);
    /* TODO: decide which destination ids are invalid; none are refused now. */
    if (cls == HH_MAC_CLASS_COUNT || (!payload && len)) {
        mac->st.refused_invalid++;
        return HH_MAC_INVALID_ARGUMENT;
    }
    q = &mac->queue[cls];
    if (q->count >= mac->cfg.queue_depth[cls]) {
        mac->st.queue_full[cls]++;
        HH_LOGD(COMP, "send_refused", "reason=queue_full class=%d depth=%u", (int)cls, q->count);
        return HH_MAC_TRY_AGAIN;
    }

    f = &q->frames[(q->head + q->count) % HH_MAC_QUEUE_CAP];
    f->kind = kind;
    f->src = mac->cfg.node_id;
    f->dst = dst;
    f->len = len;
    if (len) memcpy(f->payload, payload, len);
    q->count++;
    return HH_MAC_ACCEPTED;
}

static void pop(hh_mac_queue_t *q, hh_mac_tx_frame_t *out)
{
    *out = q->frames[q->head];
    q->head = (uint16_t)((q->head + 1) % HH_MAC_QUEUE_CAP);
    q->count--;
}

bool hh_mac_next_pdu(hh_mac_t *mac, hh_mac_tx_frame_t *out)
{
    hh_mac_queue_t *ctl, *dat;

    if (!mac || !out || !mac->opened) return false;
    /* TODO: decide what happens to queued frames outside STEADY and HOLDOVER; kept for now. */
    if (!can_send(mac->state)) return false;

    ctl = &mac->queue[HH_MAC_CLASS_CONTROL];
    dat = &mac->queue[HH_MAC_CLASS_DATA];

    if (dat->count && mac->cfg.data_share_n && mac->control_run >= mac->cfg.data_share_n) {
        pop(dat, out);
        mac->control_run = 0;
    } else if (ctl->count) {
        pop(ctl, out);
        mac->control_run++;
    } else if (dat->count) {
        pop(dat, out);
        mac->control_run = 0;
    } else {
        return false;
    }
    mac->st.frames_tx++;
    return true;
}

static hh_mac_neighbor_t *neighbor_find(hh_mac_t *mac, hh_node_id_t id, bool add)
{
    hh_mac_neighbor_t *free_slot = NULL;

    for (int i = 0; i < HH_MAC_NEIGHBOR_CAP; i++) {
        hh_mac_neighbor_t *n = &mac->neighbors[i];
        if (n->used && n->id == id) return n;
        if (!n->used && !free_slot) free_slot = n;
    }
    if (!add) return NULL;
    if (!free_slot) {
        mac->st.neighbor_table_full++;
        return NULL;
    }
    memset(free_slot, 0, sizeof *free_slot);
    free_slot->used = true;
    free_slot->id = id;
    return free_slot;
}

void hh_mac_on_rx(hh_mac_t *mac, const hh_mac_rx_pdu_t *pdu)
{
    hh_mac_neighbor_t *n;
    hh_mac_rx_frame_t *f;

    if (!mac || !pdu || !mac->opened) return;
    if (pdu->len > HH_MAC_PAYLOAD_MAX || (!pdu->payload && pdu->len)) return;

    /* Every CRC-good PDU updates the neighbour record, before the address filter. */
    n = neighbor_find(mac, pdu->src, true);
    if (n) {
        n->rx_count++;
        if (pdu->superframe_valid) {
            n->last_heard_superframe = pdu->superframe;
            n->last_heard_valid = true;
        }
    }

    if (pdu->dst != mac->cfg.node_id && pdu->dst != HH_MAC_BROADCAST) {
        mac->st.rx_not_addressed++;
        return;
    }
    if (!can_send(mac->state) && !mac->cfg.rx_deliver_all_states) {
        mac->st.rx_not_delivered_state++;
        return;
    }
    if (mac->rx_count >= mac->cfg.rx_buffer_depth) {
        mac->st.rx_overflow++;
        return;
    }

    f = &mac->rx[(mac->rx_head + mac->rx_count) % HH_MAC_RXBUF_CAP];
    f->kind = pdu->kind;
    f->src = pdu->src;
    f->dst = pdu->dst;
    f->len = pdu->len;
    if (pdu->len) memcpy(f->payload, pdu->payload, pdu->len);
    f->metrics.rssi = pdu->rssi;
    f->metrics.snr = pdu->snr;
    f->metrics.per.value = 0.0f;
    f->metrics.per.valid = false;
    f->superframe = pdu->superframe;
    f->superframe_valid = pdu->superframe_valid;
    mac->rx_count++;
}

void hh_mac_on_crc_fail(hh_mac_t *mac)
{
    if (mac) mac->st.rx_crc_fail++;
}

hh_mac_result_t hh_mac_collect(hh_mac_t *mac, hh_mac_rx_frame_t *out, bool *got)
{
    if (got) *got = false;
    if (!mac || !mac->initialized || !out || !got) return HH_MAC_INVALID_ARGUMENT;
    if (!mac->opened) return HH_MAC_NOT_OPEN;
    if (!mac->rx_count) return HH_MAC_OK;
    *out = mac->rx[mac->rx_head];
    mac->rx_head = (uint16_t)((mac->rx_head + 1) % HH_MAC_RXBUF_CAP);
    mac->rx_count--;
    mac->st.frames_rx++;
    *got = true;
    return HH_MAC_OK;
}

hh_mac_result_t hh_mac_status(const hh_mac_t *mac, hh_mac_status_t *out)
{
    if (!mac || !mac->initialized || !out) return HH_MAC_INVALID_ARGUMENT;
    if (!mac->opened) return HH_MAC_NOT_OPEN;
    *out = mac->st;
    out->state = mac->state;
    switch (mac->state) {
    case HH_MAC_STEADY:
    case HH_MAC_HOLDOVER: out->operational = true;  out->operational_known = true; break;
    case HH_MAC_OFF:
    case HH_MAC_SAFE:     out->operational = false; out->operational_known = true; break;
    default:              out->operational = false; out->operational_known = false; break;
    }
    for (int k = 0; k < HH_MAC_CLASS_COUNT; k++) out->queue_depth[k] = mac->queue[k].count;
    return HH_MAC_OK;
}

hh_mac_result_t hh_mac_link_metrics(const hh_mac_t *mac, hh_node_id_t neighbor,
                                    hh_mac_link_report_t *out)
{
    if (!mac || !mac->initialized || !out) return HH_MAC_INVALID_ARGUMENT;
    if (!mac->opened) return HH_MAC_NOT_OPEN;
    for (int i = 0; i < HH_MAC_NEIGHBOR_CAP; i++) {
        const hh_mac_neighbor_t *n = &mac->neighbors[i];
        if (!n->used || n->id != neighbor) continue;
        memset(out, 0, sizeof *out);
        out->neighbor = n->id;
        out->last_heard_superframe = n->last_heard_superframe;
        out->last_heard_valid = n->last_heard_valid;
        out->rx_count = n->rx_count;
        return HH_MAC_OK;
    }
    return HH_MAC_NOT_FOUND;
}

hh_mac_result_t hh_mac_channel_select(hh_mac_t *mac, uint32_t channel)
{
    (void)channel;
    if (!mac || !mac->initialized) return HH_MAC_INVALID_ARGUMENT;
    if (!mac->opened) return HH_MAC_NOT_OPEN;
    return HH_MAC_UNSUPPORTED;   /* TODO: define channel select */
}

void hh_mac_set_state(hh_mac_t *mac, hh_mac_state_t state)
{
    if (!mac || !mac->opened || mac->state == state) return;
    HH_LOGI(COMP, "state", "node=%u from=%s to=%s", mac->cfg.node_id,
            hh_mac_state_str(mac->state), hh_mac_state_str(state));
    mac->state = state;
    if (!can_send(state)) mac->control_run = 0;
}

const char *hh_mac_result_str(hh_mac_result_t r)
{
    switch (r) {
    case HH_MAC_OK:               return "OK";
    case HH_MAC_ACCEPTED:         return "ACCEPTED";
    case HH_MAC_TRY_AGAIN:        return "TRY_AGAIN";
    case HH_MAC_WRONG_STATE:      return "WRONG_STATE";
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
