/*
 * radiod OpenCPI backend — the real PL backend, reached through RADHAWK.
 *
 * Implements BOTH of radiod's backend contracts:
 *   hh_radio_ops_t        (radio.h)  — lifecycle and HTI-02 status
 *   hh_radiod_props_ops_t (props.h)  — the named worker-property surface
 *
 * so `start`/`stop`/`status` and the `opencpi_*` verbs are all serviced by one
 * object with one view of the application's state.
 *
 * It owns no OpenCPI code itself. Every OpenCPI operation is performed by the
 * rh_agent helper process over the contract in opencpi_agent_proto.h, which
 * explains at length why a helper is the only possible boundary with this CDK.
 * Read that header first.
 *
 * ==========================================================================
 * HOW hh_radio_status_t IS FILLED — AND WHAT IS DELIBERATELY LEFT AT ZERO
 * ==========================================================================
 * HTI-02's fields were specified for a framed packet radio. This application
 * counts words, samples and bits. Only the mappings that are actually true are
 * made; the rest read zero rather than carrying a plausible-looking number,
 * and every real counter is available in full through `opencpi status`.
 *
 *   operational   = the agent is alive AND an application session is open.
 *                   It is NOT "the link has acquired": PRBS lock is reported
 *                   separately as qpsk_rx.in_sync, because a running radio
 *                   with no signal is not a radio failure.
 *   tx_errors     = qpsk_tx.dbg_out_dropped — samples destroyed at the port
 *                   boundary. This is a genuine transmit-error count and is
 *                   required to stay 0 (CLAUDE.md, TX properties).
 *   rx_errors     = qpsk_rx.err_count — bit errors from the PRBS checker.
 *                   HTI-02 defines rx_errors as the RF-interference input, and
 *                   for this waveform that is the measurement.
 *   frequency_hz  = the last radio.tx_lo_hz written through the control
 *                   surface, read back from the agent. It is the REQUESTED
 *                   value: RADHAWK hands LO settings to ad9361_init and does
 *                   not read the synthesiser back, so this reports what was
 *                   asked for, exactly as ad9361_proxy's own properties do.
 *   frames_tx     = 0. There is no frame counter anywhere in this chain. The
 *   frames_rx       frame contract is 2240 bits = 70 words, so a frame count
 *                   could be divided out of dbg_in_words — and is not, because
 *                   a derived number presented as a measured one is how a
 *                   partially-working link gets mistaken for a healthy one.
 *   channel       = 0. No logical-channel-to-RF mapping exists for this radio
 *                   (unknown.md U-14); set_channel therefore returns
 *                   HH_ERR_UNSUPPORTED rather than guessing a channel plan.
 *   waveform_id   = 0. radio.h records the encoding as TBD and none is
 *                   assigned in this project.
 *
 * transmit(), set_rx_callback() and get_link_metrics() return
 * HH_ERR_NOT_IMPLEMENTED. That is not an omission: rc.h excludes HTI-03/04/05
 * from the control contract because they are data-plane, high-rate interfaces,
 * and this application's payload path is DMA (see DMA-PORTING-GUIDE.md), not
 * request/response IPC.
 * ==========================================================================
 */
#ifndef HHSDR_RADIOD_OPENCPI_H
#define HHSDR_RADIOD_OPENCPI_H

#include "hhsdr/radio/radio.h"
#include "hhsdr/radiod/config.h"
#include "hhsdr/radiod/opencpi_agent_proto.h"
#include "hhsdr/radiod/props.h"
#include <sys/types.h>

/* Registry capacity. The agent publishes 114 properties on 18 worker names
 * (2026-10-07: app, radio, the tx/rx chains and the 14 DSP-stage workers of
 * the worker split, now with every per-worker debug counter -- see
 * rh_agent.cc); the headroom is for further workers. HH_OC_MAX_PROPS was 96
 * until 2026-10-07 (94 used).
 * A registry larger than either limit is reported as an error at startup
 * rather than silently truncated. HH_OC_MAX_WORKERS was 12 until the split,
 * which silently dropped every name after the 12th (rx_rrc onwards, radio). */
/* 2026-10-16, API 9: the audio chain added 74 rows (38 writable + 36
 * volatile across audio_codec_dev, audio_fe, audio_tx_framer and
 * audio_rx_deframer), taking the registry to 191 properties on 22 worker
 * names. 128 would have refused to load it - which is the designed
 * behaviour, an error at startup rather than a silent truncation, but it
 * would have meant no audio control at all. */
/* 2026-10-30, API 16: the voice equaliser added 4 audio_fe rows (eq_bypass +
 * eq_gain_lo/hi/mid), taking the registry from 255 to 259. The ceiling went
 * 256 -> 320 so the 259-row registry loads (256 would have errored at startup,
 * by design); the headroom is for further audio_fe / FEC growth. */
#define HH_OC_MAX_PROPS   320
#define HH_OC_MAX_WORKERS 32

typedef struct {
    char worker[HH_RC_MAX_NAME];
    char prop[HH_RC_MAX_NAME];
    char type[16];
    char access[16];
} hh_oc_prop_t;

typedef struct {
    /* Path to the rh_agent binary. */
    char     agent_path[HH_RADIOD_MAX_EXEC_PATH];

    /* How long to wait for one agent reply. Every agent call is synchronous
     * and therefore blocks radiod's control loop, so both bounds exist to put
     * a ceiling on that: a wedged agent must not wedge the control plane.
     *
     * start_timeout_ms is much larger because `start` may run ad9361_init,
     * which reprogrammes the AD9361 and is the slowest operation in the
     * system. */
    uint32_t reply_timeout_ms;
    uint32_t start_timeout_ms;
} hh_oc_config_t;

typedef struct {
    hh_oc_config_t cfg;

    /* The helper process and its pipes. pid <= 0 means no agent. */
    pid_t    pid;
    int      to_agent;      /* radiod -> agent stdin  */
    int      from_agent;    /* agent stdout -> radiod  */

    bool     session_open;  /* an OpenCPI application is deployed and started */
    bool     dry_run;       /* the agent reported a dry-run session            */

    /* The control surface, as published by the agent at startup. */
    hh_oc_prop_t props[HH_OC_MAX_PROPS];
    size_t       nprops;
    char         workers[HH_OC_MAX_WORKERS][HH_RC_MAX_NAME];
    size_t       nworkers;

    /* Partial-line buffer: a pipe read may split a reply. */
    char     rxbuf[HH_RH_AGENT_MAX_LINE];
    size_t   rxlen;

    /* Why the last operation failed, for the response's text field. */
    char     last_error[HH_RC_MAX_TEXT];
} hh_oc_backend_t;

/* Documented defaults: the agent at HH_RH_AGENT_DEFAULT_PATH, a 3 s ordinary
 * reply bound and a 60 s start bound. */
void hh_oc_config_defaults(hh_oc_config_t *cfg);

/*
 * Spawn the agent, check its protocol version and load its property registry,
 * then bind `radio_out` and `props_out` to this backend.
 *
 * Returns HH_ERR_IO when the agent cannot be started or does not answer —
 * which is the ordinary state on a host with no OpenCPI installation. The
 * handles are still bound and still callable in that case, and every operation
 * reports the failure honestly, exactly as hw_adapter.c does; radiod does not
 * silently fall back to a different backend.
 */
hh_status_t hh_oc_backend_init(hh_oc_backend_t *b, const hh_oc_config_t *cfg,
                               hh_radio_t *radio_out, hh_radiod_props_t *props_out);

/* Stop any session, ask the agent to quit, reap it, close the pipes. */
void hh_oc_backend_release(hh_oc_backend_t *b);

/* True while the helper process is running and answering. */
bool hh_oc_backend_agent_alive(const hh_oc_backend_t *b);

/* Last failure detail, for logging. Never NULL; empty when there is none. */
const char *hh_oc_backend_last_error(const hh_oc_backend_t *b);

#endif /* HHSDR_RADIOD_OPENCPI_H */
