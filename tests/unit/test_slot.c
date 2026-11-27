/*
 * Slot scheduler: PDUs leave only in own slots, frame and superframe
 * numbering, packing against slot capacity, control traffic counted against
 * capacity, late polls, receive path. Virtual clock, no sleeps.
 */
#include "hhsdr/mac/slot.h"
#include "hhsdr/core/log.h"
#include "hh_test.h"
#include <string.h>

#define NET 0x0010u
#define A   1u
#define B   2u
#define C   3u
#define EPOCH 1000u

static hh_mac_t  mac_a, mac_b;
static hh_slot_t slot_a, slot_b;

static hh_time_ms_t t_now;
/* Virtual clock callback. */
static hh_time_ms_t vnow(void *ctx) { (void)ctx; return t_now; }
static const hh_clock_t vclock = { vnow, NULL };

typedef struct {
    hh_slot_pos_t pos;
    size_t        len;
    uint8_t       bytes[256];
} sent_t;

static sent_t sent[256];
static int    n_sent;
static bool   forward_to_b;   /* deliver A's PDUs to B's receive path */

/* Transmit function of node A: records the PDU and optionally delivers it to node B. */
static bool a_tx(void *ctx, const hh_slot_pos_t *pos, const uint8_t *pdu, size_t len)
{
    (void)ctx;
    if (n_sent < 256 && len <= sizeof sent[0].bytes) {
        sent[n_sent].pos = *pos;
        sent[n_sent].len = len;
        memcpy(sent[n_sent].bytes, pdu, len);
        n_sent++;
    }
    if (forward_to_b) hh_slot_receive(&slot_b, pos, pdu, len);
    return true;
}

/* Transmit function of node B: discards the PDU. */
static bool b_tx(void *ctx, const hh_slot_pos_t *pos, const uint8_t *pdu, size_t len)
{
    (void)ctx; (void)pos; (void)pdu; (void)len;
    return true;
}

static hh_mac_rx_frame_t got[32];
static int n_got;

/* Network layer receive callback of node B: records delivered frames. */
static bool b_receiver(void *ctx, const hh_mac_indication_t *ind)
{
    (void)ctx;
    if (ind->type == HH_MAC_IND_FRAME && n_got < 32) got[n_got++] = *ind->frame;
    return true;
}

/* Initialises a MAC and brings it to the steady state. */
static void open_mac(hh_mac_t *mac, hh_node_id_t id, hh_mac_receive_fn rx)
{
    hh_mac_config_t cfg;
    hh_mac_config_default(&cfg, id, &vclock);
    cfg.receive = rx;
    cfg.report_period_sf = 0;
    hh_mac_init(mac, &cfg);
    hh_mac_open(mac);
    hh_mac_set_state(mac, HH_MAC_STEADY);
}

/* Sets up one node: a MAC plus a scheduler with the given own slots and capacity. */
static hh_status_t setup_node(hh_slot_t *s, hh_mac_t *mac, hh_node_id_t id, uint16_t net,
                              uint64_t own, uint16_t capacity, hh_slot_tx_fn tx,
                              hh_mac_receive_fn rx)
{
    hh_slot_config_t c;
    open_mac(mac, id, rx);
    hh_slot_config_default(&c, id, net);
    c.own_slots = own;
    c.capacity = capacity;
    c.epoch_ms = EPOCH;
    return hh_slot_init(s, &c, &vclock, mac, tx, NULL);
}

/* Resets the virtual clock and the capture buffers before a test. */
static void reset(void)
{
    hh_log_set_level(HH_LOG_ERROR);
    t_now = EPOCH;
    n_sent = 0;
    n_got = 0;
    forward_to_b = false;
}

/* Polls the scheduler once per ms for ms milliseconds. */
static void run_ms(hh_slot_t *s, unsigned ms)
{
    for (unsigned i = 0; i < ms; i++) {
        hh_slot_poll(s);
        t_now++;
    }
}

/* Sends a packet of len bytes through the MAC. */
static hh_mac_result_t send_n(hh_mac_t *mac, hh_frame_kind_t k, hh_mac_class_t c, hh_node_id_t dst,
                              uint16_t len)
{
    uint8_t buf[256];
    memset(buf, 0xA5, sizeof buf);
    return hh_mac_send(mac, k, c, dst, 0, buf, len);
}

/* Invalid configurations are refused. */
static void test_init_validation(void)
{
    hh_slot_config_t c;
    reset();
    open_mac(&mac_a, A, NULL);
    hh_slot_config_default(&c, A, NET);
    c.own_slots = 1;
    HH_ASSERT_OK(hh_slot_init(&slot_a, &c, &vclock, &mac_a, a_tx, NULL));

    c.slots_per_frame = 65;
    HH_ASSERT_ERR(hh_slot_init(&slot_a, &c, &vclock, &mac_a, a_tx, NULL), HH_ERR_INVAL);
    hh_slot_config_default(&c, A, NET);
    c.own_slots = 1ull << 20;   /* slot 20 does not exist in a 20-slot frame */
    HH_ASSERT_ERR(hh_slot_init(&slot_a, &c, &vclock, &mac_a, a_tx, NULL), HH_ERR_INVAL);
    hh_slot_config_default(&c, A, NET);
    c.capacity = 20;   /* smaller than the PDU overhead */
    HH_ASSERT_ERR(hh_slot_init(&slot_a, &c, &vclock, &mac_a, a_tx, NULL), HH_ERR_INVAL);
    hh_slot_config_default(&c, 0, NET);
    HH_ASSERT_ERR(hh_slot_init(&slot_a, &c, &vclock, &mac_a, a_tx, NULL), HH_ERR_INVAL);
    hh_slot_config_default(&c, A, 0xFFFF);
    HH_ASSERT_ERR(hh_slot_init(&slot_a, &c, &vclock, &mac_a, a_tx, NULL), HH_ERR_INVAL);
    hh_slot_config_default(&c, A, NET);
    c.slot_ms = 0;
    HH_ASSERT_ERR(hh_slot_init(&slot_a, &c, &vclock, &mac_a, a_tx, NULL), HH_ERR_INVAL);
}

/* PDUs leave only in own slots, one per slot, even when empty. */
static void test_sends_only_in_own_slots(void)
{
    hh_pdu_t p;
    uint64_t last = 0;
    reset();
    HH_ASSERT_OK(setup_node(&slot_a, &mac_a, A, NET, (1ull << 5) | (1ull << 9), 128, a_tx, NULL));

    run_ms(&slot_a, 60);   /* three frames */
    HH_ASSERT_EQ_INT(n_sent, 6);
    for (int i = 0; i < n_sent; i++) {
        HH_ASSERT(sent[i].pos.slot == 5 || sent[i].pos.slot == 9);
        HH_ASSERT(i == 0 || sent[i].pos.abs_slot > last);
        last = sent[i].pos.abs_slot;
        /* One PDU per own slot, even with nothing queued; claims name the slots. */
        HH_ASSERT_OK(hh_pdu_decode(sent[i].bytes, sent[i].len, &p));
        HH_ASSERT_EQ_INT(p.n_msg, 0);
        HH_ASSERT_EQ_INT(p.slot_bytes, 3);
        HH_ASSERT_EQ_INT(p.claims[0], 0x20);
        HH_ASSERT_EQ_INT(p.claims[1], 0x02);
        HH_ASSERT_EQ_INT(p.seq, i);
    }
    HH_ASSERT_EQ_INT(slot_a.st.own_slots, 6);
    HH_ASSERT_EQ_INT(slot_a.st.pdus_empty, 6);
    HH_ASSERT_EQ_INT(slot_a.st.slots_seen, 60);
}

/* Slot, frame and superframe numbers are correct across boundaries. */
static void test_frame_and_superframe_numbers(void)
{
    reset();
    HH_ASSERT_OK(setup_node(&slot_a, &mac_a, A, NET, 1, 128, a_tx, NULL));
    t_now = EPOCH + 1000;   /* first poll lands on slot 1000 = superframe 1 */
    run_ms(&slot_a, 1001);
    HH_ASSERT_EQ_INT(n_sent, 51);   /* slots 1000, 1020, ..., 2000 */
    HH_ASSERT_EQ_INT(sent[0].pos.superframe, 1);
    HH_ASSERT_EQ_INT(sent[0].pos.frame, 0);
    HH_ASSERT_EQ_INT(sent[0].pos.slot, 0);
    HH_ASSERT_EQ_INT(sent[1].pos.frame, 1);
    HH_ASSERT_EQ_INT(sent[49].pos.superframe, 1);
    HH_ASSERT_EQ_INT(sent[49].pos.frame, 49);
    HH_ASSERT_EQ_INT(sent[50].pos.superframe, 2);
    HH_ASSERT_EQ_INT(sent[50].pos.frame, 0);
    HH_ASSERT_EQ_INT(sent[50].pos.abs_slot, 2000);
}

/* A frame with different slot count, frame count and slot length works. */
static void test_adjustable_frame(void)
{
    hh_slot_config_t c;
    reset();
    open_mac(&mac_a, A, NULL);
    hh_slot_config_default(&c, A, NET);
    c.slots_per_frame = 8;
    c.frames_per_superframe = 4;
    c.slot_ms = 2;
    c.own_slots = 1u << 3;
    c.epoch_ms = EPOCH;
    HH_ASSERT_OK(hh_slot_init(&slot_a, &c, &vclock, &mac_a, a_tx, NULL));
    run_ms(&slot_a, 8 * 2 * 5);   /* five frames of 16 ms */
    HH_ASSERT_EQ_INT(n_sent, 5);
    HH_ASSERT_EQ_INT(sent[0].pos.slot, 3);
    HH_ASSERT_EQ_INT(sent[4].pos.superframe, 1);   /* frame 4 is superframe 1, frame 0 */
    HH_ASSERT_EQ_INT(sent[4].pos.frame, 0);
    {
        hh_pdu_t p;
        HH_ASSERT_OK(hh_pdu_decode(sent[0].bytes, sent[0].len, &p));
        HH_ASSERT_EQ_INT(p.slot_bytes, 1);   /* 8 slots need one bitmap byte */
        HH_ASSERT_EQ_INT(p.claims[0], 0x08);
    }
}

/* Messages are packed up to the capacity, control first, and the rest wait. */
static void test_packing_and_order(void)
{
    hh_pdu_t p;
    reset();
    /* Capacity 105: 27 bytes of overhead (queue report included) leave 78, so three 20-byte messages (25 each). */
    HH_ASSERT_OK(setup_node(&slot_a, &mac_a, A, NET, 1, 105, a_tx, NULL));
    for (int i = 0; i < 5; i++) HH_ASSERT_EQ_INT(send_n(&mac_a, HH_FRAME_DATA, HH_MAC_CLASS_DATA, B, 20), HH_MAC_ACCEPTED);
    HH_ASSERT_EQ_INT(send_n(&mac_a, HH_FRAME_ROUTING, HH_MAC_CLASS_CONTROL, HH_MAC_BROADCAST, 20), HH_MAC_ACCEPTED);

    run_ms(&slot_a, 41);   /* slots 0, 20, 40 */
    HH_ASSERT_EQ_INT(n_sent, 3);
    HH_ASSERT_OK(hh_pdu_decode(sent[0].bytes, sent[0].len, &p));
    HH_ASSERT_EQ_INT(p.n_msg, 3);
    HH_ASSERT_EQ_INT(p.msg[0].proto, HH_PDU_PROTO_ROUTING);   /* control first */
    HH_ASSERT_EQ_INT(p.msg[0].cls, 0);
    HH_ASSERT_EQ_INT(p.msg[0].dst, HH_PDU_ADDR_BROADCAST);
    HH_ASSERT_EQ_INT(p.msg[1].cls, 4);
    HH_ASSERT(!p.more_data);   /* never set: the PL CRC accepts 0xA841 only */
    HH_ASSERT(sent[0].len <= 105);
    HH_ASSERT_OK(hh_pdu_decode(sent[1].bytes, sent[1].len, &p));
    HH_ASSERT_EQ_INT(p.n_msg, 3);
    HH_ASSERT(!p.more_data);
    HH_ASSERT_OK(hh_pdu_decode(sent[2].bytes, sent[2].len, &p));
    HH_ASSERT_EQ_INT(p.n_msg, 0);
}

/* The MAC maximum payload follows the slot capacity. */
static void test_max_payload_follows_capacity(void)
{
    hh_mac_status_t st;
    reset();
    HH_ASSERT_OK(setup_node(&slot_a, &mac_a, A, NET, 1, 105, a_tx, NULL));
    HH_ASSERT_EQ_INT(hh_mac_status(&mac_a, &st), HH_MAC_OK);
    HH_ASSERT_EQ_INT(st.max_payload, 73);   /* 105 - 27 overhead - 5 sub-header */
    HH_ASSERT_EQ_INT(send_n(&mac_a, HH_FRAME_DATA, HH_MAC_CLASS_DATA, B, 73), HH_MAC_ACCEPTED);
    HH_ASSERT_EQ_INT(send_n(&mac_a, HH_FRAME_DATA, HH_MAC_CLASS_DATA, B, 74), HH_MAC_TOO_LARGE);
    run_ms(&slot_a, 1);
    HH_ASSERT_EQ_INT(slot_a.st.messages_tx, 1);   /* the largest allowed frame does fit */
}

/* Routing and data share one capacity and are counted separately. */
static void test_control_counts_against_capacity(void)
{
    reset();
    HH_ASSERT_OK(setup_node(&slot_a, &mac_a, A, NET, 1, 100, a_tx, NULL));
    send_n(&mac_a, HH_FRAME_ROUTING, HH_MAC_CLASS_CONTROL, HH_MAC_BROADCAST, 30);
    send_n(&mac_a, HH_FRAME_DATA, HH_MAC_CLASS_DATA, B, 30);
    run_ms(&slot_a, 1);
    HH_ASSERT_EQ_INT(slot_a.st.control_bytes, 35);
    HH_ASSERT_EQ_INT(slot_a.st.routing_bytes, 35);
    HH_ASSERT_EQ_INT(slot_a.st.slots_with_routing, 1);
    HH_ASSERT_EQ_INT(slot_a.st.slots_with_control, 1);
    HH_ASSERT_EQ_INT(slot_a.st.user_bytes, 35);
    HH_ASSERT_EQ_INT(slot_a.st.overhead_bytes, 27);
    HH_ASSERT_EQ_INT(slot_a.st.pdu_bytes, 97);
    HH_ASSERT_EQ_INT(slot_a.st.capacity_bytes, 100);
    HH_ASSERT_EQ_INT(hh_slot_control_share_permille(&slot_a.st), 350);
    /* Control and user bytes share one capacity: a second control frame that
     * no longer fits waits for the next own slot. */
    send_n(&mac_a, HH_FRAME_ROUTING, HH_MAC_CLASS_CONTROL, HH_MAC_BROADCAST, 40);
    send_n(&mac_a, HH_FRAME_ROUTING, HH_MAC_CLASS_CONTROL, HH_MAC_BROADCAST, 40);
    run_ms(&slot_a, 20);
    HH_ASSERT_EQ_INT(slot_a.st.messages_tx, 3);   /* one 45-byte message fits beside 27 bytes of overhead, not two */
}

/* A beacon counts as control traffic but not as routing. */
static void test_beacon_is_control_not_routing(void)
{
    reset();
    HH_ASSERT_OK(setup_node(&slot_a, &mac_a, A, NET, 1, 100, a_tx, NULL));
    send_n(&mac_a, HH_FRAME_BEACON, HH_MAC_CLASS_DATA, HH_MAC_BROADCAST, 30);   /* forced to class 0 */
    run_ms(&slot_a, 1);
    HH_ASSERT_EQ_INT(slot_a.st.control_bytes, 35);
    HH_ASSERT_EQ_INT(slot_a.st.routing_bytes, 0);
    HH_ASSERT_EQ_INT(slot_a.st.slots_with_control, 1);
    HH_ASSERT_EQ_INT(slot_a.st.slots_with_routing, 0);
}

/* An own slot that passed before the poll is counted as missed and not sent. */
static void test_late_poll_misses_own_slot(void)
{
    reset();
    HH_ASSERT_OK(setup_node(&slot_a, &mac_a, A, NET, 1, 128, a_tx, NULL));
    hh_slot_poll(&slot_a);   /* slot 0 */
    t_now += 45;             /* slots 20 and 40 pass without a poll */
    hh_slot_poll(&slot_a);
    HH_ASSERT_EQ_INT(n_sent, 1);
    HH_ASSERT_EQ_INT(slot_a.st.own_slots, 3);
    HH_ASSERT_EQ_INT(slot_a.st.own_slots_missed, 2);
}

/* Nothing happens before the start of the first superframe. */
static void test_clock_before_epoch(void)
{
    reset();
    HH_ASSERT_OK(setup_node(&slot_a, &mac_a, A, NET, 1, 128, a_tx, NULL));
    t_now = EPOCH - 5;
    hh_slot_poll(&slot_a);
    HH_ASSERT_EQ_INT(n_sent, 0);
    HH_ASSERT_EQ_INT(slot_a.st.slots_seen, 0);
}

/* Received PDUs reach the MAC, and a control-only PDU counts as hearing the neighbour. */
static void test_receive_path(void)
{
    hh_mac_link_report_t rep;
    reset();
    HH_ASSERT_OK(setup_node(&slot_a, &mac_a, A, NET, 1, 128, a_tx, NULL));
    HH_ASSERT_OK(setup_node(&slot_b, &mac_b, B, NET, 1u << 7, 128, b_tx, b_receiver));
    forward_to_b = true;

    /* A control-only PDU still counts as hearing the neighbour. */
    HH_ASSERT_EQ_INT(hh_mac_link_metrics(&mac_b, A, &rep), HH_MAC_NOT_FOUND);
    run_ms(&slot_a, 1);
    HH_ASSERT_EQ_INT(hh_mac_link_metrics(&mac_b, A, &rep), HH_MAC_OK);
    HH_ASSERT_EQ_INT(n_got, 0);

    send_n(&mac_a, HH_FRAME_DATA, HH_MAC_CLASS_DATA, B, 20);
    send_n(&mac_a, HH_FRAME_ROUTING, HH_MAC_CLASS_CONTROL, HH_MAC_BROADCAST, 12);
    send_n(&mac_a, HH_FRAME_DATA, HH_MAC_CLASS_DATA, C, 20);   /* for another node */
    run_ms(&slot_a, 20);
    HH_ASSERT_EQ_INT(n_got, 2);
    HH_ASSERT_EQ_INT(got[0].kind, HH_FRAME_ROUTING);
    HH_ASSERT_EQ_INT(got[0].src, A);
    HH_ASSERT_EQ_INT(got[0].len, 12);
    HH_ASSERT_EQ_INT(got[1].kind, HH_FRAME_DATA);
    HH_ASSERT_EQ_INT(got[1].len, 20);
    HH_ASSERT_EQ_INT(got[1].payload[0], 0xA5);
    HH_ASSERT_EQ_INT(slot_b.st.rx_pdus, 2);
}

/* An own-slot PDU with no traffic still carries a zero queue report. */
static void test_queue_report_empty(void)
{
    hh_pdu_t p;
    reset();
    HH_ASSERT_OK(setup_node(&slot_a, &mac_a, A, NET, 1, 100, a_tx, NULL));
    run_ms(&slot_a, 1);
    HH_ASSERT_EQ_INT(n_sent, 1);
    HH_ASSERT_OK(hh_pdu_decode(sent[0].bytes, sent[0].len, &p));
    HH_ASSERT(p.has_qreport);
    for (int k = 0; k < 5; k++) HH_ASSERT_EQ_INT(p.qreport[k], 0);
    HH_ASSERT_EQ_INT((int)sent[0].len, 27);   /* 22 bytes of overhead plus the 5-byte report */
    HH_ASSERT_EQ_INT(slot_a.st.reports_tx, 1);
}

/* The report holds the messages offered in the frame, per class: those sent so far plus those still queued. */
static void test_queue_report_is_offered_load(void)
{
    hh_pdu_t p;
    reset();
    HH_ASSERT_OK(setup_node(&slot_a, &mac_a, A, NET, 1, 100, a_tx, NULL));
    send_n(&mac_a, HH_FRAME_DATA, HH_MAC_CLASS_DATA, B, 20);
    send_n(&mac_a, HH_FRAME_DATA, HH_MAC_CLASS_DATA, B, 20);
    send_n(&mac_a, HH_FRAME_DATA, HH_MAC_CLASS_DATA, B, 20);
    send_n(&mac_a, HH_FRAME_DATA, HH_MAC_CLASS_VOICE, B, 20);
    run_ms(&slot_a, 1);   /* room for two 25-byte messages: voice first, then one data */
    HH_ASSERT_OK(hh_pdu_decode(sent[0].bytes, sent[0].len, &p));
    HH_ASSERT_EQ_INT(p.n_msg, 2);
    HH_ASSERT_EQ_INT(p.qreport[HH_MAC_CLASS_VOICE], 1);   /* one sent, none left */
    HH_ASSERT_EQ_INT(p.qreport[HH_MAC_CLASS_DATA], 3);    /* one sent, two left */
    HH_ASSERT(!p.more_data);   /* the queue report carries the backlog, not the bit */
}

/* Queues two 45-byte messages before every frame, so one fits each own slot. Returns the PDUs sent. */
static int run_frames_with_two_messages(unsigned frames)
{
    for (unsigned f = 0; f < frames; f++) {
        send_n(&mac_a, HH_FRAME_DATA, HH_MAC_CLASS_DATA, B, 40);
        send_n(&mac_a, HH_FRAME_DATA, HH_MAC_CLASS_DATA, B, 40);
        run_ms(&slot_a, 20);
    }
    return n_sent;
}

/* A node that carries all it is offered reports the same load from every own slot, whatever is left in its queue. */
static void test_offered_load_is_the_same_from_every_slot(void)
{
    hh_pdu_t p;
    reset();
    HH_ASSERT_OK(setup_node(&slot_a, &mac_a, A, NET, (1u << 0) | (1u << 5) | (1u << 10) | (1u << 15), 100, a_tx, NULL));
    HH_ASSERT_EQ_INT(run_frames_with_two_messages(4), 16);
    for (int i = 0; i < 16; i++) {
        HH_ASSERT_OK(hh_pdu_decode(sent[i].bytes, sent[i].len, &p));
        HH_ASSERT_EQ_INT(p.qreport[HH_MAC_CLASS_DATA], 2);   /* also in slots 10 and 15, where nothing is left */
    }
}

/* With too few slots the reported load rises every frame, because the backlog grows. */
static void test_offered_load_rises_when_slots_are_short(void)
{
    hh_pdu_t p;
    reset();
    HH_ASSERT_OK(setup_node(&slot_a, &mac_a, A, NET, 1, 100, a_tx, NULL));
    HH_ASSERT_EQ_INT(run_frames_with_two_messages(4), 4);
    for (int i = 0; i < 4; i++) {
        HH_ASSERT_OK(hh_pdu_decode(sent[i].bytes, sent[i].len, &p));
        HH_ASSERT_EQ_INT(p.qreport[HH_MAC_CLASS_DATA], i + 2);
    }
}

/* Frames in which the node sent nothing (late polls) leave no load behind. */
static void test_offered_load_forgets_silent_frames(void)
{
    hh_pdu_t p;
    reset();
    HH_ASSERT_OK(setup_node(&slot_a, &mac_a, A, NET, 1, 100, a_tx, NULL));
    send_n(&mac_a, HH_FRAME_DATA, HH_MAC_CLASS_DATA, B, 40);
    hh_slot_poll(&slot_a);   /* frame 0: one message sent */
    t_now += 80;             /* frames 1 to 3 pass without a poll, their own slots are missed */
    hh_slot_poll(&slot_a);   /* frame 4 */
    HH_ASSERT_EQ_INT(n_sent, 2);
    HH_ASSERT_OK(hh_pdu_decode(sent[1].bytes, sent[1].len, &p));
    HH_ASSERT_EQ_INT(p.qreport[HH_MAC_CLASS_DATA], 0);
}

/* A heard queue report is stored with the sender and the slot it was sent in. */
static void test_queue_report_stored_on_receive(void)
{
    const hh_alloc_demand_t *d;
    reset();
    HH_ASSERT_OK(setup_node(&slot_a, &mac_a, A, NET, 1, 100, a_tx, NULL));
    HH_ASSERT_OK(setup_node(&slot_b, &mac_b, B, NET, 1u << 7, 100, b_tx, b_receiver));
    forward_to_b = true;
    send_n(&mac_a, HH_FRAME_DATA, HH_MAC_CLASS_DATA, C, 20);
    send_n(&mac_a, HH_FRAME_DATA, HH_MAC_CLASS_DATA, C, 20);
    send_n(&mac_a, HH_FRAME_DATA, HH_MAC_CLASS_DATA, C, 20);
    run_ms(&slot_a, 21);   /* slot 0 sends two frames; slot 20 sends the third */
    d = hh_alloc_find(&slot_b.demand, A);
    HH_ASSERT(d != NULL);
    HH_ASSERT_EQ_INT((int)d->seen_slot, 20);   /* the later report replaced the earlier one */
    HH_ASSERT_EQ_INT(d->depth[HH_MAC_CLASS_DATA], 2);   /* two sent in the first frame, none left */
    HH_ASSERT_EQ_INT(slot_b.st.reports_rx, 2);
    HH_ASSERT_EQ_INT(slot_b.st.demand_dropped, 0);
    HH_ASSERT(hh_alloc_find(&slot_a.demand, A) != NULL);   /* a node keeps its own last report */
    HH_ASSERT(hh_alloc_find(&slot_a.demand, B) == NULL);   /* and has none for a node it never heard */
}

/* Builds a control-only PDU from a node that claims the given slot bits. */
static size_t claim_pdu(uint16_t src, uint8_t claim_byte, uint8_t *buf, size_t cap)
{
    hh_pdu_t p;
    size_t len = 0;
    memset(&p, 0, sizeof p);
    p.net_id = NET;
    p.src = src;
    p.time_quality = 0xFC;
    p.slot_bytes = 3;
    p.claims[0] = claim_byte;
    if (hh_pdu_encode(&p, buf, cap, &len) != HH_OK) return 0;   /* the caller's receive then does nothing and its checks fail */
    return len;
}


/* Allocation needs usable parameters, and takes the entry slots out of the map. */
static void test_alloc_enable(void)
{
    hh_alloc_params_t ap;
    reset();
    HH_ASSERT_OK(setup_node(&slot_a, &mac_a, A, NET, 1, 100, a_tx, NULL));
    hh_alloc_params_default(&ap, 20);
    HH_ASSERT_ERR(hh_slot_enable_alloc(NULL, &ap), HH_ERR_INVAL);
    HH_ASSERT_ERR(hh_slot_enable_alloc(&slot_a, NULL), HH_ERR_INVAL);
    ap.weight_video = 0;
    ap.weight_data = 0;
    HH_ASSERT_ERR(hh_slot_enable_alloc(&slot_a, &ap), HH_ERR_INVAL);
    hh_alloc_params_default(&ap, 20);
    HH_ASSERT_OK(hh_slot_enable_alloc(&slot_a, &ap));
    HH_ASSERT(slot_a.alloc_on);
    HH_ASSERT_EQ_INT(slot_a.alloc_params.slots_per_frame, 20);
}

/* The voice admission query needs allocation, and says no once other nodes' calls use the reserve. */
static void test_voice_admit_query(void)
{
    hh_alloc_params_t ap;
    uint8_t others_calls[HH_ALLOC_CLASSES] = { 0, 0, 10, 0, 0 };
    bool ok = true;

    reset();
    HH_ASSERT_OK(setup_node(&slot_a, &mac_a, A, NET, 1, 100, a_tx, NULL));
    HH_ASSERT_ERR(hh_slot_voice_admit(&slot_a, 1, &ok), HH_ERR_STATE);   /* allocation is off */
    hh_alloc_params_default(&ap, 20);
    HH_ASSERT_OK(hh_slot_enable_alloc(&slot_a, &ap));
    HH_ASSERT_ERR(hh_slot_voice_admit(&slot_a, 0, &ok), HH_ERR_INVAL);
    HH_ASSERT_ERR(hh_slot_voice_admit(&slot_a, 1, NULL), HH_ERR_INVAL);
    HH_ASSERT_OK(hh_slot_voice_admit(&slot_a, 1, &ok));
    HH_ASSERT(ok);
    HH_ASSERT_OK(hh_alloc_note(&slot_a.demand, 7, others_calls, 0));   /* node 7 offers ten voice messages a frame */
    HH_ASSERT_OK(hh_slot_voice_admit(&slot_a, 1, &ok));
    HH_ASSERT(!ok);
}

/* At a superframe boundary a node silent for longer than the gone limit is dropped from the demand table. */
static void test_alloc_evicts_gone_node(void)
{
    hh_alloc_params_t ap;
    uint8_t r[HH_ALLOC_CLASSES] = { 0, 0, 0, 0, 3 };

    reset();
    HH_ASSERT_OK(setup_node(&slot_a, &mac_a, A, NET, 1, 100, a_tx, NULL));
    hh_alloc_params_default(&ap, 20);
    ap.gone_slots = 500;
    HH_ASSERT_OK(hh_slot_enable_alloc(&slot_a, &ap));
    HH_ASSERT_OK(hh_alloc_note(&slot_a.demand, 7, r, 0));
    run_ms(&slot_a, 400);
    HH_ASSERT(hh_alloc_find(&slot_a.demand, 7) != NULL);   /* no boundary since, and not yet gone */
    run_ms(&slot_a, 700);                                   /* the boundary at slot 1000: age 1000 */
    HH_ASSERT(hh_alloc_find(&slot_a.demand, 7) == NULL);
}

/* With demand that does not change, the node's slots stop changing after the first maps. */
static void test_alloc_positions_stay_put(void)
{
    hh_alloc_params_t ap;
    uint8_t r[HH_ALLOC_CLASSES] = { 0, 0, 0, 0, 3 };
    uint64_t changes;

    reset();
    HH_ASSERT_OK(setup_node(&slot_a, &mac_a, A, NET, 1, 100, a_tx, NULL));
    hh_alloc_params_default(&ap, 20);
    HH_ASSERT_OK(hh_slot_enable_alloc(&slot_a, &ap));
    HH_ASSERT_OK(hh_alloc_note(&slot_a.demand, 7, r, 0));
    run_ms(&slot_a, 2500);
    changes = slot_a.st.alloc_changes;
    HH_ASSERT(slot_a.st.alloc_runs >= 3);
    run_ms(&slot_a, 2000);                                  /* more maps, same demand */
    HH_ASSERT(slot_a.st.alloc_runs >= 5);
    HH_ASSERT_EQ_INT((int)slot_a.st.alloc_changes, (int)changes);
}

/* A lower node id that claims one of this node's slots keeps it; a higher id does not. */
static void test_alloc_clash_lower_id_keeps_slot(void)
{
    hh_alloc_params_t ap;
    hh_slot_pos_t pos = { 50, 0, 2, 10 };
    uint8_t buf[64];
    size_t len;

    reset();
    HH_ASSERT_OK(setup_node(&slot_b, &mac_b, B, NET, 1u << 3, 100, b_tx, b_receiver));
    hh_alloc_params_default(&ap, 20);
    HH_ASSERT_OK(hh_slot_enable_alloc(&slot_b, &ap));

    len = claim_pdu(3, 0x08, buf, sizeof buf);   /* node 3 is above node 2 in id: node 2 keeps the slot */
    hh_slot_receive(&slot_b, &pos, buf, len);
    HH_ASSERT_EQ_INT((int)slot_b.cfg.own_slots, 8);
    HH_ASSERT_EQ_INT(slot_b.st.alloc_clashes, 0);

    len = claim_pdu(1, 0x08, buf, sizeof buf);   /* node 1 is below: node 2 gives the slot up */
    hh_slot_receive(&slot_b, &pos, buf, len);
    HH_ASSERT_EQ_INT((int)slot_b.cfg.own_slots, 0);
    HH_ASSERT_EQ_INT(slot_b.st.alloc_clashes, 1);
}

/* With logic_adds_crc the scheduler sends and accepts PDUs without the CRC-32. */
static void test_logic_adds_crc(void)
{
    hh_pdu_t p;
    size_t with_crc;

    reset();
    HH_ASSERT_OK(setup_node(&slot_a, &mac_a, A, NET, 1, 128, a_tx, NULL));
    run_ms(&slot_a, 1);                                   /* a control-only PDU, CRC-32 included */
    HH_ASSERT_EQ_INT(n_sent, 1);
    with_crc = sent[0].len;
    HH_ASSERT_OK(hh_pdu_decode(sent[0].bytes, sent[0].len, &p));

    reset();
    HH_ASSERT_OK(setup_node(&slot_a, &mac_a, A, NET, 1, 128, a_tx, NULL));
    slot_a.cfg.logic_adds_crc = true;
    HH_ASSERT_OK(setup_node(&slot_b, &mac_b, B, NET, 1u << 7, 128, b_tx, b_receiver));
    slot_b.cfg.logic_adds_crc = true;
    forward_to_b = true;
    send_n(&mac_a, HH_FRAME_DATA, HH_MAC_CLASS_DATA, B, 20);
    run_ms(&slot_a, 1);
    HH_ASSERT_EQ_INT(n_sent, 1);
    HH_ASSERT_OK(hh_pdu_decode_nocrc(sent[0].bytes, sent[0].len, &p));
    HH_ASSERT_ERR(hh_pdu_decode(sent[0].bytes, sent[0].len, &p), HH_ERR_INVAL);
    HH_ASSERT_EQ_INT(slot_b.st.rx_pdus, 1);               /* B accepted it without a CRC-32 */
    HH_ASSERT_EQ_INT(n_got, 1);
    HH_ASSERT_EQ_INT(got[0].len, 20);

    /* The same capacity applies: the PL frame is the PDU plus the CRC-32. */
    reset();
    HH_ASSERT_OK(setup_node(&slot_a, &mac_a, A, NET, 1, 128, a_tx, NULL));
    slot_a.cfg.logic_adds_crc = true;
    run_ms(&slot_a, 1);
    HH_ASSERT_EQ_INT(sent[0].len + HH_PDU_CRC_LEN, with_crc);
}

/* Wrong net and corrupted PDUs are dropped and counted. */
static void test_receive_drops(void)
{
    hh_slot_pos_t pos = { 0, 0, 0, 0 };
    uint8_t copy[256];
    reset();
    HH_ASSERT_OK(setup_node(&slot_a, &mac_a, A, NET, 1, 128, a_tx, NULL));
    HH_ASSERT_OK(setup_node(&slot_b, &mac_b, B, 0x0011, 1u << 7, 128, b_tx, b_receiver));
    send_n(&mac_a, HH_FRAME_DATA, HH_MAC_CLASS_DATA, B, 20);
    run_ms(&slot_a, 1);

    hh_slot_receive(&slot_b, &pos, sent[0].bytes, sent[0].len);   /* other net */
    HH_ASSERT_EQ_INT(slot_b.st.rx_wrong_net, 1);
    HH_ASSERT_EQ_INT(n_got, 0);

    HH_ASSERT_OK(setup_node(&slot_b, &mac_b, B, NET, 1u << 7, 128, b_tx, b_receiver));
    memcpy(copy, sent[0].bytes, sent[0].len);
    copy[12] ^= 0x01;   /* corrupt the PDU: CRC fails */
    hh_slot_receive(&slot_b, &pos, copy, sent[0].len);
    HH_ASSERT_EQ_INT(slot_b.st.rx_bad, 1);
    HH_ASSERT_EQ_INT(n_got, 0);

    hh_slot_receive(&slot_b, &pos, sent[0].bytes, sent[0].len);   /* the good one */
    HH_ASSERT_EQ_INT(n_got, 1);
}

HH_TEST_MAIN_BEGIN("slot")
    HH_RUN(test_init_validation);
    HH_RUN(test_sends_only_in_own_slots);
    HH_RUN(test_frame_and_superframe_numbers);
    HH_RUN(test_adjustable_frame);
    HH_RUN(test_packing_and_order);
    HH_RUN(test_max_payload_follows_capacity);
    HH_RUN(test_control_counts_against_capacity);
    HH_RUN(test_beacon_is_control_not_routing);
    HH_RUN(test_late_poll_misses_own_slot);
    HH_RUN(test_clock_before_epoch);
    HH_RUN(test_receive_path);
    HH_RUN(test_queue_report_empty);
    HH_RUN(test_queue_report_is_offered_load);
    HH_RUN(test_offered_load_is_the_same_from_every_slot);
    HH_RUN(test_offered_load_rises_when_slots_are_short);
    HH_RUN(test_offered_load_forgets_silent_frames);
    HH_RUN(test_queue_report_stored_on_receive);
    HH_RUN(test_alloc_enable);
    HH_RUN(test_voice_admit_query);
    HH_RUN(test_alloc_evicts_gone_node);
    HH_RUN(test_alloc_positions_stay_put);
    HH_RUN(test_alloc_clash_lower_id_keeps_slot);
    HH_RUN(test_logic_adds_crc);
    HH_RUN(test_receive_drops);
HH_TEST_MAIN_END()
