/* Slot-timed multi-node network. See slot_net.h. TEST INFRASTRUCTURE ONLY. */
#include "slot_net.h"
#include "hhsdr/core/log.h"
#include <stdlib.h>
#include <string.h>

#define EPOCH_MS   1000u
#define APP_MAGIC  0xA7u
#define MAX_PKTS   16384u
#define MAX_EVENTS 32u

typedef enum {
    PK_INFLIGHT = 0, PK_DELIVERED, PK_QUEUE_DROP, PK_SCHED_REJECT,
    PK_COLLISION, PK_CHANNEL, PK_CORRUPT, PK_DEADLINE
} pk_state_t;

typedef struct {
    uint8_t  flow;
    uint8_t  state;
    uint8_t  hops;
    uint32_t gen_ms;
    uint32_t enq_ms;
    uint32_t done_ms;
} pkt_t;

typedef struct {
    uint32_t        at_ms;
    sn_event_kind_t kind;
    unsigned        a, b;
    bool            done;
} event_t;

typedef struct {
    slot_net_t   *net;
    hh_node_id_t  id;
    unsigned      slot;
    hh_mac_t      mac;
    hh_slot_t     sl;
    bool          txd;
    hh_slot_pos_t pos;
    size_t        pdu_len;
    uint8_t       pdu[HH_PDU_MAX_LEN];
    uint32_t      next_hello, next_tc;
} node_t;

struct slot_net {
    sn_config_t cfg;
    hh_time_ms_t now;
    hh_clock_t  clock;
    node_t     *nodes;
    bool        link[SN_MAX_NODES][SN_MAX_NODES];
    uint16_t    next[SN_MAX_NODES][SN_MAX_NODES];   /* next[n][d]: next hop node, 0 none */
    sn_flow_t   flows[SN_MAX_FLOWS];
    uint32_t    next_gen[SN_MAX_FLOWS];
    unsigned    n_flows;
    pkt_t      *pkts;
    uint32_t    n_pkts;
    event_t     events[MAX_EVENTS];
    unsigned    n_events;
    uint32_t    rng;
    uint32_t    gen_ms, run_ms;
    uint64_t    pdus, tx_outside, collision_ev, channel_ev, corrupt_ev, route_changes, routing_fail;
};

#define LOGF(net, ...) do { if ((net)->cfg.log) fprintf((net)->cfg.log, __VA_ARGS__); } while (0)

/* Clock callback: returns the network virtual time. */
static hh_time_ms_t clock_now(void *ctx) { return ((slot_net_t *)ctx)->now; }

/* Returns the next pseudo-random number from the seeded generator. */
static uint32_t rnd(slot_net_t *n)
{
    uint32_t x = n->rng;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    n->rng = x ? x : 1u;
    return n->rng;
}

/* Returns the virtual time since the start of the run, in ms. */
static uint32_t t_rel(const slot_net_t *n) { return (uint32_t)(n->now - EPOCH_MS); }

/* ---- application packets ---- */

/* Writes a 32-bit value, low byte first, into an application packet. */
static void put32(uint8_t *p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i)); }
/* Reads a 32-bit value, low byte first, from an application packet. */
static uint32_t get32(const uint8_t *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }

/* Sets the final state of an in-flight packet once. Later events do not change it. */
static void mark(slot_net_t *n, uint32_t id, pk_state_t st)
{
    if (id < n->n_pkts && n->pkts[id].state == PK_INFLIGHT) {
        n->pkts[id].state = (uint8_t)st;
        n->pkts[id].done_ms = t_rel(n);
    }
}

/* Marks the application packets in tx's PDU that are addressed to rx. */
static void lose_for(slot_net_t *n, const node_t *tx, unsigned rx_id, pk_state_t st)
{
    hh_pdu_t p;
    if (hh_pdu_decode(tx->pdu, tx->pdu_len, &p) != HH_OK) return;
    for (unsigned i = 0; i < p.n_msg; i++) {
        const hh_pdu_msg_t *m = &p.msg[i];
        if (m->proto == HH_PDU_PROTO_OPAQUE && m->dst == rx_id && m->len >= SN_APP_HDR &&
            m->data[0] == APP_MAGIC)
            mark(n, get32(m->data + 4), st);
    }
}

/* Hands an application packet to the MAC and records it as lost if the MAC refuses it. */
static void send_app(node_t *nd, uint32_t id, const uint8_t *payload, uint16_t len, unsigned next)
{
    slot_net_t *n = nd->net;
    hh_mac_result_t r = hh_mac_send(&nd->mac, HH_FRAME_DATA, HH_MAC_CLASS_DATA, next, 0, payload, len);
    if (r == HH_MAC_ACCEPTED) return;
    mark(n, id, r == HH_MAC_TRY_AGAIN ? PK_QUEUE_DROP : PK_SCHED_REJECT);
    LOGF(n, "t=%u ev=send_refused node=%u pkt=%u result=%s\n", t_rel(n), (unsigned)nd->id, id,
         hh_mac_result_str(r));
}

/* MAC receive callback: counts a packet as delivered at its destination, or forwards it to the next hop. */
static bool on_mac_rx(void *ctx, const hh_mac_indication_t *ind)
{
    node_t *nd = ctx;
    slot_net_t *n = nd->net;
    const hh_mac_rx_frame_t *f;
    uint32_t id;
    unsigned fin, next;
    uint8_t buf[HH_MAC_PAYLOAD_MAX];

    if (ind->type != HH_MAC_IND_FRAME) return true;
    f = ind->frame;
    if (f->kind != HH_FRAME_DATA || f->len < SN_APP_HDR || f->payload[0] != APP_MAGIC) return true;
    id = get32(f->payload + 4);
    fin = f->payload[8] | (f->payload[9] << 8);
    if (id >= n->n_pkts) return true;
    if (fin == nd->id) {
        if (n->pkts[id].state == PK_INFLIGHT) {
            n->pkts[id].state = PK_DELIVERED;
            n->pkts[id].done_ms = t_rel(n);
            n->pkts[id].hops = (uint8_t)(f->payload[2] + 1);
        }
        return true;
    }
    next = fin <= n->cfg.nodes ? n->next[nd->id - 1][fin - 1] : 0;
    if (!next) { mark(n, id, PK_SCHED_REJECT); return true; }
    memcpy(buf, f->payload, f->len);
    buf[2]++;
    send_app(nd, id, buf, f->len, next);
    return true;
}

/* ---- air ---- */

/* Scheduler transmit callback: keeps the PDU for this slot, logs it and calls the capture hook. */
static bool on_tx(void *ctx, const hh_slot_pos_t *pos, const uint8_t *pdu, size_t len)
{
    node_t *nd = ctx;
    slot_net_t *n = nd->net;
    hh_pdu_t p;
    size_t routing = 0;

    p.n_msg = 0;
    memcpy(nd->pdu, pdu, len);
    nd->pdu_len = len;
    nd->pos = *pos;
    nd->txd = true;
    n->pdus++;
    if (pos->slot != nd->slot) n->tx_outside++;
    if (hh_pdu_decode(pdu, len, &p) == HH_OK)
        for (unsigned i = 0; i < p.n_msg; i++)
            if (p.msg[i].proto == HH_PDU_PROTO_ROUTING) routing += hh_pdu_msg_size(&p.msg[i]);
    if (n->cfg.pdu_hook) n->cfg.pdu_hook(n->cfg.hook_ctx, t_rel(n), pdu, len);
    LOGF(n, "t=%u ev=slot_tx node=%u sf=%u frame=%u slot=%u bytes=%zu msgs=%u routing=%zu capacity=%u\n",
         t_rel(n), (unsigned)nd->id, pos->superframe, pos->frame, pos->slot, len, p.n_msg, routing,
         n->cfg.capacity);
    return true;
}

/* Ends a slot: delivers each PDU to the nodes that hear it, applying collision, loss and corruption. */
static void resolve(slot_net_t *n)
{
    for (unsigned r = 0; r < n->cfg.nodes; r++) {
        unsigned heard[SN_MAX_NODES], nh = 0;
        node_t *rx = &n->nodes[r];
        for (unsigned t = 0; t < n->cfg.nodes; t++)
            if (t != r && n->nodes[t].txd && n->link[t][r]) heard[nh++] = t;
        if (!nh) continue;
        if (rx->txd || nh >= 2) {
            for (unsigned i = 0; i < nh; i++) {
                lose_for(n, &n->nodes[heard[i]], rx->id, PK_COLLISION);
                n->collision_ev++;
                LOGF(n, "t=%u ev=rx_loss reason=collision rx=%u tx=%u slot=%u\n", t_rel(n),
                     (unsigned)rx->id, (unsigned)n->nodes[heard[i]].id, n->nodes[heard[i]].pos.slot);
            }
            continue;
        }
        {
            node_t *tx = &n->nodes[heard[0]];
            uint8_t buf[HH_PDU_MAX_LEN];
            if (n->cfg.loss_permille && rnd(n) % 1000u < n->cfg.loss_permille) {
                lose_for(n, tx, rx->id, PK_CHANNEL);
                n->channel_ev++;
                LOGF(n, "t=%u ev=rx_loss reason=channel rx=%u tx=%u\n", t_rel(n), (unsigned)rx->id,
                     (unsigned)tx->id);
                continue;
            }
            memcpy(buf, tx->pdu, tx->pdu_len);
            if (n->cfg.corrupt_permille && rnd(n) % 1000u < n->cfg.corrupt_permille) {
                lose_for(n, tx, rx->id, PK_CORRUPT);
                n->corrupt_ev++;
                buf[tx->pdu_len / 2] ^= 0x01;
                LOGF(n, "t=%u ev=rx_loss reason=corrupt rx=%u tx=%u\n", t_rel(n), (unsigned)rx->id,
                     (unsigned)tx->id);
            }
            hh_slot_receive(&rx->sl, &tx->pos, buf, tx->pdu_len);
        }
    }
    /* Unicast to a neighbour that cannot hear the sender: link down. */
    for (unsigned t = 0; t < n->cfg.nodes; t++) {
        hh_pdu_t p;
        if (!n->nodes[t].txd || hh_pdu_decode(n->nodes[t].pdu, n->nodes[t].pdu_len, &p) != HH_OK) continue;
        for (unsigned i = 0; i < p.n_msg; i++) {
            const hh_pdu_msg_t *m = &p.msg[i];
            if (m->proto == HH_PDU_PROTO_OPAQUE && m->dst >= 1 && m->dst <= n->cfg.nodes &&
                !n->link[t][m->dst - 1] && m->len >= SN_APP_HDR && m->data[0] == APP_MAGIC) {
                mark(n, get32(m->data + 4), PK_CHANNEL);
                n->channel_ev++;
                LOGF(n, "t=%u ev=rx_loss reason=link_down rx=%u tx=%u\n", t_rel(n), (unsigned)m->dst,
                     (unsigned)n->nodes[t].id);
            }
        }
    }
    for (unsigned t = 0; t < n->cfg.nodes; t++) n->nodes[t].txd = false;
}

/* ---- routes ---- */

/* Recomputes shortest-path next hops over the links that are up and logs each change. */
void sn_compute_routes(slot_net_t *n)
{
    for (unsigned d = 0; d < n->cfg.nodes; d++) {
        int dist[SN_MAX_NODES], q[SN_MAX_NODES], qh = 0, qt = 0;
        uint16_t nh[SN_MAX_NODES];
        for (unsigned i = 0; i < n->cfg.nodes; i++) { dist[i] = -1; nh[i] = 0; }
        dist[d] = 0;
        q[qt++] = (int)d;
        while (qh < qt) {
            int u = q[qh++];
            for (unsigned v = 0; v < n->cfg.nodes; v++)
                if (n->link[u][v] && dist[v] < 0) {
                    dist[v] = dist[u] + 1;
                    nh[v] = (uint16_t)(u + 1);   /* v reaches d through u */
                    q[qt++] = (int)v;
                }
        }
        for (unsigned s = 0; s < n->cfg.nodes; s++) {
            if (s == d || n->next[s][d] == nh[s]) continue;
            LOGF(n, "t=%u ev=route_change node=%u dst=%u old=%u new=%u\n", t_rel(n), s + 1u, d + 1u,
                 (unsigned)n->next[s][d], (unsigned)nh[s]);
            n->next[s][d] = nh[s];
            n->route_changes++;
        }
    }
}

/* ---- setup ---- */

/* Fills the configuration with defaults for the given number of nodes. */
void sn_config_default(sn_config_t *c, uint16_t nodes)
{
    memset(c, 0, sizeof *c);
    c->nodes = nodes;
    c->slots_per_frame = 20;
    c->frames_per_superframe = 50;
    c->slot_ms = 1;
    c->capacity = 128;
    c->net_id = 0x0010;
    c->seed = 1;
    c->hello_interval_ms = 2000;   /* RFC 6130 HELLO_INTERVAL default */
    c->hello_bytes = 40;           /* TODO: placeholder size */
    c->tc_interval_ms = 5000;      /* RFC 7181 TC_INTERVAL default */
    c->tc_bytes = 40;              /* TODO: placeholder size */
}

/* Creates the network with its nodes, MACs and schedulers. Returns NULL if the configuration is invalid. */
slot_net_t *sn_create(const sn_config_t *cfg)
{
    slot_net_t *n;
    if (!cfg || cfg->nodes < 2 || cfg->nodes > SN_MAX_NODES || cfg->nodes > cfg->slots_per_frame ||
        cfg->slots_per_frame > HH_SLOT_FRAME_MAX)
        return NULL;
    n = calloc(1, sizeof *n);
    if (!n) return NULL;
    n->nodes = calloc(cfg->nodes, sizeof *n->nodes);
    n->pkts = calloc(MAX_PKTS, sizeof *n->pkts);
    if (!n->nodes || !n->pkts) { sn_destroy(n); return NULL; }
    n->cfg = *cfg;
    n->now = EPOCH_MS;
    n->clock.now_ms = clock_now;
    n->clock.ctx = n;
    n->rng = cfg->seed ? cfg->seed : 1u;
    hh_log_set_level(HH_LOG_ERROR);

    for (unsigned i = 0; i < cfg->nodes; i++) {
        node_t *nd = &n->nodes[i];
        hh_mac_config_t mc;
        hh_slot_config_t sc;
        nd->net = n;
        nd->id = i + 1u;
        nd->slot = i;
        hh_mac_config_default(&mc, nd->id, &n->clock);
        mc.receive = on_mac_rx;
        mc.receive_ctx = nd;
        mc.report_period_sf = 0;
        if (hh_mac_init(&nd->mac, &mc) != HH_MAC_OK || hh_mac_open(&nd->mac) != HH_MAC_OK) { sn_destroy(n); return NULL; }
        hh_mac_set_state(&nd->mac, HH_MAC_STEADY);
        hh_slot_config_default(&sc, nd->id, cfg->net_id);
        sc.slots_per_frame = cfg->slots_per_frame;
        sc.frames_per_superframe = cfg->frames_per_superframe;
        sc.slot_ms = cfg->slot_ms;
        sc.capacity = cfg->capacity;
        sc.own_slots = 1ull << i;
        sc.epoch_ms = EPOCH_MS;
        if (hh_slot_init(&nd->sl, &sc, &n->clock, &nd->mac, on_tx, nd) != HH_OK) { sn_destroy(n); return NULL; }
        nd->next_hello = (uint32_t)((uint64_t)i * cfg->hello_interval_ms / cfg->nodes);
        nd->next_tc = (uint32_t)((uint64_t)i * cfg->tc_interval_ms / cfg->nodes);
    }
    return n;
}

/* Frees the network. */
void sn_destroy(slot_net_t *n)
{
    if (!n) return;
    free(n->nodes);
    free(n->pkts);
    free(n);
}

/* Returns the MAC of a node, or NULL for an invalid node number. */
hh_mac_t *sn_mac(slot_net_t *n, unsigned node)
{
    return n && node >= 1 && node <= n->cfg.nodes ? &n->nodes[node - 1].mac : NULL;
}

/* Sets whether two nodes can hear each other, in both directions. */
void sn_link(slot_net_t *n, unsigned a, unsigned b, bool up)
{
    if (a < 1 || b < 1 || a > n->cfg.nodes || b > n->cfg.nodes || a == b) return;
    n->link[a - 1][b - 1] = n->link[b - 1][a - 1] = up;
    LOGF(n, "t=%u ev=link a=%u b=%u up=%d\n", t_rel(n), a, b, up ? 1 : 0);
}

/* Links the nodes in a line: 1-2, 2-3 and so on. */
void sn_topology_line(slot_net_t *n) { for (unsigned i = 1; i < n->cfg.nodes; i++) sn_link(n, i, i + 1, true); }

/* Links the nodes in a line and closes it into a ring. */
void sn_topology_ring(slot_net_t *n)
{
    sn_topology_line(n);
    if (n->cfg.nodes > 2) sn_link(n, n->cfg.nodes, 1, true);
}

/* Links every pair of nodes. */
void sn_topology_mesh(slot_net_t *n)
{
    for (unsigned i = 1; i <= n->cfg.nodes; i++)
        for (unsigned j = i + 1; j <= n->cfg.nodes; j++) sn_link(n, i, j, true);
}

/* Gives a node a different own slot. */
void sn_set_slot(slot_net_t *n, unsigned node, unsigned slot)
{
    node_t *nd;
    if (node < 1 || node > n->cfg.nodes || slot >= n->cfg.slots_per_frame) return;
    nd = &n->nodes[node - 1];
    nd->slot = slot;
    nd->sl.cfg.own_slots = 1ull << slot;
}

/* Adds a traffic flow. Returns false if it is invalid or the table is full. */
bool sn_add_flow(slot_net_t *n, const sn_flow_t *f)
{
    if (!f || n->n_flows == SN_MAX_FLOWS || f->src < 1 || f->dst < 1 || f->src > n->cfg.nodes ||
        f->dst > n->cfg.nodes || f->src == f->dst || f->size < SN_APP_HDR || !f->interval_ms)
        return false;
    n->flows[n->n_flows] = *f;
    n->next_gen[n->n_flows] = f->start_ms;
    n->n_flows++;
    return true;
}

/* Schedules a link change or a route recompute at a time after the start of the run. */
bool sn_schedule(slot_net_t *n, uint32_t at_ms, sn_event_kind_t kind, unsigned a, unsigned b)
{
    if (n->n_events == MAX_EVENTS) return false;
    n->events[n->n_events++] = (event_t){ at_ms, kind, a, b, false };
    return true;
}

/* ---- run ---- */

/* Creates and sends the application packets of every flow that is due. */
static void generate(slot_net_t *n)
{
    uint32_t t = t_rel(n);
    for (unsigned f = 0; f < n->n_flows; f++) {
        const sn_flow_t *fl = &n->flows[f];
        while (t >= n->next_gen[f] && t < n->gen_ms) {
            uint8_t buf[HH_MAC_PAYLOAD_MAX];
            uint32_t id = n->n_pkts;
            unsigned next = n->next[fl->src - 1][fl->dst - 1];
            n->next_gen[f] += fl->interval_ms;
            if (id == MAX_PKTS) return;
            memset(buf, 0x5A, fl->size > sizeof buf ? sizeof buf : fl->size);
            buf[0] = APP_MAGIC; buf[1] = (uint8_t)f; buf[2] = 0; buf[3] = 0;
            put32(buf + 4, id);
            buf[8] = (uint8_t)fl->dst; buf[9] = 0;
            n->pkts[id] = (pkt_t){ .flow = (uint8_t)f, .state = PK_INFLIGHT, .gen_ms = t, .enq_ms = t };
            n->n_pkts++;
            if (!next) { mark(n, id, PK_SCHED_REJECT); continue; }
            send_app(&n->nodes[fl->src - 1], id, buf, fl->size, next);
        }
    }
}

/* Sends the stand-in HELLO and TC messages that are due. */
static void routing_messages(slot_net_t *n)
{
    uint32_t t = t_rel(n);
    for (unsigned i = 0; i < n->cfg.nodes; i++) {
        node_t *nd = &n->nodes[i];
        uint8_t buf[HH_MAC_PAYLOAD_MAX] = { 0 };
        if (n->cfg.hello_interval_ms && t >= nd->next_hello) {
            nd->next_hello += n->cfg.hello_interval_ms;
            if (hh_mac_send(&nd->mac, HH_FRAME_ROUTING, HH_MAC_CLASS_CONTROL, HH_MAC_BROADCAST, 0, buf,
                            n->cfg.hello_bytes) != HH_MAC_ACCEPTED) n->routing_fail++;
        }
        if (n->cfg.tc_interval_ms && t >= nd->next_tc) {
            nd->next_tc += n->cfg.tc_interval_ms;
            if (hh_mac_send(&nd->mac, HH_FRAME_ROUTING, HH_MAC_CLASS_CONTROL, HH_MAC_BROADCAST, 0, buf,
                            n->cfg.tc_bytes) != HH_MAC_ACCEPTED) n->routing_fail++;
        }
    }
}

/* Applies the scheduled events that are due. */
static void apply_events(slot_net_t *n)
{
    for (unsigned i = 0; i < n->n_events; i++) {
        event_t *e = &n->events[i];
        if (e->done || t_rel(n) < e->at_ms) continue;
        e->done = true;
        if (e->kind == SN_EV_REROUTE) sn_compute_routes(n);
        else sn_link(n, e->a, e->b, e->kind == SN_EV_LINK_UP);
    }
}

/* Runs traffic for gen_ms, then drains for drain_ms, one slot at a time. */
void sn_run(slot_net_t *n, uint32_t gen_ms, uint32_t drain_ms)
{
    uint32_t total = gen_ms + drain_ms;
    n->gen_ms = gen_ms;
    n->run_ms = total;
    n->now = EPOCH_MS;
    if (n->route_changes == 0) sn_compute_routes(n);
    for (uint32_t t = 0; t < total; t += n->cfg.slot_ms) {
        n->now = EPOCH_MS + t;
        apply_events(n);
        generate(n);
        routing_messages(n);
        for (unsigned i = 0; i < n->cfg.nodes; i++) hh_slot_poll(&n->nodes[i].sl);
        resolve(n);
    }
    for (uint32_t i = 0; i < n->n_pkts; i++)
        if (n->pkts[i].state == PK_INFLIGHT) n->pkts[i].state = PK_DEADLINE;
}

/* ---- results ---- */

/* Orders two 32-bit values for qsort. */
static int cmp_u32(const void *a, const void *b)
{
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return (x > y) - (x < y);
}

/* Sorts the values and returns minimum, median, 95th percentile and maximum. */
static void percentiles(uint32_t *v, uint32_t n, uint32_t out[4])
{
    if (!n) { memset(out, 0, 4 * sizeof out[0]); return; }
    qsort(v, n, sizeof v[0], cmp_u32);
    out[0] = v[0];
    out[1] = v[n / 2];
    out[2] = v[(n * 95u + 99u) / 100u - 1u];
    out[3] = v[n - 1];
}

/* Counts packets by final state and computes delays, for one flow or all flows. */
static void summarize(const slot_net_t *n, int flow, sn_flow_result_t *r)
{
    uint32_t *mac = malloc((n->n_pkts + 1u) * sizeof *mac);
    uint32_t *app = malloc((n->n_pkts + 1u) * sizeof *app);
    uint32_t k = 0;

    memset(r, 0, sizeof *r);
    if (!mac || !app) { free(mac); free(app); return; }
    for (uint32_t i = 0; i < n->n_pkts; i++) {
        const pkt_t *p = &n->pkts[i];
        if (flow >= 0 && p->flow != flow) continue;
        r->sent++;
        switch (p->state) {
        case PK_DELIVERED:
            r->delivered++;
            r->delivered_bytes += n->flows[p->flow].size;
            mac[k] = p->done_ms - p->enq_ms;
            app[k] = p->done_ms - p->gen_ms;
            k++;
            if (p->hops > r->hops_max) r->hops_max = p->hops;
            break;
        case PK_QUEUE_DROP:   r->queue_drop++; break;
        case PK_SCHED_REJECT: r->sched_reject++; break;
        case PK_COLLISION:    r->collision++; break;
        case PK_CHANNEL:      r->channel++; break;
        case PK_CORRUPT:      r->corruption++; break;
        default:              r->deadline++; break;
        }
    }
    percentiles(mac, k, r->mac_delay_ms);
    percentiles(app, k, r->app_delay_ms);
    free(mac);
    free(app);
}

/* Fills out with the results of the last run. */
void sn_result(const slot_net_t *n, sn_result_t *out)
{
    memset(out, 0, sizeof *out);
    out->n_flows = n->n_flows;
    out->run_ms = n->run_ms;
    out->gen_ms = n->gen_ms;
    summarize(n, -1, &out->total);
    for (unsigned f = 0; f < n->n_flows; f++) summarize(n, (int)f, &out->flow[f]);
    out->pdus = n->pdus;
    out->tx_outside_own_slot = n->tx_outside;
    out->collision_events = n->collision_ev;
    out->channel_events = n->channel_ev;
    out->corrupt_events = n->corrupt_ev;
    out->route_changes = n->route_changes;
    out->routing_send_failed = n->routing_fail;
    for (unsigned i = 0; i < n->cfg.nodes; i++) {
        const uint64_t *src = (const uint64_t *)&n->nodes[i].sl.st;
        uint64_t *dst = (uint64_t *)&out->slot;
        for (size_t k = 0; k < sizeof out->slot / sizeof(uint64_t); k++) dst[k] += src[k];
    }
}
