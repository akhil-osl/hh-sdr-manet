/*
 * Network-interface adapter: moves payloads between a network interface
 * (Linux manet0) and hh_radio_ops_t. OLSRd2 owns routing, Linux owns IP
 * forwarding; this only sends to a given next hop and delivers what arrives.
 *
 * Not decided, so not chosen here (unknown.md U-24): node-id mapping, IP, ARP,
 * MTU, and which frame kind each traffic type uses. Kind and next hop are
 * passed through unjudged (MAC-NETWORK-INTERFACE section 7.1, DP-11).
 *
 * Do not share a radio with the legacy stack; both use the same frame kinds.
 */
#ifndef HHSDR_NETIF_NETIF_H
#define HHSDR_NETIF_NETIF_H

#include "hhsdr/radio/radio.h"

#define HH_NETIF_BROADCAST HH_NODE_ID_INVALID

/* One radio frame, no fragmentation. Not a decided MTU. */
#define HH_NETIF_MAX_PAYLOAD HH_RADIO_MAX_FRAME

/* Called from hh_netif_poll() per received frame. from is the one-hop
 * sender, not necessarily the originator. payload and metrics are valid
 * only during the call; copy them to keep them. */
typedef void (*hh_netif_deliver_fn)(hh_frame_kind_t kind,
                                    const uint8_t *payload, uint16_t len,
                                    hh_node_id_t from,
                                    const hh_link_sample_t *metrics,
                                    void *ctx);

typedef struct {
    uint64_t tx_frames;
    uint64_t tx_broadcast;
    uint64_t tx_bytes;
    uint64_t tx_errors;          /* radio refused */
    uint64_t tx_rejected;        /* refused before the radio */
    uint64_t rx_frames;
    uint64_t rx_bytes;
    uint64_t rx_dropped_no_sink;
    uint64_t rx_dropped_closed;
    uint64_t rx_dropped_malformed; /* unknown kind or len too large */
} hh_netif_counters_t;

typedef struct {
    hh_node_id_t        self;
    hh_radio_t         *radio;
    bool                opened;
    hh_netif_deliver_fn deliver;
    void               *deliver_ctx;
    hh_netif_counters_t counters;
} hh_netif_t;

hh_status_t hh_netif_init(hh_netif_t *nif, hh_node_id_t self, hh_radio_t *radio);
void hh_netif_set_deliver(hh_netif_t *nif, hh_netif_deliver_fn fn, void *ctx);
hh_status_t hh_netif_open(hh_netif_t *nif);
hh_status_t hh_netif_close(hh_netif_t *nif);

/* payload is copied before return. HH_OK means the radio accepted it, not
 * that it was delivered. Radio errors pass through unchanged. */
hh_status_t hh_netif_send(hh_netif_t *nif, hh_frame_kind_t kind,
                          hh_node_id_t next_hop,
                          const uint8_t *payload, uint16_t len);

hh_status_t hh_netif_poll(hh_netif_t *nif, hh_time_ms_t now);

const hh_netif_counters_t *hh_netif_counters(const hh_netif_t *nif);

#endif /* HHSDR_NETIF_NETIF_H */
