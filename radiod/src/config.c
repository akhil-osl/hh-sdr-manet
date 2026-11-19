#include "hhsdr/radiod/config.h"
#include "hhsdr/protocol/rc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Defaults preserve the behaviour radiod had before it took a config file:
 * the same socket path, and the same 10 ms control-loop period. */
#define HH_RADIOD_DEFAULT_TICK_MS 10u

void hh_radiod_config_defaults(hh_radiod_config_t *cfg)
{
    if (!cfg) return;
    memset(cfg, 0, sizeof *cfg);
    snprintf(cfg->sock_path, sizeof cfg->sock_path, "%s", HH_RC_DEFAULT_SOCK_PATH);
    cfg->tick_interval_ms      = HH_RADIOD_DEFAULT_TICK_MS;
    cfg->client_idle_timeout_ms = 0;   /* disabled: prior behaviour */
    cfg->log_level             = HH_LOG_INFO;

    /* The mock backend stays the default. Every existing deployment and every
     * existing test therefore behaves exactly as before this file learned
     * about OpenCPI; a real radio is something an operator opts into. */
    snprintf(cfg->radio_backend, sizeof cfg->radio_backend, "mock");
    cfg->opencpi_agent[0]        = '\0';
    /* 10 s, not 3: `stop` on a proxy session mutes the TX, waits for the
     * proxy to confirm (<= 1.4 s), then stops two OpenCPI applications, and a
     * timeout kills the agent part-way through -- leaving the PL running. */
    cfg->opencpi_reply_timeout_ms = 10000u;
    cfg->opencpi_start_timeout_ms = 60000u;
    cfg->ring_rx_slots         = 64;     /* rings off until ring_path is set */
    cfg->ring_tx_slots         = 64;
    cfg->ring_slot_size        = 288;    /* 280 byte frame rounded to 32 */
}

const char *hh_radiod_backend_str(hh_radiod_backend_kind_t k)
{
    switch (k) {
    case HH_RADIOD_BACKEND_MOCK: return "mock";
    case HH_RADIOD_BACKEND_OCPI: return "ocpi";
    default:                     return "unknown";
    }
}

hh_status_t hh_radiod_config_validate(const hh_radiod_config_t *cfg)
{
    if (!cfg) return HH_ERR_INVAL;
    if (cfg->sock_path[0] == '\0') return HH_ERR_INVAL;
    /* The OpenCPI backend has nothing to open without an application. */
    if (cfg->backend == HH_RADIOD_BACKEND_OCPI && cfg->ocpi.app_path[0] == '\0')
        return HH_ERR_INVAL;
    /* A zero tick would spin the control loop without ever yielding. */
    if (cfg->tick_interval_ms == 0) return HH_ERR_INVAL;
    /* With the rings on, the geometry must be one the ring layout accepts. */
    if (cfg->ring_path[0] != '\0' &&
        hh_ring_region_size(cfg->ring_rx_slots, cfg->ring_tx_slots, cfg->ring_slot_size) == 0)
        return HH_ERR_INVAL;
    /* An idle timeout shorter than a tick could expire a client before it is
     * ever serviced. Zero is valid and means "no timeout". */
    if (cfg->client_idle_timeout_ms != 0 &&
        cfg->client_idle_timeout_ms < cfg->tick_interval_ms)
        return HH_ERR_INVAL;
    if (strcmp(cfg->radio_backend, "mock") != 0 &&
        strcmp(cfg->radio_backend, "opencpi") != 0)
        return HH_ERR_INVAL;
    /* A zero timeout would make every agent call fail instantly, which reads
     * as "OpenCPI is broken" rather than as a misconfiguration. */
    if (cfg->opencpi_reply_timeout_ms == 0 || cfg->opencpi_start_timeout_ms == 0)
        return HH_ERR_INVAL;
    return HH_OK;
}

static hh_status_t parse_u32(const char *v, uint32_t *out)
{
    char *end;
    unsigned long n;
    if (!v || *v == '\0') return HH_ERR_INVAL;
    n = strtoul(v, &end, 10);
    if (*end != '\0' || n > 0xFFFFFFFFul) return HH_ERR_INVAL;
    *out = (uint32_t)n;
    return HH_OK;
}

/* Refuse rather than truncate: a shortened path names a different file. */
static hh_status_t copy_path(char *dst, size_t cap, const char *v)
{
    if (*v == '\0' || strlen(v) >= cap) return HH_ERR_INVAL;
    snprintf(dst, cap, "%s", v);
    return HH_OK;
}

hh_status_t hh_radiod_config_set(hh_radiod_config_t *cfg, const char *key,
                                 const char *value)
{
    if (!cfg || !key || !value) return HH_ERR_INVAL;

    if (!strcmp(key, "sock_path")) {
        if (*value == '\0') return HH_ERR_INVAL;
        if (strlen(value) >= sizeof cfg->sock_path) return HH_ERR_INVAL;
        snprintf(cfg->sock_path, sizeof cfg->sock_path, "%s", value);
        return HH_OK;
    }
    if (!strcmp(key, "tick_interval_ms"))
        return parse_u32(value, &cfg->tick_interval_ms);
    if (!strcmp(key, "client_idle_timeout_ms"))
        return parse_u32(value, &cfg->client_idle_timeout_ms);
    if (!strcmp(key, "radio_backend")) {
        if (strcmp(value, "mock") != 0 && strcmp(value, "opencpi") != 0)
            return HH_ERR_INVAL;
        snprintf(cfg->radio_backend, sizeof cfg->radio_backend, "%s", value);
        return HH_OK;
    }
    if (!strcmp(key, "opencpi_agent")) {
        if (strlen(value) >= sizeof cfg->opencpi_agent) return HH_ERR_INVAL;
        snprintf(cfg->opencpi_agent, sizeof cfg->opencpi_agent, "%s", value);
        return HH_OK;
    }
    if (!strcmp(key, "opencpi_reply_timeout_ms"))
        return parse_u32(value, &cfg->opencpi_reply_timeout_ms);
    if (!strcmp(key, "opencpi_start_timeout_ms"))
        return parse_u32(value, &cfg->opencpi_start_timeout_ms);
    if (!strcmp(key, "ring_path"))
        return copy_path(cfg->ring_path, sizeof cfg->ring_path, value);
    if (!strcmp(key, "ring_rx_slots"))
        return parse_u32(value, &cfg->ring_rx_slots);
    if (!strcmp(key, "ring_tx_slots"))
        return parse_u32(value, &cfg->ring_tx_slots);
    if (!strcmp(key, "ring_slot_size"))
        return parse_u32(value, &cfg->ring_slot_size);
    if (!strcmp(key, "log_level")) {
        hh_log_level_t lvl;
        if (!hh_log_level_parse(value, &lvl)) return HH_ERR_INVAL;
        cfg->log_level = lvl;
        return HH_OK;
    }
    if (!strcmp(key, "backend")) {
        if (!strcmp(value, "mock"))      cfg->backend = HH_RADIOD_BACKEND_MOCK;
        else if (!strcmp(value, "ocpi")) cfg->backend = HH_RADIOD_BACKEND_OCPI;
        else return HH_ERR_INVAL;
        return HH_OK;
    }
    if (!strcmp(key, "ocpi_app"))
        return copy_path(cfg->ocpi.app_path, sizeof cfg->ocpi.app_path, value);
    if (!strcmp(key, "ocpi_library_path"))
        return copy_path(cfg->ocpi.library_path, sizeof cfg->ocpi.library_path, value);
    /* Repeatable: each occurrence adds one property write, in file order. */
    if (!strcmp(key, "ocpi_property"))
        return hh_ocpi_config_add_property(&cfg->ocpi, value);
    return HH_ERR_NOTFOUND;
}

/* Trim leading and trailing whitespace in place, matching core/config.c. */
static char *trim(char *s)
{
    char *end;
    while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n') s++;
    if (*s == '\0') return s;
    end = s + strlen(s) - 1;
    while (end > s && (*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n'))
        *end-- = '\0';
    return s;
}

hh_status_t hh_radiod_config_load_file(hh_radiod_config_t *cfg, const char *path,
                                       int *err_line)
{
    FILE *f;
    char buf[512];
    int lineno = 0;

    if (!cfg || !path) return HH_ERR_INVAL;
    f = fopen(path, "r");
    if (!f) return HH_ERR_IO;

    while (fgets(buf, sizeof buf, f)) {
        char *line, *eq, *key, *val;
        lineno++;
        line = buf;
        for (char *p = line; *p; p++) {
            if (*p == '#' || *p == ';') { *p = '\0'; break; }
        }
        line = trim(line);
        if (*line == '\0') continue;

        eq = strchr(line, '=');
        if (!eq) { if (err_line) *err_line = lineno; fclose(f); return HH_ERR_INVAL; }
        *eq = '\0';
        key = trim(line);
        val = trim(eq + 1);
        if (hh_radiod_config_set(cfg, key, val) != HH_OK) {
            if (err_line) *err_line = lineno;
            fclose(f);
            return HH_ERR_INVAL;
        }
    }
    fclose(f);
    return HH_OK;
}
