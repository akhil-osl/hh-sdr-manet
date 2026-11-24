#include "hhsdr/ring/ring.h"

#include <stdint.h>
#include <string.h>

/* The flags word is stored as a native 32-bit atomic, so the layout is
 * little-endian only on a little-endian host. Both targets are. */
_Static_assert(__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__, "ring layout is little-endian");

enum {
    H_MAGIC = 0, H_VERSION = 4, H_DESC_SIZE = 6, H_RX_SLOTS = 8, H_TX_SLOTS = 12,
    H_SLOT_SIZE = 16, H_RX_DESC = 20, H_RX_SLOT = 24, H_TX_DESC = 28, H_TX_SLOT = 32
};

/* Writes a 16-bit value, low byte first. */
static void put16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

/* Writes a 32-bit value, low byte first. */
static void put32(uint8_t *p, uint32_t v)
{
    put16(p, (uint16_t)v);
    put16(p + 2, (uint16_t)(v >> 16));
}

/* Reads a 16-bit value, low byte first. */
static uint16_t get16(const uint8_t *p)
{
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

/* Reads a 32-bit value, low byte first. */
static uint32_t get32(const uint8_t *p)
{
    return (uint32_t)get16(p) | ((uint32_t)get16(p + 2) << 16);
}

/* Views a 32-byte-aligned location in the region as an atomic index or flags word. */
static _Atomic uint32_t *atom(uint8_t *p)
{
    return (_Atomic uint32_t *)(void *)p;
}

/* True for a non-zero power of two. */
static int is_pow2(uint32_t n)
{
    return n != 0 && (n & (n - 1u)) == 0;
}

/* True if the geometry is valid for this layout version. */
static int geometry_ok(uint32_t rx_slots, uint32_t tx_slots, uint32_t slot_size)
{
    return is_pow2(rx_slots) && is_pow2(tx_slots) && slot_size != 0 &&
           (slot_size % HH_RING_ALIGN) == 0;
}

/* The four table offsets of the only layout this version defines. Returns 0
 * if the region would not fit in 32 bits. */
static int layout(uint32_t rx_slots, uint32_t tx_slots, uint32_t slot_size,
                  uint32_t *rx_desc, uint32_t *rx_slot, uint32_t *tx_desc,
                  uint32_t *tx_slot, uint64_t *total)
{
    uint64_t o = HH_RING_OFF_TABLES;

    *rx_desc = (uint32_t)o;
    o += (uint64_t)rx_slots * HH_RING_DESC_SIZE;
    *rx_slot = (uint32_t)o;
    o += (uint64_t)rx_slots * slot_size;
    if (o > UINT32_MAX) return 0;
    *tx_desc = (uint32_t)o;
    o += (uint64_t)tx_slots * HH_RING_DESC_SIZE;
    *tx_slot = (uint32_t)o;
    o += (uint64_t)tx_slots * slot_size;
    if (o > UINT32_MAX) return 0;
    *total = o;
    return 1;
}

/* Bytes needed for a region. 0 if the geometry is not valid. */
size_t hh_ring_region_size(uint32_t rx_slots, uint32_t tx_slots, uint32_t slot_size)
{
    uint32_t a, b, c, d;
    uint64_t total;

    if (!geometry_ok(rx_slots, tx_slots, slot_size)) return 0;
    if (!layout(rx_slots, tx_slots, slot_size, &a, &b, &c, &d, &total)) return 0;
    return (size_t)total;
}

/* Writes the header and clears the region. */
hh_status_t hh_ring_region_init(void *mem, size_t size, uint32_t rx_slots,
                                uint32_t tx_slots, uint32_t slot_size)
{
    uint8_t *p = mem;
    uint32_t rd, rs, td, ts;
    uint64_t total;

    if (!mem || ((uintptr_t)mem % HH_RING_ALIGN) != 0) return HH_ERR_INVAL;
    if (!geometry_ok(rx_slots, tx_slots, slot_size)) return HH_ERR_INVAL;
    if (!layout(rx_slots, tx_slots, slot_size, &rd, &rs, &td, &ts, &total)) return HH_ERR_INVAL;
    if (size < total) return HH_ERR_INVAL;

    memset(p, 0, (size_t)total);
    put32(p + H_MAGIC, HH_RING_MAGIC);
    put16(p + H_VERSION, (uint16_t)HH_RING_VERSION);
    put16(p + H_DESC_SIZE, (uint16_t)HH_RING_DESC_SIZE);
    put32(p + H_RX_SLOTS, rx_slots);
    put32(p + H_TX_SLOTS, tx_slots);
    put32(p + H_SLOT_SIZE, slot_size);
    put32(p + H_RX_DESC, rd);
    put32(p + H_RX_SLOT, rs);
    put32(p + H_TX_DESC, td);
    put32(p + H_TX_SLOT, ts);
    return HH_OK;
}

/* Binds one ring to its tables and index lines. */
static void bind_ring(hh_ring_view_t *v, uint8_t *base, uint32_t desc_off, uint32_t slot_off,
                      uint32_t count, uint32_t slot_size, uint32_t widx_off, uint32_t ridx_off)
{
    v->desc = base + desc_off;
    v->slots = base + slot_off;
    v->count = count;
    v->slot_size = slot_size;
    v->widx = atom(base + widx_off);
    v->ridx = atom(base + ridx_off);
}

/* Checks the header and binds to the rings. */
hh_status_t hh_ring_region_attach(hh_ring_region_t *r, void *mem, size_t size)
{
    uint8_t *p = mem;
    uint32_t rx, tx, ss, rd, rs, td, ts;
    uint64_t total;

    if (!r || !mem || ((uintptr_t)mem % HH_RING_ALIGN) != 0 || size < HH_RING_OFF_TABLES)
        return HH_ERR_INVAL;
    if (get32(p + H_MAGIC) != HH_RING_MAGIC || get16(p + H_VERSION) != HH_RING_VERSION ||
        get16(p + H_DESC_SIZE) != HH_RING_DESC_SIZE)
        return HH_ERR_INVAL;
    rx = get32(p + H_RX_SLOTS);
    tx = get32(p + H_TX_SLOTS);
    ss = get32(p + H_SLOT_SIZE);
    if (!geometry_ok(rx, tx, ss)) return HH_ERR_INVAL;
    if (!layout(rx, tx, ss, &rd, &rs, &td, &ts, &total) || size < total) return HH_ERR_INVAL;
    if (get32(p + H_RX_DESC) != rd || get32(p + H_RX_SLOT) != rs ||
        get32(p + H_TX_DESC) != td || get32(p + H_TX_SLOT) != ts)
        return HH_ERR_INVAL;

    bind_ring(&r->rx, p, rd, rs, rx, ss, HH_RING_OFF_RX_WRITE, HH_RING_OFF_RX_READ);
    bind_ring(&r->tx, p, td, ts, tx, ss, HH_RING_OFF_TX_WRITE, HH_RING_OFF_TX_READ);
    return HH_OK;
}

/* Fills a slot handle for the free-running position pos. */
static void slot_at(const hh_ring_view_t *v, hh_ring_slot_t *s, uint32_t pos)
{
    uint32_t i = pos & (v->count - 1u);

    s->frame = v->slots + (size_t)i * v->slot_size;
    s->desc = v->desc + (size_t)i * HH_RING_DESC_SIZE;
    s->cap = v->slot_size;
    s->index = pos;
}

/* Producer: claims the next free slot. */
hh_status_t hh_ring_produce_begin(hh_ring_view_t *v, hh_ring_slot_t *s)
{
    uint32_t w, r;

    if (!v || !s) return HH_ERR_INVAL;
    w = atomic_load_explicit(v->widx, memory_order_relaxed);
    r = atomic_load_explicit(v->ridx, memory_order_acquire);
    if (w - r >= v->count) return HH_ERR_NOMEM;
    slot_at(v, s, w);
    return HH_OK;
}

/* Producer: publishes the slot, flags word first and then the write index. */
void hh_ring_produce_commit(hh_ring_view_t *v, const hh_ring_slot_t *s, uint32_t flags)
{
    atomic_store_explicit(atom(s->desc), flags | HH_RING_F_READY, memory_order_release);
    atomic_store_explicit(v->widx, s->index + 1u, memory_order_release);
}

/* Consumer: looks at the oldest published slot. */
hh_status_t hh_ring_consume_peek(hh_ring_view_t *v, hh_ring_slot_t *s)
{
    uint32_t r, w;

    if (!v || !s) return HH_ERR_INVAL;
    r = atomic_load_explicit(v->ridx, memory_order_relaxed);
    w = atomic_load_explicit(v->widx, memory_order_acquire);
    if (r == w) return HH_ERR_AGAIN;
    slot_at(v, s, r);
    if ((atomic_load_explicit(atom(s->desc), memory_order_acquire) & HH_RING_F_READY) == 0)
        return HH_ERR_AGAIN;
    return HH_OK;
}

/* Consumer: frees the slot, flags word first and then the read index. */
void hh_ring_consume_release(hh_ring_view_t *v, const hh_ring_slot_t *s, uint32_t flags_after)
{
    atomic_store_explicit(atom(s->desc), flags_after & ~HH_RING_F_READY, memory_order_release);
    atomic_store_explicit(v->ridx, s->index + 1u, memory_order_release);
}

/* Slots published and not yet released. */
uint32_t hh_ring_view_used(const hh_ring_view_t *v)
{
    return atomic_load_explicit(v->widx, memory_order_acquire) -
           atomic_load_explicit(v->ridx, memory_order_acquire);
}

/* Writes every RX descriptor field except the flags word. */
void hh_ring_rx_desc_write(uint8_t desc[HH_RING_DESC_SIZE], const hh_ring_rx_desc_t *d)
{
    memset(desc + 4, 0, HH_RING_DESC_SIZE - 4u);
    put16(desc + 4, d->length);
    put16(desc + 6, d->slot_in_frame);
    put32(desc + 8, d->superframe);
    put32(desc + 12, (uint32_t)d->arrival_offset_ns);
    put16(desc + 16, (uint16_t)d->rssi);
    put16(desc + 18, (uint16_t)d->snr);
    put16(desc + 20, (uint16_t)d->rx_gain);
    put16(desc + 22, d->metric_flags);
    put32(desc + 24, d->rx_seq);
}

/* Reads an RX descriptor, flags word included. */
void hh_ring_rx_desc_read(const uint8_t desc[HH_RING_DESC_SIZE], hh_ring_rx_desc_t *d)
{
    d->flags = get32(desc);
    d->length = get16(desc + 4);
    d->slot_in_frame = get16(desc + 6);
    d->superframe = get32(desc + 8);
    d->arrival_offset_ns = (int32_t)get32(desc + 12);
    d->rssi = (int16_t)get16(desc + 16);
    d->snr = (int16_t)get16(desc + 18);
    d->rx_gain = (int16_t)get16(desc + 20);
    d->metric_flags = get16(desc + 22);
    d->rx_seq = get32(desc + 24);
}

/* Writes every TX descriptor field except the flags word. */
void hh_ring_tx_desc_write(uint8_t desc[HH_RING_DESC_SIZE], const hh_ring_tx_desc_t *d)
{
    memset(desc + 4, 0, HH_RING_DESC_SIZE - 4u);
    put16(desc + 4, d->length);
    desc[6] = d->traffic_class;
    put32(desc + 8, d->target_superframe);
    put16(desc + 12, d->target_frame);
    put16(desc + 14, d->target_slot);
}

/* Reads a TX descriptor, flags word included. */
void hh_ring_tx_desc_read(const uint8_t desc[HH_RING_DESC_SIZE], hh_ring_tx_desc_t *d)
{
    d->flags = get32(desc);
    d->length = get16(desc + 4);
    d->traffic_class = desc[6];
    d->target_superframe = get32(desc + 8);
    d->target_frame = get16(desc + 12);
    d->target_slot = get16(desc + 14);
}
