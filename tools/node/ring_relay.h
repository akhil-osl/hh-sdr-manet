/*
 * Ring relay: stands in for the PL and the radio on a PC. It copies every
 * frame that a MAC process writes into its TX ring into the RX ring of the
 * other process, as the PL would after a transmission, with the CRC OK flag
 * set. The rings carry the PDU without its CRC-32 (ring contract, section 6.3).
 * Development tooling only, never linked into a daemon.
 */
#ifndef HH_NODE_RING_RELAY_H
#define HH_NODE_RING_RELAY_H

#include <stdint.h>

#include "hhsdr/ring/ring.h"

typedef struct {
    uint64_t copied;     /* frames moved from the TX ring to the RX ring */
    uint64_t rx_full;    /* RX ring full: frame dropped, as the PL does */
    uint64_t too_big;    /* frame longer than a slot of either ring: dropped */
    uint32_t rx_seq;     /* sequence written into the RX descriptors */
} relay_dir_t;

/* Moves up to max frames from the TX ring of one region to the RX ring of the
 * other. Returns how many frames it took from the TX ring. */
uint32_t relay_move(hh_ring_region_t *from, hh_ring_region_t *to, relay_dir_t *st, uint32_t max);

#endif /* HH_NODE_RING_RELAY_H */
