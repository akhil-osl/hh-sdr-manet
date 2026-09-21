#include "hhsdr/radiod/mock_backend.h"
#include <string.h>

static hh_status_t mb_open(void *self)
{
    hh_mock_backend_t *m = self;
    if (!m) return HH_ERR_INVAL;
    m->opened = true;
    m->operational = (m->active_fault != HH_RC_FAULT_HW_FAULT);
    return HH_OK;
}

static hh_status_t mb_close(void *self)
{
    hh_mock_backend_t *m = self;
    if (!m) return HH_ERR_INVAL;
    m->opened = false;
    return HH_OK;
}

static hh_status_t mb_transmit(void *self, const hh_frame_t *frame)
{
    hh_mock_backend_t *m = self;
    if (!m || !frame) return HH_ERR_INVAL;
    if (!m->opened) return HH_ERR_STATE;
    if (!m->operational || m->active_fault == HH_RC_FAULT_TX_FAILURE) {
        m->tx_errors++;
        return HH_ERR_IO;
    }
    m->frames_tx++;
    return HH_OK;
}

static hh_status_t mb_set_rx_callback(void *self, hh_radio_rx_fn fn, void *ctx)
{
    hh_mock_backend_t *m = self;
    if (!m) return HH_ERR_INVAL;
    m->rx_fn = fn;
    m->rx_ctx = ctx;
    return HH_OK;
}

static hh_status_t mb_get_status(void *self, hh_radio_status_t *out)
{
    hh_mock_backend_t *m = self;
    if (!m || !out) return HH_ERR_INVAL;
    memset(out, 0, sizeof *out);
    out->operational  = m->operational;
    out->channel      = m->channel;
    out->frequency_hz = m->frequency_hz;
    out->waveform_id  = m->waveform_id;
    out->frames_tx    = m->frames_tx;
    out->frames_rx    = m->frames_rx;
    out->tx_errors    = m->tx_errors;
    out->rx_errors    = m->rx_errors;
    return HH_OK;
}

static hh_status_t mb_get_link_metrics(void *self, hh_node_id_t neighbor,
                                       hh_link_sample_t *out)
{
    (void)self; (void)neighbor; (void)out;
    /* No per-neighbor tracking: HTI-05 is out of scope for this control-plane
     * daemon per the task boundary (data plane stays off generic daemon IPC). */
    return HH_ERR_UNSUPPORTED;
}

static hh_status_t mb_set_channel(void *self, uint32_t channel)
{
    hh_mock_backend_t *m = self;
    if (!m) return HH_ERR_INVAL;
    if (!m->opened) return HH_ERR_STATE;
    if (m->active_fault == HH_RC_FAULT_BACKEND_IO) return HH_ERR_IO;
    m->channel = channel;
    return HH_OK;
}

static hh_status_t mb_poll(void *self, hh_time_ms_t now)
{
    hh_mock_backend_t *m = self;
    (void)now;
    if (!m) return HH_ERR_INVAL;
    if (!m->opened) return HH_ERR_STATE;
    /* rx_silence models total inbound loss: poll runs but delivers nothing,
     * matching HTI-02's "sudden absence of received traffic" signal. Deliberately
     * no synthetic frame generation otherwise — this is not a PHY simulator. */
    return HH_OK;
}

static const hh_radio_ops_t g_mock_ops = {
    .name             = "mock",
    .open             = mb_open,
    .close            = mb_close,
    .transmit         = mb_transmit,
    .set_rx_callback  = mb_set_rx_callback,
    .get_status       = mb_get_status,
    .get_link_metrics = mb_get_link_metrics,
    .set_channel      = mb_set_channel,
    .poll             = mb_poll,
};

void hh_mock_backend_init(hh_mock_backend_t *m, hh_radio_t *out)
{
    if (!m || !out) return;
    memset(m, 0, sizeof *m);
    m->frequency_hz = 0.0f;
    m->waveform_id  = 0;
    out->ops  = &g_mock_ops;
    out->self = m;
}

void hh_mock_backend_inject_fault(hh_mock_backend_t *m, hh_rc_fault_t fault)
{
    if (!m) return;
    m->active_fault = fault;
    m->operational = (fault != HH_RC_FAULT_HW_FAULT) && m->opened;
}

hh_rc_fault_t hh_mock_backend_active_fault(const hh_mock_backend_t *m)
{
    return m ? m->active_fault : HH_RC_FAULT_NONE;
}
