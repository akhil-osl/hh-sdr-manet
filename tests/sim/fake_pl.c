#include "fake_pl.h"

#include <string.h>

#include "hhsdr/mac/pdu.h"

/* Clears the counters and test controls. */
void fake_pl_init(fake_pl_t *p)
{
    memset(p, 0, sizeof *p);
}

/* Reads the CRC-32 at the end of an air frame, low byte first. */
static uint32_t air_crc(const uint8_t *frame, uint16_t len)
{
    return (uint32_t)frame[len - 4] | ((uint32_t)frame[len - 3] << 8) |
           ((uint32_t)frame[len - 2] << 16) | ((uint32_t)frame[len - 1] << 24);
}

/* A burst arrives: checks and removes the CRC-32, then writes the PDU and its descriptor into the RX ring. */
hh_status_t fake_pl_receive(fake_pl_t *p, hh_ring_region_t *r, const uint8_t *frame,
                            uint16_t len, const hh_ring_rx_desc_t *meta)
{
    hh_ring_slot_t s;
    hh_ring_rx_desc_t d;
    hh_status_t st;

    if (!p || !r || !frame || !meta || len < HH_PDU_CRC_LEN) return HH_ERR_INVAL;
    len = (uint16_t)(len - HH_PDU_CRC_LEN);                     /* the PDU, without the CRC-32 */
    if (hh_crc32(frame, len) != air_crc(frame, (uint16_t)(len + HH_PDU_CRC_LEN))) {
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
    uint32_t n = 0, i, k, b, crc;
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
            if (p->items[k].length + HH_PDU_CRC_LEN > sizeof out[i].bytes) break;   /* cannot go on air */
            memcpy(out[i].bytes, p->items[k].frame, p->items[k].length);
            crc = hh_crc32(out[i].bytes, p->items[k].length);       /* the PL appends the CRC-32 */
            for (b = 0; b < HH_PDU_CRC_LEN; b++) out[i].bytes[p->items[k].length + b] = (uint8_t)(crc >> (8 * b));
            out[i].length = (uint16_t)(p->items[k].length + HH_PDU_CRC_LEN);
            out[i].control_only = 0;
            break;
        }
        if (out[i].control_only) p->tx_control_only++;
        else p->tx_frames_sent++;
    }
    return HH_OK;
}
