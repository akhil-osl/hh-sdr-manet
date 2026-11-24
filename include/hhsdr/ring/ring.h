/*
 * Shared frame rings between radiod, the MAC process and the PL.
 *
 * One reserved memory region holds a 64-byte header, four ring indices, then an
 * RX ring and a TX ring. Each ring is a table of 32-byte descriptors and the
 * frame slots they describe. Descriptor i describes frame slot i.
 *
 * The region is shared between processes, so it holds offsets and no pointers.
 * Every field is little-endian. Each ring has one producer and one consumer.
 * Each index has one writer. Indices run freely and wrap by modulo, so the
 * slot count is a power of two. Frame bytes and descriptor fields are written
 * before the ready flag, and the ready flag before the index (release order).
 *
 * No locks, no threads, no allocation. Memory is supplied by the caller.
 *
 * A "slot" here is a place in memory. It is not a TDMA time slot.
 */
#ifndef HHSDR_RING_RING_H
#define HHSDR_RING_RING_H

#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>

#include "hhsdr/core/types.h"

#define HH_RING_VERSION        1u
#define HH_RING_HEADER_SIZE    64u
#define HH_RING_DESC_SIZE      32u
#define HH_RING_ALIGN          32u   /* Cortex-A9 cache line */
/* TODO: the magic value is not fixed by the contract; this is a placeholder. */
#define HH_RING_MAGIC          0x474E5248u

/* Index lines, one 32-byte line each, after the header. */
#define HH_RING_OFF_RX_WRITE   64u
#define HH_RING_OFF_RX_READ    96u
#define HH_RING_OFF_TX_WRITE   128u
#define HH_RING_OFF_TX_READ    160u
#define HH_RING_OFF_TABLES     192u

/* Descriptor flags word. Bit 0 is the ready flag in both rings. */
#define HH_RING_F_READY        0x00000001u   /* RX: FILLED. TX: READY */
#define HH_RING_RX_CRC_OK      0x00000002u
#define HH_RING_RX_AUTH_OK     0x00000004u
#define HH_RING_TX_TAKEN       0x00000002u

/* RX metric_flags bits. A clear bit means "no information", not zero. */
#define HH_RING_M_RSSI_VALID   0x0001u
#define HH_RING_M_SNR_VALID    0x0002u
#define HH_RING_M_GAIN_VALID   0x0004u
#define HH_RING_M_ARRIVAL_VALID 0x0008u
#define HH_RING_M_RSSI_CALIB   0x0010u

/* Traffic classes carried in the TX descriptor. */
#define HH_RING_CLASS_CONTROL   0u
#define HH_RING_CLASS_EMERGENCY 1u
#define HH_RING_CLASS_VOICE     2u
#define HH_RING_CLASS_VIDEO     3u
#define HH_RING_CLASS_DATA      4u

/* Decoded RX descriptor. Levels are signed, in 0.25 dB steps. */
typedef struct {
    uint32_t flags;
    uint16_t length;
    uint16_t slot_in_frame;     /* TDMA slot the frame arrived in */
    uint32_t superframe;
    int32_t  arrival_offset_ns;
    int16_t  rssi;
    int16_t  snr;
    int16_t  rx_gain;
    uint16_t metric_flags;
    uint32_t rx_seq;            /* frames delivered so far, to detect loss */
} hh_ring_rx_desc_t;

/* Decoded TX descriptor. Read by the software side only. */
typedef struct {
    uint32_t flags;
    uint16_t length;
    uint8_t  traffic_class;
    uint32_t target_superframe;
    uint16_t target_frame;
    uint16_t target_slot;       /* own TDMA slot inside that frame */
} hh_ring_tx_desc_t;

/* One ring inside an attached region. */
typedef struct {
    uint8_t          *desc;     /* descriptor table, count entries */
    uint8_t          *slots;    /* frame slots, count entries of slot_size */
    _Atomic uint32_t *widx;     /* written by the producer */
    _Atomic uint32_t *ridx;     /* written by the consumer */
    uint32_t          count;    /* power of two */
    uint32_t          slot_size;
} hh_ring_view_t;

/* An attached region: the two rings. */
typedef struct {
    hh_ring_view_t rx;          /* producer PL, consumer MAC process */
    hh_ring_view_t tx;          /* producer MAC process, consumer servant */
} hh_ring_region_t;

/* The slot being written or read: frame bytes, descriptor, capacity. */
typedef struct {
    uint8_t  *frame;
    uint8_t  *desc;
    uint32_t  cap;
    uint32_t  index;            /* free-running position */
} hh_ring_slot_t;

/* Bytes needed for a region. 0 if the geometry is not valid. */
size_t hh_ring_region_size(uint32_t rx_slots, uint32_t tx_slots, uint32_t slot_size);

/* radiod: write the header and clear the region. HH_ERR_INVAL on a bad
 * geometry, a base that is not 32-byte aligned, or a region too small. */
hh_status_t hh_ring_region_init(void *mem, size_t size, uint32_t rx_slots,
                                uint32_t tx_slots, uint32_t slot_size);

/* Any process: check the header and bind to the rings. HH_ERR_INVAL if the
 * header or an offset is not valid for this size. */
hh_status_t hh_ring_region_attach(hh_ring_region_t *r, void *mem, size_t size);

/* Producer: claim the next free slot. HH_ERR_NOMEM if the ring is full. The
 * slot is not visible to the consumer until hh_ring_produce_commit. */
hh_status_t hh_ring_produce_begin(hh_ring_view_t *v, hh_ring_slot_t *s);

/* Producer: publish the slot. Sets the ready flag on top of flags, then
 * advances the write index. The frame bytes and the descriptor fields other
 * than flags must already be written. */
void hh_ring_produce_commit(hh_ring_view_t *v, const hh_ring_slot_t *s, uint32_t flags);

/* Consumer: look at the oldest published slot without freeing it. HH_ERR_AGAIN
 * if there is none. The frame and descriptor are valid until release. */
hh_status_t hh_ring_consume_peek(hh_ring_view_t *v, hh_ring_slot_t *s);

/* Consumer: free the slot. Stores flags_after in the flags word (0 for RX,
 * HH_RING_TX_TAKEN for a frame that was sent), then advances the read index. */
void hh_ring_consume_release(hh_ring_view_t *v, const hh_ring_slot_t *s, uint32_t flags_after);

/* Slots currently published and not yet released. */
uint32_t hh_ring_view_used(const hh_ring_view_t *v);

/* Descriptor fields other than the flags word. The flags word is written by
 * hh_ring_produce_commit so that it is the last store. */
void hh_ring_rx_desc_write(uint8_t desc[HH_RING_DESC_SIZE], const hh_ring_rx_desc_t *d);
void hh_ring_rx_desc_read(const uint8_t desc[HH_RING_DESC_SIZE], hh_ring_rx_desc_t *d);
void hh_ring_tx_desc_write(uint8_t desc[HH_RING_DESC_SIZE], const hh_ring_tx_desc_t *d);
void hh_ring_tx_desc_read(const uint8_t desc[HH_RING_DESC_SIZE], hh_ring_tx_desc_t *d);

#endif /* HHSDR_RING_RING_H */
