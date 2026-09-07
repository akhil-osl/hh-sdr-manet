/*
 * Virtual clock — TEST INFRASTRUCTURE ONLY.
 *
 * Binds hh_clock_t to a test-controlled counter so every scenario advances time
 * explicitly. No test sleeps, and a scenario replays identically every run,
 * which is what makes the failure/recovery timing assertions meaningful.
 */
#ifndef HH_VCLOCK_H
#define HH_VCLOCK_H

#include "hhsdr/core/clock.h"

typedef struct { hh_time_ms_t now; hh_clock_t clock; } vclock_t;

static hh_time_ms_t vclock_now_fn(void *ctx) { return ((vclock_t *)ctx)->now; }

static inline void vclock_init(vclock_t *v, hh_time_ms_t start)
{
    v->now = start;
    v->clock.now_ms = vclock_now_fn;
    v->clock.ctx = v;
}

static inline void vclock_advance(vclock_t *v, hh_time_ms_t ms) { v->now += ms; }

#endif /* HH_VCLOCK_H */
