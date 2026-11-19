/*
 * Multi-node network whose PDUs travel through the frame rings. TEST
 * INFRASTRUCTURE ONLY.
 *
 * Each node is a real hh_mac_t and hh_slot_t, with the MAC-side ring driver on
 * a ring region of its own and a fake PL beside it. At the start of every TDMA
 * frame each fake PL pulls that frame's TX frames from its node's TX ring, and
 * in each slot the frame of the node that owns it reaches every node that can
 * hear it, through that node's RX ring. A receiver loses the frame when it
 * hears two or more senders in a slot, or is sending itself.
 *
 * Every node owns slot (id - 1). The clock is set by the caller, one slot per
 * step in real time or in a loop.
 */
#ifndef HH_RING_NET_H
#define HH_RING_NET_H

#include <stdio.h>

#include "hhsdr/mac/slot.h"

#define RN_MAX_NODES 32u

typedef struct {
    uint16_t nodes;
    uint16_t slots_per_frame;
    uint16_t frames_per_superframe;
    uint16_t slot_ms;
    uint16_t capacity;          /* PDU bytes; also the ring slot size, rounded up to 32 */
    uint16_t net_id;
    uint16_t ring_slots;        /* per ring, power of two */
    uint16_t lead_frames;       /* TDMA frames between building a PDU and sending it */
    uint32_t loss_permille;     /* every link, per PDU */
    uint32_t seed;
    /* Called for every frame put on the air, with the run time in ms. NULL = none. */
    void   (*pdu_hook)(void *ctx, uint32_t t_ms, const uint8_t *pdu, size_t len);
    void    *hook_ctx;
    /* Called for every frame delivered to a node's network layer. node is 1-based. */
    void   (*rx_hook)(void *ctx, unsigned node, const hh_mac_rx_frame_t *frame);
    void    *rx_ctx;
} rn_config_t;

typedef struct {
    uint64_t pdus_on_air;
    uint64_t control_only;       /* own slots the PL filled with a control-only frame */
    uint64_t collision_events;   /* frame lost at one receiver */
    uint64_t channel_events;
    uint64_t tx_ring_full;       /* summed over nodes */
    uint64_t rx_ring_full;
    uint64_t pull_stale;         /* frames dropped by the pull for an earlier target */
    uint64_t rx_to_mac;          /* frames passed from the RX rings to the schedulers */
} rn_stats_t;

typedef struct ring_net ring_net_t;

void rn_config_default(rn_config_t *cfg, uint16_t nodes);
ring_net_t *rn_create(const rn_config_t *cfg);   /* NULL on invalid configuration */
void rn_destroy(ring_net_t *net);

void rn_link(ring_net_t *net, unsigned a, unsigned b, bool up);
void rn_topology_line(ring_net_t *net);
void rn_topology_ring(ring_net_t *net);
void rn_topology_mesh(ring_net_t *net);

/* Hands a frame to a node's MAC, as the network layer above it would. */
hh_mac_result_t rn_send(ring_net_t *net, unsigned node, hh_frame_kind_t kind, hh_mac_class_t cls,
                        hh_node_id_t dst, const uint8_t *payload, uint16_t len);

/* Sets the time since the start of the run, then runs the slot that is due. */
void rn_set_time(ring_net_t *net, uint32_t t_ms);
void rn_step(ring_net_t *net);

hh_mac_t *rn_mac(ring_net_t *net, unsigned node);
const hh_slot_stats_t *rn_slot_stats(const ring_net_t *net, unsigned node);
void rn_stats(const ring_net_t *net, rn_stats_t *out);

#endif /* HH_RING_NET_H */
