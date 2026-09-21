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
}

hh_status_t hh_radiod_config_validate(const hh_radiod_config_t *cfg)
{
    if (!cfg) return HH_ERR_INVAL;
    if (cfg->sock_path[0] == '\0') return HH_ERR_INVAL;
    /* A zero tick would spin the control loop without ever yielding. */
    if (cfg->tick_interval_ms == 0) return HH_ERR_INVAL;
    /* An idle timeout shorter than a tick could expire a client before it is
     * ever serviced. Zero is valid and means "no timeout". */
    if (cfg->client_idle_timeout_ms != 0 &&
        cfg->client_idle_timeout_ms < cfg->tick_interval_ms)
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
    if (!strcmp(key, "log_level")) {
        hh_log_level_t lvl;
        if (!hh_log_level_parse(value, &lvl)) return HH_ERR_INVAL;
        cfg->log_level = lvl;
        return HH_OK;
    }
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
