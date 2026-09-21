/*
 * Radio Control (RC) protocol — client <-> radiod IPC contract.
 *
 * ==========================================================================
 * WHY THIS HEADER EXISTS SEPARATELY FROM radio.h
 * ==========================================================================
 * hh_radio_ops_t (radio.h) is a same-process function-pointer vtable: it
 * takes a raw `void *self`, returns C structs by pointer, and assumes a
 * single address space. It has no request framing, no error-in-band
 * encoding, and no notion of "client" versus "server" — it is the
 * radiod <-> backend contract, not a wire protocol.
 *
 * A standalone radiod process needs an actual client <-> daemon contract
 * that crosses a process boundary. That is what this header defines. It is
 * narrowly scoped to the control-plane interfaces the HW/SW Interface
 * Specification assigns to the Radio/SDR Interface boundary:
 *   - HTI-02  Radio Status
 *   - HTI-14  Channel / Radio Control
 *   - HTI-15  Control Plane <-> Management/Telemetry (config in, status out)
 *
 * It deliberately excludes HTI-03/04/05 (frame transmit/receive and
 * per-frame link-quality samples): those are data-plane / high-rate
 * interfaces and must not be routed through generic daemon request/response
 * IPC. A future data-plane transport (e.g. shared memory or a dedicated
 * socket) is out of scope for this contract.
 *
 * This does not redefine HTI semantics and does not duplicate
 * hh_radio_ops_t: it is a thin line-oriented request/response framing over
 * the same status/config/channel concepts, for use across a process
 * boundary. Encoding is line-oriented key=value text, matching the
 * project's existing config-file and telemetry-export conventions
 * (see include/hhsdr/core/config.h, tools/sim/telemetry_server.h) rather
 * than inventing a new serialization layer.
 * ==========================================================================
 */
#ifndef HHSDR_RADIO_RC_H
#define HHSDR_RADIO_RC_H

#include "hhsdr/core/types.h"
#include <stddef.h>

/* Default UNIX domain socket path. Overridable by the daemon/client caller. */
#define HH_RC_DEFAULT_SOCK_PATH "/tmp/hh-radiod.sock"

/* One request/response line, including its trailing newline, must fit here. */
#define HH_RC_MAX_LINE 512

/* radiod lifecycle state (Phase 3). Names are drawn directly from the
 * existing hh_node_state_t (node.h) plus a FAULTED state required by
 * HTI-02's own-radio-failure semantics; no new naming scheme is invented. */
typedef enum {
    HH_RC_STATE_CREATED = 0,
    HH_RC_STATE_INITIALIZED,
    HH_RC_STATE_CONFIGURED,
    HH_RC_STATE_RUNNING,
    HH_RC_STATE_STOPPED,
    HH_RC_STATE_FAULTED,
    HH_RC_STATE_RELEASED
} hh_rc_state_t;

const char *hh_rc_state_str(hh_rc_state_t s);

/* Request verbs. Each maps to one control-plane operation; there is no
 * separate wire opcode for RX/TX (data plane, out of scope here). */
typedef enum {
    HH_RC_CMD_INIT = 0,     /* -> INITIALIZED                          */
    HH_RC_CMD_CONFIGURE,    /* -> CONFIGURED (HTI-15 config in)        */
    HH_RC_CMD_START,        /* -> RUNNING                              */
    HH_RC_CMD_STOP,         /* -> STOPPED                              */
    HH_RC_CMD_SHUTDOWN,     /* -> RELEASED, daemon exits after reply   */
    HH_RC_CMD_STATUS,       /* HTI-02 radio status                     */
    HH_RC_CMD_STATS,        /* counters (subset of HTI-02/HTI-01-like) */
    HH_RC_CMD_SET_CHANNEL,  /* HTI-14 channel/radio control             */
    HH_RC_CMD_INJECT_FAULT, /* fault-injection control, for validation */
    HH_RC_CMD_CLEAR_FAULT,
    HH_RC_CMD__MAX
} hh_rc_cmd_t;

const char *hh_rc_cmd_str(hh_rc_cmd_t c);
bool        hh_rc_cmd_parse(const char *s, hh_rc_cmd_t *out);

/* Fault kinds meaningful to the control contract (Phase 4). Each corresponds
 * to an error path already representable in hh_radio_status_t / hh_status_t;
 * none are invented simulation features. */
typedef enum {
    HH_RC_FAULT_NONE = 0,
    HH_RC_FAULT_TX_FAILURE,     /* transmit() reports HH_ERR_IO           */
    HH_RC_FAULT_RX_SILENCE,     /* no inbound frames/metrics delivered    */
    HH_RC_FAULT_HW_FAULT,       /* radio reports operational=false        */
    HH_RC_FAULT_BACKEND_IO      /* backend I/O failure (e.g. set_channel) */
} hh_rc_fault_t;

const char *hh_rc_fault_str(hh_rc_fault_t f);
bool        hh_rc_fault_parse(const char *s, hh_rc_fault_t *out);

/* A parsed request. Text on the wire is one line:
 *   <cmd>[ key=value]*\n
 * e.g. "configure node_id=1 channel=3\n", "set_channel channel=5\n",
 * "inject_fault kind=tx_failure\n", "status\n"
 */
typedef struct {
    hh_rc_cmd_t   cmd;
    hh_node_id_t  node_id;      /* CONFIGURE                            */
    uint32_t      channel;      /* CONFIGURE, SET_CHANNEL               */
    hh_rc_fault_t fault;        /* INJECT_FAULT                         */
} hh_rc_request_t;

/* A parsed/formatted response. Text on the wire is one line:
 *   ok <cmd>[ key=value]*\n   or   err <cmd> reason=<status>\n
 */
typedef struct {
    bool          ok;
    hh_rc_cmd_t   cmd;
    hh_status_t   reason;        /* meaningful when !ok                  */

    /* STATUS payload (HTI-02). */
    hh_rc_state_t state;
    bool          operational;
    uint32_t      channel;
    float         frequency_hz;
    uint32_t      waveform_id;

    /* STATS payload. */
    uint64_t      frames_tx;
    uint64_t      frames_rx;
    uint64_t      tx_errors;
    uint64_t      rx_errors;
    uint64_t      requests_total;
    uint64_t      requests_rejected;
} hh_rc_response_t;

/* Parse one request line (no trailing newline required). HH_ERR_INVAL on a
 * malformed or unknown-command line. */
hh_status_t hh_rc_request_parse(const char *line, hh_rc_request_t *out);

/* Format a request line into buf (NUL-terminated, includes trailing '\n').
 * Returns bytes written excluding the NUL, or 0 if it does not fit. */
size_t hh_rc_request_format(const hh_rc_request_t *r, char *buf, size_t cap);

/* Parse one response line. HH_ERR_INVAL on a malformed line. */
hh_status_t hh_rc_response_parse(const char *line, hh_rc_response_t *out);

/* Format a response line into buf (NUL-terminated, includes trailing '\n').
 * Returns bytes written excluding the NUL, or 0 if it does not fit. */
size_t hh_rc_response_format(const hh_rc_response_t *r, char *buf, size_t cap);

#endif /* HHSDR_RADIO_RC_H */
