#include "hhsdr/dataterm/dataterm.h"
#include "hhsdr/core/log.h"
#include <arpa/inet.h>
#include <coap3/coap.h>
#include <inttypes.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>

#define COMP "dataterm"

/* 1500 minus IPv4 20 and UDP 8. Don't-fragment makes the kernel refuse more. */
#define DT_COAP_MTU 1472u
#define DT_MAX_FDS  1024
/* A Message ID further ahead than this means the sender restarted. */
#define DT_MID_JUMP 1000u

/* libcoap after 4.3.5 deprecates the one-argument setter. No release has the
 * new one yet, so CMake checks for it. */
#ifdef HH_COAP_HAS_APP_DATA2
#define dt_session_set_app_data(s, d) coap_session_set_app_data2((s), (d), NULL)
#else
#define dt_session_set_app_data(s, d) coap_session_set_app_data((s), (d))
#endif

static int     g_users;
/* Sockets already seen, by inode, so each new libcoap socket is marked once. */
static ino_t   g_seen_ino[DT_MAX_FDS];

void hh_dt_config_default(hh_dt_config_t *cfg)
{
    if (!cfg) return;
    cfg->ack_timeout_ms = 2000;  /* RFC 7252 ACK_TIMEOUT */
    cfg->max_retransmit = 4;     /* RFC 7252 MAX_RETRANSMIT */
    /* 1280-byte IP packet minus IPv4 20, UDP 8 and the CoAP header 20. */
    cfg->max_payload = 1232;
}

static void coap_log_to_hh(coap_log_t level, const char *message)
{
    char buf[256];
    size_t n = strlen(message);
    if (n >= sizeof buf) n = sizeof buf - 1;
    memcpy(buf, message, n);
    while (n && (buf[n - 1] == '\n' || buf[n - 1] == '\r')) n--;
    buf[n] = '\0';
    if (level <= LOG_ERR) HH_LOGE(COMP, "libcoap", "msg=\"%s\"", buf);
    else HH_LOGW(COMP, "libcoap", "msg=\"%s\"", buf);
}

/* Sets DSCP and don't-fragment on every UDP socket opened since the last
 * call, or only records them when mark is false. libcoap gives no access to
 * its sockets, so they are found here. Returns the local port of the last
 * new socket, or 0. */
static uint16_t scan_new_sockets(bool mark, int dscp)
{
    uint16_t port = 0;
    for (int fd = 0; fd < DT_MAX_FDS; fd++) {
        struct stat st;
        if (fstat(fd, &st) != 0 || !S_ISSOCK(st.st_mode)) continue;
        if (g_seen_ino[fd] == st.st_ino) continue;
        if (!mark) {
            g_seen_ino[fd] = st.st_ino;
            continue;
        }
        int type = 0;
        socklen_t l = sizeof type;
        struct sockaddr_in a;
        socklen_t al = sizeof a;
        if (getsockopt(fd, SOL_SOCKET, SO_TYPE, &type, &l) != 0 || type != SOCK_DGRAM ||
            getsockname(fd, (struct sockaddr *)&a, &al) != 0 || a.sin_family != AF_INET)
            continue;
        g_seen_ino[fd] = st.st_ino;
        int tos = dscp << 2;
        int pmtu = IP_PMTUDISC_DO;
        if (setsockopt(fd, IPPROTO_IP, IP_TOS, &tos, sizeof tos) < 0 ||
            setsockopt(fd, IPPROTO_IP, IP_MTU_DISCOVER, &pmtu, sizeof pmtu) < 0)
            HH_LOGW(COMP, "socket_mark_failed", "fd=%d", fd);
        port = ntohs(a.sin_port);
        HH_LOGD(COMP, "socket_marked", "fd=%d port=%u dscp=%d df=on", fd, port, dscp);
    }
    return port;
}

static hh_dt_rx_peer_t *rx_find(hh_dt_t *dt, const struct sockaddr_in *a)
{
    for (int i = 0; i < HH_DT_MAX_PEERS; i++)
        if (dt->rx[i].used && dt->rx[i].addr.sin_addr.s_addr == a->sin_addr.s_addr)
            return &dt->rx[i];
    return NULL;
}

static hh_dt_rx_peer_t *rx_new(hh_dt_t *dt, const struct sockaddr_in *a)
{
    for (int i = 0; i < HH_DT_MAX_PEERS; i++) {
        if (!dt->rx[i].used) {
            memset(&dt->rx[i], 0, sizeof dt->rx[i]);
            dt->rx[i].used = true;
            dt->rx[i].addr = *a;
            dt->rx[i].node = hh_dt_peers_node_of(dt->peers, a);
            return &dt->rx[i];
        }
    }
    return NULL;
}

/* Returns true if the message is new and must be delivered. */
static bool rx_track(hh_dt_t *dt, hh_dt_rx_peer_t *r, uint16_t mid, bool first)
{
    if (first) {
        r->last_mid = mid;
        r->window = 1;
        return true;
    }
    uint16_t ahead = (uint16_t)(mid - r->last_mid);
    if (ahead == 0) return false;
    if (ahead < 0x8000u) {
        if (ahead > DT_MID_JUMP) {
            dt->counters.rx_resync++;
            HH_LOGW(COMP, "rx_resync", "node=%u last_mid=%u mid=%u", r->node, r->last_mid, mid);
            r->window = 1;
        } else {
            r->gaps += ahead - 1u;
            dt->counters.rx_gaps += ahead - 1u;
            if (ahead > 1)
                HH_LOGI(COMP, "gap", "node=%u missing=%u last_mid=%u mid=%u", r->node,
                        ahead - 1u, r->last_mid, mid);
            r->window = ahead >= HH_DT_RX_WINDOW ? 1u : (r->window << ahead) | 1u;
        }
        r->last_mid = mid;
        return true;
    }
    uint16_t back = (uint16_t)(r->last_mid - mid);
    if (back < HH_DT_RX_WINDOW) {
        if (r->window & (1u << back)) return false;
        /* Late arrival of one counted as missing. */
        r->window |= 1u << back;
        if (r->gaps) r->gaps--;
        if (dt->counters.rx_gaps) dt->counters.rx_gaps--;
        return true;
    }
    dt->counters.rx_resync++;
    HH_LOGW(COMP, "rx_resync", "node=%u last_mid=%u mid=%u", r->node, r->last_mid, mid);
    r->last_mid = mid;
    r->window = 1;
    return true;
}

static void on_post(coap_resource_t *res, coap_session_t *session, const coap_pdu_t *req,
                    const coap_string_t *query, coap_pdu_t *resp)
{
    (void)query;
    hh_dt_t *dt = coap_resource_get_userdata(res);
    const coap_address_t *ra = coap_session_get_addr_remote(session);
    if (!dt || !ra || ra->addr.sa.sa_family != AF_INET) {
        coap_pdu_set_code(resp, COAP_RESPONSE_CODE_INTERNAL_ERROR);
        return;
    }
    const struct sockaddr_in *from = &ra->addr.sin;
    char ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &from->sin_addr, ip, sizeof ip);

    coap_opt_iterator_t it;
    coap_opt_t *cf = coap_check_option(req, COAP_OPTION_CONTENT_FORMAT, &it);
    if (cf && coap_decode_var_bytes(coap_opt_value(cf), coap_opt_length(cf)) !=
                  COAP_MEDIATYPE_TEXT_PLAIN) {
        dt->counters.rx_bad_format++;
        HH_LOGW(COMP, "rx_refused", "ip=%s reason=content_format", ip);
        coap_pdu_set_code(resp, COAP_RESPONSE_CODE_UNSUPPORTED_CONTENT_FORMAT);
        return;
    }

    dt->counters.rx_data++;
    hh_dt_rx_peer_t *r = rx_find(dt, from);
    bool first = !r;
    if (first) {
        r = rx_new(dt, from);
        if (!r) {
            dt->counters.rx_table_full++;
            coap_pdu_set_code(resp, COAP_RESPONSE_CODE_SERVICE_UNAVAILABLE);
            return;
        }
        if (r->node == HH_NODE_ID_INVALID) {
            dt->counters.rx_unknown_sender++;
            HH_LOGI(COMP, "rx_unknown_sender", "ip=%s", ip);
        }
    } else if (r->addr.sin_port != from->sin_port) {
        /* A new run of the sender: new session, new port, new Message IDs. */
        dt->counters.rx_resync++;
        HH_LOGI(COMP, "rx_sender_restart", "node=%u ip=%s port=%u", r->node, ip,
                ntohs(from->sin_port));
        r->addr = *from;
        first = true;
    }

    uint16_t mid = (uint16_t)coap_pdu_get_mid(req);
    size_t len = 0;
    const uint8_t *data = NULL;
    if (!coap_get_data(req, &len, &data)) len = 0;
    if (rx_track(dt, r, mid, first)) {
        r->delivered++;
        dt->counters.rx_delivered++;
        HH_LOGD(COMP, "rx", "node=%u ip=%s mid=%u len=%zu", r->node, ip, mid, len);
        if (dt->deliver) dt->deliver(r->node, ip, mid, data, len, dt->deliver_ctx);
    } else {
        r->duplicates++;
        dt->counters.rx_duplicates++;
        HH_LOGD(COMP, "rx_duplicate", "node=%u ip=%s mid=%u", r->node, ip, mid);
    }
    /* A duplicate is acknowledged again, so the sender stops resending. */
    coap_pdu_set_code(resp, COAP_RESPONSE_CODE_CHANGED);
}

static void finish(hh_dt_tx_peer_t *p, uint16_t mid, bool acked, const char *reason)
{
    hh_dt_t *dt = p->dt;
    if (p->outstanding) p->outstanding--;
    HH_LOGD(COMP, "result", "to=%u mid=%u acked=%d reason=%s", p->node, mid, acked, reason);
    if (dt->result) dt->result(p->node, mid, acked, reason, dt->result_ctx);
}

/* The NON wait whose token the reply carries, or NULL for a CON reply. */
static hh_dt_non_t *non_match(hh_dt_tx_peer_t *p, const coap_pdu_t *received)
{
    coap_bin_const_t tok = coap_pdu_get_token(received);
    if (tok.length != sizeof p->non[0].token) return NULL;
    for (unsigned i = 0; i < HH_DT_QUEUE_LEN; i++)
        if (p->non[i].used && memcmp(p->non[i].token, tok.s, tok.length) == 0)
            return &p->non[i];
    return NULL;
}

static coap_response_t on_response(coap_session_t *session, const coap_pdu_t *sent,
                                   const coap_pdu_t *received, const coap_mid_t mid_in)
{
    (void)sent;
    hh_dt_tx_peer_t *p = coap_session_get_app_data(session);
    if (!p) return COAP_RESPONSE_OK;
    coap_mid_t mid = mid_in;
    if (coap_pdu_get_type(received) == COAP_MESSAGE_NON) {
        hh_dt_non_t *n = non_match(p, received);
        if (!n) return COAP_RESPONSE_OK;  /* late reply after give-up */
        n->used = false;
        mid = n->mid;
    }
    coap_pdu_code_t code = coap_pdu_get_code(received);
    if (code == COAP_RESPONSE_CODE_CHANGED) {
        p->dt->counters.tx_acked++;
        finish(p, (uint16_t)mid, true, "acked");
    } else {
        p->dt->counters.tx_refused++;
        HH_LOGW(COMP, "tx_refused", "to=%u mid=%d code=%u.%02u", p->node, mid,
                COAP_RESPONSE_CLASS(code), code & 0x1fu);
        finish(p, (uint16_t)mid, false, "refused");
    }
    return COAP_RESPONSE_OK;
}

static void on_nack(coap_session_t *session, const coap_pdu_t *sent,
                    const coap_nack_reason_t reason, const coap_mid_t mid)
{
    (void)sent;
    hh_dt_tx_peer_t *p = coap_session_get_app_data(session);
    if (!p) return;
    switch (reason) {
    case COAP_NACK_TOO_MANY_RETRIES:
        p->dt->counters.tx_given_up++;
        HH_LOGW(COMP, "give_up", "to=%u mid=%d", p->node, mid);
        finish(p, (uint16_t)mid, false, "timeout");
        break;
    case COAP_NACK_RST:
        p->dt->counters.tx_rst++;
        HH_LOGW(COMP, "tx_rst", "to=%u mid=%d", p->node, mid);
        finish(p, (uint16_t)mid, false, "rst");
        break;
    default:
        p->dt->counters.tx_unreachable++;
        HH_LOGW(COMP, "tx_unreachable", "to=%u mid=%d reason=%d", p->node, mid, (int)reason);
        finish(p, (uint16_t)mid, false, "unreachable");
        break;
    }
}

hh_status_t hh_dt_open(hh_dt_t *dt, const char *bind_ip, uint16_t port, int dscp,
                       const hh_dt_config_t *cfg, const hh_dt_peers_t *peers)
{
    if (!dt || !cfg || !peers || dscp < 0 || dscp > 63) return HH_ERR_INVAL;
    if (cfg->max_payload > HH_DT_PAYLOAD_MAX) return HH_ERR_INVAL;
    if (cfg->ack_timeout_ms < HH_DT_ACK_TIMEOUT_MIN_MS) {
        /* libcoap ignores a smaller value and keeps 2 s. */
        HH_LOGE(COMP, "bad_config", "ack_timeout_ms=%u min=%u", cfg->ack_timeout_ms,
                HH_DT_ACK_TIMEOUT_MIN_MS);
        return HH_ERR_INVAL;
    }
    memset(dt, 0, sizeof *dt);
    dt->cfg = *cfg;
    dt->dscp = dscp;
    dt->peers = peers;
    dt->clock = hh_clock_monotonic();

    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(port) };
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind_ip && inet_pton(AF_INET, bind_ip, &a.sin_addr) != 1) return HH_ERR_INVAL;

    if (g_users++ == 0) {
        coap_startup();
        coap_set_log_handler(coap_log_to_hh);
        coap_set_log_level(LOG_WARNING);
    }
    scan_new_sockets(false, 0);
    dt->ctx = coap_new_context(NULL);
    if (!dt->ctx) goto fail;

    coap_address_t ea;
    coap_address_init(&ea);
    ea.addr.sin = a;
    ea.size = sizeof a;
    if (!coap_new_endpoint(dt->ctx, &ea, COAP_PROTO_UDP)) {
        HH_LOGE(COMP, "endpoint_failed", "port=%u", port);
        goto fail;
    }
    dt->port = scan_new_sockets(true, dscp);

    coap_resource_t *res = coap_resource_init(coap_make_str_const("msg"), 0);
    if (!res) goto fail;
    coap_resource_set_userdata(res, dt);
    coap_register_handler(res, COAP_REQUEST_POST, on_post);
    coap_add_resource(dt->ctx, res);
    coap_register_response_handler(dt->ctx, on_response);
    coap_register_nack_handler(dt->ctx, on_nack);

    HH_LOGI(COMP, "opened", "port=%u dscp=%d df=on ack_timeout_ms=%u retransmit=%u "
            "max_payload=%u", dt->port, dscp, cfg->ack_timeout_ms, cfg->max_retransmit,
            cfg->max_payload);
    return HH_OK;

fail:
    hh_dt_close(dt);
    return HH_ERR_IO;
}

void hh_dt_close(hh_dt_t *dt)
{
    if (!dt || g_users == 0) return;
    if (dt->ctx) {
        for (int i = 0; i < HH_DT_MAX_PEERS; i++)
            if (dt->tx[i].session) dt_session_set_app_data(dt->tx[i].session, NULL);
        coap_free_context(dt->ctx);
        dt->ctx = NULL;
    }
    if (--g_users == 0) coap_cleanup();
}

void hh_dt_set_deliver(hh_dt_t *dt, hh_dt_deliver_fn fn, void *ctx)
{
    if (!dt) return;
    dt->deliver = fn;
    dt->deliver_ctx = ctx;
}

void hh_dt_set_result(hh_dt_t *dt, hh_dt_result_fn fn, void *ctx)
{
    if (!dt) return;
    dt->result = fn;
    dt->result_ctx = ctx;
}

static hh_dt_tx_peer_t *tx_get(hh_dt_t *dt, const hh_dt_peer_addr_t *pa)
{
    for (int i = 0; i < HH_DT_MAX_PEERS; i++)
        if (dt->tx[i].used && dt->tx[i].node == pa->node) return &dt->tx[i];
    hh_dt_tx_peer_t *p = NULL;
    for (int i = 0; i < HH_DT_MAX_PEERS && !p; i++)
        if (!dt->tx[i].used) p = &dt->tx[i];
    if (!p) return NULL;

    coap_address_t dst;
    coap_address_init(&dst);
    dst.addr.sin = pa->addr;
    dst.size = sizeof pa->addr;
    coap_session_t *s = coap_new_client_session(dt->ctx, NULL, &dst, COAP_PROTO_UDP);
    if (!s) {
        HH_LOGE(COMP, "session_failed", "to=%u", pa->node);
        return NULL;
    }
    scan_new_sockets(true, dt->dscp);
    coap_fixed_point_t t = { (uint16_t)(dt->cfg.ack_timeout_ms / 1000u),
                             (uint16_t)(dt->cfg.ack_timeout_ms % 1000u) };
    coap_session_set_ack_timeout(s, t);
    coap_session_set_max_retransmit(s, dt->cfg.max_retransmit);
    coap_session_set_mtu(s, DT_COAP_MTU);

    memset(p, 0, sizeof *p);
    p->used = true;
    p->node = pa->node;
    p->session = s;
    p->dt = dt;
    dt_session_set_app_data(s, p);
    return p;
}

hh_status_t hh_dt_send(hh_dt_t *dt, hh_node_id_t to, const uint8_t *data, size_t len)
{
    if (!dt || !dt->ctx || (!data && len)) return HH_ERR_INVAL;
    if (len > dt->cfg.max_payload) {
        dt->counters.tx_too_large++;
        return HH_ERR_INVAL;
    }
    const hh_dt_peer_addr_t *pa = hh_dt_peers_by_node(dt->peers, to);
    if (!pa) {
        dt->counters.tx_no_route++;
        HH_LOGW(COMP, "tx_no_route", "to=%u", to);
        return HH_ERR_NOTFOUND;
    }
    hh_dt_tx_peer_t *p = tx_get(dt, pa);
    if (!p) return HH_ERR_IO;
    if (p->outstanding >= HH_DT_QUEUE_LEN) {
        dt->counters.tx_queue_full++;
        return HH_ERR_AGAIN;
    }

    bool con = dt->cfg.max_retransmit > 0;
    coap_pdu_t *pdu = coap_pdu_init(con ? COAP_MESSAGE_CON : COAP_MESSAGE_NON,
                                    COAP_REQUEST_CODE_POST,
                                    coap_new_message_id(p->session),
                                    coap_session_max_pdu_size(p->session));
    uint8_t token[4];
    coap_prng(token, sizeof token);
    uint8_t cf[4];
    if (!pdu || !coap_add_token(pdu, sizeof token, token) ||
        !coap_add_option(pdu, COAP_OPTION_URI_PATH, 3, (const uint8_t *)"msg") ||
        !coap_add_option(pdu, COAP_OPTION_CONTENT_FORMAT,
                         coap_encode_var_safe(cf, sizeof cf, COAP_MEDIATYPE_TEXT_PLAIN), cf) ||
        (len && !coap_add_data(pdu, len, data))) {
        if (pdu) coap_delete_pdu(pdu);
        dt->counters.tx_errors++;
        HH_LOGE(COMP, "tx_build_failed", "to=%u len=%zu", to, len);
        return HH_ERR_IO;
    }
    coap_mid_t mid = coap_send(p->session, pdu);
    if (mid == COAP_INVALID_MID) {
        dt->counters.tx_errors++;
        HH_LOGW(COMP, "tx_send_failed", "to=%u len=%zu", to, len);
        return HH_ERR_IO;
    }
    if (!con) {
        for (unsigned i = 0; i < HH_DT_QUEUE_LEN; i++) {
            hh_dt_non_t *n = &p->non[i];
            if (n->used) continue;
            n->used = true;
            n->mid = (uint16_t)mid;
            memcpy(n->token, token, sizeof token);
            n->deadline = hh_now(dt->clock) + dt->cfg.ack_timeout_ms * 3u / 2u;
            break;
        }
    }
    p->outstanding++;
    dt->counters.tx_data++;
    HH_LOGD(COMP, "tx", "to=%u mid=%d len=%zu type=%s", to, mid, len, con ? "con" : "non");
    return HH_OK;
}

int hh_dt_fd(const hh_dt_t *dt)
{
    return dt && dt->ctx ? coap_context_get_coap_fd(dt->ctx) : -1;
}

int64_t hh_dt_next_timeout_ms(const hh_dt_t *dt)
{
    if (!dt) return -1;
    hh_time_ms_t now = hh_now(dt->clock);
    int64_t best = -1;
    for (int i = 0; i < HH_DT_MAX_PEERS; i++) {
        for (unsigned j = 0; dt->tx[i].used && j < HH_DT_QUEUE_LEN; j++) {
            const hh_dt_non_t *n = &dt->tx[i].non[j];
            if (!n->used) continue;
            int64_t left = n->deadline > now ? (int64_t)(n->deadline - now) : 0;
            if (best < 0 || left < best) best = left;
        }
    }
    return best;
}

void hh_dt_process(hh_dt_t *dt)
{
    if (!dt || !dt->ctx) return;
    coap_io_process(dt->ctx, COAP_IO_NO_WAIT);
    hh_time_ms_t now = hh_now(dt->clock);
    for (int i = 0; i < HH_DT_MAX_PEERS; i++) {
        hh_dt_tx_peer_t *p = &dt->tx[i];
        for (unsigned j = 0; p->used && j < HH_DT_QUEUE_LEN; j++) {
            hh_dt_non_t *n = &p->non[j];
            if (!n->used || now < n->deadline) continue;
            n->used = false;
            dt->counters.tx_given_up++;
            HH_LOGW(COMP, "give_up", "to=%u mid=%u type=non", p->node, n->mid);
            finish(p, n->mid, false, "timeout");
        }
    }
}

bool hh_dt_busy(const hh_dt_t *dt)
{
    if (!dt) return false;
    for (int i = 0; i < HH_DT_MAX_PEERS; i++)
        if (dt->tx[i].used && dt->tx[i].outstanding) return true;
    return false;
}

const hh_dt_counters_t *hh_dt_counters(const hh_dt_t *dt) { return dt ? &dt->counters : NULL; }

void hh_dt_log_stats(const hh_dt_t *dt)
{
    if (!dt) return;
    const hh_dt_counters_t *c = &dt->counters;
    HH_LOGI(COMP, "stats",
            "tx_data=%" PRIu64 " tx_acked=%" PRIu64 " tx_given_up=%" PRIu64 " tx_rst=%" PRIu64
            " tx_refused=%" PRIu64 " tx_unreachable=%" PRIu64 " tx_errors=%" PRIu64
            " tx_too_large=%" PRIu64 " tx_queue_full=%" PRIu64 " tx_no_route=%" PRIu64
            " rx_data=%" PRIu64 " rx_delivered=%" PRIu64 " rx_duplicates=%" PRIu64
            " rx_gaps=%" PRIu64 " rx_resync=%" PRIu64 " rx_bad_format=%" PRIu64
            " rx_unknown_sender=%" PRIu64 " rx_table_full=%" PRIu64,
            c->tx_data, c->tx_acked, c->tx_given_up, c->tx_rst, c->tx_refused,
            c->tx_unreachable, c->tx_errors, c->tx_too_large, c->tx_queue_full,
            c->tx_no_route, c->rx_data, c->rx_delivered, c->rx_duplicates, c->rx_gaps,
            c->rx_resync, c->rx_bad_format, c->rx_unknown_sender, c->rx_table_full);
    for (int i = 0; i < HH_DT_MAX_PEERS; i++) {
        const hh_dt_rx_peer_t *r = &dt->rx[i];
        if (!r->used) continue;
        char ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &r->addr.sin_addr, ip, sizeof ip);
        HH_LOGI(COMP, "stats_peer",
                "node=%u ip=%s delivered=%" PRIu64 " missing=%" PRIu64 " duplicates=%" PRIu64,
                r->node, ip, r->delivered, r->gaps, r->duplicates);
    }
}
