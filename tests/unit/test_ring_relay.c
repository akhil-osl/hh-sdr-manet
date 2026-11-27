/*
 * Ring relay: a frame written into the TX ring of one region reaches the RX ring
 * of the other with its length, slot and superframe, and the CRC OK flag. Full
 * and oversize cases drop the frame, count it and free the TX place.
 * Links hhsdr_ring and the relay only. No sleeps.
 */
#include "hhsdr/ring/ring.h"
#include "hh_test.h"
#include "ring_relay.h"
#include <string.h>

#define SLOTS 4u
#define SSIZE 64u

static _Alignas(32) uint8_t mem_a[1024];
static _Alignas(32) uint8_t mem_b[1024];

/* Makes two attached regions with the same geometry. */
static void two_regions(hh_ring_region_t *a, hh_ring_region_t *b)
{
    HH_ASSERT_OK(hh_ring_region_init(mem_a, sizeof mem_a, SLOTS, SLOTS, SSIZE));
    HH_ASSERT_OK(hh_ring_region_attach(a, mem_a, sizeof mem_a));
    HH_ASSERT_OK(hh_ring_region_init(mem_b, sizeof mem_b, SLOTS, SLOTS, SSIZE));
    HH_ASSERT_OK(hh_ring_region_attach(b, mem_b, sizeof mem_b));
}

/* Writes one frame of len bytes into the TX ring, every byte set to fill. */
static void put_tx(hh_ring_region_t *r, uint16_t len, uint8_t fill, uint16_t slot, uint32_t superframe)
{
    hh_ring_slot_t s;
    hh_ring_tx_desc_t td;

    HH_ASSERT_OK(hh_ring_produce_begin(&r->tx, &s));
    memset(s.frame, fill, len);
    memset(&td, 0, sizeof td);
    td.length = len;
    td.traffic_class = HH_RING_CLASS_DATA;
    td.target_superframe = superframe;
    td.target_slot = slot;
    hh_ring_tx_desc_write(s.desc, &td);
    hh_ring_produce_commit(&r->tx, &s, 0);
}

/* A frame is copied with its fields, marked CRC OK, and the TX place is freed. */
static void test_frame_arrives(void)
{
    hh_ring_region_t a, b;
    relay_dir_t st = {0};
    hh_ring_slot_t s;
    hh_ring_rx_desc_t rd;

    two_regions(&a, &b);
    put_tx(&a, 40, 0xA5, 7, 3);
    HH_ASSERT_EQ_INT(relay_move(&a, &b, &st, 8), 1);
    HH_ASSERT_EQ_INT((int)st.copied, 1);
    HH_ASSERT_EQ_INT((int)hh_ring_view_used(&a.tx), 0);
    HH_ASSERT_OK(hh_ring_consume_peek(&b.rx, &s));
    hh_ring_rx_desc_read(s.desc, &rd);
    HH_ASSERT_EQ_INT(rd.length, 40);
    HH_ASSERT_EQ_INT(rd.slot_in_frame, 7);
    HH_ASSERT_EQ_INT((int)rd.superframe, 3);
    HH_ASSERT_EQ_INT((int)rd.rx_seq, 1);
    HH_ASSERT((rd.flags & HH_RING_RX_CRC_OK) != 0);
    HH_ASSERT(s.frame[0] == 0xA5 && s.frame[39] == 0xA5);
}

/* Frames keep their order and the sequence number rises. */
static void test_order_and_sequence(void)
{
    hh_ring_region_t a, b;
    relay_dir_t st = {0};
    hh_ring_slot_t s;
    hh_ring_rx_desc_t rd;

    two_regions(&a, &b);
    put_tx(&a, 10, 1, 0, 0);
    put_tx(&a, 11, 2, 1, 0);
    put_tx(&a, 12, 3, 2, 0);
    HH_ASSERT_EQ_INT(relay_move(&a, &b, &st, 8), 3);
    for (int i = 0; i < 3; i++) {
        HH_ASSERT_OK(hh_ring_consume_peek(&b.rx, &s));
        hh_ring_rx_desc_read(s.desc, &rd);
        HH_ASSERT_EQ_INT(rd.length, 10 + i);
        HH_ASSERT_EQ_INT((int)rd.rx_seq, i + 1);
        hh_ring_consume_release(&b.rx, &s, 0);
    }
}

/* The limit per call is respected. */
static void test_move_limit(void)
{
    hh_ring_region_t a, b;
    relay_dir_t st = {0};

    two_regions(&a, &b);
    put_tx(&a, 8, 1, 0, 0);
    put_tx(&a, 8, 2, 1, 0);
    HH_ASSERT_EQ_INT(relay_move(&a, &b, &st, 1), 1);
    HH_ASSERT_EQ_INT((int)hh_ring_view_used(&a.tx), 1);
    HH_ASSERT_EQ_INT(relay_move(&a, &b, &st, 1), 1);
    HH_ASSERT_EQ_INT((int)st.copied, 2);
}

/* A full RX ring drops the frame, counts it and still frees the TX place. */
static void test_rx_full_drops(void)
{
    hh_ring_region_t a, b;
    relay_dir_t st = {0};

    two_regions(&a, &b);
    for (unsigned i = 0; i < SLOTS; i++) put_tx(&a, 8, (uint8_t)i, 0, 0);
    HH_ASSERT_EQ_INT(relay_move(&a, &b, &st, 8), (int)SLOTS);
    HH_ASSERT_EQ_INT((int)st.copied, (int)SLOTS);
    put_tx(&a, 8, 9, 0, 0);
    HH_ASSERT_EQ_INT(relay_move(&a, &b, &st, 8), 1);
    HH_ASSERT_EQ_INT((int)st.rx_full, 1);
    HH_ASSERT_EQ_INT((int)st.copied, (int)SLOTS);
    HH_ASSERT_EQ_INT((int)hh_ring_view_used(&a.tx), 0);
}

/* A frame longer than the RX place is dropped and counted. */
static void test_oversize_dropped(void)
{
    hh_ring_region_t a, b;
    relay_dir_t st = {0};

    two_regions(&a, &b);
    HH_ASSERT_OK(hh_ring_region_init(mem_b, sizeof mem_b, 2, 2, 32));   /* smaller places on the receiving side */
    HH_ASSERT_OK(hh_ring_region_attach(&b, mem_b, sizeof mem_b));
    put_tx(&a, 48, 1, 0, 0);
    HH_ASSERT_EQ_INT(relay_move(&a, &b, &st, 8), 1);
    HH_ASSERT_EQ_INT((int)st.too_big, 1);
    HH_ASSERT_EQ_INT((int)st.copied, 0);
    HH_ASSERT_EQ_INT((int)hh_ring_view_used(&b.rx), 0);
}

/* An empty TX ring moves nothing. */
static void test_empty_moves_nothing(void)
{
    hh_ring_region_t a, b;
    relay_dir_t st = {0};

    two_regions(&a, &b);
    HH_ASSERT_EQ_INT(relay_move(&a, &b, &st, 8), 0);
    HH_ASSERT_EQ_INT((int)st.copied, 0);
}

HH_TEST_MAIN_BEGIN("ring_relay")
    HH_RUN(test_frame_arrives);
    HH_RUN(test_order_and_sequence);
    HH_RUN(test_move_limit);
    HH_RUN(test_rx_full_drops);
    HH_RUN(test_oversize_dropped);
    HH_RUN(test_empty_moves_nothing);
HH_TEST_MAIN_END()
