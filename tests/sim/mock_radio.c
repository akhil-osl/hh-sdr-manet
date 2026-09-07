#include "mock_radio.h"
#include <string.h>

static hh_status_t m_open(void *self)
{
    mock_radio_t *m = self;
    if (!m) return HH_ERR_INVAL;
    if (!m->operational) return HH_ERR_IO;
    m->opened = true;
    return HH_OK;
}

static hh_status_t m_close(void *self)
{
    mock_radio_t *m = self;
    if (!m) return HH_ERR_INVAL;
    m->opened = false;
    return HH_OK;
}

static hh_status_t m_transmit(void *self, const hh_frame_t *f)
{
    mock_radio_t *m = self;
    if (!m || !f) return HH_ERR_INVAL;
    if (!m->opened || !m->operational) return HH_ERR_IO;
    if (m->tx_fails) return HH_ERR_IO;

    m->tx_log[m->tx_count % MOCK_TX_LOG] = *f;
    m->tx_count++;
    m->tx_total++;
    return HH_OK;
}

static hh_status_t m_set_rx_callback(void *self, hh_radio_rx_fn fn, void *ctx)
{
    mock_radio_t *m = self;
    if (!m) return HH_ERR_INVAL;
    m->rx_fn = fn;
    m->rx_ctx = ctx;
    return HH_OK;
}

static hh_status_t m_get_status(void *self, hh_radio_status_t *out)
{
    mock_radio_t *m = self;
    if (!m || !out) return HH_ERR_INVAL;
    memset(out, 0, sizeof *out);
    out->operational = m->operational;
    out->channel     = m->channel;
    out->frames_tx   = m->tx_total;
    out->frames_rx   = m->frames_rx;
    out->rx_errors   = m->rx_errors;
    return HH_OK;
}

static hh_status_t m_get_link_metrics(void *self, hh_node_id_t n, hh_link_sample_t *out)
{
    mock_radio_t *m = self;
    if (!m || !out) return HH_ERR_INVAL;
    for (size_t i = 0; i < MOCK_MAX_PEERS; i++) {
        if (m->peers[i].set && m->peers[i].id == n) { *out = m->peers[i].s; return HH_OK; }
    }
    return HH_ERR_NOTFOUND;
}

static hh_status_t m_set_channel(void *self, uint32_t ch)
{
    mock_radio_t *m = self;
    if (!m) return HH_ERR_INVAL;
    if (!m->supports_channel_change) return HH_ERR_UNSUPPORTED;
    m->channel = ch;
    return HH_OK;
}

static hh_status_t m_poll(void *self, hh_time_ms_t now)
{
    mock_radio_t *m = self;
    if (!m) return HH_ERR_INVAL;
    m->now = now;
    if (!m->operational) return HH_OK;   /* a dead radio delivers nothing */

    for (size_t i = 0; i < m->rx_count; i++) {
        mock_rx_item_t *it = &m->rx[i];
        if (it->used || it->deliver_at > now) continue;
        it->used = true;
        m->frames_rx++;
        if (m->rx_fn) m->rx_fn(&it->frame, &it->metrics, m->rx_ctx);
    }
    /* Compact the queue so long runs do not exhaust it. */
    {
        size_t w = 0;
        for (size_t i = 0; i < m->rx_count; i++)
            if (!m->rx[i].used) m->rx[w++] = m->rx[i];
        m->rx_count = w;
    }
    return HH_OK;
}

static const hh_radio_ops_t g_mock_ops = {
    .name             = "mock",
    .open             = m_open,
    .close            = m_close,
    .transmit         = m_transmit,
    .set_rx_callback  = m_set_rx_callback,
    .get_status       = m_get_status,
    .get_link_metrics = m_get_link_metrics,
    .set_channel      = m_set_channel,
    .poll             = m_poll,
};

void mock_radio_init(mock_radio_t *m, const char *label, hh_radio_t *out)
{
    if (!m || !out) return;
    memset(m, 0, sizeof *m);
    m->label = label ? label : "mock";
    m->operational = true;
    m->supports_channel_change = true;
    out->ops  = &g_mock_ops;
    out->self = m;
}

void mock_radio_enqueue_rx(mock_radio_t *m, const hh_frame_t *f,
                           const hh_link_sample_t *metrics, hh_time_ms_t deliver_at)
{
    mock_rx_item_t *it;
    if (!m || !f || m->rx_count >= MOCK_RX_QUEUE) return;
    it = &m->rx[m->rx_count++];
    memset(it, 0, sizeof *it);
    it->frame = *f;
    if (metrics) it->metrics = *metrics;
    it->deliver_at = deliver_at;
}

void mock_radio_set_peer_metrics(mock_radio_t *m, hh_node_id_t id, const hh_link_sample_t *s)
{
    if (!m || !s) return;
    for (size_t i = 0; i < MOCK_MAX_PEERS; i++) {
        if (m->peers[i].set && m->peers[i].id == id) { m->peers[i].s = *s; return; }
    }
    for (size_t i = 0; i < MOCK_MAX_PEERS; i++) {
        if (!m->peers[i].set) { m->peers[i].set = true; m->peers[i].id = id; m->peers[i].s = *s; return; }
    }
}

void mock_radio_clear_tx_log(mock_radio_t *m) { if (m) { m->tx_count = 0; } }

size_t mock_radio_tx_count_kind(const mock_radio_t *m, hh_frame_kind_t kind)
{
    size_t n = 0, lim;
    if (!m) return 0;
    lim = m->tx_count < MOCK_TX_LOG ? m->tx_count : MOCK_TX_LOG;
    for (size_t i = 0; i < lim; i++) if (m->tx_log[i].kind == kind) n++;
    return n;
}

const hh_frame_t *mock_radio_last_tx(const mock_radio_t *m, hh_frame_kind_t kind)
{
    size_t lim;
    if (!m || m->tx_count == 0) return NULL;
    lim = m->tx_count < MOCK_TX_LOG ? m->tx_count : MOCK_TX_LOG;
    for (size_t i = lim; i > 0; i--) if (m->tx_log[i-1].kind == kind) return &m->tx_log[i-1];
    return NULL;
}
