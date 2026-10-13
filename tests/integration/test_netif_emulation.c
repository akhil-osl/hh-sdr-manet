/*
 * hh_netif over an emulated medium, nodes A - B - C, no A-C link.
 * Not an OLSRv2 test: the fixture supplies fixed next hops.
 */
#include "hhsdr/netif/netif.h"
#include "hhsdr/core/log.h"
#include "emu_medium.h"
#include "hh_test.h"
#include <string.h>

#define A 1u
#define B 2u
#define C 3u

/* Fixture choice; the real kind is undecided (U-24). */
#define KIND HH_FRAME_DATA

typedef struct node {
    hh_node_id_t id;
    mock_radio_t mock;
    hh_radio_t   radio;
    hh_netif_t   nif;

    int          rx_count;
    uint8_t      last[HH_NETIF_MAX_PAYLOAD];
    uint16_t     last_len;
    hh_node_id_t last_from;

    /* If set, re-send received payloads here, as Linux forwarding would. */
    hh_node_id_t relay_to;
    int          relay_sent;
} node_t;

typedef struct {
    emu_medium_t medium;
    node_t       nodes[3];
    hh_time_ms_t now;
} net_t;

static void on_deliver(hh_frame_kind_t kind, const uint8_t *payload, uint16_t len,
                       hh_node_id_t from, const hh_link_sample_t *m, void *ctx)
{
    node_t *n = ctx;
    (void)m;
    (void)kind;
    n->rx_count++;
    memcpy(n->last, payload, len);
    n->last_len = len;
    n->last_from = from;
    if (n->relay_to != HH_NODE_ID_INVALID &&
        hh_netif_send(&n->nif, KIND, n->relay_to, payload, len) == HH_OK)
        n->relay_sent++;
}

static node_t *node(net_t *net, hh_node_id_t id) { return &net->nodes[id - 1]; }

static void net_init(net_t *net)
{
    memset(net, 0, sizeof *net);
    hh_log_set_level(HH_LOG_WARN);
    emu_medium_init(&net->medium);
    for (hh_node_id_t id = A; id <= C; id++) {
        node_t *n = node(net, id);
        n->id = id;
        mock_radio_init(&n->mock, "emu", &n->radio);
        hh_netif_init(&n->nif, id, &n->radio);
        hh_netif_set_deliver(&n->nif, on_deliver, n);
        hh_netif_open(&n->nif);
        emu_medium_attach(&net->medium, id, &n->mock);
    }
    /* A - B - C. There is deliberately no A-C link. */
    emu_medium_link_up(&net->medium, A, B, -50.0f);
    emu_medium_link_up(&net->medium, B, C, -50.0f);
}

/* Two rounds so a relayed payload is delivered too. */
static void net_run(net_t *net)
{
    for (int round = 0; round < 2; round++) {
        net->now += 10;
        emu_medium_step(&net->medium, net->now);
        for (hh_node_id_t id = A; id <= C; id++)
            hh_netif_poll(&node(net, id)->nif, net->now);
    }
}

static void test_broadcast_reaches_neighbours_only(void)
{
    net_t net;
    const uint8_t p[] = "hello";

    net_init(&net);
    HH_ASSERT_OK(hh_netif_send(&node(&net, A)->nif, KIND, HH_NETIF_BROADCAST, p, sizeof p));
    net_run(&net);
    HH_ASSERT_EQ_INT(node(&net, B)->rx_count, 1);
    HH_ASSERT_EQ_INT(node(&net, B)->last_from, A);
    /* Broadcast is not relayed. */
    HH_ASSERT_EQ_INT(node(&net, C)->rx_count, 0);

    HH_ASSERT_OK(hh_netif_send(&node(&net, B)->nif, KIND, HH_NETIF_BROADCAST, p, sizeof p));
    net_run(&net);
    HH_ASSERT_EQ_INT(node(&net, A)->rx_count, 1);
    HH_ASSERT_EQ_INT(node(&net, C)->rx_count, 1);
    HH_ASSERT_EQ_INT(node(&net, B)->nif.counters.tx_frames, 1);
}

static void test_unicast_reaches_only_the_next_hop(void)
{
    net_t net;
    const uint8_t p[] = {1, 2, 3};

    net_init(&net);
    HH_ASSERT_OK(hh_netif_send(&node(&net, B)->nif, KIND, C, p, sizeof p));
    net_run(&net);
    HH_ASSERT_EQ_INT(node(&net, C)->rx_count, 1);
    HH_ASSERT(memcmp(node(&net, C)->last, p, sizeof p) == 0);
    /* In range but not addressed. */
    HH_ASSERT_EQ_INT(node(&net, A)->rx_count, 0);
    HH_ASSERT(net.medium.not_addressed >= 1);
}

static void test_no_direct_path_from_a_to_c(void)
{
    net_t net;
    const uint8_t p[] = {7};

    net_init(&net);
    /* Accepted is not delivered. */
    HH_ASSERT_OK(hh_netif_send(&node(&net, A)->nif, KIND, C, p, sizeof p));
    net_run(&net);
    HH_ASSERT_EQ_INT(node(&net, C)->rx_count, 0);
    HH_ASSERT_EQ_INT(node(&net, B)->rx_count, 0);
}

static void test_two_hops_when_the_network_side_forwards(void)
{
    net_t net;
    const uint8_t p[] = "A to C through B";

    net_init(&net);
    /* Forwarding is the fixture's, not the adapter's. */
    node(&net, B)->relay_to = C;

    HH_ASSERT_OK(hh_netif_send(&node(&net, A)->nif, KIND, B, p, sizeof p));
    net_run(&net);

    HH_ASSERT_EQ_INT(node(&net, B)->relay_sent, 1);
    HH_ASSERT_EQ_INT(node(&net, C)->rx_count, 1);
    HH_ASSERT_EQ_INT(node(&net, C)->last_len, sizeof p);
    HH_ASSERT(memcmp(node(&net, C)->last, p, sizeof p) == 0);
    /* Source is the last hop, not the originator. */
    HH_ASSERT_EQ_INT(node(&net, C)->last_from, B);
}

static void test_broken_link_stops_traffic_until_restored(void)
{
    net_t net;
    const uint8_t p[] = {42};

    net_init(&net);
    node(&net, B)->relay_to = C;

    emu_medium_link_down(&net.medium, B, C);
    hh_netif_send(&node(&net, A)->nif, KIND, B, p, sizeof p);
    net_run(&net);
    HH_ASSERT_EQ_INT(node(&net, B)->rx_count, 1);
    HH_ASSERT_EQ_INT(node(&net, C)->rx_count, 0);

    emu_medium_link_up(&net.medium, B, C, -50.0f);
    hh_netif_send(&node(&net, A)->nif, KIND, B, p, sizeof p);
    net_run(&net);
    HH_ASSERT_EQ_INT(node(&net, C)->rx_count, 1);
    HH_ASSERT_EQ_INT(node(&net, C)->last_from, B);
}

HH_TEST_MAIN_BEGIN("netif_emulation")
    HH_RUN(test_broadcast_reaches_neighbours_only);
    HH_RUN(test_unicast_reaches_only_the_next_hop);
    HH_RUN(test_no_direct_path_from_a_to_c);
    HH_RUN(test_two_hops_when_the_network_side_forwards);
    HH_RUN(test_broken_link_stops_traffic_until_restored);
HH_TEST_MAIN_END()
