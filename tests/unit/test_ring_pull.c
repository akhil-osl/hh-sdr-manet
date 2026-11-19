/*
 * TX pull logic and the fake PL, over the shared rings. Checks the servant
 * rules (match, stale, future, caps, wrap), the PL behaviour for a missing or
 * late reply, and that a real MAC PDU crosses both directions unchanged.
 * Links hhsdr_ring_pull, hhsdr_mac and the fake PL.
 */
#include "fake_pl.h"
#include "hhsdr/mac/pdu.h"
#include "hh_test.h"
#include <string.h>

#define SLOTS 8u
#define SSIZE 2048u

static _Alignas(32) uint8_t mem[34000];
static fake_pl_t pl;
static hh_ring_region_t reg;
static hh_pull_item_t items[8];
static uint8_t rbuf[4096];
static hh_pull_stats_t st;

/* Vector A of the PDU codec test: node 1 sends HELLO to node 2. */
static const uint8_t vec_a[] = {
    0x41, 0xA8, 0x21, 0x10, 0x00, 0xFF, 0xFF, 0x01, 0x00, 0x10, 0x00, 0x00, 0x00, 0x00, 0x03,
    0x20, 0x00, 0x00, 0xF0, 0x02, 0x00, 0x05, 0xA0, 0x07, 0x48, 0x45, 0x4C, 0x4C, 0x4F,
    0x5F, 0x1A, 0xD6, 0x46 };

/* Fresh region, fake PL and counters. */
static hh_status_t fresh(void)
{
    hh_status_t s = hh_ring_region_init(mem, sizeof mem, SLOTS, SLOTS, SSIZE);

    memset(&st, 0, sizeof st);
    fake_pl_init(&pl);
    return s == HH_OK ? hh_ring_region_attach(&reg, mem, sizeof mem) : s;
}

/* The MAC process writes a TX frame of len bytes, all fill, for a target. */
static hh_status_t write_tx(uint8_t fill, uint16_t len, uint8_t cls, uint32_t sf, uint16_t fr,
                            uint16_t slot, uint8_t **desc_out)
{
    hh_ring_slot_t s;
    hh_ring_tx_desc_t d = { .length = len, .traffic_class = cls, .target_superframe = sf,
                            .target_frame = fr, .target_slot = slot };
    hh_status_t r = hh_ring_produce_begin(&reg.tx, &s);

    if (r != HH_OK) return r;
    memset(s.frame, fill, len);
    hh_ring_tx_desc_write(s.desc, &d);
    hh_ring_produce_commit(&reg.tx, &s, 0);
    if (desc_out) *desc_out = s.desc;
    return HH_OK;
}

/* A pull with room for 8 items. */
static hh_status_t pull(uint32_t sf, uint16_t fr, uint32_t *n)
{
    return hh_pull_frame(&reg.tx, sf, fr, items, 8, rbuf, sizeof rbuf, n, &st);
}

/* A matching frame comes back unchanged, is marked TAKEN and frees its slot. */
static void test_exact_match(void)
{
    uint8_t *desc;
    uint32_t n;
    hh_ring_tx_desc_t d;

    HH_ASSERT_OK(fresh());
    HH_ASSERT_OK(write_tx(0xA5, 40, HH_RING_CLASS_DATA, 0x1235, 7, 5, &desc));
    HH_ASSERT_OK(pull(0x1235, 7, &n));
    HH_ASSERT_EQ_INT(n, 1);
    HH_ASSERT_EQ_INT(items[0].slot, 5);
    HH_ASSERT_EQ_INT(items[0].traffic_class, HH_RING_CLASS_DATA);
    HH_ASSERT_EQ_INT(items[0].length, 40);
    HH_ASSERT_EQ_INT(items[0].frame[0], 0xA5);
    HH_ASSERT_EQ_INT(items[0].frame[39], 0xA5);
    HH_ASSERT_EQ_INT(hh_ring_view_used(&reg.tx), 0);
    hh_ring_tx_desc_read(desc, &d);
    HH_ASSERT_EQ_INT(d.flags, HH_RING_TX_TAKEN);
    HH_ASSERT_EQ_INT(st.frames_returned, 1);
}

/* Several frames for one TDMA frame come back in the order written. */
static void test_several_in_order(void)
{
    uint32_t n;

    HH_ASSERT_OK(fresh());
    HH_ASSERT_OK(write_tx(1, 10, HH_RING_CLASS_VOICE, 0x1235, 7, 2, NULL));
    HH_ASSERT_OK(write_tx(2, 20, HH_RING_CLASS_DATA, 0x1235, 7, 5, NULL));
    HH_ASSERT_OK(pull(0x1235, 7, &n));
    HH_ASSERT_EQ_INT(n, 2);
    HH_ASSERT_EQ_INT(items[0].slot, 2);
    HH_ASSERT_EQ_INT(items[0].frame[0], 1);
    HH_ASSERT_EQ_INT(items[1].slot, 5);
    HH_ASSERT_EQ_INT(items[1].frame[0], 2);
    HH_ASSERT_EQ_INT(items[1].frame[19], 2);
}

/* Frames for an earlier target are dropped and counted, the match still comes back. */
static void test_stale_dropped(void)
{
    uint32_t n;

    HH_ASSERT_OK(fresh());
    HH_ASSERT_OK(write_tx(0x11, 8, HH_RING_CLASS_DATA, 0x1234, 9, 1, NULL));
    HH_ASSERT_OK(write_tx(0x22, 8, HH_RING_CLASS_DATA, 0x1235, 3, 1, NULL));
    HH_ASSERT_OK(write_tx(0x33, 8, HH_RING_CLASS_DATA, 0x1235, 7, 5, NULL));
    HH_ASSERT_OK(pull(0x1235, 7, &n));
    HH_ASSERT_EQ_INT(n, 1);
    HH_ASSERT_EQ_INT(items[0].frame[0], 0x33);
    HH_ASSERT_EQ_INT(st.stale_dropped, 2);
    HH_ASSERT_EQ_INT(hh_ring_view_used(&reg.tx), 0);
}

/* A frame for a later target stays in the ring until its own pull. */
static void test_future_stays(void)
{
    uint32_t n;

    HH_ASSERT_OK(fresh());
    HH_ASSERT_OK(write_tx(0x44, 8, HH_RING_CLASS_DATA, 0x1235, 8, 5, NULL));
    HH_ASSERT_OK(pull(0x1235, 7, &n));
    HH_ASSERT_EQ_INT(n, 0);
    HH_ASSERT_EQ_INT(hh_ring_view_used(&reg.tx), 1);
    HH_ASSERT_EQ_INT(st.stale_dropped, 0);
    HH_ASSERT_OK(pull(0x1235, 8, &n));
    HH_ASSERT_EQ_INT(n, 1);
    HH_ASSERT_EQ_INT(items[0].frame[0], 0x44);
}

/* An empty ring gives an empty reply. */
static void test_empty(void)
{
    uint32_t n = 99;

    HH_ASSERT_OK(fresh());
    HH_ASSERT_OK(pull(5, 5, &n));
    HH_ASSERT_EQ_INT(n, 0);
    HH_ASSERT_EQ_INT(st.pulls, 1);
}

/* Order of targets survives a wrap of the 32-bit superframe count. */
static void test_superframe_wrap(void)
{
    uint32_t n;

    HH_ASSERT_OK(fresh());
    HH_ASSERT_OK(write_tx(0x55, 8, HH_RING_CLASS_DATA, 0xFFFFFFFFu, 3, 1, NULL));
    HH_ASSERT_OK(write_tx(0x66, 8, HH_RING_CLASS_DATA, 1, 0, 2, NULL));
    HH_ASSERT_OK(pull(1, 0, &n));
    HH_ASSERT_EQ_INT(n, 1);
    HH_ASSERT_EQ_INT(items[0].frame[0], 0x66);
    HH_ASSERT_EQ_INT(st.stale_dropped, 1);
}

/* The item cap and the buffer cap leave the rest in the ring and count it. */
static void test_caps(void)
{
    uint32_t n;

    HH_ASSERT_OK(fresh());
    HH_ASSERT_OK(write_tx(1, 40, HH_RING_CLASS_DATA, 3, 1, 1, NULL));
    HH_ASSERT_OK(write_tx(2, 40, HH_RING_CLASS_DATA, 3, 1, 2, NULL));
    HH_ASSERT_OK(hh_pull_frame(&reg.tx, 3, 1, items, 1, rbuf, sizeof rbuf, &n, &st));
    HH_ASSERT_EQ_INT(n, 1);
    HH_ASSERT_EQ_INT(st.truncated, 1);
    HH_ASSERT_EQ_INT(hh_ring_view_used(&reg.tx), 1);
    HH_ASSERT_OK(hh_pull_frame(&reg.tx, 3, 1, items, 8, rbuf, 30, &n, &st));
    HH_ASSERT_EQ_INT(n, 0);
    HH_ASSERT_EQ_INT(st.truncated, 2);
    HH_ASSERT_EQ_INT(hh_ring_view_used(&reg.tx), 1);
    HH_ASSERT_OK(pull(3, 1, &n));
    HH_ASSERT_EQ_INT(n, 1);
    HH_ASSERT_EQ_INT(items[0].frame[0], 2);
}

/* A descriptor that claims more bytes than the slot holds is dropped and counted. */
static void test_bad_descriptor(void)
{
    hh_ring_slot_t s;
    hh_ring_tx_desc_t d = { .length = 5000, .target_superframe = 3, .target_frame = 1 };
    uint32_t n;

    HH_ASSERT_OK(fresh());
    HH_ASSERT_OK(hh_ring_produce_begin(&reg.tx, &s));
    hh_ring_tx_desc_write(s.desc, &d);
    hh_ring_produce_commit(&reg.tx, &s, 0);
    HH_ASSERT_OK(pull(3, 1, &n));
    HH_ASSERT_EQ_INT(n, 0);
    HH_ASSERT_EQ_INT(st.bad_descriptors, 1);
    HH_ASSERT_EQ_INT(hh_ring_view_used(&reg.tx), 0);
}

/* Null arguments are refused. */
static void test_null_args(void)
{
    uint32_t n;

    HH_ASSERT_OK(fresh());
    HH_ASSERT_ERR(hh_pull_frame(NULL, 1, 1, items, 8, rbuf, sizeof rbuf, &n, &st), HH_ERR_INVAL);
    HH_ASSERT_ERR(hh_pull_frame(&reg.tx, 1, 1, NULL, 8, rbuf, sizeof rbuf, &n, &st), HH_ERR_INVAL);
    HH_ASSERT_ERR(hh_pull_frame(&reg.tx, 1, 1, items, 8, rbuf, sizeof rbuf, NULL, &st), HH_ERR_INVAL);
}

/* The fake PL writes a received frame and its descriptor, and counts the frames. */
static void test_fake_receive(void)
{
    hh_ring_rx_desc_t meta = { .slot_in_frame = 5, .superframe = 0x1234, .arrival_offset_ns = 120,
                               .rssi = -320, .snr = 80, .rx_gain = 120, .metric_flags = 0x000F };
    hh_ring_slot_t s;
    hh_ring_rx_desc_t d;

    HH_ASSERT_OK(fresh());
    HH_ASSERT_OK(fake_pl_receive(&pl, &reg, vec_a, sizeof vec_a, 1, &meta));
    HH_ASSERT_OK(fake_pl_receive(&pl, &reg, vec_a, sizeof vec_a, 1, &meta));
    HH_ASSERT_EQ_INT(pl.rx_frames, 2);
    HH_ASSERT_OK(hh_ring_consume_peek(&reg.rx, &s));
    hh_ring_rx_desc_read(s.desc, &d);
    HH_ASSERT_EQ_INT(d.flags, HH_RING_F_READY | HH_RING_RX_CRC_OK);
    HH_ASSERT_EQ_INT(d.length, sizeof vec_a);
    HH_ASSERT_EQ_INT(d.slot_in_frame, 5);
    HH_ASSERT_EQ_INT(d.rssi, -320);
    HH_ASSERT_EQ_INT(d.rx_seq, 1);
    hh_ring_consume_release(&reg.rx, &s, 0);
    HH_ASSERT_OK(hh_ring_consume_peek(&reg.rx, &s));
    hh_ring_rx_desc_read(s.desc, &d);
    HH_ASSERT_EQ_INT(d.rx_seq, 2);
}

/* A bad CRC is counted and not delivered, and a full ring drops and counts. */
static void test_fake_receive_drops(void)
{
    hh_ring_rx_desc_t meta = { 0 };
    unsigned i;

    HH_ASSERT_OK(fresh());
    HH_ASSERT_OK(fake_pl_receive(&pl, &reg, vec_a, sizeof vec_a, 0, &meta));
    HH_ASSERT_EQ_INT(pl.rx_crc_bad, 1);
    HH_ASSERT_EQ_INT(hh_ring_view_used(&reg.rx), 0);
    for (i = 0; i < SLOTS; i++) HH_ASSERT_OK(fake_pl_receive(&pl, &reg, vec_a, sizeof vec_a, 1, &meta));
    HH_ASSERT_ERR(fake_pl_receive(&pl, &reg, vec_a, sizeof vec_a, 1, &meta), HH_ERR_NOMEM);
    HH_ASSERT_EQ_INT(pl.rx_ring_full, 1);
    HH_ASSERT_EQ_INT(hh_ring_view_used(&reg.rx), SLOTS);
}

/* An own slot with no frame in the reply sends a control-only frame. */
static void test_fake_transmit_control_only(void)
{
    static fake_pl_sent_t out[2];
    const uint16_t own[2] = { 2, 5 };

    HH_ASSERT_OK(fresh());
    HH_ASSERT_OK(write_tx(0x5A, 30, HH_RING_CLASS_DATA, 0x1235, 7, 5, NULL));
    HH_ASSERT_OK(fake_pl_transmit(&pl, &reg, 0x1235, 7, own, 2, out));
    HH_ASSERT(out[0].control_only);
    HH_ASSERT(!out[1].control_only);
    HH_ASSERT_EQ_INT(out[1].length, 30);
    HH_ASSERT_EQ_INT(out[1].bytes[29], 0x5A);
    HH_ASSERT_EQ_INT(pl.tx_control_only, 1);
    HH_ASSERT_EQ_INT(pl.tx_frames_sent, 1);
    HH_ASSERT_EQ_INT(pl.tx_pulls, 1);
}

/* A reply that arrives after the slot edge costs the frame: control-only, counted. */
static void test_fake_transmit_late_reply(void)
{
    static fake_pl_sent_t out[1];
    const uint16_t own[1] = { 5 };

    HH_ASSERT_OK(fresh());
    HH_ASSERT_OK(write_tx(0x5A, 30, HH_RING_CLASS_DATA, 0x1235, 7, 5, NULL));
    pl.drop_next_reply = 1;
    HH_ASSERT_OK(fake_pl_transmit(&pl, &reg, 0x1235, 7, own, 1, out));
    HH_ASSERT(out[0].control_only);
    HH_ASSERT_EQ_INT(pl.tx_pull_late, 1);
    HH_ASSERT_EQ_INT(pl.tx_control_only, 1);
    HH_ASSERT_EQ_INT(hh_ring_view_used(&reg.tx), 0);
}

/* A real MAC PDU goes through the TX path and the RX path with every byte unchanged. */
static void test_real_pdu_both_ways(void)
{
    static fake_pl_sent_t out[1];
    const uint16_t own[1] = { 5 };
    hh_ring_rx_desc_t meta = { .slot_in_frame = 5, .superframe = 0x1234 };
    hh_ring_slot_t s;
    hh_ring_slot_t w;
    hh_ring_tx_desc_t td = { .length = sizeof vec_a, .traffic_class = HH_RING_CLASS_DATA,
                             .target_superframe = 0x1235, .target_frame = 7, .target_slot = 5 };
    hh_pdu_t p;

    HH_ASSERT_OK(fresh());
    HH_ASSERT_OK(hh_ring_produce_begin(&reg.tx, &w));
    memcpy(w.frame, vec_a, sizeof vec_a);
    hh_ring_tx_desc_write(w.desc, &td);
    hh_ring_produce_commit(&reg.tx, &w, 0);
    HH_ASSERT_OK(fake_pl_transmit(&pl, &reg, 0x1235, 7, own, 1, out));
    HH_ASSERT(!out[0].control_only);
    HH_ASSERT_EQ_INT(out[0].length, sizeof vec_a);
    HH_ASSERT(memcmp(out[0].bytes, vec_a, sizeof vec_a) == 0);
    HH_ASSERT_OK(hh_pdu_decode(out[0].bytes, out[0].length, &p));
    HH_ASSERT(memcmp(p.msg[0].data, "HELLO", 5) == 0);

    HH_ASSERT_OK(fake_pl_receive(&pl, &reg, vec_a, sizeof vec_a, 1, &meta));
    HH_ASSERT_OK(hh_ring_consume_peek(&reg.rx, &s));
    HH_ASSERT(memcmp(s.frame, vec_a, sizeof vec_a) == 0);
    HH_ASSERT_OK(hh_pdu_decode(s.frame, sizeof vec_a, &p));
    HH_ASSERT_EQ_INT(p.msg[0].dst, 2);
    HH_ASSERT_EQ_INT(p.msg[0].len, 5);
}

HH_TEST_MAIN_BEGIN("ring_pull")
    HH_RUN(test_exact_match);
    HH_RUN(test_several_in_order);
    HH_RUN(test_stale_dropped);
    HH_RUN(test_future_stays);
    HH_RUN(test_empty);
    HH_RUN(test_superframe_wrap);
    HH_RUN(test_caps);
    HH_RUN(test_bad_descriptor);
    HH_RUN(test_null_args);
    HH_RUN(test_fake_receive);
    HH_RUN(test_fake_receive_drops);
    HH_RUN(test_fake_transmit_control_only);
    HH_RUN(test_fake_transmit_late_reply);
    HH_RUN(test_real_pdu_both_ways);
HH_TEST_MAIN_END()
