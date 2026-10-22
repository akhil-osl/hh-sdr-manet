/* Protocol engine on an emulated link with a virtual clock: ACK, retry,
 * timeout doubling, give-up, duplicates and gap counting. */
#include "hh_test.h"
#include "vclock.h"
#include "hhsdr/core/log.h"
#include "hhsdr/dataterm/dataterm.h"

typedef bool (*drop_fn)(const hh_dt_hdr_t *h);

typedef struct {
    uint8_t      buf[HH_DT_MSG_MAX];
    size_t       len;
    hh_node_id_t to;
} pkt_t;

typedef struct {
    pkt_t   q[64];
    int     n;
    drop_fn drop;
    int     dropped;
} link_t;

static link_t   L;
static vclock_t VC;
static hh_dt_t  A, B;   /* node 1 and node 2 */

static hh_seq_t delivered[64];
static int      ndelivered;
static int      nresults, nacked;
static uint8_t  last_attempts;
static int      gap_records;

static hh_status_t link_tx(hh_node_id_t to, const uint8_t *buf, size_t len, void *ctx)
{
    (void)ctx;
    hh_dt_hdr_t h;
    if (hh_dt_decode(buf, len, &h, NULL) == HH_DT_RX_OK && L.drop && L.drop(&h)) {
        L.dropped++;
        return HH_OK;
    }
    if (L.n == 64) return HH_ERR_AGAIN;
    memcpy(L.q[L.n].buf, buf, len);
    L.q[L.n].len = len;
    L.q[L.n].to = to;
    L.n++;
    return HH_OK;
}

static void on_deliver(hh_node_id_t from, hh_seq_t seq, const uint8_t *d, size_t len, void *ctx)
{
    (void)from; (void)d; (void)len; (void)ctx;
    if (ndelivered < 64) delivered[ndelivered++] = seq;
}

static void on_result(hh_node_id_t to, hh_seq_t seq, bool acked, uint8_t attempts, void *ctx)
{
    (void)to; (void)seq; (void)ctx;
    nresults++;
    if (acked) nacked++;
    last_attempts = attempts;
}

static void sink(hh_log_level_t lvl, const char *line, void *ctx)
{
    (void)lvl; (void)ctx;
    if (strstr(line, "event=gap ")) gap_records++;
}

static void setup(uint32_t timeout, uint8_t retries, drop_fn drop)
{
    memset(&L, 0, sizeof L);
    L.drop = drop;
    ndelivered = nresults = nacked = gap_records = 0;
    last_attempts = 0;
    vclock_init(&VC, 1000);
    hh_log_reset();
    hh_log_set_sink(sink, NULL);
    hh_dt_config_t cfg;
    hh_dt_config_default(&cfg);
    cfg.timeout_ms = timeout;
    cfg.max_retries = retries;
    hh_dt_init(&A, 1, &cfg, &VC.clock, link_tx, NULL);
    hh_dt_init(&B, 2, &cfg, &VC.clock, link_tx, NULL);
    hh_dt_set_result(&A, on_result, NULL);
    hh_dt_set_deliver(&B, on_deliver, NULL);
}

/* Delivers queued datagrams, then moves time in 10 ms steps. */
static void run(hh_time_ms_t ms)
{
    for (hh_time_ms_t t = 0; t <= ms; t += 10) {
        while (L.n) {
            pkt_t p = L.q[0];
            memmove(&L.q[0], &L.q[1], sizeof(pkt_t) * (size_t)(L.n - 1));
            L.n--;
            hh_dt_input(p.to == 1 ? &A : &B, p.buf, p.len);
        }
        hh_dt_poll(&A);
        hh_dt_poll(&B);
        if (t < ms) vclock_advance(&VC, 10);
    }
}

static void send_n(int n)
{
    for (int i = 0; i < n; i++) hh_dt_send(&A, 2, (const uint8_t *)"HELLO", 5);
}

static bool drop_data_seq3_first(const hh_dt_hdr_t *h)
{ return h->type == HH_DT_TYPE_DATA && h->seq == 3 && h->attempt == 0; }
static bool drop_ack_seq2_first(const hh_dt_hdr_t *h)
{ return h->type == HH_DT_TYPE_ACK && h->seq == 2 && h->attempt == 0; }
static bool drop_data_seq2(const hh_dt_hdr_t *h)
{ return h->type == HH_DT_TYPE_DATA && h->seq == 2; }
static bool drop_data_seq1(const hh_dt_hdr_t *h)
{ return h->type == HH_DT_TYPE_DATA && h->seq == 1; }
static bool drop_all(const hh_dt_hdr_t *h) { (void)h; return true; }

static void test_clean_link(void)
{
    setup(100, 4, NULL);
    send_n(5);
    run(1000);
    const hh_dt_counters_t *a = hh_dt_counters(&A), *b = hh_dt_counters(&B);
    HH_ASSERT_EQ_INT(ndelivered, 5);
    for (int i = 0; i < 5; i++) HH_ASSERT_EQ_INT(delivered[i], i + 1);
    HH_ASSERT_EQ_INT(a->tx_data, 5);
    HH_ASSERT_EQ_INT(a->tx_acked, 5);
    HH_ASSERT_EQ_INT(a->tx_acked_first_try, 5);
    HH_ASSERT_EQ_INT(a->tx_retries, 0);
    HH_ASSERT_EQ_INT(b->rx_gaps, 0);
    HH_ASSERT_EQ_INT(b->rx_duplicates, 0);
    HH_ASSERT(!hh_dt_busy(&A));
    HH_ASSERT_EQ_INT(hh_dt_next_timeout_ms(&A), -1);
}

static void test_lost_data_is_retried(void)
{
    setup(100, 4, drop_data_seq3_first);
    send_n(5);
    run(2000);
    const hh_dt_counters_t *a = hh_dt_counters(&A), *b = hh_dt_counters(&B);
    HH_ASSERT_EQ_INT(ndelivered, 5);
    HH_ASSERT_EQ_INT(a->tx_retries, 1);
    HH_ASSERT_EQ_INT(a->tx_acked, 5);
    HH_ASSERT_EQ_INT(a->tx_acked_first_try, 4);
    HH_ASSERT_EQ_INT(b->rx_first_try, 4);
    HH_ASSERT_EQ_INT(b->rx_gaps, 0);
    HH_ASSERT_EQ_INT(b->rx_duplicates, 0);
}

static void test_lost_ack_gives_duplicate_delivered_once(void)
{
    setup(100, 4, drop_ack_seq2_first);
    send_n(3);
    run(2000);
    const hh_dt_counters_t *a = hh_dt_counters(&A), *b = hh_dt_counters(&B);
    HH_ASSERT_EQ_INT(ndelivered, 3);
    HH_ASSERT_EQ_INT(b->rx_duplicates, 1);
    HH_ASSERT_EQ_INT(b->tx_acks, 4);
    HH_ASSERT_EQ_INT(a->tx_retries, 1);
    HH_ASSERT_EQ_INT(a->tx_acked, 3);
}

static void test_give_up_then_gap(void)
{
    setup(100, 2, drop_data_seq2);
    send_n(3);
    run(3000);
    const hh_dt_counters_t *a = hh_dt_counters(&A), *b = hh_dt_counters(&B);
    HH_ASSERT_EQ_INT(a->tx_given_up, 1);
    HH_ASSERT_EQ_INT(a->tx_retries, 2);
    HH_ASSERT_EQ_INT(nresults, 3);
    HH_ASSERT_EQ_INT(nacked, 2);
    HH_ASSERT_EQ_INT(ndelivered, 2);
    HH_ASSERT_EQ_INT(delivered[0], 1);
    HH_ASSERT_EQ_INT(delivered[1], 3);
    HH_ASSERT_EQ_INT(b->rx_gaps, 1);
    HH_ASSERT_EQ_INT(gap_records, 1);
}

static void test_retry_off_counts_raw_loss(void)
{
    setup(100, 0, drop_data_seq2);
    send_n(4);
    run(2000);
    const hh_dt_counters_t *a = hh_dt_counters(&A), *b = hh_dt_counters(&B);
    HH_ASSERT_EQ_INT(a->tx_retries, 0);
    HH_ASSERT_EQ_INT(a->tx_given_up, 1);
    HH_ASSERT_EQ_INT(b->rx_delivered, 3);
    HH_ASSERT_EQ_INT(b->rx_gaps, 1);
}

static void test_lost_first_message_counts_as_gap(void)
{
    setup(100, 0, drop_data_seq1);
    send_n(2);
    run(1000);
    HH_ASSERT_EQ_INT(hh_dt_counters(&B)->rx_gaps, 1);
    HH_ASSERT_EQ_INT(ndelivered, 1);
    HH_ASSERT_EQ_INT(delivered[0], 2);
}

static void test_timeout_doubles(void)
{
    setup(100, 2, drop_all);
    send_n(1);
    const hh_dt_counters_t *a = hh_dt_counters(&A);
    HH_ASSERT_EQ_INT(hh_dt_next_timeout_ms(&A), 100);
    vclock_advance(&VC, 99);  hh_dt_poll(&A); HH_ASSERT_EQ_INT(a->tx_retries, 0);
    vclock_advance(&VC, 1);   hh_dt_poll(&A); HH_ASSERT_EQ_INT(a->tx_retries, 1);
    HH_ASSERT_EQ_INT(hh_dt_next_timeout_ms(&A), 200);
    vclock_advance(&VC, 199); hh_dt_poll(&A); HH_ASSERT_EQ_INT(a->tx_retries, 1);
    vclock_advance(&VC, 1);   hh_dt_poll(&A); HH_ASSERT_EQ_INT(a->tx_retries, 2);
    vclock_advance(&VC, 399); hh_dt_poll(&A); HH_ASSERT_EQ_INT(a->tx_given_up, 0);
    vclock_advance(&VC, 1);   hh_dt_poll(&A); HH_ASSERT_EQ_INT(a->tx_given_up, 1);
    HH_ASSERT_EQ_INT(last_attempts, 3);
    HH_ASSERT_EQ_INT(L.dropped, 3);
}

static void test_send_refusals(void)
{
    setup(100, 4, drop_all);
    uint8_t big[1233] = { 0 };
    HH_ASSERT_ERR(hh_dt_send(&A, 2, big, sizeof big), HH_ERR_INVAL);
    HH_ASSERT_EQ_INT(hh_dt_counters(&A)->tx_too_large, 1);
    HH_ASSERT_OK(hh_dt_send(&A, 2, big, 1232));
    HH_ASSERT_ERR(hh_dt_send(&A, 1, big, 1), HH_ERR_INVAL);
    HH_ASSERT_ERR(hh_dt_send(&A, 0, big, 1), HH_ERR_INVAL);
    for (int i = 1; i < HH_DT_QUEUE_LEN; i++) HH_ASSERT_OK(hh_dt_send(&A, 2, big, 1));
    HH_ASSERT_ERR(hh_dt_send(&A, 2, big, 1), HH_ERR_AGAIN);
    HH_ASSERT_EQ_INT(hh_dt_counters(&A)->tx_queue_full, 1);
}

static void test_not_for_me_and_corrupt(void)
{
    setup(100, 4, NULL);
    hh_dt_hdr_t h = { .type = HH_DT_TYPE_DATA, .flags = HH_DT_FLAG_ACK_REQ,
                      .src = 1, .dst = 3, .seq = 1, .len = 2 };
    uint8_t buf[64];
    size_t n;
    hh_dt_encode(&h, (const uint8_t *)"hi", buf, sizeof buf, &n);
    HH_ASSERT_EQ_INT(hh_dt_input(&B, buf, n), HH_DT_RX_OK);
    HH_ASSERT_EQ_INT(hh_dt_counters(&B)->rx_not_for_me, 1);
    HH_ASSERT_EQ_INT(hh_dt_counters(&B)->tx_acks, 0);
    HH_ASSERT_EQ_INT(L.n, 0);

    buf[n - 1] ^= 0x40;
    HH_ASSERT_EQ_INT(hh_dt_input(&B, buf, n), HH_DT_RX_CRC);
    HH_ASSERT_EQ_INT(hh_dt_counters(&B)->rx_bad_crc, 1);
    HH_ASSERT_EQ_INT(ndelivered, 0);
}

static void test_sender_restart(void)
{
    setup(100, 4, NULL);
    send_n(3);
    run(500);
    hh_dt_config_t cfg;
    hh_dt_config_default(&cfg);
    cfg.timeout_ms = 100;
    hh_dt_init(&A, 1, &cfg, &VC.clock, link_tx, NULL);
    send_n(2);
    run(500);
    const hh_dt_counters_t *b = hh_dt_counters(&B);
    HH_ASSERT_EQ_INT(ndelivered, 5);
    HH_ASSERT_EQ_INT(delivered[3], 1);
    HH_ASSERT_EQ_INT(b->rx_duplicates, 0);
    HH_ASSERT_EQ_INT(b->rx_gaps, 0);
}

HH_TEST_MAIN_BEGIN("dataterm")
    HH_RUN(test_clean_link);
    HH_RUN(test_lost_data_is_retried);
    HH_RUN(test_lost_ack_gives_duplicate_delivered_once);
    HH_RUN(test_give_up_then_gap);
    HH_RUN(test_retry_off_counts_raw_loss);
    HH_RUN(test_lost_first_message_counts_as_gap);
    HH_RUN(test_timeout_doubles);
    HH_RUN(test_send_refusals);
    HH_RUN(test_not_for_me_and_corrupt);
    HH_RUN(test_sender_restart);
    hh_log_reset();
HH_TEST_MAIN_END()
