#include "ring_relay.h"

#include <string.h>

/* Moves up to max frames from the TX ring of one region to the RX ring of the other. */
uint32_t relay_move(hh_ring_region_t *from, hh_ring_region_t *to, relay_dir_t *st, uint32_t max)
{
    uint32_t taken = 0;

    while (taken < max) {
        hh_ring_slot_t in, out;
        hh_ring_tx_desc_t td;
        hh_ring_rx_desc_t rd;

        if (hh_ring_consume_peek(&from->tx, &in) != HH_OK) break;
        taken++;
        hh_ring_tx_desc_read(in.desc, &td);
        if (td.length > in.cap || td.length > to->rx.slot_size) {
            st->too_big++;
        } else if (hh_ring_produce_begin(&to->rx, &out) != HH_OK) {
            st->rx_full++;
        } else {
            memcpy(out.frame, in.frame, td.length);
            memset(&rd, 0, sizeof rd);
            rd.length = td.length;
            rd.slot_in_frame = td.target_slot;
            rd.superframe = td.target_superframe;
            rd.rx_seq = ++st->rx_seq;
            hh_ring_rx_desc_write(out.desc, &rd);
            hh_ring_produce_commit(&to->rx, &out, HH_RING_RX_CRC_OK);
            st->copied++;
        }
        hh_ring_consume_release(&from->tx, &in, HH_RING_TX_TAKEN);
    }
    return taken;
}
