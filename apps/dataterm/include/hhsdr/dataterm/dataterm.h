/*
 * Data terminal protocol engine: stop-and-wait ACK and retry per destination,
 * duplicate detection and loss counting from sequence gaps.
 *
 * Transport-free. Datagrams leave through tx_fn and arrive through
 * hh_dt_input(), so tests can run it on an emulated link with a virtual clock.
 * Single-threaded, no allocation.
 */
#ifndef HHSDR_DATATERM_DATATERM_H
#define HHSDR_DATATERM_DATATERM_H

#include "hhsdr/core/clock.h"
#include "hhsdr/dataterm/msg.h"

#define HH_DT_MAX_PEERS  16
#define HH_DT_QUEUE_LEN  8

typedef struct {
    uint32_t timeout_ms;     /* first ACK wait, doubles per retry       */
    uint8_t  max_retries;    /* 0 = send once, retry off                */
    uint16_t max_payload;    /* larger messages are refused             */
} hh_dt_config_t;

void hh_dt_config_default(hh_dt_config_t *cfg);

typedef hh_status_t (*hh_dt_tx_fn)(hh_node_id_t to, const uint8_t *buf, size_t len,
                                   void *ctx);
typedef void (*hh_dt_deliver_fn)(hh_node_id_t from, hh_seq_t seq, const uint8_t *data,
                                 size_t len, void *ctx);
/* Called once per message when it is acknowledged or given up. */
typedef void (*hh_dt_result_fn)(hh_node_id_t to, hh_seq_t seq, bool acked,
                                uint8_t attempts, void *ctx);

typedef struct {
    uint64_t tx_data;            /* first transmissions                     */
    uint64_t tx_retries;
    uint64_t tx_acked;
    uint64_t tx_acked_first_try; /* ACK echoed attempt 0                    */
    uint64_t tx_given_up;
    uint64_t tx_acks;
    uint64_t tx_errors;          /* transport refused a datagram            */
    uint64_t tx_too_large;
    uint64_t tx_queue_full;
    uint64_t rx_data;
    uint64_t rx_delivered;
    uint64_t rx_first_try;       /* delivered with attempt 0                */
    uint64_t rx_duplicates;
    uint64_t rx_gaps;            /* messages missing from sequence gaps     */
    uint64_t rx_acks;
    uint64_t rx_acks_unexpected;
    uint64_t rx_not_for_me;
    uint64_t rx_bad;             /* failed decode, any reason               */
    uint64_t rx_bad_crc;
    uint64_t rx_peer_table_full;
} hh_dt_counters_t;

typedef struct {
    uint16_t len;
    uint8_t  data[HH_DT_PAYLOAD_MAX];
} hh_dt_slot_t;

typedef struct {
    bool         used;
    hh_node_id_t node;

    hh_seq_t     next_seq;
    hh_dt_slot_t queue[HH_DT_QUEUE_LEN];
    uint8_t      q_head;
    uint8_t      q_count;
    bool         inflight;
    hh_seq_t     inflight_seq;
    uint8_t      attempt;
    uint32_t     cur_timeout;
    hh_time_ms_t deadline;

    bool         rx_seen;
    hh_seq_t     rx_first;
    hh_seq_t     rx_last;
    uint64_t     rx_delivered;
    uint64_t     rx_gaps;
    uint64_t     rx_duplicates;
} hh_dt_peer_t;

typedef struct {
    hh_node_id_t      self;
    hh_dt_config_t    cfg;
    const hh_clock_t *clock;
    hh_dt_tx_fn       tx;
    void             *tx_ctx;
    hh_dt_deliver_fn  deliver;
    void             *deliver_ctx;
    hh_dt_result_fn   result;
    void             *result_ctx;
    hh_dt_peer_t      peers[HH_DT_MAX_PEERS];
    hh_dt_counters_t  counters;
    uint8_t           txbuf[HH_DT_MSG_MAX];
} hh_dt_t;

hh_status_t hh_dt_init(hh_dt_t *dt, hh_node_id_t self, const hh_dt_config_t *cfg,
                       const hh_clock_t *clock, hh_dt_tx_fn tx, void *tx_ctx);
void hh_dt_set_deliver(hh_dt_t *dt, hh_dt_deliver_fn fn, void *ctx);
void hh_dt_set_result(hh_dt_t *dt, hh_dt_result_fn fn, void *ctx);

/* Queues a message. HH_ERR_INVAL if too large, HH_ERR_AGAIN if the queue to
 * that destination is full, HH_ERR_NOMEM if the peer table is full. */
hh_status_t hh_dt_send(hh_dt_t *dt, hh_node_id_t to, const uint8_t *data, size_t len);

/* One received datagram. Returns the decode result. */
hh_dt_rx_err_t hh_dt_input(hh_dt_t *dt, const uint8_t *buf, size_t len);

/* Runs retry and give-up timers. Call whenever time has moved. */
void hh_dt_poll(hh_dt_t *dt);

/* Milliseconds until the next timer, or -1 if none is running. */
int64_t hh_dt_next_timeout_ms(const hh_dt_t *dt);

/* True while any message waits for an ACK or sits in a queue. */
bool hh_dt_busy(const hh_dt_t *dt);

const hh_dt_counters_t *hh_dt_counters(const hh_dt_t *dt);

/* One stats record, then one record per peer, at INFO. */
void hh_dt_log_stats(const hh_dt_t *dt);

#endif /* HHSDR_DATATERM_DATATERM_H */
