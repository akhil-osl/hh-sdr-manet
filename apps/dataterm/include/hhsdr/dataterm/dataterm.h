/*
 * Data terminal over CoAP (RFC 7252) with libcoap. A text message is a
 * confirmable POST to /msg with Content-Format 0. libcoap owns the sockets,
 * the ACK and RST, and the retransmission timer.
 *
 * Each destination and class gets one client session, so Message IDs count up
 * per session and the receiver counts lost messages from gaps per stream. Some libcoap versions hand a
 * repeated CON to the handler again, so duplicates are also dropped here with
 * a 32-entry window.
 *
 * With max_retransmit 0 a message goes as NON, because libcoap will not send a
 * CON without retries. The 2.04 reply is then waited for one ACK timeout.
 */
#ifndef HHSDR_DATATERM_DATATERM_H
#define HHSDR_DATATERM_DATATERM_H

#include "hhsdr/core/clock.h"
#include "hhsdr/dataterm/call.h"
#include "hhsdr/dataterm/peers.h"
#include "hhsdr/dataterm/sched.h"

#define HH_DT_COAP_PORT     5683u
/* Ethernet 1500 minus IPv4 20, UDP 8 and the CoAP header: the buffer bound. */
#define HH_DT_PAYLOAD_MAX   1452u
/* Messages in flight or waiting in libcoap, per destination. */
#define HH_DT_QUEUE_LEN     8u
#define HH_DT_RX_WINDOW     32u
/* Messages waiting for their turn, per class. */
#define HH_DT_PENDING_LEN   8u
/* One CoAP session, and so one source port, per destination and class. */
#define HH_DT_STREAMS       (HH_DT_MAX_PEERS * HH_DT_CLASS_COUNT)
/* libcoap accepts no shorter ACK timeout. */
#define HH_DT_ACK_TIMEOUT_MIN_MS 1000u

typedef struct {
    uint32_t ack_timeout_ms; /* first ACK wait. libcoap adds up to 50 % at random */
    uint8_t  max_retransmit; /* 0 = send once as NON, retry off                  */
    uint16_t max_payload;    /* larger messages are refused                       */
} hh_dt_config_t;

void hh_dt_config_default(hh_dt_config_t *cfg);

/* from is HH_NODE_ID_INVALID when the sender's IP is not in the peer table. */
typedef void (*hh_dt_deliver_fn)(hh_node_id_t from, const char *from_ip, uint16_t mid,
                                 const uint8_t *data, size_t len, void *ctx);
/* Called once per message. reason: "acked", "timeout", "rst", "refused" or
 * "unreachable". */
typedef void (*hh_dt_result_fn)(hh_node_id_t to, uint16_t mid, bool acked,
                                const char *reason, void *ctx);

typedef struct {
    uint64_t tx_data;
    uint64_t tx_acked;
    uint64_t tx_given_up;     /* no ACK after all retransmissions, or no
                                 reply to a NON in one ACK timeout       */
    uint64_t tx_rst;          /* peer answered RST                       */
    uint64_t tx_refused;      /* peer answered with an error code        */
    uint64_t tx_unreachable;  /* ICMP error or socket failure            */
    uint64_t tx_errors;       /* libcoap refused to build or send        */
    uint64_t tx_too_large;
    uint64_t tx_queue_full;
    uint64_t tx_no_route;     /* destination not in the peer table       */
    uint64_t tx_pending_full; /* a class queue was full                  */
    uint64_t rx_data;
    uint64_t rx_delivered;
    uint64_t rx_duplicates;
    uint64_t rx_gaps;         /* messages missing from Message ID gaps   */
    uint64_t rx_resync;       /* sender restarted (new source port) or its
                                 Message ID jumped too far               */
    uint64_t rx_bad_format;   /* Content-Format other than text, answered 4.15 */
    uint64_t rx_unknown_sender;
    uint64_t rx_table_full;   /* oldest stream dropped to make room       */
} hh_dt_counters_t;

struct coap_context_t;
struct coap_session_t;
struct hh_dt;

typedef struct {
    bool         used;
    uint16_t     mid;
    uint8_t      token[4];
    hh_time_ms_t deadline;
} hh_dt_non_t;

typedef struct {
    bool                   used;
    hh_node_id_t           node;
    hh_dt_class_t          cls;
    struct coap_session_t *session;
    uint8_t                outstanding;   /* CON and NON together */
    hh_dt_non_t            non[HH_DT_QUEUE_LEN];
    struct hh_dt          *dt;
} hh_dt_tx_peer_t;

typedef struct {
    bool               used;
    struct sockaddr_in addr;      /* IP and source port: one stream */
    hh_time_ms_t       last_ms;
    hh_node_id_t       node;
    uint16_t           last_mid;
    uint32_t           window;    /* bit n: last_mid - n was received */
    uint64_t           delivered;
    uint64_t           gaps;
    uint64_t           duplicates;
} hh_dt_rx_peer_t;

typedef enum { HH_DT_PATH_MSG = 0, HH_DT_PATH_CALL } hh_dt_path_t;

typedef struct {
    hh_node_id_t to;
    hh_dt_path_t path;
    uint16_t     len;
    uint8_t      data[HH_DT_PAYLOAD_MAX];
} hh_dt_pending_t;

typedef struct hh_dt {
    struct coap_context_t *ctx;
    const hh_clock_t      *clock;
    hh_dt_config_t         cfg;
    int                    dscp;
    uint16_t               port;
    const hh_dt_peers_t   *peers;
    hh_dt_deliver_fn       deliver;
    void                  *deliver_ctx;
    hh_dt_result_fn        result;
    void                  *result_ctx;
    hh_dt_tx_peer_t        tx[HH_DT_STREAMS];
    hh_dt_rx_peer_t        rx[HH_DT_STREAMS];
    hh_dt_pending_t        pend[HH_DT_CLASS_COUNT][HH_DT_PENDING_LEN];  /* ring per class */
    uint8_t                pend_head[HH_DT_CLASS_COUNT];
    uint8_t                pend_n[HH_DT_CLASS_COUNT];
    hh_dt_sched_t          sched;
    hh_call_t              call;
    hh_call_event_fn       call_event;
    void                  *call_event_ctx;
    hh_dt_counters_t       counters;
} hh_dt_t;

/* Opens the CoAP endpoint. port 0 picks a free port. dscp is 0 to 63 and is
 * set, with don't-fragment, on every socket libcoap opens. */
hh_status_t hh_dt_open(hh_dt_t *dt, const char *bind_ip, uint16_t port, int dscp,
                       const hh_dt_config_t *cfg, const hh_dt_peers_t *peers);
void hh_dt_close(hh_dt_t *dt);

void hh_dt_set_deliver(hh_dt_t *dt, hh_dt_deliver_fn fn, void *ctx);
void hh_dt_set_result(hh_dt_t *dt, hh_dt_result_fn fn, void *ctx);

/* Sends one message. HH_ERR_INVAL if too large, HH_ERR_NOTFOUND if the
 * destination is not in the peer table, HH_ERR_AGAIN if HH_DT_QUEUE_LEN
 * messages to it are already outstanding, HH_ERR_IO if libcoap fails. */
hh_status_t hh_dt_send(hh_dt_t *dt, hh_node_id_t to, const uint8_t *data, size_t len);

/* Queues a message in a class. It leaves at the next hh_dt_process(), in class
 * order (voice, video, data) with the guards in sched.h, on a socket marked
 * with that class's DSCP. HH_ERR_INVAL for the signalling class, which only
 * call setup uses. HH_ERR_AGAIN if HH_DT_PENDING_LEN messages already wait in
 * the class. Other errors as hh_dt_send(). The data class is marked with the
 * dscp given to hh_dt_open(). */
hh_status_t hh_dt_send_class(hh_dt_t *dt, hh_node_id_t to, hh_dt_class_t cls,
                             const uint8_t *data, size_t len);

/* Voice call setup, see call.h. Signalling leaves in the signalling class
 * (DSCP CS5) as CoAP POST to /call. HH_ERR_NOTFOUND if the destination is not
 * in the peer table. */
hh_status_t hh_dt_call_request(hh_dt_t *dt, hh_node_id_t to);
hh_status_t hh_dt_call_accept(hh_dt_t *dt);
hh_status_t hh_dt_call_reject(hh_dt_t *dt);
hh_status_t hh_dt_call_release(hh_dt_t *dt);
void        hh_dt_set_call_event(hh_dt_t *dt, hh_call_event_fn fn, void *ctx);
const hh_call_t *hh_dt_call(const hh_dt_t *dt);

/* File descriptor to poll. libcoap's epoll set, which also wakes for its
 * retransmission timers. */
int hh_dt_fd(const hh_dt_t *dt);

/* Milliseconds until the next NON reply wait ends, or -1 if none is running.
 * CON timers wake hh_dt_fd() by themselves. */
int64_t hh_dt_next_timeout_ms(const hh_dt_t *dt);

/* Reads waiting input and runs due timers. Never blocks. */
void hh_dt_process(hh_dt_t *dt);

/* True while any message waits for its ACK. */
bool hh_dt_busy(const hh_dt_t *dt);

const hh_dt_counters_t *hh_dt_counters(const hh_dt_t *dt);

/* One stats record, then one record per sender, at INFO. */
void hh_dt_log_stats(const hh_dt_t *dt);

#endif /* HHSDR_DATATERM_DATATERM_H */
