#include "hhsdr/core/log.h"
#include <stdarg.h>
#include <string.h>

static hh_log_level_t g_level = HH_LOG_INFO;
static hh_log_sink_fn g_sink = NULL;
static void *g_sink_ctx = NULL;

void hh_log_set_level(hh_log_level_t lvl) { g_level = lvl; }
hh_log_level_t hh_log_level(void) { return g_level; }

void hh_log_set_sink(hh_log_sink_fn fn, void *ctx) { g_sink = fn; g_sink_ctx = ctx; }

void hh_log_reset(void) { g_sink = NULL; g_sink_ctx = NULL; g_level = HH_LOG_INFO; }

const char *hh_log_level_str(hh_log_level_t lvl)
{
    switch (lvl) {
    case HH_LOG_ERROR: return "error";
    case HH_LOG_WARN:  return "warn";
    case HH_LOG_INFO:  return "info";
    case HH_LOG_DEBUG: return "debug";
    case HH_LOG_TRACE: return "trace";
    }
    return "info";
}

bool hh_log_level_parse(const char *s, hh_log_level_t *out)
{
    if (!s || !out) return false;
    if (!strcmp(s, "error")) { *out = HH_LOG_ERROR; return true; }
    if (!strcmp(s, "warn"))  { *out = HH_LOG_WARN;  return true; }
    if (!strcmp(s, "info"))  { *out = HH_LOG_INFO;  return true; }
    if (!strcmp(s, "debug")) { *out = HH_LOG_DEBUG; return true; }
    if (!strcmp(s, "trace")) { *out = HH_LOG_TRACE; return true; }
    return false;
}

void hh_log_emit(hh_log_level_t lvl, const char *component, const char *event,
                 const char *fmt, ...)
{
    char line[512];
    int n = snprintf(line, sizeof line, "lvl=%s comp=%s event=%s ",
                     hh_log_level_str(lvl), component, event);
    if (n < 0) return;
    if ((size_t)n < sizeof line) {
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(line + n, sizeof line - (size_t)n, fmt, ap);
        va_end(ap);
    }
    if (g_sink) {
        g_sink(lvl, line, g_sink_ctx);
    } else {
        fprintf(stderr, "%s\n", line);
    }
}
