/*
 * MAC service for the network layer, as in the MAC to network layer
 * interface specification v1.1 (docs/MAC-NETWORK-INTERFACE.md). Names follow
 * the specification.
 *
 * The lower side (MAC logic: slots, CRC, RF) is reached only through the
 * hh_mac_next_pdu(), hh_mac_on_*() and hh_mac_set_*() calls below. Its real
 * binding is not defined yet.
 *
 * Single-threaded, no dynamic allocation.
 */
#ifndef HHSDR_MAC_MAC_H
#define HHSDR_MAC_MAC_H

#include "hhsdr/core/types.h"
#include "hhsdr/core/clock.h"
#include "hhsdr/radio/radio.h"

/* Compile-time caps. Configured values must not exceed them. */
#define HH_MAC_PAYLOAD_MAX   1532  /* larger target of OI-2; default is 1280 */
#define HH_MAC_TX_POOL_CAP   256   /* frames across all transmit queues */
#define HH_MAC_RX_POOL_CAP   128   /* frames across all holding buffers */
#define HH_MAC_EVENT_CAP     32
#define HH_MAC_NEIGHBOR_CAP  32

/* Node id values, section 8.5. */
#define HH_MAC_BROADCAST     ((hh_node_id_t)0u)
#define HH_MAC_GROUP_FIRST   ((hh_node_id_t)0xF0000000u)
#define HH_MAC_GROUP_LAST    ((hh_node_id_t)0xFFFFFFFEu)
#define HH_MAC_ID_RESERVED   ((hh_node_id_t)0xFFFFFFFFu)

typedef enum {
    HH_MAC_OK = 0,          /* success for calls other than Send */
    HH_MAC_ACCEPTED,        /* Send: queued, not transmitted */
    HH_MAC_TRY_AGAIN,
    HH_MAC_WRONG_STATE,
    HH_MAC_TX_INHIBITED,
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

/* Values and order as section 8.5: highest priority first. */
typedef enum {
    HH_MAC_CLASS_CONTROL = 0,
    HH_MAC_CLASS_EMERGENCY,
    HH_MAC_CLASS_VOICE,
    HH_MAC_CLASS_VIDEO,
    HH_MAC_CLASS_DATA,
    HH_MAC_CLASS_COUNT
} hh_mac_class_t;

typedef enum {
    HH_MAC_INHIBIT_NONE = 0,
    HH_MAC_INHIBIT_SULK,
    HH_MAC_INHIBIT_ZEROISE,
    HH_MAC_INHIBIT_FAULT,
    HH_MAC_INHIBIT_SYNC_LOSS,
    HH_MAC_INHIBIT_HOLDOVER_LIMIT
} hh_mac_inhibit_t;

typedef enum {
    HH_MAC_TIME_GNSS = 0,
    HH_MAC_TIME_EXTERNAL_GNSS,
    HH_MAC_TIME_NETWORK,
    HH_MAC_TIME_HOLDOVER
} hh_mac_time_source_t;

typedef enum {
    HH_MAC_ENTRY_RETRIES_EXHAUSTED = 0,
    HH_MAC_ENTRY_NOT_AUTHORISED
} hh_mac_entry_fail_t;

/* Per-frame metrics, section 8.2. Levels in 0.25 dB steps (RSSI in dBm).
 * A value has no meaning unless its valid bit is set. */
#define HH_MAC_M_RSSI_VALID       0x01u
#define HH_MAC_M_SNR_VALID        0x02u
#define HH_MAC_M_RX_GAIN_VALID    0x04u
#define HH_MAC_M_ARRIVAL_VALID    0x08u
#define HH_MAC_M_RSSI_CALIBRATED  0x10u

typedef struct {
    uint8_t flags;
    int16_t rssi_qdb;
    int16_t snr_qdb;
    int16_t rx_gain_qdb;
    int32_t arrival_offset_ns;
} hh_mac_frame_metrics_t;

/* Handed to MAC logic for an own slot. */
typedef struct {
    hh_frame_kind_t kind;
    hh_mac_class_t  cls;
    hh_node_id_t    src;
    hh_node_id_t    dst;
    uint16_t        len;
    uint8_t         payload[HH_MAC_PAYLOAD_MAX];
} hh_mac_tx_frame_t;

/* A CRC-good PDU from MAC logic, any destination. */
typedef struct {
    hh_frame_kind_t        kind;
    hh_mac_class_t         cls;
    hh_node_id_t           src;
    hh_node_id_t           dst;
    uint32_t               superframe;   /* receive time, net time */
    hh_mac_frame_metrics_t metrics;
    uint16_t               len;
    const uint8_t         *payload;
} hh_mac_rx_pdu_t;

/* A received frame as delivered to the network layer, section 8.1. */
typedef struct {
    hh_frame_kind_t        kind;
    hh_mac_class_t         cls;
    hh_node_id_t           src;
    hh_node_id_t           dst;
    uint32_t               superframe;
    hh_mac_frame_metrics_t metrics;
    uint16_t               len;
    uint8_t                payload[HH_MAC_PAYLOAD_MAX];
} hh_mac_rx_frame_t;

/* Per-neighbour record, section 12.2. Valid bits in flags. */
#define HH_MAC_L_RSSI_VALID        0x0001u
#define HH_MAC_L_SNR_VALID         0x0002u
#define HH_MAC_L_PER_FROM_VALID    0x0004u
#define HH_MAC_L_PER_TO_VALID      0x0008u
#define HH_MAC_L_RATE_TX_VALID     0x0010u
#define HH_MAC_L_RATE_RX_VALID     0x0020u
#define HH_MAC_L_MAX_RATE_TX_VALID 0x0040u
#define HH_MAC_L_MAX_RATE_RX_VALID 0x0080u
#define HH_MAC_L_LATENCY_VALID     0x0100u
#define HH_MAC_L_CRC_FAIL_VALID    0x0200u

typedef struct {
    hh_node_id_t neighbor;
    uint16_t     flags;
    int16_t      rssi_qdb;          /* averaged; TODO: averaging method (DP-6) */
    int16_t      snr_qdb;           /* averaged; TODO: averaging method (DP-6) */
    uint16_t     per_from;          /* 0.01 %; TODO: needs slot ownership (OI-3) */
    uint16_t     per_to;            /* 0.01 %; TODO: needs MAC control fields (OI-2) */
    uint32_t     rate_tx_bps;       /* towards the neighbour */
    uint32_t     rate_rx_bps;       /* from the neighbour */
    uint32_t     max_rate_tx_bps;
    uint32_t     max_rate_rx_bps;
    uint32_t     latency;           /* TODO: units */
    uint32_t     last_heard_superframe;
    uint32_t     rx_count;          /* all CRC-good PDUs from it, any destination */
    uint32_t     crc_fail_count;    /* TODO: needs slot ownership (OI-3) */
} hh_mac_link_report_t;

/* Indications, section 6.8. */
typedef enum {
    HH_MAC_EV_NEIGHBOUR_HEARD = 0,
    HH_MAC_EV_NEIGHBOUR_SILENT,     /* TODO: not raised until slot ownership is known (OI-3) */
    HH_MAC_EV_LINK_REPORT,
    HH_MAC_EV_STATE_CHANGED,
    HH_MAC_EV_TX_INHIBIT_ON,
    HH_MAC_EV_TX_INHIBIT_OFF,
    HH_MAC_EV_BREAK_IN_ON,
    HH_MAC_EV_BREAK_IN_OFF,
    HH_MAC_EV_OWN_SLOTS_CHANGED,
    HH_MAC_EV_PARAMETERS_CHANGED,
    HH_MAC_EV_ENTRY_FAILED
} hh_mac_event_kind_t;

typedef struct {
    hh_mac_event_kind_t  kind;
    hh_node_id_t         node;
    uint32_t             superframe;
    /* New state, inhibit reason, slot count, max payload or entry-fail reason. */
    uint32_t             detail;
    hh_mac_link_report_t report;   /* LINK_REPORT only */
} hh_mac_event_t;

typedef enum {
    HH_MAC_IND_FRAME = 0,
    HH_MAC_IND_EVENT
} hh_mac_ind_type_t;

typedef struct {
    hh_mac_ind_type_t      type;
    const hh_mac_rx_frame_t *frame;   /* FRAME only */
    const hh_mac_event_t    *event;   /* EVENT only */
} hh_mac_indication_t;

/* Receive indication. The network layer only enqueues and returns. Return
 * false when it cannot take it now: the MAC keeps it and retries on the next
 * delivery or on hh_mac_flush(). */
typedef bool (*hh_mac_receive_fn)(void *ctx, const hh_mac_indication_t *ind);

typedef struct {
    bool                 operational;
    bool                 operational_known;   /* TODO: flag outside STEADY, HOLDOVER, OFF, SAFE (DP-9) */
    hh_mac_state_t       state;
    hh_mac_time_source_t time_source;
    bool                 time_source_valid;
    uint32_t             time_in_holdover_ms;
    hh_mac_inhibit_t     tx_inhibit_reason;
    uint16_t             max_payload;
    hh_node_id_t         node_id;
    uint16_t             own_slots_per_frame;
    bool                 own_slots_valid;
    /* TODO: sync quality (OI-7), TX power and range profile (DP-13), slot and
     * frame length (OI-1), schedule version (OI-3), net preset (OI-4). */
    uint32_t             restart_count;
    uint32_t             frames_tx;           /* handed to logic */
    uint32_t             frames_rx;           /* delivered to the network layer */
    uint32_t             queue_depth[HH_MAC_CLASS_COUNT];
    uint32_t             queue_full[HH_MAC_CLASS_COUNT];       /* TRY_AGAIN */
    uint32_t             drop_oldest[HH_MAC_CLASS_COUNT];      /* voice, video */
    uint32_t             lifetime_expired[HH_MAC_CLASS_COUNT];
    uint32_t             close_discards[HH_MAC_CLASS_COUNT];
    uint32_t             refused_state;
    uint32_t             refused_inhibited;
    uint32_t             refused_too_large;
    uint32_t             refused_invalid;
    uint32_t             tx_underrun;
    uint32_t             tx_inhibit;          /* slots not sent */
    uint32_t             rx_crc_fail;
    uint32_t             rx_not_addressed;
    uint32_t             rx_not_delivered_state;
    uint32_t             hold_depth[HH_MAC_CLASS_COUNT];
    uint32_t             hold_overflow[HH_MAC_CLASS_COUNT];    /* oldest dropped */
    uint32_t             hold_close_discards;
    uint32_t             delivery_refused;
    uint32_t             event_overflow;
    uint32_t             neighbor_table_full;
} hh_mac_status_t;

typedef struct {
    hh_node_id_t      node_id;
    const hh_clock_t *clock;
    uint16_t          max_payload;                       /* <= HH_MAC_PAYLOAD_MAX */
    uint16_t          queue_depth[HH_MAC_CLASS_COUNT];   /* frames; sum <= HH_MAC_TX_POOL_CAP */
    /* TODO: holding buffer depth per class (DP-5). Sum <= HH_MAC_RX_POOL_CAP. */
    uint16_t          hold_depth[HH_MAC_CLASS_COUNT];
    uint16_t          report_period_sf;                  /* LINK_REPORT period; 0 = off */
    /* Control cap, a token bucket (section 10.4). TODO: values (DP-10).
     * A rate of 0 turns that limit off. */
    uint16_t          ctl_cap_pdus_per_s;
    uint16_t          ctl_burst_pdus;
    uint32_t          ctl_cap_bytes_per_s;
    uint32_t          ctl_burst_bytes;
    /* Minimum share: when user traffic is queued, at least one own slot in N
     * carries a user frame. 0 = off. TODO: value (DP-10). */
    uint16_t          min_share_n;
    hh_mac_receive_fn receive;
    void             *receive_ctx;
} hh_mac_config_t;

/* Internal state below. Callers allocate hh_mac_t but do not touch it. */
typedef struct {
    hh_mac_tx_frame_t f;
    hh_time_ms_t      expires;   /* 0 = no lifetime */
} hh_mac_tx_entry_t;

typedef struct {
    hh_mac_rx_frame_t f;
    uint32_t          seq;
} hh_mac_rx_entry_t;

typedef struct {
    hh_mac_event_t ev;
    uint32_t       seq;
} hh_mac_ev_entry_t;

typedef struct {
    uint16_t idx[HH_MAC_TX_POOL_CAP];
    uint16_t head;
    uint16_t count;
} hh_mac_ring_t;

typedef struct {
    hh_node_id_t id;
    bool         used;
    uint32_t     last_heard_superframe;
    uint32_t     rx_count;
} hh_mac_neighbor_t;

typedef struct {
    hh_mac_config_t   cfg;
    bool              initialized;
    bool              opened;
    hh_mac_state_t    state;
    hh_time_ms_t      state_since;
    hh_mac_inhibit_t  inhibit;
    uint16_t          max_payload;
    uint32_t          superframe;
    uint16_t          sf_since_report;

    hh_mac_tx_entry_t tx_pool[HH_MAC_TX_POOL_CAP];
    uint16_t          tx_free[HH_MAC_TX_POOL_CAP];
    uint16_t          tx_free_count;
    hh_mac_ring_t     txq[HH_MAC_CLASS_COUNT];

    uint64_t          ctl_pdu_tokens;    /* thousandths of a PDU */
    uint64_t          ctl_byte_tokens;   /* thousandths of a byte */
    hh_time_ms_t      ctl_refill_at;
    uint16_t          slots_without_user;

    hh_mac_rx_entry_t rx_pool[HH_MAC_RX_POOL_CAP];
    uint16_t          rx_free[HH_MAC_RX_POOL_CAP];
    uint16_t          rx_free_count;
    hh_mac_ring_t     hold[HH_MAC_CLASS_COUNT];
    hh_mac_ev_entry_t events[HH_MAC_EVENT_CAP];
    uint16_t          ev_head;
    uint16_t          ev_count;
    uint32_t          seq;
    bool              delivering;

    hh_mac_neighbor_t neighbors[HH_MAC_NEIGHBOR_CAP];
    hh_mac_status_t   st;
} hh_mac_t;

/* Defaults: payload 1280; depths control 16, emergency 8, voice 8, video 32,
 * data 64; holding buffer 16 per class; link report every superframe;
 * control cap and minimum share off. */
void hh_mac_config_default(hh_mac_config_t *cfg, hh_node_id_t node_id,
                           const hh_clock_t *clock);

/* Writes the configuration; the MAC is OFF afterwards. */
hh_mac_result_t hh_mac_init(hh_mac_t *mac, const hh_mac_config_t *cfg);

/* Network layer operations (section 6). */
hh_mac_result_t hh_mac_open(hh_mac_t *mac);
hh_mac_result_t hh_mac_close(hh_mac_t *mac);
/* lifetime_ms 0 = no limit. Beacon and routing kinds are forced to control. */
hh_mac_result_t hh_mac_send(hh_mac_t *mac, hh_frame_kind_t kind, hh_mac_class_t cls,
                            hh_node_id_t dst, uint16_t lifetime_ms,
                            const uint8_t *payload, uint16_t len);
hh_mac_result_t hh_mac_status(const hh_mac_t *mac, hh_mac_status_t *out);
hh_mac_result_t hh_mac_link_metrics(const hh_mac_t *mac, hh_node_id_t neighbor,
                                    hh_mac_link_report_t *out);
hh_mac_result_t hh_mac_net_preset_select(hh_mac_t *mac, uint32_t preset);
/* Retry held deliveries after the network layer refused one. */
void hh_mac_flush(hh_mac_t *mac);

/* MAC logic side. One call fills one own slot with one frame.
 * TODO: several frames per slot needs the sub-frame header (OI-2). */
bool hh_mac_next_pdu(hh_mac_t *mac, hh_mac_tx_frame_t *out);
void hh_mac_on_rx(hh_mac_t *mac, const hh_mac_rx_pdu_t *pdu);
void hh_mac_on_crc_fail(hh_mac_t *mac);
void hh_mac_on_tx_underrun(hh_mac_t *mac);
void hh_mac_on_tx_inhibited_slot(hh_mac_t *mac);
void hh_mac_on_superframe(hh_mac_t *mac, uint32_t superframe);
void hh_mac_on_break_in(hh_mac_t *mac, hh_node_id_t node, bool on);
void hh_mac_on_entry_failed(hh_mac_t *mac, hh_mac_entry_fail_t reason);
/* A MAC reset: restart count goes up, state goes to WAIT_TIME. */
void hh_mac_on_reset(hh_mac_t *mac);
void hh_mac_set_state(hh_mac_t *mac, hh_mac_state_t state);
void hh_mac_set_tx_inhibit(hh_mac_t *mac, hh_mac_inhibit_t reason);
void hh_mac_set_own_slots(hh_mac_t *mac, uint16_t own_slots_per_frame);
void hh_mac_set_time_source(hh_mac_t *mac, hh_mac_time_source_t src);
hh_mac_result_t hh_mac_set_max_payload(hh_mac_t *mac, uint16_t max_payload);

const char *hh_mac_result_str(hh_mac_result_t r);
const char *hh_mac_state_str(hh_mac_state_t s);
const char *hh_mac_class_str(hh_mac_class_t c);

#endif /* HHSDR_MAC_MAC_H */
