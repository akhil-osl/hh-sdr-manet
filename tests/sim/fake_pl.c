#include "fake_pl.h"

#include <string.h>

/* Clears the counters and test controls. */
void fake_pl_init(fake_pl_t *p)
{
    memset(p, 0, sizeof *p);
}

/* A burst arrives: writes the frame and its descriptor into the RX ring. */
hh_status_t fake_pl_receive(fake_pl_t *p, hh_ring_region_t *r, const uint8_t *frame,
                            uint16_t len, int crc_ok, const hh_ring_rx_desc_t *meta)
{
    hh_ring_slot_t s;
    hh_ring_rx_desc_t d;
    hh_status_t st;

    if (!crc_ok) {
        p->rx_crc_bad++;
        return HH_OK;
    }
    st = hh_ring_produce_begin(&r->rx, &s);
    if (st != HH_OK) {
        p->rx_ring_full++;
        return st;
    }
    if (len > s.cap) return HH_ERR_INVAL;
    memcpy(s.frame, frame, len);
    d = *meta;
    d.length = len;
    d.rx_seq = ++p->rx_seq;
    hh_ring_rx_desc_write(s.desc, &d);
    hh_ring_produce_commit(&r->rx, &s, HH_RING_RX_CRC_OK);
    p->rx_frames++;
    return HH_OK;
}

/* The PL pulls the frames of one TDMA frame and sends them, or a control-only frame. */
hh_status_t fake_pl_transmit(fake_pl_t *p, hh_ring_region_t *r, uint32_t superframe,
                             uint16_t frame, const uint16_t *own_slots, uint32_t n_own,
                             fake_pl_sent_t *out)
{
    uint32_t n = 0, i, k;
    int late = p->drop_next_reply;
    hh_status_t st;

    p->drop_next_reply = 0;
    p->tx_pulls++;
    st = hh_pull_frame(&r->tx, superframe, frame, p->items, FAKE_PL_MAX_ITEMS,
                       p->buf, (uint32_t)sizeof p->buf, &n, &p->servant);
    if (st != HH_OK) return st;
    if (late) {
        p->tx_pull_late++;
        n = 0;
    }
    for (i = 0; i < n_own; i++) {
        out[i].slot = own_slots[i];
        out[i].control_only = 1;
        out[i].length = 0;
        for (k = 0; k < n; k++) {
            if (p->items[k].slot != own_slots[i]) continue;
            memcpy(out[i].bytes, p->items[k].frame, p->items[k].length);
            out[i].length = p->items[k].length;
            out[i].control_only = 0;
            break;
        }
        if (out[i].control_only) p->tx_control_only++;
        else p->tx_frames_sent++;
    }
    return HH_OK;
}
