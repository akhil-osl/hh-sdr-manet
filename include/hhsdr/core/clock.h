/*
 * Clock abstraction.
 *
 * Doc 1 §11 requires timers that "fire exactly when due via timerfd" rather than
 * a poll loop. Production binds this to CLOCK_MONOTONIC; tests bind it to a
 * virtual clock so scenario timing is deterministic and reproducible without
 * sleeping. No production component reads wall-clock time directly.
 */
#ifndef HHSDR_CORE_CLOCK_H
#define HHSDR_CORE_CLOCK_H

#include "hhsdr/core/types.h"

typedef struct hh_clock {
    hh_time_ms_t (*now_ms)(void *ctx);
    void *ctx;
} hh_clock_t;

static inline hh_time_ms_t hh_now(const hh_clock_t *c) { return c->now_ms(c->ctx); }

/* Production clock: CLOCK_MONOTONIC. */
const hh_clock_t *hh_clock_monotonic(void);

#endif /* HHSDR_CORE_CLOCK_H */
