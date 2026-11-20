/*
 * Shared-memory file for the frame rings. The owner (radiod) creates the file
 * and lays out the rings in it. Every other process opens the same file by
 * path and works on the same memory through the ring interface in ring.h.
 *
 * Frames never pass through the owner. Opening needs only the path; who may
 * open it is set by the file permissions. Single-threaded, no allocation.
 */
#ifndef HHSDR_RING_SHM_H
#define HHSDR_RING_SHM_H

#include "hhsdr/ring/ring.h"

#define HH_RING_SHM_PATH_MAX 108

typedef struct {
    void  *base;
    size_t size;
    int    owner;                      /* 1: created here, removed on close */
    char   path[HH_RING_SHM_PATH_MAX];
} hh_ring_shm_t;

/* Owner: create the file at path (replacing a stale one), lay out the rings
 * and bind region to them. HH_ERR_INVAL on a bad geometry or path, HH_ERR_IO
 * if the file cannot be created or mapped. */
hh_status_t hh_ring_shm_create(hh_ring_shm_t *h, hh_ring_region_t *region, const char *path,
                               uint32_t rx_slots, uint32_t tx_slots, uint32_t slot_size);

/* Any process: open an existing file and bind region to its rings.
 * HH_ERR_IO if it cannot be opened, HH_ERR_INVAL if its header is not valid. */
hh_status_t hh_ring_shm_open(hh_ring_shm_t *h, hh_ring_region_t *region, const char *path);

/* Unmap. The owner also removes the file. Safe on a handle that is not open. */
void hh_ring_shm_close(hh_ring_shm_t *h);

#endif /* HHSDR_RING_SHM_H */
