#include "hhsdr/manet/link_health.h"
#include "hhsdr/core/log.h"
#include <string.h>

#define COMP "linkhealth"

/* Signal quality thresholds. HTI spec §12 items 2/4 record units and thresholds
 * as TBD; these are this implementation's operating point over the float values
 * the radio abstraction carries, not specification values. */
#define RSSI_GOOD_DBM   (-70.0f)
#define RSSI_BAD_DBM    (-85.0f)
#define SNR_GOOD_DB     (15.0f)
#define SNR_BAD_DB      (5.0f)
#define PER_BAD         (0.20f)
#define LATENCY_BAD_MS  (500.0f)
#define RSSI_TREND_FALLING (-1.5f)   /* dBm per sample, sustained => mobility */

hh_status_t hh_link_health_init(hh_link_health_t *lh, const hh_config_t *cfg,
                                const hh_clock_t *clock, hh_dispatcher_t *bus)
{
    if (!lh || !cfg || !clock || !bus) return HH_ERR_INVAL;
    memset(lh, 0, sizeof *lh);
    lh->cfg = cfg;
    lh->clock = clock;
    lh->bus = bus;
    return HH_OK;
}

static hh_link_t *find(hh_link_health_t *lh, hh_node_id_t id)
{
    for (size_t i = 0; i < HH_LH_MAX_LINKS; i++)
        if (lh->links[i].used && lh->links[i].id == id) return &lh->links[i];
    return NULL;
}

hh_status_t hh_link_health_add(hh_link_health_t *lh, hh_node_id_t id, hh_time_ms_t now)
{
    if (!lh || id == HH_NODE_ID_INVALID) return HH_ERR_INVAL;
    if (find(lh, id)) return HH_OK;

    for (size_t i = 0; i < HH_LH_MAX_LINKS; i++) {
        hh_link_t *l = &lh->links[i];
        if (l->used) continue;
        memset(l, 0, sizeof *l);
        l->used  = true;
        l->id    = id;
        /* A newly discovered neighbor starts Healthy: it was just heard from. */
        l->state = HH_LINK_HEALTHY;
        l->score = 1.0f;
        l->state_since    = now;
        l->last_beacon_at = now;
        l->last_sample_at = now;
        l->flap_window_start = now;
        lh->count++;
        HH_LOGD(COMP, "link_tracked", "neighbor=%u state=Healthy", id);
        return HH_OK;
    }
    return HH_ERR_NOMEM;
}

hh_status_t hh_link_health_remove(hh_link_health_t *lh, hh_node_id_t id)
{
    hh_link_t *l;
    if (!lh) return HH_ERR_INVAL;
    l = find(lh, id);
    if (!l) return HH_ERR_NOTFOUND;
    memset(l, 0, sizeof *l);
    lh->count--;
    return HH_OK;
}

/*
 * Classify the failure cause from the observed pattern (Doc 1 §7).
 * This is a diagnostic hint, deliberately not a verdict: the Failure Detector
 * confirms, and Self-Healing chooses a strategy from it.
 */
static hh_cause_hint_t classify(const hh_link_health_t *lh, const hh_link_t *l,
                                hh_time_ms_t now)
{
    bool silent = (now - l->last_beacon_at) > (hh_time_ms_t)lh->cfg->beacon_interval_ms * 2;

    /* Sudden total silence across every neighbor => our own radio, not theirs. */
    if (lh->count > 1 && lh->last_any_rx_at != 0 &&
        (now - lh->last_any_rx_at) > (hh_time_ms_t)lh->cfg->beacon_interval_ms * 2)
        return HH_CAUSE_OWN_RADIO_FAILURE;

    /* PHY errors on several neighbors at once => the channel, not a node. */
    if ((l->bad_signals & HH_SIG_PHY_ERR) && lh->links_with_phy_errors > 1)
        return HH_CAUSE_RF_INTERFERENCE;

    /* RSSI falling steadily with loss rising in step => moving out of range. */
    if (l->rssi_trend <= RSSI_TREND_FALLING && (l->bad_signals & HH_SIG_PER))
        return HH_CAUSE_MOBILITY;

    /* Beacons landing but data ACKs failing => asymmetric link/contention,
     * which Doc 1 §7 says stays Degraded rather than reading as failure. */
    if ((l->bad_signals & HH_SIG_ACK) && !(l->bad_signals & HH_SIG_BEACON))
        return HH_CAUSE_ASYMMETRIC_LINK;

    /* Clean silence from one neighbor with no error indication. */
    if (silent && !(l->bad_signals & HH_SIG_PHY_ERR))
        return HH_CAUSE_NODE_FAILURE;

    return HH_CAUSE_UNKNOWN;
}

static void transition(hh_link_health_t *lh, hh_link_t *l, hh_link_state_t next,
                       hh_time_ms_t now)
{
    hh_event_t ev;
    hh_link_state_t prev = l->state;
    hh_cause_hint_t cause;

    if (prev == next) return;
    cause = classify(lh, l, now);

    /* Flap accounting for route dampening (Doc 1 §8): count Failed->Recovering
     * cycles in a rolling window. */
    if (prev == HH_LINK_FAILED && next == HH_LINK_RECOVERING) {
        if (now - l->flap_window_start > lh->cfg->dampening_window_ms) {
            l->flap_window_start = now;
            l->flap_count = 0;
        }
        l->flap_count++;
    }

    l->state = next;
    l->state_since = now;
    l->last_cause = cause;
    lh->transitions++;

    HH_LOGI(COMP, "link_state_changed", "neighbor=%u old=%s new=%s cause=%s score=%.3f "
            "bad_signals=0x%x", l->id, hh_link_state_str(prev), hh_link_state_str(next),
            hh_cause_hint_str(cause), (double)l->score, l->bad_signals);

    memset(&ev, 0, sizeof ev);
    ev.type = HH_EV_LINK_STATE_CHANGED;
    ev.timestamp = now;
    ev.u.link_state.neighbor   = l->id;
    ev.u.link_state.old_state  = prev;
    ev.u.link_state.new_state  = next;
    ev.u.link_state.cause_hint = cause;
    hh_dispatcher_publish(lh->bus, &ev);
}

/* Map one signal to a 0..1 quality contribution. */
static float ramp(float v, float bad, float good)
{
    if (good > bad) { if (v <= bad) return 0.0f; if (v >= good) return 1.0f; }
    else            { if (v >= bad) return 0.0f; if (v <= good) return 1.0f; }
    return (v - bad) / (good - bad);
}

/*
 * Fuse the available signals into one 0..1 score and record which signal types
 * are bad. Only signals the radio actually reported participate: an absent
 * metric is excluded rather than counted as zero, which would misread a radio
 * that cannot measure SNR as a failing link.
 */
static float fuse(hh_link_t *l, const hh_link_sample_t *s)
{
    float sum = 0.0f;
    float worst = 1.0f;
    int   terms = 0;
    uint32_t bad = 0;

    /* RSSI (always reported by any radio that can receive a frame). */
    {
        float q = ramp(s->rssi, RSSI_BAD_DBM, RSSI_GOOD_DBM);
        sum += q; terms++; if (q < worst) worst = q;
        if (s->rssi <= RSSI_BAD_DBM) bad |= HH_SIG_RSSI;

        if (l->prev_rssi_valid) {
            float d = s->rssi - l->prev_rssi;
            l->rssi_trend = l->rssi_trend * 0.5f + d * 0.5f;   /* smoothed */
        }
        l->prev_rssi = s->rssi;
        l->prev_rssi_valid = true;
    }
    /* SNR. */
    {
        float q = ramp(s->snr, SNR_BAD_DB, SNR_GOOD_DB);
        sum += q; terms++; if (q < worst) worst = q;
        if (s->snr <= SNR_BAD_DB) bad |= HH_SIG_SNR;
    }
    /* Packet error rate. */
    {
        float q = 1.0f - (s->per > 1.0f ? 1.0f : (s->per < 0.0f ? 0.0f : s->per));
        sum += q; terms++; if (q < worst) worst = q;
        if (s->per >= PER_BAD) bad |= HH_SIG_PER;
    }
    /* PHY/decode errors — distinct from MAC loss, and the key discriminator
     * between RF interference and a node simply going away (Doc 1 §7). */
    if (s->phy_errors > 0) bad |= HH_SIG_PHY_ERR;

    /* Data-plane ACK outcome, only when the adapter can report it. */
    if (s->ack_valid) {
        float q = s->ack_success ? 1.0f : 0.0f;
        sum += q; terms++; if (q < worst) worst = q;
        if (!s->ack_success) bad |= HH_SIG_ACK;
    }
    /* Latency trend, only when measured. */
    if (s->latency_valid) {
        float q = ramp(s->latency_ms, LATENCY_BAD_MS, 0.0f);
        sum += q; terms++; if (q < worst) worst = q;
        if (s->latency_ms >= LATENCY_BAD_MS) bad |= HH_SIG_LATENCY;
    }

    l->bad_signals = bad;
    if (terms == 0) return 1.0f;

    /* Blend the mean with the worst single term rather than taking a plain
     * average. A plain mean lets two good signals mask one catastrophic one --
     * a link whose RSSI is at the noise floor is not "healthy" because its SNR
     * reads well. Weighting the worst term keeps a single failing signal
     * visible in the score, while the >=2-independent-signals rule in
     * reevaluate() still governs whether the link may pass Degraded. */
    {
        float mean = sum / (float)terms;
        return mean * 0.5f + worst * 0.5f;
    }
}

/* Count distinct bad signal types, for the ">= 2 independent signals" rule. */
static uint32_t popcount_signals(uint32_t bits)
{
    uint32_t n = 0;
    while (bits) { n += bits & 1u; bits >>= 1; }
    return n;
}

static void reevaluate(hh_link_health_t *lh, hh_link_t *l, hh_time_ms_t now)
{
    const hh_config_t *c = lh->cfg;
    uint32_t nbad = popcount_signals(l->bad_signals);

    switch (l->state) {
    case HH_LINK_HEALTHY:
        if (l->score < c->lh_degrade_threshold)
            transition(lh, l, HH_LINK_DEGRADED, now);
        break;

    case HH_LINK_DEGRADED:
        /* ">= 2 independent signals miss" — no single signal may push a link
         * past Degraded, however bad that one signal is (Doc 1 §7). */
        if (nbad >= c->lh_min_signals_suspect)
            transition(lh, l, HH_LINK_SUSPECTED_FAILURE, now);
        /* Hysteresis: recovery needs the stricter high-water mark. */
        else if (l->score >= c->lh_recover_threshold)
            transition(lh, l, HH_LINK_HEALTHY, now);
        break;

    case HH_LINK_SUSPECTED_FAILURE:
        if (nbad < c->lh_min_signals_suspect) {
            /* Signals recovered before the hold time elapsed. */
            transition(lh, l, HH_LINK_DEGRADED, now);
        } else if (now - l->state_since >= c->lh_suspect_hold_ms) {
            /* Suspicion persisted past the confirmation hold-down. */
            transition(lh, l, HH_LINK_FAILED, now);
        }
        break;

    case HH_LINK_FAILED:
        /* Only a fresh valid beacon leaves Failed (hh_link_health_on_beacon). */
        break;

    case HH_LINK_RECOVERING:
        if (nbad >= c->lh_min_signals_suspect) {
            /* Relapse before the hold-down completed. */
            transition(lh, l, HH_LINK_FAILED, now);
        } else if (now - l->state_since >= c->lh_recover_hold_ms &&
                   l->score >= c->lh_recover_threshold) {
            transition(lh, l, HH_LINK_HEALTHY, now);
        }
        break;
    }
}

/* Publish an adaptive-cadence hint (HTI-16) only when the verdict changes, so
 * a stable neighborhood generates no control traffic. */
static void maybe_publish_cadence(hh_link_health_t *lh, hh_time_ms_t now)
{
    size_t unhealthy = 0;
    bool unstable;
    hh_event_t ev;

    for (size_t i = 0; i < HH_LH_MAX_LINKS; i++)
        if (lh->links[i].used && lh->links[i].state != HH_LINK_HEALTHY) unhealthy++;

    unstable = unhealthy > 0;
    if (unstable == lh->cadence_unstable) return;
    lh->cadence_unstable = unstable;

    memset(&ev, 0, sizeof ev);
    ev.type = HH_EV_CADENCE_HINT;
    ev.timestamp = now;
    ev.u.cadence.unstable = unstable;
    ev.u.cadence.instability = lh->count ? (float)unhealthy / (float)lh->count : 0.0f;
    hh_dispatcher_publish(lh->bus, &ev);
}

hh_status_t hh_link_health_on_sample(hh_link_health_t *lh, const hh_link_sample_t *s,
                                     hh_time_ms_t now)
{
    hh_link_t *l;
    float raw;
    uint32_t had_phy;

    if (!lh || !s) return HH_ERR_INVAL;
    l = find(lh, s->neighbor_id);
    if (!l) return HH_ERR_NOTFOUND;

    had_phy = l->bad_signals & HH_SIG_PHY_ERR;
    raw = fuse(l, s);

    /* Track how many links show PHY errors, which distinguishes a jammed
     * channel from one neighbor disappearing. */
    if (!had_phy && (l->bad_signals & HH_SIG_PHY_ERR)) lh->links_with_phy_errors++;
    else if (had_phy && !(l->bad_signals & HH_SIG_PHY_ERR) && lh->links_with_phy_errors)
        lh->links_with_phy_errors--;

    /* EWMA so a single bad sample cannot move the state: "requires sustained
     * evidence, not one bad sample" (Doc 1 §7). */
    if (!l->score_primed) {
        l->score = raw;
        l->score_primed = true;
    } else {
        uint32_t sh = lh->cfg->lh_ewma_shift ? lh->cfg->lh_ewma_shift : 2;
        float a = 1.0f / (float)(1u << sh);
        l->score = l->score * (1.0f - a) + raw * a;
    }

    l->last_sample_at = now;
    lh->last_any_rx_at = now;

    reevaluate(lh, l, now);
    maybe_publish_cadence(lh, now);
    return HH_OK;
}

hh_status_t hh_link_health_on_beacon(hh_link_health_t *lh, hh_node_id_t id,
                                     hh_time_ms_t now)
{
    hh_link_t *l;
    if (!lh) return HH_ERR_INVAL;
    l = find(lh, id);
    if (!l) return HH_ERR_NOTFOUND;

    l->last_beacon_at  = now;
    lh->last_any_rx_at = now;
    l->bad_signals &= ~HH_SIG_BEACON;

    /* Doc 1 §7: "Failed -> Recovering: fresh valid beacon received." */
    if (l->state == HH_LINK_FAILED)
        transition(lh, l, HH_LINK_RECOVERING, now);

    return HH_OK;
}

size_t hh_link_health_tick(hh_link_health_t *lh, hh_time_ms_t now)
{
    size_t changed = 0;

    if (!lh) return 0;
    for (size_t i = 0; i < HH_LH_MAX_LINKS; i++) {
        hh_link_t *l = &lh->links[i];
        hh_link_state_t before;
        hh_time_ms_t silence;
        uint32_t miss_deadline;
        if (!l->used) continue;

        before = l->state;

        /* Beacon silence is itself a signal: mark it bad once the allowed loss
         * count is exceeded, so silence participates in the >=2-signal rule. */
        silence = now - l->last_beacon_at;
        miss_deadline = lh->cfg->beacon_interval_ms * lh->cfg->neighbor_allowed_loss;
        if (silence > miss_deadline) l->bad_signals |= HH_SIG_BEACON;

        /* Sustained silence also drives the score down without new samples,
         * otherwise a link that goes quiet would hold its last good score. */
        if (silence > (hh_time_ms_t)lh->cfg->beacon_interval_ms * 2) {
            l->score *= 0.5f;
            if (l->state == HH_LINK_HEALTHY && l->score < lh->cfg->lh_degrade_threshold)
                transition(lh, l, HH_LINK_DEGRADED, now);
        }

        /*
         * A neighbor that has missed every expected beacon past the allowed
         * loss count is not merely degraded: every signal we have depends on
         * receiving frames from it, so prolonged silence is decisive on its own.
         * Without this a silent link stalls at Degraded, and the neighbor is
         * eventually deleted by expiry before the state machine ever reaches
         * Failed -- which would bypass the whole confirmation and recovery
         * pipeline that Doc 1 §8 specifies.
         */
        if (silence > (hh_time_ms_t)miss_deadline &&
            l->state != HH_LINK_FAILED && l->state != HH_LINK_RECOVERING) {
            l->bad_signals |= HH_SIG_BEACON | HH_SIG_RSSI;
            if (l->state == HH_LINK_HEALTHY)
                transition(lh, l, HH_LINK_DEGRADED, now);
            if (l->state == HH_LINK_DEGRADED)
                transition(lh, l, HH_LINK_SUSPECTED_FAILURE, now);
            /* Confirmation still respects the hold-down: reaching Failed
             * requires the suspicion to persist, exactly as for any other
             * cause. */
            if (l->state == HH_LINK_SUSPECTED_FAILURE &&
                now - l->state_since >= lh->cfg->lh_suspect_hold_ms)
                transition(lh, l, HH_LINK_FAILED, now);
        }

        reevaluate(lh, l, now);
        if (l->state != before) changed++;
    }
    maybe_publish_cadence(lh, now);
    return changed;
}

const hh_link_t *hh_link_health_get(const hh_link_health_t *lh, hh_node_id_t id)
{
    if (!lh) return NULL;
    for (size_t i = 0; i < HH_LH_MAX_LINKS; i++)
        if (lh->links[i].used && lh->links[i].id == id) return &lh->links[i];
    return NULL;
}

hh_link_state_t hh_link_health_state(const hh_link_health_t *lh, hh_node_id_t id)
{
    const hh_link_t *l = hh_link_health_get(lh, id);
    return l ? l->state : HH_LINK_FAILED;
}

float hh_link_health_score(const hh_link_health_t *lh, hh_node_id_t id)
{
    const hh_link_t *l = hh_link_health_get(lh, id);
    return l ? l->score : 0.0f;
}
