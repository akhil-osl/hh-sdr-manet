#include "hhsdr/core/config.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>

void hh_config_defaults(hh_config_t *cfg)
{
    if (!cfg) return;
    memset(cfg, 0, sizeof *cfg);

    cfg->node_id          = HH_NODE_ID_INVALID;
    cfg->capabilities     = HH_CAP_ROUTING_CAPABLE;
    cfg->routing_capable  = true;

    cfg->beacon_interval_acq_ms = 200;
    cfg->beacon_interval_ms     = 1000;
    cfg->beacon_interval_min_ms = 200;
    cfg->beacon_interval_max_ms = 4000;
    cfg->acquisition_timeout_ms = 3000;

    cfg->neighbor_allowed_loss = 3;
    cfg->max_neighbors         = 64;

    cfg->lh_degrade_threshold  = 0.60f;
    cfg->lh_recover_threshold  = 0.80f;   /* hysteresis: exit stricter than entry */
    cfg->lh_ewma_shift         = 2;
    cfg->lh_suspect_hold_ms    = 2000;
    cfg->lh_recover_hold_ms    = 3000;
    cfg->lh_min_signals_suspect = 2;      /* : ">= 2 independent signals" */

    cfg->route_active_timeout_ms  = 6000;
    cfg->route_delete_period_ms   = 6000;
    cfg->route_update_interval_ms = 1000;
    cfg->max_routes    = 256;
    cfg->max_hop_count = 16;
    cfg->metric_w_quality = 1.0f;
    cfg->metric_w_hop     = 0.15f;
    cfg->metric_w_age     = 0.05f;

    cfg->hold_down_ms              = 2000;
    cfg->merge_hold_down_ms        = 4000;
    cfg->dampening_flap_threshold  = 3;
    cfg->dampening_window_ms       = 20000;
    cfg->dampening_penalty_ms      = 10000;
    cfg->rediscovery_backoff_ms    = 500;
    cfg->rediscovery_max_retries   = 3;

    cfg->forward_queue_depth = 32;

    cfg->log_level = HH_LOG_INFO;
    snprintf(cfg->radio_adapter, sizeof cfg->radio_adapter, "hw");
}

hh_status_t hh_config_validate(const hh_config_t *cfg)
{
    if (!cfg) return HH_ERR_INVAL;
    if (cfg->node_id == HH_NODE_ID_INVALID) return HH_ERR_INVAL;
    if (cfg->beacon_interval_ms == 0 || cfg->beacon_interval_acq_ms == 0) return HH_ERR_INVAL;
    if (cfg->beacon_interval_min_ms > cfg->beacon_interval_max_ms) return HH_ERR_INVAL;
    if (cfg->beacon_interval_ms < cfg->beacon_interval_min_ms ||
        cfg->beacon_interval_ms > cfg->beacon_interval_max_ms) return HH_ERR_INVAL;
    if (cfg->neighbor_allowed_loss == 0) return HH_ERR_INVAL;
    if (cfg->max_neighbors == 0 || cfg->max_routes == 0) return HH_ERR_INVAL;
    /* Hysteresis is the point of these two thresholds: recover must be strictly
     * stricter than degrade, else a link can oscillate on noise. */
    if (!(cfg->lh_recover_threshold > cfg->lh_degrade_threshold)) return HH_ERR_INVAL;
    if (cfg->lh_degrade_threshold < 0.0f || cfg->lh_recover_threshold > 1.0f) return HH_ERR_INVAL;
    if (cfg->lh_min_signals_suspect == 0) return HH_ERR_INVAL;
    if (cfg->max_hop_count == 0) return HH_ERR_INVAL;
    if (cfg->forward_queue_depth == 0) return HH_ERR_INVAL;
    return HH_OK;
}

static bool parse_u32(const char *v, uint32_t *out)
{
    char *end = NULL;
    unsigned long long x;
    errno = 0;
    x = strtoull(v, &end, 10);
    if (errno || end == v || *end != '\0' || x > 0xFFFFFFFFull) return false;
    *out = (uint32_t)x;
    return true;
}

static bool parse_f32(const char *v, float *out)
{
    char *end = NULL;
    double x;
    errno = 0;
    x = strtod(v, &end);
    if (errno || end == v || *end != '\0') return false;
    *out = (float)x;
    return true;
}

static bool parse_bool(const char *v, bool *out)
{
    if (!strcmp(v, "true")  || !strcmp(v, "1") || !strcmp(v, "yes")) { *out = true;  return true; }
    if (!strcmp(v, "false") || !strcmp(v, "0") || !strcmp(v, "no"))  { *out = false; return true; }
    return false;
}

hh_status_t hh_config_set(hh_config_t *cfg, const char *key, const char *value)
{
    if (!cfg || !key || !value) return HH_ERR_INVAL;

#define U32(name, field) \
    if (!strcmp(key, name)) return parse_u32(value, &cfg->field) ? HH_OK : HH_ERR_INVAL;
#define F32(name, field) \
    if (!strcmp(key, name)) return parse_f32(value, &cfg->field) ? HH_OK : HH_ERR_INVAL;

    U32("node_id",                     node_id)
    U32("capabilities",                capabilities)
    U32("beacon_interval_acq_ms",      beacon_interval_acq_ms)
    U32("beacon_interval_ms",          beacon_interval_ms)
    U32("beacon_interval_min_ms",      beacon_interval_min_ms)
    U32("beacon_interval_max_ms",      beacon_interval_max_ms)
    U32("acquisition_timeout_ms",      acquisition_timeout_ms)
    U32("neighbor_allowed_loss",       neighbor_allowed_loss)
    U32("max_neighbors",               max_neighbors)
    U32("lh_ewma_shift",               lh_ewma_shift)
    U32("lh_suspect_hold_ms",          lh_suspect_hold_ms)
    U32("lh_recover_hold_ms",          lh_recover_hold_ms)
    U32("lh_min_signals_suspect",      lh_min_signals_suspect)
    U32("route_active_timeout_ms",     route_active_timeout_ms)
    U32("route_delete_period_ms",      route_delete_period_ms)
    U32("route_update_interval_ms",    route_update_interval_ms)
    U32("max_routes",                  max_routes)
    U32("hold_down_ms",                hold_down_ms)
    U32("merge_hold_down_ms",          merge_hold_down_ms)
    U32("dampening_flap_threshold",    dampening_flap_threshold)
    U32("dampening_window_ms",         dampening_window_ms)
    U32("dampening_penalty_ms",        dampening_penalty_ms)
    U32("rediscovery_backoff_ms",      rediscovery_backoff_ms)
    U32("rediscovery_max_retries",     rediscovery_max_retries)
    U32("forward_queue_depth",         forward_queue_depth)

    F32("lh_degrade_threshold",        lh_degrade_threshold)
    F32("lh_recover_threshold",        lh_recover_threshold)
    F32("metric_w_quality",            metric_w_quality)
    F32("metric_w_hop",                metric_w_hop)
    F32("metric_w_age",                metric_w_age)

#undef U32
#undef F32

    if (!strcmp(key, "max_hop_count")) {
        uint32_t v;
        if (!parse_u32(value, &v) || v == 0 || v > 255) return HH_ERR_INVAL;
        cfg->max_hop_count = (uint8_t)v;
        return HH_OK;
    }
    if (!strcmp(key, "routing_capable"))
        return parse_bool(value, &cfg->routing_capable) ? HH_OK : HH_ERR_INVAL;
    if (!strcmp(key, "log_level"))
        return hh_log_level_parse(value, &cfg->log_level) ? HH_OK : HH_ERR_INVAL;
    if (!strcmp(key, "radio_adapter")) {
        if (strlen(value) >= sizeof cfg->radio_adapter) return HH_ERR_INVAL;
        snprintf(cfg->radio_adapter, sizeof cfg->radio_adapter, "%s", value);
        return HH_OK;
    }
    return HH_ERR_NOTFOUND;
}

static char *trim(char *s)
{
    char *end;
    while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n') s++;
    if (*s == '\0') return s;
    end = s + strlen(s) - 1;
    while (end > s && (*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n')) *end-- = '\0';
    return s;
}

hh_status_t hh_config_load_file(hh_config_t *cfg, const char *path, int *err_line)
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
        /* strip comments */
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
        if (hh_config_set(cfg, key, val) != HH_OK) {
            if (err_line) *err_line = lineno;
            fclose(f);
            return HH_ERR_INVAL;
        }
    }
    fclose(f);
    return HH_OK;
}
