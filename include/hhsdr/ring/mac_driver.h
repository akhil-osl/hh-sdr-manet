/*
 * MAC side of the frame rings. Sits between the slot scheduler and the ring
 * region: the scheduler's transmit function writes each PDU into the TX ring
 * with a descriptor, and the receive side drains the RX ring into the scheduler.
 *
 * The PL pulls the TX frames of a TDMA frame ahead of time, so a PDU is
 * targeted lead_frames TDMA frames after the slot the scheduler built it in.
 * Single-threaded, no allocation. No ORB and no hardware access here.
 */
#ifndef HHSDR_RING_MAC_DRIVER_H
#define HHSDR_RING_MAC_DRIVER_H

#include "hhsdr/mac/slot.h"
#include "hhsdr/ring/ring.h"

typedef struct {
    uint64_t tx_frames;        /* PDUs written to the TX ring */
    uint64_t tx_ring_full;     /* PDU refused, ring full */
    uint64_t tx_too_big;       /* PDU larger than a ring slot */
    uint64_t rx_frames;        /* frames passed to the scheduler */
    uint64_t rx_crc_bad;       /* frames the PL marked bad, not passed up */
    uint64_t rx_bad_descriptor;/* length larger than the slot, dropped */
} hh_ring_mac_stats_t;

typedef struct {
    hh_ring_region_t   *region;
    hh_slot_t          *slot;
    uint16_t            lead_frames;   /* TDMA frames between building and sending, at least 1 */
    hh_ring_mac_stats_t st;
} hh_ring_mac_t;

/* HH_ERR_INVAL on a null argument or lead_frames of 0. HH_ERR_STATE if the
 * scheduler is not configured with logic_adds_crc: frames in the rings carry no
 * CRC-32, because the PL appends it on transmit and removes it on receive. */
hh_status_t hh_ring_mac_init(hh_ring_mac_t *m, hh_ring_region_t *region, hh_slot_t *slot,
                             uint16_t lead_frames);

/* Transmit function for hh_slot_init; pass the hh_ring_mac_t as ctx. */
bool hh_ring_mac_tx(void *ctx, const hh_slot_pos_t *pos, const uint8_t *pdu, size_t len);

/* Passes every frame waiting in the RX ring to the scheduler. Returns how many. */
uint32_t hh_ring_mac_poll(hh_ring_mac_t *m);

#endif /* HHSDR_RING_MAC_DRIVER_H */
