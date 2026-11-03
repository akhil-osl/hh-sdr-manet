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

static void call_tx(hh_node_id_t to, const uint8_t *pdu, size_t len, void *ctx);
static void call_ev(const hh_call_event_t *ev, void *ctx);
static void on_call(coap_resource_t *res, coap_session_t *session, const coap_pdu_t *req,
                    const coap_string_t *query, coap_pdu_t *resp);

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

/* A stream is one source IP and port. A sender has one per class, and a
 * restarted sender gets new ones, so Message IDs are counted per stream. */
static hh_dt_rx_peer_t *rx_find(hh_dt_t *dt, const struct sockaddr_in *a)
{
    for (int i = 0; i < HH_DT_STREAMS; i++)
        if (dt->rx[i].used && dt->rx[i].addr.sin_addr.s_addr == a->sin_addr.s_addr &&
            dt->rx[i].addr.sin_port == a->sin_port)
            return &dt->rx[i];
    return NULL;
}

static hh_dt_rx_peer_t *rx_new(hh_dt_t *dt, const struct sockaddr_in *a)
{
    hh_dt_rx_peer_t *slot = NULL;
    for (int i = 0; i < HH_DT_STREAMS && !slot; i++)
        if (!dt->rx[i].used) slot = &dt->rx[i];
    if (!slot) {
        /* Table full: the stream silent for longest is most likely a dead run. */
        slot = &dt->rx[0];
        for (int i = 1; i < HH_DT_STREAMS; i++)
            if (dt->rx[i].last_ms < slot->last_ms) slot = &dt->rx[i];
        dt->counters.rx_table_full++;
        HH_LOGW(COMP, "rx_stream_dropped", "node=%u", slot->node);
    }
    memset(slot, 0, sizeof *slot);
    slot->used = true;
    slot->addr = *a;
    slot->node = hh_dt_peers_node_of(dt->peers, a);
    return slot;
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
        if (r->node == HH_NODE_ID_INVALID) {
            dt->counters.rx_unknown_sender++;
            HH_LOGI(COMP, "rx_unknown_sender", "ip=%s", ip);
        }
    }
    r->last_ms = hh_now(dt->clock);

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

/* Call setup answers itself at the call level, so its CoAP results are only
 * logged and the application's result callback sees messages alone. */
static void finish(hh_dt_tx_peer_t *p, uint16_t mid, bool acked, const char *reason)
{
    hh_dt_t *dt = p->dt;
    if (p->outstanding) p->outstanding--;
    HH_LOGD(COMP, "result", "to=%u mid=%u class=%s acked=%d reason=%s", p->node, mid,
            hh_dt_class_str(p->cls), acked, reason);
    if (p->cls != HH_DT_CLASS_SIGNAL && dt->result)
        dt->result(p->node, mid, acked, reason, dt->result_ctx);
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
    coap_resource_t *call = coap_resource_init(coap_make_str_const("call"), 0);
    if (!call) goto fail;
    coap_resource_set_userdata(call, dt);
    coap_register_handler(call, COAP_REQUEST_POST, on_call);
    coap_add_resource(dt->ctx, call);
    hh_dt_sched_config_t scfg;
    hh_dt_sched_config_default(&scfg);
    hh_call_config_t ccfg;
    hh_call_config_default(&ccfg);
    if (hh_dt_sched_init(&dt->sched, &scfg, hh_now(dt->clock)) != HH_OK ||
        hh_call_init(&dt->call, &ccfg, dt->clock, call_tx, call_ev, dt) != HH_OK)
        goto fail;
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
        for (int i = 0; i < HH_DT_STREAMS; i++)
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

/* DSCP of a class. The data class takes the one given to hh_dt_open(). */
static int class_dscp(const hh_dt_t *dt, hh_dt_class_t cls)
{
    return cls == HH_DT_CLASS_DATA ? dt->dscp : hh_dt_class_dscp(cls);
}

static hh_dt_tx_peer_t *tx_find(hh_dt_t *dt, hh_node_id_t node, hh_dt_class_t cls)
{
    for (int i = 0; i < HH_DT_STREAMS; i++)
        if (dt->tx[i].used && dt->tx[i].node == node && dt->tx[i].cls == cls)
            return &dt->tx[i];
    return NULL;
}

/* The session for a destination and class. Each has its own socket, marked
 * once with the class's DSCP, so a retransmission keeps its class. */
static hh_dt_tx_peer_t *tx_get(hh_dt_t *dt, const hh_dt_peer_addr_t *pa, hh_dt_class_t cls)
{
    hh_dt_tx_peer_t *p = tx_find(dt, pa->node, cls);
    if (p) return p;
    for (int i = 0; i < HH_DT_STREAMS && !p; i++)
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
    scan_new_sockets(true, class_dscp(dt, cls));
    coap_fixed_point_t t = { (uint16_t)(dt->cfg.ack_timeout_ms / 1000u),
                             (uint16_t)(dt->cfg.ack_timeout_ms % 1000u) };
    coap_session_set_ack_timeout(s, t);
    coap_session_set_max_retransmit(s, dt->cfg.max_retransmit);
    coap_session_set_mtu(s, DT_COAP_MTU);

    memset(p, 0, sizeof *p);
    p->used = true;
    p->node = pa->node;
    p->cls = cls;
    p->session = s;
    p->dt = dt;
    dt_session_set_app_data(s, p);
    return p;
}

/* Builds and sends one POST on the session of the destination and class. */
static hh_status_t tx_post(hh_dt_t *dt, hh_node_id_t to, hh_dt_class_t cls, hh_dt_path_t path,
                           const uint8_t *data, size_t len)
{
    const hh_dt_peer_addr_t *pa = hh_dt_peers_by_node(dt->peers, to);
    if (!pa) {
        dt->counters.tx_no_route++;
        HH_LOGW(COMP, "tx_no_route", "to=%u", to);
        return HH_ERR_NOTFOUND;
    }
    hh_dt_tx_peer_t *p = tx_get(dt, pa, cls);
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
    const bool call = path == HH_DT_PATH_CALL;
    if (!pdu || !coap_add_token(pdu, sizeof token, token) ||
        !coap_add_option(pdu, COAP_OPTION_URI_PATH, call ? 4 : 3,
                         (const uint8_t *)(call ? "call" : "msg")) ||
        !coap_add_option(pdu, COAP_OPTION_CONTENT_FORMAT,
                         coap_encode_var_safe(cf, sizeof cf,
                                              call ? COAP_MEDIATYPE_APPLICATION_OCTET_STREAM
                                                   : COAP_MEDIATYPE_TEXT_PLAIN), cf) ||
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
    HH_LOGD(COMP, "tx", "to=%u mid=%d len=%zu type=%s class=%s dscp=%d path=%s", to, mid, len,
            con ? "con" : "non", hh_dt_class_str(cls), class_dscp(dt, cls), call ? "call" : "msg");
    return HH_OK;
}

static hh_status_t check_message(hh_dt_t *dt, const uint8_t *data, size_t len)
{
    if (!dt || !dt->ctx || (!data && len)) return HH_ERR_INVAL;
    if (len > dt->cfg.max_payload) {
        dt->counters.tx_too_large++;
        return HH_ERR_INVAL;
    }
    return HH_OK;
}

hh_status_t hh_dt_send(hh_dt_t *dt, hh_node_id_t to, const uint8_t *data, size_t len)
{
    hh_status_t s = check_message(dt, data, len);
    return s != HH_OK ? s : tx_post(dt, to, HH_DT_CLASS_DATA, HH_DT_PATH_MSG, data, len);
}

static hh_status_t pend_add(hh_dt_t *dt, hh_dt_class_t cls, hh_node_id_t to, hh_dt_path_t path,
                            const uint8_t *data, size_t len)
{
    if (dt->pend_n[cls] >= HH_DT_PENDING_LEN) {
        dt->counters.tx_pending_full++;
        HH_LOGW(COMP, "pending_full", "class=%s to=%u", hh_dt_class_str(cls), to);
        return HH_ERR_AGAIN;
    }
    hh_dt_pending_t *e = &dt->pend[cls][(dt->pend_head[cls] + dt->pend_n[cls]) % HH_DT_PENDING_LEN];
    e->to = to;
    e->path = path;
    e->len = (uint16_t)len;
    if (len) memcpy(e->data, data, len);
    dt->pend_n[cls]++;
    HH_LOGD(COMP, "queued", "class=%s to=%u len=%zu waiting=%u", hh_dt_class_str(cls), to, len,
            dt->pend_n[cls]);
    return HH_OK;
}

hh_status_t hh_dt_send_class(hh_dt_t *dt, hh_node_id_t to, hh_dt_class_t cls,
                             const uint8_t *data, size_t len)
{
    hh_status_t s = check_message(dt, data, len);
    if (s != HH_OK) return s;
    if ((unsigned)cls >= HH_DT_CLASS_COUNT || cls == HH_DT_CLASS_SIGNAL) return HH_ERR_INVAL;
    if (!hh_dt_peers_by_node(dt->peers, to)) {
        dt->counters.tx_no_route++;
        HH_LOGW(COMP, "tx_no_route", "to=%u", to);
        return HH_ERR_NOTFOUND;
    }
    return pend_add(dt, cls, to, HH_DT_PATH_MSG, data, len);
}

/* True if the oldest message of a class can go now: its session has room. */
static bool head_ready(hh_dt_t *dt, hh_dt_class_t cls)
{
    if (!dt->pend_n[cls]) return false;
    const hh_dt_pending_t *e = &dt->pend[cls][dt->pend_head[cls]];
    const hh_dt_tx_peer_t *p = tx_find(dt, e->to, cls);
    return !p || p->outstanding < HH_DT_QUEUE_LEN;
}

/* Sends queued messages, class by class as the scheduler chooses. */
static void pump(hh_dt_t *dt)
{
    for (;;) {
        uint16_t waiting[HH_DT_CLASS_COUNT];
        for (int c = 0; c < HH_DT_CLASS_COUNT; c++)
            waiting[c] = head_ready(dt, (hh_dt_class_t)c) ? dt->pend_n[c] : 0;
        int c = hh_dt_sched_pick(&dt->sched, waiting, hh_now(dt->clock));
        if (c < 0) return;
        hh_dt_pending_t *e = &dt->pend[c][dt->pend_head[c]];
        hh_status_t s = tx_post(dt, e->to, (hh_dt_class_t)c, e->path, e->data, e->len);
        if (s != HH_OK) HH_LOGW(COMP, "queued_send_failed", "class=%s to=%u status=%s",
                                hh_dt_class_str((hh_dt_class_t)c), e->to, hh_status_str(s));
        dt->pend_head[c] = (uint8_t)((dt->pend_head[c] + 1u) % HH_DT_PENDING_LEN);
        dt->pend_n[c]--;
    }
}

static void call_tx(hh_node_id_t to, const uint8_t *pdu, size_t len, void *ctx)
{
    pend_add(ctx, HH_DT_CLASS_SIGNAL, to, HH_DT_PATH_CALL, pdu, len);
}

static void call_ev(const hh_call_event_t *ev, void *ctx)
{
    hh_dt_t *dt = ctx;
    if (dt->call_event) dt->call_event(ev, dt->call_event_ctx);
}

static void on_call(coap_resource_t *res, coap_session_t *session, const coap_pdu_t *req,
                    const coap_string_t *query, coap_pdu_t *resp)
{
    (void)query;
    hh_dt_t *dt = coap_resource_get_userdata(res);
    const coap_address_t *ra = coap_session_get_addr_remote(session);
    if (!dt || !ra || ra->addr.sa.sa_family != AF_INET) {
        coap_pdu_set_code(resp, COAP_RESPONSE_CODE_INTERNAL_ERROR);
        return;
    }
    hh_node_id_t from = hh_dt_peers_node_of(dt->peers, &ra->addr.sin);
    if (from == HH_NODE_ID_INVALID) {
        HH_LOGW(COMP, "call_rx_refused", "reason=unknown_sender");
        coap_pdu_set_code(resp, COAP_RESPONSE_CODE_FORBIDDEN);
        return;
    }
    coap_opt_iterator_t it;
    coap_opt_t *cf = coap_check_option(req, COAP_OPTION_CONTENT_FORMAT, &it);
    if (!cf || coap_decode_var_bytes(coap_opt_value(cf), coap_opt_length(cf)) !=
                   COAP_MEDIATYPE_APPLICATION_OCTET_STREAM) {
        HH_LOGW(COMP, "call_rx_refused", "from=%u reason=content_format", from);
        coap_pdu_set_code(resp, COAP_RESPONSE_CODE_UNSUPPORTED_CONTENT_FORMAT);
        return;
    }
    size_t len = 0;
    const uint8_t *data = NULL;
    if (!coap_get_data(req, &len, &data)) len = 0;
    hh_call_on_rx(&dt->call, from, data, len);
    coap_pdu_set_code(resp, COAP_RESPONSE_CODE_CHANGED);
}

hh_status_t hh_dt_call_request(hh_dt_t *dt, hh_node_id_t to)
{
    if (!dt || !dt->ctx) return HH_ERR_INVAL;
    if (!hh_dt_peers_by_node(dt->peers, to)) return HH_ERR_NOTFOUND;
    return hh_call_request(&dt->call, to);
}

hh_status_t hh_dt_call_accept(hh_dt_t *dt)  { return dt ? hh_call_accept(&dt->call) : HH_ERR_INVAL; }
hh_status_t hh_dt_call_reject(hh_dt_t *dt)  { return dt ? hh_call_reject(&dt->call) : HH_ERR_INVAL; }
hh_status_t hh_dt_call_release(hh_dt_t *dt) { return dt ? hh_call_release(&dt->call) : HH_ERR_INVAL; }

void hh_dt_set_call_event(hh_dt_t *dt, hh_call_event_fn fn, void *ctx)
{
    if (!dt) return;
    dt->call_event = fn;
    dt->call_event_ctx = ctx;
}

const hh_call_t *hh_dt_call(const hh_dt_t *dt) { return dt ? &dt->call : NULL; }

int hh_dt_fd(const hh_dt_t *dt)
{
    return dt && dt->ctx ? coap_context_get_coap_fd(dt->ctx) : -1;
}

int64_t hh_dt_next_timeout_ms(const hh_dt_t *dt)
{
    if (!dt) return -1;
    hh_time_ms_t now = hh_now(dt->clock);
    int64_t best = hh_call_next_timeout_ms(&dt->call);
    for (int c = 0; c < HH_DT_CLASS_COUNT; c++)
        if (dt->pend_n[c]) return 0;  /* queued messages leave at the next process call */
    for (int i = 0; i < HH_DT_STREAMS; i++) {
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
    hh_call_poll(&dt->call);
    pump(dt);
    hh_time_ms_t now = hh_now(dt->clock);
    for (int i = 0; i < HH_DT_STREAMS; i++) {
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
    for (int c = 0; c < HH_DT_CLASS_COUNT; c++)
        if (dt->pend_n[c]) return true;
    for (int i = 0; i < HH_DT_STREAMS; i++)
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
            " rx_unknown_sender=%" PRIu64 " rx_table_full=%" PRIu64
            " tx_pending_full=%" PRIu64,
            c->tx_data, c->tx_acked, c->tx_given_up, c->tx_rst, c->tx_refused,
            c->tx_unreachable, c->tx_errors, c->tx_too_large, c->tx_queue_full,
            c->tx_no_route, c->rx_data, c->rx_delivered, c->rx_duplicates, c->rx_gaps,
            c->rx_resync, c->rx_bad_format, c->rx_unknown_sender, c->rx_table_full,
            c->tx_pending_full);
    for (int i = 0; i < HH_DT_STREAMS; i++) {
        const hh_dt_rx_peer_t *r = &dt->rx[i];
        if (!r->used) continue;
        char ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &r->addr.sin_addr, ip, sizeof ip);
        HH_LOGI(COMP, "stats_peer",
                "node=%u ip=%s delivered=%" PRIu64 " missing=%" PRIu64 " duplicates=%" PRIu64,
                r->node, ip, r->delivered, r->gaps, r->duplicates);
    }
}
