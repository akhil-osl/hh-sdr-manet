/*
 * radiod — standalone radio-control daemon.
 *
 * Owns one hh_radio_t backend and exposes it to out-of-process clients over
 * the rc.h control contract (HTI-02 status, HTI-14 channel control, HTI-15
 * config/telemetry). radiod itself carries no MANET/topology semantics,
 * mirroring the architecture's "Radio/SDR Interface... carries no topology
 * semantics" boundary.
 *
 * Single-threaded and poll-driven, matching the project's existing
 * control-loop style (src/main.c): each tick accepts new connections,
 * services ready clients, and polls the backend. No locking, no threads.
 *
 * The caller supplies the waiting: hh_radiod_pollfds() reports the
 * descriptors to watch, so the process blocks in poll() until something
 * actually happens instead of waking on a fixed timer.
 *
 * TARGET ARCHITECTURE: radiod is to be the single PL/OpenCPI owner — the only
 * process that opens the OpenCPI application, with every other process
 * reaching the radio through the control protocol. That ownership is NOT
 * implemented here: the application XML, worker names, properties and ACI
 * lifecycle are all unspecified (see unknown.md, U-03 and U-04). radiod owns
 * an hh_radio_ops_t backend, and a real backend will implement that same
 * vtable, so nothing in this file needs to change when one arrives.
 */
#ifndef HHSDR_RADIOD_H
#define HHSDR_RADIOD_H

#include "hhsdr/core/clock.h"
#include "hhsdr/protocol/rc.h"
#include "hhsdr/radio/radio.h"
#include "hhsdr/radiod/config.h"
#include "hhsdr/radiod/events.h"
#include <poll.h>

#define HH_RADIOD_MAX_CLIENTS 16

typedef struct {
    int fd;
    char inbuf[HH_RC_MAX_LINE];
    size_t inlen;

    /* Pending response bytes, for a client whose socket would not accept the
     * whole reply at once. Without this a short write silently truncated the
     * response and desynchronised that client's stream. */
    char outbuf[HH_RC_MAX_LINE];
    size_t outlen;
    size_t outsent;

    /* Last time this client sent anything, for the idle timeout. */
    hh_time_ms_t last_activity;
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
    char               sock_path[HH_RADIOD_MAX_PATH];
    hh_radiod_client_t clients[HH_RADIOD_MAX_CLIENTS];

    /* Idle-client timeout, copied from configuration. Zero disables it. */
    uint32_t           client_idle_timeout_ms;

    /* Fault registry: what is wrong, how often, and since when. */
    hh_radiod_faults_t faults;

    uint64_t requests_total;
    uint64_t requests_rejected;
} hh_radiod_t;

/* Construct radiod bound to `radio` (not yet opened) and `clock`. */
hh_status_t hh_radiod_init(hh_radiod_t *d, hh_radio_t *radio, const hh_clock_t *clock);

/* Wire a backend-specific fault-injection hook (optional; INJECT_FAULT and
 * CLEAR_FAULT still update radiod's own state machine when this is unset,
 * but the backend itself will not reflect the fault). */
void hh_radiod_set_fault_hook(hh_radiod_t *d, hh_radiod_fault_fn fn, void *ctx);

/* Apply configuration. Optional: without it the daemon keeps its defaults.
 * Must be called before hh_radiod_listen, since it carries the socket path. */
hh_status_t hh_radiod_configure(hh_radiod_t *d, const hh_radiod_config_t *cfg);

/* Bind and listen on a UNIX domain socket at `sock_path`. Must be called
 * once, after hh_radiod_init, before the poll loop starts.
 *
 * The path is remembered so that hh_radiod_release can unlink it: previously
 * a stale socket file was left behind on every exit, and recovery depended on
 * the *next* start unlinking it. */
hh_status_t hh_radiod_listen(hh_radiod_t *d, const char *sock_path);

/* Fill `fds` with the descriptors radiod wants watched, and return how many
 * were written. Lets the caller block in poll() rather than spinning on a
 * timer: previously the control loop woke every 10 ms regardless of traffic,
 * which both burned CPU when idle and added up to 10 ms to every request.
 *
 * `cap` must be at least HH_RADIOD_MAX_CLIENTS + 1. Returns 0 if it is not. */
size_t hh_radiod_pollfds(const hh_radiod_t *d, struct pollfd *fds, size_t cap);

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

/* Read-only view of the fault registry.
 *
 * Not yet exposed over the control protocol: reporting it would need either a
 * new response payload or an asynchronous event, and the wire format for
 * neither is specified (unknown.md, U-01 and U-05). The registry is
 * maintained and observable in-process so that the data exists and is tested
 * when a wire format does. */
const hh_radiod_faults_t *hh_radiod_faults(const hh_radiod_t *d);

#endif /* HHSDR_RADIOD_H */
