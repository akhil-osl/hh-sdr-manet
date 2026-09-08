#include "hhsdr/dataplane/forwarder.h"
#include "hhsdr/core/log.h"
#include <string.h>

#define COMP "forwarder"

/* Data-plane frame header: TTL and the true source/destination, which survive
 * per-hop retransmission. Kept deliberately small — cost multiplies per hop. */
#define FWD_HDR_LEN 10u

hh_status_t hh_forwarder_init(hh_forwarder_t *fw, const hh_config_t *cfg,
                              hh_radio_t *radio, const hh_route_publisher_t *routes)
{
    if (!fw || !cfg || !radio || !routes) return HH_ERR_INVAL;
    memset(fw, 0, sizeof *fw);
    fw->cfg = cfg;
    fw->radio = radio;
    fw->routes = routes;
    return HH_OK;
}

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

static uint32_t get32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/*
 * The fast path. One snapshot pointer read, one flat lookup, one transmit.
 * No lock is taken and no control-plane computation is invoked.
 */
static hh_status_t transmit_via_route(hh_forwarder_t *fw, hh_frame_t *f,
                                      hh_node_id_t final_dst)
{
    const hh_route_snapshot_t *snap;
    const hh_route_entry_t *route;
    hh_status_t st;

    snap = hh_route_publisher_current(fw->routes);
    if (!snap) return HH_ERR_AGAIN;
    fw->last_snapshot_version = snap->version;

    route = hh_route_lookup(snap, final_dst);
    if (!route) return HH_ERR_AGAIN;   /* miss: control plane resolves it */

    f->dst = route->next_hop;
    st = hh_radio_transmit(fw->radio, f);
    if (st != HH_OK) {
        fw->dropped_tx_error++;
        HH_LOGW(COMP, "tx_failed", "dst=%u next_hop=%u status=%s",
                final_dst, route->next_hop, hh_status_str(st));
        return HH_ERR_IO;
    }
    fw->forwarded++;
    HH_LOGT(COMP, "forwarded", "dst=%u next_hop=%u snapshot_version=%u",
            final_dst, route->next_hop, snap->version);
    return HH_OK;
}

static hh_status_t enqueue_pending(hh_forwarder_t *fw, const hh_frame_t *f,
                                   hh_time_ms_t now)
{
    size_t limit = fw->cfg->forward_queue_depth < HH_FWD_QUEUE_MAX
                 ? fw->cfg->forward_queue_depth : HH_FWD_QUEUE_MAX;

    if (fw->pending_count >= limit) {
        /* Bounded: drop rather than grow. A data plane that buffers without
         * limit turns a routing outage into memory exhaustion. */
        fw->dropped_queue_full++;
        HH_LOGW(COMP, "dropped", "reason=queue_full depth=%zu", fw->pending_count);
        return HH_ERR_NOMEM;
    }
    for (size_t i = 0; i < HH_FWD_QUEUE_MAX; i++) {
        if (fw->pending[i].used) continue;
        fw->pending[i].used = true;
        fw->pending[i].frame = *f;
        fw->pending[i].queued_at = now;
        fw->pending_count++;
        fw->queued++;
        HH_LOGD(COMP, "queued", "reason=no_route depth=%zu", fw->pending_count);
        return HH_ERR_AGAIN;
    }
    return HH_ERR_NOMEM;
}

hh_status_t hh_forwarder_send(hh_forwarder_t *fw, hh_node_id_t dst,
                              const uint8_t *payload, uint16_t len,
                              uint8_t ttl, hh_time_ms_t now)
{
    hh_frame_t f;
    hh_status_t st;

    if (!fw || (!payload && len) || ttl == 0) return HH_ERR_INVAL;
    if ((size_t)len + FWD_HDR_LEN > HH_RADIO_MAX_FRAME) return HH_ERR_INVAL;

    /* A packet addressed to ourselves never touches the radio. */
    if (dst == fw->cfg->node_id) { fw->delivered_local++; return HH_OK; }

    memset(&f, 0, sizeof f);
    f.kind = HH_FRAME_DATA;
    f.src  = fw->cfg->node_id;
    put32(f.data + 0, fw->cfg->node_id);   /* original source      */
    put32(f.data + 4, dst);                /* final destination    */
    f.data[8] = ttl;
    f.data[9] = 0;                         /* hop count so far     */
    if (len) memcpy(f.data + FWD_HDR_LEN, payload, len);
    f.len = (uint16_t)(FWD_HDR_LEN + len);

    st = transmit_via_route(fw, &f, dst);
    if (st == HH_ERR_AGAIN) {
        fw->dropped_no_route++;
        return enqueue_pending(fw, &f, now);
    }
    return st;
}

hh_status_t hh_forwarder_forward(hh_forwarder_t *fw, const hh_frame_t *in,
                                 hh_time_ms_t now)
{
    hh_frame_t f;
    hh_node_id_t final_dst;
    hh_status_t st;

    if (!fw || !in || in->len < FWD_HDR_LEN) return HH_ERR_INVAL;
    if (in->kind != HH_FRAME_DATA) return HH_ERR_INVAL;

    f = *in;
    final_dst = get32(f.data + 4);

    /* Arrived at its destination: deliver locally, do not retransmit. */
    if (final_dst == fw->cfg->node_id) { fw->delivered_local++; return HH_OK; }

    /* TTL guards against a forwarding loop surviving a transient inconsistency. */
    if (f.data[8] <= 1) {
        fw->dropped_ttl++;
        HH_LOGD(COMP, "dropped", "reason=ttl_exhausted dst=%u", final_dst);
        return HH_ERR_INVAL;
    }
    f.data[8]--;
    if (f.data[9] < 255) f.data[9]++;
    f.src = fw->cfg->node_id;

    st = transmit_via_route(fw, &f, final_dst);
    if (st == HH_ERR_AGAIN) {
        fw->dropped_no_route++;
        return enqueue_pending(fw, &f, now);
    }
    return st;
}

size_t hh_forwarder_flush(hh_forwarder_t *fw, hh_time_ms_t now, uint32_t max_wait_ms)
{
    size_t sent = 0;

    if (!fw) return 0;
    for (size_t i = 0; i < HH_FWD_QUEUE_MAX; i++) {
        hh_pending_packet_t *p = &fw->pending[i];
        hh_node_id_t dst;
        if (!p->used) continue;

        if (now - p->queued_at > max_wait_ms) {
            /* Buffered "briefly", not indefinitely. */
            HH_LOGD(COMP, "dropped", "reason=pending_timeout waited_ms=%llu",
                    (unsigned long long)(now - p->queued_at));
            p->used = false;
            fw->pending_count--;
            continue;
        }
        dst = get32(p->frame.data + 4);
        if (transmit_via_route(fw, &p->frame, dst) == HH_OK) {
            p->used = false;
            fw->pending_count--;
            fw->requeued_ok++;
            sent++;
        }
    }
    return sent;
}

size_t hh_forwarder_pending(const hh_forwarder_t *fw)
{
    return fw ? fw->pending_count : 0;
}
