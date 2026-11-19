/*
 * MAC side of the rings: the scheduler's PDUs go into the TX ring with the
 * right target and class, and frames from the RX ring reach the scheduler.
 * Two nodes joined by the fake PL. Virtual clock, no sleeps.
 */
#include "hhsdr/ring/mac_driver.h"
#include "hhsdr/core/log.h"
#include "fake_pl.h"
#include "hh_test.h"
#include <stdalign.h>
#include <string.h>

#define NET   0x0010u
#define A     1u
#define B     2u
#define EPOCH 1000u
#define SLOT_SIZE 256u
#define FRAME_MS  20u   /* 20 slots of 1 ms */

typedef struct {
    alignas(32) uint8_t mem[65536];
    hh_ring_region_t    region;
    hh_mac_t            mac;
    hh_slot_t           slot;
    hh_ring_mac_t       drv;
    fake_pl_t           pl;
    hh_mac_rx_frame_t   got[8];
    int                 n_got;
} node_t;

static node_t na, nb;
static hh_time_ms_t t_now;

/* Virtual clock callback. */
static hh_time_ms_t vnow(void *ctx) { (void)ctx; return t_now; }
static const hh_clock_t vclock = { vnow, NULL };

/* Network layer receive callback: records delivered frames. */
static bool receiver(void *ctx, const hh_mac_indication_t *ind)
{
    node_t *n = ctx;
    if (ind->type == HH_MAC_IND_FRAME && n->n_got < 8) n->got[n->n_got++] = *ind->frame;
    return true;
}

/* Brings up one node on its own ring region. ring_slots sets both ring sizes. */
static void setup(node_t *n, hh_node_id_t id, uint64_t own, uint32_t ring_slots)
{
    hh_mac_config_t mc;
    hh_slot_config_t sc;

    memset(n, 0, sizeof *n);
    hh_mac_config_default(&mc, id, &vclock);
    mc.receive = receiver;
    mc.receive_ctx = n;
    mc.report_period_sf = 0;
    hh_mac_init(&n->mac, &mc);
    hh_mac_open(&n->mac);
    hh_mac_set_state(&n->mac, HH_MAC_STEADY);

    HH_ASSERT_OK(hh_ring_region_init(n->mem, hh_ring_region_size(ring_slots, ring_slots, SLOT_SIZE), ring_slots, ring_slots, SLOT_SIZE));
    HH_ASSERT_OK(hh_ring_region_attach(&n->region, n->mem, hh_ring_region_size(ring_slots, ring_slots, SLOT_SIZE)));
    hh_slot_config_default(&sc, id, NET);
    sc.own_slots = own;
    sc.capacity = 128;
    sc.epoch_ms = EPOCH;
    HH_ASSERT_OK(hh_slot_init(&n->slot, &sc, &vclock, &n->mac, hh_ring_mac_tx, &n->drv));
    HH_ASSERT_OK(hh_ring_mac_init(&n->drv, &n->region, &n->slot, 1));
    fake_pl_init(&n->pl);
}

/* Starts a test: quiet log, clock at the epoch, both nodes up. */
static void reset(uint32_t ring_slots)
{
    hh_log_set_level(HH_LOG_ERROR);
    t_now = EPOCH;
    setup(&na, A, 1u << 0, ring_slots);
    setup(&nb, B, 1u << 1, ring_slots);
}

/* Polls the scheduler once per ms for ms milliseconds. */
static void run_ms(node_t *n, unsigned ms)
{
    for (unsigned i = 0; i < ms; i++) {
        hh_slot_poll(&n->slot);
        t_now++;
    }
}

/* Reads the oldest TX descriptor without taking it. */
static hh_status_t peek_tx(node_t *n, hh_ring_tx_desc_t *d)
{
    hh_ring_slot_t s;
    hh_status_t st = hh_ring_consume_peek(&n->region.tx, &s);
    if (st == HH_OK) hh_ring_tx_desc_read(s.desc, d);
    return st;
}

/* A PDU built in frame 0 is targeted at the same slot of frame 1, with its class and length. */
static void test_tx_descriptor(void)
{
    uint8_t pay[20];
    hh_ring_tx_desc_t d;
    hh_ring_slot_t s;
    hh_pdu_t p;

    reset(16);
    memset(pay, 0x5A, sizeof pay);
    HH_ASSERT_EQ_INT(hh_mac_send(&na.mac, HH_FRAME_DATA, HH_MAC_CLASS_DATA, B, 0, pay, sizeof pay), HH_MAC_ACCEPTED);
    run_ms(&na, 1);

    HH_ASSERT_OK(peek_tx(&na, &d));
    HH_ASSERT_EQ_INT(d.target_superframe, 0);
    HH_ASSERT_EQ_INT(d.target_frame, 1);
    HH_ASSERT_EQ_INT(d.target_slot, 0);
    HH_ASSERT_EQ_INT(d.traffic_class, HH_RING_CLASS_DATA);
    HH_ASSERT_EQ_INT(na.drv.st.tx_frames, 1);

    hh_ring_consume_peek(&na.region.tx, &s);
    HH_ASSERT_OK(hh_pdu_decode(s.frame, d.length, &p));
    HH_ASSERT_EQ_INT(p.n_msg, 1);
    HH_ASSERT_EQ_INT(p.msg[0].len, 20);
}

/* The most urgent class among the messages is the ring class; an empty PDU is control. */
static void test_class_is_most_urgent(void)
{
    uint8_t pay[8] = {1};
    hh_ring_tx_desc_t d;
    hh_ring_slot_t s;

    reset(16);
    run_ms(&na, 1);                                  /* nothing queued */
    HH_ASSERT_OK(peek_tx(&na, &d));
    HH_ASSERT_EQ_INT(d.traffic_class, HH_RING_CLASS_CONTROL);
    hh_ring_consume_peek(&na.region.tx, &s);
    hh_ring_consume_release(&na.region.tx, &s, 0);

    run_ms(&na, FRAME_MS - 1);                       /* to the next own slot */
    hh_mac_send(&na.mac, HH_FRAME_DATA, HH_MAC_CLASS_DATA, B, 0, pay, sizeof pay);
    hh_mac_send(&na.mac, HH_FRAME_DATA, HH_MAC_CLASS_EMERGENCY, B, 0, pay, sizeof pay);
    run_ms(&na, 1);
    HH_ASSERT_OK(peek_tx(&na, &d));
    HH_ASSERT_EQ_INT(d.traffic_class, HH_RING_CLASS_EMERGENCY);
}

/* A PDU built in the last frame of a superframe is targeted at frame 0 of the next. */
static void test_target_wraps_superframe(void)
{
    hh_ring_tx_desc_t d;
    hh_ring_slot_t s;

    reset(64);
    run_ms(&na, 49 * FRAME_MS + 1);                  /* own slot of frame 49 is the last */
    for (int i = 0; i < 49; i++) {                   /* take frames 0 to 48 */
        hh_ring_consume_peek(&na.region.tx, &s);
        hh_ring_consume_release(&na.region.tx, &s, 0);
    }
    HH_ASSERT_OK(peek_tx(&na, &d));
    HH_ASSERT_EQ_INT(d.target_superframe, 1);
    HH_ASSERT_EQ_INT(d.target_frame, 0);
    HH_ASSERT_EQ_INT(d.target_slot, 0);
}

/* A full TX ring refuses the PDU, and the scheduler counts the failure. */
static void test_tx_ring_full(void)
{
    reset(4);
    run_ms(&na, 5 * FRAME_MS);                       /* five own slots, four places */
    HH_ASSERT_EQ_INT(na.drv.st.tx_frames, 4);
    HH_ASSERT_EQ_INT(na.drv.st.tx_ring_full, 1);
    HH_ASSERT_EQ_INT(na.slot.st.tx_failed, 1);
}

/* A frame goes node A, TX ring, fake PL pull, air, node B RX ring, node B network layer. */
static void test_end_to_end(void)
{
    uint8_t pay[20];
    uint16_t own = 0;
    fake_pl_sent_t out;
    hh_ring_rx_desc_t meta = { .slot_in_frame = 0, .superframe = 0 };

    reset(16);
    for (unsigned i = 0; i < sizeof pay; i++) pay[i] = (uint8_t)(0xC0 + i);
    hh_mac_send(&na.mac, HH_FRAME_DATA, HH_MAC_CLASS_DATA, B, 0, pay, sizeof pay);
    run_ms(&na, 1);

    HH_ASSERT_OK(fake_pl_transmit(&na.pl, &na.region, 0, 1, &own, 1, &out));
    HH_ASSERT_EQ_INT(out.control_only, 0);
    HH_ASSERT_EQ_INT(na.pl.servant.frames_returned, 1);

    t_now = EPOCH + FRAME_MS;                        /* B hears it in frame 1, slot 0 */
    HH_ASSERT_OK(fake_pl_receive(&nb.pl, &nb.region, out.bytes, out.length, 1, &meta));
    HH_ASSERT_EQ_INT(hh_ring_mac_poll(&nb.drv), 1);
    HH_ASSERT_EQ_INT(nb.n_got, 1);
    HH_ASSERT_EQ_INT(nb.got[0].src, A);
    HH_ASSERT_EQ_INT(nb.got[0].len, sizeof pay);
    HH_ASSERT(memcmp(nb.got[0].payload, pay, sizeof pay) == 0);
    HH_ASSERT_EQ_INT(hh_ring_view_used(&nb.region.rx), 0);
}

/* A frame marked bad by the PL, or longer than the slot, is dropped and counted. */
static void test_rx_drops(void)
{
    hh_ring_slot_t s;
    hh_ring_rx_desc_t d = { .slot_in_frame = 0, .superframe = 0, .length = 10 };

    reset(16);
    hh_ring_produce_begin(&nb.region.rx, &s);        /* CRC flag clear */
    hh_ring_rx_desc_write(s.desc, &d);
    hh_ring_produce_commit(&nb.region.rx, &s, 0);

    hh_ring_produce_begin(&nb.region.rx, &s);        /* length over the slot */
    d.length = SLOT_SIZE + 1;
    hh_ring_rx_desc_write(s.desc, &d);
    hh_ring_produce_commit(&nb.region.rx, &s, HH_RING_RX_CRC_OK);

    HH_ASSERT_EQ_INT(hh_ring_mac_poll(&nb.drv), 0);
    HH_ASSERT_EQ_INT(nb.drv.st.rx_crc_bad, 1);
    HH_ASSERT_EQ_INT(nb.drv.st.rx_bad_descriptor, 1);
    HH_ASSERT_EQ_INT(nb.n_got, 0);
    HH_ASSERT_EQ_INT(hh_ring_view_used(&nb.region.rx), 0);
}

/* Bad arguments are refused. */
static void test_args(void)
{
    hh_ring_mac_t m;
    reset(16);
    HH_ASSERT_ERR(hh_ring_mac_init(&m, NULL, &na.slot, 1), HH_ERR_INVAL);
    HH_ASSERT_ERR(hh_ring_mac_init(&m, &na.region, NULL, 1), HH_ERR_INVAL);
    HH_ASSERT_ERR(hh_ring_mac_init(&m, &na.region, &na.slot, 0), HH_ERR_INVAL);
    HH_ASSERT_EQ_INT(hh_ring_mac_poll(NULL), 0);
}

HH_TEST_MAIN_BEGIN("ring_mac")
    HH_RUN(test_tx_descriptor);
    HH_RUN(test_class_is_most_urgent);
    HH_RUN(test_target_wraps_superframe);
    HH_RUN(test_tx_ring_full);
    HH_RUN(test_end_to_end);
    HH_RUN(test_rx_drops);
    HH_RUN(test_args);
HH_TEST_MAIN_END()
