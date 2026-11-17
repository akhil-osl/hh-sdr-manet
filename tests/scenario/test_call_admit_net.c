/*
 * Voice admission at call setup, on the slot-timed network with allocation on.
 * The call module asks the scheduler whether the voice reserve has room. An
 * existing voice flow uses most of the reserve, so a call that needs more is
 * refused and the existing flow is untouched; a smaller call still fits.
 */
#include "slot_net.h"
#include "hhsdr/dataterm/call.h"
#include "hh_test.h"
#include <stdio.h>
#include <string.h>

typedef struct {
    const slot_net_t *net;
    unsigned          node;
    unsigned          msgs;   /* voice messages per frame the call would send */
} ask_t;

static hh_time_ms_t g_now;
static hh_time_ms_t clk_now(void *ctx) { (void)ctx; return g_now; }
static const hh_clock_t g_clk = { clk_now, NULL };

/* Sends nothing anywhere: only the refusal of the request is looked at. */
static hh_status_t null_send(hh_node_id_t to, const uint8_t *pdu, size_t len, void *ctx)
{
    (void)to; (void)pdu; (void)len; (void)ctx;
    return HH_OK;
}

/* Asks the scheduler of the node whether the call fits the voice reserve. */
static bool room(void *ctx)
{
    ask_t *a = ctx;
    bool ok = false;

    return hh_slot_voice_admit(sn_slot(a->net, a->node), a->msgs, &ok) == HH_OK && ok;
}

/* Makes a call module for a node that asks for msgs messages per frame. */
static void make_call(hh_call_t *c, ask_t *a, const slot_net_t *n, unsigned node, unsigned msgs)
{
    hh_call_config_t cfg;

    hh_call_config_default(&cfg);
    a->net = n;
    a->node = node;
    a->msgs = msgs;
    HH_ASSERT_OK(hh_call_init(c, &cfg, &g_clk, null_send, NULL, NULL));
    hh_call_set_admit(c, room, a);
}

static void test_call_refused_when_voice_reserve_is_used(void)
{
    sn_config_t cfg;
    hh_alloc_params_t ap;
    slot_net_t *n;
    hh_call_t big, small, early;
    ask_t abig, asmall, aearly;
    sn_flow_t voice = { 1, 2, 24, 5, 3000, HH_MAC_CLASS_VOICE };   /* four messages a frame from 3 s on */
    sn_flow_t data = { 2, 3, 40, 20, 0, 0 };
    sn_result_t r;

    hh_alloc_params_default(&ap, 20);
    ap.voice_reserve = 4;
    sn_config_default(&cfg, 4);
    cfg.hello_interval_ms = 0;
    cfg.tc_interval_ms = 0;
    cfg.alloc = &ap;
    n = sn_create(&cfg);
    HH_ASSERT(n != NULL);
    sn_topology_mesh(n);
    HH_ASSERT(sn_add_flow(n, &voice));
    HH_ASSERT(sn_add_flow(n, &data));
    make_call(&big, &abig, n, 3, 4);       /* a call as large as the flow already running */
    make_call(&small, &asmall, n, 3, 2);   /* a call of half the size */
    make_call(&early, &aearly, n, 3, 4);

    sn_advance(n, 2500);   /* no voice yet: the reserve is free */
    HH_ASSERT_OK(hh_call_request(&early, 4));
    HH_ASSERT_OK(hh_call_release(&early));

    sn_advance(n, 7500);   /* the voice flow is carried and reported everywhere */
    g_now = 10000;
    HH_ASSERT_ERR(hh_call_request(&big, 4), HH_ERR_AGAIN);
    HH_ASSERT_EQ_INT(big.state, HH_CALL_IDLE);
    HH_ASSERT_EQ_INT(big.counters.admit_refused, 1);
    HH_ASSERT_OK(hh_call_request(&small, 4));
    sn_advance(n, 5000);
    sn_result(n, &r);
    HH_ASSERT(r.total.sent > 0);
    HH_ASSERT(r.flow[0].delivered == r.flow[0].sent || r.flow[0].delivered + 8 >= r.flow[0].sent);
    printf("call_admit big_call=refused small_call=admitted voice_sent=%u voice_delivered=%u\n", r.flow[0].sent,
           r.flow[0].delivered);
    sn_destroy(n);
}

HH_TEST_MAIN_BEGIN("call_admit_net")
    HH_RUN(test_call_refused_when_voice_reserve_is_used);
HH_TEST_MAIN_END()
