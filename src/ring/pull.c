#include "hhsdr/ring/pull.h"

#include <string.h>

/* Orders two targets: superframes by serial-number arithmetic so a wrap of the
 * 32-bit count keeps its order, then the frame inside the superframe. */
static int cmp_target(uint32_t sf_a, uint16_t fr_a, uint32_t sf_b, uint16_t fr_b)
{
    int32_t d = (int32_t)(sf_a - sf_b);

    if (d != 0) return d < 0 ? -1 : 1;
    if (fr_a != fr_b) return fr_a < fr_b ? -1 : 1;
    return 0;
}

/* Reply to a pull for one TDMA frame. */
hh_status_t hh_pull_frame(hh_ring_view_t *tx, uint32_t superframe, uint16_t frame,
                          hh_pull_item_t *items, uint32_t max_items,
                          uint8_t *buf, uint32_t buf_cap,
                          uint32_t *n_items, hh_pull_stats_t *stats)
{
    uint32_t n = 0, used = 0;
    hh_ring_slot_t s;
    hh_ring_tx_desc_t d;

    if (!tx || !items || !buf || !n_items || !stats) return HH_ERR_INVAL;
    stats->pulls++;

    while (hh_ring_consume_peek(tx, &s) == HH_OK) {
        int c;

        hh_ring_tx_desc_read(s.desc, &d);
        if (d.length > s.cap) {
            stats->bad_descriptors++;
            hh_ring_consume_release(tx, &s, 0);
            continue;
        }
        c = cmp_target(d.target_superframe, d.target_frame, superframe, frame);
        if (c < 0) {
            stats->stale_dropped++;
            hh_ring_consume_release(tx, &s, 0);
            continue;
        }
        if (c > 0) break;
        if (n >= max_items || d.length > buf_cap - used) {
            stats->truncated++;
            break;
        }
        memcpy(buf + used, s.frame, d.length);
        items[n].slot = d.target_slot;
        items[n].traffic_class = d.traffic_class;
        items[n].length = d.length;
        items[n].frame = buf + used;
        used += d.length;
        n++;
        stats->frames_returned++;
        hh_ring_consume_release(tx, &s, HH_RING_TX_TAKEN);
    }
    *n_items = n;
    return HH_OK;
}
