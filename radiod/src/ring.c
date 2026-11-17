#include "hhsdr/radiod/ring.h"

#include <string.h>

hh_status_t hh_ring_init(hh_ring_t *r, uint8_t *mem, uint32_t *lens,
                         uint32_t slot_size, uint32_t count)
{
    if (!r || !mem || !lens || slot_size == 0 || count == 0 ||
        (count & (count - 1u)) != 0)
        return HH_ERR_INVAL;
    r->mem = mem;
    r->lens = lens;
    r->slot_size = slot_size;
    r->count = count;
    atomic_init(&r->prod, 0);
    atomic_init(&r->cons, 0);
    atomic_init(&r->full_drops, 0);
    return HH_OK;
}

hh_status_t hh_ring_push(hh_ring_t *r, const void *frame, uint32_t len)
{
    uint32_t prod, cons, idx;

    if (!r || !frame || len > r->slot_size)
        return HH_ERR_INVAL;
    prod = atomic_load_explicit(&r->prod, memory_order_relaxed);
    cons = atomic_load_explicit(&r->cons, memory_order_acquire);
    if (prod - cons >= r->count) {
        atomic_fetch_add_explicit(&r->full_drops, 1, memory_order_relaxed);
        return HH_ERR_NOMEM;
    }
    idx = prod & (r->count - 1u);
    memcpy(r->mem + (size_t)idx * r->slot_size, frame, len);
    r->lens[idx] = len;
    atomic_store_explicit(&r->prod, prod + 1u, memory_order_release);
    return HH_OK;
}

hh_status_t hh_ring_pop(hh_ring_t *r, void *out, uint32_t cap, uint32_t *len)
{
    uint32_t prod, cons, idx, n;

    if (!r || !out || !len)
        return HH_ERR_INVAL;
    cons = atomic_load_explicit(&r->cons, memory_order_relaxed);
    prod = atomic_load_explicit(&r->prod, memory_order_acquire);
    if (prod == cons)
        return HH_ERR_AGAIN;
    idx = cons & (r->count - 1u);
    n = r->lens[idx];
    if (cap < n)
        return HH_ERR_INVAL;
    memcpy(out, r->mem + (size_t)idx * r->slot_size, n);
    *len = n;
    atomic_store_explicit(&r->cons, cons + 1u, memory_order_release);
    return HH_OK;
}

uint32_t hh_ring_used(const hh_ring_t *r)
{
    return atomic_load_explicit(&r->prod, memory_order_acquire) -
           atomic_load_explicit(&r->cons, memory_order_acquire);
}

uint32_t hh_ring_full_drops(const hh_ring_t *r)
{
    return atomic_load_explicit(&r->full_drops, memory_order_relaxed);
}
