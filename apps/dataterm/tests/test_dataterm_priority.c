/*
 * Class priority and call setup between two dataterm instances on loopback.
 * Messages leave in class order, each class on its own socket marked with its
 * DSCP, and call setup crosses as CoAP on /call. The order seen by the
 * receiver is printed, which is the priority log.
 */
#include "hh_test.h"
#include "hhsdr/core/log.h"
#include "hhsdr/dataterm/dataterm.h"
#include <arpa/inet.h>
#include <poll.h>
#include <stdio.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

static hh_dt_t       g_a, g_b;
static hh_dt_peers_t g_pa, g_pb;
static char          g_order[64];  /* as the receiver delivered them */
static int           g_n;
static char          g_sent[64];   /* as dataterm handed them to the CoAP layer */
static int           g_ns;

/* Records the class of every message sent, from the debug log: v voice,
 * m video, d data, s signalling. */
static void capture(hh_log_level_t lvl, const char *line, void *ctx)
{
    (void)lvl; (void)ctx;
    const char *c = strstr(line, " path=");
    if (!c || !(c = strstr(line, "class=")) || g_ns >= (int)sizeof g_sent - 1) return;
    c += 6;
    g_sent[g_ns++] = !strncmp(c, "voice", 5) ? 'v' : !strncmp(c, "video", 5) ? 'm'
                   : !strncmp(c, "signal", 6) ? 's' : 'd';
}
static hh_call_event_t g_ev_a[8], g_ev_b[8];
static int           g_na, g_nb;

static void on_deliver(hh_node_id_t from, const char *ip, uint16_t mid, const uint8_t *d,
                       size_t len, void *ctx)
{
    (void)from; (void)ip; (void)mid; (void)ctx;
    if (len == 1 && g_n < (int)sizeof g_order - 1) g_order[g_n++] = (char)d[0];
}

static void ev_a(const hh_call_event_t *e, void *ctx) { (void)ctx; if (g_na < 8) g_ev_a[g_na++] = *e; }
static void ev_b(const hh_call_event_t *e, void *ctx) { (void)ctx; if (g_nb < 8) g_ev_b[g_nb++] = *e; }

static int64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* Runs both instances until cond() holds or 3 s pass. */
static bool spin(bool (*cond)(void))
{
    int64_t end = now_ms() + 3000;
    while (now_ms() < end) {
        hh_dt_process(&g_a);
        hh_dt_process(&g_b);
        if (cond && cond()) return true;
        struct pollfd p[2] = { { .fd = hh_dt_fd(&g_a), .events = POLLIN },
                               { .fd = hh_dt_fd(&g_b), .events = POLLIN } };
        poll(p, 2, 5);
    }
    return cond == NULL;
}

static bool settled(void) { return !hh_dt_busy(&g_a) && !hh_dt_busy(&g_b); }
static bool a_active(void) { return g_na && g_ev_a[g_na - 1].kind == HH_CALL_EV_ACTIVE; }
static bool b_incoming(void) { return g_nb && g_ev_b[g_nb - 1].kind == HH_CALL_EV_INCOMING; }
static bool b_active(void) { return g_nb && g_ev_b[g_nb - 1].kind == HH_CALL_EV_ACTIVE; }
static bool a_ended(void) { return g_na && g_ev_a[g_na - 1].kind == HH_CALL_EV_ENDED; }
static bool b_ended(void) { return g_nb && g_ev_b[g_nb - 1].kind == HH_CALL_EV_ENDED; }

static void setup(void)
{
    hh_dt_config_t cfg;
    hh_dt_config_default(&cfg);
    g_n = g_na = g_nb = g_ns = 0;
    memset(g_order, 0, sizeof g_order);
    memset(g_sent, 0, sizeof g_sent);
    hh_dt_peers_init(&g_pa);
    hh_dt_peers_init(&g_pb);
    hh_dt_open(&g_a, "127.0.0.1", 0, 0, &cfg, &g_pa);
    hh_dt_peers_add(&g_pb, 1, "127.0.0.1", g_a.port);
    hh_dt_open(&g_b, "127.0.0.1", 0, 0, &cfg, &g_pb);
    hh_dt_peers_add(&g_pa, 2, "127.0.0.1", g_b.port);
    hh_dt_set_deliver(&g_b, on_deliver, NULL);
    hh_dt_set_call_event(&g_a, ev_a, NULL);
    hh_dt_set_call_event(&g_b, ev_b, NULL);
}

static void teardown(void)
{
    hh_dt_close(&g_a);
    hh_dt_close(&g_b);
}

static hh_status_t put(hh_dt_class_t cls, char c)
{
    return hh_dt_send_class(&g_a, 2, cls, (const uint8_t *)&c, 1);
}

static void test_classes_leave_in_priority_order(void)
{
    setup();
    HH_ASSERT_OK(put(HH_DT_CLASS_DATA, 'd'));
    HH_ASSERT_OK(put(HH_DT_CLASS_DATA, 'D'));
    HH_ASSERT_OK(put(HH_DT_CLASS_VIDEO, 'm'));
    HH_ASSERT_OK(put(HH_DT_CLASS_VOICE, 'v'));
    HH_ASSERT_OK(put(HH_DT_CLASS_VOICE, 'V'));
    printf("    queued: data d, data D, video m, voice v, voice V\n");
    HH_ASSERT(spin(settled));
    printf("    sent order (class): %s\n    receiver order: %s\n", g_sent, g_order);
    HH_ASSERT_EQ_STR(g_sent, "vvmdd");
    /* CoAP allows one outstanding message per session (NSTART 1), so the second
     * voice message waits for the first one's ACK. What always holds: the first
     * message of each class arrives in class order, and a class stays in order. */
    HH_ASSERT_EQ_INT(g_n, 5);
    HH_ASSERT(strchr(g_order, 'v') < strchr(g_order, 'm'));
    HH_ASSERT(strchr(g_order, 'm') < strchr(g_order, 'd'));
    HH_ASSERT(strchr(g_order, 'v') < strchr(g_order, 'V'));
    HH_ASSERT(strchr(g_order, 'd') < strchr(g_order, 'D'));
    teardown();
}

static void test_data_still_leaves_under_voice_flood(void)
{
    setup();
    for (int i = 0; i < 8; i++) HH_ASSERT_OK(put(HH_DT_CLASS_VOICE, 'V'));
    for (int i = 0; i < 3; i++) HH_ASSERT_OK(put(HH_DT_CLASS_DATA, 'd'));
    HH_ASSERT(spin(settled));
    printf("    sent order (class): %s\n    receiver order: %s\n", g_sent, g_order);
    /* Data gets a send after every 7 higher-class sends while it waits. */
    HH_ASSERT_EQ_STR(g_sent, "vvvvvvvdvdd");
    HH_ASSERT_EQ_INT(g_n, 11);  /* nothing lost */
    teardown();
}

static int tos_of(int fd)
{
    int tos = -1;
    socklen_t l = sizeof tos;
    getsockopt(fd, IPPROTO_IP, IP_TOS, &tos, &l);
    return tos;
}

static void test_each_class_has_its_own_marked_socket(void)
{
    setup();
    HH_ASSERT_OK(put(HH_DT_CLASS_VOICE, 'v'));
    HH_ASSERT_OK(put(HH_DT_CLASS_VIDEO, 'm'));
    HH_ASSERT_OK(put(HH_DT_CLASS_DATA, 'd'));
    HH_ASSERT_OK(hh_dt_call_request(&g_a, 2));
    HH_ASSERT(spin(settled));
    int seen[4] = { 0 }, total = 0;
    const int want[4] = { 46 << 2, 34 << 2, 40 << 2, 0 };
    for (int fd = 0; fd < 1024; fd++) {
        struct sockaddr_in p;
        socklen_t pl = sizeof p;
        int type = 0;
        socklen_t tl = sizeof type;
        if (getsockopt(fd, SOL_SOCKET, SO_TYPE, &type, &tl) != 0 || type != SOCK_DGRAM) continue;
        if (getpeername(fd, (struct sockaddr *)&p, &pl) != 0 || p.sin_family != AF_INET ||
            ntohs(p.sin_port) != g_b.port)
            continue;
        total++;
        for (int i = 0; i < 4; i++)
            if (tos_of(fd) == want[i]) seen[i]++;
    }
    printf("    sockets to the peer: %d, tos EF=%d AF41=%d CS5=%d default=%d\n", total, seen[0],
           seen[1], seen[2], seen[3]);
    HH_ASSERT_EQ_INT(total, 4);
    for (int i = 0; i < 4; i++) HH_ASSERT_EQ_INT(seen[i], 1);
    teardown();
}

static void test_call_setup_over_coap(void)
{
    setup();
    HH_ASSERT_OK(hh_dt_call_request(&g_a, 2));
    HH_ASSERT(spin(b_incoming));
    HH_ASSERT_EQ_INT(hh_dt_call(&g_b)->state, HH_CALL_RINGING);
    HH_ASSERT_OK(hh_dt_call_accept(&g_b));
    HH_ASSERT(spin(a_active));
    HH_ASSERT(spin(b_active));
    printf("    call up: a=%s b=%s\n", hh_call_state_str(hh_dt_call(&g_a)->state),
           hh_call_state_str(hh_dt_call(&g_b)->state));
    HH_ASSERT_OK(hh_dt_call_release(&g_a));
    HH_ASSERT(spin(b_ended));
    HH_ASSERT_EQ_INT(g_ev_b[g_nb - 1].reason, HH_CALL_REASON_NORMAL);
    HH_ASSERT_EQ_INT(hh_dt_call(&g_a)->state, HH_CALL_IDLE);
    HH_ASSERT_EQ_INT(hh_dt_call(&g_b)->state, HH_CALL_IDLE);
    HH_ASSERT(spin(settled));
    /* A second call works: nothing was left behind. */
    g_na = g_nb = 0;
    HH_ASSERT_OK(hh_dt_call_request(&g_a, 2));
    HH_ASSERT(spin(b_incoming));
    HH_ASSERT_OK(hh_dt_call_reject(&g_b));
    HH_ASSERT(spin(a_ended));
    HH_ASSERT_EQ_INT(g_ev_a[g_na - 1].reason, HH_CALL_REASON_DECLINED);
    printf("    second call rejected, both idle again\n");
    teardown();
}

static void test_limits_and_refusals(void)
{
    setup();
    HH_ASSERT_ERR(put(HH_DT_CLASS_SIGNAL, 'x'), HH_ERR_INVAL);
    HH_ASSERT_ERR(hh_dt_send_class(&g_a, 9, HH_DT_CLASS_DATA, (const uint8_t *)"x", 1),
                  HH_ERR_NOTFOUND);
    HH_ASSERT_ERR(hh_dt_call_request(&g_a, 9), HH_ERR_NOTFOUND);
    for (unsigned i = 0; i < HH_DT_PENDING_LEN; i++) HH_ASSERT_OK(put(HH_DT_CLASS_DATA, 'd'));
    HH_ASSERT_ERR(put(HH_DT_CLASS_DATA, 'd'), HH_ERR_AGAIN);
    HH_ASSERT_EQ_INT(hh_dt_counters(&g_a)->tx_pending_full, 1);
    HH_ASSERT(hh_dt_busy(&g_a));
    teardown();
}

HH_TEST_MAIN_BEGIN("dataterm priority and calls")
    hh_log_set_sink(capture, NULL);
    hh_log_set_level(HH_LOG_DEBUG);
    HH_RUN(test_classes_leave_in_priority_order);
    HH_RUN(test_data_still_leaves_under_voice_flood);
    HH_RUN(test_each_class_has_its_own_marked_socket);
    HH_RUN(test_call_setup_over_coap);
    HH_RUN(test_limits_and_refusals);
    hh_log_reset();
HH_TEST_MAIN_END()
