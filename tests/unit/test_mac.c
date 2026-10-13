/*
 * hh_mac unit tests: queues, priority, starvation limit, refusals, receive
 * buffer and link reports. Links hhsdr_mac only.
 */
#include "hhsdr/mac/mac.h"
#include "hhsdr/core/log.h"
#include "hh_test.h"
#include <string.h>

#define SELF 7u

#define ASSERT_RES(expr, want) \
    do { hh_mac_result_t _r = (expr); \
         if (_r != (want)) HH_FAIL("%s returned %s, want %s", #expr, \
                                   hh_mac_result_str(_r), hh_mac_result_str(want)); } while (0)

static hh_mac_t mac;   /* large, so not on the stack */

static int g_errors;
static void count_errors(hh_log_level_t lvl, const char *line, void *ctx)
{
    (void)line; (void)ctx;
    if (lvl == HH_LOG_ERROR) g_errors++;
}

static void setup_cfg(const hh_mac_config_t *cfg)
{
    hh_log_set_level(HH_LOG_ERROR);
    hh_mac_init(&mac, cfg);
    hh_mac_open(&mac);
    hh_mac_set_state(&mac, HH_MAC_STEADY);
}

static void setup(void)
{
    hh_mac_config_t cfg;
    hh_mac_config_default(&cfg, SELF);
    setup_cfg(&cfg);
}

static hh_mac_result_t send1(hh_frame_kind_t kind, uint8_t tag)
{
    return hh_mac_send(&mac, kind, 3, &tag, 1);
}

static uint8_t next_tag(void)
{
    hh_mac_tx_frame_t f;
    return hh_mac_next_pdu(&mac, &f) ? f.payload[0] : 0xFF;
}

static void rx_pdu(hh_node_id_t src, hh_node_id_t dst, const char *text, uint32_t sf)
{
    hh_mac_rx_pdu_t p;
    memset(&p, 0, sizeof p);
    p.kind = HH_FRAME_DATA;
    p.src = src;
    p.dst = dst;
    p.len = (uint16_t)strlen(text);
    p.payload = (const uint8_t *)text;
    p.rssi.value = -60.0f; p.rssi.valid = true;
    p.snr.valid = false;
    p.superframe = sf; p.superframe_valid = true;
    hh_mac_on_rx(&mac, &p);
}

/* Default settings are correct; out-of-range settings are refused. */
static void test_config_defaults_and_limits(void)
{
    hh_mac_config_t cfg;
    hh_mac_config_default(&cfg, SELF);
    HH_ASSERT_EQ_INT(cfg.queue_depth[HH_MAC_CLASS_CONTROL], 16);
    HH_ASSERT_EQ_INT(cfg.queue_depth[HH_MAC_CLASS_DATA], 64);
    HH_ASSERT_EQ_INT(cfg.data_share_n, 0);
    HH_ASSERT_EQ_INT(cfg.max_payload, 512);

    g_errors = 0;
    hh_log_set_sink(count_errors, NULL);
    cfg.queue_depth[HH_MAC_CLASS_DATA] = HH_MAC_QUEUE_CAP + 1;
    ASSERT_RES(hh_mac_init(&mac, &cfg), HH_MAC_INVALID_CONFIG);
    cfg.queue_depth[HH_MAC_CLASS_DATA] = 0;
    ASSERT_RES(hh_mac_init(&mac, &cfg), HH_MAC_INVALID_CONFIG);
    hh_mac_config_default(&cfg, HH_NODE_ID_INVALID);
    ASSERT_RES(hh_mac_init(&mac, &cfg), HH_MAC_INVALID_CONFIG);
    HH_ASSERT_EQ_INT(g_errors, 3);
    hh_log_reset();
}

/* A frame comes out of the queue exactly as it went in. */
static void test_enqueue_dequeue_unchanged(void)
{
    hh_mac_tx_frame_t f;
    const uint8_t p[5] = {1, 2, 3, 4, 5};

    setup();
    ASSERT_RES(hh_mac_send(&mac, HH_FRAME_DATA, 9, p, sizeof p), HH_MAC_ACCEPTED);
    HH_ASSERT(hh_mac_next_pdu(&mac, &f));
    HH_ASSERT_EQ_INT(f.kind, HH_FRAME_DATA);
    HH_ASSERT_EQ_INT(f.src, SELF);
    HH_ASSERT_EQ_INT(f.dst, 9);
    HH_ASSERT_EQ_INT(f.len, sizeof p);
    HH_ASSERT(memcmp(f.payload, p, sizeof p) == 0);
    HH_ASSERT(!hh_mac_next_pdu(&mac, &f));
}

/* Within one class queue, the oldest frame goes first. */
static void test_fifo_within_class(void)
{
    setup();
    for (uint8_t i = 1; i <= 5; i++) ASSERT_RES(send1(HH_FRAME_DATA, i), HH_MAC_ACCEPTED);
    for (uint8_t i = 1; i <= 5; i++) HH_ASSERT_EQ_INT(next_tag(), i);
}

/* Control goes before data, even data sent earlier. */
static void test_control_before_data(void)
{
    setup();
    send1(HH_FRAME_DATA, 1);
    send1(HH_FRAME_DATA, 2);
    send1(HH_FRAME_BEACON, 10);
    send1(HH_FRAME_ROUTING, 11);
    HH_ASSERT_EQ_INT(next_tag(), 10);
    HH_ASSERT_EQ_INT(next_tag(), 11);
    HH_ASSERT_EQ_INT(next_tag(), 1);
    HH_ASSERT_EQ_INT(next_tag(), 2);
}

/* A full queue returns TRY_AGAIN and drops nothing already queued. */
static void test_overflow_try_again_nothing_evicted(void)
{
    hh_mac_config_t cfg;
    hh_mac_status_t st;

    hh_mac_config_default(&cfg, SELF);
    cfg.queue_depth[HH_MAC_CLASS_DATA] = 3;
    setup_cfg(&cfg);
    for (uint8_t i = 1; i <= 3; i++) ASSERT_RES(send1(HH_FRAME_DATA, i), HH_MAC_ACCEPTED);
    ASSERT_RES(send1(HH_FRAME_DATA, 4), HH_MAC_TRY_AGAIN);
    ASSERT_RES(send1(HH_FRAME_DATA, 5), HH_MAC_TRY_AGAIN);

    /* A full data queue never blocks control. */
    ASSERT_RES(send1(HH_FRAME_BEACON, 20), HH_MAC_ACCEPTED);

    hh_mac_status(&mac, &st);
    HH_ASSERT_EQ_INT(st.queue_full[HH_MAC_CLASS_DATA], 2);
    HH_ASSERT_EQ_INT(st.queue_depth[HH_MAC_CLASS_DATA], 3);
    HH_ASSERT_EQ_INT(st.queue_depth[HH_MAC_CLASS_CONTROL], 1);

    HH_ASSERT_EQ_INT(next_tag(), 20);
    for (uint8_t i = 1; i <= 3; i++) HH_ASSERT_EQ_INT(next_tag(), i);
    HH_ASSERT_EQ_INT(next_tag(), 0xFF);
}

/* Of 1000 sends, only 64 data and 16 control frames are accepted. */
static void test_queue_bounded_at_configured_depth(void)
{
    int accepted = 0;

    setup();
    for (int i = 0; i < 1000; i++)
        if (send1(HH_FRAME_DATA, (uint8_t)i) == HH_MAC_ACCEPTED) accepted++;
    HH_ASSERT_EQ_INT(accepted, 64);
    accepted = 0;
    for (int i = 0; i < 1000; i++)
        if (send1(HH_FRAME_ROUTING, (uint8_t)i) == HH_MAC_ACCEPTED) accepted++;
    HH_ASSERT_EQ_INT(accepted, 16);
}

/* Starvation limit off: data waits while control keeps coming. */
static void test_strict_priority_starves_data_when_off(void)
{
    setup();
    send1(HH_FRAME_DATA, 1);
    for (int round = 0; round < 10; round++) {
        send1(HH_FRAME_BEACON, 9);
        HH_ASSERT_EQ_INT(next_tag(), 9);
    }
    HH_ASSERT_EQ_INT(next_tag(), 1);
}

/* Starvation limit 3: data gets every 4th slot. */
static void test_starvation_limit_gives_data_a_share(void)
{
    hh_mac_config_t cfg;
    int data_seen = 0;

    hh_mac_config_default(&cfg, SELF);
    cfg.data_share_n = 3;
    setup_cfg(&cfg);
    for (uint8_t i = 1; i <= 3; i++) send1(HH_FRAME_DATA, i);

    /* Control keeps arriving; data still gets every fourth slot. */
    for (int slot = 0; slot < 12; slot++) {
        uint8_t t;
        send1(HH_FRAME_BEACON, 9);
        t = next_tag();
        if (t != 9) {
            data_seen++;
            HH_ASSERT_EQ_INT(t, data_seen);
            HH_ASSERT_EQ_INT(slot % 4, 3);
        }
    }
    HH_ASSERT_EQ_INT(data_seen, 3);
}

/* Starvation limit on, no data waiting: control goes every slot. */
static void test_starvation_limit_does_not_delay_control_without_data(void)
{
    hh_mac_config_t cfg;

    hh_mac_config_default(&cfg, SELF);
    cfg.data_share_n = 1;
    setup_cfg(&cfg);
    for (int i = 0; i < 5; i++) send1(HH_FRAME_ROUTING, 9);
    for (int i = 0; i < 5; i++) HH_ASSERT_EQ_INT(next_tag(), 9);
}

/* Each wrong call gets the right refusal code and is counted. */
static void test_refusals_and_counters(void)
{
    hh_mac_status_t st;
    static uint8_t big[HH_MAC_PAYLOAD_MAX + 1];

    hh_log_set_level(HH_LOG_ERROR);
    hh_mac_config_t cfg;
    hh_mac_config_default(&cfg, SELF);
    hh_mac_init(&mac, &cfg);
    ASSERT_RES(send1(HH_FRAME_DATA, 1), HH_MAC_NOT_OPEN);

    hh_mac_open(&mac);
    ASSERT_RES(send1(HH_FRAME_DATA, 1), HH_MAC_WRONG_STATE);   /* WAIT_TIME */
    hh_mac_set_state(&mac, HH_MAC_HOLDOVER);
    ASSERT_RES(send1(HH_FRAME_DATA, 1), HH_MAC_ACCEPTED);
    hh_mac_set_state(&mac, HH_MAC_STEADY);

    ASSERT_RES(hh_mac_send(&mac, HH_FRAME_DATA, 3, big, HH_MAC_PAYLOAD_MAX), HH_MAC_ACCEPTED);
    ASSERT_RES(hh_mac_send(&mac, HH_FRAME_DATA, 3, big, HH_MAC_PAYLOAD_MAX + 1), HH_MAC_TOO_LARGE);
    ASSERT_RES(hh_mac_send(&mac, (hh_frame_kind_t)99, 3, big, 1), HH_MAC_INVALID_ARGUMENT);
    ASSERT_RES(hh_mac_send(&mac, HH_FRAME_DATA, 3, NULL, 4), HH_MAC_INVALID_ARGUMENT);
    ASSERT_RES(hh_mac_send(&mac, HH_FRAME_DATA, HH_MAC_BROADCAST, NULL, 0), HH_MAC_ACCEPTED);
    ASSERT_RES(hh_mac_channel_select(&mac, 2), HH_MAC_UNSUPPORTED);

    hh_mac_status(&mac, &st);
    HH_ASSERT_EQ_INT(st.refused_state, 1);
    HH_ASSERT_EQ_INT(st.refused_too_large, 1);
    HH_ASSERT_EQ_INT(st.refused_invalid, 2);
    HH_ASSERT_EQ_INT(st.queue_depth[HH_MAC_CLASS_DATA], 3);
}

/* Outside STEADY nothing goes to MAC logic; queued frames are kept. */
static void test_no_pdu_outside_steady_frames_kept(void)
{
    setup();
    send1(HH_FRAME_DATA, 1);
    hh_mac_set_state(&mac, HH_MAC_LOST);
    HH_ASSERT_EQ_INT(next_tag(), 0xFF);
    hh_mac_set_state(&mac, HH_MAC_STEADY);
    HH_ASSERT_EQ_INT(next_tag(), 1);
}

/* Close discards and counts queued frames; a second Close is safe. */
static void test_close_discards_and_is_safe_twice(void)
{
    hh_mac_status_t st;

    setup();
    send1(HH_FRAME_DATA, 1);
    send1(HH_FRAME_DATA, 2);
    send1(HH_FRAME_BEACON, 3);
    ASSERT_RES(hh_mac_close(&mac), HH_MAC_OK);
    ASSERT_RES(hh_mac_close(&mac), HH_MAC_OK);
    ASSERT_RES(send1(HH_FRAME_DATA, 1), HH_MAC_NOT_OPEN);

    hh_mac_open(&mac);
    hh_mac_status(&mac, &st);
    HH_ASSERT_EQ_INT(st.close_discards[HH_MAC_CLASS_DATA], 2);
    HH_ASSERT_EQ_INT(st.close_discards[HH_MAC_CLASS_CONTROL], 1);
    HH_ASSERT_EQ_INT(st.queue_depth[HH_MAC_CLASS_DATA], 0);
    HH_ASSERT_EQ_INT(st.state, HH_MAC_WAIT_TIME);
}

/* Received frames come up only on collect, and only for this node or broadcast. */
static void test_receive_only_on_collect_with_filter(void)
{
    hh_mac_rx_frame_t f;
    hh_mac_status_t st;
    bool got;

    setup();
    rx_pdu(4, SELF, "to me", 100);
    rx_pdu(4, HH_MAC_BROADCAST, "bcast", 101);
    rx_pdu(4, 99, "other", 102);

    ASSERT_RES(hh_mac_collect(&mac, &f, &got), HH_MAC_OK);
    HH_ASSERT(got);
    HH_ASSERT_EQ_INT(f.src, 4);
    HH_ASSERT_EQ_INT(f.dst, SELF);
    HH_ASSERT(memcmp(f.payload, "to me", 5) == 0);
    HH_ASSERT(f.metrics.rssi.valid);
    HH_ASSERT_NEAR(f.metrics.rssi.value, -60.0, 0.01);
    HH_ASSERT(!f.metrics.snr.valid);
    HH_ASSERT(!f.metrics.per.valid);
    HH_ASSERT(f.superframe_valid);
    HH_ASSERT_EQ_INT(f.superframe, 100);

    hh_mac_collect(&mac, &f, &got);
    HH_ASSERT(got);
    HH_ASSERT_EQ_INT(f.dst, HH_MAC_BROADCAST);
    hh_mac_collect(&mac, &f, &got);
    HH_ASSERT(!got);

    hh_mac_status(&mac, &st);
    HH_ASSERT_EQ_INT(st.rx_not_addressed, 1);
    HH_ASSERT_EQ_INT(st.frames_rx, 2);
}

/* A full receive buffer drops the newest frame and counts it. */
static void test_rx_overflow_drops_newest(void)
{
    hh_mac_config_t cfg;
    hh_mac_rx_frame_t f;
    hh_mac_status_t st;
    bool got;

    hh_mac_config_default(&cfg, SELF);
    cfg.rx_buffer_depth = 2;
    setup_cfg(&cfg);
    rx_pdu(4, SELF, "a", 1);
    rx_pdu(4, SELF, "b", 2);
    rx_pdu(4, SELF, "c", 3);

    hh_mac_status(&mac, &st);
    HH_ASSERT_EQ_INT(st.rx_overflow, 1);
    hh_mac_collect(&mac, &f, &got);
    HH_ASSERT(got && f.payload[0] == 'a');
    hh_mac_collect(&mac, &f, &got);
    HH_ASSERT(got && f.payload[0] == 'b');
    hh_mac_collect(&mac, &f, &got);
    HH_ASSERT(!got);
}

/* Frames received in LISTEN are not delivered by default. */
static void test_rx_outside_steady_not_delivered_by_default(void)
{
    hh_mac_rx_frame_t f;
    hh_mac_status_t st;
    bool got;

    setup();
    hh_mac_set_state(&mac, HH_MAC_LISTEN);
    rx_pdu(4, SELF, "x", 1);
    hh_mac_collect(&mac, &f, &got);
    HH_ASSERT(!got);
    hh_mac_status(&mac, &st);
    HH_ASSERT_EQ_INT(st.rx_not_delivered_state, 1);
    HH_ASSERT(!st.operational_known);
}

/* Unknown neighbour gives NOT_FOUND; averaged values are not valid. */
static void test_link_report_valid_flags(void)
{
    hh_mac_link_report_t r;

    setup();
    ASSERT_RES(hh_mac_link_metrics(&mac, 4, &r), HH_MAC_NOT_FOUND);
    rx_pdu(4, SELF, "a", 50);
    rx_pdu(4, 99, "b", 51);        /* other destination still counts */
    ASSERT_RES(hh_mac_link_metrics(&mac, 4, &r), HH_MAC_OK);
    HH_ASSERT_EQ_INT(r.neighbor, 4);
    HH_ASSERT_EQ_INT(r.rx_count, 2);
    HH_ASSERT(r.last_heard_valid);
    HH_ASSERT_EQ_INT(r.last_heard_superframe, 51);
    /* No averaging yet, so no made-up values. */
    HH_ASSERT(!r.rssi.valid);
    HH_ASSERT(!r.snr.valid);
    HH_ASSERT(!r.per.valid);
}

/* A full neighbour table counts the overflow and does not grow. */
static void test_neighbor_table_bounded(void)
{
    hh_mac_status_t st;

    setup();
    for (hh_node_id_t id = 100; id < 100 + HH_MAC_NEIGHBOR_CAP + 5; id++)
        rx_pdu(id, SELF, "n", 1);
    hh_mac_status(&mac, &st);
    HH_ASSERT_EQ_INT(st.neighbor_table_full, 5);
}

/* Status gives the right operational flag and counts CRC failures. */
static void test_status_operational(void)
{
    hh_mac_status_t st;

    setup();
    hh_mac_status(&mac, &st);
    HH_ASSERT(st.operational && st.operational_known);
    hh_mac_set_state(&mac, HH_MAC_SAFE);
    hh_mac_status(&mac, &st);
    HH_ASSERT(!st.operational && st.operational_known);
    hh_mac_on_crc_fail(&mac);
    hh_mac_status(&mac, &st);
    HH_ASSERT_EQ_INT(st.rx_crc_fail, 1);
}

/* NULL arguments do not crash. */
static void test_null_safety(void)
{
    hh_mac_tx_frame_t f;
    bool got;
    ASSERT_RES(hh_mac_init(NULL, NULL), HH_MAC_INVALID_ARGUMENT);
    ASSERT_RES(hh_mac_send(NULL, HH_FRAME_DATA, 1, NULL, 0), HH_MAC_INVALID_ARGUMENT);
    ASSERT_RES(hh_mac_collect(NULL, NULL, &got), HH_MAC_INVALID_ARGUMENT);
    HH_ASSERT(!hh_mac_next_pdu(NULL, &f));
    hh_mac_on_rx(NULL, NULL);
    hh_mac_set_state(NULL, HH_MAC_STEADY);
}

HH_TEST_MAIN_BEGIN("mac")
    HH_RUN(test_config_defaults_and_limits);
    HH_RUN(test_enqueue_dequeue_unchanged);
    HH_RUN(test_fifo_within_class);
    HH_RUN(test_control_before_data);
    HH_RUN(test_overflow_try_again_nothing_evicted);
    HH_RUN(test_queue_bounded_at_configured_depth);
    HH_RUN(test_strict_priority_starves_data_when_off);
    HH_RUN(test_starvation_limit_gives_data_a_share);
    HH_RUN(test_starvation_limit_does_not_delay_control_without_data);
    HH_RUN(test_refusals_and_counters);
    HH_RUN(test_no_pdu_outside_steady_frames_kept);
    HH_RUN(test_close_discards_and_is_safe_twice);
    HH_RUN(test_receive_only_on_collect_with_filter);
    HH_RUN(test_rx_overflow_drops_newest);
    HH_RUN(test_rx_outside_steady_not_delivered_by_default);
    HH_RUN(test_link_report_valid_flags);
    HH_RUN(test_neighbor_table_bounded);
    HH_RUN(test_status_operational);
    HH_RUN(test_null_safety);
HH_TEST_MAIN_END()
