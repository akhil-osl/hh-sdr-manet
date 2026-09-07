/*
 * Mock radio adapter — TEST INFRASTRUCTURE ONLY.
 *
 * Lives under tests/ and is never linked into the production library. It
 * implements exactly the same hh_radio_ops_t contract the future FPGA adapter
 * will implement, so the production stack cannot tell the two apart and needs
 * no simulation-specific code path.
 *
 * It supplies controlled values through the same interfaces real hardware will
 * later provide: frames arrive via the rx callback, metrics via the sample
 * carried with each frame and via get_link_metrics.
 */
#ifndef HH_MOCK_RADIO_H
#define HH_MOCK_RADIO_H

#include "hhsdr/radio/radio.h"

#define MOCK_RX_QUEUE   256
#define MOCK_TX_LOG     256
#define MOCK_MAX_PEERS  32

typedef struct {
    hh_frame_t       frame;
    hh_link_sample_t metrics;
    hh_time_ms_t     deliver_at;   /* virtual-time delivery, models latency */
    bool             used;
} mock_rx_item_t;

typedef struct mock_radio {
    const char      *label;
    bool             opened;
    bool             operational;       /* false models own-radio failure    */
    uint32_t         channel;
    hh_radio_rx_fn   rx_fn;
    void            *rx_ctx;

    mock_rx_item_t   rx[MOCK_RX_QUEUE];
    size_t           rx_count;

    hh_frame_t       tx_log[MOCK_TX_LOG];
    size_t           tx_count;         /* wraps; tx_total is the true count  */
    uint64_t         tx_total;
    bool             tx_fails;         /* models a transmit-path failure     */

    /* Per-peer metrics returned by get_link_metrics (HTI-05 periodic path). */
    struct { hh_node_id_t id; hh_link_sample_t s; bool set; } peers[MOCK_MAX_PEERS];

    bool             supports_channel_change;
    uint64_t         frames_rx;
    uint64_t         rx_errors;

    /* Set by the harness so poll() can use virtual time. */
    hh_time_ms_t     now;
} mock_radio_t;

void mock_radio_init(mock_radio_t *m, const char *label, hh_radio_t *out);

/* Queue an inbound frame for delivery at or after deliver_at (virtual time). */
void mock_radio_enqueue_rx(mock_radio_t *m, const hh_frame_t *f,
                           const hh_link_sample_t *metrics, hh_time_ms_t deliver_at);

void mock_radio_set_peer_metrics(mock_radio_t *m, hh_node_id_t id, const hh_link_sample_t *s);
void mock_radio_clear_tx_log(mock_radio_t *m);

/* Count transmitted frames of a given kind since the log was last cleared. */
size_t mock_radio_tx_count_kind(const mock_radio_t *m, hh_frame_kind_t kind);
const hh_frame_t *mock_radio_last_tx(const mock_radio_t *m, hh_frame_kind_t kind);

#endif /* HH_MOCK_RADIO_H */
