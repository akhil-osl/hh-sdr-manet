#include "hhsdr/radiod/events.h"
#include "hhsdr/core/log.h"
#include <string.h>

#define COMP "radiod.faults"

static bool kind_in_range(hh_rc_fault_t kind)
{
    return kind > HH_RC_FAULT_NONE && kind < HH_RADIOD_FAULT_SLOTS;
}

void hh_radiod_faults_init(hh_radiod_faults_t *f)
{
    if (!f) return;
    memset(f, 0, sizeof *f);
}

void hh_radiod_faults_assert(hh_radiod_faults_t *f, hh_rc_fault_t kind,
                             hh_time_ms_t now)
{
    hh_radiod_fault_record_t *r;

    if (!f || !kind_in_range(kind)) return;   /* NONE is not a fault */

    r = &f->faults[kind];
    r->count++;
    r->last_seen = now;

    if (r->active) return;   /* re-report of a standing fault: not a new event */

    r->active     = true;
    r->first_seen = now;
    f->generation++;
    f->asserted_total++;

    HH_LOGW(COMP, "asserted", "kind=%s count=%u", hh_rc_fault_str(kind), r->count);
}

void hh_radiod_faults_clear(hh_radiod_faults_t *f, hh_rc_fault_t kind,
                            hh_time_ms_t now)
{
    hh_radiod_fault_record_t *r;

    if (!f || !kind_in_range(kind)) return;

    r = &f->faults[kind];
    if (!r->active) return;   /* clearing an inactive fault is a no-op */

    r->active     = false;
    r->cleared_at = now;
    f->generation++;
    f->cleared_total++;

    HH_LOGI(COMP, "cleared", "kind=%s", hh_rc_fault_str(kind));
}

void hh_radiod_faults_clear_all(hh_radiod_faults_t *f, hh_time_ms_t now)
{
    if (!f) return;
    for (int k = HH_RC_FAULT_NONE + 1; k < HH_RADIOD_FAULT_SLOTS; k++)
        hh_radiod_faults_clear(f, (hh_rc_fault_t)k, now);
}

bool hh_radiod_faults_any_active(const hh_radiod_faults_t *f)
{
    if (!f) return false;
    for (int k = HH_RC_FAULT_NONE + 1; k < HH_RADIOD_FAULT_SLOTS; k++)
        if (f->faults[k].active) return true;
    return false;
}

uint32_t hh_radiod_faults_active_count(const hh_radiod_faults_t *f)
{
    uint32_t n = 0;
    if (!f) return 0;
    for (int k = HH_RC_FAULT_NONE + 1; k < HH_RADIOD_FAULT_SLOTS; k++)
        if (f->faults[k].active) n++;
    return n;
}

const hh_radiod_fault_record_t *hh_radiod_faults_get(const hh_radiod_faults_t *f,
                                                     hh_rc_fault_t kind)
{
    if (!f || !kind_in_range(kind)) return NULL;
    return &f->faults[kind];
}
