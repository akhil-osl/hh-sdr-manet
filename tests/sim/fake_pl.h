/*
 * Fake PL for host tests. It plays the PL side of the ring interface: it
 * writes received frames and descriptors into the RX ring, and it asks for the
 * TX frames of a TDMA frame through the servant pull logic.
 *
 * Test infrastructure. Never linked into production targets.
 */
#ifndef HH_FAKE_PL_H
#define HH_FAKE_PL_H

#include <stdint.h>

#include "hhsdr/ring/pull.h"
#include "hhsdr/ring/ring.h"

#define FAKE_PL_MAX_FRAME 2048u
#define FAKE_PL_MAX_ITEMS 32u

typedef struct {
    /* PL counters */
    uint32_t rx_frames, rx_crc_bad, rx_ring_full;
    uint32_t tx_pulls, tx_frames_sent, tx_control_only, tx_pull_late;
    uint32_t rx_seq;
    /* servant counters */
    hh_pull_stats_t servant;
    /* test control: the next reply arrives after the slot edge */
    int drop_next_reply;
    /* pull working space */
    hh_pull_item_t items[FAKE_PL_MAX_ITEMS];
    uint8_t buf[FAKE_PL_MAX_ITEMS * FAKE_PL_MAX_FRAME];
} fake_pl_t;

/* What the PL put on the air for one own slot. */
typedef struct {
    uint16_t slot;
    int      control_only;          /* no frame arrived in time */
    uint16_t length;
    uint8_t  bytes[FAKE_PL_MAX_FRAME];
} fake_pl_sent_t;

void fake_pl_init(fake_pl_t *p);

/* A burst arrives. A bad CRC is counted and not delivered. A full RX ring drops
 * the frame and counts it. meta gives the descriptor fields other than length
 * and the receive count, which the fake sets itself. */
hh_status_t fake_pl_receive(fake_pl_t *p, hh_ring_region_t *r, const uint8_t *frame,
                            uint16_t len, int crc_ok, const hh_ring_rx_desc_t *meta);

/* The PL pulls for (superframe, frame) and fills out[i] for own_slots[i]: the
 * frame bytes if the reply carried one, otherwise a control-only frame. */
hh_status_t fake_pl_transmit(fake_pl_t *p, hh_ring_region_t *r, uint32_t superframe,
                             uint16_t frame, const uint16_t *own_slots, uint32_t n_own,
                             fake_pl_sent_t *out);

#endif /* HH_FAKE_PL_H */
