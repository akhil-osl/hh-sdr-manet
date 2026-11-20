/*
 * TX pull logic: what the servant does when the PL asks for the frames of one
 * TDMA frame. It reads the TX ring, keeps the frames whose target matches the
 * request, drops late ones, and copies the frame bytes into a caller buffer so
 * the ring slots can be freed at once.
 *
 * This is plain logic with no ORB. The CORBA servant wraps hh_pull_frame.
 * It never waits for the MAC process: if nothing matches, the list is empty.
 */
#ifndef HHSDR_RING_PULL_H
#define HHSDR_RING_PULL_H

#include <stdint.h>

#include "hhsdr/ring/ring.h"

/* One frame in a pull reply. The bytes live in the caller buffer. */
typedef struct {
    uint16_t       slot;            /* own TDMA slot inside the requested frame */
    uint8_t        traffic_class;
    uint16_t       length;
    const uint8_t *frame;
} hh_pull_item_t;

/* Counters kept by the servant. They only grow. */
typedef struct {
    uint32_t pulls;
    uint32_t frames_returned;
    uint32_t stale_dropped;         /* target earlier than the request */
    uint32_t bad_descriptors;       /* length larger than the slot */
    uint32_t truncated;             /* stopped early: items or buffer full */
} hh_pull_stats_t;

/* Reply to a pull for (superframe, frame). Frames written in target order are
 * returned in that order. A frame with an earlier target is dropped and counted,
 * a frame with a later target stays in the ring. Matching frames are marked
 * TAKEN and their slots freed.
 *
 * If max_items or buf_cap is reached, the rest stay in the ring and truncated
 * is counted. HH_ERR_INVAL on a null argument. *n_items is the reply size. */
hh_status_t hh_pull_frame(hh_ring_view_t *tx, uint32_t superframe, uint16_t frame,
                          hh_pull_item_t *items, uint32_t max_items,
                          uint8_t *buf, uint32_t buf_cap,
                          uint32_t *n_items, hh_pull_stats_t *stats);

#endif /* HHSDR_RING_PULL_H */
