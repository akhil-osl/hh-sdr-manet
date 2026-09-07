/*
 * Deterministic multi-node network simulator — TEST INFRASTRUCTURE ONLY.
 *
 * Lives entirely under tests/ and is never linked into the production library.
 * It drives the REAL production stack: each virtual node is a genuine hh_node_t
 * with a mock radio bound to the same hh_radio_ops_t contract the FPGA adapter
 * will implement. There is no second networking implementation anywhere.
 *
 * Determinism: all timing is virtual, and loss uses a seeded xorshift PRNG, so a
 * scenario replays identically every run. That is what makes assertions about
 * convergence timing and failure/recovery behavior meaningful rather than flaky.
 *
 * Models: virtual links with per-link loss and delay, controlled RSSI/SNR/PER,
 * beacon loss, node failure, link failure and recovery, partitions, merges,
 * mobility, and asymmetric links.
 */
#ifndef HH_NETSIM_H
#define HH_NETSIM_H

#include "hhsdr/manet/node.h"
#include "mock_radio.h"
#include "vclock.h"

#define SIM_MAX_NODES 12

typedef struct {
    bool  up;              /* link exists at all                        */
    float loss;            /* 0..1 probability a frame is dropped       */
    uint32_t delay_ms;     /* propagation/queueing delay                */
    float rssi;            /* reported to the receiver                  */
    float snr;
    float per;
    uint32_t phy_errors;   /* CRC/decode errors reported with the frame */
    bool  ack_valid;
    bool  ack_success;
} sim_link_t;

typedef struct {
    hh_node_id_t id;
    bool         alive;         /* false models a powered-off node      */
    hh_node_t    node;
    mock_radio_t mock;
    hh_radio_t   radio;
    hh_config_t  cfg;
    bool         used;
} sim_node_t;

typedef struct {
    vclock_t   vc;
    sim_node_t nodes[SIM_MAX_NODES];
    size_t     node_count;
    /* Directed links: [from][to], so asymmetry is expressible. */
    sim_link_t links[SIM_MAX_NODES][SIM_MAX_NODES];
    uint64_t   rng;
    uint64_t   frames_delivered;
    uint64_t   frames_dropped;
} netsim_t;

void netsim_init(netsim_t *s, uint64_t seed);

/* Add a node running the real production stack. Returns its index. */
int  netsim_add_node(netsim_t *s, hh_node_id_t id);
int  netsim_add_node_cfg(netsim_t *s, hh_node_id_t id, const hh_config_t *cfg);

sim_node_t *netsim_node(netsim_t *s, hh_node_id_t id);

/* Topology control. All links start down; a scenario declares its own. */
void netsim_link_up(netsim_t *s, hh_node_id_t a, hh_node_id_t b, float rssi);
void netsim_link_down(netsim_t *s, hh_node_id_t a, hh_node_id_t b);
void netsim_link_set_loss(netsim_t *s, hh_node_id_t a, hh_node_id_t b, float loss);
void netsim_link_set_delay(netsim_t *s, hh_node_id_t a, hh_node_id_t b, uint32_t ms);
void netsim_link_set_quality(netsim_t *s, hh_node_id_t a, hh_node_id_t b,
                             float rssi, float snr, float per);
void netsim_link_set_phy_errors(netsim_t *s, hh_node_id_t a, hh_node_id_t b, uint32_t n);
/* Directed: models an asymmetric link (beacons land one way only). */
void netsim_link_up_directed(netsim_t *s, hh_node_id_t from, hh_node_id_t to, float rssi);
void netsim_link_down_directed(netsim_t *s, hh_node_id_t from, hh_node_id_t to);

/* Fault injection. */
void netsim_node_fail(netsim_t *s, hh_node_id_t id);      /* radio goes dark */
void netsim_node_recover(netsim_t *s, hh_node_id_t id);
void netsim_partition(netsim_t *s, const hh_node_id_t *group_a, size_t na,
                      const hh_node_id_t *group_b, size_t nb);
void netsim_merge(netsim_t *s, const hh_node_id_t *group_a, size_t na,
                  const hh_node_id_t *group_b, size_t nb, float rssi);

/* Start every node (SCA configure + start). */
hh_status_t netsim_start_all(netsim_t *s);

/* Advance virtual time by step_ms, ticking every node and moving frames. */
void netsim_run(netsim_t *s, uint32_t duration_ms, uint32_t step_ms);

/* Assertion helpers. */
bool   netsim_has_route(netsim_t *s, hh_node_id_t from, hh_node_id_t to);
hh_node_id_t netsim_next_hop(netsim_t *s, hh_node_id_t from, hh_node_id_t to);
bool   netsim_is_neighbor(netsim_t *s, hh_node_id_t from, hh_node_id_t to);
size_t netsim_route_count(netsim_t *s, hh_node_id_t id);

#endif /* HH_NETSIM_H */
