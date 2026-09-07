#include "hhsdr/core/clock.h"
#include <time.h>

static hh_time_ms_t monotonic_now(void *ctx)
{
    struct timespec ts;
    (void)ctx;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (hh_time_ms_t)ts.tv_sec * 1000u + (hh_time_ms_t)(ts.tv_nsec / 1000000);
}

static const hh_clock_t g_monotonic = { monotonic_now, NULL };

const hh_clock_t *hh_clock_monotonic(void) { return &g_monotonic; }
