#include "hhsdr/netif/netif.h"
#include "hhsdr/core/log.h"
#include <string.h>

#define COMP "netif"

static bool kind_valid(hh_frame_kind_t k)
{
    return k == HH_FRAME_BEACON || k == HH_FRAME_DATA || k == HH_FRAME_ROUTING;
}

/* MAC already filtered by address, so no destination check here. */
static void radio_rx(const hh_frame_t *f, const hh_link_sample_t *m, void *ctx)
{
    hh_netif_t *nif = ctx;

    if (!nif || !f) return;
    if (!nif->opened) {
        nif->counters.rx_dropped_closed++;
        return;
    }
    if (!kind_valid(f->kind) || f->len > HH_NETIF_MAX_PAYLOAD) {
        nif->counters.rx_dropped_malformed++;
        HH_LOGW(COMP, "rx_dropped", "reason=malformed kind=%d from=%u len=%u",
                (int)f->kind, f->src, f->len);
        return;
    }
    if (!nif->deliver) {
        nif->counters.rx_dropped_no_sink++;
        HH_LOGD(COMP, "rx_dropped", "reason=no_sink from=%u len=%u", f->src, f->len);
        return;
    }
    nif->counters.rx_frames++;
    nif->counters.rx_bytes += f->len;
    HH_LOGT(COMP, "rx", "kind=%d from=%u dst=%u len=%u", (int)f->kind, f->src,
            f->dst, f->len);
    nif->deliver(f->kind, f->data, f->len, f->src, m, nif->deliver_ctx);
}

hh_status_t hh_netif_init(hh_netif_t *nif, hh_node_id_t self, hh_radio_t *radio)
{
    if (!nif || !radio || !radio->ops || self == HH_NODE_ID_INVALID)
        return HH_ERR_INVAL;
    memset(nif, 0, sizeof *nif);
    nif->self  = self;
    nif->radio = radio;
    HH_LOGI(COMP, "initialized", "node=%u radio=%s", self,
            radio->ops && radio->ops->name ? radio->ops->name : "?");
    return HH_OK;
}

void hh_netif_set_deliver(hh_netif_t *nif, hh_netif_deliver_fn fn, void *ctx)
{
    if (!nif) return;
    nif->deliver = fn;
    nif->deliver_ctx = ctx;
}

hh_status_t hh_netif_open(hh_netif_t *nif)
{
    hh_status_t st;

    if (!nif) return HH_ERR_INVAL;
    if (nif->opened) return HH_ERR_STATE;
    st = hh_radio_set_rx_callback(nif->radio, radio_rx, nif);
    if (st == HH_OK) st = hh_radio_open(nif->radio);
    if (st != HH_OK) {
        hh_radio_set_rx_callback(nif->radio, NULL, NULL);
        HH_LOGE(COMP, "radio_open_failed", "node=%u status=%s",
                nif->self, hh_status_str(st));
        return st;
    }
    nif->opened = true;
    HH_LOGI(COMP, "opened", "node=%u", nif->self);
    return HH_OK;
}

/* The netif is closed even if the radio reports an error, so the caller
 * may free it: the radio no longer holds a pointer to it. */
hh_status_t hh_netif_close(hh_netif_t *nif)
{
    hh_status_t st;

    if (!nif) return HH_ERR_INVAL;
    if (!nif->opened) return HH_ERR_STATE;
    nif->opened = false;
    hh_radio_set_rx_callback(nif->radio, NULL, NULL);
    st = hh_radio_close(nif->radio);
    if (st != HH_OK) {
        HH_LOGW(COMP, "radio_close_failed", "node=%u status=%s",
                nif->self, hh_status_str(st));
        return st;
    }
    HH_LOGI(COMP, "closed", "node=%u", nif->self);
    return HH_OK;
}

hh_status_t hh_netif_send(hh_netif_t *nif, hh_frame_kind_t kind,
                          hh_node_id_t next_hop,
                          const uint8_t *payload, uint16_t len)
{
    hh_frame_t f;
    hh_status_t st;
    const char *why = NULL;

    if (!nif) return HH_ERR_INVAL;
    if (!nif->opened) return HH_ERR_STATE;

    if (!kind_valid(kind))               why = "kind";
    else if (!payload && len)            why = "no_payload";
    /* No fragmentation; owner undecided (MAC-NETWORK-INTERFACE section 18). */
    else if (len > HH_NETIF_MAX_PAYLOAD) why = "too_large";
    if (why) {
        nif->counters.tx_rejected++;
        HH_LOGD(COMP, "tx_rejected", "reason=%s kind=%d len=%u max=%u", why,
                (int)kind, len, (unsigned)HH_NETIF_MAX_PAYLOAD);
        return HH_ERR_INVAL;
    }

    memset(&f, 0, sizeof f);
    f.kind = kind;
    f.src  = nif->self;
    f.dst  = next_hop;
    f.len  = len;
    if (len) memcpy(f.data, payload, len);

    st = hh_radio_transmit(nif->radio, &f);
    if (st != HH_OK) {
        nif->counters.tx_errors++;
        HH_LOGW(COMP, "tx_failed", "kind=%d next_hop=%u len=%u status=%s",
                (int)kind, next_hop, len, hh_status_str(st));
        return st;
    }
    nif->counters.tx_frames++;
    nif->counters.tx_bytes += len;
    if (next_hop == HH_NETIF_BROADCAST) nif->counters.tx_broadcast++;
    HH_LOGT(COMP, "tx", "kind=%d next_hop=%u len=%u", (int)kind, next_hop, len);
    return HH_OK;
}

hh_status_t hh_netif_poll(hh_netif_t *nif, hh_time_ms_t now)
{
    if (!nif) return HH_ERR_INVAL;
    if (!nif->opened) return HH_ERR_STATE;
    return hh_radio_poll(nif->radio, now);
}

const hh_netif_counters_t *hh_netif_counters(const hh_netif_t *nif)
{
    return nif ? &nif->counters : NULL;
}
