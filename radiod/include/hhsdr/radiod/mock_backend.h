/*
 * radiod mock backend — deterministic hh_radio_ops_t implementation.
 *
 * This is radiod's own backend, distinct from tests/sim/mock_radio.c (which
 * is test-harness infrastructure for exercising the MANET stack in-process
 * and is never linked into radiod). It implements exactly the
 * hh_radio_ops_t contract from radio.h — the same contract the future FPGA
 * adapter will implement — so radiod's control loop and state machine are
 * backend-agnostic.
 *
 * It is a control-plane validation backend, not an RF/PHY simulator: no
 * waveform, modulation, or propagation modeling. Frame transmit/receive is
 * modeled only to the extent radiod's own status/statistics reporting needs
 * (frames_tx/frames_rx/error counters), per HTI-02.
 */
#ifndef HHSDR_RADIOD_MOCK_BACKEND_H
#define HHSDR_RADIOD_MOCK_BACKEND_H

#include "hhsdr/radio/radio.h"
#include "hhsdr/protocol/rc.h"

typedef struct {
    bool         opened;
    bool         operational;
    uint32_t     channel;
    float        frequency_hz;
    uint32_t     waveform_id;

    uint64_t     frames_tx;
    uint64_t     frames_rx;
    uint64_t     tx_errors;
    uint64_t     rx_errors;

    hh_rc_fault_t active_fault;

    hh_radio_rx_fn rx_fn;
    void          *rx_ctx;
} hh_mock_backend_t;

void hh_mock_backend_init(hh_mock_backend_t *m, hh_radio_t *out);

/* Fault injection control (Phase 4). Idempotent; HH_RC_FAULT_NONE clears. */
void hh_mock_backend_inject_fault(hh_mock_backend_t *m, hh_rc_fault_t fault);
hh_rc_fault_t hh_mock_backend_active_fault(const hh_mock_backend_t *m);

#endif /* HHSDR_RADIOD_MOCK_BACKEND_H */
