/*
 * Slot-timed multi-node network for tests and tools. TEST INFRASTRUCTURE ONLY.
 *
 * Each node is a real hh_mac_t plus hh_slot_t. The air is simulated: a PDU
 * sent in a slot reaches every node that can hear the sender, in the same
 * slot. A receiver loses the PDU when it hears two or more senders in that
 * slot, or when it is sending itself. Routing is a stand-in: shortest-path
 * next hops over the links that are up, plus periodic routing-sized broadcast
 * messages. It is not OLSRd2.
 *
 * Every node owns slot (id - 1) unless sn_set_slot() says otherwise.
 * The clock is virtual and advances one slot per step.
 */
#ifndef HH_SLOT_NET_H
#define HH_SLOT_NET_H

#include "hhsdr/mac/slot.h"
#include <stdio.h>

#define SN_MAX_NODES 32u
#define SN_MAX_FLOWS 16u
#define SN_APP_HDR   12u   /* smallest application packet */

typedef struct {
    uint16_t nodes;
    uint16_t slots_per_frame;
    uint16_t frames_per_superframe;
    uint16_t slot_ms;
    uint16_t capacity;
    uint16_t net_id;
    uint32_t loss_permille;      /* every link, per PDU */
    uint32_t corrupt_permille;   /* PDU arrives with a bad CRC */
    uint32_t seed;
    uint32_t hello_interval_ms;  /* routing stand-in; 0 = off */
    uint16_t hello_bytes;
    uint32_t tc_interval_ms;     /* 0 = off */
    uint16_t tc_bytes;
    FILE    *log;                /* key=value lines, NULL = none */
    /* Called for every PDU sent, with the run time in ms. NULL = none. */
    void   (*pdu_hook)(void *ctx, uint32_t t_ms, const uint8_t *pdu, size_t len);
    void    *hook_ctx;
} sn_config_t;

typedef struct {
    uint8_t  src, dst;           /* node numbers, 1 to nodes */
    uint16_t size;               /* application packet bytes */
    uint32_t interval_ms;
    uint32_t start_ms;           /* after the start of the run */
} sn_flow_t;

typedef enum { SN_EV_LINK_DOWN = 0, SN_EV_LINK_UP, SN_EV_REROUTE } sn_event_kind_t;

typedef struct {
    uint32_t sent;               /* packets handed to the network */
    uint32_t delivered;
    uint64_t delivered_bytes;
    uint32_t queue_drop;
    uint32_t sched_reject;
    uint32_t collision;
    uint32_t channel;
    uint32_t corruption;
    uint32_t deadline;
    uint32_t mac_delay_ms[4];    /* min, median, p95, max */
    uint32_t app_delay_ms[4];
    uint8_t  hops_max;
} sn_flow_result_t;

typedef struct {
    sn_flow_result_t total;
    sn_flow_result_t flow[SN_MAX_FLOWS];
    unsigned         n_flows;
    uint32_t         run_ms;
    uint32_t         gen_ms;
    uint64_t         pdus;
    uint64_t         tx_outside_own_slot;
    uint64_t         collision_events;   /* PDU lost at one receiver */
    uint64_t         channel_events;
    uint64_t         corrupt_events;
    uint64_t         route_changes;
    uint64_t         routing_send_failed;
    hh_slot_stats_t  slot;               /* summed over nodes */
} sn_result_t;

typedef struct slot_net slot_net_t;

void sn_config_default(sn_config_t *cfg, uint16_t nodes);
slot_net_t *sn_create(const sn_config_t *cfg);   /* NULL on invalid configuration */
void sn_destroy(slot_net_t *net);

void sn_link(slot_net_t *net, unsigned a, unsigned b, bool up);
void sn_topology_line(slot_net_t *net);
void sn_topology_ring(slot_net_t *net);
void sn_topology_mesh(slot_net_t *net);
void sn_set_slot(slot_net_t *net, unsigned node, unsigned slot);
void sn_compute_routes(slot_net_t *net);   /* logs every next hop that changed */
bool sn_add_flow(slot_net_t *net, const sn_flow_t *flow);
bool sn_schedule(slot_net_t *net, uint32_t at_ms, sn_event_kind_t kind, unsigned a, unsigned b);

/* Generate traffic for gen_ms, stop it, then drain for drain_ms. */
void sn_run(slot_net_t *net, uint32_t gen_ms, uint32_t drain_ms);
void sn_result(const slot_net_t *net, sn_result_t *out);

hh_mac_t *sn_mac(slot_net_t *net, unsigned node);

#endif /* HH_SLOT_NET_H */
