/*
 * radiod frame ring: a single-producer, single-consumer ring of fixed-size
 * frame slots. One instance is the RX ring (producer: PL, consumer: MAC), a
 * second is the TX ring (producer: MAC, consumer: PL OpenCPI worker).
 *
 * Producer and consumer indices run freely and wrap by modulo, so the slot
 * count must be a power of two. Each index is written by one side only; the
 * other side reads it with acquire/release ordering. No locks, no threads.
 *
 * Scope: this is the software model of the ring. Storage is supplied by the
 * caller, so radiod can later point it at reserved DMA memory. It defines no
 * PL-visible descriptor bytes, ring sizes or slot sizes; those are still open
 * (see unknown.md). The per-slot length is a software field only.
 */
#ifndef HHSDR_RADIOD_RING_H
#define HHSDR_RADIOD_RING_H

#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>

#include "hhsdr/core/types.h"

typedef struct {
    uint8_t  *mem;        /* count * slot_size bytes, caller owned          */
    uint32_t *lens;       /* count entries, frame length per slot           */
    uint32_t  slot_size;  /* capacity of one slot, bytes                    */
    uint32_t  count;      /* slots, power of two                            */
    _Atomic uint32_t prod; /* next slot to write (producer only)            */
    _Atomic uint32_t cons; /* next slot to read (consumer only)             */
    _Atomic uint32_t full_drops; /* frames refused because the ring was full */
} hh_ring_t;

/* Bind a ring to caller storage. HH_ERR_INVAL on null storage, zero slot
 * size, or a slot count that is not a power of two. All slots start empty. */
hh_status_t hh_ring_init(hh_ring_t *r, uint8_t *mem, uint32_t *lens,
                         uint32_t slot_size, uint32_t count);

/* Producer: copy one frame into the next free slot. HH_ERR_INVAL if len
 * exceeds the slot size; HH_ERR_NOMEM if the ring is full (the frame is
 * dropped and counted, a slot the consumer owns is never overwritten). */
hh_status_t hh_ring_push(hh_ring_t *r, const void *frame, uint32_t len);

/* Consumer: copy the oldest frame out and free its slot. HH_ERR_AGAIN if
 * empty; HH_ERR_INVAL if cap is smaller than the frame (frame stays queued). */
hh_status_t hh_ring_pop(hh_ring_t *r, void *out, uint32_t cap, uint32_t *len);

/* Frames currently queued. */
uint32_t hh_ring_used(const hh_ring_t *r);

/* Frames refused so far because the ring was full. */
uint32_t hh_ring_full_drops(const hh_ring_t *r);

#endif
