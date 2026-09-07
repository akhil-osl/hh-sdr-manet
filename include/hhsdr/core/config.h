/*
 * Node configuration (Doc 1 §10 / task §10: configuration separated from
 * implementation; no topology or scenario hard-coded into production code).
 *
 * Every value here is a tunable Doc 1 names but does not assign a number to
 * (HTI spec §12 item 4 records all timer/threshold values as TBD). The defaults
 * below are this implementation's chosen operating point, not a specification
 * value, and each is overridable from a config file at runtime.
 *
 * These fields are also the SCA PRF property surface (Doc 1 §12): each maps to a
 * `configure`-kind property applied via CF::PropertySet::configure.
 */
#ifndef HHSDR_CORE_CONFIG_H
#define HHSDR_CORE_CONFIG_H

#include "hhsdr/core/log.h"
#include "hhsdr/core/types.h"

typedef struct {
    /* ---- identity ---- */
    hh_node_id_t node_id;
    uint32_t     capabilities;        /* HH_CAP_* bit-field                     */
    bool         routing_capable;

    /* ---- discovery (Doc 1 §4) ---- */
    uint32_t beacon_interval_acq_ms;  /* fast acquisition cadence at startup    */
    uint32_t beacon_interval_ms;      /* steady-state cadence                   */
    uint32_t beacon_interval_min_ms;  /* adaptive-cadence lower bound (HTI-16)  */
    uint32_t beacon_interval_max_ms;  /* adaptive-cadence upper bound           */
    uint32_t acquisition_timeout_ms;  /* bounded timeout leaving acquisition    */

    /* ---- neighbor management (Doc 1 §4) ---- */
    uint32_t neighbor_allowed_loss;   /* missed beacons tolerated (by cadence)  */
    uint32_t max_neighbors;           /* bounded table, LRU eviction            */

    /* ---- link health (Doc 1 §7) ---- */
    float    lh_degrade_threshold;    /* enter Degraded below this fused score  */
    float    lh_recover_threshold;    /* high-water mark to return Healthy      */
    uint32_t lh_ewma_shift;           /* EWMA smoothing (score >>= shift)       */
    uint32_t lh_suspect_hold_ms;      /* SuspectedFailure -> Failed hold-down   */
    uint32_t lh_recover_hold_ms;      /* Recovering -> Healthy hold-down        */
    uint32_t lh_min_signals_suspect;  /* independent signals to reach Suspected */

    /* ---- routing (Doc 1 §6) ---- */
    uint32_t route_active_timeout_ms; /* ACTIVE_ROUTE_TIMEOUT analogue          */
    uint32_t route_delete_period_ms;  /* DELETE_PERIOD grace before removal     */
    uint32_t route_update_interval_ms;/* proactive update cadence               */
    uint32_t max_routes;
    uint8_t  max_hop_count;
    float    metric_w_quality;        /* composite metric term weights          */
    float    metric_w_hop;
    float    metric_w_age;

    /* ---- recovery (Doc 1 §8) ---- */
    uint32_t hold_down_ms;            /* new route stability window             */
    uint32_t merge_hold_down_ms;      /* trust merged routes as primary after   */
    uint32_t dampening_flap_threshold;/* flaps in window before penalty         */
    uint32_t dampening_window_ms;
    uint32_t dampening_penalty_ms;    /* cooldown during which next-hop pays    */
    uint32_t rediscovery_backoff_ms;  /* bounded retry base                     */
    uint32_t rediscovery_max_retries;

    /* ---- data plane ---- */
    uint32_t forward_queue_depth;     /* buffered on route miss, then dropped   */

    /* ---- observability / radio selection ---- */
    hh_log_level_t log_level;
    char           radio_adapter[32]; /* adapter name, e.g. "hw" or a test one  */
} hh_config_t;

/* Populate with the documented defaults. Never fails. */
void hh_config_defaults(hh_config_t *cfg);

/* Validate invariants (bounds ordering, non-zero intervals, sane thresholds). */
hh_status_t hh_config_validate(const hh_config_t *cfg);

/* Apply one `key = value` pair. Returns HH_ERR_NOTFOUND for an unknown key and
 * HH_ERR_INVAL for a malformed value, so callers can report the offending line. */
hh_status_t hh_config_set(hh_config_t *cfg, const char *key, const char *value);

/* Load a `key = value` file; `#` and `;` begin comments, blank lines ignored.
 * On a bad line, reports the 1-based line number through *err_line when non-NULL. */
hh_status_t hh_config_load_file(hh_config_t *cfg, const char *path, int *err_line);

#endif /* HHSDR_CORE_CONFIG_H */
