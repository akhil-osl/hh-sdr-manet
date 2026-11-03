/*
 * Call setup signalling: request, accept, reject, release and timeouts.
 * Nodes are wired through an in-memory link on a fake clock, so no test
 * sleeps and a message can be dropped on purpose. Each message and event is
 * printed with the fake time, which is the call setup log.
 */
#include "hh_test.h"
#include "hhsdr/dataterm/call.h"
#include <stdio.h>

#define NODES 3
#define QLEN  16

typedef struct {
    hh_node_id_t from, to;
    uint8_t      pdu[HH_CALL_PDU_LEN];
    size_t       len;
} link_msg_t;

typedef struct {
    hh_node_id_t       id;
    hh_call_t          call;
    hh_call_event_kind_t last_ev;
    hh_call_reason_t   last_reason;
    int                ev_count;
} node_t;

static hh_time_ms_t g_now;
static node_t       g_n[NODES];
static link_msg_t   g_q[QLEN];
static int          g_qn;

static hh_time_ms_t clk_now(void *ctx) { (void)ctx; return g_now; }
static const hh_clock_t g_clk = { clk_now, NULL };

static const char *msg_name(uint8_t t)
{
    static const char *n[] = { "?", "request", "accept", "reject", "release" };
    return t < 5 ? n[t] : "?";
}

static void on_send(hh_node_id_t to, const uint8_t *pdu, size_t len, void *ctx)
{
    node_t *from = ctx;
    printf("    t=%-6llu node %u -> node %u  %-8s id=%u reason=%u\n", (unsigned long long)g_now,
           from->id, to, msg_name(pdu[2]), (pdu[0] << 8) | pdu[1], pdu[3]);
    if (g_qn < QLEN && len == HH_CALL_PDU_LEN) {
        g_q[g_qn] = (link_msg_t){ .from = from->id, .to = to, .len = len };
        memcpy(g_q[g_qn].pdu, pdu, len);
        g_qn++;
    }
}

static void on_event(const hh_call_event_t *ev, void *ctx)
{
    static const char *k[] = { "incoming", "active", "ended" };
    static const char *r[] = { "none", "busy", "declined", "timeout", "normal" };
    node_t *n = ctx;
    n->last_ev = ev->kind;
    n->last_reason = ev->reason;
    n->ev_count++;
    printf("    t=%-6llu node %u event %-8s peer=%u id=%u reason=%s\n", (unsigned long long)g_now,
           n->id, k[ev->kind], ev->peer, ev->id, r[ev->reason]);
}

static node_t *node(hh_node_id_t id) { return &g_n[id - 1]; }

static void setup(void)
{
    hh_call_config_t cfg;
    hh_call_config_default(&cfg);
    cfg.setup_timeout_ms = 1000;
    cfg.ring_timeout_ms = 3000;
    g_now = 0;
    g_qn = 0;
    for (int i = 0; i < NODES; i++) {
        memset(&g_n[i], 0, sizeof g_n[i]);
        g_n[i].id = (hh_node_id_t)(i + 1);
        hh_call_init(&g_n[i].call, &cfg, &g_clk, on_send, on_event, &g_n[i]);
    }
}

/* Delivers everything queued, including replies sent while delivering. */
static void pump(void)
{
    for (int guard = 0; g_qn && guard < 64; guard++) {
        link_msg_t m = g_q[0];
        memmove(&g_q[0], &g_q[1], (size_t)(g_qn - 1) * sizeof g_q[0]);
        g_qn--;
        hh_call_on_rx(&node(m.to)->call, m.from, m.pdu, m.len);
    }
}

static void drop_all(void) { g_qn = 0; }

static void advance(uint32_t ms)
{
    g_now += ms;
    for (int i = 0; i < NODES; i++) hh_call_poll(&g_n[i].call);
}

static void test_request_accept_release(void)
{
    puts("    -- request, accept, release");
    setup();
    HH_ASSERT_OK(hh_call_request(&node(1)->call, 2));
    HH_ASSERT_EQ_INT(node(1)->call.state, HH_CALL_CALLING);
    pump();
    HH_ASSERT_EQ_INT(node(2)->call.state, HH_CALL_RINGING);
    HH_ASSERT_EQ_INT(node(2)->last_ev, HH_CALL_EV_INCOMING);
    HH_ASSERT_OK(hh_call_accept(&node(2)->call));
    pump();
    HH_ASSERT_EQ_INT(node(1)->call.state, HH_CALL_ACTIVE);
    HH_ASSERT_EQ_INT(node(2)->call.state, HH_CALL_ACTIVE);
    advance(60000);  /* an active call has no timeout */
    HH_ASSERT_EQ_INT(node(1)->call.state, HH_CALL_ACTIVE);
    HH_ASSERT_OK(hh_call_release(&node(1)->call));
    pump();
    HH_ASSERT_EQ_INT(node(1)->call.state, HH_CALL_IDLE);
    HH_ASSERT_EQ_INT(node(2)->call.state, HH_CALL_IDLE);
    HH_ASSERT_EQ_INT(node(2)->last_ev, HH_CALL_EV_ENDED);
    HH_ASSERT_EQ_INT(node(2)->last_reason, HH_CALL_REASON_NORMAL);
}

static void test_reject(void)
{
    puts("    -- reject");
    setup();
    HH_ASSERT_OK(hh_call_request(&node(1)->call, 2));
    pump();
    HH_ASSERT_OK(hh_call_reject(&node(2)->call));
    pump();
    HH_ASSERT_EQ_INT(node(1)->call.state, HH_CALL_IDLE);
    HH_ASSERT_EQ_INT(node(1)->last_reason, HH_CALL_REASON_DECLINED);
    HH_ASSERT_EQ_INT(node(2)->call.state, HH_CALL_IDLE);
}

static void test_busy_node_rejects_second_caller(void)
{
    puts("    -- busy");
    setup();
    HH_ASSERT_OK(hh_call_request(&node(1)->call, 2));
    pump();
    HH_ASSERT_OK(hh_call_accept(&node(2)->call));
    pump();
    HH_ASSERT_OK(hh_call_request(&node(3)->call, 2));
    pump();
    HH_ASSERT_EQ_INT(node(3)->call.state, HH_CALL_IDLE);
    HH_ASSERT_EQ_INT(node(3)->last_reason, HH_CALL_REASON_BUSY);
    HH_ASSERT_EQ_INT(node(2)->call.state, HH_CALL_ACTIVE);  /* the call in progress is untouched */
    HH_ASSERT_EQ_INT(node(2)->call.counters.busy_rejected, 1);
}

static void test_setup_timeout_clears_state(void)
{
    puts("    -- setup timeout, state cleared, next call works");
    setup();
    HH_ASSERT_OK(hh_call_request(&node(1)->call, 2));
    uint16_t first = node(1)->call.id;
    HH_ASSERT_EQ_INT(hh_call_next_timeout_ms(&node(1)->call), 1000);
    drop_all();  /* the request is lost */
    advance(999);
    HH_ASSERT_EQ_INT(node(1)->call.state, HH_CALL_CALLING);
    advance(1);
    HH_ASSERT_EQ_INT(node(1)->call.state, HH_CALL_IDLE);
    HH_ASSERT_EQ_INT(node(1)->last_reason, HH_CALL_REASON_TIMEOUT);
    HH_ASSERT_EQ_INT(node(1)->call.peer, HH_NODE_ID_INVALID);
    HH_ASSERT_EQ_INT(node(1)->call.id, 0);
    HH_ASSERT_EQ_INT(hh_call_next_timeout_ms(&node(1)->call), -1);
    drop_all();
    /* A late accept for the dead call is ignored. */
    uint8_t late[HH_CALL_PDU_LEN] = { (uint8_t)(first >> 8), (uint8_t)first, HH_CALL_MSG_ACCEPT, 0 };
    hh_call_on_rx(&node(1)->call, 2, late, sizeof late);
    HH_ASSERT_EQ_INT(node(1)->call.state, HH_CALL_IDLE);
    HH_ASSERT_EQ_INT(node(1)->call.counters.rx_stray, 1);
    HH_ASSERT_OK(hh_call_request(&node(1)->call, 2));
    HH_ASSERT(node(1)->call.id != first);
}

static void test_ring_timeout_rejects_and_clears(void)
{
    puts("    -- ring timeout");
    setup();
    HH_ASSERT_OK(hh_call_request(&node(1)->call, 2));
    pump();
    advance(2999);
    HH_ASSERT_EQ_INT(node(2)->call.state, HH_CALL_RINGING);
    advance(1);
    HH_ASSERT_EQ_INT(node(2)->call.state, HH_CALL_IDLE);
    HH_ASSERT_EQ_INT(node(2)->call.id, 0);
    pump();
    HH_ASSERT_EQ_INT(node(1)->call.state, HH_CALL_IDLE);
    HH_ASSERT_EQ_INT(node(1)->last_reason, HH_CALL_REASON_TIMEOUT);
    HH_ASSERT_ERR(hh_call_accept(&node(2)->call), HH_ERR_STATE);
}

static void test_cancel_while_ringing_at_peer(void)
{
    puts("    -- caller cancels");
    setup();
    HH_ASSERT_OK(hh_call_request(&node(1)->call, 2));
    pump();
    HH_ASSERT_OK(hh_call_release(&node(1)->call));
    pump();
    HH_ASSERT_EQ_INT(node(2)->call.state, HH_CALL_IDLE);
    HH_ASSERT_EQ_INT(node(2)->last_reason, HH_CALL_REASON_NORMAL);
}

static void test_duplicates_and_bad_input_are_ignored(void)
{
    puts("    -- duplicate and malformed input");
    setup();
    HH_ASSERT_OK(hh_call_request(&node(1)->call, 2));
    uint8_t dup[HH_CALL_PDU_LEN];
    memcpy(dup, g_q[0].pdu, sizeof dup);
    pump();
    hh_call_on_rx(&node(2)->call, 1, dup, sizeof dup);  /* repeated request */
    HH_ASSERT_EQ_INT(node(2)->call.state, HH_CALL_RINGING);
    HH_ASSERT_EQ_INT(node(2)->call.counters.rx_duplicate, 1);
    HH_ASSERT_EQ_INT(node(2)->ev_count, 1);
    uint8_t bad_type[HH_CALL_PDU_LEN] = { 0, 1, 9, 0 };
    uint8_t zero_id[HH_CALL_PDU_LEN] = { 0, 0, HH_CALL_MSG_RELEASE, 0 };
    hh_call_on_rx(&node(2)->call, 1, bad_type, sizeof bad_type);
    hh_call_on_rx(&node(2)->call, 1, zero_id, sizeof zero_id);
    hh_call_on_rx(&node(2)->call, 1, dup, 3);
    hh_call_on_rx(&node(2)->call, 1, NULL, 0);
    HH_ASSERT_EQ_INT(node(2)->call.counters.rx_bad, 4);
    HH_ASSERT_EQ_INT(node(2)->call.state, HH_CALL_RINGING);
    /* A release from a node that is not the caller does not end the call. */
    uint8_t rel[HH_CALL_PDU_LEN] = { dup[0], dup[1], HH_CALL_MSG_RELEASE, 4 };
    hh_call_on_rx(&node(2)->call, 3, rel, sizeof rel);
    HH_ASSERT_EQ_INT(node(2)->call.state, HH_CALL_RINGING);
}

static void test_calls_refused_in_wrong_state(void)
{
    puts("    -- wrong state");
    setup();
    HH_ASSERT_ERR(hh_call_accept(&node(1)->call), HH_ERR_STATE);
    HH_ASSERT_ERR(hh_call_reject(&node(1)->call), HH_ERR_STATE);
    HH_ASSERT_ERR(hh_call_release(&node(1)->call), HH_ERR_STATE);
    HH_ASSERT_ERR(hh_call_request(&node(1)->call, HH_NODE_ID_INVALID), HH_ERR_INVAL);
    HH_ASSERT_OK(hh_call_request(&node(1)->call, 2));
    HH_ASSERT_ERR(hh_call_request(&node(1)->call, 3), HH_ERR_STATE);
}

HH_TEST_MAIN_BEGIN("dataterm call setup")
    HH_RUN(test_request_accept_release);
    HH_RUN(test_reject);
    HH_RUN(test_busy_node_rejects_second_caller);
    HH_RUN(test_setup_timeout_clears_state);
    HH_RUN(test_ring_timeout_rejects_and_clears);
    HH_RUN(test_cancel_while_ringing_at_peer);
    HH_RUN(test_duplicates_and_bad_input_are_ignored);
    HH_RUN(test_calls_refused_in_wrong_state);
HH_TEST_MAIN_END()
