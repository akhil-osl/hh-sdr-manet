/* Slot scheduler. See include/hhsdr/mac/slot.h. */
#include "hhsdr/mac/slot.h"
#include "hhsdr/mac/entry.h"
#include "hhsdr/core/log.h"
#include <string.h>

#define COMP "slot"

#define SUB_LEN          5u       /* message sub-header without link sequence */
#define GROUP_AIR_FIRST  0xF000u
#define GROUP_AIR_LAST   0xFFFDu
#define TIME_UNKNOWN     0xFCu    /* own GNSS source, error field 63: unknown */

/* Fills cfg with the default 20-slot frame and a placeholder slot capacity. */
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

/* Returns the slot bitmap length L in bytes for the configured frame. */
static uint16_t bitmap_bytes(const hh_slot_config_t *c) { return (uint16_t)((c->slots_per_frame + 7u) / 8u); }

/* Encodes a PDU for the transmit function, with or without the CRC-32 as configured. */
static hh_status_t encode_pdu(hh_slot_t *s, const hh_pdu_t *p, size_t *len)
{
    return s->cfg.logic_adds_crc ? hh_pdu_encode_nocrc(p, s->buf, sizeof s->buf, len)
                                 : hh_pdu_encode(p, s->buf, sizeof s->buf, len);
}

/* Validates the configuration, resets the scheduler and limits the MAC payload to what one PDU carries. */
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
        ((cfg->own_slots | cfg->entry_slots) >> cfg->slots_per_frame) != 0)
        return HH_ERR_INVAL;
    if (cfg->own_slots & cfg->entry_slots) return HH_ERR_INVAL;

    memset(&probe, 0, sizeof probe);
    probe.slot_bytes = (uint8_t)bitmap_bytes(cfg);
    probe.has_qreport = true;   /* every own-slot PDU carries the queue report */
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

/* Converts a 16-bit on-air address to a 32-bit node id. ok is false for an invalid address. */
static hh_node_id_t air_to_node(uint16_t a, bool *ok)
{
    *ok = true;
    if (a == HH_PDU_ADDR_BROADCAST) return HH_MAC_BROADCAST;
    if (a >= GROUP_AIR_FIRST && a <= GROUP_AIR_LAST) return HH_MAC_GROUP_FIRST + (a - GROUP_AIR_FIRST);
    if (a >= 1u && a <= 0xEFFFu) return a;
    *ok = false;
    return 0;
}

/* Converts a 32-bit node id to a 16-bit on-air address. ok is false if it has no on-air form. */
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

/* Returns the PDU protocol ID for a frame kind. */
static uint8_t proto_for(hh_frame_kind_t k)
{
    switch (k) {
    case HH_FRAME_BEACON:  return HH_PDU_PROTO_BEACON;
    case HH_FRAME_ROUTING: return HH_PDU_PROTO_ROUTING;
    default:               return HH_PDU_PROTO_OPAQUE;
    }
}

/* Maps a PDU protocol ID to a frame kind. False for MAC commands and reserved IDs. */
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

/* Writes the frames still queued per class, 255 at most each (the report byte), and returns true if any are queued. */
static bool queue_left(const hh_mac_t *mac, uint8_t out[HH_ALLOC_CLASSES])
{
    hh_mac_status_t st;
    bool any = false;

    memset(out, 0, HH_ALLOC_CLASSES);
    if (hh_mac_status(mac, &st) != HH_MAC_OK) return false;
    for (int k = 0; k < HH_MAC_CLASS_COUNT; k++) {
        out[k] = st.queue_depth[k] > 255u ? 255u : (uint8_t)st.queue_depth[k];
        if (st.queue_depth[k]) any = true;
    }
    return any;
}

/* Returns the number of set bits. */
static unsigned count_bits(uint64_t m)
{
    unsigned n = 0;
    for (; m; m &= m - 1u) n++;
    return n;
}

/* Applies what the entry logic asked for: new own slots, a failure, and the matching MAC state. */
static void apply_entry(hh_slot_t *s, const hh_entry_act_t *act)
{
    hh_entry_state_t st = hh_entry_state(s->entry);

    if (act->own_changed) {
        s->cfg.own_slots = act->own_slots;
        hh_mac_set_own_slots(s->mac, (uint16_t)count_bits(act->own_slots));
    }
    if (act->failed) hh_mac_on_entry_failed(s->mac, act->fail_reason);
    for (unsigned i = 0; i < act->n_released; i++) hh_alloc_remove(&s->demand, act->released[i]);   /* a member that left is no longer counted */
    if ((int)st == s->entry_state) return;
    s->entry_state = (int)st;
    switch (st) {
    case HH_ENTRY_LISTEN:
    case HH_ENTRY_HOLDOFF:
    case HH_ENTRY_REFUSED:   hh_mac_set_state(s->mac, HH_MAC_LISTEN); break;
    case HH_ENTRY_BACKOFF:
    case HH_ENTRY_WAIT_GRANT:
    case HH_ENTRY_GRANTED:   hh_mac_set_state(s->mac, HH_MAC_ENTRY); break;
    case HH_ENTRY_MEMBER:    hh_mac_set_state(s->mac, HH_MAC_STEADY); break;
    case HH_ENTRY_OFF:       hh_mac_set_state(s->mac, HH_MAC_WAIT_TIME); break;
    case HH_ENTRY_FOUNDER:   break;   /* TODO: the MAC state of a founder is not defined (U-23) */
    }
}

/* Fills pos for an absolute slot number. */
static void pos_for(const hh_slot_t *s, uint64_t abs, hh_slot_pos_t *pos)
{
    uint64_t per_frame = s->cfg.slots_per_frame;
    uint64_t per_super = per_frame * s->cfg.frames_per_superframe;

    pos->abs_slot = abs;
    pos->superframe = (uint32_t)(abs / per_super);
    pos->frame = (uint16_t)((abs % per_super) / per_frame);
    pos->slot = (uint16_t)(abs % per_frame);
}

/* Fills pos for the slot the clock is in now. False before the epoch. */
static bool pos_now(const hh_slot_t *s, hh_slot_pos_t *pos)
{
    hh_time_ms_t now = hh_now(s->clock);
    if (now < s->cfg.epoch_ms) return false;
    pos_for(s, (now - s->cfg.epoch_ms) / s->cfg.slot_ms, pos);
    return true;
}

/* Connects the entry logic to the scheduler. */
hh_status_t hh_slot_attach_entry(hh_slot_t *s, hh_entry_t *e)
{
    if (!s || !e) return HH_ERR_INVAL;
    s->entry = e;
    s->entry_state = (int)hh_entry_state(e);
    return HH_OK;
}

/* Starts network entry at the current slot. */
hh_status_t hh_slot_join(hh_slot_t *s)
{
    hh_slot_pos_t pos;
    hh_entry_act_t act = { 0 };
    hh_status_t rc;

    if (!s || !s->entry || !pos_now(s, &pos)) return HH_ERR_STATE;
    rc = hh_entry_join(s->entry, &pos);
    if (rc == HH_OK) apply_entry(s, &act);
    return rc;
}

/* Starts the leave at the current slot. */
hh_status_t hh_slot_leave(hh_slot_t *s)
{
    hh_slot_pos_t pos;

    if (!s || !s->entry || !pos_now(s, &pos)) return HH_ERR_STATE;
    return hh_entry_leave(s->entry, &pos);
}

/* Sends the entry request in a shared entry slot, when the back-off says it is time. */
static void send_request(hh_slot_t *s, const hh_slot_pos_t *pos)
{
    hh_entry_cmd_t c;
    hh_entry_act_t act = { 0 };
    hh_pdu_t *p = &s->pdu;
    uint8_t body[HH_ENTRY_CMD_MAX];
    size_t blen = 0, len = 0;

    if (!hh_entry_on_opportunity(s->entry, pos, &c)) return;
    apply_entry(s, &act);
    if (hh_entry_cmd_encode(&c, body, sizeof body, &blen) != HH_OK) { s->st.tx_failed++; return; }

    memset(p, 0, sizeof *p);
    p->seq = s->seq++;
    p->net_id = s->cfg.net_id;
    p->src = (uint16_t)s->cfg.node_id;
    p->time_quality = TIME_UNKNOWN;
    p->n_msg = 1;   /* a joining radio owns no slot: the bitmap is empty */
    p->msg[0] = (hh_pdu_msg_t){ .proto = HH_PDU_PROTO_COMMAND, .dst = HH_PDU_ADDR_BROADCAST, .cls = 0,
                                .data = body, .len = (uint16_t)blen };
    if (encode_pdu(s, p, &len) != HH_OK || !s->tx(s->tx_ctx, pos, s->buf, len)) {
        s->st.tx_failed++;
        return;
    }
    s->st.entry_pdus_tx++;
    HH_LOGT(COMP, "entry_request", "node=%u sf=%u slot=%u bytes=%zu", (unsigned)s->cfg.node_id, pos->superframe,
            pos->slot, len);
}

/* Moves the sent-message counters on to the frame of this own slot. A frame with no own slot leaves the earlier count at zero. */
static void roll_frame(hh_slot_t *s, const hh_slot_pos_t *pos)
{
    uint64_t frame = pos->abs_slot / s->cfg.slots_per_frame;

    if (frame == s->load_frame) return;
    if (frame == s->load_frame + 1u) memcpy(s->sent_prev, s->sent_cur, sizeof s->sent_prev);
    else memset(s->sent_prev, 0, sizeof s->sent_prev);
    memset(s->sent_cur, 0, sizeof s->sent_cur);
    s->load_frame = frame;
}

/* Fills the queue report with the offered load per class: the larger of what was sent in the last frame
 * and what was sent so far in this frame plus what is still queued, 255 at most. */
static void offered_load(const hh_slot_t *s, const uint8_t left[HH_ALLOC_CLASSES], uint8_t out[HH_ALLOC_CLASSES])
{
    for (unsigned k = 0; k < HH_ALLOC_CLASSES; k++) {
        uint32_t now = (uint32_t)s->sent_cur[k] + left[k];
        uint32_t v = s->sent_prev[k] > now ? s->sent_prev[k] : now;
        out[k] = v > 255u ? 255u : (uint8_t)v;
    }
}

/* Builds, encodes and sends the PDU for one own slot, then updates the counters. */
static void send_own(hh_slot_t *s, const hh_slot_pos_t *pos)
{
    hh_pdu_t *p = &s->pdu;
    size_t base, used = 0, len = 0, ctl = 0, user = 0, routing = 0;
    unsigned n_cmd = 0;
    uint8_t left[HH_ALLOC_CLASSES];

    roll_frame(s, pos);
    memset(p, 0, sizeof *p);
    p->seq = s->seq++;
    p->net_id = s->cfg.net_id;
    p->src = (uint16_t)s->cfg.node_id;
    p->time_quality = TIME_UNKNOWN;
    p->slot_bytes = (uint8_t)bitmap_bytes(&s->cfg);
    for (unsigned n = 0; n < s->cfg.slots_per_frame; n++)
        if ((s->cfg.own_slots >> n) & 1u) p->claims[n / 8u] |= (uint8_t)(1u << (n % 8u));
    p->has_qreport = true;   /* values are filled after packing, the size is fixed now */
    base = hh_pdu_overhead(p);

    /* Answers to entry requests and the leave go first, as control messages. */
    while (s->entry && p->n_msg < HH_PDU_MSG_MAX && n_cmd < HH_SLOT_CMD_MAX) {
        hh_pdu_msg_t *m = &p->msg[p->n_msg];
        size_t room = s->cfg.capacity - base - used, clen = 0;
        uint16_t dst = 0;

        if (room <= SUB_LEN) break;
        if (!hh_entry_next_command(s->entry, pos, (uint16_t)(room - SUB_LEN), s->cmd[n_cmd], &clen, &dst)) break;
        memset(m, 0, sizeof *m);
        m->proto = HH_PDU_PROTO_COMMAND;
        m->dst = dst;
        m->data = s->cmd[n_cmd++];
        m->len = (uint16_t)clen;
        used += hh_pdu_msg_size(m);
        ctl += hh_pdu_msg_size(m);
        p->n_msg++;
    }

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
        if (s->sent_cur[f->cls] < UINT16_MAX) s->sent_cur[f->cls]++;
        p->n_msg++;
    }
    p->more_data = queue_left(s->mac, left);
    offered_load(s, left, p->qreport);   /* demand: what the node offers per frame, whichever slot reports it */

    if (encode_pdu(s, p, &len) != HH_OK) {
        s->st.tx_failed++;
        return;
    }
    if (!s->tx(s->tx_ctx, pos, s->buf, len)) {
        s->st.tx_failed++;
        return;
    }
    s->st.pdus_tx++;
    s->st.reports_tx++;
    /* The own report is kept like any other, so every node allocates from what was sent on air. */
    if (hh_alloc_note(&s->demand, (uint16_t)s->cfg.node_id, p->qreport, pos->abs_slot) != HH_OK) s->st.demand_dropped++;
    if (s->entry) hh_entry_on_own_tx(s->entry, pos);
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
    HH_LOGT(COMP, "own_slot", "node=%u sf=%u frame=%u slot=%u msgs=%u bytes=%zu ctl=%zu user=%zu cap=%u "
            "queued=%u/%u/%u/%u/%u", (unsigned)s->cfg.node_id, pos->superframe, pos->frame, pos->slot, p->n_msg,
            len, ctl, user, s->cfg.capacity, p->qreport[0], p->qreport[1], p->qreport[2], p->qreport[3],
            p->qreport[4]);
}

/* Turns on slot allocation for this node. */
hh_status_t hh_slot_enable_alloc(hh_slot_t *s, const hh_alloc_params_t *p)
{
    if (!s || !p || hh_alloc_params_check(p) != HH_OK) return HH_ERR_INVAL;
    s->alloc_params = *p;
    s->alloc_params.slots_per_frame = s->cfg.slots_per_frame;
    s->alloc_params.fixed_slots |= s->cfg.entry_slots;
    s->alloc_on = true;
    s->alloc_pending = false;
    return HH_OK;
}

/* Answers whether a new voice flow fits the reserve, from the table and the current slot. */
hh_status_t hh_slot_voice_admit(const hh_slot_t *s, unsigned msgs, bool *admit)
{
    hh_slot_pos_t pos;

    if (!s || !admit || msgs == 0) return HH_ERR_INVAL;
    if (!s->alloc_on || !pos_now(s, &pos)) return HH_ERR_STATE;
    *admit = hh_alloc_voice_admit(&s->alloc_params, &s->demand, pos.abs_slot, (uint16_t)s->cfg.node_id, msgs);
    return HH_OK;
}

/* Sets the own slot set from the allocator or a clash, and tells the MAC and the entry logic. */
static void set_own(hh_slot_t *s, uint64_t mask)
{
    s->cfg.own_slots = mask;
    hh_mac_set_own_slots(s->mac, (uint16_t)count_bits(mask));
    if (s->entry) hh_entry_set_own(s->entry, mask);
}

/* True when the allocator may change this node's slots: no entry logic, or a member that is not leaving. */
static bool alloc_may_move(const hh_slot_t *s)
{
    return !s->entry || (hh_entry_state(s->entry) == HH_ENTRY_MEMBER && !s->entry->leaving);
}

/* Applies the map computed one superframe ago: this node takes its own row. A map without a slot for it is not applied. */
static void apply_map(hh_slot_t *s, const hh_slot_pos_t *pos)
{
    uint64_t mask = 0;

    s->alloc_pending = false;
    for (unsigned n = 0; n < s->cfg.slots_per_frame; n++)
        if (s->alloc_next.owner[n] == s->cfg.node_id) mask |= 1ull << n;
    if (!mask) {
        s->st.alloc_no_slot++;   /* a silent node could never report again, so it keeps what it has */
        return;
    }
    s->alloc_applied_slot = pos->abs_slot;
    if (mask == s->cfg.own_slots) return;
    set_own(s, mask);
    s->st.alloc_changes++;
    HH_LOGD(COMP, "alloc_applied", "node=%u sf=%u slots=%u mask=0x%llx", (unsigned)s->cfg.node_id, pos->superframe,
            count_bits(mask), (unsigned long long)mask);
}

/* At a superframe boundary: applies the map computed one superframe ago, then computes the next one from the table. */
static void alloc_boundary(hh_slot_t *s, const hh_slot_pos_t *pos)
{
    hh_alloc_params_t prm = s->alloc_params;

    if (!alloc_may_move(s)) {   /* a joining or leaving radio stays out of the allocation */
        s->alloc_pending = false;
        return;
    }
    if (s->alloc_pending) apply_map(s, pos);
    s->st.alloc_runs++;
    hh_alloc_evict_gone(&s->demand, pos->abs_slot, prm.gone_slots);   /* silent members free their table entry */
    if (s->entry) prm.fixed_slots |= hh_entry_provisional(s->entry);   /* grants not yet confirmed are not given away */
    if (hh_alloc_compute(&prm, &s->demand, pos->abs_slot, pos->superframe, &s->alloc_next) == HH_OK) {
        s->alloc_pending = true;
    } else {
        s->alloc_pending = false;
        s->st.alloc_errors++;
    }
}

/* Handles one slot: raises the superframe event, then sends if it is an own slot. */
static void run_slot(hh_slot_t *s, uint64_t abs, bool late)
{
    hh_slot_pos_t pos;

    pos_for(s, abs, &pos);
    s->st.slots_seen++;
    if (pos.frame == 0 && pos.slot == 0) {
        hh_mac_on_superframe(s->mac, pos.superframe);
        if (s->alloc_on) alloc_boundary(s, &pos);
    }
    if (s->entry) {
        hh_entry_act_t act;
        hh_entry_tick(s->entry, &pos, &act);
        apply_entry(s, &act);
    }
    if ((s->cfg.entry_slots >> pos.slot) & 1u) {
        if (s->entry && !late) send_request(s, &pos);
        return;
    }
    if (!((s->cfg.own_slots >> pos.slot) & 1u)) return;

    s->st.own_slots++;
    if (late) {
        s->st.own_slots_missed++;
        return;
    }
    send_own(s, &pos);
}

/* Runs every slot that ended since the last call, using the injected clock. */
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

/* Decodes a received PDU and passes its messages to the MAC. */
void hh_slot_receive(hh_slot_t *s, const hh_slot_pos_t *pos, const uint8_t *pdu, size_t len)
{
    hh_pdu_t *p;
    bool ok;
    hh_node_id_t src;
    hh_status_t rc;

    if (!s || !pos || !pdu) return;
    p = &s->pdu;
    rc = s->cfg.logic_adds_crc ? hh_pdu_decode_nocrc(pdu, len, p) : hh_pdu_decode(pdu, len, p);
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
    if (p->has_qreport) {
        s->st.reports_rx++;
        if (hh_alloc_note(&s->demand, p->src, p->qreport, pos->abs_slot) != HH_OK) s->st.demand_dropped++;
    }
    /* A lower node id claiming a slot of this node keeps it. Claims sent before the current map began are old news. */
    if (s->alloc_on && alloc_may_move(s) && pos->abs_slot >= s->alloc_applied_slot && p->src < s->cfg.node_id) {
        uint64_t clash = hh_entry_mask_from_bytes(p->claims, p->slot_bytes) & s->cfg.own_slots;
        if (clash) {
            set_own(s, s->cfg.own_slots & ~clash);
            s->st.alloc_clashes++;
            HH_LOGD(COMP, "alloc_clash", "node=%u from=%u mask=0x%llx", (unsigned)s->cfg.node_id, (unsigned)p->src,
                    (unsigned long long)clash);
        }
    }
    if (s->entry) hh_entry_on_pdu(s->entry, pos, p->src, hh_entry_mask_from_bytes(p->claims, p->slot_bytes));

    for (unsigned i = 0; i < p->n_msg; i++) {
        const hh_pdu_msg_t *m = &p->msg[i];
        hh_mac_rx_pdu_t up;
        hh_frame_kind_t kind;
        hh_node_id_t dst;

        /* MAC commands are read whatever their destination. */
        if (m->proto == HH_PDU_PROTO_COMMAND && s->entry) {
            hh_entry_act_t act;
            hh_entry_on_command(s->entry, pos, p->src, m->dst, m->data, m->len, &act);
            apply_entry(s, &act);
            s->st.rx_commands++;
            continue;
        }
        dst = air_to_node(m->dst, &ok);

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

/* Returns class 0 message bytes as thousandths of own-slot capacity. */
uint32_t hh_slot_control_share_permille(const hh_slot_stats_t *st)
{
    if (!st || !st->capacity_bytes) return 0;
    return (uint32_t)((st->control_bytes * 1000u) / st->capacity_bytes);
}
