#include "hhsdr/dataterm/call.h"
#include "hhsdr/core/log.h"
#include <string.h>

#define COMP "dataterm_call"

void hh_call_config_default(hh_call_config_t *cfg)
{
    if (!cfg) return;
    /* TODO: confirm both values; no document gives them. */
    cfg->setup_timeout_ms = 10000;
    cfg->ring_timeout_ms  = 30000;
}

const char *hh_call_state_str(hh_call_state_t s)
{
    switch (s) {
    case HH_CALL_IDLE:    return "idle";
    case HH_CALL_CALLING: return "calling";
    case HH_CALL_RINGING: return "ringing";
    case HH_CALL_ACTIVE:  return "active";
    }
    return "?";
}

static const char *msg_str(hh_call_msg_t m)
{
    switch (m) {
    case HH_CALL_MSG_REQUEST: return "request";
    case HH_CALL_MSG_ACCEPT:  return "accept";
    case HH_CALL_MSG_REJECT:  return "reject";
    case HH_CALL_MSG_RELEASE: return "release";
    }
    return "?";
}

static const char *reason_str(hh_call_reason_t r)
{
    switch (r) {
    case HH_CALL_REASON_NONE:     return "none";
    case HH_CALL_REASON_BUSY:     return "busy";
    case HH_CALL_REASON_DECLINED: return "declined";
    case HH_CALL_REASON_TIMEOUT:  return "timeout";
    case HH_CALL_REASON_NORMAL:   return "normal";
    }
    return "?";
}

static void set_state(hh_call_t *c, hh_call_state_t s)
{
    HH_LOGI(COMP, "call_state", "peer=%u id=%u from=%s to=%s", c->peer, c->id,
            hh_call_state_str(c->state), hh_call_state_str(s));
    c->state = s;
}

static hh_status_t tx(hh_call_t *c, hh_node_id_t to, uint16_t id, hh_call_msg_t m,
                      hh_call_reason_t r)
{
    uint8_t pdu[HH_CALL_PDU_LEN] = { (uint8_t)(id >> 8), (uint8_t)id, (uint8_t)m, (uint8_t)r };
    hh_status_t s = c->send(to, pdu, sizeof pdu, c->ctx);
    if (s != HH_OK) {
        c->counters.tx_failed++;
        HH_LOGW(COMP, "call_tx_failed", "to=%u id=%u msg=%s status=%s", to, id, msg_str(m),
                hh_status_str(s));
        return s;
    }
    HH_LOGI(COMP, "call_tx", "to=%u id=%u msg=%s reason=%s", to, id, msg_str(m),
            reason_str(r));
    return HH_OK;
}

static void emit(hh_call_t *c, hh_call_event_kind_t kind, hh_call_reason_t reason)
{
    hh_call_event_t ev = { .kind = kind, .peer = c->peer, .id = c->id, .reason = reason };
    if (c->event) c->event(&ev, c->ctx);
}

/* Back to idle with nothing left of the call. */
static void end_call(hh_call_t *c, hh_call_reason_t reason)
{
    hh_call_event_t ev = { .kind = HH_CALL_EV_ENDED, .peer = c->peer, .id = c->id,
                           .reason = reason };
    HH_LOGI(COMP, "call_ended", "peer=%u id=%u reason=%s", c->peer, c->id, reason_str(reason));
    set_state(c, HH_CALL_IDLE);
    c->peer = HH_NODE_ID_INVALID;
    c->id = 0;
    c->deadline = 0;
    if (c->event) c->event(&ev, c->ctx);
}

hh_status_t hh_call_init(hh_call_t *c, const hh_call_config_t *cfg, const hh_clock_t *clock,
                         hh_call_send_fn send, hh_call_event_fn event, void *ctx)
{
    if (!c || !cfg || !clock || !send || !cfg->setup_timeout_ms || !cfg->ring_timeout_ms)
        return HH_ERR_INVAL;
    memset(c, 0, sizeof *c);
    c->cfg = *cfg;
    c->clock = clock;
    c->send = send;
    c->event = event;
    c->ctx = ctx;
    return HH_OK;
}

hh_status_t hh_call_request(hh_call_t *c, hh_node_id_t to)
{
    if (!c || to == HH_NODE_ID_INVALID) return HH_ERR_INVAL;
    if (c->state != HH_CALL_IDLE) return HH_ERR_STATE;
    uint16_t id = (uint16_t)(c->last_id + 1u);
    if (id == 0) id = 1;
    hh_status_t s = tx(c, to, id, HH_CALL_MSG_REQUEST, HH_CALL_REASON_NONE);
    if (s != HH_OK) return s;
    c->last_id = id;
    c->peer = to;
    c->id = id;
    c->deadline = hh_now(c->clock) + c->cfg.setup_timeout_ms;
    set_state(c, HH_CALL_CALLING);
    return HH_OK;
}

hh_status_t hh_call_accept(hh_call_t *c)
{
    if (!c) return HH_ERR_INVAL;
    if (c->state != HH_CALL_RINGING) return HH_ERR_STATE;
    hh_status_t s = tx(c, c->peer, c->id, HH_CALL_MSG_ACCEPT, HH_CALL_REASON_NONE);
    if (s != HH_OK) return s;
    c->deadline = 0;
    set_state(c, HH_CALL_ACTIVE);
    emit(c, HH_CALL_EV_ACTIVE, HH_CALL_REASON_NONE);
    return HH_OK;
}

hh_status_t hh_call_reject(hh_call_t *c)
{
    if (!c) return HH_ERR_INVAL;
    if (c->state != HH_CALL_RINGING) return HH_ERR_STATE;
    hh_status_t s = tx(c, c->peer, c->id, HH_CALL_MSG_REJECT, HH_CALL_REASON_DECLINED);
    if (s != HH_OK) return s;
    end_call(c, HH_CALL_REASON_DECLINED);
    return HH_OK;
}

hh_status_t hh_call_release(hh_call_t *c)
{
    if (!c) return HH_ERR_INVAL;
    if (c->state != HH_CALL_CALLING && c->state != HH_CALL_ACTIVE) return HH_ERR_STATE;
    hh_status_t s = tx(c, c->peer, c->id, HH_CALL_MSG_RELEASE, HH_CALL_REASON_NORMAL);
    if (s != HH_OK) return s;
    end_call(c, HH_CALL_REASON_NORMAL);
    return HH_OK;
}

static bool is_current(const hh_call_t *c, hh_node_id_t from, uint16_t id)
{
    return c->state != HH_CALL_IDLE && c->peer == from && c->id == id;
}

void hh_call_on_rx(hh_call_t *c, hh_node_id_t from, const uint8_t *pdu, size_t len)
{
    if (!c) return;
    uint16_t id = len == HH_CALL_PDU_LEN ? (uint16_t)((pdu[0] << 8) | pdu[1]) : 0;
    uint8_t type = len == HH_CALL_PDU_LEN ? pdu[2] : 0;
    uint8_t rsn = len == HH_CALL_PDU_LEN ? pdu[3] : 0;
    if (len != HH_CALL_PDU_LEN || id == 0 || type < HH_CALL_MSG_REQUEST ||
        type > HH_CALL_MSG_RELEASE || rsn > HH_CALL_REASON_NORMAL) {
        c->counters.rx_bad++;
        HH_LOGW(COMP, "call_rx_bad", "from=%u len=%zu", from, len);
        return;
    }
    HH_LOGI(COMP, "call_rx", "from=%u id=%u msg=%s reason=%s", from, id,
            msg_str((hh_call_msg_t)type), reason_str((hh_call_reason_t)rsn));

    switch (type) {
    case HH_CALL_MSG_REQUEST:
        if (c->state == HH_CALL_IDLE) {
            c->peer = from;
            c->id = id;
            c->deadline = hh_now(c->clock) + c->cfg.ring_timeout_ms;
            set_state(c, HH_CALL_RINGING);
            emit(c, HH_CALL_EV_INCOMING, HH_CALL_REASON_NONE);
        } else if (is_current(c, from, id)) {
            c->counters.rx_duplicate++;
        } else {
            /* TODO: both sides calling each other at once also ends here. */
            c->counters.busy_rejected++;
            /* A lost busy reject leaves the caller to its own setup timeout. */
            (void)tx(c, from, id, HH_CALL_MSG_REJECT, HH_CALL_REASON_BUSY);
        }
        return;
    case HH_CALL_MSG_ACCEPT:
        if (c->state == HH_CALL_CALLING && is_current(c, from, id)) {
            c->deadline = 0;
            set_state(c, HH_CALL_ACTIVE);
            emit(c, HH_CALL_EV_ACTIVE, HH_CALL_REASON_NONE);
            return;
        }
        break;
    case HH_CALL_MSG_REJECT:
        if (c->state == HH_CALL_CALLING && is_current(c, from, id)) {
            end_call(c, (hh_call_reason_t)rsn);
            return;
        }
        break;
    case HH_CALL_MSG_RELEASE:
        if (is_current(c, from, id)) {
            end_call(c, rsn == HH_CALL_REASON_NONE ? HH_CALL_REASON_NORMAL
                                                   : (hh_call_reason_t)rsn);
            return;
        }
        break;
    }
    c->counters.rx_stray++;
    HH_LOGD(COMP, "call_rx_stray", "from=%u id=%u state=%s", from, id,
            hh_call_state_str(c->state));
}

void hh_call_poll(hh_call_t *c)
{
    if (!c || (c->state != HH_CALL_CALLING && c->state != HH_CALL_RINGING)) return;
    if (hh_now(c->clock) < c->deadline) return;
    c->counters.timeouts++;
    /* TODO: end an active call when the peer goes silent. Needs a keepalive contract. */
    /* The call ends even if this message is lost, or it would never end. The
     * peer ends it on its own timeout. */
    if (c->state == HH_CALL_CALLING)
        (void)tx(c, c->peer, c->id, HH_CALL_MSG_RELEASE, HH_CALL_REASON_TIMEOUT);
    else
        (void)tx(c, c->peer, c->id, HH_CALL_MSG_REJECT, HH_CALL_REASON_TIMEOUT);
    end_call(c, HH_CALL_REASON_TIMEOUT);
}

int64_t hh_call_next_timeout_ms(const hh_call_t *c)
{
    if (!c || (c->state != HH_CALL_CALLING && c->state != HH_CALL_RINGING)) return -1;
    hh_time_ms_t now = hh_now(c->clock);
    return c->deadline > now ? (int64_t)(c->deadline - now) : 0;
}
