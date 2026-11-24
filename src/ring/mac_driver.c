#include "hhsdr/ring/mac_driver.h"

#include <string.h>

hh_status_t hh_ring_mac_init(hh_ring_mac_t *m, hh_ring_region_t *region, hh_slot_t *slot,
                             uint16_t lead_frames)
{
    if (!m || !region || !slot || lead_frames == 0) return HH_ERR_INVAL;
    if (!slot->cfg.logic_adds_crc) return HH_ERR_STATE;   /* ring frames carry no CRC-32 */
    memset(m, 0, sizeof *m);
    m->region = region;
    m->slot = slot;
    m->lead_frames = lead_frames;
    return HH_OK;
}

/* Ring class for a PDU: the most urgent class among its messages, control for
 * a PDU with none. Classes 0 to 4 are numbered alike in the PDU and the ring. */
static uint8_t class_of(const uint8_t *pdu, size_t len)
{
    hh_pdu_t p;
    uint8_t best = HH_RING_CLASS_CONTROL;
    bool any = false;

    if (hh_pdu_decode_nocrc(pdu, len, &p) != HH_OK) return HH_RING_CLASS_CONTROL;
    for (unsigned i = 0; i < p.n_msg; i++) {
        uint8_t c = p.msg[i].cls > HH_RING_CLASS_DATA ? HH_RING_CLASS_DATA : p.msg[i].cls;
        if (!any || c < best) best = c;
        any = true;
    }
    return best;
}

/* Scheduler transmit function: queues the PDU for the same slot of a later frame. */
bool hh_ring_mac_tx(void *ctx, const hh_slot_pos_t *pos, const uint8_t *pdu, size_t len)
{
    hh_ring_mac_t *m = ctx;
    hh_ring_view_t *tx;
    hh_ring_slot_t s;
    hh_ring_tx_desc_t d;
    uint32_t frame, super;

    if (!m || !pos || !pdu) return false;
    tx = &m->region->tx;
    if (len > tx->slot_size) {
        m->st.tx_too_big++;
        return false;
    }
    if (hh_ring_produce_begin(tx, &s) != HH_OK) {
        m->st.tx_ring_full++;
        return false;
    }
    frame = (uint32_t)pos->frame + m->lead_frames;
    super = pos->superframe + frame / m->slot->cfg.frames_per_superframe;
    frame %= m->slot->cfg.frames_per_superframe;

    memcpy(s.frame, pdu, len);
    memset(&d, 0, sizeof d);
    d.length = (uint16_t)len;
    d.traffic_class = class_of(pdu, len);
    d.target_superframe = super;
    d.target_frame = (uint16_t)frame;
    d.target_slot = pos->slot;
    hh_ring_tx_desc_write(s.desc, &d);
    hh_ring_produce_commit(tx, &s, 0);
    m->st.tx_frames++;
    return true;
}

/* Position for a received frame: superframe and slot from the descriptor, the
 * frame inside the superframe from the clock, because the descriptor has none. */
static void rx_pos(const hh_slot_t *s, const hh_ring_rx_desc_t *d, hh_slot_pos_t *pos)
{
    const hh_slot_config_t *c = &s->cfg;
    uint64_t per_frame = c->slots_per_frame;
    uint64_t per_super = per_frame * c->frames_per_superframe;
    hh_time_ms_t now = hh_now(s->clock);
    uint64_t abs_now = now >= c->epoch_ms ? (now - c->epoch_ms) / c->slot_ms : 0;

    pos->superframe = d->superframe;
    pos->slot = d->slot_in_frame;
    pos->frame = (uint16_t)((abs_now % per_super) / per_frame);
    pos->abs_slot = (uint64_t)d->superframe * per_super + (uint64_t)pos->frame * per_frame + d->slot_in_frame;
}

/* Drains the RX ring into the scheduler. */
uint32_t hh_ring_mac_poll(hh_ring_mac_t *m)
{
    hh_ring_view_t *rx;
    hh_ring_slot_t s;
    uint32_t n = 0;

    if (!m) return 0;
    rx = &m->region->rx;
    while (hh_ring_consume_peek(rx, &s) == HH_OK) {
        hh_ring_rx_desc_t d;
        hh_slot_pos_t pos;

        hh_ring_rx_desc_read(s.desc, &d);
        if (d.length > s.cap) {
            m->st.rx_bad_descriptor++;
        } else if (!(d.flags & HH_RING_RX_CRC_OK)) {
            m->st.rx_crc_bad++;
        } else {
            rx_pos(m->slot, &d, &pos);
            hh_slot_receive(m->slot, &pos, s.frame, d.length);
            m->st.rx_frames++;
            n++;
        }
        hh_ring_consume_release(rx, &s, 0);
    }
    return n;
}
