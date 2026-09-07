#include "hhsdr/manet/discovery.h"
#include "hhsdr/core/log.h"
#include "hhsdr/core/seq.h"
#include "hhsdr/radio/wire.h"
#include <string.h>

#define COMP "discovery"

hh_status_t hh_discovery_init(hh_discovery_t *d, const hh_config_t *cfg,
                              const hh_clock_t *clock, hh_dispatcher_t *bus,
                              hh_radio_t *radio)
{
    if (!d || !cfg || !clock || !bus || !radio) return HH_ERR_INVAL;
    memset(d, 0, sizeof *d);
    d->cfg   = cfg;
    d->clock = clock;
    d->bus   = bus;
    d->radio = radio;
    /* initialize() must leave a known internal state before any wiring or
     * configuration occurs (Doc 1 §3 lifecycle step 2). */
    d->phase       = HH_DISC_ACQUISITION;
    d->interval_ms = cfg->beacon_interval_acq_ms;
    d->running     = false;
    return HH_OK;
}

hh_status_t hh_discovery_start(hh_discovery_t *d)
{
    if (!d || !d->cfg) return HH_ERR_INVAL;
    d->running    = true;
    d->started_at = hh_now(d->clock);
    /* Beacon immediately on start rather than waiting one interval: a node that
     * has just come up should be discoverable as fast as possible. */
    d->last_beacon_at = 0;
    d->phase          = HH_DISC_ACQUISITION;
    d->interval_ms    = d->cfg->beacon_interval_acq_ms;
    HH_LOGI(COMP, "started", "node=%u phase=acquisition interval_ms=%u",
            d->cfg->node_id, d->interval_ms);
    return HH_OK;
}

hh_status_t hh_discovery_stop(hh_discovery_t *d)
{
    if (!d) return HH_ERR_INVAL;
    d->running = false;
    HH_LOGI(COMP, "stopped", "node=%u beacons_sent=%llu",
            d->cfg ? d->cfg->node_id : 0, (unsigned long long)d->beacons_sent);
    return HH_OK;
}

uint32_t hh_discovery_interval(const hh_discovery_t *d)
{
    return d ? d->interval_ms : 0;
}

static void build_beacon(const hh_discovery_t *d, hh_time_ms_t now, hh_beacon_t *b)
{
    memset(b, 0, sizeof *b);
    b->node_id          = d->cfg->node_id;
    b->protocol_version = HH_PROTOCOL_VERSION;
    b->sequence_no      = d->tx_seq;
    b->timestamp        = now;
    b->capabilities     = d->cfg->capabilities;
    b->routing_capable  = d->cfg->routing_capable;
    /* radio_caps / supported_waveforms / channel_freq are HARDWARE-DEPENDENT:
     * they are advertised as zero until the radio can report them, rather than
     * fabricating capability claims the hardware may not honor. */
}

hh_status_t hh_discovery_tick(hh_discovery_t *d, hh_time_ms_t now)
{
    hh_frame_t f;
    hh_beacon_t b;
    size_t n;
    hh_status_t st;

    if (!d || !d->running) return HH_ERR_STATE;

    /* Bounded acquisition: drop to steady state on timeout even if no neighbor
     * was heard, so an isolated node does not beacon fast forever (Doc 1 §4). */
    if (d->phase == HH_DISC_ACQUISITION &&
        now - d->started_at >= d->cfg->acquisition_timeout_ms) {
        d->phase       = HH_DISC_STEADY;
        d->interval_ms = d->cfg->beacon_interval_ms;
        HH_LOGI(COMP, "phase_change", "node=%u phase=steady reason=acquisition_timeout "
                "interval_ms=%u", d->cfg->node_id, d->interval_ms);
    }

    if (d->last_beacon_at != 0 && now - d->last_beacon_at < d->interval_ms)
        return HH_ERR_AGAIN;    /* not due yet */

    build_beacon(d, now, &b);
    memset(&f, 0, sizeof f);
    f.kind = HH_FRAME_BEACON;
    f.src  = d->cfg->node_id;
    f.dst  = HH_NODE_ID_INVALID;   /* broadcast */
    n = hh_beacon_encode(&b, f.data, sizeof f.data);
    if (n == 0) return HH_ERR_INVAL;
    f.len = (uint16_t)n;

    st = hh_radio_transmit(d->radio, &f);
    if (st != HH_OK) {
        d->tx_failures++;
        HH_LOGW(COMP, "beacon_tx_failed", "node=%u seq=%u status=%s",
                d->cfg->node_id, d->tx_seq, hh_status_str(st));
        /* Still advance the schedule: retrying instantly would spin against a
         * radio that is down. The failure is counted and visible. */
        d->last_beacon_at = now;
        return st;
    }

    d->last_beacon_at = now;
    d->beacons_sent++;
    d->tx_seq++;                   /* wraps naturally; comparisons are serial */
    HH_LOGD(COMP, "beacon_tx", "node=%u seq=%u interval_ms=%u phase=%s",
            d->cfg->node_id, b.sequence_no, d->interval_ms,
            d->phase == HH_DISC_ACQUISITION ? "acquisition" : "steady");
    return HH_OK;
}

/* Returns the slot for src, allocating or LRU-replacing as needed. */
static size_t seen_slot(hh_discovery_t *d, hh_node_id_t src)
{
    size_t free_slot = HH_DISC_MAX_TRACKED;
    for (size_t i = 0; i < HH_DISC_MAX_TRACKED; i++) {
        if (d->seen[i].used && d->seen[i].id == src) return i;
        if (!d->seen[i].used && free_slot == HH_DISC_MAX_TRACKED) free_slot = i;
    }
    if (free_slot != HH_DISC_MAX_TRACKED) {
        d->seen[free_slot].used = true;
        d->seen[free_slot].id   = src;
        d->seen[free_slot].seq  = 0;
        return free_slot;
    }
    /* Table full: reuse slot 0. Bounded memory matters more here than perfect
     * duplicate suppression for an unusually large neighborhood. */
    d->seen[0].id  = src;
    d->seen[0].seq = 0;
    return 0;
}

hh_status_t hh_discovery_on_frame(hh_discovery_t *d, const hh_frame_t *f,
                                  const hh_link_sample_t *metrics)
{
    hh_beacon_t b;
    hh_status_t st;
    hh_event_t ev;
    size_t slot;
    bool first_from_src;

    if (!d || !f || !metrics) return HH_ERR_INVAL;
    if (f->kind != HH_FRAME_BEACON) return HH_ERR_INVAL;

    st = hh_beacon_decode(f->data, f->len, &b);
    if (st != HH_OK) {
        d->beacons_rx_malformed++;
        HH_LOGW(COMP, "beacon_rejected", "src=%u reason=%s",
                f->src, st == HH_ERR_UNSUPPORTED ? "version_mismatch" : "malformed");
        return st;
    }

    /* Ignore our own beacon reflected back by the medium. */
    if (b.node_id == d->cfg->node_id) return HH_ERR_INVAL;

    slot = seen_slot(d, b.node_id);
    first_from_src = (d->seen[slot].seq == 0);

    /* Doc 1 §4: "A beacon not newer than the last accepted one is dropped."
     * Serial comparison so a wrapped counter is still ordered correctly. */
    if (!first_from_src && !hh_seq_gt(b.sequence_no, d->seen[slot].seq)) {
        d->beacons_rx_duplicate++;
        HH_LOGD(COMP, "beacon_duplicate", "src=%u seq=%u last=%u",
                b.node_id, b.sequence_no, d->seen[slot].seq);
        return HH_ERR_AGAIN;
    }
    d->seen[slot].seq = b.sequence_no;
    d->beacons_rx_accepted++;

    /* Hearing any neighbor ends acquisition early (Doc 1 §4). */
    hh_discovery_notify_neighbor_heard(d);

    memset(&ev, 0, sizeof ev);
    ev.type      = HH_EV_BEACON_RX;
    ev.timestamp = hh_now(d->clock);
    ev.u.beacon_rx.beacon = b;
    ev.u.beacon_rx.sample = *metrics;
    ev.u.beacon_rx.sample.neighbor_id = b.node_id;

    HH_LOGD(COMP, "beacon_accepted", "src=%u seq=%u rssi=%.1f snr=%.1f",
            b.node_id, b.sequence_no, (double)metrics->rssi, (double)metrics->snr);
    return hh_dispatcher_publish(d->bus, &ev);
}

void hh_discovery_notify_neighbor_heard(hh_discovery_t *d)
{
    if (!d || d->phase != HH_DISC_ACQUISITION) return;
    d->phase       = HH_DISC_STEADY;
    d->interval_ms = d->cfg->beacon_interval_ms;
    HH_LOGI(COMP, "phase_change", "node=%u phase=steady reason=neighbor_heard "
            "interval_ms=%u", d->cfg->node_id, d->interval_ms);
}

void hh_discovery_apply_cadence_hint(hh_discovery_t *d, const hh_ev_cadence_hint_t *hint)
{
    uint32_t want;
    const hh_config_t *c;

    if (!d || !hint || !d->cfg) return;
    c = d->cfg;

    /* Doc 1 §4: tighten on instability for faster reconvergence, relax when
     * stable to bound control overhead. Always clamped to the configured
     * bounds so a runaway hint cannot flood the channel. */
    if (hint->unstable) {
        want = d->interval_ms / 2;
        if (want < c->beacon_interval_min_ms) want = c->beacon_interval_min_ms;
    } else {
        want = d->interval_ms * 2;
        if (want > c->beacon_interval_ms) want = c->beacon_interval_ms;
    }
    if (want < c->beacon_interval_min_ms) want = c->beacon_interval_min_ms;
    if (want > c->beacon_interval_max_ms) want = c->beacon_interval_max_ms;

    if (want != d->interval_ms) {
        HH_LOGI(COMP, "cadence_changed", "node=%u old_ms=%u new_ms=%u unstable=%d",
                c->node_id, d->interval_ms, want, (int)hint->unstable);
        d->interval_ms = want;
    }
}
