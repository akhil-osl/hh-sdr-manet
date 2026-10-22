/*
 * dataterm: send and receive text messages over UDP with ACK and retry.
 *
 *   dataterm -n ID [options] recv
 *   dataterm -n ID [options] chat DEST
 *   dataterm -n ID [options] send DEST --count N [--interval MS] [--size B]
 */
#include "hhsdr/core/log.h"
#include "hhsdr/dataterm/dataterm.h"
#include "hhsdr/dataterm/udp.h"
#include <errno.h>
#include <getopt.h>
#include <inttypes.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define COMP "dataterm"
#define DEFAULT_PORT 5000

static volatile sig_atomic_t g_stop;
static void on_signal(int sig) { (void)sig; g_stop = 1; }

typedef enum { MODE_RECV, MODE_CHAT, MODE_SEND } app_mode_t;

typedef struct {
    bool     quiet;
    uint64_t sent_done;
    uint64_t sent_acked;
} app_t;

static void usage(FILE *f)
{
    fprintf(f,
        "usage: dataterm -n ID [options] recv\n"
        "       dataterm -n ID [options] chat DEST\n"
        "       dataterm -n ID [options] send DEST --count N [--interval MS] [--size B]\n"
        "\n"
        "  -n, --node ID         this node's id (required)\n"
        "  -p, --port PORT       UDP port to bind and default peer port (5000)\n"
        "  -b, --bind IP         local address to bind (all)\n"
        "  -P, --peer ID=IP[:PORT]  node id to address, repeatable\n"
        "  -t, --timeout MS      first ACK wait, doubles per retry (1000)\n"
        "  -r, --retries N       retries per message, 0 = retry off (4)\n"
        "  -m, --max-payload B   largest message, bytes (1232)\n"
        "  -d, --dscp D          DSCP on sent packets, 0 to 63 (0)\n"
        "  -c, --count N         send: messages to send\n"
        "  -i, --interval MS     send: gap between messages (100)\n"
        "  -s, --size B          send: payload size (32)\n"
        "  -D, --duration S      recv: stop after S seconds (run until Ctrl-C)\n"
        "  -q, --quiet           do not print received messages\n"
        "  -v, --log-level L     error|warn|info|debug|trace (info)\n");
}

static bool parse_u64(const char *s, uint64_t max, uint64_t *out)
{
    char *end = NULL;
    errno = 0;
    unsigned long long v = strtoull(s, &end, 0);
    if (!s[0] || *end || errno || v > max) return false;
    *out = v;
    return true;
}

static void on_deliver(hh_node_id_t from, hh_seq_t seq, const uint8_t *data, size_t len,
                       void *ctx)
{
    app_t *app = ctx;
    if (app->quiet) return;
    printf("[%u #%" PRIu32 "] ", from, seq);
    for (size_t i = 0; i < len; i++) {
        unsigned char ch = data[i];
        putchar(ch >= 0x20 && ch < 0x7f ? ch : '.');
    }
    putchar('\n');
    fflush(stdout);
}

static void on_result(hh_node_id_t to, hh_seq_t seq, bool acked, uint8_t attempts, void *ctx)
{
    app_t *app = ctx;
    app->sent_done++;
    if (acked) app->sent_acked++;
    HH_LOGD(COMP, "result", "to=%u seq=%" PRIu32 " acked=%d attempts=%u", to, seq,
            acked, attempts);
}

static void drain_socket(hh_dt_t *dt, hh_dt_udp_t *u)
{
    uint8_t buf[HH_DT_MSG_MAX];
    for (;;) {
        size_t n = 0;
        struct sockaddr_in from;
        hh_status_t s = hh_dt_udp_rx(u, buf, sizeof buf, &n, &from);
        if (s == HH_ERR_AGAIN || s == HH_ERR_IO || s == HH_ERR_INVAL) return;
        if (s != HH_OK) continue;
        hh_dt_hdr_t h;
        /* Learn the reply address before the engine sends its ACK. */
        if (hh_dt_decode(buf, n, &h, NULL) == HH_DT_RX_OK && h.dst == dt->self)
            hh_dt_udp_learn(u, h.src, &from);
        hh_dt_input(dt, buf, n);
    }
}

static void chat_line(hh_dt_t *dt, hh_node_id_t dest, const char *line, size_t len)
{
    if (len == 0) return;
    hh_status_t s = hh_dt_send(dt, dest, (const uint8_t *)line, len);
    if (s == HH_ERR_AGAIN) fprintf(stderr, "busy: line not sent, try again\n");
    else if (s == HH_ERR_INVAL) fprintf(stderr, "too long: %zu bytes, limit %u\n", len,
                                        dt->cfg.max_payload);
    else if (s != HH_OK) fprintf(stderr, "not sent: %s\n", hh_status_str(s));
}

static void log_summary(const hh_dt_t *dt, const hh_dt_udp_t *u)
{
    const hh_dt_counters_t *c = hh_dt_counters(dt);
    hh_dt_log_stats(dt);
    if (c->tx_data) {
        HH_LOGI(COMP, "summary_tx",
                "sent=%" PRIu64 " acked=%" PRIu64 " given_up=%" PRIu64
                " first_try_loss_pct=%.2f residual_loss_pct=%.2f no_route=%" PRIu64,
                c->tx_data, c->tx_acked, c->tx_given_up,
                100.0 * (double)(c->tx_data - c->tx_acked_first_try) / (double)c->tx_data,
                100.0 * (double)c->tx_given_up / (double)c->tx_data, u->tx_no_route);
    }
    if (c->rx_delivered || c->rx_gaps) {
        HH_LOGI(COMP, "summary_rx",
                "delivered=%" PRIu64 " missing=%" PRIu64 " loss_pct=%.2f duplicates=%" PRIu64
                " bad=%" PRIu64,
                c->rx_delivered, c->rx_gaps,
                100.0 * (double)c->rx_gaps / (double)(c->rx_delivered + c->rx_gaps),
                c->rx_duplicates, c->rx_bad);
    }
}

int main(int argc, char **argv)
{
    static const struct option opts[] = {
        { "node", required_argument, 0, 'n' },     { "port", required_argument, 0, 'p' },
        { "bind", required_argument, 0, 'b' },     { "peer", required_argument, 0, 'P' },
        { "timeout", required_argument, 0, 't' },  { "retries", required_argument, 0, 'r' },
        { "max-payload", required_argument, 0, 'm' }, { "dscp", required_argument, 0, 'd' },
        { "count", required_argument, 0, 'c' },    { "interval", required_argument, 0, 'i' },
        { "size", required_argument, 0, 's' },     { "duration", required_argument, 0, 'D' },
        { "quiet", no_argument, 0, 'q' },          { "log-level", required_argument, 0, 'v' },
        { "help", no_argument, 0, 'h' },           { 0, 0, 0, 0 }
    };

    hh_dt_config_t cfg;
    hh_dt_config_default(&cfg);
    uint64_t node = 0, port = DEFAULT_PORT, dscp = 0, count = 0, interval = 100,
             size = 32, duration = 0, v;
    const char *bind_ip = NULL;
    const char *peers[HH_DT_MAX_PEERS];
    int npeers = 0;
    app_t app = { 0 };
    hh_log_level_t lvl;

    int ch;
    while ((ch = getopt_long(argc, argv, "n:p:b:P:t:r:m:d:c:i:s:D:qv:h", opts, NULL)) != -1) {
        bool ok = true;
        switch (ch) {
        case 'n': ok = parse_u64(optarg, UINT32_MAX, &node) && node; break;
        case 'p': ok = parse_u64(optarg, 65535, &port) && port; break;
        case 'b': bind_ip = optarg; break;
        case 'P':
            ok = npeers < HH_DT_MAX_PEERS;
            if (ok) peers[npeers++] = optarg;
            break;
        case 't': ok = parse_u64(optarg, 600000, &v) && v; cfg.timeout_ms = (uint32_t)v; break;
        case 'r': ok = parse_u64(optarg, 255, &v); cfg.max_retries = (uint8_t)v; break;
        case 'm': ok = parse_u64(optarg, HH_DT_PAYLOAD_MAX, &v); cfg.max_payload = (uint16_t)v; break;
        case 'd': ok = parse_u64(optarg, 63, &dscp); break;
        case 'c': ok = parse_u64(optarg, UINT32_MAX, &count); break;
        case 'i': ok = parse_u64(optarg, 3600000, &interval); break;
        case 's': ok = parse_u64(optarg, HH_DT_PAYLOAD_MAX, &size); break;
        case 'D': ok = parse_u64(optarg, 31536000, &duration); break;
        case 'q': app.quiet = true; break;
        case 'v': ok = hh_log_level_parse(optarg, &lvl); if (ok) hh_log_set_level(lvl); break;
        case 'h': usage(stdout); return 0;
        default: usage(stderr); return 2;
        }
        if (!ok) {
            fprintf(stderr, "dataterm: bad value for -%c: %s\n", ch, optarg ? optarg : "");
            return 2;
        }
    }
    if (!node || optind >= argc) { usage(stderr); return 2; }

    app_mode_t mode;
    uint64_t dest = 0;
    const char *m = argv[optind++];
    if (!strcmp(m, "recv")) mode = MODE_RECV;
    else if (!strcmp(m, "chat")) mode = MODE_CHAT;
    else if (!strcmp(m, "send")) mode = MODE_SEND;
    else { usage(stderr); return 2; }
    if (mode != MODE_RECV) {
        if (optind >= argc || !parse_u64(argv[optind], UINT32_MAX, &dest) || !dest) {
            fprintf(stderr, "dataterm: %s needs a destination node id\n", m);
            return 2;
        }
        optind++;
    }
    if (mode == MODE_SEND && !count) {
        fprintf(stderr, "dataterm: send needs --count\n");
        return 2;
    }
    if (mode == MODE_SEND && size > cfg.max_payload) {
        fprintf(stderr, "dataterm: --size %" PRIu64 " is over the limit %u\n", size,
                cfg.max_payload);
        return 2;
    }

    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_signal;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    static hh_dt_udp_t udp;
    static hh_dt_t dt;
    if (hh_dt_udp_open(&udp, bind_ip, (uint16_t)port, (int)dscp) != HH_OK) return 1;
    for (int i = 0; i < npeers; i++) {
        if (hh_dt_udp_add_peer_str(&udp, peers[i], (uint16_t)port) != HH_OK) {
            fprintf(stderr, "dataterm: bad --peer %s (want ID=IP or ID=IP:PORT)\n", peers[i]);
            return 2;
        }
    }
    const hh_clock_t *clk = hh_clock_monotonic();
    if (hh_dt_init(&dt, (hh_node_id_t)node, &cfg, clk, hh_dt_udp_tx, &udp) != HH_OK) return 1;
    hh_dt_set_deliver(&dt, on_deliver, &app);
    hh_dt_set_result(&dt, on_result, &app);

    hh_time_ms_t start = hh_now(clk);
    hh_time_ms_t next_send = start;
    uint64_t queued = 0;
    bool input_done = (mode != MODE_CHAT);
    char line[HH_DT_PAYLOAD_MAX + 2];
    size_t line_len = 0;
    bool line_skip = false;
    uint8_t payload[HH_DT_PAYLOAD_MAX];

    HH_LOGI(COMP, "start", "mode=%s node=%" PRIu64 " dest=%" PRIu64 " port=%u", m, node,
            dest, udp.port);

    while (!g_stop) {
        hh_time_ms_t now = hh_now(clk);

        if (mode == MODE_SEND) {
            while (queued < count && now >= next_send) {
                int k = snprintf((char *)payload, sizeof payload, "msg %" PRIu64 " ", queued + 1);
                size_t n = (size_t)k < size ? (size_t)k : size;
                for (size_t j = n; j < size; j++) payload[j] = '.';
                hh_status_t s = hh_dt_send(&dt, (hh_node_id_t)dest, payload, size);
                if (s == HH_ERR_AGAIN) break;
                if (s != HH_OK) { g_stop = 1; break; }
                queued++;
                next_send += interval;
            }
            if (queued == count && app.sent_done == count) break;
        } else if (mode == MODE_CHAT && input_done && !hh_dt_busy(&dt)) {
            break;
        }
        if (mode == MODE_RECV && duration && now - start >= duration * 1000) break;

        int64_t wait = hh_dt_next_timeout_ms(&dt);
        if (wait < 0 || wait > 1000) wait = 1000;
        if (mode == MODE_SEND && queued < count) {
            int64_t ds = next_send > now ? (int64_t)(next_send - now) : 0;
            if (ds < wait) wait = ds;
        }

        struct pollfd pfd[2] = { { .fd = udp.fd, .events = POLLIN },
                                 { .fd = STDIN_FILENO, .events = POLLIN } };
        int nfds = (mode == MODE_CHAT && !input_done) ? 2 : 1;
        int r = poll(pfd, (nfds_t)nfds, (int)wait);
        if (r < 0 && errno != EINTR) {
            HH_LOGE(COMP, "poll_failed", "errno=%d (%s)", errno, strerror(errno));
            break;
        }
        if (r > 0 && (pfd[0].revents & POLLIN)) drain_socket(&dt, &udp);
        if (r > 0 && nfds == 2 && (pfd[1].revents & (POLLIN | POLLHUP))) {
            ssize_t n = read(STDIN_FILENO, line + line_len, sizeof line - line_len);
            if (n <= 0) {
                if (line_len && !line_skip) chat_line(&dt, (hh_node_id_t)dest, line, line_len);
                input_done = true;
            } else {
                line_len += (size_t)n;
                char *nl;
                while ((nl = memchr(line, '\n', line_len)) != NULL) {
                    size_t l = (size_t)(nl - line);
                    if (!line_skip) chat_line(&dt, (hh_node_id_t)dest, line, l);
                    line_skip = false;
                    memmove(line, nl + 1, line_len - l - 1);
                    line_len -= l + 1;
                }
                if (line_len == sizeof line) {
                    fprintf(stderr, "too long: line over %zu bytes dropped\n", sizeof line);
                    line_len = 0;
                    line_skip = true;
                }
            }
        }
        hh_dt_poll(&dt);
    }

    log_summary(&dt, &udp);
    hh_dt_udp_close(&udp);
    return 0;
}
