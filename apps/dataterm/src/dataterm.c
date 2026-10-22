#include "hhsdr/dataterm/dataterm.h"
#include "hhsdr/core/log.h"
#include "hhsdr/core/seq.h"
#include <inttypes.h>
#include <string.h>

#define COMP "dataterm"

void hh_dt_config_default(hh_dt_config_t *cfg)
{
    if (!cfg) return;
    cfg->timeout_ms  = 1000;
    cfg->max_retries = 4;
    /* 1280-byte IP packet minus IPv4 20, UDP 8 and our header 20. */
    cfg->max_payload = 1232;
}

hh_status_t hh_dt_init(hh_dt_t *dt, hh_node_id_t self, const hh_dt_config_t *cfg,
                       const hh_clock_t *clock, hh_dt_tx_fn tx, void *tx_ctx)
{
    if (!dt || !cfg || !clock || !tx || self == HH_NODE_ID_INVALID) return HH_ERR_INVAL;
    if (cfg->timeout_ms == 0 || cfg->max_payload > HH_DT_PAYLOAD_MAX) return HH_ERR_INVAL;
    memset(dt, 0, sizeof *dt);
    dt->self   = self;
    dt->cfg    = *cfg;
    dt->clock  = clock;
    dt->tx     = tx;
    dt->tx_ctx = tx_ctx;
    HH_LOGI(COMP, "initialized", "node=%u timeout_ms=%u retries=%u max_payload=%u",
            self, cfg->timeout_ms, cfg->max_retries, cfg->max_payload);
    return HH_OK;
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

static hh_dt_peer_t *peer_find(hh_dt_t *dt, hh_node_id_t node)
{
    for (int i = 0; i < HH_DT_MAX_PEERS; i++)
        if (dt->peers[i].used && dt->peers[i].node == node) return &dt->peers[i];
    return NULL;
}

static hh_dt_peer_t *peer_get(hh_dt_t *dt, hh_node_id_t node)
{
    hh_dt_peer_t *p = peer_find(dt, node);
    if (p) return p;
    for (int i = 0; i < HH_DT_MAX_PEERS; i++) {
        if (!dt->peers[i].used) {
            p = &dt->peers[i];
            memset(p, 0, sizeof *p);
            p->used = true;
            p->node = node;
            p->next_seq = 1;
            return p;
        }
    }
    return NULL;
}

static void transmit(hh_dt_t *dt, const hh_dt_hdr_t *h, const uint8_t *payload)
{
    size_t n = 0;
    if (hh_dt_encode(h, payload, dt->txbuf, sizeof dt->txbuf, &n) != HH_OK) return;
    hh_status_t s = dt->tx(h->dst, dt->txbuf, n, dt->tx_ctx);
    if (s != HH_OK) {
        dt->counters.tx_errors++;
        HH_LOGW(COMP, "tx_error", "to=%u seq=%" PRIu32 " type=%u status=%s", h->dst,
                h->seq, h->type, hh_status_str(s));
    }
}

static void send_inflight(hh_dt_t *dt, hh_dt_peer_t *p)
{
    const hh_dt_slot_t *m = &p->queue[p->q_head];
    hh_dt_hdr_t h = {
        .type = HH_DT_TYPE_DATA, .flags = HH_DT_FLAG_ACK_REQ, .attempt = p->attempt,
        .src = dt->self, .dst = p->node, .seq = p->inflight_seq, .len = m->len,
    };
    transmit(dt, &h, m->data);
    p->deadline = hh_now(dt->clock) + p->cur_timeout;
}

static void pump(hh_dt_t *dt, hh_dt_peer_t *p)
{
    if (p->inflight || p->q_count == 0) return;
    p->inflight     = true;
    p->inflight_seq = p->next_seq++;
    p->attempt      = 0;
    p->cur_timeout  = dt->cfg.timeout_ms;
    dt->counters.tx_data++;
    HH_LOGD(COMP, "tx_data", "to=%u seq=%" PRIu32 " len=%u", p->node, p->inflight_seq,
            p->queue[p->q_head].len);
    send_inflight(dt, p);
}

static void finish(hh_dt_t *dt, hh_dt_peer_t *p, bool acked)
{
    hh_seq_t seq = p->inflight_seq;
    uint8_t attempts = (uint8_t)(p->attempt + 1);
    p->inflight = false;
    p->q_head = (uint8_t)((p->q_head + 1) % HH_DT_QUEUE_LEN);
    p->q_count--;
    if (dt->result) dt->result(p->node, seq, acked, attempts, dt->result_ctx);
    pump(dt, p);
}

hh_status_t hh_dt_send(hh_dt_t *dt, hh_node_id_t to, const uint8_t *data, size_t len)
{
    if (!dt || (len && !data) || to == HH_NODE_ID_INVALID || to == dt->self)
        return HH_ERR_INVAL;
    if (len > dt->cfg.max_payload) {
        dt->counters.tx_too_large++;
        HH_LOGW(COMP, "tx_refused", "reason=too_large to=%u len=%zu max=%u", to, len,
                dt->cfg.max_payload);
        return HH_ERR_INVAL;
    }
    hh_dt_peer_t *p = peer_get(dt, to);
    if (!p) {
        HH_LOGW(COMP, "tx_refused", "reason=peer_table_full to=%u", to);
        return HH_ERR_NOMEM;
    }
    if (p->q_count == HH_DT_QUEUE_LEN) {
        dt->counters.tx_queue_full++;
        return HH_ERR_AGAIN;
    }
    hh_dt_slot_t *m = &p->queue[(p->q_head + p->q_count) % HH_DT_QUEUE_LEN];
    m->len = (uint16_t)len;
    if (len) memcpy(m->data, data, len);
    p->q_count++;
    pump(dt, p);
    return HH_OK;
}

static void on_ack(hh_dt_t *dt, const hh_dt_hdr_t *h)
{
    dt->counters.rx_acks++;
    hh_dt_peer_t *p = peer_find(dt, h->src);
    if (!p || !p->inflight || h->seq != p->inflight_seq) {
        dt->counters.rx_acks_unexpected++;
        HH_LOGD(COMP, "ack_unexpected", "from=%u seq=%" PRIu32, h->src, h->seq);
        return;
    }
    dt->counters.tx_acked++;
    if (h->attempt == 0) dt->counters.tx_acked_first_try++;
    HH_LOGD(COMP, "acked", "to=%u seq=%" PRIu32 " attempt=%u", h->src, h->seq, h->attempt);
    finish(dt, p, true);
}

static void on_data(hh_dt_t *dt, const hh_dt_hdr_t *h, const uint8_t *payload)
{
    dt->counters.rx_data++;
    hh_dt_peer_t *p = peer_get(dt, h->src);
    if (!p) {
        dt->counters.rx_peer_table_full++;
        HH_LOGW(COMP, "rx_dropped", "reason=peer_table_full from=%u", h->src);
        return;
    }

    if (h->flags & HH_DT_FLAG_ACK_REQ) {
        hh_dt_hdr_t ack = { .type = HH_DT_TYPE_ACK, .attempt = h->attempt,
                            .src = dt->self, .dst = h->src, .seq = h->seq };
        dt->counters.tx_acks++;
        transmit(dt, &ack, NULL);
    }

    /* Sequences start at 1, so a lost first message still shows as a gap. */
    hh_seq_t expected = p->rx_seen ? p->rx_last + 1 : 1;
    bool restart = p->rx_seen && h->seq == 1 && p->rx_last >= 2;

    if (h->seq == expected || hh_seq_gt(h->seq, expected) || restart) {
        if (restart) {
            HH_LOGI(COMP, "peer_restart", "from=%u last_seq=%" PRIu32, h->src, p->rx_last);
        } else if (h->seq != expected) {
            hh_seq_t missing = h->seq - expected;
            p->rx_gaps += missing;
            dt->counters.rx_gaps += missing;
            HH_LOGW(COMP, "gap", "from=%u first_missing=%" PRIu32 " last_missing=%" PRIu32
                    " missing=%" PRIu32, h->src, expected, h->seq - 1, missing);
        }
        if (!p->rx_seen) p->rx_first = h->seq;
        p->rx_seen = true;
        p->rx_last = h->seq;
        p->rx_delivered++;
        dt->counters.rx_delivered++;
        if (h->attempt == 0) dt->counters.rx_first_try++;
        HH_LOGD(COMP, "rx_deliver", "from=%u seq=%" PRIu32 " attempt=%u len=%u", h->src,
                h->seq, h->attempt, h->len);
        if (dt->deliver) dt->deliver(h->src, h->seq, payload, h->len, dt->deliver_ctx);
        return;
    }

    p->rx_duplicates++;
    dt->counters.rx_duplicates++;
    HH_LOGI(COMP, "duplicate", "from=%u seq=%" PRIu32 " attempt=%u", h->src, h->seq,
            h->attempt);
}

hh_dt_rx_err_t hh_dt_input(hh_dt_t *dt, const uint8_t *buf, size_t len)
{
    if (!dt) return HH_DT_RX_SHORT;
    hh_dt_hdr_t h;
    const uint8_t *payload = NULL;
    hh_dt_rx_err_t e = hh_dt_decode(buf, len, &h, &payload);
    if (e != HH_DT_RX_OK) {
        dt->counters.rx_bad++;
        if (e == HH_DT_RX_CRC) dt->counters.rx_bad_crc++;
        HH_LOGW(COMP, "rx_dropped", "reason=%s len=%zu", hh_dt_rx_err_str(e), len);
        return e;
    }
    if (h.dst != dt->self) {
        dt->counters.rx_not_for_me++;
        HH_LOGD(COMP, "rx_dropped", "reason=not_for_me from=%u dst=%u", h.src, h.dst);
        return HH_DT_RX_OK;
    }
    if (h.type == HH_DT_TYPE_ACK) on_ack(dt, &h);
    else on_data(dt, &h, payload);
    return HH_DT_RX_OK;
}

void hh_dt_poll(hh_dt_t *dt)
{
    if (!dt) return;
    hh_time_ms_t now = hh_now(dt->clock);
    for (int i = 0; i < HH_DT_MAX_PEERS; i++) {
        hh_dt_peer_t *p = &dt->peers[i];
        if (!p->used || !p->inflight || now < p->deadline) continue;
        if (p->attempt < dt->cfg.max_retries) {
            p->attempt++;
            p->cur_timeout *= 2;
            dt->counters.tx_retries++;
            HH_LOGI(COMP, "retry", "to=%u seq=%" PRIu32 " attempt=%u next_timeout_ms=%u",
                    p->node, p->inflight_seq, p->attempt, p->cur_timeout);
            send_inflight(dt, p);
        } else {
            dt->counters.tx_given_up++;
            HH_LOGW(COMP, "given_up", "to=%u seq=%" PRIu32 " attempts=%u", p->node,
                    p->inflight_seq, p->attempt + 1);
            finish(dt, p, false);
        }
    }
}

int64_t hh_dt_next_timeout_ms(const hh_dt_t *dt)
{
    if (!dt) return -1;
    hh_time_ms_t now = hh_now(dt->clock);
    int64_t best = -1;
    for (int i = 0; i < HH_DT_MAX_PEERS; i++) {
        const hh_dt_peer_t *p = &dt->peers[i];
        if (!p->used || !p->inflight) continue;
        int64_t left = p->deadline > now ? (int64_t)(p->deadline - now) : 0;
        if (best < 0 || left < best) best = left;
    }
    return best;
}

bool hh_dt_busy(const hh_dt_t *dt)
{
    if (!dt) return false;
    for (int i = 0; i < HH_DT_MAX_PEERS; i++)
        if (dt->peers[i].used && dt->peers[i].q_count) return true;
    return false;
}

const hh_dt_counters_t *hh_dt_counters(const hh_dt_t *dt) { return dt ? &dt->counters : NULL; }

void hh_dt_log_stats(const hh_dt_t *dt)
{
    if (!dt) return;
    const hh_dt_counters_t *c = &dt->counters;
    HH_LOGI(COMP, "stats",
            "node=%u tx_data=%" PRIu64 " tx_retries=%" PRIu64 " tx_acked=%" PRIu64
            " tx_acked_first_try=%" PRIu64 " tx_given_up=%" PRIu64 " tx_errors=%" PRIu64
            " tx_too_large=%" PRIu64 " rx_delivered=%" PRIu64 " rx_first_try=%" PRIu64
            " rx_gaps=%" PRIu64 " rx_duplicates=%" PRIu64 " rx_bad=%" PRIu64
            " rx_bad_crc=%" PRIu64 " rx_not_for_me=%" PRIu64,
            dt->self, c->tx_data, c->tx_retries, c->tx_acked, c->tx_acked_first_try,
            c->tx_given_up, c->tx_errors, c->tx_too_large, c->rx_delivered,
            c->rx_first_try, c->rx_gaps, c->rx_duplicates, c->rx_bad, c->rx_bad_crc,
            c->rx_not_for_me);
    for (int i = 0; i < HH_DT_MAX_PEERS; i++) {
        const hh_dt_peer_t *p = &dt->peers[i];
        if (!p->used || !p->rx_seen) continue;
        HH_LOGI(COMP, "rx_peer",
                "from=%u first_seq=%" PRIu32 " last_seq=%" PRIu32 " delivered=%" PRIu64
                " gaps=%" PRIu64 " duplicates=%" PRIu64,
                p->node, p->rx_first, p->rx_last, p->rx_delivered, p->rx_gaps,
                p->rx_duplicates);
    }
}
