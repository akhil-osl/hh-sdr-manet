/* UDP transport over loopback: socket options, peer table, and one message
 * with its ACK between two real sockets. */
#include "hh_test.h"
#include "vclock.h"
#include "hhsdr/core/log.h"
#include "hhsdr/dataterm/udp.h"
#include <arpa/inet.h>
#include <poll.h>
#include <sys/socket.h>

static int ndelivered;

static void on_deliver(hh_node_id_t from, hh_seq_t seq, const uint8_t *d, size_t len, void *ctx)
{
    (void)from; (void)seq; (void)ctx;
    if (len == 5 && memcmp(d, "HELLO", 5) == 0) ndelivered++;
}

static void quiet(hh_log_level_t lvl, const char *line, void *ctx) { (void)lvl; (void)line; (void)ctx; }

/* Waits up to 1 s for a datagram, then feeds it in. Loopback is immediate. */
static bool pump(hh_dt_t *dt, hh_dt_udp_t *u)
{
    struct pollfd p = { .fd = u->fd, .events = POLLIN };
    if (poll(&p, 1, 1000) != 1) return false;
    uint8_t buf[HH_DT_MSG_MAX];
    size_t n;
    struct sockaddr_in from;
    if (hh_dt_udp_rx(u, buf, sizeof buf, &n, &from) != HH_OK) return false;
    hh_dt_hdr_t h;
    if (hh_dt_decode(buf, n, &h, NULL) == HH_DT_RX_OK) hh_dt_udp_learn(u, h.src, &from);
    hh_dt_input(dt, buf, n);
    return true;
}

static void test_socket_options(void)
{
    hh_dt_udp_t u;
    HH_ASSERT_OK(hh_dt_udp_open(&u, "127.0.0.1", 0, 10));
    int tos = 0, pmtu = 0;
    socklen_t l = sizeof tos;
    getsockopt(u.fd, IPPROTO_IP, IP_TOS, &tos, &l);
    l = sizeof pmtu;
    getsockopt(u.fd, IPPROTO_IP, IP_MTU_DISCOVER, &pmtu, &l);
    HH_ASSERT_EQ_INT(tos, 10 << 2);
    HH_ASSERT_EQ_INT(pmtu, IP_PMTUDISC_DO);
    HH_ASSERT(u.port != 0);
    hh_dt_udp_close(&u);
    HH_ASSERT_ERR(hh_dt_udp_open(&u, "127.0.0.1", 0, 64), HH_ERR_INVAL);
}

static void test_peer_strings(void)
{
    hh_dt_udp_t u;
    HH_ASSERT_OK(hh_dt_udp_open(&u, "127.0.0.1", 0, 0));
    HH_ASSERT_OK(hh_dt_udp_add_peer_str(&u, "2=10.0.2.2", 5000));
    HH_ASSERT_OK(hh_dt_udp_add_peer_str(&u, "3=10.0.1.3:6000", 5000));
    HH_ASSERT_EQ_INT(ntohs(u.peers[0].addr.sin_port), 5000);
    HH_ASSERT_EQ_INT(ntohs(u.peers[1].addr.sin_port), 6000);
    HH_ASSERT_ERR(hh_dt_udp_add_peer_str(&u, "0=10.0.0.1", 5000), HH_ERR_INVAL);
    HH_ASSERT_ERR(hh_dt_udp_add_peer_str(&u, "4=nonsense", 5000), HH_ERR_INVAL);
    HH_ASSERT_ERR(hh_dt_udp_add_peer_str(&u, "5", 5000), HH_ERR_INVAL);
    HH_ASSERT_ERR(hh_dt_udp_add_peer_str(&u, "6=10.0.0.6:70000", 5000), HH_ERR_INVAL);
    HH_ASSERT_ERR(hh_dt_udp_tx(9, (const uint8_t *)"x", 1, &u), HH_ERR_NOTFOUND);
    HH_ASSERT_EQ_INT(u.tx_no_route, 1);
    hh_dt_udp_close(&u);
}

static void test_message_and_ack_over_loopback(void)
{
    static hh_dt_udp_t ua, ub;
    static hh_dt_t a, b;
    vclock_t vc;
    vclock_init(&vc, 0);
    hh_dt_config_t cfg;
    hh_dt_config_default(&cfg);

    HH_ASSERT_OK(hh_dt_udp_open(&ua, "127.0.0.1", 0, 0));
    HH_ASSERT_OK(hh_dt_udp_open(&ub, "127.0.0.1", 0, 0));
    /* Only the sender is told where node 2 is. The receiver learns node 1. */
    HH_ASSERT_OK(hh_dt_udp_add_peer(&ua, 2, "127.0.0.1", ub.port));
    HH_ASSERT_OK(hh_dt_init(&a, 1, &cfg, &vc.clock, hh_dt_udp_tx, &ua));
    HH_ASSERT_OK(hh_dt_init(&b, 2, &cfg, &vc.clock, hh_dt_udp_tx, &ub));
    hh_dt_set_deliver(&b, on_deliver, NULL);

    HH_ASSERT_OK(hh_dt_send(&a, 2, (const uint8_t *)"HELLO", 5));
    HH_ASSERT(pump(&b, &ub));
    HH_ASSERT(pump(&a, &ua));

    HH_ASSERT_EQ_INT(ndelivered, 1);
    HH_ASSERT_EQ_INT(hh_dt_counters(&a)->tx_acked, 1);
    HH_ASSERT(ub.peers[0].learned);
    HH_ASSERT_EQ_INT(ub.peers[0].node, 1);
    HH_ASSERT(!hh_dt_busy(&a));
    hh_dt_udp_close(&ua);
    hh_dt_udp_close(&ub);
}

HH_TEST_MAIN_BEGIN("dataterm_udp")
    hh_log_set_sink(quiet, NULL);
    HH_RUN(test_socket_options);
    HH_RUN(test_peer_strings);
    HH_RUN(test_message_and_ack_over_loopback);
    hh_log_reset();
HH_TEST_MAIN_END()
