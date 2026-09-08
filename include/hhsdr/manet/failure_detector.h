/*
 * Failure Detector.
 *
 * Owns debounced, hysteresis-guarded suspected->confirmed failure transitions.
 * It explicitly "doesn't recompute routes" — it only confirms, and publishes
 * FailureDetected (HTI-10) for Self-Healing and Routing to act on.
 *
 * Node-failure confirmation: a node is removed from the aggregate
 * topology only once every neighbor that could see it independently confirms,
 * so one bad link cannot delete a still-reachable node. Locally this component
 * confirms what THIS node observes; the corroboration count is tracked so the
 * Topology Manager can apply that rule.
 */
#ifndef HHSDR_MANET_FAILURE_DETECTOR_H
#define HHSDR_MANET_FAILURE_DETECTOR_H

#include "hhsdr/core/clock.h"
#include "hhsdr/core/config.h"
#include "hhsdr/core/dispatcher.h"

#define HH_FD_MAX_TRACKED 64

typedef struct {
    hh_node_id_t    id;
    bool            used;
    bool            confirmed;         /* failure already announced */
    hh_cause_hint_t cause;
    hh_time_ms_t    suspected_at;
    hh_time_ms_t    confirmed_at;
    uint32_t        corroborations;    /* independent confirmations seen */
} hh_fd_entry_t;

typedef struct {
    const hh_config_t *cfg;
    const hh_clock_t  *clock;
    hh_dispatcher_t   *bus;

    hh_fd_entry_t entries[HH_FD_MAX_TRACKED];
    uint64_t      confirmations;
    uint64_t      retractions;
} hh_failure_detector_t;

hh_status_t hh_fd_init(hh_failure_detector_t *fd, const hh_config_t *cfg,
                       const hh_clock_t *clock, hh_dispatcher_t *bus);

/* Consume HTI-07 LinkStateChanged. Confirms on entry to Failed, retracts when a
 * previously failed link recovers. */
hh_status_t hh_fd_on_link_state(hh_failure_detector_t *fd,
                                const hh_ev_link_state_t *ev, hh_time_ms_t now);

/* Record that another neighbor also observed this node as failed. */
void hh_fd_corroborate(hh_failure_detector_t *fd, hh_node_id_t id);

bool     hh_fd_is_failed(const hh_failure_detector_t *fd, hh_node_id_t id);
uint32_t hh_fd_corroborations(const hh_failure_detector_t *fd, hh_node_id_t id);

#endif /* HHSDR_MANET_FAILURE_DETECTOR_H */
