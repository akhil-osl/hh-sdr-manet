/*
 * MAC service for the network layer: queues, priority, receive buffer and
 * link quality reports, as in the MAC to network layer interface note
 * (MAC-NETWORK-INTERFACE, F01-AK-3). Names follow the note.
 *
 * The lower side (MAC logic: slots, CRC, RF) is reached only through
 * hh_mac_next_pdu(), hh_mac_on_rx() and hh_mac_set_state(). Its real
 * binding is not defined yet.
 *
 * Single-threaded, no dynamic allocation.
 */
#ifndef HHSDR_MAC_MAC_H
#define HHSDR_MAC_MAC_H

#include "hhsdr/core/types.h"
#include "hhsdr/radio/radio.h"

/* Compile-time caps. Configured values must not exceed them. */
#define HH_MAC_PAYLOAD_MAX   512   /* current network limit, may change */
#define HH_MAC_QUEUE_CAP     128   /* frames per class */
#define HH_MAC_RXBUF_CAP     64
#define HH_MAC_NEIGHBOR_CAP  32

#define HH_MAC_BROADCAST HH_NODE_ID_INVALID

typedef enum {
    HH_MAC_OK = 0,          /* success for calls other than Send */
    HH_MAC_ACCEPTED,        /* Send: queued, not transmitted */
    HH_MAC_TRY_AGAIN,
    HH_MAC_WRONG_STATE,
    HH_MAC_TOO_LARGE,
    HH_MAC_INVALID_ARGUMENT,
    HH_MAC_INVALID_CONFIG,
    HH_MAC_NOT_OPEN,
    HH_MAC_NOT_FOUND,
    HH_MAC_UNSUPPORTED,
    HH_MAC_FAULT
} hh_mac_result_t;

typedef enum {
    HH_MAC_OFF = 0,
    HH_MAC_WAIT_TIME,
    HH_MAC_LISTEN,
    HH_MAC_ENTRY,
    HH_MAC_STEADY,
    HH_MAC_HOLDOVER,
    HH_MAC_LOST,
    HH_MAC_SAFE
} hh_mac_state_t;

typedef enum {
    HH_MAC_CLASS_CONTROL = 0,
    HH_MAC_CLASS_DATA,
    HH_MAC_CLASS_COUNT
} hh_mac_class_t;

/* A measured value. When valid is false the value has no meaning.
 * TODO: units. */
typedef struct {
    float value;
    bool  valid;
} hh_mac_metric_t;

typedef struct {
    hh_mac_metric_t rssi;
    hh_mac_metric_t snr;
    hh_mac_metric_t per;    /* TODO: define per-frame PER; not valid until then */
} hh_mac_frame_metrics_t;

/* Handed to MAC logic for an own slot. */
typedef struct {
    hh_frame_kind_t kind;
    hh_node_id_t    src;
    hh_node_id_t    dst;
    uint16_t        len;
    uint8_t         payload[HH_MAC_PAYLOAD_MAX];
} hh_mac_tx_frame_t;

/* A CRC-good PDU from MAC logic, any destination. */
typedef struct {
    hh_frame_kind_t kind;
    hh_node_id_t    src;
    hh_node_id_t    dst;
    uint16_t        len;
    const uint8_t  *payload;
    hh_mac_metric_t rssi;
    hh_mac_metric_t snr;
    uint32_t        superframe;
    bool            superframe_valid;
} hh_mac_rx_pdu_t;

/* What Receive (collect) returns. */
typedef struct {
    hh_frame_kind_t        kind;
    hh_node_id_t           src;
    hh_node_id_t           dst;
    uint16_t               len;
    uint8_t                payload[HH_MAC_PAYLOAD_MAX];
    hh_mac_frame_metrics_t metrics;
    uint32_t               superframe;   /* receive time */
    bool                   superframe_valid;
} hh_mac_rx_frame_t;

/* Link metrics for one neighbour.
 * TODO: averaging method; until then the averaged values stay not valid. */
typedef struct {
    hh_node_id_t    neighbor;
    hh_mac_metric_t rssi;
    hh_mac_metric_t snr;
    hh_mac_metric_t per;
    uint32_t        last_heard_superframe;
    bool            last_heard_valid;
    uint64_t        rx_count;   /* all CRC-good PDUs from it, any destination */
} hh_mac_link_report_t;

typedef struct {
    bool           operational;
    bool           operational_known;    /* TODO: flag not defined outside STEADY, HOLDOVER, OFF, SAFE */
    hh_mac_state_t state;
    uint64_t       frames_tx;           /* handed to logic */
    uint64_t       frames_rx;           /* delivered by collect */
    uint32_t       queue_depth[HH_MAC_CLASS_COUNT];
    uint64_t       queue_full[HH_MAC_CLASS_COUNT];       /* TRY_AGAIN */
    uint64_t       close_discards[HH_MAC_CLASS_COUNT];
    uint64_t       refused_state;
    uint64_t       refused_too_large;
    uint64_t       refused_invalid;
    uint64_t       rx_overflow;          /* newest frame dropped */
    uint64_t       rx_not_addressed;
    uint64_t       rx_not_delivered_state;
    uint64_t       rx_crc_fail;
    uint64_t       neighbor_table_full;
} hh_mac_status_t;

typedef struct {
    hh_node_id_t node_id;
    uint16_t     max_payload;                        /* <= HH_MAC_PAYLOAD_MAX */
    uint16_t     queue_depth[HH_MAC_CLASS_COUNT];    /* frames, <= HH_MAC_QUEUE_CAP */
    uint16_t     rx_buffer_depth;                    /* <= HH_MAC_RXBUF_CAP */
    /* Starvation limit: after this many control frames in a row, one waiting
     * data frame goes next. 0 = strict priority. */
    uint16_t     data_share_n;
    /* Deliver received frames outside STEADY and HOLDOVER.
     * TODO: decide the default. */
    bool         rx_deliver_all_states;
} hh_mac_config_t;

typedef struct {
    hh_mac_tx_frame_t frames[HH_MAC_QUEUE_CAP];
    uint16_t          head;
    uint16_t          count;
} hh_mac_queue_t;

typedef struct {
    hh_node_id_t id;
    bool         used;
    uint32_t     last_heard_superframe;
    bool         last_heard_valid;
    uint64_t     rx_count;
} hh_mac_neighbor_t;

typedef struct {
    hh_mac_config_t   cfg;
    bool              initialized;
    bool              opened;
    hh_mac_state_t    state;
    hh_mac_queue_t    queue[HH_MAC_CLASS_COUNT];
    uint16_t          control_run;
    hh_mac_rx_frame_t rx[HH_MAC_RXBUF_CAP];
    uint16_t          rx_head;
    uint16_t          rx_count;
    hh_mac_neighbor_t neighbors[HH_MAC_NEIGHBOR_CAP];
    hh_mac_status_t   st;
} hh_mac_t;

/* Defaults: payload 512, control 16, data 64, rx 16, strict priority. */
void hh_mac_config_default(hh_mac_config_t *cfg, hh_node_id_t node_id);

hh_mac_result_t hh_mac_init(hh_mac_t *mac, const hh_mac_config_t *cfg);

/* Network layer operations (note section 6). */
hh_mac_result_t hh_mac_open(hh_mac_t *mac);
hh_mac_result_t hh_mac_close(hh_mac_t *mac);
hh_mac_result_t hh_mac_send(hh_mac_t *mac, hh_frame_kind_t kind, hh_node_id_t dst,
                            const uint8_t *payload, uint16_t len);
/* One frame per call; *got is false when nothing is waiting. */
hh_mac_result_t hh_mac_collect(hh_mac_t *mac, hh_mac_rx_frame_t *out, bool *got);
hh_mac_result_t hh_mac_status(const hh_mac_t *mac, hh_mac_status_t *out);
hh_mac_result_t hh_mac_link_metrics(const hh_mac_t *mac, hh_node_id_t neighbor,
                                    hh_mac_link_report_t *out);
hh_mac_result_t hh_mac_channel_select(hh_mac_t *mac, uint32_t channel);

/* MAC logic side. */
bool hh_mac_next_pdu(hh_mac_t *mac, hh_mac_tx_frame_t *out);
void hh_mac_on_rx(hh_mac_t *mac, const hh_mac_rx_pdu_t *pdu);
void hh_mac_on_crc_fail(hh_mac_t *mac);
void hh_mac_set_state(hh_mac_t *mac, hh_mac_state_t state);

hh_mac_class_t hh_mac_class_of(hh_frame_kind_t kind);
const char *hh_mac_result_str(hh_mac_result_t r);
const char *hh_mac_state_str(hh_mac_state_t s);

#endif /* HHSDR_MAC_MAC_H */
