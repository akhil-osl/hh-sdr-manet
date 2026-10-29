/* Bytes on the wire, checked with a raw UDP socket as the peer: the message
 * dataterm sends, the ACK it returns for the frame specification v2 example,
 * and the versioning rules: unknown elective options are skipped, unknown
 * critical options are refused (4.02 on CON, RST on NON), an unknown
 * Content-Format is refused with 4.15, and CoAP version 2 is not accepted. */
#include "hh_test.h"
#include "hhsdr/core/log.h"
#include "hhsdr/dataterm/dataterm.h"
#include <arpa/inet.h>
#include <poll.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

static void quiet(hh_log_level_t lvl, const char *line, void *ctx) { (void)lvl; (void)line; (void)ctx; }

static hh_dt_t       g_e;
static hh_dt_peers_t g_peers;
static int           g_raw = -1;     /* node 3 */
static int           g_stranger = -1; /* not in the peer table */

static int      g_delivered;
static uint32_t g_from;
static char     g_text[64];
static int      g_done;
static char     g_reason[16];

static void on_deliver(hh_node_id_t from, const char *ip, uint16_t mid, const uint8_t *d,
                       size_t len, void *ctx)
{
    (void)ip; (void)mid; (void)ctx;
    g_delivered++;
    g_from = from;
    size_t n = len < sizeof g_text - 1 ? len : sizeof g_text - 1;
    memcpy(g_text, d, n);
    g_text[n] = '\0';
}

static void on_result(hh_node_id_t to, uint16_t mid, bool acked, const char *reason, void *ctx)
{
    (void)to; (void)mid; (void)acked; (void)ctx;
    g_done++;
    snprintf(g_reason, sizeof g_reason, "%s", reason);
}

static int64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static int raw_socket(const char *ip)
{
    int fd = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK, 0);
    struct sockaddr_in a = { .sin_family = AF_INET };
    inet_pton(AF_INET, ip, &a.sin_addr);
    if (fd >= 0 && bind(fd, (struct sockaddr *)&a, sizeof a) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static uint16_t port_of(int fd)
{
    struct sockaddr_in a;
    socklen_t l = sizeof a;
    getsockname(fd, (struct sockaddr *)&a, &l);
    return ntohs(a.sin_port);
}

/* Runs the engine and reads one datagram from fd, waiting up to ms. */
static ssize_t run_and_read(int fd, uint8_t *buf, size_t cap, int ms, struct sockaddr_in *from)
{
    int64_t end = now_ms() + ms;
    while (now_ms() < end) {
        struct pollfd p[2] = { { .fd = hh_dt_fd(&g_e), .events = POLLIN },
                               { .fd = fd, .events = POLLIN } };
        poll(p, 2, 5);
        hh_dt_process(&g_e);
        struct sockaddr_in a;
        socklen_t l = sizeof a;
        ssize_t n = recvfrom(fd, buf, cap, 0, (struct sockaddr *)&a, &l);
        if (n >= 0) {
            if (from) *from = a;
            return n;
        }
    }
    return -1;
}

/* Sends msg to dataterm from fd and returns the reply length, or -1. */
static ssize_t exchange(int fd, const uint8_t *msg, size_t len, uint8_t *reply, size_t cap)
{
    struct sockaddr_in to = { .sin_family = AF_INET, .sin_port = htons(g_e.port) };
    inet_pton(AF_INET, "127.0.0.2", &to.sin_addr);
    sendto(fd, msg, len, 0, (struct sockaddr *)&to, sizeof to);
    return run_and_read(fd, reply, cap, 500, NULL);
}

static void test_sent_message_bytes(void)
{
    g_done = 0;
    HH_ASSERT_OK(hh_dt_send(&g_e, 3, (const uint8_t *)"HELLO", 5));
    uint8_t b[64];
    struct sockaddr_in from;
    ssize_t n = run_and_read(g_raw, b, sizeof b, 1000, &from);
    HH_ASSERT_EQ_INT(n, 19);
    HH_ASSERT_EQ_INT(b[0], 0x44);  /* version 1, CON, token length 4 */
    HH_ASSERT_EQ_INT(b[1], 0x02);  /* POST */
    static const uint8_t tail[] = { 0xB3, 'm', 's', 'g', 0x10, 0xFF, 'H', 'E', 'L', 'L', 'O' };
    HH_ASSERT(memcmp(b + 8, tail, sizeof tail) == 0);

    /* ACK 2.04 echoing Message ID and token. */
    uint8_t ack[8] = { 0x64, 0x44, b[2], b[3], b[4], b[5], b[6], b[7] };
    sendto(g_raw, ack, sizeof ack, 0, (struct sockaddr *)&from, sizeof from);
    run_and_read(g_raw, b, sizeof b, 200, NULL);
    HH_ASSERT_EQ_INT(g_done, 1);
    HH_ASSERT_EQ_STR(g_reason, "acked");
}

static void test_spec_example_and_ack(void)
{
    g_delivered = 0;
    static const uint8_t con[] = { 0x44, 0x02, 0x00, 0x07, 0x1A, 0x2B, 0x3C, 0x4D, 0xB3, 'm',
                                   's', 'g', 0x10, 0xFF, 'H', 'E', 'L', 'L', 'O' };
    static const uint8_t want[] = { 0x64, 0x44, 0x00, 0x07, 0x1A, 0x2B, 0x3C, 0x4D };
    uint8_t r[64];
    HH_ASSERT_EQ_INT(exchange(g_raw, con, sizeof con, r, sizeof r), 8);
    HH_ASSERT(memcmp(r, want, sizeof want) == 0);
    HH_ASSERT_EQ_INT(g_delivered, 1);
    HH_ASSERT_EQ_INT(g_from, 3);
    HH_ASSERT_EQ_STR(g_text, "HELLO");
}

static void test_unknown_elective_option_is_skipped(void)
{
    g_delivered = 0;
    /* Option 65000 is even, so elective. */
    static const uint8_t con[] = { 0x44, 0x02, 0x00, 0x08, 1, 2, 3, 4, 0xB3, 'm', 's', 'g',
                                   0x10, 0xE0, 0xFC, 0xCF, 0xFF, 'H', 'I' };
    uint8_t r[64];
    HH_ASSERT(exchange(g_raw, con, sizeof con, r, sizeof r) >= 4);
    HH_ASSERT_EQ_INT(r[1], 0x44);
    HH_ASSERT_EQ_INT(g_delivered, 1);
    HH_ASSERT_EQ_STR(g_text, "HI");
}

static void test_unknown_critical_option_is_refused(void)
{
    g_delivered = 0;
    /* Option 65001 is odd, so critical. CON gets 4.02 Bad Option. */
    static const uint8_t con[] = { 0x44, 0x02, 0x00, 0x09, 1, 2, 3, 5, 0xB3, 'm', 's', 'g',
                                   0x10, 0xE0, 0xFC, 0xD0, 0xFF, 'H', 'I' };
    uint8_t r[64];
    HH_ASSERT(exchange(g_raw, con, sizeof con, r, sizeof r) >= 4);
    HH_ASSERT_EQ_INT(r[0] & 0x30, 0x20);  /* ACK */
    HH_ASSERT_EQ_INT(r[1], 0x82);         /* 4.02 */

    /* NON is refused too: RST from libcoap 4.3.0, a NON 4.02 from 4.3.5. */
    static const uint8_t non[] = { 0x54, 0x02, 0x00, 0x0A, 1, 2, 3, 6, 0xB3, 'm', 's', 'g',
                                   0x10, 0xE0, 0xFC, 0xD0, 0xFF, 'H', 'I' };
    HH_ASSERT(exchange(g_raw, non, sizeof non, r, sizeof r) >= 4);
    HH_ASSERT((r[0] & 0x30) == 0x30 || ((r[0] & 0x30) == 0x10 && r[1] == 0x82));
    HH_ASSERT_EQ_INT(g_delivered, 0);
}

static void test_unknown_content_format_is_refused(void)
{
    g_delivered = 0;
    uint64_t before = hh_dt_counters(&g_e)->rx_bad_format;
    /* Content-Format 50, application/json. */
    static const uint8_t con[] = { 0x44, 0x02, 0x00, 0x0B, 1, 2, 3, 7, 0xB3, 'm', 's', 'g',
                                   0x11, 0x32, 0xFF, '{', '}' };
    uint8_t r[64];
    HH_ASSERT(exchange(g_raw, con, sizeof con, r, sizeof r) >= 4);
    HH_ASSERT_EQ_INT(r[1], 0x8F);  /* 4.15 */
    HH_ASSERT_EQ_INT(g_delivered, 0);
    HH_ASSERT_EQ_INT(hh_dt_counters(&g_e)->rx_bad_format, before + 1);
}

static void test_version_2_is_not_accepted(void)
{
    g_delivered = 0;
    static const uint8_t con[] = { 0x84, 0x02, 0x00, 0x0C, 1, 2, 3, 8, 0xB3, 'm', 's', 'g',
                                   0xFF, 'H', 'I' };
    uint8_t r[64];
    exchange(g_raw, con, sizeof con, r, sizeof r);
    HH_ASSERT_EQ_INT(g_delivered, 0);
}

static void test_unknown_sender_is_delivered_by_ip(void)
{
    g_delivered = 0;
    static const uint8_t con[] = { 0x44, 0x02, 0x00, 0x01, 9, 9, 9, 9, 0xB3, 'm', 's', 'g',
                                   0x10, 0xFF, 'Y', 'O' };
    uint8_t r[64];
    HH_ASSERT(exchange(g_stranger, con, sizeof con, r, sizeof r) >= 4);
    HH_ASSERT_EQ_INT(r[1], 0x44);
    HH_ASSERT_EQ_INT(g_delivered, 1);
    HH_ASSERT_EQ_INT(g_from, HH_NODE_ID_INVALID);
    HH_ASSERT_EQ_INT(hh_dt_counters(&g_e)->rx_unknown_sender, 1);
}

static void reply_to_next_send(uint8_t type_byte, uint8_t code, const char *want_reason)
{
    g_done = 0;
    HH_ASSERT_OK(hh_dt_send(&g_e, 3, (const uint8_t *)"Q", 1));
    uint8_t b[64];
    struct sockaddr_in from;
    HH_ASSERT(run_and_read(g_raw, b, sizeof b, 1000, &from) > 0);
    uint8_t rep[8] = { type_byte, code, b[2], b[3], b[4], b[5], b[6], b[7] };
    size_t len = (type_byte & 0x0F) ? 8 : 4;  /* RST carries no token */
    sendto(g_raw, rep, len, 0, (struct sockaddr *)&from, sizeof from);
    run_and_read(g_raw, b, sizeof b, 200, NULL);
    HH_ASSERT_EQ_INT(g_done, 1);
    HH_ASSERT_EQ_STR(g_reason, want_reason);
}

static void test_peer_rst_and_error_code(void)
{
    reply_to_next_send(0x70, 0x00, "rst");
    HH_ASSERT_EQ_INT(hh_dt_counters(&g_e)->tx_rst, 1);
    reply_to_next_send(0x64, 0x84, "refused");  /* ACK 4.04 */
    HH_ASSERT_EQ_INT(hh_dt_counters(&g_e)->tx_refused, 1);
}

static void test_setup(void)
{
    g_raw = raw_socket("127.0.0.3");
    g_stranger = raw_socket("127.0.0.6");
    HH_ASSERT(g_raw >= 0 && g_stranger >= 0);
    hh_dt_peers_init(&g_peers);
    hh_dt_config_t cfg;
    hh_dt_config_default(&cfg);
    cfg.ack_timeout_ms = HH_DT_ACK_TIMEOUT_MIN_MS;
    HH_ASSERT_OK(hh_dt_peers_add(&g_peers, 3, "127.0.0.3", port_of(g_raw)));
    HH_ASSERT_OK(hh_dt_open(&g_e, "127.0.0.2", 0, 0, &cfg, &g_peers));
    hh_dt_set_deliver(&g_e, on_deliver, NULL);
    hh_dt_set_result(&g_e, on_result, NULL);
}

HH_TEST_MAIN_BEGIN("dataterm_wire")
    hh_log_set_sink(quiet, NULL);
    HH_RUN(test_setup);
    HH_RUN(test_sent_message_bytes);
    HH_RUN(test_spec_example_and_ack);
    HH_RUN(test_unknown_elective_option_is_skipped);
    HH_RUN(test_unknown_critical_option_is_refused);
    HH_RUN(test_unknown_content_format_is_refused);
    HH_RUN(test_version_2_is_not_accepted);
    HH_RUN(test_unknown_sender_is_delivered_by_ip);
    HH_RUN(test_peer_rst_and_error_code);
    hh_dt_close(&g_e);
    close(g_raw);
    close(g_stranger);
    hh_log_reset();
HH_TEST_MAIN_END()
