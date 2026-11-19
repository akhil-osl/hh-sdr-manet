#include "ring_net.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fake_pl.h"
#include "hhsdr/core/log.h"
#include "hhsdr/ring/mac_driver.h"

#define EPOCH_MS 1000u

typedef struct {
    ring_net_t       *net;
    hh_node_id_t      id;
    unsigned          slot;
    void             *mem;
    hh_ring_shm_t     own_shm, mac_shm, pl_shm;   /* with ring_path_fmt */
    hh_ring_region_t  own_region;
    hh_ring_region_t  region;                     /* the MAC side view */
    hh_ring_region_t  pl_region;                  /* the fake PL view */
    hh_mac_t          mac;
    hh_slot_t         sl;
    hh_ring_mac_t     drv;
    fake_pl_t         pl;
    fake_pl_sent_t    sent;
    bool              sent_valid;
} rnode_t;

struct ring_net {
    rn_config_t  cfg;
    hh_time_ms_t now;
    hh_clock_t   clock;
    rnode_t     *nodes;
    bool         link[RN_MAX_NODES][RN_MAX_NODES];
    bool         started;
    uint64_t     last_frame;
    uint32_t     rng;
    uint64_t     pdus, control_only, collision, channel, rx_to_mac;
};

/* Clock callback: returns the network time. */
static hh_time_ms_t clock_now(void *ctx) { return ((ring_net_t *)ctx)->now; }

/* Next pseudo-random number. */
static uint32_t rnd(ring_net_t *n)
{
    n->rng = n->rng * 1664525u + 1013904223u;
    return n->rng >> 8;
}

/* Run time in ms. */
static uint32_t t_rel(const ring_net_t *n) { return (uint32_t)(n->now - EPOCH_MS); }

/* MAC receive callback: passes frames to the network layer hook. */
static bool on_mac_rx(void *ctx, const hh_mac_indication_t *ind)
{
    rnode_t *nd = ctx;
    if (ind->type == HH_MAC_IND_FRAME && nd->net->cfg.rx_hook)
        nd->net->cfg.rx_hook(nd->net->cfg.rx_ctx, nd->id, ind->frame);
    return true;
}

void rn_config_default(rn_config_t *c, uint16_t nodes)
{
    memset(c, 0, sizeof *c);
    c->nodes = nodes;
    c->slots_per_frame = 20;
    c->frames_per_superframe = 50;
    c->slot_ms = 1;
    c->capacity = 256;
    c->net_id = 0x0010;
    c->ring_slots = 32;
    c->lead_frames = 1;
    c->seed = 1;
}

/* Rounds up to a multiple of 32, the ring slot alignment. */
static uint32_t up32(uint32_t v) { return (v + 31u) & ~31u; }

ring_net_t *rn_create(const rn_config_t *cfg)
{
    ring_net_t *n;
    uint32_t slot_size;
    size_t rsize;

    if (!cfg || cfg->nodes < 2 || cfg->nodes > RN_MAX_NODES || cfg->nodes > cfg->slots_per_frame ||
        cfg->slots_per_frame > HH_SLOT_FRAME_MAX || cfg->capacity > FAKE_PL_MAX_FRAME || cfg->lead_frames == 0)
        return NULL;
    slot_size = up32(cfg->capacity);
    if (cfg->ring_create && !cfg->ring_path_fmt) return NULL;
    rsize = hh_ring_region_size(cfg->ring_slots, cfg->ring_slots, slot_size);
    if (!rsize) return NULL;
    n = calloc(1, sizeof *n);
    if (!n) return NULL;
    n->nodes = calloc(cfg->nodes, sizeof *n->nodes);
    if (!n->nodes) { rn_destroy(n); return NULL; }
    n->cfg = *cfg;
    n->now = EPOCH_MS;
    n->clock.now_ms = clock_now;
    n->clock.ctx = n;
    n->rng = cfg->seed ? cfg->seed : 1u;
    hh_log_set_level(HH_LOG_ERROR);

    for (unsigned i = 0; i < cfg->nodes; i++) {
        rnode_t *nd = &n->nodes[i];
        hh_mac_config_t mc;
        hh_slot_config_t sc;

        nd->net = n;
        nd->id = i + 1u;
        nd->slot = i;
        if (cfg->ring_path_fmt) {
            char p[HH_RING_SHM_PATH_MAX];

            snprintf(p, sizeof p, cfg->ring_path_fmt, (unsigned)nd->id);
            if (cfg->ring_create &&
                hh_ring_shm_create(&nd->own_shm, &nd->own_region, p, cfg->ring_slots, cfg->ring_slots,
                                   slot_size) != HH_OK) { rn_destroy(n); return NULL; }
            if (hh_ring_shm_open(&nd->mac_shm, &nd->region, p) != HH_OK ||
                hh_ring_shm_open(&nd->pl_shm, &nd->pl_region, p) != HH_OK ||
                nd->region.rx.slot_size < cfg->capacity || nd->region.tx.slot_size < cfg->capacity) {
                rn_destroy(n);
                return NULL;
            }
        } else {
            nd->mem = aligned_alloc(HH_RING_ALIGN, up32((uint32_t)rsize));
            if (!nd->mem) { rn_destroy(n); return NULL; }
            if (hh_ring_region_init(nd->mem, rsize, cfg->ring_slots, cfg->ring_slots, slot_size) != HH_OK ||
                hh_ring_region_attach(&nd->region, nd->mem, rsize) != HH_OK) { rn_destroy(n); return NULL; }
            nd->pl_region = nd->region;
        }
        hh_mac_config_default(&mc, nd->id, &n->clock);
        mc.receive = on_mac_rx;
        mc.receive_ctx = nd;
        mc.report_period_sf = 0;
        if (hh_mac_init(&nd->mac, &mc) != HH_MAC_OK || hh_mac_open(&nd->mac) != HH_MAC_OK) { rn_destroy(n); return NULL; }
        hh_mac_set_state(&nd->mac, HH_MAC_STEADY);
        hh_slot_config_default(&sc, nd->id, cfg->net_id);
        sc.slots_per_frame = cfg->slots_per_frame;
        sc.frames_per_superframe = cfg->frames_per_superframe;
        sc.slot_ms = cfg->slot_ms;
        sc.capacity = cfg->capacity;
        sc.own_slots = 1ull << i;
        sc.epoch_ms = EPOCH_MS;
        if (hh_slot_init(&nd->sl, &sc, &n->clock, &nd->mac, hh_ring_mac_tx, &nd->drv) != HH_OK ||
            hh_ring_mac_init(&nd->drv, &nd->region, &nd->sl, cfg->lead_frames) != HH_OK) { rn_destroy(n); return NULL; }
        fake_pl_init(&nd->pl);
    }
    return n;
}

void rn_destroy(ring_net_t *n)
{
    if (!n) return;
    if (n->nodes)
        for (unsigned i = 0; i < n->cfg.nodes; i++) {
            hh_ring_shm_close(&n->nodes[i].mac_shm);
            hh_ring_shm_close(&n->nodes[i].pl_shm);
            hh_ring_shm_close(&n->nodes[i].own_shm);
            free(n->nodes[i].mem);
        }
    free(n->nodes);
    free(n);
}

void rn_link(ring_net_t *n, unsigned a, unsigned b, bool up)
{
    if (!n || a < 1 || b < 1 || a > n->cfg.nodes || b > n->cfg.nodes || a == b) return;
    n->link[a - 1][b - 1] = n->link[b - 1][a - 1] = up;
}

void rn_topology_line(ring_net_t *n) { for (unsigned i = 1; i < n->cfg.nodes; i++) rn_link(n, i, i + 1, true); }

void rn_topology_ring(ring_net_t *n)
{
    rn_topology_line(n);
    if (n->cfg.nodes > 2) rn_link(n, n->cfg.nodes, 1, true);
}

void rn_topology_mesh(ring_net_t *n)
{
    for (unsigned a = 1; a <= n->cfg.nodes; a++)
        for (unsigned b = a + 1; b <= n->cfg.nodes; b++) rn_link(n, a, b, true);
}

hh_mac_result_t rn_send(ring_net_t *n, unsigned node, hh_frame_kind_t kind, hh_mac_class_t cls,
                        hh_node_id_t dst, const uint8_t *payload, uint16_t len)
{
    if (!n || node < 1 || node > n->cfg.nodes) return HH_MAC_INVALID_ARGUMENT;
    return hh_mac_send(&n->nodes[node - 1].mac, kind, cls, dst, 0, payload, len);
}

void rn_set_time(ring_net_t *n, uint32_t t_ms) { n->now = EPOCH_MS + t_ms; }

/* Every PL pulls the TX frames of the TDMA frame that starts now. */
static void pull_frame(ring_net_t *n, uint32_t superframe, uint16_t frame)
{
    for (unsigned i = 0; i < n->cfg.nodes; i++) {
        rnode_t *nd = &n->nodes[i];
        uint16_t own = (uint16_t)nd->slot;
        nd->sent_valid = fake_pl_transmit(&nd->pl, &nd->pl_region, superframe, frame, &own, 1, &nd->sent) == HH_OK;
    }
}

/* Puts the frames of this slot on the air and delivers them through the RX rings. */
static void air(ring_net_t *n, uint32_t superframe, uint16_t slot)
{
    bool txd[RN_MAX_NODES] = { false };

    for (unsigned t = 0; t < n->cfg.nodes; t++) {
        rnode_t *nd = &n->nodes[t];
        if (nd->sent_valid && nd->sent.slot == slot) txd[t] = true;
    }
    for (unsigned r = 0; r < n->cfg.nodes; r++) {
        unsigned heard[RN_MAX_NODES], nh = 0;

        for (unsigned t = 0; t < n->cfg.nodes; t++)
            if (t != r && txd[t] && n->link[t][r]) heard[nh++] = t;
        if (!nh) continue;
        if (txd[r] || nh >= 2) {
            n->collision += nh;
            continue;
        }
        {
            const fake_pl_sent_t *s = &n->nodes[heard[0]].sent;
            hh_ring_rx_desc_t meta;

            if (s->control_only) continue;
            if (n->cfg.loss_permille && rnd(n) % 1000u < n->cfg.loss_permille) {
                n->channel++;
                continue;
            }
            memset(&meta, 0, sizeof meta);
            meta.superframe = superframe;
            meta.slot_in_frame = slot;
            fake_pl_receive(&n->nodes[r].pl, &n->nodes[r].pl_region, s->bytes, s->length, 1, &meta);
        }
    }
    for (unsigned t = 0; t < n->cfg.nodes; t++) {
        rnode_t *nd = &n->nodes[t];
        if (!txd[t]) continue;
        if (nd->sent.control_only) n->control_only++;
        else {
            n->pdus++;
            if (n->cfg.pdu_hook) n->cfg.pdu_hook(n->cfg.hook_ctx, t_rel(n), nd->sent.bytes, nd->sent.length);
        }
        nd->sent_valid = false;
    }
}

void rn_step(ring_net_t *n)
{
    uint64_t per_frame = n->cfg.slots_per_frame;
    uint64_t abs = n->now >= EPOCH_MS ? (n->now - EPOCH_MS) / n->cfg.slot_ms : 0;
    uint64_t frame_abs = abs / per_frame;
    uint32_t superframe = (uint32_t)(frame_abs / n->cfg.frames_per_superframe);
    uint16_t frame = (uint16_t)(frame_abs % n->cfg.frames_per_superframe);

    if (!n->started || frame_abs != n->last_frame) {
        pull_frame(n, superframe, frame);
        n->started = true;
        n->last_frame = frame_abs;
    }
    air(n, superframe, (uint16_t)(abs % per_frame));
    for (unsigned i = 0; i < n->cfg.nodes; i++) n->rx_to_mac += hh_ring_mac_poll(&n->nodes[i].drv);
    for (unsigned i = 0; i < n->cfg.nodes; i++) hh_slot_poll(&n->nodes[i].sl);
}

hh_mac_t *rn_mac(ring_net_t *n, unsigned node)
{
    return n && node >= 1 && node <= n->cfg.nodes ? &n->nodes[node - 1].mac : NULL;
}

const hh_slot_stats_t *rn_slot_stats(const ring_net_t *n, unsigned node)
{
    return n && node >= 1 && node <= n->cfg.nodes ? &n->nodes[node - 1].sl.st : NULL;
}

void rn_stats(const ring_net_t *n, rn_stats_t *o)
{
    memset(o, 0, sizeof *o);
    o->pdus_on_air = n->pdus;
    o->control_only = n->control_only;
    o->collision_events = n->collision;
    o->channel_events = n->channel;
    o->rx_to_mac = n->rx_to_mac;
    for (unsigned i = 0; i < n->cfg.nodes; i++) {
        const rnode_t *nd = &n->nodes[i];
        o->tx_ring_full += nd->drv.st.tx_ring_full;
        o->rx_ring_full += nd->pl.rx_ring_full;
        o->pull_stale += nd->pl.servant.stale_dropped;
    }
}
