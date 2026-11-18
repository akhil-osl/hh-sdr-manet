/*
 * hh_netif unit tests on one mock radio. Links without hhsdr_core, so a
 * dependency on the legacy stack fails the build.
 */
#include "hhsdr/netif/netif.h"
#include "hhsdr/core/log.h"
#include "mock_radio.h"
#include "hh_test.h"
#include <string.h>

#define SELF 7u

typedef struct {
    int              count;
    hh_frame_kind_t  kind;
    uint8_t          payload[HH_NETIF_MAX_PAYLOAD];
    uint16_t         len;
    hh_node_id_t     from;
    hh_link_sample_t metrics;
} sink_t;

static void on_deliver(hh_frame_kind_t kind, const uint8_t *payload, uint16_t len,
                       hh_node_id_t from, const hh_link_sample_t *m, void *ctx)
{
    sink_t *s = ctx;
    s->count++;
    s->kind = kind;
    memcpy(s->payload, payload, len);
    s->len = len;
    s->from = from;
    s->metrics = *m;
}

static char g_last_log[512];
static void capture(hh_log_level_t lvl, const char *line, void *ctx)
{
    (void)lvl; (void)ctx;
    strncpy(g_last_log, line, sizeof g_last_log - 1);
}

static void setup(mock_radio_t *m, hh_radio_t *r, hh_netif_t *nif)
{
    hh_log_set_level(HH_LOG_WARN);
    mock_radio_init(m, "n7", r);
    hh_netif_init(nif, SELF, r);
}

static void enqueue(mock_radio_t *m, hh_frame_kind_t kind, hh_node_id_t src,
                    hh_node_id_t dst, const char *text, hh_time_ms_t at)
{
    hh_frame_t f;
    hh_link_sample_t s;
    memset(&f, 0, sizeof f);
    memset(&s, 0, sizeof s);
    f.kind = kind; f.src = src; f.dst = dst;
    f.len = (uint16_t)strlen(text);
    memcpy(f.data, text, f.len);
    s.neighbor_id = src; s.rssi = -55.0f; s.snr = 18.0f;
    mock_radio_enqueue_rx(m, &f, &s, at);
}

static void test_init_rejects_bad_arguments(void)
{
    mock_radio_t m;
    hh_radio_t r;
    hh_netif_t nif;

    mock_radio_init(&m, "n7", &r);
    hh_log_set_level(HH_LOG_WARN);
    HH_ASSERT_ERR(hh_netif_init(NULL, SELF, &r), HH_ERR_INVAL);
    HH_ASSERT_ERR(hh_netif_init(&nif, SELF, NULL), HH_ERR_INVAL);
    {
        hh_radio_t no_ops = { NULL, NULL };
        HH_ASSERT_ERR(hh_netif_init(&nif, SELF, &no_ops), HH_ERR_INVAL);
    }
    /* Id 0 is broadcast. */
    HH_ASSERT_ERR(hh_netif_init(&nif, HH_NODE_ID_INVALID, &r), HH_ERR_INVAL);
    HH_ASSERT_OK(hh_netif_init(&nif, SELF, &r));
}

static void test_send_requires_open(void)
{
    mock_radio_t m;
    hh_radio_t r;
    hh_netif_t nif;
    uint8_t p[4] = {1, 2, 3, 4};

    setup(&m, &r, &nif);
    HH_ASSERT_ERR(hh_netif_send(&nif, HH_FRAME_DATA, 2, p, sizeof p), HH_ERR_STATE);
    HH_ASSERT_ERR(hh_netif_poll(&nif, 0), HH_ERR_STATE);
    HH_ASSERT_OK(hh_netif_open(&nif));
    HH_ASSERT_ERR(hh_netif_open(&nif), HH_ERR_STATE);
    HH_ASSERT_OK(hh_netif_send(&nif, HH_FRAME_DATA, 2, p, sizeof p));
    HH_ASSERT_OK(hh_netif_close(&nif));
    HH_ASSERT_ERR(hh_netif_send(&nif, HH_FRAME_DATA, 2, p, sizeof p), HH_ERR_STATE);
    HH_ASSERT(!m.opened);
}

static void test_unicast_goes_to_the_given_next_hop(void)
{
    mock_radio_t m;
    hh_radio_t r;
    hh_netif_t nif;
    const hh_frame_t *f;
    uint8_t p[5] = {0xde, 0xad, 0xbe, 0xef, 0x01};

    setup(&m, &r, &nif);
    hh_netif_open(&nif);
    HH_ASSERT_OK(hh_netif_send(&nif, HH_FRAME_DATA, 3, p, sizeof p));

    f = mock_radio_last_tx(&m, HH_FRAME_DATA);
    HH_ASSERT(f != NULL);
    HH_ASSERT_EQ_INT(f->src, SELF);
    HH_ASSERT_EQ_INT(f->dst, 3);
    HH_ASSERT_EQ_INT(f->len, sizeof p);
    HH_ASSERT(memcmp(f->data, p, sizeof p) == 0);
    HH_ASSERT_EQ_INT(nif.counters.tx_frames, 1);
    HH_ASSERT_EQ_INT(nif.counters.tx_broadcast, 0);
    HH_ASSERT_EQ_INT(nif.counters.tx_bytes, sizeof p);
}

static void test_kind_is_the_callers_choice(void)
{
    mock_radio_t m;
    hh_radio_t r;
    hh_netif_t nif;
    uint8_t p[1] = {1};

    setup(&m, &r, &nif);
    hh_netif_open(&nif);
    /* Kind for OLSRv2 traffic is undecided (U-24); any kind is carried. */
    HH_ASSERT_OK(hh_netif_send(&nif, HH_FRAME_ROUTING, 2, p, 1));
    HH_ASSERT_OK(hh_netif_send(&nif, HH_FRAME_BEACON, HH_NETIF_BROADCAST, p, 1));
    HH_ASSERT_OK(hh_netif_send(&nif, HH_FRAME_DATA, 2, p, 1));
    HH_ASSERT_EQ_INT(mock_radio_tx_count_kind(&m, HH_FRAME_ROUTING), 1);
    HH_ASSERT_EQ_INT(mock_radio_tx_count_kind(&m, HH_FRAME_BEACON), 1);
    HH_ASSERT_EQ_INT(mock_radio_tx_count_kind(&m, HH_FRAME_DATA), 1);
    HH_ASSERT_ERR(hh_netif_send(&nif, (hh_frame_kind_t)0, 2, p, 1), HH_ERR_INVAL);
    HH_ASSERT_ERR(hh_netif_send(&nif, (hh_frame_kind_t)99, 2, p, 1), HH_ERR_INVAL);
    HH_ASSERT_EQ_INT(m.tx_total, 3);
}

static void test_broadcast_is_destination_zero(void)
{
    mock_radio_t m;
    hh_radio_t r;
    hh_netif_t nif;
    uint8_t p[3] = {9, 9, 9};

    setup(&m, &r, &nif);
    hh_netif_open(&nif);
    HH_ASSERT_OK(hh_netif_send(&nif, HH_FRAME_DATA, HH_NETIF_BROADCAST, p, sizeof p));
    HH_ASSERT_EQ_INT(mock_radio_last_tx(&m, HH_FRAME_DATA)->dst, 0);
    HH_ASSERT_EQ_INT(m.tx_total, 1);
    HH_ASSERT_EQ_INT(nif.counters.tx_broadcast, 1);
}

static void test_next_hop_is_not_judged(void)
{
    mock_radio_t m;
    hh_radio_t r;
    hh_netif_t nif;
    uint8_t p[1] = {1};

    setup(&m, &r, &nif);
    hh_netif_open(&nif);
    /* Invalid destination ids are undecided (DP-11), so none are refused. */
    HH_ASSERT_OK(hh_netif_send(&nif, HH_FRAME_DATA, SELF, p, 1));
    HH_ASSERT_OK(hh_netif_send(&nif, HH_FRAME_DATA, 0xFFFFFFFFu, p, 1));
    HH_ASSERT_EQ_INT(m.tx_total, 2);
}

static void test_send_refusals(void)
{
    mock_radio_t m;
    hh_radio_t r;
    hh_netif_t nif;
    static uint8_t big[HH_NETIF_MAX_PAYLOAD + 1];

    setup(&m, &r, &nif);
    hh_netif_open(&nif);
    hh_log_set_sink(capture, NULL);
    hh_log_set_level(HH_LOG_DEBUG);

    HH_ASSERT_OK(hh_netif_send(&nif, HH_FRAME_DATA, 2, big, HH_NETIF_MAX_PAYLOAD));
    HH_ASSERT_ERR(hh_netif_send(&nif, HH_FRAME_DATA, 2, big, HH_NETIF_MAX_PAYLOAD + 1),
                  HH_ERR_INVAL);
    HH_ASSERT(strstr(g_last_log, "event=tx_rejected reason=too_large") != NULL);
    HH_ASSERT_ERR(hh_netif_send(&nif, HH_FRAME_DATA, 2, NULL, 2), HH_ERR_INVAL);
    HH_ASSERT(strstr(g_last_log, "reason=no_payload") != NULL);

    HH_ASSERT_EQ_INT(m.tx_total, 1);
    HH_ASSERT_EQ_INT(nif.counters.tx_rejected, 2);

    /* Empty payload is allowed. */
    HH_ASSERT_OK(hh_netif_send(&nif, HH_FRAME_DATA, 2, NULL, 0));
    HH_ASSERT_EQ_INT(mock_radio_last_tx(&m, HH_FRAME_DATA)->len, 0);
    hh_log_reset();
}

static void test_radio_refusal_is_passed_through(void)
{
    mock_radio_t m;
    hh_radio_t r;
    hh_netif_t nif;
    uint8_t p[2] = {1, 2};

    setup(&m, &r, &nif);
    hh_netif_open(&nif);
    hh_log_set_level(HH_LOG_ERROR);
    m.tx_fails = true;
    HH_ASSERT_ERR(hh_netif_send(&nif, HH_FRAME_DATA, 2, p, sizeof p), HH_ERR_IO);
    HH_ASSERT_EQ_INT(nif.counters.tx_errors, 1);
    HH_ASSERT_EQ_INT(nif.counters.tx_frames, 0);
    hh_log_reset();
}

static void test_open_failure_is_surfaced(void)
{
    mock_radio_t m;
    hh_radio_t r;
    hh_netif_t nif;

    setup(&m, &r, &nif);
    hh_log_set_sink(capture, NULL);
    hh_log_set_level(HH_LOG_ERROR);
    m.operational = false;
    HH_ASSERT_ERR(hh_netif_open(&nif), HH_ERR_IO);
    HH_ASSERT(strstr(g_last_log, "event=radio_open_failed") != NULL);
    HH_ASSERT(!nif.opened);
    hh_log_reset();
}

static void test_receive_delivers_kind_payload_sender_and_metrics(void)
{
    mock_radio_t m;
    hh_radio_t r;
    hh_netif_t nif;
    sink_t sink;

    setup(&m, &r, &nif);
    memset(&sink, 0, sizeof sink);
    hh_netif_set_deliver(&nif, on_deliver, &sink);
    hh_netif_open(&nif);

    enqueue(&m, HH_FRAME_ROUTING, 4, SELF, "abc", 10);
    HH_ASSERT_OK(hh_netif_poll(&nif, 10));
    HH_ASSERT_EQ_INT(sink.count, 1);
    HH_ASSERT_EQ_INT(sink.kind, HH_FRAME_ROUTING);
    HH_ASSERT_EQ_INT(sink.len, 3);
    HH_ASSERT(memcmp(sink.payload, "abc", 3) == 0);
    HH_ASSERT_EQ_INT(sink.from, 4);
    HH_ASSERT_NEAR(sink.metrics.rssi, -55.0, 0.01);
    HH_ASSERT_NEAR(sink.metrics.snr, 18.0, 0.01);
    HH_ASSERT_EQ_INT(nif.counters.rx_frames, 1);
    HH_ASSERT_EQ_INT(nif.counters.rx_bytes, 3);
}

static void test_receive_does_not_refilter_by_address(void)
{
    mock_radio_t m;
    hh_radio_t r;
    hh_netif_t nif;
    sink_t sink;

    setup(&m, &r, &nif);
    memset(&sink, 0, sizeof sink);
    hh_netif_set_deliver(&nif, on_deliver, &sink);
    hh_netif_open(&nif);

    /* The MAC filters by address; the adapter must not filter again. */
    enqueue(&m, HH_FRAME_DATA, 4, HH_NETIF_BROADCAST, "b", 0);
    enqueue(&m, HH_FRAME_DATA, 4, 99, "u", 0);
    hh_netif_poll(&nif, 0);
    HH_ASSERT_EQ_INT(sink.count, 2);
}

static void test_receive_without_sink_is_counted(void)
{
    mock_radio_t m;
    hh_radio_t r;
    hh_netif_t nif;

    setup(&m, &r, &nif);
    hh_netif_open(&nif);
    enqueue(&m, HH_FRAME_DATA, 4, SELF, "x", 0);
    hh_netif_poll(&nif, 0);
    HH_ASSERT_EQ_INT(nif.counters.rx_dropped_no_sink, 1);
    HH_ASSERT_EQ_INT(nif.counters.rx_frames, 0);
}

static void test_send_copies_payload(void)
{
    mock_radio_t m;
    hh_radio_t r;
    hh_netif_t nif;
    uint8_t p[3] = {1, 2, 3};

    setup(&m, &r, &nif);
    hh_netif_open(&nif);
    HH_ASSERT_OK(hh_netif_send(&nif, HH_FRAME_DATA, 2, p, sizeof p));
    memset(p, 0xff, sizeof p);
    HH_ASSERT(memcmp(mock_radio_last_tx(&m, HH_FRAME_DATA)->data, "\1\2\3", 3) == 0);
}

static void test_poll_delivers_only_due_frames_in_order(void)
{
    mock_radio_t m;
    hh_radio_t r;
    hh_netif_t nif;
    sink_t sink;

    setup(&m, &r, &nif);
    memset(&sink, 0, sizeof sink);
    hh_netif_set_deliver(&nif, on_deliver, &sink);
    hh_netif_open(&nif);

    enqueue(&m, HH_FRAME_DATA, 4, SELF, "a", 5);
    enqueue(&m, HH_FRAME_DATA, 4, SELF, "b", 5);
    enqueue(&m, HH_FRAME_DATA, 4, SELF, "c", 20);
    HH_ASSERT_OK(hh_netif_poll(&nif, 4));
    HH_ASSERT_EQ_INT(sink.count, 0);
    HH_ASSERT_OK(hh_netif_poll(&nif, 10));
    HH_ASSERT_EQ_INT(sink.count, 2);
    HH_ASSERT(sink.payload[0] == 'b');
    HH_ASSERT_OK(hh_netif_poll(&nif, 20));
    HH_ASSERT_EQ_INT(sink.count, 3);
    HH_ASSERT(sink.payload[0] == 'c');
}

static void test_malformed_rx_is_dropped(void)
{
    mock_radio_t m;
    hh_radio_t r;
    hh_netif_t nif;
    sink_t sink;
    hh_frame_t f;
    hh_link_sample_t s;

    setup(&m, &r, &nif);
    hh_log_set_level(HH_LOG_ERROR);
    memset(&sink, 0, sizeof sink);
    hh_netif_set_deliver(&nif, on_deliver, &sink);
    hh_netif_open(&nif);

    memset(&f, 0, sizeof f);
    memset(&s, 0, sizeof s);
    f.kind = HH_FRAME_DATA; f.src = 4; f.dst = SELF;
    f.len = HH_NETIF_MAX_PAYLOAD + 1;
    mock_radio_enqueue_rx(&m, &f, &s, 0);
    f.len = 1;
    f.kind = (hh_frame_kind_t)99;
    mock_radio_enqueue_rx(&m, &f, &s, 0);
    hh_netif_poll(&nif, 0);

    HH_ASSERT_EQ_INT(sink.count, 0);
    HH_ASSERT_EQ_INT(nif.counters.rx_dropped_malformed, 2);
    hh_log_reset();
}

static void test_close_stops_delivery_and_unregisters(void)
{
    mock_radio_t m;
    hh_radio_t r;
    hh_netif_t nif;
    sink_t sink;

    setup(&m, &r, &nif);
    memset(&sink, 0, sizeof sink);
    hh_netif_set_deliver(&nif, on_deliver, &sink);
    HH_ASSERT(m.rx_fn == NULL);
    hh_netif_open(&nif);
    HH_ASSERT(m.rx_fn != NULL);
    HH_ASSERT_OK(hh_netif_close(&nif));
    HH_ASSERT(m.rx_fn == NULL);
    HH_ASSERT_ERR(hh_netif_close(&nif), HH_ERR_STATE);

    /* Someone else polls the radio after close. */
    enqueue(&m, HH_FRAME_DATA, 4, SELF, "x", 0);
    hh_radio_poll(&r, 0);
    HH_ASSERT_EQ_INT(sink.count, 0);
}

static void test_reopen_after_close(void)
{
    mock_radio_t m;
    hh_radio_t r;
    hh_netif_t nif;
    sink_t sink;
    uint8_t p[1] = {1};

    setup(&m, &r, &nif);
    memset(&sink, 0, sizeof sink);
    hh_netif_set_deliver(&nif, on_deliver, &sink);
    hh_netif_open(&nif);
    hh_netif_close(&nif);
    HH_ASSERT_OK(hh_netif_open(&nif));
    HH_ASSERT_OK(hh_netif_send(&nif, HH_FRAME_DATA, 2, p, 1));
    enqueue(&m, HH_FRAME_DATA, 4, SELF, "y", 0);
    hh_netif_poll(&nif, 0);
    HH_ASSERT_EQ_INT(sink.count, 1);
}

static void test_open_failure_leaves_no_callback(void)
{
    mock_radio_t m;
    hh_radio_t r;
    hh_netif_t nif;

    setup(&m, &r, &nif);
    hh_log_set_sink(capture, NULL);
    m.operational = false;
    HH_ASSERT_ERR(hh_netif_open(&nif), HH_ERR_IO);
    HH_ASSERT(m.rx_fn == NULL);
    hh_log_reset();
}

static hh_status_t failing_close(void *self) { (void)self; return HH_ERR_IO; }

static void test_close_failure_is_surfaced(void)
{
    mock_radio_t m;
    hh_radio_t r;
    hh_netif_t nif;
    hh_radio_ops_t ops;

    mock_radio_init(&m, "n7", &r);
    ops = *r.ops;
    ops.close = failing_close;
    r.ops = &ops;
    hh_log_set_sink(capture, NULL);
    hh_log_set_level(HH_LOG_WARN);
    hh_netif_init(&nif, SELF, &r);
    hh_netif_open(&nif);

    HH_ASSERT_ERR(hh_netif_close(&nif), HH_ERR_IO);
    HH_ASSERT(strstr(g_last_log, "event=radio_close_failed") != NULL);
    HH_ASSERT(!nif.opened);
    HH_ASSERT(m.rx_fn == NULL);
    hh_log_reset();
}

typedef struct {
    hh_netif_t *nif;
    int         sent;
} echo_t;

static void on_echo(hh_frame_kind_t kind, const uint8_t *payload, uint16_t len,
                    hh_node_id_t from, const hh_link_sample_t *m, void *ctx)
{
    echo_t *e = ctx;
    (void)m;
    if (hh_netif_send(e->nif, kind, from, payload, len) == HH_OK) e->sent++;
}

static void test_send_from_inside_delivery(void)
{
    mock_radio_t m;
    hh_radio_t r;
    hh_netif_t nif;
    echo_t e;
    const hh_frame_t *f;

    setup(&m, &r, &nif);
    e.nif = &nif;
    e.sent = 0;
    hh_netif_set_deliver(&nif, on_echo, &e);
    hh_netif_open(&nif);
    enqueue(&m, HH_FRAME_ROUTING, 4, HH_NETIF_BROADCAST, "hi", 0);
    hh_netif_poll(&nif, 0);

    HH_ASSERT_EQ_INT(e.sent, 1);
    f = mock_radio_last_tx(&m, HH_FRAME_ROUTING);
    HH_ASSERT(f != NULL);
    HH_ASSERT_EQ_INT(f->dst, 4);
    HH_ASSERT(memcmp(f->data, "hi", 2) == 0);
}

static void test_null_handle_is_safe(void)
{
    uint8_t p[1] = {0};
    HH_ASSERT_ERR(hh_netif_send(NULL, HH_FRAME_DATA, 2, p, 1), HH_ERR_INVAL);
    HH_ASSERT_ERR(hh_netif_open(NULL), HH_ERR_INVAL);
    HH_ASSERT_ERR(hh_netif_close(NULL), HH_ERR_INVAL);
    HH_ASSERT_ERR(hh_netif_poll(NULL, 0), HH_ERR_INVAL);
    HH_ASSERT(hh_netif_counters(NULL) == NULL);
    hh_netif_set_deliver(NULL, on_deliver, NULL);
}

HH_TEST_MAIN_BEGIN("netif")
    HH_RUN(test_init_rejects_bad_arguments);
    HH_RUN(test_send_requires_open);
    HH_RUN(test_unicast_goes_to_the_given_next_hop);
    HH_RUN(test_kind_is_the_callers_choice);
    HH_RUN(test_broadcast_is_destination_zero);
    HH_RUN(test_next_hop_is_not_judged);
    HH_RUN(test_send_refusals);
    HH_RUN(test_radio_refusal_is_passed_through);
    HH_RUN(test_open_failure_is_surfaced);
    HH_RUN(test_receive_delivers_kind_payload_sender_and_metrics);
    HH_RUN(test_receive_does_not_refilter_by_address);
    HH_RUN(test_receive_without_sink_is_counted);
    HH_RUN(test_send_copies_payload);
    HH_RUN(test_poll_delivers_only_due_frames_in_order);
    HH_RUN(test_malformed_rx_is_dropped);
    HH_RUN(test_close_stops_delivery_and_unregisters);
    HH_RUN(test_reopen_after_close);
    HH_RUN(test_open_failure_leaves_no_callback);
    HH_RUN(test_close_failure_is_surfaced);
    HH_RUN(test_send_from_inside_delivery);
    HH_RUN(test_null_handle_is_safe);
HH_TEST_MAIN_END()
