#include "hhsdr/manet/failure_detector.h"
#include "hhsdr/core/log.h"
#include <string.h>

#define COMP "failuredet"

hh_status_t hh_fd_init(hh_failure_detector_t *fd, const hh_config_t *cfg,
                       const hh_clock_t *clock, hh_dispatcher_t *bus)
{
    if (!fd || !cfg || !clock || !bus) return HH_ERR_INVAL;
    memset(fd, 0, sizeof *fd);
    fd->cfg = cfg;
    fd->clock = clock;
    fd->bus = bus;
    return HH_OK;
}

static hh_fd_entry_t *slot(hh_failure_detector_t *fd, hh_node_id_t id)
{
    hh_fd_entry_t *freeslot = NULL;
    for (size_t i = 0; i < HH_FD_MAX_TRACKED; i++) {
        if (fd->entries[i].used && fd->entries[i].id == id) return &fd->entries[i];
        if (!fd->entries[i].used && !freeslot) freeslot = &fd->entries[i];
    }
    if (freeslot) {
        memset(freeslot, 0, sizeof *freeslot);
        freeslot->used = true;
        freeslot->id = id;
    }
    return freeslot;
}

hh_status_t hh_fd_on_link_state(hh_failure_detector_t *fd,
                                const hh_ev_link_state_t *ev, hh_time_ms_t now)
{
    hh_fd_entry_t *e;

    if (!fd || !ev) return HH_ERR_INVAL;
    e = slot(fd, ev->neighbor);
    if (!e) return HH_ERR_NOMEM;

    if (ev->new_state == HH_LINK_SUSPECTED_FAILURE) {
        /* Suspicion is recorded but never announced: Doc 1 §8 is explicit that
         * "Degradation Detected alone, without confirmation, never reaches
         * route invalidation." */
        if (!e->suspected_at) e->suspected_at = now;
        e->cause = ev->cause_hint;
        return HH_OK;
    }

    if (ev->new_state == HH_LINK_FAILED && !e->confirmed) {
        hh_event_t out;
        e->confirmed = true;
        e->confirmed_at = now;
        e->cause = ev->cause_hint;
        e->corroborations = 1;      /* our own observation */
        fd->confirmations++;

        HH_LOGI(COMP, "failure_confirmed", "node=%u cause=%s suspected_ms=%llu",
                ev->neighbor, hh_cause_hint_str(e->cause),
                (unsigned long long)(e->suspected_at ? now - e->suspected_at : 0));

        memset(&out, 0, sizeof out);
        out.type = HH_EV_FAILURE_DETECTED;
        out.timestamp = now;
        out.u.failure.neighbor_or_node_id = ev->neighbor;
        out.u.failure.cause_hint = e->cause;
        out.u.failure.confirmation_time = now;
        return hh_dispatcher_publish(fd->bus, &out);
    }

    /* Recovery retracts the confirmation so a returning node is treated as a
     * fresh discovery rather than staying permanently marked failed. */
    if ((ev->new_state == HH_LINK_RECOVERING || ev->new_state == HH_LINK_HEALTHY) &&
        e->confirmed) {
        e->confirmed = false;
        e->suspected_at = 0;
        e->corroborations = 0;
        fd->retractions++;
        HH_LOGI(COMP, "failure_retracted", "node=%u new_state=%s",
                ev->neighbor, hh_link_state_str(ev->new_state));
    }
    return HH_OK;
}

void hh_fd_corroborate(hh_failure_detector_t *fd, hh_node_id_t id)
{
    hh_fd_entry_t *e;
    if (!fd) return;
    e = slot(fd, id);
    if (e && e->corroborations < UINT32_MAX) e->corroborations++;
}

bool hh_fd_is_failed(const hh_failure_detector_t *fd, hh_node_id_t id)
{
    if (!fd) return false;
    for (size_t i = 0; i < HH_FD_MAX_TRACKED; i++)
        if (fd->entries[i].used && fd->entries[i].id == id)
            return fd->entries[i].confirmed;
    return false;
}

uint32_t hh_fd_corroborations(const hh_failure_detector_t *fd, hh_node_id_t id)
{
    if (!fd) return 0;
    for (size_t i = 0; i < HH_FD_MAX_TRACKED; i++)
        if (fd->entries[i].used && fd->entries[i].id == id)
            return fd->entries[i].corroborations;
    return 0;
}
