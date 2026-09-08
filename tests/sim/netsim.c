#include "netsim.h"
#include <string.h>

/* Seeded xorshift: deterministic, so a scenario replays identically. */
static uint32_t rng_next(netsim_t *s)
{
    uint64_t x = s->rng;
    x ^= x << 13; x ^= x >> 7; x ^= x << 17;
    s->rng = x;
    return (uint32_t)(x >> 32);
}

static bool rng_drop(netsim_t *s, float loss)
{
    if (loss <= 0.0f) return false;
    if (loss >= 1.0f) return true;
    return (float)(rng_next(s) % 10000u) / 10000.0f < loss;
}

static int index_of(netsim_t *s, hh_node_id_t id)
{
    for (size_t i = 0; i < s->node_count; i++)
        if (s->nodes[i].used && s->nodes[i].id == id) return (int)i;
    return -1;
}

void netsim_init(netsim_t *s, uint64_t seed)
{
    memset(s, 0, sizeof *s);
    vclock_init(&s->vc, 1000);
    s->rng = seed ? seed : 0x9E3779B97F4A7C15ull;
}

int netsim_add_node_cfg(netsim_t *s, hh_node_id_t id, const hh_config_t *cfg)
{
    sim_node_t *n;
    if (s->node_count >= SIM_MAX_NODES) return -1;

    n = &s->nodes[s->node_count];
    memset(n, 0, sizeof *n);
    n->used  = true;
    n->id    = id;
    n->alive = true;
    n->cfg   = *cfg;
    n->cfg.node_id = id;

    mock_radio_init(&n->mock, "sim", &n->radio);
    /* The node is constructed against the abstract radio handle, exactly as it
     * will be against the hardware adapter. */
    if (hh_node_init(&n->node, &n->cfg, &s->vc.clock, &n->radio) != HH_OK) return -1;
    return (int)s->node_count++;
}

int netsim_add_node(netsim_t *s, hh_node_id_t id)
{
    hh_config_t cfg;
    hh_config_defaults(&cfg);
    cfg.node_id = id;
    /* Faster cadences keep scenario runtimes short while preserving the
     * relative ordering of every timer the architecture specifies. */
    cfg.beacon_interval_acq_ms = 100;
    cfg.beacon_interval_ms     = 200;
    cfg.beacon_interval_min_ms = 50;
    cfg.beacon_interval_max_ms = 800;
    cfg.acquisition_timeout_ms = 500;
    cfg.route_update_interval_ms = 200;
    cfg.route_active_timeout_ms  = 4000;
    cfg.route_delete_period_ms   = 2000;
    cfg.lh_suspect_hold_ms = 400;
    cfg.lh_recover_hold_ms = 400;
    cfg.hold_down_ms       = 300;
    cfg.merge_hold_down_ms = 600;
    cfg.log_level = HH_LOG_ERROR;   /* scenarios raise this when debugging */
    return netsim_add_node_cfg(s, id, &cfg);
}

sim_node_t *netsim_node(netsim_t *s, hh_node_id_t id)
{
    int i = index_of(s, id);
    return i < 0 ? NULL : &s->nodes[i];
}

/* ---- topology control ---- */

static void set_link(netsim_t *s, int a, int b, bool up, float rssi)
{
    sim_link_t *l = &s->links[a][b];
    l->up   = up;
    l->rssi = rssi;
    if (up) {
        if (l->snr == 0.0f) l->snr = 25.0f;
        l->per = 0.0f;
        l->phy_errors = 0;
    }
}

void netsim_link_up(netsim_t *s, hh_node_id_t a, hh_node_id_t b, float rssi)
{
    int ia = index_of(s, a), ib = index_of(s, b);
    if (ia < 0 || ib < 0) return;
    set_link(s, ia, ib, true, rssi);
    set_link(s, ib, ia, true, rssi);
}

void netsim_link_down(netsim_t *s, hh_node_id_t a, hh_node_id_t b)
{
    int ia = index_of(s, a), ib = index_of(s, b);
    if (ia < 0 || ib < 0) return;
    s->links[ia][ib].up = false;
    s->links[ib][ia].up = false;
}

void netsim_link_up_directed(netsim_t *s, hh_node_id_t from, hh_node_id_t to, float rssi)
{
    int ia = index_of(s, from), ib = index_of(s, to);
    if (ia < 0 || ib < 0) return;
    set_link(s, ia, ib, true, rssi);
}

void netsim_link_down_directed(netsim_t *s, hh_node_id_t from, hh_node_id_t to)
{
    int ia = index_of(s, from), ib = index_of(s, to);
    if (ia < 0 || ib < 0) return;
    s->links[ia][ib].up = false;
}

void netsim_link_set_loss(netsim_t *s, hh_node_id_t a, hh_node_id_t b, float loss)
{
    int ia = index_of(s, a), ib = index_of(s, b);
    if (ia < 0 || ib < 0) return;
    s->links[ia][ib].loss = loss;
    s->links[ib][ia].loss = loss;
}

void netsim_link_set_delay(netsim_t *s, hh_node_id_t a, hh_node_id_t b, uint32_t ms)
{
    int ia = index_of(s, a), ib = index_of(s, b);
    if (ia < 0 || ib < 0) return;
    s->links[ia][ib].delay_ms = ms;
    s->links[ib][ia].delay_ms = ms;
}

void netsim_link_set_quality(netsim_t *s, hh_node_id_t a, hh_node_id_t b,
                             float rssi, float snr, float per)
{
    int ia = index_of(s, a), ib = index_of(s, b);
    if (ia < 0 || ib < 0) return;
    s->links[ia][ib].rssi = rssi; s->links[ia][ib].snr = snr; s->links[ia][ib].per = per;
    s->links[ib][ia].rssi = rssi; s->links[ib][ia].snr = snr; s->links[ib][ia].per = per;
}

void netsim_link_set_phy_errors(netsim_t *s, hh_node_id_t a, hh_node_id_t b, uint32_t n)
{
    int ia = index_of(s, a), ib = index_of(s, b);
    if (ia < 0 || ib < 0) return;
    s->links[ia][ib].phy_errors = n;
    s->links[ib][ia].phy_errors = n;
}

/* ---- fault injection ---- */

void netsim_node_fail(netsim_t *s, hh_node_id_t id)
{
    sim_node_t *n = netsim_node(s, id);
    if (!n) return;
    n->alive = false;
    /* The radio goes dark: no tx, no rx. From every neighbor's local view this
     * is indistinguishable from the node disappearing, which is the point. */
    n->mock.operational = false;
}

void netsim_node_recover(netsim_t *s, hh_node_id_t id)
{
    sim_node_t *n = netsim_node(s, id);
    if (!n) return;
    n->alive = true;
    n->mock.operational = true;
}

void netsim_partition(netsim_t *s, const hh_node_id_t *ga, size_t na,
                      const hh_node_id_t *gb, size_t nb)
{
    for (size_t i = 0; i < na; i++)
        for (size_t j = 0; j < nb; j++)
            netsim_link_down(s, ga[i], gb[j]);
}

void netsim_merge(netsim_t *s, const hh_node_id_t *ga, size_t na,
                  const hh_node_id_t *gb, size_t nb, float rssi)
{
    for (size_t i = 0; i < na; i++)
        for (size_t j = 0; j < nb; j++)
            netsim_link_up(s, ga[i], gb[j], rssi);
}

/* ---- run loop ---- */

hh_status_t netsim_start_all(netsim_t *s)
{
    for (size_t i = 0; i < s->node_count; i++) {
        sim_node_t *n = &s->nodes[i];
        hh_status_t st;
        if (!n->used) continue;
        st = hh_node_configure(&n->node, NULL);
        if (st != HH_OK) return st;
        st = hh_node_start(&n->node);
        if (st != HH_OK) return st;
    }
    return HH_OK;
}

/*
 * Move frames each node transmitted into the receive queues of nodes reachable
 * over an up link, applying that link's loss, delay, and reported metrics.
 * This is the only place the simulator touches the stack, and it does so
 * through the same rx path the hardware adapter will use.
 */
static void deliver_frames(netsim_t *s, hh_time_ms_t now)
{
    for (size_t i = 0; i < s->node_count; i++) {
        sim_node_t *tx = &s->nodes[i];
        size_t pending;
        if (!tx->used) continue;

        pending = tx->mock.tx_count;
        for (size_t k = 0; k < pending; k++) {
            const hh_frame_t *f = &tx->mock.tx_log[k];

            for (size_t j = 0; j < s->node_count; j++) {
                sim_node_t *rx = &s->nodes[j];
                sim_link_t *l = &s->links[i][j];
                hh_link_sample_t m;

                if (i == j || !rx->used || !rx->alive) continue;
                if (!l->up) continue;
                /* Unicast frames only reach the addressed next hop. */
                if (f->dst != HH_NODE_ID_INVALID && f->dst != rx->id) continue;
                if (rng_drop(s, l->loss)) {
                    s->frames_dropped++;
                    if (s->on_frame) s->on_frame(tx->id, rx->id, f, false, now,
                                                 s->on_frame_ctx);
                    continue;
                }

                memset(&m, 0, sizeof m);
                m.neighbor_id = tx->id;
                m.rssi = l->rssi;
                m.snr  = l->snr;
                m.per  = l->per;
                m.phy_errors = l->phy_errors;
                m.ack_valid   = l->ack_valid;
                m.ack_success = l->ack_success;

                mock_radio_enqueue_rx(&rx->mock, f, &m, now + l->delay_ms);
                s->frames_delivered++;
                if (s->on_frame) s->on_frame(tx->id, rx->id, f, true, now,
                                             s->on_frame_ctx);
            }
        }
        mock_radio_clear_tx_log(&tx->mock);
    }
}

void netsim_set_frame_observer(netsim_t *s, netsim_frame_fn fn, void *ctx)
{
    if (!s) return;
    s->on_frame = fn;
    s->on_frame_ctx = ctx;
}

void netsim_set_step_observer(netsim_t *s, netsim_step_fn fn, void *ctx)
{
    if (!s) return;
    s->on_step = fn;
    s->on_step_ctx = ctx;
}

void netsim_step(netsim_t *s, uint32_t step_ms)
{
    if (!s) return;
    if (step_ms == 0) step_ms = 10;

    for (size_t i = 0; i < s->node_count; i++) {
        sim_node_t *n = &s->nodes[i];
        if (!n->used || !n->alive) continue;
        s->current_node = n->id;
        hh_node_tick(&n->node, s->vc.now);
    }
    s->current_node = HH_NODE_ID_INVALID;
    deliver_frames(s, s->vc.now);
    if (s->on_step) s->on_step(s->vc.now, s->on_step_ctx);
    vclock_advance(&s->vc, step_ms);
}

void netsim_run(netsim_t *s, uint32_t duration_ms, uint32_t step_ms)
{
    uint32_t elapsed = 0;
    if (step_ms == 0) step_ms = 10;

    while (elapsed < duration_ms) {
        netsim_step(s, step_ms);
        elapsed += step_ms;
    }
}

/* ---- assertions ---- */

bool netsim_has_route(netsim_t *s, hh_node_id_t from, hh_node_id_t to)
{
    sim_node_t *n = netsim_node(s, from);
    const hh_route_snapshot_t *snap;
    if (!n) return false;
    snap = hh_routing_snapshot(&n->node.routing);
    return hh_route_lookup(snap, to) != NULL;
}

hh_node_id_t netsim_next_hop(netsim_t *s, hh_node_id_t from, hh_node_id_t to)
{
    sim_node_t *n = netsim_node(s, from);
    const hh_route_entry_t *e;
    if (!n) return HH_NODE_ID_INVALID;
    e = hh_route_lookup(hh_routing_snapshot(&n->node.routing), to);
    return e ? e->next_hop : HH_NODE_ID_INVALID;
}

bool netsim_is_neighbor(netsim_t *s, hh_node_id_t from, hh_node_id_t to)
{
    sim_node_t *n = netsim_node(s, from);
    return n && hh_neighbor_get(&n->node.neighbors, to) != NULL;
}

size_t netsim_route_count(netsim_t *s, hh_node_id_t id)
{
    sim_node_t *n = netsim_node(s, id);
    const hh_route_snapshot_t *snap;
    size_t c = 0;
    if (!n) return 0;
    snap = hh_routing_snapshot(&n->node.routing);
    for (size_t i = 0; i < snap->count; i++) if (snap->entries[i].valid) c++;
    return c;
}
