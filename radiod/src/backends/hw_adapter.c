/*
 * Hardware radio adapter — PLACEHOLDER.
 * See include/hhsdr/radio/hw_adapter.h for what real integration must supply.
 * No FPGA, AXI, DMA, PHY, or RF API is invented here.
 */
#include "hhsdr/radio/hw_adapter.h"
#include "hhsdr/core/log.h"
#include <string.h>

#define COMP "radio.hw"

static hh_status_t hw_open(void *self)
{
    hh_hw_adapter_t *a = self;
    if (!a) return HH_ERR_INVAL;
    /* Loud and unambiguous: this node has no radio backend. */
    HH_LOGE(COMP, "not_implemented",
            "op=open detail=%s",
            "FPGA/PL radio API unavailable; no hardware contract defined yet");
    return HH_ERR_NOT_IMPLEMENTED;
}

static hh_status_t hw_close(void *self)
{
    hh_hw_adapter_t *a = self;
    if (!a) return HH_ERR_INVAL;
    a->opened = false;
    return HH_OK;
}

static hh_status_t hw_transmit(void *self, const hh_frame_t *frame)
{
    (void)self; (void)frame;
    /* TBD: hand the framed message to the PL modulation path. */
    return HH_ERR_NOT_IMPLEMENTED;
}

static hh_status_t hw_set_rx_callback(void *self, hh_radio_rx_fn fn, void *ctx)
{
    (void)self; (void)fn; (void)ctx;
    /* TBD: register the demodulated-frame delivery path. */
    return HH_ERR_NOT_IMPLEMENTED;
}

static hh_status_t hw_get_status(void *self, hh_radio_status_t *out)
{
    hh_hw_adapter_t *a = self;
    if (!a || !out) return HH_ERR_INVAL;
    /* Report an honest "not operational" rather than fabricating counters. */
    memset(out, 0, sizeof *out);
    out->operational = false;
    out->channel     = a->channel;
    return HH_ERR_NOT_IMPLEMENTED;
}

static hh_status_t hw_get_link_metrics(void *self, hh_node_id_t neighbor,
                                       hh_link_sample_t *out)
{
    (void)self; (void)neighbor; (void)out;
    /* TBD: PHY-reported RSSI/SNR/PER and MAC retransmit counters. */
    return HH_ERR_NOT_IMPLEMENTED;
}

static hh_status_t hw_set_channel(void *self, uint32_t channel)
{
    (void)self; (void)channel;
    /* TBD: retune. Channel agility is itself a hardware capability question. */
    return HH_ERR_NOT_IMPLEMENTED;
}

static hh_status_t hw_poll(void *self, hh_time_ms_t now)
{
    (void)self; (void)now;
    return HH_ERR_NOT_IMPLEMENTED;
}

static const hh_radio_ops_t g_hw_ops = {
    .name             = "hw",
    .open             = hw_open,
    .close            = hw_close,
    .transmit         = hw_transmit,
    .set_rx_callback  = hw_set_rx_callback,
    .get_status       = hw_get_status,
    .get_link_metrics = hw_get_link_metrics,
    .set_channel      = hw_set_channel,
    .poll             = hw_poll,
};

void hh_hw_adapter_init(hh_hw_adapter_t *a, hh_radio_t *out)
{
    if (!a || !out) return;
    memset(a, 0, sizeof *a);
    out->ops  = &g_hw_ops;
    out->self = a;
}

bool hh_hw_adapter_is_stub(void) { return true; }
