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
 * The OpenCPI keys name an application file and property values supplied by
 * the operator; radiod itself names no worker or property. Settings for the
 * PL, hopsets and TLV are deliberately absent — those contracts do not exist
 * yet (see unknown.md), and inventing configuration for them would be
 * inventing the contract.
 */
#ifndef HHSDR_RADIOD_CONFIG_H
#define HHSDR_RADIOD_CONFIG_H

#include "hhsdr/core/log.h"
#include "hhsdr/core/types.h"
#include "hhsdr/radiod/ocpi_backend.h"

/* Longest socket path radiod will accept. sockaddr_un.sun_path is 108 bytes on
 * Linux; bind() is what ultimately enforces the limit, and this bound simply
 * lets the field be stored by value. */
#define HH_RADIOD_MAX_PATH 108

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

    /* Backend selection. Defaults to the mock, which is what radiod ran before
     * a backend could be chosen. */
    hh_radiod_backend_kind_t backend;

    /* Used only when backend == HH_RADIOD_BACKEND_OCPI. Keys: ocpi_app,
     * ocpi_library_path, and ocpi_property (repeatable). */
    hh_ocpi_config_t ocpi;
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
