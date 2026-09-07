/*
 * Packet Forwarder — Data Plane (Doc 1 §3, §10; HTI-08/09 destination).
 *
 * Owns route-table lookup and per-packet transmission. It "never decides which
 * route is correct, only reads the table Routing Engine publishes" (Doc 1 §3).
 *
 * SCA: deliberately OUTSIDE the SCA Resource graph (Doc 1 §14) — the descriptor
 * -driven deployment model governs component lifecycle and interconnection, not
 * sub-microsecond forwarding-path timing.
 *
 * Fast-path rules this implementation obeys:
 *  - Reads exactly one thing from the control plane: the atomically-swapped
 *    route snapshot pointer. No lock, no blocking, no IPC round trip.
 *  - Performs NO routing computation. On a route miss it buffers briefly or
 *    drops, and lets the control plane resolve the route asynchronously — the
 *    pattern Doc 1 §10 specifies.
 *  - Emits no telemetry synchronously: counters are plain increments read by
 *    the management plane out of band.
 */
#ifndef HHSDR_DATAPLANE_FORWARDER_H
#define HHSDR_DATAPLANE_FORWARDER_H

#include "hhsdr/core/config.h"
#include "hhsdr/manet/route_table.h"
#include "hhsdr/radio/radio.h"

#define HH_FWD_QUEUE_MAX 64

typedef struct {
    hh_frame_t   frame;
    hh_time_ms_t queued_at;
    bool         used;
} hh_pending_packet_t;

typedef struct {
    const hh_config_t          *cfg;
    hh_radio_t                 *radio;
    const hh_route_publisher_t *routes;   /* read-only snapshot source */

    /* Bounded pending queue for route misses. Doc 1 §10: "a route miss is
     * buffered briefly or dropped while control plane resolves it
     * asynchronously". */
    hh_pending_packet_t pending[HH_FWD_QUEUE_MAX];
    size_t              pending_count;

    /* Counters. Plain increments; never a synchronous telemetry call. */
    uint64_t forwarded;
    uint64_t delivered_local;
    uint64_t dropped_no_route;
    uint64_t dropped_ttl;
    uint64_t dropped_queue_full;
    uint64_t dropped_tx_error;
    uint64_t queued;
    uint64_t requeued_ok;
    uint32_t last_snapshot_version;
} hh_forwarder_t;

hh_status_t hh_forwarder_init(hh_forwarder_t *fw, const hh_config_t *cfg,
                              hh_radio_t *radio, const hh_route_publisher_t *routes);

/*
 * Forward one packet toward dst.
 *  HH_OK            transmitted, or delivered locally when dst is this node
 *  HH_ERR_AGAIN     no route: buffered pending control-plane resolution
 *  HH_ERR_NOMEM     no route and the pending queue is full: dropped
 *  HH_ERR_INVAL     TTL exhausted or malformed
 *  HH_ERR_IO        radio transmit failed
 */
hh_status_t hh_forwarder_send(hh_forwarder_t *fw, hh_node_id_t dst,
                              const uint8_t *payload, uint16_t len,
                              uint8_t ttl, hh_time_ms_t now);

/* Forward a frame that arrived from the radio and is not addressed to us. */
hh_status_t hh_forwarder_forward(hh_forwarder_t *fw, const hh_frame_t *f,
                                 hh_time_ms_t now);

/* Retry buffered packets against the current snapshot, and drop those whose
 * wait exceeded max_wait_ms. Returns how many were sent. */
size_t hh_forwarder_flush(hh_forwarder_t *fw, hh_time_ms_t now, uint32_t max_wait_ms);

size_t hh_forwarder_pending(const hh_forwarder_t *fw);

#endif /* HHSDR_DATAPLANE_FORWARDER_H */
