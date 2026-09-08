/*
 * Self-Healing / Recovery Manager.
 *
 * Orchestrates local repair, alternate routes, rediscovery, and partition/merge
 * handling. It does NOT do per-packet forwarding and does not itself compute
 * routes — it drives the Routing Engine and, for an RF cause, the radio.
 *
 * Recovery pipeline, entered on a confirmed failure:
 *   detection -> classification -> confirmation -> event -> topology update ->
 *   route invalidation -> local repair / alternate route -> rediscovery ->
 *   stabilization
 *
 * Strategy selection by cause hint:
 *  - RF interference  -> prefer a channel change over route churn (HTI-14).
 *    When the radio cannot retune, fall back to route-based recovery. The HW/SW
 *    spec leaves that fallback TBD; this stack chooses route-based
 *    recovery and logs the decision rather than failing the recovery.
 *  - anything else    -> invalidation cascade, then a warm alternate if one
 *    exists, else bounded rediscovery with backoff.
 */
#ifndef HHSDR_MANET_SELF_HEALING_H
#define HHSDR_MANET_SELF_HEALING_H

#include "hhsdr/manet/failure_detector.h"
#include "hhsdr/manet/routing.h"
#include "hhsdr/manet/topology.h"
#include "hhsdr/radio/radio.h"

#define HH_SH_MAX_ACTIVE 32

typedef struct {
    hh_node_id_t           target;
    bool                   used;
    hh_recovery_strategy_t strategy;
    hh_time_ms_t           started_at;
    hh_time_ms_t           next_retry_at;
    uint32_t               retries;
    bool                   completed;
} hh_recovery_t;

typedef struct {
    const hh_config_t *cfg;
    const hh_clock_t  *clock;
    hh_dispatcher_t   *bus;
    hh_routing_t      *routing;      /* driven, not read-only */
    hh_topology_t     *topology;
    hh_radio_t        *radio;

    hh_recovery_t active[HH_SH_MAX_ACTIVE];

    uint64_t recoveries_started;
    uint64_t recoveries_completed;
    uint64_t alternate_switches;
    uint64_t rediscoveries;
    uint64_t channel_changes;
    uint64_t channel_change_failures;
} hh_self_healing_t;

hh_status_t hh_sh_init(hh_self_healing_t *sh, const hh_config_t *cfg,
                       const hh_clock_t *clock, hh_dispatcher_t *bus,
                       hh_routing_t *routing, hh_topology_t *topology,
                       hh_radio_t *radio);

/* Consume HTI-10 FailureDetected and enter the recovery pipeline. */
hh_status_t hh_sh_on_failure(hh_self_healing_t *sh, const hh_ev_failure_t *ev,
                             hh_time_ms_t now);

/* Consume HTI-12 PartitionDetected. */
void hh_sh_on_partition(hh_self_healing_t *sh, const hh_ev_partition_t *ev,
                        hh_time_ms_t now);

/* Consume HTI-13 NetworkMerged. */
void hh_sh_on_merge(hh_self_healing_t *sh, const hh_ev_merged_t *ev, hh_time_ms_t now);

/* Drive retries and stabilization. Returns how many recoveries completed. */
size_t hh_sh_tick(hh_self_healing_t *sh, hh_time_ms_t now);

bool   hh_sh_is_recovering(const hh_self_healing_t *sh, hh_node_id_t target);
size_t hh_sh_active_count(const hh_self_healing_t *sh);

#endif /* HHSDR_MANET_SELF_HEALING_H */
