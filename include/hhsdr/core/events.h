/*
 * Typed event definitions — the control plane's inter-component contract.
 *
 * Every event here corresponds to a numbered interface in the HW/SW Interface
 * Specification (HTI-nn) and carries that interface's field list. Where the
 * spec marks a field list "Recommended" rather than "Defined", the fields are
 * taken from the spec's own recommendation, not invented here.
 */
#ifndef HHSDR_CORE_EVENTS_H
#define HHSDR_CORE_EVENTS_H

#include "hhsdr/core/types.h"

typedef enum {
    HH_EV_NONE = 0,
    HH_EV_BEACON_RX,          /* HTI-04  Radio      -> Discovery              */
    HH_EV_LINK_SAMPLE,        /* HTI-05  Radio/LMP  -> Link Health            */
    HH_EV_NEIGHBOR_UP,        /* HTI-06  Neighbor   -> Topo/Route/LinkHealth  */
    HH_EV_NEIGHBOR_DOWN,      /* HTI-06                                       */
    HH_EV_NEIGHBOR_CHANGED,   /* HTI-06                                       */
    HH_EV_LINK_STATE_CHANGED, /* HTI-07  LinkHealth -> FD/Route/Topo/Mgmt     */
    HH_EV_ROUTE_INSTALLED,    /* HTI-08  Routing    -> Forwarder/Topo         */
    HH_EV_ROUTE_WITHDRAWN,    /* HTI-09  Routing    -> Forwarder/Topo         */
    HH_EV_FAILURE_DETECTED,   /* HTI-10  FD         -> SelfHealing/Routing    */
    HH_EV_RECOVERY_STARTED,   /* HTI-11  SelfHealing-> Routing/Topo/Mgmt      */
    HH_EV_RECOVERY_COMPLETED, /* HTI-11                                       */
    HH_EV_PARTITION_DETECTED, /* HTI-12  Topology   -> SelfHealing/Mgmt       */
    HH_EV_NETWORK_MERGED,     /* HTI-13  Topology   -> SelfHealing/Route/Mgmt */
    HH_EV_CADENCE_HINT,       /* HTI-16  LinkHealth -> Discovery              */
    HH_EV__MAX
} hh_event_type_t;

const char *hh_event_type_str(hh_event_type_t t);

/* ---- HTI-04: DiscoveryBeacon (field list Defined in Doc 1 §4) ---- */
typedef struct {
    hh_node_id_t node_id;
    uint16_t     protocol_version;
    hh_seq_t     sequence_no;
    hh_time_ms_t timestamp;
    uint32_t     capabilities;         /* HH_CAP_* bit-field; encoding TBD    */
    uint32_t     radio_caps;           /* encoding TBD                        */
    float        channel_freq;
    uint32_t     supported_waveforms;  /* encoding TBD                        */
    bool         routing_capable;
    /* Optional fields — must degrade gracefully when absent (Doc 1 §4). */
    bool  position_valid;
    float position_x, position_y, position_z;
    bool  power_valid;
    float power_battery;
} hh_beacon_t;

/* Metrics observed by the receiver for the frame that carried a beacon.
 * Reported by the radio adapter alongside the beacon; not part of the wire
 * format, since a sender cannot measure the receiver's RSSI. */
typedef struct {
    hh_node_id_t neighbor_id;
    float    rssi;            /* dBm; units TBD (HTI spec §12 item 2)         */
    float    snr;             /* dB;  units TBD                               */
    float    per;             /* 0..1 packet error rate; units TBD            */
    uint32_t retransmit_count;
    uint32_t phy_errors;      /* CRC/decode errors, distinct from MAC loss    */
    bool     ack_success;     /* data-plane ACK outcome, if known             */
    bool     ack_valid;       /* false when the adapter cannot report ACKs    */
    float    latency_ms;      /* latency trend input                          */
    bool     latency_valid;
} hh_link_sample_t;

typedef struct { hh_beacon_t beacon; hh_link_sample_t sample; } hh_ev_beacon_rx_t;

typedef struct {
    hh_node_id_t neighbor_id;
    hh_time_ms_t timestamp;
    uint32_t     capabilities;
    uint32_t     radio_caps;
} hh_ev_neighbor_t;   /* NeighborUp / NeighborDown */

#define HH_NBR_ATTR_CAPABILITIES (1u << 0)
#define HH_NBR_ATTR_METRICS      (1u << 1)
#define HH_NBR_ATTR_RADIO_CAPS   (1u << 2)

typedef struct {
    hh_node_id_t neighbor_id;
    uint32_t     changed_attributes;   /* HH_NBR_ATTR_* bit-field */
} hh_ev_neighbor_changed_t;

/* ---- HTI-07: LinkStateChanged (signature Defined in Doc 1 §7) ---- */
typedef struct {
    hh_node_id_t    neighbor;
    hh_link_state_t old_state;
    hh_link_state_t new_state;
    hh_cause_hint_t cause_hint;
} hh_ev_link_state_t;

/* ---- HTI-08 / HTI-09 ---- */
typedef struct {
    hh_node_id_t destination;
    hh_node_id_t next_hop;
    float        metric;        /* composite (Doc 1 §6): lower is better */
    hh_seq_t     sequence_no;
    uint8_t      hop_count;
} hh_ev_route_installed_t;

typedef enum {
    HH_WITHDRAW_EXPIRED = 0,
    HH_WITHDRAW_FAILURE_CASCADE,
    HH_WITHDRAW_EXPLICIT
} hh_withdraw_reason_t;

const char *hh_withdraw_reason_str(hh_withdraw_reason_t r);

typedef struct {
    hh_node_id_t         destination;
    hh_node_id_t         next_hop;
    hh_withdraw_reason_t reason;
} hh_ev_route_withdrawn_t;

/* ---- HTI-10 ---- */
typedef struct {
    hh_node_id_t    neighbor_or_node_id;
    hh_cause_hint_t cause_hint;
    hh_time_ms_t    confirmation_time;
} hh_ev_failure_t;

/* ---- HTI-11 ---- */
typedef enum {
    HH_RECOVERY_ALTERNATE_ROUTE = 0,
    HH_RECOVERY_REDISCOVERY,
    HH_RECOVERY_CHANNEL_CHANGE   /* HTI-14 path, for an RF-interference cause */
} hh_recovery_strategy_t;

const char *hh_recovery_strategy_str(hh_recovery_strategy_t s);

typedef struct {
    hh_node_id_t           target;
    hh_recovery_strategy_t strategy;
    bool                   hold_down_active;
    bool                   succeeded;      /* RecoveryCompleted only */
} hh_ev_recovery_t;

/* ---- HTI-12 / HTI-13 ---- */
typedef struct {
    hh_node_id_t branch_root;   /* representation TBD; a neighbor branch root */
    uint32_t     unreachable_count;
    hh_time_ms_t detected_at;
} hh_ev_partition_t;

typedef struct {
    hh_node_id_t rejoined_neighbor;
    hh_time_ms_t hold_down_until;
} hh_ev_merged_t;

/* ---- HTI-16 ---- */
typedef struct {
    bool  unstable;             /* true tightens cadence, false relaxes it */
    float instability;          /* 0..1 fraction of neighbors not Healthy  */
} hh_ev_cadence_hint_t;

/* Tagged union carried by the dispatcher. Fixed size and copied by value:
 * no subscriber can free or mutate another subscriber's copy. */
typedef struct {
    hh_event_type_t type;
    hh_time_ms_t    timestamp;
    union {
        hh_ev_beacon_rx_t        beacon_rx;
        hh_link_sample_t         link_sample;
        hh_ev_neighbor_t         neighbor;
        hh_ev_neighbor_changed_t neighbor_changed;
        hh_ev_link_state_t       link_state;
        hh_ev_route_installed_t  route_installed;
        hh_ev_route_withdrawn_t  route_withdrawn;
        hh_ev_failure_t          failure;
        hh_ev_recovery_t         recovery;
        hh_ev_partition_t        partition;
        hh_ev_merged_t           merged;
        hh_ev_cadence_hint_t     cadence;
    } u;
} hh_event_t;

#endif /* HHSDR_CORE_EVENTS_H */
