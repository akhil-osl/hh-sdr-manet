/*
 * Structured logging (Doc 1 §11 observability requirement).
 *
 * Emits one key=value record per line so both operators and automated tests can
 * consume it. Tests assert on these records rather than on internal state, which
 * keeps assertions tied to externally observable behavior.
 *
 * Logging is off the fast path: the forwarder logs at HH_LOG_TRACE only, and the
 * sink is a plain synchronous write with no lock the control plane can contend on.
 */
#ifndef HHSDR_CORE_LOG_H
#define HHSDR_CORE_LOG_H

#include "hhsdr/core/types.h"
#include <stdio.h>

typedef enum {
    HH_LOG_ERROR = 0,
    HH_LOG_WARN,
    HH_LOG_INFO,
    HH_LOG_DEBUG,
    HH_LOG_TRACE
} hh_log_level_t;

/* Sink receives a fully formatted record line (no trailing newline). */
typedef void (*hh_log_sink_fn)(hh_log_level_t lvl, const char *line, void *ctx);

void hh_log_set_level(hh_log_level_t lvl);
hh_log_level_t hh_log_level(void);
void hh_log_set_sink(hh_log_sink_fn fn, void *ctx);
void hh_log_reset(void);   /* restore default stderr sink, INFO level */

void hh_log_emit(hh_log_level_t lvl, const char *component, const char *event,
                 const char *fmt, ...);

#define HH_LOG(lvl, comp, ev, ...) \
    do { if ((lvl) <= hh_log_level()) hh_log_emit((lvl), (comp), (ev), __VA_ARGS__); } while (0)

#define HH_LOGE(comp, ev, ...) HH_LOG(HH_LOG_ERROR, comp, ev, __VA_ARGS__)
#define HH_LOGW(comp, ev, ...) HH_LOG(HH_LOG_WARN,  comp, ev, __VA_ARGS__)
#define HH_LOGI(comp, ev, ...) HH_LOG(HH_LOG_INFO,  comp, ev, __VA_ARGS__)
#define HH_LOGD(comp, ev, ...) HH_LOG(HH_LOG_DEBUG, comp, ev, __VA_ARGS__)
#define HH_LOGT(comp, ev, ...) HH_LOG(HH_LOG_TRACE, comp, ev, __VA_ARGS__)

const char *hh_log_level_str(hh_log_level_t lvl);
bool hh_log_level_parse(const char *s, hh_log_level_t *out);

#endif /* HHSDR_CORE_LOG_H */
