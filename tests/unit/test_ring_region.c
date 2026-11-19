/*
 * Shared frame rings: region layout, descriptor bytes, producer and consumer
 * order, full and empty rings, index wrap. Links hhsdr_ring only.
 *
 * The golden descriptor bytes are the receive and transmit descriptors of the
 * 280-byte reference frames.
 */
#include "hhsdr/ring/ring.h"
#include "hh_test.h"
#include <string.h>

#define SLOTS 4u
#define SSIZE 64u

static _Alignas(32) uint8_t mem[1024];

static const uint8_t golden_rx[32] = {
    0x03, 0x00, 0x00, 0x00, 0x18, 0x01, 0x05, 0x00, 0x34, 0x12, 0x00, 0x00, 0x78, 0x00, 0x00, 0x00,
    0xC0, 0xFE, 0x50, 0x00, 0x78, 0x00, 0x0F, 0x00, 0xE8, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
static const uint8_t golden_tx[32] = {
    0x01, 0x00, 0x00, 0x00, 0x18, 0x01, 0x04, 0x00, 0x35, 0x12, 0x00, 0x00, 0x07, 0x00, 0x05, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };

/* Initialises the test region and attaches to it. */
static hh_status_t fresh(hh_ring_region_t *r)
{
    hh_status_t s = hh_ring_region_init(mem, sizeof mem, SLOTS, SLOTS, SSIZE);
    return s == HH_OK ? hh_ring_region_attach(r, mem, sizeof mem) : s;
}

/* Publishes one frame whose bytes are all fill, with the given length in the RX descriptor. */
static hh_status_t push_rx(hh_ring_region_t *r, uint8_t fill, uint16_t len)
{
    hh_ring_slot_t s;
    hh_ring_rx_desc_t d = { .length = len };
    hh_status_t st = hh_ring_produce_begin(&r->rx, &s);

    if (st != HH_OK) return st;
    memset(s.frame, fill, len);
    hh_ring_rx_desc_write(s.desc, &d);
    hh_ring_produce_commit(&r->rx, &s, HH_RING_RX_CRC_OK);
    return HH_OK;
}

/* Region sizes follow the layout, and a bad geometry gives 0. */
static void test_region_size(void)
{
    HH_ASSERT_EQ_INT(hh_ring_region_size(128, 64, 2048), 192 + 128 * 32 + 128 * 2048 + 64 * 32 + 64 * 2048);
    HH_ASSERT_EQ_INT(hh_ring_region_size(SLOTS, SLOTS, SSIZE), 960);
    HH_ASSERT_EQ_INT(hh_ring_region_size(3, 4, 64), 0);
    HH_ASSERT_EQ_INT(hh_ring_region_size(4, 0, 64), 0);
    HH_ASSERT_EQ_INT(hh_ring_region_size(4, 4, 0), 0);
    HH_ASSERT_EQ_INT(hh_ring_region_size(4, 4, 48), 0);
    HH_ASSERT_EQ_INT(hh_ring_region_size(0x10000000u, 4, 2048), 0);
}

/* The header bytes follow the contract and the rings bind to the offsets in it. */
static void test_init_header(void)
{
    hh_ring_region_t r;
    HH_ASSERT_OK(fresh(&r));
    HH_ASSERT_EQ_INT(mem[4], 1);
    HH_ASSERT_EQ_INT(mem[5], 0);
    HH_ASSERT_EQ_INT(mem[6], 32);
    HH_ASSERT_EQ_INT(mem[8], SLOTS);
    HH_ASSERT_EQ_INT(mem[12], SLOTS);
    HH_ASSERT_EQ_INT(mem[16], SSIZE);
    HH_ASSERT_EQ_INT(mem[20], 192);
    HH_ASSERT_EQ_INT(mem[24] | (mem[25] << 8), 192 + SLOTS * 32);
    HH_ASSERT((void *)r.rx.desc == (void *)(mem + 192));
    HH_ASSERT((uintptr_t)r.rx.widx == (uintptr_t)(mem + HH_RING_OFF_RX_WRITE));
    HH_ASSERT((uintptr_t)r.tx.ridx == (uintptr_t)(mem + HH_RING_OFF_TX_READ));
    HH_ASSERT_EQ_INT(r.rx.count, SLOTS);
    HH_ASSERT_EQ_INT(r.tx.slot_size, SSIZE);
    HH_ASSERT_EQ_INT(hh_ring_view_used(&r.rx), 0);
}

/* Init and attach refuse bad input. */
static void test_bad_input(void)
{
    hh_ring_region_t r;

    HH_ASSERT_ERR(hh_ring_region_init(mem, sizeof mem, 3, 4, SSIZE), HH_ERR_INVAL);
    HH_ASSERT_ERR(hh_ring_region_init(mem, 100, SLOTS, SLOTS, SSIZE), HH_ERR_INVAL);
    HH_ASSERT_ERR(hh_ring_region_init(mem + 1, sizeof mem - 1, SLOTS, SLOTS, SSIZE), HH_ERR_INVAL);
    HH_ASSERT_ERR(hh_ring_region_init(NULL, sizeof mem, SLOTS, SLOTS, SSIZE), HH_ERR_INVAL);

    HH_ASSERT_OK(fresh(&r));
    HH_ASSERT_ERR(hh_ring_region_attach(&r, mem, 959), HH_ERR_INVAL);
    HH_ASSERT_ERR(hh_ring_region_attach(&r, mem, 100), HH_ERR_INVAL);
    HH_ASSERT_ERR(hh_ring_region_attach(&r, mem + 1, sizeof mem - 1), HH_ERR_INVAL);
    mem[0] ^= 0xFF;
    HH_ASSERT_ERR(hh_ring_region_attach(&r, mem, sizeof mem), HH_ERR_INVAL);
    mem[0] ^= 0xFF;
    mem[4] = 2;
    HH_ASSERT_ERR(hh_ring_region_attach(&r, mem, sizeof mem), HH_ERR_INVAL);
    mem[4] = 1;
    mem[20] = 224;
    HH_ASSERT_ERR(hh_ring_region_attach(&r, mem, sizeof mem), HH_ERR_INVAL);
    mem[20] = 192;
    HH_ASSERT_OK(hh_ring_region_attach(&r, mem, sizeof mem));
}

/* The descriptors of the 280-byte reference frames are reproduced byte for byte. */
static void test_golden_descriptors(void)
{
    hh_ring_region_t r;
    hh_ring_slot_t s;
    hh_ring_rx_desc_t rd = { .length = 280, .slot_in_frame = 5, .superframe = 0x1234,
                             .arrival_offset_ns = 120, .rssi = -320, .snr = 80, .rx_gain = 120,
                             .metric_flags = HH_RING_M_RSSI_VALID | HH_RING_M_SNR_VALID |
                                             HH_RING_M_GAIN_VALID | HH_RING_M_ARRIVAL_VALID,
                             .rx_seq = 1000 };
    hh_ring_tx_desc_t td = { .length = 280, .traffic_class = HH_RING_CLASS_DATA,
                             .target_superframe = 0x1235, .target_frame = 7, .target_slot = 5 };
    hh_ring_rx_desc_t rb;
    hh_ring_tx_desc_t tb;

    HH_ASSERT_OK(fresh(&r));
    HH_ASSERT_OK(hh_ring_produce_begin(&r.rx, &s));
    hh_ring_rx_desc_write(s.desc, &rd);
    hh_ring_produce_commit(&r.rx, &s, HH_RING_RX_CRC_OK);
    HH_ASSERT(memcmp(s.desc, golden_rx, 32) == 0);

    HH_ASSERT_OK(hh_ring_consume_peek(&r.rx, &s));
    hh_ring_rx_desc_read(s.desc, &rb);
    HH_ASSERT_EQ_INT(rb.flags, HH_RING_F_READY | HH_RING_RX_CRC_OK);
    HH_ASSERT_EQ_INT(rb.length, 280);
    HH_ASSERT_EQ_INT(rb.slot_in_frame, 5);
    HH_ASSERT_EQ_INT(rb.superframe, 0x1234);
    HH_ASSERT_EQ_INT(rb.arrival_offset_ns, 120);
    HH_ASSERT_EQ_INT(rb.rssi, -320);
    HH_ASSERT_EQ_INT(rb.snr, 80);
    HH_ASSERT_EQ_INT(rb.rx_gain, 120);
    HH_ASSERT_EQ_INT(rb.metric_flags, 0x000F);
    HH_ASSERT_EQ_INT(rb.rx_seq, 1000);

    HH_ASSERT_OK(hh_ring_produce_begin(&r.tx, &s));
    hh_ring_tx_desc_write(s.desc, &td);
    hh_ring_produce_commit(&r.tx, &s, 0);
    HH_ASSERT(memcmp(s.desc, golden_tx, 32) == 0);

    HH_ASSERT_OK(hh_ring_consume_peek(&r.tx, &s));
    hh_ring_tx_desc_read(s.desc, &tb);
    HH_ASSERT_EQ_INT(tb.length, 280);
    HH_ASSERT_EQ_INT(tb.traffic_class, HH_RING_CLASS_DATA);
    HH_ASSERT_EQ_INT(tb.target_superframe, 0x1235);
    HH_ASSERT_EQ_INT(tb.target_frame, 7);
    HH_ASSERT_EQ_INT(tb.target_slot, 5);
}

/* Frames come out in the order they went in, across several wraps. */
static void test_fifo_across_wraps(void)
{
    hh_ring_region_t r;
    hh_ring_slot_t s;
    unsigned i;

    HH_ASSERT_OK(fresh(&r));
    HH_ASSERT_ERR(hh_ring_consume_peek(&r.rx, &s), HH_ERR_AGAIN);
    for (i = 0; i < 11; i++) {
        HH_ASSERT_OK(push_rx(&r, (uint8_t)(0x10 + i), 20));
        HH_ASSERT_OK(push_rx(&r, (uint8_t)(0x80 + i), 30));
        for (unsigned k = 0; k < 2; k++) {
            hh_ring_rx_desc_t d;
            uint8_t want = (uint8_t)(k == 0 ? 0x10 + i : 0x80 + i);
            HH_ASSERT_OK(hh_ring_consume_peek(&r.rx, &s));
            hh_ring_rx_desc_read(s.desc, &d);
            HH_ASSERT_EQ_INT(d.length, k == 0 ? 20 : 30);
            HH_ASSERT_EQ_INT(s.frame[0], want);
            HH_ASSERT_EQ_INT(s.frame[d.length - 1u], want);
            hh_ring_consume_release(&r.rx, &s, 0);
        }
    }
    HH_ASSERT_EQ_INT(hh_ring_view_used(&r.rx), 0);
}

/* A full ring refuses the producer and keeps every queued frame. */
static void test_full_keeps_frames(void)
{
    hh_ring_region_t r;
    hh_ring_slot_t s;
    unsigned i;

    HH_ASSERT_OK(fresh(&r));
    for (i = 0; i < SLOTS; i++) HH_ASSERT_OK(push_rx(&r, (uint8_t)(i + 1), 8));
    HH_ASSERT_EQ_INT(hh_ring_view_used(&r.rx), SLOTS);
    HH_ASSERT_ERR(push_rx(&r, 0xEE, 8), HH_ERR_NOMEM);
    HH_ASSERT_ERR(hh_ring_produce_begin(&r.rx, &s), HH_ERR_NOMEM);
    HH_ASSERT_OK(hh_ring_consume_peek(&r.rx, &s));
    HH_ASSERT_EQ_INT(s.frame[0], 1);
    hh_ring_consume_release(&r.rx, &s, 0);
    HH_ASSERT_OK(push_rx(&r, 0x55, 8));
    for (i = 0; i < SLOTS; i++) {
        uint8_t want = (uint8_t)(i < SLOTS - 1 ? i + 2 : 0x55);
        HH_ASSERT_OK(hh_ring_consume_peek(&r.rx, &s));
        HH_ASSERT_EQ_INT(s.frame[0], want);
        hh_ring_consume_release(&r.rx, &s, 0);
    }
}

/* A claimed slot is invisible until it is committed, and peek does not free it. */
static void test_commit_publishes(void)
{
    hh_ring_region_t r;
    hh_ring_slot_t a, b;

    HH_ASSERT_OK(fresh(&r));
    HH_ASSERT_OK(hh_ring_produce_begin(&r.rx, &a));
    memset(a.frame, 0x77, 8);
    HH_ASSERT_ERR(hh_ring_consume_peek(&r.rx, &b), HH_ERR_AGAIN);
    hh_ring_produce_commit(&r.rx, &a, 0);
    HH_ASSERT_OK(hh_ring_consume_peek(&r.rx, &b));
    HH_ASSERT_OK(hh_ring_consume_peek(&r.rx, &b));
    HH_ASSERT_EQ_INT(hh_ring_view_used(&r.rx), 1);
    HH_ASSERT(b.frame == a.frame);
    hh_ring_consume_release(&r.rx, &b, 0);
    HH_ASSERT_EQ_INT(hh_ring_view_used(&r.rx), 0);
}

/* The index alone does not publish a slot: the ready flag must be set too. */
static void test_ready_flag_required(void)
{
    hh_ring_region_t r;
    hh_ring_slot_t s;

    HH_ASSERT_OK(fresh(&r));
    atomic_store(r.rx.widx, 1u);
    HH_ASSERT_ERR(hh_ring_consume_peek(&r.rx, &s), HH_ERR_AGAIN);
}

/* A released TX slot carries the TAKEN flag and no ready flag. */
static void test_tx_taken(void)
{
    hh_ring_region_t r;
    hh_ring_slot_t s;
    hh_ring_tx_desc_t d;

    HH_ASSERT_OK(fresh(&r));
    HH_ASSERT_OK(hh_ring_produce_begin(&r.tx, &s));
    hh_ring_produce_commit(&r.tx, &s, 0);
    HH_ASSERT_OK(hh_ring_consume_peek(&r.tx, &s));
    hh_ring_consume_release(&r.tx, &s, HH_RING_TX_TAKEN);
    hh_ring_tx_desc_read(s.desc, &d);
    HH_ASSERT_EQ_INT(d.flags, HH_RING_TX_TAKEN);
}

/* Two attachments to the same memory see the same rings, as two processes would. */
static void test_two_attachments(void)
{
    hh_ring_region_t a, b;
    hh_ring_slot_t s;

    HH_ASSERT_OK(fresh(&a));
    HH_ASSERT_OK(hh_ring_region_attach(&b, mem, sizeof mem));
    HH_ASSERT_OK(push_rx(&a, 0x3C, 12));
    HH_ASSERT_EQ_INT(hh_ring_view_used(&b.rx), 1);
    HH_ASSERT_OK(hh_ring_consume_peek(&b.rx, &s));
    HH_ASSERT_EQ_INT(s.frame[11], 0x3C);
    hh_ring_consume_release(&b.rx, &s, 0);
    HH_ASSERT_EQ_INT(hh_ring_view_used(&a.rx), 0);
}

/* The free-running indices wrap past 2^32 without losing order. */
static void test_index_wrap(void)
{
    hh_ring_region_t r;
    hh_ring_slot_t s;
    unsigned i;

    HH_ASSERT_OK(fresh(&r));
    atomic_store(r.rx.widx, 0xFFFFFFFEu);
    atomic_store(r.rx.ridx, 0xFFFFFFFEu);
    for (i = 0; i < 6; i++) {
        HH_ASSERT_OK(push_rx(&r, (uint8_t)(0x20 + i), 4));
        HH_ASSERT_OK(hh_ring_consume_peek(&r.rx, &s));
        hh_ring_consume_release(&r.rx, &s, 0);
    }
    HH_ASSERT_EQ_INT(atomic_load(r.rx.widx), 4);
    for (i = 0; i < SLOTS; i++) HH_ASSERT_OK(push_rx(&r, (uint8_t)(0x40 + i), 4));
    HH_ASSERT_ERR(push_rx(&r, 0, 4), HH_ERR_NOMEM);
    for (i = 0; i < SLOTS; i++) {
        HH_ASSERT_OK(hh_ring_consume_peek(&r.rx, &s));
        HH_ASSERT_EQ_INT(s.frame[0], 0x40 + i);
        hh_ring_consume_release(&r.rx, &s, 0);
    }
}

HH_TEST_MAIN_BEGIN("ring_region")
    HH_RUN(test_region_size);
    HH_RUN(test_init_header);
    HH_RUN(test_bad_input);
    HH_RUN(test_golden_descriptors);
    HH_RUN(test_fifo_across_wraps);
    HH_RUN(test_full_keeps_frames);
    HH_RUN(test_commit_publishes);
    HH_RUN(test_ready_flag_required);
    HH_RUN(test_tx_taken);
    HH_RUN(test_two_attachments);
    HH_RUN(test_index_wrap);
HH_TEST_MAIN_END()
