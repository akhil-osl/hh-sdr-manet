/*
 * radiod configuration.
 *
 * Deliberately separate from hh_config_t (include/hhsdr/core/config.h), which
 * is the MANET node's configuration: radiod is a different process with a
 * different job, and the two have no keys in common. Sharing one config struct
 * would mean each daemon carrying — and validating — the other's settings.
 *
 * The file format is the project's existing `key = value` convention, with
 * `#` and `;` beginning comments (see config/node.example.conf), parsed the
 * same way so operators meet one syntax across the system.
 *
 * SCOPE: every key here corresponds to something radiod actually does today.
 *
 * The OpenCPI keys below were added when a real OpenCPI contract arrived: the
 * RADHAWK function-call API (api/radhawk.h in the OpenCPI project) and the
 * rh_agent helper built on it. They are deliberately only the four things
 * radiod itself needs -- which backend to run, where the agent binary is, and
 * the two bounds on how long a backend call may block the control loop.
 *
 * There is still NO per-worker or per-property configuration here, and there
 * must not be. Worker properties are set through the control protocol at run
 * time (`opencpi set ...`), and their names, types and ranges belong to the
 * backend, not to radiod's configuration file. Hopset and TLV settings remain
 * absent because those contracts still do not exist (unknown.md U-01, U-14).
 *
 * Upstream also added an in-process OpenCPI backend (`backend = ocpi`, keys
 * ocpi_app / ocpi_library_path / ocpi_property). It is independent of the
 * rh_agent backend; radiod refuses a config that selects both.
 */
#ifndef HHSDR_RADIOD_CONFIG_H
#define HHSDR_RADIOD_CONFIG_H

#include "hhsdr/core/log.h"
#include "hhsdr/core/types.h"
#include "hhsdr/radiod/ocpi_backend.h"
#include "hhsdr/ring/shm.h"

/* Longest socket path radiod will accept. sockaddr_un.sun_path is 108 bytes on
 * Linux; bind() is what ultimately enforces the limit, and this bound simply
 * lets the field be stored by value. */
#define HH_RADIOD_MAX_PATH 108

/* Longest path to an executable radiod will accept. Deliberately NOT
 * HH_RADIOD_MAX_PATH: that bound exists because sockaddr_un.sun_path is 108
 * bytes, and an executable path has no such limit. Sharing the socket bound
 * rejected a perfectly ordinary agent path out of a development tree. */
#define HH_RADIOD_MAX_EXEC_PATH 512
/* Which hh_radio_ops_t implementation radiod owns. */
typedef enum {
    HH_RADIOD_BACKEND_MOCK = 0,   /* control-plane validation, no hardware */
    HH_RADIOD_BACKEND_OCPI        /* an OpenCPI application, via the ACI    */
} hh_radiod_backend_kind_t;

const char *hh_radiod_backend_str(hh_radiod_backend_kind_t k);

typedef struct {
    /* Control socket to bind. Overridable on the command line. */
    char sock_path[HH_RADIOD_MAX_PATH];

    /* Control-loop period. radiod waits for socket activity up to this long,
     * then services the backend, so this bounds how often a backend with no
     * client traffic is polled. */
    uint32_t tick_interval_ms;

    /* Drop a connected client that has sent nothing for this long. Zero
     * disables the timeout, which is the historical behaviour: before this
     * setting existed, an idle client held its slot indefinitely and 16 such
     * clients would exhaust the table. */
    uint32_t client_idle_timeout_ms;

    hh_log_level_t log_level;

    /* Which backend to run: "mock" (the default, and what every existing
     * deployment gets) or "opencpi". An unknown name is rejected at load
     * rather than quietly falling back, because a typo that silently produced
     * a mock radio would be indistinguishable from a real one that is idle. */
    char     radio_backend[16];

    /* Path to the rh_agent helper. Only consulted when radio_backend is
     * "opencpi". Empty means the compiled-in default. */
    char     opencpi_agent[HH_RADIOD_MAX_EXEC_PATH];

    /* Bounds on a synchronous agent call. Every OpenCPI operation blocks
     * radiod's control loop for up to one of these, so they are the ceiling on
     * how long a wedged agent can hold up the control plane.
     *
     * The start bound is far larger because `start` may run ad9361_init, which
     * reprogrammes the AD9361 and is the slowest operation in the system. */
    uint32_t opencpi_reply_timeout_ms;
    uint32_t opencpi_start_timeout_ms;

    /* Backend selection. Defaults to the mock, which is what radiod ran before
     * a backend could be chosen. */
    hh_radiod_backend_kind_t backend;

    /* Used only when backend == HH_RADIOD_BACKEND_OCPI. Keys: ocpi_app,
     * ocpi_library_path, and ocpi_property (repeatable). */
    hh_ocpi_config_t ocpi;

    /* Frame rings. radiod creates the shared-memory file at ring_path and lays
     * out the rings in it. An empty path switches the rings off. Keys:
     * ring_path, ring_rx_slots, ring_tx_slots, ring_slot_size. The sizes are
     * placeholders until the PL fixes them. */
    char     ring_path[HH_RING_SHM_PATH_MAX];
    uint32_t ring_rx_slots;
    uint32_t ring_tx_slots;
    uint32_t ring_slot_size;
} hh_radiod_config_t;

/* Populate with the documented defaults. Never fails. */
void hh_radiod_config_defaults(hh_radiod_config_t *cfg);

/* Validate invariants. Returns HH_ERR_INVAL on a nonsensical combination. */
hh_status_t hh_radiod_config_validate(const hh_radiod_config_t *cfg);

/* Apply one `key = value` pair. HH_ERR_NOTFOUND for an unknown key and
 * HH_ERR_INVAL for a malformed value, so callers can report which line is at
 * fault. */
hh_status_t hh_radiod_config_set(hh_radiod_config_t *cfg, const char *key,
                                 const char *value);

/* Load a `key = value` file. On a bad line, reports the 1-based line number
 * through *err_line when non-NULL. */
hh_status_t hh_radiod_config_load_file(hh_radiod_config_t *cfg, const char *path,
                                       int *err_line);

#endif /* HHSDR_RADIOD_CONFIG_H */
