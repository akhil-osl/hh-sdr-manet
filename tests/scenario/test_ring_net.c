/*
 * Ring-based network: frames between real MACs and schedulers travel through
 * the TX ring, the fake PL pull, the air and the RX ring. Virtual time.
 */
#include "ring_net.h"
#include "hh_test.h"
#include <string.h>

typedef struct { unsigned node; hh_node_id_t src; uint16_t len; uint8_t first; } got_t;

static got_t got[32];
static int n_got;

/* Records frames delivered to a node's network layer. */
static void on_rx(void *ctx, unsigned node, const hh_mac_rx_frame_t *f)
{
    (void)ctx;
    if (n_got < 32) { got[n_got].node = node; got[n_got].src = f->src; got[n_got].len = f->len; got[n_got].first = f->payload[0]; n_got++; }
}

/* A network of n nodes on a line, 20 slots of 1 ms. */
static ring_net_t *make(unsigned n)
{
    rn_config_t c;
    n_got = 0;
    rn_config_default(&c, (uint16_t)n);
    c.rx_hook = on_rx;
    return rn_create(&c);
}

/* Runs ms milliseconds, one slot per step, from time t0. */
static void run(ring_net_t *net, uint32_t t0, uint32_t ms)
{
    for (uint32_t t = t0; t < t0 + ms; t++) { rn_set_time(net, t); rn_step(net); }
}

/* A frame from node 1 reaches node 2, and not within the frame it was built in. */
static void test_delivery_with_lead(void)
{
    ring_net_t *net = make(3);
    uint8_t pay[16] = { 0x77 };
    rn_stats_t st;

    HH_ASSERT(net != NULL);
    rn_topology_line(net);
    HH_ASSERT_EQ_INT(rn_send(net, 1, HH_FRAME_DATA, HH_MAC_CLASS_DATA, 2, pay, sizeof pay), HH_MAC_ACCEPTED);
    run(net, 0, 19);                       /* the rest of the frame it was built in */
    HH_ASSERT_EQ_INT(n_got, 0);
    run(net, 19, 22);                      /* into frame 1, past slot 0 */
    HH_ASSERT_EQ_INT(n_got, 1);
    HH_ASSERT_EQ_INT(got[0].node, 2);
    HH_ASSERT_EQ_INT(got[0].src, 1);
    HH_ASSERT_EQ_INT(got[0].len, 16);
    HH_ASSERT_EQ_INT(got[0].first, 0x77);
    rn_stats(net, &st);
    HH_ASSERT_EQ_INT(st.collision_events, 0);
    HH_ASSERT_EQ_INT(st.tx_ring_full, 0);
    rn_destroy(net);
}

/* A unicast frame is for its next hop only; a node out of range hears nothing. */
static void test_unicast_and_range(void)
{
    ring_net_t *net = make(3);
    uint8_t pay[8] = { 1 };

    HH_ASSERT(net != NULL);
    rn_topology_line(net);
    rn_send(net, 1, HH_FRAME_DATA, HH_MAC_CLASS_DATA, 3, pay, sizeof pay);   /* node 3 is two hops away */
    run(net, 0, 60);
    HH_ASSERT_EQ_INT(n_got, 0);
    rn_destroy(net);
}

/* A broadcast reaches every neighbour; a link taken down stops it. */
static void test_broadcast_and_link_down(void)
{
    ring_net_t *net = make(3);
    uint8_t pay[8] = { 2 };

    HH_ASSERT(net != NULL);
    rn_topology_mesh(net);
    rn_send(net, 1, HH_FRAME_DATA, HH_MAC_CLASS_DATA, HH_MAC_BROADCAST, pay, sizeof pay);
    run(net, 0, 60);
    HH_ASSERT_EQ_INT(n_got, 2);

    n_got = 0;
    rn_link(net, 1, 3, false);
    rn_send(net, 1, HH_FRAME_DATA, HH_MAC_CLASS_DATA, HH_MAC_BROADCAST, pay, sizeof pay);
    run(net, 60, 60);
    HH_ASSERT_EQ_INT(n_got, 1);
    HH_ASSERT_EQ_INT(got[0].node, 2);
    rn_destroy(net);
}

/* Several frames in a row keep their order and none is lost on a quiet link. */
static void test_stream_in_order(void)
{
    ring_net_t *net = make(2);
    rn_stats_t st;

    HH_ASSERT(net != NULL);
    rn_topology_line(net);
    for (uint8_t i = 0; i < 5; i++) {
        uint8_t pay[8];
        memset(pay, i, sizeof pay);
        rn_send(net, 1, HH_FRAME_DATA, HH_MAC_CLASS_DATA, 2, pay, sizeof pay);
    }
    run(net, 0, 400);
    HH_ASSERT_EQ_INT(n_got, 5);
    for (int i = 0; i < 5; i++) HH_ASSERT_EQ_INT(got[i].first, i);
    rn_stats(net, &st);
    HH_ASSERT_EQ_INT(st.pull_stale, 0);
    rn_destroy(net);
}

/* Invalid configurations are refused. */
static void test_invalid(void)
{
    rn_config_t c;
    rn_config_default(&c, 1);
    HH_ASSERT(rn_create(&c) == NULL);
    rn_config_default(&c, 4);
    c.lead_frames = 0;
    HH_ASSERT(rn_create(&c) == NULL);
    c.lead_frames = 1;
    c.ring_slots = 3;
    HH_ASSERT(rn_create(&c) == NULL);
}

HH_TEST_MAIN_BEGIN("ring_net")
    HH_RUN(test_delivery_with_lead);
    HH_RUN(test_unicast_and_range);
    HH_RUN(test_broadcast_and_link_down);
    HH_RUN(test_stream_in_order);
    HH_RUN(test_invalid);
HH_TEST_MAIN_END()
