/*
 * Discovery Manager (Doc 1 §4; SCA class: Resource).
 *
 * Owns beacon tx/rx scheduling, initial and periodic discovery, and capability
 * advertisement. It explicitly does NOT own neighbor state validity — that is
 * the Neighbor Manager's, and this component only publishes validated beacon
 * events for it to consume.
 *
 * Lifecycle (Doc 1 §4):
 *   - Acquisition: beacon at a fast interval until either a neighbor is heard
 *     or acquisition_timeout_ms elapses, then drop to steady-state cadence.
 *   - Steady state: beacons double as the liveness heartbeat, so discovery and
 *     heartbeat are one message stream, not two.
 *   - Adaptive cadence (HTI-16): tightens on local instability, relaxes when
 *     stable, always clamped to [beacon_interval_min_ms, beacon_interval_max_ms].
 *
 * Duplicate detection: a beacon not newer than the last accepted sequence number
 * for that node is dropped (Doc 1 §4), using wraparound-safe comparison.
 */
#ifndef HHSDR_MANET_DISCOVERY_H
#define HHSDR_MANET_DISCOVERY_H

#include "hhsdr/core/clock.h"
#include "hhsdr/core/config.h"
#include "hhsdr/core/dispatcher.h"
#include "hhsdr/radio/radio.h"

typedef enum {
    HH_DISC_ACQUISITION = 0,   /* fast beaconing, no neighbor heard yet */
    HH_DISC_STEADY
} hh_disc_phase_t;

#define HH_DISC_MAX_TRACKED 64

typedef struct {
    const hh_config_t *cfg;
    const hh_clock_t  *clock;
    hh_dispatcher_t   *bus;
    hh_radio_t        *radio;

    hh_disc_phase_t phase;
    hh_seq_t        tx_seq;             /* our own outgoing sequence number   */
    hh_time_ms_t    started_at;
    hh_time_ms_t    last_beacon_at;
    uint32_t        interval_ms;        /* current cadence, adaptive          */
    bool            running;

    /* Last accepted sequence per source, for duplicate/replay rejection. */
    struct { hh_node_id_t id; hh_seq_t seq; bool used; } seen[HH_DISC_MAX_TRACKED];

    /* Counters, surfaced through telemetry. */
    uint64_t beacons_sent;
    uint64_t beacons_rx_accepted;
    uint64_t beacons_rx_duplicate;
    uint64_t beacons_rx_malformed;
    uint64_t tx_failures;
} hh_discovery_t;

hh_status_t hh_discovery_init(hh_discovery_t *d, const hh_config_t *cfg,
                              const hh_clock_t *clock, hh_dispatcher_t *bus,
                              hh_radio_t *radio);
hh_status_t hh_discovery_start(hh_discovery_t *d);
hh_status_t hh_discovery_stop(hh_discovery_t *d);

/* Emit a beacon if one is due. Called from the node's control loop. */
hh_status_t hh_discovery_tick(hh_discovery_t *d, hh_time_ms_t now);

/* HTI-04 ingress: validate a received beacon frame and, if accepted, publish
 * HH_EV_BEACON_RX for the Neighbor Manager. Returns:
 *   HH_OK          accepted and published
 *   HH_ERR_AGAIN   valid but not newer than the last accepted (duplicate)
 *   HH_ERR_INVAL   malformed, or our own beacon echoed back
 *   HH_ERR_UNSUPPORTED  protocol version mismatch */
hh_status_t hh_discovery_on_frame(hh_discovery_t *d, const hh_frame_t *f,
                                  const hh_link_sample_t *metrics);

/* HTI-16: apply an adaptive-cadence hint from the Link Health Monitor. */
void hh_discovery_apply_cadence_hint(hh_discovery_t *d, const hh_ev_cadence_hint_t *hint);

/* Leave acquisition early; called when the first neighbor is confirmed. */
void hh_discovery_notify_neighbor_heard(hh_discovery_t *d);

uint32_t hh_discovery_interval(const hh_discovery_t *d);

#endif /* HHSDR_MANET_DISCOVERY_H */
