/*
 * radiod fault registry.
 *
 * The architecture assigns "Events, fault registry" to radiod. This is the
 * registry half: a bounded record of which faults are active, how often each
 * has occurred, and when it was first and last seen — so an operator asking
 * "what is wrong with this radio, and since when?" gets an answer from radiod
 * rather than by correlating log lines.
 *
 * ==========================================================================
 * WHAT THIS DELIBERATELY DOES NOT DEFINE
 * ==========================================================================
 * The authoritative fault taxonomy does not exist yet (see unknown.md, U-05):
 * the real hardware fault list, severity levels, which faults latch versus
 * clear themselves, who may clear a latched fault, and required history depth
 * are all unspecified.
 *
 * So this registry is built over exactly the five fault kinds the control
 * protocol already defines and already tests (hh_rc_fault_t): none,
 * tx_failure, rx_silence, hw_fault, backend_io. No severity field, no fault
 * classes, and no hardware fault codes are invented here.
 *
 * It is also purely IN-PROCESS. radiod does not push events to clients,
 * because asynchronous delivery needs a wire framing for unsolicited
 * messages, and the control protocol has none (U-01: the protocol is strictly
 * one response per request). Clients poll. When an event framing exists, this
 * registry is the source that feeds it — that is why the record carries
 * timestamps and sequence numbers it does not strictly need today.
 * ==========================================================================
 */
#ifndef HHSDR_RADIOD_EVENTS_H
#define HHSDR_RADIOD_EVENTS_H

#include "hhsdr/core/types.h"
#include "hhsdr/protocol/rc.h"

/* One entry per fault kind. HH_RC_FAULT_NONE occupies slot 0 and is never
 * recorded as a fault; sizing by the enum means adding a kind cannot silently
 * overflow the table. */
#define HH_RADIOD_FAULT_SLOTS (HH_RC_FAULT_BACKEND_IO + 1)

typedef struct {
    bool         active;        /* currently asserted                       */
    uint32_t     count;         /* times asserted since reset               */
    hh_time_ms_t first_seen;    /* when it was first asserted               */
    hh_time_ms_t last_seen;     /* when it was most recently asserted       */
    hh_time_ms_t cleared_at;    /* when it was last cleared; 0 if never     */
} hh_radiod_fault_record_t;

typedef struct {
    hh_radiod_fault_record_t faults[HH_RADIOD_FAULT_SLOTS];

    /* Monotonic counter of registry transitions (assert or clear). Lets a
     * polling client tell "nothing changed" from "changed and changed back"
     * without a push channel. */
    uint64_t generation;

    /* Total assertions and clears, across all kinds. */
    uint64_t asserted_total;
    uint64_t cleared_total;
} hh_radiod_faults_t;

/* Reset to empty. */
void hh_radiod_faults_init(hh_radiod_faults_t *f);

/* Record a fault as asserted. Idempotent for an already-active fault: the
 * count and last_seen advance, but first_seen and the generation do not, so a
 * backend that re-reports the same condition every poll does not look like a
 * storm of distinct faults.
 *
 * HH_RC_FAULT_NONE is ignored -- "no fault" is not a fault. */
void hh_radiod_faults_assert(hh_radiod_faults_t *f, hh_rc_fault_t kind,
                             hh_time_ms_t now);

/* Clear one fault. Clearing an inactive fault is a no-op. */
void hh_radiod_faults_clear(hh_radiod_faults_t *f, hh_rc_fault_t kind,
                            hh_time_ms_t now);

/* Clear every active fault. This is what HH_RC_CMD_CLEAR_FAULT maps onto,
 * since that command carries no fault kind. */
void hh_radiod_faults_clear_all(hh_radiod_faults_t *f, hh_time_ms_t now);

/* True if any fault is currently active. */
bool hh_radiod_faults_any_active(const hh_radiod_faults_t *f);

/* Number of currently active faults. */
uint32_t hh_radiod_faults_active_count(const hh_radiod_faults_t *f);

/* Read one record. Returns NULL for an out-of-range kind. */
const hh_radiod_fault_record_t *hh_radiod_faults_get(const hh_radiod_faults_t *f,
                                                     hh_rc_fault_t kind);

#endif /* HHSDR_RADIOD_EVENTS_H */
