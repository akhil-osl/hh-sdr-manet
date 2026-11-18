/*
 * radiod shared-memory frame ring: the THIRD ring, the RX frame ring.
 *
 * radiod OWNS it (creates it at start-up, removes it on exit). rh_agent, a
 * separate process, ATTACHES to it and pushes every complete received frame
 * (the whole MAC PDU: header, data, CRC) into it. radiod is the consumer;
 * the MAC will pop it. Same single-producer / single-consumer rules as
 * hh_ring_t (ring.h), but the indices live in the shared block, so two
 * processes can use it. ring.h itself is unchanged.
 *
 * Shared block (one mmap'd file, all fields little-endian host order):
 *   header 64 B : magic, version, slot_size, count, prod, cons, drops, ...
 *   lens        : count x uint32   (bytes in each slot)
 *   slots       : count x slot_size bytes
 *
 * Indices run freely and wrap by modulo; count must be a power of two.
 * The header is private to shm_ring.c, so this file is safe to include
 * from C++ (rh_agent).
 */
#ifndef HHSDR_RADIOD_SHM_RING_H
#define HHSDR_RADIOD_SHM_RING_H

#include <stddef.h>
#include <stdint.h>

#include "hhsdr/core/types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Where radiod puts the RX frame ring, and its shape: 64 slots of one
 * FEC-off radio frame (70 words = 280 bytes). */
#define HH_RX_FRAME_RING_PATH  "/tmp/hh_rx_frames.ring"
#define HH_RX_FRAME_RING_SLOT  280u
#define HH_RX_FRAME_RING_COUNT 64u

struct hh_shm_hdr;

typedef struct {
    struct hh_shm_hdr *hdr;   /* NULL = not open */
    uint32_t *lens;
    uint8_t  *mem;
    size_t    map_size;
    int       owner;          /* 1 = created it (radiod), removes it on close */
    char      path[108];
} hh_shm_ring_t;

/* radiod: create (or re-create) the ring file and map it. All slots empty. */
hh_status_t hh_shm_ring_create(hh_shm_ring_t *r, const char *path,
                               uint32_t slot_size, uint32_t count);

/* rh_agent: map an existing ring. HH_ERR_NOTFOUND if radiod has not made
 * it; HH_ERR_INVAL if the file is not a ring of this version. */
hh_status_t hh_shm_ring_attach(hh_shm_ring_t *r, const char *path);

/* Unmap; the owner also removes the file. Safe on a ring never opened. */
void        hh_shm_ring_close(hh_shm_ring_t *r);

int         hh_shm_ring_is_open(const hh_shm_ring_t *r);

/* Producer. HH_ERR_INVAL: too long / not open. HH_ERR_NOMEM: full (counted
 * in drops, nothing overwritten). */
hh_status_t hh_shm_ring_push(hh_shm_ring_t *r, const void *frame, uint32_t len);

/* Consumer. HH_ERR_AGAIN: empty. HH_ERR_INVAL: cap too small (frame stays). */
hh_status_t hh_shm_ring_pop(hh_shm_ring_t *r, void *out, uint32_t cap, uint32_t *len);

uint32_t    hh_shm_ring_used(const hh_shm_ring_t *r);
uint32_t    hh_shm_ring_drops(const hh_shm_ring_t *r);
uint32_t    hh_shm_ring_count(const hh_shm_ring_t *r);
uint32_t    hh_shm_ring_slot_size(const hh_shm_ring_t *r);

#ifdef __cplusplus
}
#endif

#endif
