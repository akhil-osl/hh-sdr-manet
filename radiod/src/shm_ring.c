/* radiod shared-memory frame ring. See include/hhsdr/radiod/shm_ring.h. */
/* POSIX 2008 for O_CLOEXEC, ftruncate, pread: rh_agent compiles this file
 * with OpenCPI's strict -std=c99, which hides them otherwise. */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#include "hhsdr/radiod/shm_ring.h"

#include <errno.h>
#include <fcntl.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#define SHM_MAGIC   0x52465848u   /* "HXFR" little-endian */
#define SHM_VERSION 1u

struct hh_shm_hdr {
    uint32_t magic;
    uint32_t version;
    uint32_t slot_size;
    uint32_t count;
    _Atomic uint32_t prod;      /* written by the producer only */
    _Atomic uint32_t cons;      /* written by the consumer only */
    _Atomic uint32_t drops;     /* frames refused, ring full    */
    uint32_t reserved[9];       /* pad to 64 bytes              */
};

_Static_assert(sizeof(struct hh_shm_hdr) == 64, "shared header must stay 64 bytes");

static size_t layout(uint32_t slot_size, uint32_t count)
{
    return sizeof(struct hh_shm_hdr) + (size_t)count * 4u + (size_t)count * slot_size;
}

static void bind(hh_shm_ring_t *r, void *base, size_t size)
{
    r->hdr = (struct hh_shm_hdr *)base;
    r->lens = (uint32_t *)((uint8_t *)base + sizeof(struct hh_shm_hdr));
    r->mem = (uint8_t *)(r->lens + r->hdr->count);
    r->map_size = size;
}

hh_status_t hh_shm_ring_create(hh_shm_ring_t *r, const char *path,
                               uint32_t slot_size, uint32_t count)
{
    size_t size;
    void *base;
    int fd;

    if (!r || !path || slot_size == 0u || count == 0u || (count & (count - 1u)) != 0u ||
        strlen(path) >= sizeof r->path)
        return HH_ERR_INVAL;
    memset(r, 0, sizeof *r);
    size = layout(slot_size, count);
    (void)unlink(path);                          /* never inherit a stale ring */
    fd = open(path, O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (fd < 0)
        return HH_ERR_IO;
    if (ftruncate(fd, (off_t)size) != 0) {
        close(fd);
        (void)unlink(path);
        return HH_ERR_IO;
    }
    base = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (base == MAP_FAILED) {
        (void)unlink(path);
        return HH_ERR_IO;
    }
    memset(base, 0, size);
    {
        struct hh_shm_hdr *h = (struct hh_shm_hdr *)base;
        h->slot_size = slot_size;
        h->count = count;
        h->version = SHM_VERSION;
        atomic_init(&h->prod, 0u);
        atomic_init(&h->cons, 0u);
        atomic_init(&h->drops, 0u);
        atomic_thread_fence(memory_order_release);
        h->magic = SHM_MAGIC;                    /* last: the block is ready */
    }
    bind(r, base, size);
    r->owner = 1;
    snprintf(r->path, sizeof r->path, "%s", path);
    return HH_OK;
}

hh_status_t hh_shm_ring_attach(hh_shm_ring_t *r, const char *path)
{
    struct stat st;
    struct hh_shm_hdr h;
    void *base;
    int fd;

    if (!r || !path || strlen(path) >= sizeof r->path)
        return HH_ERR_INVAL;
    memset(r, 0, sizeof *r);
    fd = open(path, O_RDWR | O_CLOEXEC);
    if (fd < 0)
        return (errno == ENOENT) ? HH_ERR_NOTFOUND : HH_ERR_IO;
    if (fstat(fd, &st) != 0 || (size_t)st.st_size < sizeof h ||
        pread(fd, &h, sizeof h, 0) != (ssize_t)sizeof h ||
        h.magic != SHM_MAGIC || h.version != SHM_VERSION || h.slot_size == 0u ||
        h.count == 0u || (h.count & (h.count - 1u)) != 0u ||
        (size_t)st.st_size != layout(h.slot_size, h.count)) {
        close(fd);
        return HH_ERR_INVAL;
    }
    base = mmap(NULL, (size_t)st.st_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (base == MAP_FAILED)
        return HH_ERR_IO;
    bind(r, base, (size_t)st.st_size);
    r->owner = 0;
    snprintf(r->path, sizeof r->path, "%s", path);
    return HH_OK;
}

void hh_shm_ring_close(hh_shm_ring_t *r)
{
    if (!r || !r->hdr)
        return;
    (void)munmap(r->hdr, r->map_size);
    if (r->owner)
        (void)unlink(r->path);
    memset(r, 0, sizeof *r);
}

int hh_shm_ring_is_open(const hh_shm_ring_t *r)
{
    return (r != NULL && r->hdr != NULL);
}

hh_status_t hh_shm_ring_push(hh_shm_ring_t *r, const void *frame, uint32_t len)
{
    uint32_t prod, cons, idx;
    struct hh_shm_hdr *h;

    if (!r || !r->hdr || !frame || len > r->hdr->slot_size)
        return HH_ERR_INVAL;
    h = r->hdr;
    prod = atomic_load_explicit(&h->prod, memory_order_relaxed);
    cons = atomic_load_explicit(&h->cons, memory_order_acquire);
    if (prod - cons >= h->count) {
        atomic_fetch_add_explicit(&h->drops, 1u, memory_order_relaxed);
        return HH_ERR_NOMEM;
    }
    idx = prod & (h->count - 1u);
    memcpy(r->mem + (size_t)idx * h->slot_size, frame, len);
    r->lens[idx] = len;
    atomic_store_explicit(&h->prod, prod + 1u, memory_order_release);
    return HH_OK;
}

hh_status_t hh_shm_ring_pop(hh_shm_ring_t *r, void *out, uint32_t cap, uint32_t *len)
{
    uint32_t prod, cons, idx, n;
    struct hh_shm_hdr *h;

    if (!r || !r->hdr || !out || !len)
        return HH_ERR_INVAL;
    h = r->hdr;
    cons = atomic_load_explicit(&h->cons, memory_order_relaxed);
    prod = atomic_load_explicit(&h->prod, memory_order_acquire);
    if (prod == cons)
        return HH_ERR_AGAIN;
    idx = cons & (h->count - 1u);
    n = r->lens[idx];
    if (cap < n)
        return HH_ERR_INVAL;
    memcpy(out, r->mem + (size_t)idx * h->slot_size, n);
    *len = n;
    atomic_store_explicit(&h->cons, cons + 1u, memory_order_release);
    return HH_OK;
}

uint32_t hh_shm_ring_used(const hh_shm_ring_t *r)
{
    if (!r || !r->hdr)
        return 0u;
    return atomic_load_explicit(&r->hdr->prod, memory_order_acquire) -
           atomic_load_explicit(&r->hdr->cons, memory_order_acquire);
}

uint32_t hh_shm_ring_drops(const hh_shm_ring_t *r)
{
    return (r && r->hdr) ? atomic_load_explicit(&r->hdr->drops, memory_order_relaxed) : 0u;
}

uint32_t hh_shm_ring_count(const hh_shm_ring_t *r)     { return (r && r->hdr) ? r->hdr->count : 0u; }
uint32_t hh_shm_ring_slot_size(const hh_shm_ring_t *r) { return (r && r->hdr) ? r->hdr->slot_size : 0u; }
