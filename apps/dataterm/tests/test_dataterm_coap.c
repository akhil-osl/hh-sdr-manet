/* Two dataterm instances on loopback, with libcoap doing ACK and retry in real
 * time. A small UDP relay between them drops chosen datagrams, to show loss
 * counting from Message ID gaps, retransmission and duplicate handling.
 * Timeouts are at libcoap's minimum of 1 s, so the suite takes a few seconds. */
#include "hh_test.h"
#include "hhsdr/core/log.h"
#include "hhsdr/dataterm/dataterm.h"
#include <arpa/inet.h>
#include <poll.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

static void quiet(hh_log_level_t lvl, const char *line, void *ctx) { (void)lvl; (void)line; (void)ctx; }

typedef struct {
    int      delivered;
    uint32_t last_from;
    char     last_text[64];
} rx_log_t;

typedef struct {
    int  done;
    int  acked;
    char last_reason[16];
} tx_log_t;

static void on_deliver(hh_node_id_t from, const char *ip, uint16_t mid, const uint8_t *d,
                       size_t len, void *ctx)
{
    (void)ip; (void)mid;
    rx_log_t *r = ctx;
    r->delivered++;
    r->last_from = from;
    size_t n = len < sizeof r->last_text - 1 ? len : sizeof r->last_text - 1;
    memcpy(r->last_text, d, n);
    r->last_text[n] = '\0';
}

static void on_result(hh_node_id_t to, uint16_t mid, bool acked, const char *reason, void *ctx)
{
    (void)to; (void)mid;
    tx_log_t *t = ctx;
    t->done++;
    if (acked) t->acked++;
    snprintf(t->last_reason, sizeof t->last_reason, "%s", reason);
}

/* Relay: A sends to it, it forwards to B and B's replies back to A. */
typedef struct {
    int                fd;
    uint16_t           port;
    struct sockaddr_in a, b;
    bool               have_a;
    int                a2b_count, b2a_count;
    int                drop_a2b[4], drop_b2a[4];  /* 1-based datagram numbers, 0 = unused */
    bool               drop_all_a2b;
} relay_t;

static bool in_list(const int *l, int n)
{
    for (int i = 0; i < 4; i++)
        if (l[i] && l[i] == n) return true;
    return false;
}

static void relay_open(relay_t *r, const char *ip, uint16_t b_port, const char *b_ip)
{
    memset(r, 0, sizeof *r);
    r->fd = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK, 0);
    struct sockaddr_in s = { .sin_family = AF_INET };
    inet_pton(AF_INET, ip, &s.sin_addr);
    HH_ASSERT(bind(r->fd, (struct sockaddr *)&s, sizeof s) == 0);
    socklen_t l = sizeof s;
    getsockname(r->fd, (struct sockaddr *)&s, &l);
    r->port = ntohs(s.sin_port);
    r->b.sin_family = AF_INET;
    r->b.sin_port = htons(b_port);
    inet_pton(AF_INET, b_ip, &r->b.sin_addr);
}

static void relay_step(relay_t *r)
{
    uint8_t buf[1600];
    for (;;) {
        struct sockaddr_in from;
        socklen_t l = sizeof from;
        ssize_t n = recvfrom(r->fd, buf, sizeof buf, 0, (struct sockaddr *)&from, &l);
        if (n < 0) return;
        bool from_b = from.sin_addr.s_addr == r->b.sin_addr.s_addr &&
                      from.sin_port == r->b.sin_port;
        if (from_b) {
            if (!r->have_a || in_list(r->drop_b2a, ++r->b2a_count)) continue;
            sendto(r->fd, buf, (size_t)n, 0, (struct sockaddr *)&r->a, sizeof r->a);
        } else {
            r->a = from;
            r->have_a = true;
            if (r->drop_all_a2b || in_list(r->drop_a2b, ++r->a2b_count)) continue;
            sendto(r->fd, buf, (size_t)n, 0, (struct sockaddr *)&r->b, sizeof r->b);
        }
    }
}

static int64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* Runs both instances and the relay until *done reaches want or 3 s pass. */
static bool run_until(hh_dt_t *a, hh_dt_t *b, relay_t *r, const int *done, int want)
{
    int64_t end = now_ms() + 8000;
    while (now_ms() < end) {
        struct pollfd p[3] = { { .fd = hh_dt_fd(a), .events = POLLIN },
                               { .fd = b ? hh_dt_fd(b) : -1, .events = POLLIN },
                               { .fd = r ? r->fd : -1, .events = POLLIN } };
        poll(p, 3, 5);
        hh_dt_process(a);
        if (b) hh_dt_process(b);
        if (r) relay_step(r);
        if (*done >= want) {
            /* Let the last ACK or late copies settle. */
            int64_t settle = now_ms() + 50;
            while (now_ms() < settle) {
                poll(p, 3, 5);
                hh_dt_process(a);
                if (b) hh_dt_process(b);
                if (r) relay_step(r);
            }
            return true;
        }
    }
    return false;
}

static hh_dt_t      g_a, g_b;
static hh_dt_peers_t g_pa, g_pb;
static rx_log_t     g_rx;
static tx_log_t     g_tx;

static void setup(uint8_t retransmit, relay_t *r)
{
    memset(&g_rx, 0, sizeof g_rx);
    memset(&g_tx, 0, sizeof g_tx);
    hh_dt_config_t cfg;
    hh_dt_config_default(&cfg);
    cfg.ack_timeout_ms = HH_DT_ACK_TIMEOUT_MIN_MS;
    cfg.max_retransmit = retransmit;

    hh_dt_peers_init(&g_pa);
    hh_dt_peers_init(&g_pb);
    HH_ASSERT_OK(hh_dt_open(&g_b, "127.0.0.2", 0, 0, &cfg, &g_pb));
    if (r) {
        relay_open(r, "127.0.0.4", g_b.port, "127.0.0.2");
        HH_ASSERT_OK(hh_dt_peers_add(&g_pa, 2, "127.0.0.4", r->port));
        HH_ASSERT_OK(hh_dt_peers_add(&g_pb, 1, "127.0.0.4", 1));
    } else {
        HH_ASSERT_OK(hh_dt_peers_add(&g_pa, 2, "127.0.0.2", g_b.port));
        HH_ASSERT_OK(hh_dt_peers_add(&g_pb, 1, "127.0.0.1", 1));
    }
    HH_ASSERT_OK(hh_dt_open(&g_a, "127.0.0.1", 0, 0, &cfg, &g_pa));
    hh_dt_set_deliver(&g_b, on_deliver, &g_rx);
    hh_dt_set_result(&g_a, on_result, &g_tx);
}

static void teardown(relay_t *r)
{
    hh_dt_close(&g_a);
    hh_dt_close(&g_b);
    if (r) close(r->fd);
}

static void send_text(const char *s)
{
    HH_ASSERT_OK(hh_dt_send(&g_a, 2, (const uint8_t *)s, strlen(s)));
}

static void test_message_and_ack(void)
{
    setup(4, NULL);
    send_text("HELLO");
    HH_ASSERT(run_until(&g_a, &g_b, NULL, &g_tx.done, 1));
    HH_ASSERT_EQ_INT(g_rx.delivered, 1);
    HH_ASSERT_EQ_STR(g_rx.last_text, "HELLO");
    HH_ASSERT_EQ_INT(g_rx.last_from, 1);
    HH_ASSERT_EQ_INT(g_tx.acked, 1);
    HH_ASSERT_EQ_STR(g_tx.last_reason, "acked");
    HH_ASSERT_EQ_INT(hh_dt_counters(&g_a)->tx_acked, 1);
    HH_ASSERT_EQ_INT(hh_dt_counters(&g_b)->rx_gaps, 0);
    HH_ASSERT(!hh_dt_busy(&g_a));
    teardown(NULL);
}

static void test_lost_message_counted_as_gap(void)
{
    relay_t r;
    setup(0, &r);  /* retry off: sent as NON */
    r.drop_a2b[0] = 2;
    send_text("one");
    send_text("two");
    send_text("three");
    HH_ASSERT(run_until(&g_a, &g_b, &r, &g_tx.done, 3));
    HH_ASSERT_EQ_INT(g_tx.acked, 2);
    HH_ASSERT_EQ_INT(hh_dt_counters(&g_a)->tx_given_up, 1);
    HH_ASSERT_EQ_INT(g_rx.delivered, 2);
    HH_ASSERT_EQ_STR(g_rx.last_text, "three");
    HH_ASSERT_EQ_INT(hh_dt_counters(&g_b)->rx_gaps, 1);
    teardown(&r);
}

static void test_retransmission_recovers_loss(void)
{
    relay_t r;
    setup(2, &r);
    r.drop_a2b[0] = 1;
    send_text("again");
    HH_ASSERT(run_until(&g_a, &g_b, &r, &g_tx.done, 1));
    HH_ASSERT_EQ_INT(g_tx.acked, 1);
    HH_ASSERT_EQ_INT(g_rx.delivered, 1);
    HH_ASSERT_EQ_INT(hh_dt_counters(&g_b)->rx_gaps, 0);
    HH_ASSERT_EQ_INT(hh_dt_counters(&g_b)->rx_duplicates, 0);
    teardown(&r);
}

static void test_lost_ack_gives_duplicate(void)
{
    relay_t r;
    setup(2, &r);
    r.drop_b2a[0] = 1;
    send_text("dup");
    HH_ASSERT(run_until(&g_a, &g_b, &r, &g_tx.done, 1));
    HH_ASSERT_EQ_INT(g_tx.acked, 1);
    /* Delivered once. libcoap 4.3.5 answers the repeat from its own cache, so
     * the handler may never see it; older versions pass it up. */
    HH_ASSERT_EQ_INT(g_rx.delivered, 1);
    HH_ASSERT(hh_dt_counters(&g_b)->rx_duplicates <= 1);
    HH_ASSERT(r.a2b_count >= 2);
    teardown(&r);
}

static void test_sender_restart_is_not_a_gap(void)
{
    setup(4, NULL);
    send_text("first run");
    HH_ASSERT(run_until(&g_a, &g_b, NULL, &g_tx.done, 1));
    /* A new sender run: new session, new source port, new Message IDs. */
    hh_dt_close(&g_a);
    hh_dt_config_t cfg;
    hh_dt_config_default(&cfg);
    HH_ASSERT_OK(hh_dt_open(&g_a, "127.0.0.1", 0, 0, &cfg, &g_pa));
    hh_dt_set_result(&g_a, on_result, &g_tx);
    send_text("second run");
    HH_ASSERT(run_until(&g_a, &g_b, NULL, &g_tx.done, 2));
    HH_ASSERT_EQ_INT(g_rx.delivered, 2);
    HH_ASSERT_EQ_INT(hh_dt_counters(&g_b)->rx_gaps, 0);
    HH_ASSERT_EQ_INT(hh_dt_counters(&g_b)->rx_resync, 1);
    teardown(NULL);
}

static void test_limits(void)
{
    relay_t r;
    setup(4, &r);
    r.drop_all_a2b = true;
    for (unsigned i = 0; i < HH_DT_QUEUE_LEN; i++) send_text("x");
    HH_ASSERT_ERR(hh_dt_send(&g_a, 2, (const uint8_t *)"x", 1), HH_ERR_AGAIN);
    HH_ASSERT_EQ_INT(hh_dt_counters(&g_a)->tx_queue_full, 1);
    HH_ASSERT_ERR(hh_dt_send(&g_a, 9, (const uint8_t *)"x", 1), HH_ERR_NOTFOUND);
    static uint8_t big[1233];
    HH_ASSERT_ERR(hh_dt_send(&g_a, 2, big, sizeof big), HH_ERR_INVAL);
    HH_ASSERT(hh_dt_busy(&g_a));
    teardown(&r);
}

static int tos_of(int fd)
{
    int tos = -1;
    socklen_t l = sizeof tos;
    getsockopt(fd, IPPROTO_IP, IP_TOS, &tos, &l);
    return tos;
}

static int pmtu_of(int fd)
{
    int v = -1;
    socklen_t l = sizeof v;
    getsockopt(fd, IPPROTO_IP, IP_MTU_DISCOVER, &v, &l);
    return v;
}

static void test_dscp_and_df_on_every_socket(void)
{
    hh_dt_config_t cfg;
    hh_dt_config_default(&cfg);
    hh_dt_peers_init(&g_pa);
    HH_ASSERT_OK(hh_dt_peers_add(&g_pa, 7, "127.0.0.7", 9));
    HH_ASSERT_OK(hh_dt_open(&g_a, "127.0.0.5", 0, 10, &cfg, &g_pa));
    HH_ASSERT(g_a.port != 0);
    HH_ASSERT_OK(hh_dt_send(&g_a, 7, (const uint8_t *)"x", 1));

    int endpoint = 0, session = 0;
    for (int fd = 0; fd < 1024; fd++) {
        struct sockaddr_in s, p;
        socklen_t l = sizeof s, pl = sizeof p;
        int type = 0;
        socklen_t tl = sizeof type;
        if (getsockopt(fd, SOL_SOCKET, SO_TYPE, &type, &tl) != 0 || type != SOCK_DGRAM) continue;
        if (getsockname(fd, (struct sockaddr *)&s, &l) != 0 || s.sin_family != AF_INET) continue;
        if (ntohs(s.sin_port) == g_a.port && s.sin_addr.s_addr == htonl(0x7f000005)) {
            endpoint++;
            HH_ASSERT_EQ_INT(tos_of(fd), 10 << 2);
            HH_ASSERT_EQ_INT(pmtu_of(fd), IP_PMTUDISC_DO);
        } else if (getpeername(fd, (struct sockaddr *)&p, &pl) == 0 &&
                   p.sin_addr.s_addr == htonl(0x7f000007)) {
            session++;
            HH_ASSERT_EQ_INT(tos_of(fd), 10 << 2);
            HH_ASSERT_EQ_INT(pmtu_of(fd), IP_PMTUDISC_DO);
        }
    }
    HH_ASSERT_EQ_INT(endpoint, 1);
    HH_ASSERT_EQ_INT(session, 1);
    hh_dt_close(&g_a);
    HH_ASSERT_ERR(hh_dt_open(&g_a, "127.0.0.5", 0, 64, &cfg, &g_pa), HH_ERR_INVAL);
    cfg.ack_timeout_ms = HH_DT_ACK_TIMEOUT_MIN_MS - 1;
    HH_ASSERT_ERR(hh_dt_open(&g_a, "127.0.0.5", 0, 0, &cfg, &g_pa), HH_ERR_INVAL);
}

HH_TEST_MAIN_BEGIN("dataterm_coap")
    hh_log_set_sink(quiet, NULL);
    HH_RUN(test_message_and_ack);
    HH_RUN(test_lost_message_counted_as_gap);
    HH_RUN(test_retransmission_recovers_loss);
    HH_RUN(test_lost_ack_gives_duplicate);
    HH_RUN(test_sender_restart_is_not_a_gap);
    HH_RUN(test_limits);
    HH_RUN(test_dscp_and_df_on_every_socket);
    hh_log_reset();
HH_TEST_MAIN_END()
