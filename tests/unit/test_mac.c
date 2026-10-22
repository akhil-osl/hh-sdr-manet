/*
 * hh_mac unit tests against the MAC to network interface v1.1: five class
 * queues, drop policy, lifetime, TX inhibit, control cap, minimum share, push
 * delivery with holding buffer, events, status and link records.
 * Links hhsdr_mac only.
 */
#include "hhsdr/mac/mac.h"
#include "hhsdr/core/log.h"
#include "hh_test.h"
#include <string.h>

#define SELF 7u
#define GROUP 0xF0000005u

#define ASSERT_RES(expr, want) \
    do { hh_mac_result_t _r = (expr); \
         if (_r != (want)) HH_FAIL("%s returned %s, want %s", #expr, \
                                   hh_mac_result_str(_r), hh_mac_result_str(want)); } while (0)

static hh_mac_t mac;   /* large, so not on the stack */

static hh_time_ms_t t_now;
static hh_time_ms_t vnow(void *ctx) { (void)ctx; return t_now; }
static const hh_clock_t vclock = { vnow, NULL };

/* What the network layer received. */
typedef struct {
    hh_mac_ind_type_t   type;
    hh_mac_rx_frame_t   frame;
    hh_mac_event_t      event;
} rec_t;
static rec_t got[64];
static int n_got;
static bool refuse;

static bool receiver(void *ctx, const hh_mac_indication_t *ind)
{
    (void)ctx;
    if (refuse || n_got == 64) return false;
    got[n_got].type = ind->type;
    if (ind->type == HH_MAC_IND_FRAME) got[n_got].frame = *ind->frame;
    else got[n_got].event = *ind->event;
    n_got++;
    return true;
}

static int g_errors;
static void count_errors(hh_log_level_t lvl, const char *line, void *ctx)
{
    (void)line; (void)ctx;
    if (lvl == HH_LOG_ERROR) g_errors++;
}

static void base_cfg(hh_mac_config_t *cfg)
{
    hh_mac_config_default(cfg, SELF, &vclock);
    cfg->receive = receiver;
    cfg->report_period_sf = 0;
}

static void setup_cfg(const hh_mac_config_t *cfg)
{
    hh_log_set_level(HH_LOG_ERROR);
    t_now = 1000;
    n_got = 0;
    refuse = false;
    hh_mac_init(&mac, cfg);
    hh_mac_open(&mac);
    hh_mac_set_state(&mac, HH_MAC_STEADY);
    n_got = 0;   /* drop the STATE_CHANGED event */
}

static void setup(void)
{
    hh_mac_config_t cfg;
    base_cfg(&cfg);
    setup_cfg(&cfg);
}

static hh_mac_result_t sendc(hh_mac_class_t cls, uint8_t tag)
{
    return hh_mac_send(&mac, HH_FRAME_DATA, cls, 3, 0, &tag, 1);
}

static hh_mac_result_t send_ctl(uint8_t tag)
{
    return hh_mac_send(&mac, HH_FRAME_ROUTING, HH_MAC_CLASS_CONTROL, 0, 0, &tag, 1);
}

static uint8_t next_tag(void)
{
    hh_mac_tx_frame_t f;
    return hh_mac_next_pdu(&mac, &f) ? f.payload[0] : 0xFF;
}

static void rx_pdu(hh_node_id_t src, hh_node_id_t dst, hh_mac_class_t cls, uint8_t tag)
{
    hh_mac_rx_pdu_t p;
    memset(&p, 0, sizeof p);
    p.kind = HH_FRAME_DATA;
    p.cls = cls;
    p.src = src;
    p.dst = dst;
    p.superframe = 42;
    p.len = 1;
    p.payload = &tag;
    hh_mac_on_rx(&mac, &p);
}

static int count_events(hh_mac_event_kind_t kind)
{
    int n = 0;
    for (int i = 0; i < n_got; i++)
        if (got[i].type == HH_MAC_IND_EVENT && got[i].event.kind == kind) n++;
    return n;
}

static int count_frames(void)
{
    int n = 0;
    for (int i = 0; i < n_got; i++) if (got[i].type == HH_MAC_IND_FRAME) n++;
    return n;
}

/* Defaults follow section 9.1; out-of-range settings are refused; Close is safe after a failed init. */
static void test_config_defaults_and_limits(void)
{
    hh_mac_config_t cfg;
    hh_mac_config_default(&cfg, SELF, &vclock);
    HH_ASSERT_EQ_INT(cfg.max_payload, 1280);
    HH_ASSERT_EQ_INT(cfg.queue_depth[HH_MAC_CLASS_CONTROL], 16);
    HH_ASSERT_EQ_INT(cfg.queue_depth[HH_MAC_CLASS_EMERGENCY], 8);
    HH_ASSERT_EQ_INT(cfg.queue_depth[HH_MAC_CLASS_VOICE], 8);
    HH_ASSERT_EQ_INT(cfg.queue_depth[HH_MAC_CLASS_VIDEO], 32);
    HH_ASSERT_EQ_INT(cfg.queue_depth[HH_MAC_CLASS_DATA], 64);
    HH_ASSERT_EQ_INT(cfg.report_period_sf, 1);
    HH_ASSERT_EQ_INT(cfg.ctl_cap_pdus_per_s, 0);
    HH_ASSERT_EQ_INT(cfg.min_share_n, 0);

    g_errors = 0;
    hh_log_set_sink(count_errors, NULL);
    cfg.queue_depth[HH_MAC_CLASS_DATA] = HH_MAC_TX_POOL_CAP;
    ASSERT_RES(hh_mac_init(&mac, &cfg), HH_MAC_INVALID_CONFIG);
    ASSERT_RES(hh_mac_close(&mac), HH_MAC_OK);
    ASSERT_RES(hh_mac_open(&mac), HH_MAC_INVALID_CONFIG);
    hh_mac_config_default(&cfg, SELF, &vclock);
    cfg.max_payload = HH_MAC_PAYLOAD_MAX + 1;
    ASSERT_RES(hh_mac_init(&mac, &cfg), HH_MAC_INVALID_CONFIG);
    hh_mac_config_default(&cfg, SELF, NULL);
    ASSERT_RES(hh_mac_init(&mac, &cfg), HH_MAC_INVALID_CONFIG);
    hh_mac_config_default(&cfg, HH_MAC_BROADCAST, &vclock);
    ASSERT_RES(hh_mac_init(&mac, &cfg), HH_MAC_INVALID_CONFIG);
    hh_mac_config_default(&cfg, GROUP, &vclock);
    ASSERT_RES(hh_mac_init(&mac, &cfg), HH_MAC_INVALID_CONFIG);
    hh_mac_config_default(&cfg, SELF, &vclock);
    cfg.ctl_cap_bytes_per_s = 500;
    cfg.ctl_burst_bytes = 100;   /* smaller than one frame */
    ASSERT_RES(hh_mac_init(&mac, &cfg), HH_MAC_INVALID_CONFIG);
    HH_ASSERT_EQ_INT(g_errors, 6);
    hh_log_reset();
}

/* Open enters WAIT_TIME and is idempotent; Send is refused until STEADY. */
static void test_open_and_send_states(void)
{
    hh_mac_config_t cfg;
    hh_mac_status_t st;
    uint8_t b = 1;

    base_cfg(&cfg);
    hh_log_set_level(HH_LOG_ERROR);
    hh_mac_init(&mac, &cfg);
    ASSERT_RES(hh_mac_send(&mac, HH_FRAME_DATA, HH_MAC_CLASS_DATA, 3, 0, &b, 1), HH_MAC_NOT_OPEN);
    ASSERT_RES(hh_mac_open(&mac), HH_MAC_OK);
    ASSERT_RES(hh_mac_open(&mac), HH_MAC_OK);
    ASSERT_RES(hh_mac_status(&mac, &st), HH_MAC_OK);
    HH_ASSERT_EQ_INT(st.state, HH_MAC_WAIT_TIME);
    const hh_mac_state_t refused[] = { HH_MAC_WAIT_TIME, HH_MAC_LISTEN, HH_MAC_ENTRY,
                                       HH_MAC_LOST, HH_MAC_SAFE };
    for (unsigned i = 0; i < sizeof refused / sizeof refused[0]; i++) {
        hh_mac_set_state(&mac, refused[i]);
        ASSERT_RES(hh_mac_send(&mac, HH_FRAME_DATA, HH_MAC_CLASS_DATA, 3, 0, &b, 1),
                   HH_MAC_WRONG_STATE);
    }
    hh_mac_set_state(&mac, HH_MAC_HOLDOVER);
    ASSERT_RES(hh_mac_send(&mac, HH_FRAME_DATA, HH_MAC_CLASS_DATA, 3, 0, &b, 1), HH_MAC_ACCEPTED);
    hh_mac_set_state(&mac, HH_MAC_STEADY);
    ASSERT_RES(hh_mac_send(&mac, HH_FRAME_DATA, HH_MAC_CLASS_DATA, 3, 0, &b, 1), HH_MAC_ACCEPTED);
}

/* A frame comes out exactly as it went in, with source and class filled by the MAC. */
static void test_enqueue_dequeue_unchanged(void)
{
    hh_mac_tx_frame_t f;
    const uint8_t p[5] = {1, 2, 3, 4, 5};

    setup();
    ASSERT_RES(hh_mac_send(&mac, HH_FRAME_DATA, HH_MAC_CLASS_VIDEO, 9, 0, p, sizeof p),
               HH_MAC_ACCEPTED);
    HH_ASSERT(hh_mac_next_pdu(&mac, &f));
    HH_ASSERT_EQ_INT(f.kind, HH_FRAME_DATA);
    HH_ASSERT_EQ_INT(f.cls, HH_MAC_CLASS_VIDEO);
    HH_ASSERT_EQ_INT(f.src, SELF);
    HH_ASSERT_EQ_INT(f.dst, 9);
    HH_ASSERT_EQ_INT(f.len, sizeof p);
    HH_ASSERT(memcmp(f.payload, p, sizeof p) == 0);
    HH_ASSERT(!hh_mac_next_pdu(&mac, &f));
}

/* Beacon and routing are forced to control; data may not use control; unknown values are refused. */
static void test_kind_and_class_rules(void)
{
    hh_mac_tx_frame_t f;
    uint8_t b = 1;

    setup();
    ASSERT_RES(hh_mac_send(&mac, HH_FRAME_BEACON, HH_MAC_CLASS_DATA, 0, 0, &b, 1), HH_MAC_ACCEPTED);
    HH_ASSERT(hh_mac_next_pdu(&mac, &f));
    HH_ASSERT_EQ_INT(f.cls, HH_MAC_CLASS_CONTROL);
    ASSERT_RES(hh_mac_send(&mac, HH_FRAME_DATA, HH_MAC_CLASS_CONTROL, 3, 0, &b, 1),
               HH_MAC_INVALID_ARGUMENT);
    ASSERT_RES(hh_mac_send(&mac, HH_FRAME_DATA, HH_MAC_CLASS_COUNT, 3, 0, &b, 1),
               HH_MAC_INVALID_ARGUMENT);
    ASSERT_RES(hh_mac_send(&mac, (hh_frame_kind_t)99, HH_MAC_CLASS_DATA, 3, 0, &b, 1),
               HH_MAC_INVALID_ARGUMENT);
    ASSERT_RES(hh_mac_send(&mac, HH_FRAME_DATA, HH_MAC_CLASS_DATA, 3, 0, NULL, 4),
               HH_MAC_INVALID_ARGUMENT);
    HH_ASSERT_EQ_INT(mac.st.refused_invalid, 4);
}

/* Classes leave in the order CONTROL > EMERGENCY > VOICE > VIDEO > DATA. */
static void test_class_order(void)
{
    setup();
    sendc(HH_MAC_CLASS_DATA, 5);
    sendc(HH_MAC_CLASS_VIDEO, 4);
    sendc(HH_MAC_CLASS_VOICE, 3);
    sendc(HH_MAC_CLASS_EMERGENCY, 2);
    send_ctl(1);
    for (uint8_t i = 1; i <= 5; i++) HH_ASSERT_EQ_INT(next_tag(), i);
    HH_ASSERT_EQ_INT(next_tag(), 0xFF);
}

/* Within one class, the oldest frame goes first. */
static void test_fifo_within_class(void)
{
    setup();
    for (uint8_t i = 1; i <= 5; i++) ASSERT_RES(sendc(HH_MAC_CLASS_DATA, i), HH_MAC_ACCEPTED);
    for (uint8_t i = 1; i <= 5; i++) HH_ASSERT_EQ_INT(next_tag(), i);
}

/* Control, emergency and data refuse with TRY_AGAIN when full and drop nothing. */
static void test_try_again_classes_drop_nothing(void)
{
    const hh_mac_class_t cls[] = { HH_MAC_CLASS_EMERGENCY, HH_MAC_CLASS_DATA };
    hh_mac_config_t cfg;

    base_cfg(&cfg);
    for (int k = 0; k < HH_MAC_CLASS_COUNT; k++) cfg.queue_depth[k] = 3;
    for (unsigned c = 0; c < 2; c++) {
        setup_cfg(&cfg);
        for (uint8_t i = 1; i <= 3; i++) ASSERT_RES(sendc(cls[c], i), HH_MAC_ACCEPTED);
        ASSERT_RES(sendc(cls[c], 4), HH_MAC_TRY_AGAIN);
        HH_ASSERT_EQ_INT(mac.st.queue_full[cls[c]], 1);
        for (uint8_t i = 1; i <= 3; i++) HH_ASSERT_EQ_INT(next_tag(), i);
    }
    setup_cfg(&cfg);
    for (uint8_t i = 1; i <= 3; i++) ASSERT_RES(send_ctl(i), HH_MAC_ACCEPTED);
    ASSERT_RES(send_ctl(4), HH_MAC_TRY_AGAIN);
    HH_ASSERT_EQ_INT(mac.st.queue_full[HH_MAC_CLASS_CONTROL], 1);
}

/* Voice and video drop their oldest frame when full, accept the new one, and count the drop. */
static void test_voice_video_drop_oldest(void)
{
    const hh_mac_class_t cls[] = { HH_MAC_CLASS_VOICE, HH_MAC_CLASS_VIDEO };
    hh_mac_config_t cfg;

    base_cfg(&cfg);
    for (int k = 0; k < HH_MAC_CLASS_COUNT; k++) cfg.queue_depth[k] = 3;
    for (unsigned c = 0; c < 2; c++) {
        setup_cfg(&cfg);
        for (uint8_t i = 1; i <= 5; i++) ASSERT_RES(sendc(cls[c], i), HH_MAC_ACCEPTED);
        HH_ASSERT_EQ_INT(mac.st.drop_oldest[cls[c]], 2);
        for (uint8_t i = 3; i <= 5; i++) HH_ASSERT_EQ_INT(next_tag(), i);
        HH_ASSERT_EQ_INT(next_tag(), 0xFF);
    }
}

/* A full queue of one class never stops another class being accepted. */
static void test_full_class_does_not_block_others(void)
{
    hh_mac_config_t cfg;

    base_cfg(&cfg);
    cfg.queue_depth[HH_MAC_CLASS_DATA] = 1;
    setup_cfg(&cfg);
    ASSERT_RES(sendc(HH_MAC_CLASS_DATA, 1), HH_MAC_ACCEPTED);
    ASSERT_RES(sendc(HH_MAC_CLASS_DATA, 2), HH_MAC_TRY_AGAIN);
    ASSERT_RES(sendc(HH_MAC_CLASS_VOICE, 3), HH_MAC_ACCEPTED);
    ASSERT_RES(send_ctl(4), HH_MAC_ACCEPTED);
}

/* A frame whose lifetime has passed is discarded before it is sent and counted; 0 means no limit. */
static void test_lifetime(void)
{
    uint8_t a = 1, b = 2;

    setup();
    ASSERT_RES(hh_mac_send(&mac, HH_FRAME_DATA, HH_MAC_CLASS_VOICE, 3, 50, &a, 1), HH_MAC_ACCEPTED);
    ASSERT_RES(hh_mac_send(&mac, HH_FRAME_DATA, HH_MAC_CLASS_DATA, 3, 0, &b, 1), HH_MAC_ACCEPTED);
    t_now += 50;
    HH_ASSERT_EQ_INT(next_tag(), 2);
    HH_ASSERT_EQ_INT(next_tag(), 0xFF);
    HH_ASSERT_EQ_INT(mac.st.lifetime_expired[HH_MAC_CLASS_VOICE], 1);

    ASSERT_RES(hh_mac_send(&mac, HH_FRAME_DATA, HH_MAC_CLASS_VOICE, 3, 50, &a, 1), HH_MAC_ACCEPTED);
    t_now += 49;
    HH_ASSERT_EQ_INT(next_tag(), 1);
}

/* While TX is inhibited Send returns TX_INHIBITED, no PDU is handed out, and ON/OFF events carry the reason. */
static void test_tx_inhibit(void)
{
    hh_mac_status_t st;

    setup();
    ASSERT_RES(sendc(HH_MAC_CLASS_DATA, 1), HH_MAC_ACCEPTED);
    hh_mac_set_tx_inhibit(&mac, HH_MAC_INHIBIT_SULK);
    ASSERT_RES(sendc(HH_MAC_CLASS_DATA, 2), HH_MAC_TX_INHIBITED);
    HH_ASSERT_EQ_INT(next_tag(), 0xFF);
    hh_mac_on_tx_inhibited_slot(&mac);
    hh_mac_status(&mac, &st);
    HH_ASSERT_EQ_INT(st.tx_inhibit_reason, HH_MAC_INHIBIT_SULK);
    HH_ASSERT_EQ_INT(st.tx_inhibit, 1);
    HH_ASSERT_EQ_INT(st.refused_inhibited, 1);

    rx_pdu(5, SELF, HH_MAC_CLASS_DATA, 9);   /* RX still delivered in sulk */
    hh_mac_set_tx_inhibit(&mac, HH_MAC_INHIBIT_NONE);
    HH_ASSERT_EQ_INT(next_tag(), 1);

    HH_ASSERT_EQ_INT(count_events(HH_MAC_EV_TX_INHIBIT_ON), 1);
    HH_ASSERT_EQ_INT(count_events(HH_MAC_EV_TX_INHIBIT_OFF), 1);
    HH_ASSERT_EQ_INT(got[0].event.detail, HH_MAC_INHIBIT_SULK);
    HH_ASSERT_EQ_INT(count_frames(), 1);
}

/* Payloads up to the current maximum are accepted; a change is announced by PARAMETERS_CHANGED. */
static void test_max_payload(void)
{
    static uint8_t big[HH_MAC_PAYLOAD_MAX];
    hh_mac_status_t st;

    setup();
    ASSERT_RES(hh_mac_send(&mac, HH_FRAME_DATA, HH_MAC_CLASS_DATA, 3, 0, big, 1280), HH_MAC_ACCEPTED);
    ASSERT_RES(hh_mac_send(&mac, HH_FRAME_DATA, HH_MAC_CLASS_DATA, 3, 0, big, 1281), HH_MAC_TOO_LARGE);
    ASSERT_RES(hh_mac_set_max_payload(&mac, HH_MAC_PAYLOAD_MAX), HH_MAC_OK);
    ASSERT_RES(hh_mac_send(&mac, HH_FRAME_DATA, HH_MAC_CLASS_DATA, 3, 0, big, HH_MAC_PAYLOAD_MAX),
               HH_MAC_ACCEPTED);
    ASSERT_RES(hh_mac_set_max_payload(&mac, HH_MAC_PAYLOAD_MAX + 1), HH_MAC_INVALID_ARGUMENT);
    hh_mac_status(&mac, &st);
    HH_ASSERT_EQ_INT(st.max_payload, HH_MAC_PAYLOAD_MAX);
    HH_ASSERT_EQ_INT(st.refused_too_large, 1);
    HH_ASSERT_EQ_INT(count_events(HH_MAC_EV_PARAMETERS_CHANGED), 1);
    HH_ASSERT_EQ_INT(got[0].event.detail, HH_MAC_PAYLOAD_MAX);
}

/* The control token bucket holds control back over the cap and lets user frames go meanwhile. */
static void test_control_cap_pdus(void)
{
    hh_mac_config_t cfg;

    base_cfg(&cfg);
    cfg.ctl_cap_pdus_per_s = 2;
    cfg.ctl_burst_pdus = 2;
    setup_cfg(&cfg);
    for (uint8_t i = 1; i <= 3; i++) send_ctl(i);
    sendc(HH_MAC_CLASS_DATA, 10);
    HH_ASSERT_EQ_INT(next_tag(), 1);
    HH_ASSERT_EQ_INT(next_tag(), 2);
    HH_ASSERT_EQ_INT(next_tag(), 10);   /* control over the cap waits */
    HH_ASSERT_EQ_INT(next_tag(), 0xFF);
    t_now += 499;
    HH_ASSERT_EQ_INT(next_tag(), 0xFF);
    t_now += 1;
    HH_ASSERT_EQ_INT(next_tag(), 3);
}

/* The byte limit of the control cap counts frame lengths. */
static void test_control_cap_bytes(void)
{
    static uint8_t p[1000];
    hh_mac_config_t cfg;
    hh_mac_tx_frame_t f;

    base_cfg(&cfg);
    cfg.ctl_cap_bytes_per_s = 1000;
    cfg.ctl_burst_bytes = 1500;
    setup_cfg(&cfg);
    hh_mac_send(&mac, HH_FRAME_ROUTING, HH_MAC_CLASS_CONTROL, 0, 0, p, 1000);
    hh_mac_send(&mac, HH_FRAME_ROUTING, HH_MAC_CLASS_CONTROL, 0, 0, p, 1000);
    HH_ASSERT(hh_mac_next_pdu(&mac, &f));
    HH_ASSERT(!hh_mac_next_pdu(&mac, &f));
    t_now += 500;
    HH_ASSERT(hh_mac_next_pdu(&mac, &f));
}

/* Without a minimum share, strict priority lets control starve user traffic. */
static void test_strict_priority_when_share_off(void)
{
    setup();
    sendc(HH_MAC_CLASS_DATA, 100);
    for (uint8_t i = 1; i <= 10; i++) send_ctl(i);
    for (uint8_t i = 1; i <= 10; i++) HH_ASSERT_EQ_INT(next_tag(), i);
    HH_ASSERT_EQ_INT(next_tag(), 100);
}

/* With minimum share N, at least one own slot in N carries a user frame while one is queued. */
static void test_minimum_share(void)
{
    hh_mac_config_t cfg;

    base_cfg(&cfg);
    cfg.min_share_n = 3;
    setup_cfg(&cfg);
    for (uint8_t i = 1; i <= 6; i++) send_ctl(i);
    sendc(HH_MAC_CLASS_DATA, 100);
    sendc(HH_MAC_CLASS_VOICE, 50);
    HH_ASSERT_EQ_INT(next_tag(), 1);
    HH_ASSERT_EQ_INT(next_tag(), 2);
    HH_ASSERT_EQ_INT(next_tag(), 50);   /* highest user class first */
    HH_ASSERT_EQ_INT(next_tag(), 3);
    HH_ASSERT_EQ_INT(next_tag(), 4);
    HH_ASSERT_EQ_INT(next_tag(), 100);
    HH_ASSERT_EQ_INT(next_tag(), 5);
    HH_ASSERT_EQ_INT(next_tag(), 6);
}

/* Frames for this node, broadcast and any group are pushed up; other unicast only updates the record. */
static void test_receive_filter_and_push(void)
{
    hh_mac_link_report_t r;

    setup();
    rx_pdu(5, SELF, HH_MAC_CLASS_DATA, 1);
    rx_pdu(5, HH_MAC_BROADCAST, HH_MAC_CLASS_CONTROL, 2);
    rx_pdu(5, GROUP, HH_MAC_CLASS_VOICE, 3);
    rx_pdu(5, 9, HH_MAC_CLASS_DATA, 4);
    rx_pdu(5, HH_MAC_ID_RESERVED, HH_MAC_CLASS_DATA, 5);
    HH_ASSERT_EQ_INT(count_frames(), 3);
    HH_ASSERT_EQ_INT(mac.st.rx_not_addressed, 2);
    HH_ASSERT_EQ_INT(mac.st.frames_rx, 3);
    ASSERT_RES(hh_mac_link_metrics(&mac, 5, &r), HH_MAC_OK);
    HH_ASSERT_EQ_INT(r.rx_count, 5);
}

/* A delivered frame carries its fields and metrics unchanged and the payload byte-identical. */
static void test_received_frame_fields(void)
{
    const uint8_t p[4] = {9, 8, 7, 6};
    hh_mac_rx_pdu_t pdu;

    setup();
    memset(&pdu, 0, sizeof pdu);
    pdu.kind = HH_FRAME_DATA;
    pdu.cls = HH_MAC_CLASS_VIDEO;
    pdu.src = 5;
    pdu.dst = SELF;
    pdu.superframe = 1234;
    pdu.metrics.flags = HH_MAC_M_RSSI_VALID | HH_MAC_M_ARRIVAL_VALID;
    pdu.metrics.rssi_qdb = -240;    /* -60 dBm */
    pdu.metrics.arrival_offset_ns = -150;
    pdu.len = sizeof p;
    pdu.payload = p;
    hh_mac_on_rx(&mac, &pdu);

    HH_ASSERT_EQ_INT(count_frames(), 1);
    const hh_mac_rx_frame_t *f = &got[n_got - 1].frame;
    HH_ASSERT_EQ_INT(f->cls, HH_MAC_CLASS_VIDEO);
    HH_ASSERT_EQ_INT(f->src, 5);
    HH_ASSERT_EQ_INT(f->superframe, 1234);
    HH_ASSERT_EQ_INT(f->metrics.flags, HH_MAC_M_RSSI_VALID | HH_MAC_M_ARRIVAL_VALID);
    HH_ASSERT_EQ_INT(f->metrics.rssi_qdb, -240);
    HH_ASSERT_EQ_INT(f->metrics.arrival_offset_ns, -150);
    HH_ASSERT_EQ_INT(f->len, sizeof p);
    HH_ASSERT(memcmp(f->payload, p, sizeof p) == 0);
}

/* Received frames are delivered in LISTEN, ENTRY and LOST, but not in WAIT_TIME or SAFE (DP-9 (a)). */
static void test_delivery_by_state(void)
{
    const hh_mac_state_t yes[] = { HH_MAC_LISTEN, HH_MAC_ENTRY, HH_MAC_LOST, HH_MAC_HOLDOVER };
    const hh_mac_state_t no[] = { HH_MAC_WAIT_TIME, HH_MAC_SAFE };

    setup();
    for (unsigned i = 0; i < 4; i++) {
        hh_mac_set_state(&mac, yes[i]);
        rx_pdu(5, SELF, HH_MAC_CLASS_DATA, (uint8_t)i);
    }
    HH_ASSERT_EQ_INT(count_frames(), 4);
    for (unsigned i = 0; i < 2; i++) {
        hh_mac_set_state(&mac, no[i]);
        rx_pdu(5, SELF, HH_MAC_CLASS_DATA, 9);
    }
    HH_ASSERT_EQ_INT(count_frames(), 4);
    HH_ASSERT_EQ_INT(mac.st.rx_not_delivered_state, 2);
}

/* When the network layer refuses, frames wait per class, the oldest is dropped when full, and flush delivers in order received. */
static void test_holding_buffer(void)
{
    hh_mac_config_t cfg;
    hh_mac_status_t st;

    base_cfg(&cfg);
    cfg.hold_depth[HH_MAC_CLASS_DATA] = 2;
    setup_cfg(&cfg);
    refuse = true;
    rx_pdu(5, SELF, HH_MAC_CLASS_DATA, 1);
    rx_pdu(5, SELF, HH_MAC_CLASS_VOICE, 2);
    rx_pdu(5, SELF, HH_MAC_CLASS_DATA, 3);
    rx_pdu(5, SELF, HH_MAC_CLASS_DATA, 4);   /* data holding buffer full: 1 dropped */
    hh_mac_status(&mac, &st);
    HH_ASSERT_EQ_INT(st.hold_depth[HH_MAC_CLASS_DATA], 2);
    HH_ASSERT_EQ_INT(st.hold_overflow[HH_MAC_CLASS_DATA], 1);
    HH_ASSERT(st.delivery_refused > 0);

    refuse = false;
    n_got = 0;
    hh_mac_flush(&mac);
    HH_ASSERT_EQ_INT(n_got, 4);   /* NEIGHBOUR_HEARD event, then frames */
    HH_ASSERT_EQ_INT(got[0].event.kind, HH_MAC_EV_NEIGHBOUR_HEARD);
    HH_ASSERT_EQ_INT(got[1].frame.payload[0], 2);
    HH_ASSERT_EQ_INT(got[2].frame.payload[0], 3);
    HH_ASSERT_EQ_INT(got[3].frame.payload[0], 4);
}

/* NEIGHBOUR_HEARD comes once, before the first frame from that node. */
static void test_neighbour_heard(void)
{
    setup();
    rx_pdu(5, SELF, HH_MAC_CLASS_DATA, 1);
    rx_pdu(5, SELF, HH_MAC_CLASS_DATA, 2);
    rx_pdu(6, 9, HH_MAC_CLASS_DATA, 3);   /* not addressed, still heard */
    HH_ASSERT_EQ_INT(count_events(HH_MAC_EV_NEIGHBOUR_HEARD), 2);
    HH_ASSERT_EQ_INT(got[0].type, HH_MAC_IND_EVENT);
    HH_ASSERT_EQ_INT(got[0].event.node, 5);
    HH_ASSERT_EQ_INT(got[1].type, HH_MAC_IND_FRAME);
}

/* LINK_REPORT comes every report period for each neighbour, with unmeasured values not valid. */
static void test_link_report_period(void)
{
    hh_mac_config_t cfg;

    base_cfg(&cfg);
    cfg.report_period_sf = 2;
    setup_cfg(&cfg);
    rx_pdu(5, SELF, HH_MAC_CLASS_DATA, 1);
    rx_pdu(6, SELF, HH_MAC_CLASS_DATA, 1);
    n_got = 0;
    hh_mac_on_superframe(&mac, 100);
    HH_ASSERT_EQ_INT(count_events(HH_MAC_EV_LINK_REPORT), 0);
    hh_mac_on_superframe(&mac, 101);
    HH_ASSERT_EQ_INT(count_events(HH_MAC_EV_LINK_REPORT), 2);
    HH_ASSERT_EQ_INT(got[0].event.report.neighbor, 5);
    HH_ASSERT_EQ_INT(got[0].event.report.flags, 0);
    HH_ASSERT_EQ_INT(got[0].event.report.rx_count, 1);
    HH_ASSERT_EQ_INT(got[0].event.report.last_heard_superframe, 42);
    HH_ASSERT_EQ_INT(got[0].event.superframe, 101);
}

/* State, break-in, own slots and entry failure are reported as events. */
static void test_events(void)
{
    setup();
    hh_mac_set_state(&mac, HH_MAC_HOLDOVER);
    hh_mac_on_break_in(&mac, 12, true);
    hh_mac_on_break_in(&mac, 12, false);
    hh_mac_set_own_slots(&mac, 2);
    hh_mac_set_own_slots(&mac, 2);   /* no change, no event */
    hh_mac_set_own_slots(&mac, 1);
    hh_mac_on_entry_failed(&mac, HH_MAC_ENTRY_NOT_AUTHORISED);
    HH_ASSERT_EQ_INT(n_got, 6);
    HH_ASSERT_EQ_INT(got[0].event.kind, HH_MAC_EV_STATE_CHANGED);
    HH_ASSERT_EQ_INT(got[0].event.detail, HH_MAC_HOLDOVER);
    HH_ASSERT_EQ_INT(got[1].event.kind, HH_MAC_EV_BREAK_IN_ON);
    HH_ASSERT_EQ_INT(got[1].event.node, 12);
    HH_ASSERT_EQ_INT(got[2].event.kind, HH_MAC_EV_BREAK_IN_OFF);
    HH_ASSERT_EQ_INT(got[3].event.kind, HH_MAC_EV_OWN_SLOTS_CHANGED);
    HH_ASSERT_EQ_INT(got[4].event.detail, 1);
    HH_ASSERT_EQ_INT(got[5].event.kind, HH_MAC_EV_ENTRY_FAILED);
    HH_ASSERT_EQ_INT(got[5].event.detail, HH_MAC_ENTRY_NOT_AUTHORISED);
}

/* Status reports operational by state, time in holdover, restarts and logic counters. */
static void test_status(void)
{
    hh_mac_status_t st;

    setup();
    hh_mac_status(&mac, &st);
    HH_ASSERT(st.operational && st.operational_known);
    HH_ASSERT_EQ_INT(st.node_id, SELF);
    HH_ASSERT(!st.time_source_valid);
    HH_ASSERT(!st.own_slots_valid);

    hh_mac_set_time_source(&mac, HH_MAC_TIME_NETWORK);
    hh_mac_set_state(&mac, HH_MAC_HOLDOVER);
    t_now += 250;
    hh_mac_on_tx_underrun(&mac);
    hh_mac_on_crc_fail(&mac);
    hh_mac_status(&mac, &st);
    HH_ASSERT_EQ_INT(st.time_in_holdover_ms, 250);
    HH_ASSERT(st.time_source_valid);
    HH_ASSERT_EQ_INT(st.time_source, HH_MAC_TIME_NETWORK);
    HH_ASSERT_EQ_INT(st.tx_underrun, 1);
    HH_ASSERT_EQ_INT(st.rx_crc_fail, 1);

    hh_mac_set_state(&mac, HH_MAC_LISTEN);
    hh_mac_status(&mac, &st);
    HH_ASSERT(!st.operational_known);
    hh_mac_set_state(&mac, HH_MAC_SAFE);
    hh_mac_status(&mac, &st);
    HH_ASSERT(!st.operational && st.operational_known);

    hh_mac_on_reset(&mac);
    hh_mac_status(&mac, &st);
    HH_ASSERT_EQ_INT(st.restart_count, 1);
    HH_ASSERT_EQ_INT(st.state, HH_MAC_WAIT_TIME);
}

/* Link metrics returns NOT_FOUND for an unknown neighbour and NOT_OPEN when closed. */
static void test_link_metrics_results(void)
{
    hh_mac_link_report_t r;

    setup();
    ASSERT_RES(hh_mac_link_metrics(&mac, 5, &r), HH_MAC_NOT_FOUND);
    rx_pdu(5, SELF, HH_MAC_CLASS_DATA, 1);
    ASSERT_RES(hh_mac_link_metrics(&mac, 5, &r), HH_MAC_OK);
    HH_ASSERT_EQ_INT(r.flags, 0);
    hh_mac_close(&mac);
    ASSERT_RES(hh_mac_link_metrics(&mac, 5, &r), HH_MAC_NOT_OPEN);
}

/* Close discards and counts queued and held frames, is safe twice, and nothing is delivered after it. */
static void test_close(void)
{
    hh_mac_status_t st;

    setup();
    sendc(HH_MAC_CLASS_DATA, 1);
    sendc(HH_MAC_CLASS_VOICE, 2);
    refuse = true;
    rx_pdu(5, SELF, HH_MAC_CLASS_DATA, 3);
    ASSERT_RES(hh_mac_close(&mac), HH_MAC_OK);
    ASSERT_RES(hh_mac_close(&mac), HH_MAC_OK);
    HH_ASSERT_EQ_INT(mac.st.close_discards[HH_MAC_CLASS_DATA], 1);
    HH_ASSERT_EQ_INT(mac.st.close_discards[HH_MAC_CLASS_VOICE], 1);
    HH_ASSERT_EQ_INT(mac.st.hold_close_discards, 1);
    refuse = false;
    n_got = 0;
    hh_mac_flush(&mac);
    rx_pdu(5, SELF, HH_MAC_CLASS_DATA, 4);
    HH_ASSERT_EQ_INT(n_got, 0);
    ASSERT_RES(hh_mac_status(&mac, &st), HH_MAC_NOT_OPEN);
    ASSERT_RES(sendc(HH_MAC_CLASS_DATA, 5), HH_MAC_NOT_OPEN);
}

/* Net preset select is UNSUPPORTED until OI-4 closes. */
static void test_net_preset_unsupported(void)
{
    setup();
    ASSERT_RES(hh_mac_net_preset_select(&mac, 1), HH_MAC_UNSUPPORTED);
}

/* The neighbour table is bounded and counts what it cannot hold. */
static void test_neighbor_table_bounded(void)
{
    setup();
    for (uint32_t i = 0; i < HH_MAC_NEIGHBOR_CAP + 3; i++)
        rx_pdu(100 + i, 9, HH_MAC_CLASS_DATA, 0);
    HH_ASSERT_EQ_INT(mac.st.neighbor_table_full, 3);
}

/* The transmit pool never runs out when every queue is full. */
static void test_pool_holds_all_queues(void)
{
    hh_mac_config_t cfg;

    base_cfg(&cfg);
    cfg.queue_depth[HH_MAC_CLASS_DATA] = HH_MAC_TX_POOL_CAP - 16 - 8 - 8 - 32;
    setup_cfg(&cfg);
    for (int i = 0; i < cfg.queue_depth[HH_MAC_CLASS_CONTROL]; i++)
        ASSERT_RES(send_ctl(1), HH_MAC_ACCEPTED);
    for (int k = HH_MAC_CLASS_EMERGENCY; k < HH_MAC_CLASS_COUNT; k++)
        for (int i = 0; i < cfg.queue_depth[k]; i++)
            ASSERT_RES(sendc((hh_mac_class_t)k, 1), HH_MAC_ACCEPTED);
    HH_ASSERT_EQ_INT(mac.tx_free_count, 0);
    ASSERT_RES(sendc(HH_MAC_CLASS_DATA, 2), HH_MAC_TRY_AGAIN);
    ASSERT_RES(sendc(HH_MAC_CLASS_VOICE, 2), HH_MAC_ACCEPTED);   /* drop oldest still works */
}

/* NULL arguments do not crash. */
static void test_null_safety(void)
{
    hh_mac_tx_frame_t f;
    ASSERT_RES(hh_mac_init(NULL, NULL), HH_MAC_INVALID_ARGUMENT);
    ASSERT_RES(hh_mac_send(NULL, HH_FRAME_DATA, HH_MAC_CLASS_DATA, 1, 0, NULL, 0), HH_MAC_NOT_OPEN);
    ASSERT_RES(hh_mac_close(NULL), HH_MAC_OK);
    HH_ASSERT(!hh_mac_next_pdu(NULL, &f));
    hh_mac_on_rx(NULL, NULL);
    hh_mac_set_state(NULL, HH_MAC_STEADY);
    hh_mac_set_tx_inhibit(NULL, HH_MAC_INHIBIT_SULK);
    hh_mac_on_superframe(NULL, 1);
    hh_mac_flush(NULL);
}

HH_TEST_MAIN_BEGIN("mac")
    HH_RUN(test_config_defaults_and_limits);
    HH_RUN(test_open_and_send_states);
    HH_RUN(test_enqueue_dequeue_unchanged);
    HH_RUN(test_kind_and_class_rules);
    HH_RUN(test_class_order);
    HH_RUN(test_fifo_within_class);
    HH_RUN(test_try_again_classes_drop_nothing);
    HH_RUN(test_voice_video_drop_oldest);
    HH_RUN(test_full_class_does_not_block_others);
    HH_RUN(test_lifetime);
    HH_RUN(test_tx_inhibit);
    HH_RUN(test_max_payload);
    HH_RUN(test_control_cap_pdus);
    HH_RUN(test_control_cap_bytes);
    HH_RUN(test_strict_priority_when_share_off);
    HH_RUN(test_minimum_share);
    HH_RUN(test_receive_filter_and_push);
    HH_RUN(test_received_frame_fields);
    HH_RUN(test_delivery_by_state);
    HH_RUN(test_holding_buffer);
    HH_RUN(test_neighbour_heard);
    HH_RUN(test_link_report_period);
    HH_RUN(test_events);
    HH_RUN(test_status);
    HH_RUN(test_link_metrics_results);
    HH_RUN(test_close);
    HH_RUN(test_net_preset_unsupported);
    HH_RUN(test_neighbor_table_bounded);
    HH_RUN(test_pool_holds_all_queues);
    HH_RUN(test_null_safety);
HH_TEST_MAIN_END()
