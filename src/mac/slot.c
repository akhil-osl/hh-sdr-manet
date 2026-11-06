/* Slot scheduler. See include/hhsdr/mac/slot.h. */
#include "hhsdr/mac/slot.h"
#include "hhsdr/core/log.h"
#include <string.h>

#define COMP "slot"

#define SUB_LEN          5u       /* message sub-header without link sequence */
#define GROUP_AIR_FIRST  0xF000u
#define GROUP_AIR_LAST   0xFFFDu
#define TIME_UNKNOWN     0xFCu    /* own GNSS source, error field 63: unknown */

void hh_slot_config_default(hh_slot_config_t *cfg, hh_node_id_t node_id, uint16_t net_id)
{
    memset(cfg, 0, sizeof *cfg);
    cfg->slots_per_frame = 20;
    cfg->frames_per_superframe = 50;
    cfg->slot_ms = 1;
    cfg->capacity = 128;   /* TODO: placeholder until the burst rate and coding are chosen */
    cfg->net_id = net_id;
    cfg->node_id = node_id;
}

static uint16_t bitmap_bytes(const hh_slot_config_t *c) { return (uint16_t)((c->slots_per_frame + 7u) / 8u); }

hh_status_t hh_slot_init(hh_slot_t *s, const hh_slot_config_t *cfg, const hh_clock_t *clock,
                         hh_mac_t *mac, hh_slot_tx_fn tx, void *tx_ctx)
{
    hh_pdu_t probe;
    size_t floor_len;
    uint16_t max_payload;

    if (!s || !cfg || !clock || !mac || !tx) return HH_ERR_INVAL;
    if (cfg->slots_per_frame == 0 || cfg->slots_per_frame > HH_SLOT_FRAME_MAX ||
        cfg->frames_per_superframe == 0 || cfg->slot_ms == 0 ||
        cfg->node_id == 0 || cfg->node_id > 0xEFFFu || cfg->net_id == 0xFFFFu)
        return HH_ERR_INVAL;
    if (cfg->slots_per_frame < HH_SLOT_FRAME_MAX &&
        (cfg->own_slots >> cfg->slots_per_frame) != 0)
        return HH_ERR_INVAL;

    memset(&probe, 0, sizeof probe);
    probe.slot_bytes = (uint8_t)bitmap_bytes(cfg);
    floor_len = hh_pdu_overhead(&probe) + SUB_LEN;
    if (cfg->capacity <= floor_len || cfg->capacity > HH_PDU_MAX_LEN) return HH_ERR_INVAL;

    memset(s, 0, sizeof *s);
    s->cfg = *cfg;
    s->clock = clock;
    s->mac = mac;
    s->tx = tx;
    s->tx_ctx = tx_ctx;

    /* One PDU is the largest unit until fragmentation exists. */
    max_payload = (uint16_t)(cfg->capacity - floor_len);
    if (max_payload > HH_MAC_PAYLOAD_MAX) max_payload = HH_MAC_PAYLOAD_MAX;
    if (hh_mac_set_max_payload(mac, max_payload) != HH_MAC_OK) return HH_ERR_STATE;
    return HH_OK;
}

static hh_node_id_t air_to_node(uint16_t a, bool *ok)
{
    *ok = true;
    if (a == HH_PDU_ADDR_BROADCAST) return HH_MAC_BROADCAST;
    if (a >= GROUP_AIR_FIRST && a <= GROUP_AIR_LAST) return HH_MAC_GROUP_FIRST + (a - GROUP_AIR_FIRST);
    if (a >= 1u && a <= 0xEFFFu) return a;
    *ok = false;
    return 0;
}

static uint16_t node_to_air(hh_node_id_t n, bool *ok)
{
    *ok = true;
    if (n == HH_MAC_BROADCAST) return HH_PDU_ADDR_BROADCAST;
    if (n >= HH_MAC_GROUP_FIRST && n - HH_MAC_GROUP_FIRST <= (GROUP_AIR_LAST - GROUP_AIR_FIRST))
        return (uint16_t)(GROUP_AIR_FIRST + (n - HH_MAC_GROUP_FIRST));
    if (n >= 1u && n <= 0xEFFFu) return (uint16_t)n;
    *ok = false;
    return 0;
}

static uint8_t proto_for(hh_frame_kind_t k)
{
    switch (k) {
    case HH_FRAME_BEACON:  return HH_PDU_PROTO_BEACON;
    case HH_FRAME_ROUTING: return HH_PDU_PROTO_ROUTING;
    default:               return HH_PDU_PROTO_OPAQUE;
    }
}

static bool kind_for(uint8_t proto, hh_frame_kind_t *k)
{
    switch (proto) {
    case HH_PDU_PROTO_BEACON:  *k = HH_FRAME_BEACON;  return true;
    case HH_PDU_PROTO_ROUTING: *k = HH_FRAME_ROUTING; return true;
    case HH_PDU_PROTO_IPV6:
    case HH_PDU_PROTO_IPV4:
    case HH_PDU_PROTO_OPAQUE:
    case HH_PDU_PROTO_TEST:    *k = HH_FRAME_DATA;    return true;
    default:                   return false;   /* MAC commands and reserved IDs */
    }
}

static bool queued_frames(const hh_mac_t *mac)
{
    hh_mac_status_t st;
    if (hh_mac_status(mac, &st) != HH_MAC_OK) return false;
    for (int k = 0; k < HH_MAC_CLASS_COUNT; k++)
        if (st.queue_depth[k]) return true;
    return false;
}

static void send_own(hh_slot_t *s, const hh_slot_pos_t *pos)
{
    hh_pdu_t *p = &s->pdu;
    size_t base, used = 0, len = 0, ctl = 0, user = 0, routing = 0;

    memset(p, 0, sizeof *p);
    p->seq = s->seq++;
    p->net_id = s->cfg.net_id;
    p->src = (uint16_t)s->cfg.node_id;
    p->time_quality = TIME_UNKNOWN;
    p->slot_bytes = (uint8_t)bitmap_bytes(&s->cfg);
    for (unsigned n = 0; n < s->cfg.slots_per_frame; n++)
        if ((s->cfg.own_slots >> n) & 1u) p->claims[n / 8u] |= (uint8_t)(1u << (n % 8u));
    base = hh_pdu_overhead(p);

    while (p->n_msg < HH_PDU_MSG_MAX) {
        hh_mac_tx_frame_t *f = &s->frames[p->n_msg];
        hh_pdu_msg_t *m = &p->msg[p->n_msg];
        size_t room = s->cfg.capacity - base - used;
        bool ok;

        if (room <= SUB_LEN) break;
        if (!hh_mac_next_frame_fit(s->mac, (uint16_t)(room - SUB_LEN), f)) break;
        memset(m, 0, sizeof *m);
        m->dst = node_to_air(f->dst, &ok);
        if (!ok) { s->st.messages_dropped++; continue; }
        m->proto = proto_for(f->kind);
        m->cls = (uint8_t)f->cls;
        m->data = f->payload;
        m->len = f->len;
        used += hh_pdu_msg_size(m);
        if (f->cls == HH_MAC_CLASS_CONTROL) ctl += hh_pdu_msg_size(m);
        else user += hh_pdu_msg_size(m);
        if (f->kind == HH_FRAME_ROUTING) routing += hh_pdu_msg_size(m);
        p->n_msg++;
    }
    p->more_data = queued_frames(s->mac);

    if (hh_pdu_encode(p, s->buf, sizeof s->buf, &len) != HH_OK) {
        s->st.tx_failed++;
        return;
    }
    if (!s->tx(s->tx_ctx, pos, s->buf, len)) {
        s->st.tx_failed++;
        return;
    }
    s->st.pdus_tx++;
    if (!p->n_msg) s->st.pdus_empty++;
    s->st.messages_tx += p->n_msg;
    s->st.capacity_bytes += s->cfg.capacity;
    s->st.pdu_bytes += len;
    s->st.overhead_bytes += base;
    s->st.control_bytes += ctl;
    s->st.routing_bytes += routing;
    if (ctl) s->st.slots_with_control++;
    if (routing) s->st.slots_with_routing++;
    s->st.user_bytes += user;
    HH_LOGT(COMP, "own_slot", "node=%u sf=%u frame=%u slot=%u msgs=%u bytes=%zu ctl=%zu user=%zu cap=%u",
            (unsigned)s->cfg.node_id, pos->superframe, pos->frame, pos->slot, p->n_msg, len, ctl, user,
            s->cfg.capacity);
}

static void run_slot(hh_slot_t *s, uint64_t abs, bool late)
{
    hh_slot_pos_t pos;
    uint64_t per_frame = s->cfg.slots_per_frame;
    uint64_t per_super = per_frame * s->cfg.frames_per_superframe;

    pos.abs_slot = abs;
    pos.superframe = (uint32_t)(abs / per_super);
    pos.frame = (uint16_t)((abs % per_super) / per_frame);
    pos.slot = (uint16_t)(abs % per_frame);

    s->st.slots_seen++;
    if (pos.frame == 0 && pos.slot == 0) hh_mac_on_superframe(s->mac, pos.superframe);
    if (!((s->cfg.own_slots >> pos.slot) & 1u)) return;

    s->st.own_slots++;
    if (late) {
        s->st.own_slots_missed++;
        return;
    }
    send_own(s, &pos);
}

void hh_slot_poll(hh_slot_t *s)
{
    hh_time_ms_t now;
    uint64_t cur;

    if (!s) return;
    now = hh_now(s->clock);
    if (now < s->cfg.epoch_ms) return;
    cur = (now - s->cfg.epoch_ms) / s->cfg.slot_ms;
    if (!s->started) {
        s->started = true;
        s->next_slot = cur;
    }
    while (s->next_slot <= cur) {
        run_slot(s, s->next_slot, s->next_slot < cur);
        s->next_slot++;
    }
}

void hh_slot_receive(hh_slot_t *s, const hh_slot_pos_t *pos, const uint8_t *pdu, size_t len)
{
    hh_pdu_t *p;
    bool ok;
    hh_node_id_t src;
    hh_status_t rc;

    if (!s || !pos || !pdu) return;
    p = &s->pdu;
    rc = hh_pdu_decode(pdu, len, p);
    if (rc == HH_ERR_UNSUPPORTED) { s->st.rx_unsupported++; return; }
    if (rc != HH_OK) { s->st.rx_bad++; return; }
    if (p->net_id != s->cfg.net_id) { s->st.rx_wrong_net++; return; }
    src = air_to_node(p->src, &ok);
    if (!ok || src == HH_MAC_BROADCAST || src >= HH_MAC_GROUP_FIRST || src == s->cfg.node_id) {
        s->st.rx_bad++;
        return;
    }
    s->st.rx_pdus++;
    hh_mac_on_heard(s->mac, src, pos->superframe);

    for (unsigned i = 0; i < p->n_msg; i++) {
        const hh_pdu_msg_t *m = &p->msg[i];
        hh_mac_rx_pdu_t up;
        hh_frame_kind_t kind;
        hh_node_id_t dst = air_to_node(m->dst, &ok);

        if (!ok || m->cls >= HH_MAC_CLASS_COUNT || !kind_for(m->proto, &kind) || m->len > HH_MAC_PAYLOAD_MAX) {
            s->st.rx_messages_skipped++;
            continue;
        }
        memset(&up, 0, sizeof up);
        up.kind = kind;
        up.cls = (hh_mac_class_t)m->cls;
        up.src = src;
        up.dst = dst;
        up.superframe = pos->superframe;
        up.len = m->len;
        up.payload = m->data;
        hh_mac_on_rx(s->mac, &up);
    }
}

uint32_t hh_slot_control_share_permille(const hh_slot_stats_t *st)
{
    if (!st || !st->capacity_bytes) return 0;
    return (uint32_t)((st->control_bytes * 1000u) / st->capacity_bytes);
}
