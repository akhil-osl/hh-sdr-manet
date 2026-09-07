/*
 * Link Health Monitor (Doc 1 §7; SCA class: Resource).
 *
 * Owns multi-signal fusion into a per-neighbor link state. It explicitly does
 * NOT decide what to do about a bad link — that is Self-Healing's job — and it
 * does not confirm failures, which is the Failure Detector's.
 *
 * State machine (Doc 1 §7):
 *   Healthy -> Degraded          fused score drops, sustained
 *   Degraded -> Healthy          score recovers past the high-water mark
 *   Degraded -> SuspectedFailure >= 2 independent signals miss
 *   SuspectedFailure -> Degraded signals recover before hold time
 *   SuspectedFailure -> Failed   suspicion persists past hold-down
 *   Failed -> Recovering         fresh valid beacon received
 *   Recovering -> Healthy        stable through hold-down window
 *   Recovering -> Failed         relapse before hold-down completes
 *
 * Hysteresis is structural: the exit threshold from a worse state is stricter
 * than the entry threshold (lh_recover_threshold > lh_degrade_threshold, which
 * hh_config_validate enforces), so a link cannot oscillate on noise alone.
 *
 * "No single signal alone can move a link past Degraded" (Doc 1 §7) is enforced
 * by counting how many INDEPENDENT signal types are bad and requiring at least
 * lh_min_signals_suspect of them before SuspectedFailure.
 *
 * Signal availability: not every metric exists on every radio. Each signal
 * carries a validity flag, and absent signals are excluded from fusion rather
 * than being treated as zero — which would otherwise read as a perfect link.
 */
#ifndef HHSDR_MANET_LINK_HEALTH_H
#define HHSDR_MANET_LINK_HEALTH_H

#include "hhsdr/core/clock.h"
#include "hhsdr/core/config.h"
#include "hhsdr/core/dispatcher.h"

#define HH_LH_MAX_LINKS 64

/* Bad-signal bits, used both for the >=2-independent-signals rule and for
 * cause-hint classification. */
#define HH_SIG_RSSI      (1u << 0)
#define HH_SIG_SNR       (1u << 1)
#define HH_SIG_PER       (1u << 2)
#define HH_SIG_BEACON    (1u << 3)   /* beacon/heartbeat success ratio */
#define HH_SIG_ACK       (1u << 4)   /* data-plane ACK success ratio   */
#define HH_SIG_PHY_ERR   (1u << 5)   /* PHY/CRC decode errors          */
#define HH_SIG_LATENCY   (1u << 6)

typedef struct {
    hh_node_id_t    id;
    bool            used;
    hh_link_state_t state;

    float    score;              /* fused EWMA score, 0..1 (1 == best) */
    bool     score_primed;
    uint32_t bad_signals;        /* HH_SIG_* bits currently bad        */

    hh_time_ms_t state_since;    /* when the current state was entered */
    hh_time_ms_t last_sample_at;
    hh_time_ms_t last_beacon_at;

    /* Trend tracking, for distinguishing mobility from a clean failure. */
    float    prev_rssi;
    bool     prev_rssi_valid;
    float    rssi_trend;         /* negative == falling                */

    /* Flap accounting for route dampening (Doc 1 §8). */
    uint32_t     flap_count;
    hh_time_ms_t flap_window_start;

    hh_cause_hint_t last_cause;
} hh_link_t;

typedef struct {
    const hh_config_t *cfg;
    const hh_clock_t  *clock;
    hh_dispatcher_t   *bus;

    hh_link_t links[HH_LH_MAX_LINKS];
    size_t    count;

    /* Node-wide observations used to tell "my radio died" and "the channel is
     * jammed" apart from "one neighbor went away" (Doc 1 §7). */
    uint32_t     links_with_phy_errors;
    hh_time_ms_t last_any_rx_at;

    uint64_t transitions;
    bool     cadence_unstable;   /* last hint published, for edge detection */
} hh_link_health_t;

hh_status_t hh_link_health_init(hh_link_health_t *lh, const hh_config_t *cfg,
                                const hh_clock_t *clock, hh_dispatcher_t *bus);

/* Track / stop tracking a neighbor, driven by HTI-06 NeighborUp/Down. */
hh_status_t hh_link_health_add(hh_link_health_t *lh, hh_node_id_t id, hh_time_ms_t now);
hh_status_t hh_link_health_remove(hh_link_health_t *lh, hh_node_id_t id);

/* Feed a raw sample (HTI-05). Runs fusion and may publish LinkStateChanged. */
hh_status_t hh_link_health_on_sample(hh_link_health_t *lh, const hh_link_sample_t *s,
                                     hh_time_ms_t now);

/* A fresh valid beacon arrived: the Failed -> Recovering trigger (Doc 1 §7). */
hh_status_t hh_link_health_on_beacon(hh_link_health_t *lh, hh_node_id_t id,
                                     hh_time_ms_t now);

/* Time-driven transitions: hold-down expiry both ways, and silence detection. */
size_t hh_link_health_tick(hh_link_health_t *lh, hh_time_ms_t now);

const hh_link_t *hh_link_health_get(const hh_link_health_t *lh, hh_node_id_t id);
hh_link_state_t  hh_link_health_state(const hh_link_health_t *lh, hh_node_id_t id);
float            hh_link_health_score(const hh_link_health_t *lh, hh_node_id_t id);

#endif /* HHSDR_MANET_LINK_HEALTH_H */
