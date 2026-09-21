/*
 * radiod — standalone radio-control daemon.
 *
 * Owns one hh_radio_t backend and exposes it to out-of-process clients over
 * the rc.h control contract (HTI-02 status, HTI-14 channel control, HTI-15
 * config/telemetry). radiod itself carries no MANET/topology semantics,
 * mirroring the architecture's "Radio/SDR Interface... carries no topology
 * semantics" boundary.
 *
 * Single-threaded, poll-driven, matching the project's existing control-loop
 * style (src/main.c): each tick accepts new connections, services one
 * request per ready client, and polls the backend. No locking, no threads.
 */
#ifndef HHSDR_RADIOD_H
#define HHSDR_RADIOD_H

#include "hhsdr/core/clock.h"
#include "hhsdr/radio/radio.h"
#include "hhsdr/radio/rc.h"

#define HH_RADIOD_MAX_CLIENTS 16

typedef struct {
    int fd;
    char inbuf[HH_RC_MAX_LINE];
    size_t inlen;
} hh_radiod_client_t;

/* Backend-specific fault injection hook. radiod's state machine and IPC
 * handling stay backend-agnostic (they only know hh_radio_ops_t); the
 * concrete backend (e.g. hh_mock_backend_t) is wired in by whoever
 * constructs radiod, via this single indirection point. */
typedef void (*hh_radiod_fault_fn)(void *ctx, hh_rc_fault_t fault);

typedef struct {
    hh_radio_t        *radio;
    const hh_clock_t  *clock;
    hh_rc_state_t      state;
    hh_node_id_t       node_id;

    hh_radiod_fault_fn fault_fn;
    void              *fault_ctx;

    int                listen_fd;
    hh_radiod_client_t clients[HH_RADIOD_MAX_CLIENTS];

    uint64_t requests_total;
    uint64_t requests_rejected;
} hh_radiod_t;

/* Construct radiod bound to `radio` (not yet opened) and `clock`. */
hh_status_t hh_radiod_init(hh_radiod_t *d, hh_radio_t *radio, const hh_clock_t *clock);

/* Wire a backend-specific fault-injection hook (optional; INJECT_FAULT and
 * CLEAR_FAULT still update radiod's own state machine when this is unset,
 * but the backend itself will not reflect the fault). */
void hh_radiod_set_fault_hook(hh_radiod_t *d, hh_radiod_fault_fn fn, void *ctx);

/* Bind and listen on a UNIX domain socket at `sock_path`. Must be called
 * once, after hh_radiod_init, before the poll loop starts. */
hh_status_t hh_radiod_listen(hh_radiod_t *d, const char *sock_path);

/* One control-loop iteration: accept new clients, service ready ones,
 * poll the backend. Returns HH_ERR_STATE only if called after RELEASED. */
hh_status_t hh_radiod_tick(hh_radiod_t *d, hh_time_ms_t now);

/* Release all resources (backend, listen socket, client sockets). */
void hh_radiod_release(hh_radiod_t *d);

/* Apply one already-parsed request directly (used by tests and by the
 * socket-servicing path alike, so state-machine behavior has one
 * implementation regardless of transport). */
void hh_radiod_handle_request(hh_radiod_t *d, const hh_rc_request_t *req,
                              hh_rc_response_t *resp);

/* True once SHUTDOWN has been accepted and the daemon should exit its
 * process loop after finishing the current tick. */
bool hh_radiod_shutdown_requested(const hh_radiod_t *d);

#endif /* HHSDR_RADIOD_H */
