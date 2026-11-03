/*
 * Voice call setup signalling for the data terminal: request, accept, reject
 * and release, with timeouts. One call at a time. No media: this is the
 * signalling state machine only.
 *
 * The module neither opens sockets nor reads the clock itself. The caller
 * hands it a send function (the CoAP layer) and a clock, and feeds received
 * payloads to hh_call_on_rx() and the clock to hh_call_poll().
 *
 * PROPOSED payload, 4 bytes, no contract exists yet:
 *   bytes 0-1  call id, big endian, never 0
 *   byte  2    type: 1 request, 2 accept, 3 reject, 4 release
 *   byte  3    reason: 0 none, 1 busy, 2 declined, 3 timeout, 4 normal
 */
#ifndef HHSDR_DATATERM_CALL_H
#define HHSDR_DATATERM_CALL_H

#include "hhsdr/core/clock.h"

#define HH_CALL_PDU_LEN 4u

typedef enum {
    HH_CALL_IDLE = 0,
    HH_CALL_CALLING,   /* request sent, waiting for accept or reject */
    HH_CALL_RINGING,   /* request received, waiting for the user     */
    HH_CALL_ACTIVE
} hh_call_state_t;

typedef enum {
    HH_CALL_MSG_REQUEST = 1,
    HH_CALL_MSG_ACCEPT  = 2,
    HH_CALL_MSG_REJECT  = 3,
    HH_CALL_MSG_RELEASE = 4
} hh_call_msg_t;

typedef enum {
    HH_CALL_REASON_NONE     = 0,
    HH_CALL_REASON_BUSY     = 1,
    HH_CALL_REASON_DECLINED = 2,
    HH_CALL_REASON_TIMEOUT  = 3,
    HH_CALL_REASON_NORMAL   = 4
} hh_call_reason_t;

typedef enum {
    HH_CALL_EV_INCOMING,  /* a request arrived: answer with accept or reject */
    HH_CALL_EV_ACTIVE,    /* the call is up on both sides                    */
    HH_CALL_EV_ENDED      /* the call is over, the module is idle again      */
} hh_call_event_kind_t;

typedef struct {
    hh_call_event_kind_t kind;
    hh_node_id_t         peer;
    uint16_t             id;
    hh_call_reason_t     reason;  /* HH_CALL_EV_ENDED only */
} hh_call_event_t;

/* Queues or sends one signalling payload to a node. Returns HH_OK only if the
 * payload is on its way; a full queue returns HH_ERR_AGAIN. */
typedef hh_status_t (*hh_call_send_fn)(hh_node_id_t to, const uint8_t *pdu, size_t len,
                                       void *ctx);
typedef void (*hh_call_event_fn)(const hh_call_event_t *ev, void *ctx);

typedef struct {
    uint32_t setup_timeout_ms;  /* waiting for accept or reject */
    uint32_t ring_timeout_ms;   /* waiting for the user to answer */
} hh_call_config_t;

typedef struct {
    uint64_t rx_bad;        /* wrong length, unknown type or call id 0     */
    uint64_t rx_stray;      /* valid, but not for the current call         */
    uint64_t rx_duplicate;  /* repeated request of the current call        */
    uint64_t busy_rejected;
    uint64_t timeouts;
    uint64_t tx_failed;     /* the send function refused a payload */
} hh_call_counters_t;

typedef struct {
    hh_call_config_t   cfg;
    const hh_clock_t  *clock;
    hh_call_send_fn    send;
    hh_call_event_fn   event;
    void              *ctx;
    hh_call_state_t    state;
    hh_node_id_t       peer;
    uint16_t           id;
    uint16_t           last_id;
    hh_time_ms_t       deadline;  /* valid in CALLING and RINGING */
    hh_call_counters_t counters;
} hh_call_t;

void hh_call_config_default(hh_call_config_t *cfg);

hh_status_t hh_call_init(hh_call_t *c, const hh_call_config_t *cfg, const hh_clock_t *clock,
                         hh_call_send_fn send, hh_call_event_fn event, void *ctx);

/* Each call sends its message first. If the send function fails, the call
 * returns that status and the state does not change, so the caller can retry.
 * HH_ERR_STATE unless idle. HH_ERR_INVAL for an invalid node. */
hh_status_t hh_call_request(hh_call_t *c, hh_node_id_t to);
/* HH_ERR_STATE unless a request is ringing. */
hh_status_t hh_call_accept(hh_call_t *c);
hh_status_t hh_call_reject(hh_call_t *c);
/* Ends a call being set up (cancel) or an active call. HH_ERR_STATE otherwise. */
hh_status_t hh_call_release(hh_call_t *c);

/* Feeds one received payload. Malformed or unexpected input is counted, never
 * acted on. */
void hh_call_on_rx(hh_call_t *c, hh_node_id_t from, const uint8_t *pdu, size_t len);

/* Runs due timeouts. A timeout always ends the call, even if its release or
 * reject cannot be sent: the peer then ends it on its own timeout. */
void hh_call_poll(hh_call_t *c);
/* Milliseconds until the next timeout, or -1 when none is pending. */
int64_t hh_call_next_timeout_ms(const hh_call_t *c);

const char *hh_call_state_str(hh_call_state_t s);

#endif /* HHSDR_DATATERM_CALL_H */
