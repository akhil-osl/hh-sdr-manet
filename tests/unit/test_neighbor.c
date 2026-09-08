/*
 * Neighbor Manager tests.
 *
 * Verifies the architectural claim that this is the authoritative one-hop
 * source: it is the only writer, its expiry is cadence-based rather than a
 * fixed wall-clock timeout, its table is bounded with LRU eviction, and every
 * transition emits the HTI-06 event downstream components rely on.
 */
#include "hhsdr/manet/neighbor.h"
#include "hhsdr/radio/wire.h"
#include "hh_test.h"
#include "vclock.h"
#include <string.h>

typedef struct {
    hh_config_t       cfg;
    vclock_t          vc;
    hh_dispatcher_t   bus;
    hh_neighbor_mgr_t nm;
} fix_t;

/* Records HTI-06 events so tests assert on published behavior, not internals. */
typedef struct {
    int ups, downs, changes;
    hh_node_id_t last_up, last_down;
    uint32_t last_changed_attrs;
} obs_t;

static void observe(const hh_event_t *ev, void *ctx)
{
    obs_t *o = ctx;
    switch (ev->type) {
    case HH_EV_NEIGHBOR_UP:      o->ups++;   o->last_up = ev->u.neighbor.neighbor_id;   break;
    case HH_EV_NEIGHBOR_DOWN:    o->downs++; o->last_down = ev->u.neighbor.neighbor_id; break;
    case HH_EV_NEIGHBOR_CHANGED: o->changes++;
        o->last_changed_attrs = ev->u.neighbor_changed.changed_attributes; break;
    default: break;
    }
}

static void fix_init(fix_t *f, obs_t *o)
{
    memset(f, 0, sizeof *f);
    memset(o, 0, sizeof *o);
    hh_config_defaults(&f->cfg);
    f->cfg.node_id = 1;
    vclock_init(&f->vc, 1000);
    hh_dispatcher_init(&f->bus);
    hh_dispatcher_subscribe(&f->bus, "obs",
        HH_EV_MASK(HH_EV_NEIGHBOR_UP) | HH_EV_MASK(HH_EV_NEIGHBOR_DOWN) |
        HH_EV_MASK(HH_EV_NEIGHBOR_CHANGED), observe, o);
    hh_neighbor_init(&f->nm, &f->cfg, &f->vc.clock, &f->bus);
}

static hh_beacon_t mk_beacon(hh_node_id_t id, hh_seq_t seq)
{
    hh_beacon_t b;
    memset(&b, 0, sizeof b);
    b.node_id = id;
    b.protocol_version = HH_PROTOCOL_VERSION;
    b.sequence_no = seq;
    b.capabilities = HH_CAP_ROUTING_CAPABLE;
    b.routing_capable = true;
    return b;
}

static hh_link_sample_t mk_sample(hh_node_id_t id, float rssi)
{
    hh_link_sample_t s;
    memset(&s, 0, sizeof s);
    s.neighbor_id = id; s.rssi = rssi; s.snr = 20.0f;
    return s;
}

static void test_first_beacon_creates_neighbor_and_emits_up(void)
{
    fix_t f; obs_t o; fix_init(&f, &o);
    hh_beacon_t b = mk_beacon(2, 1);
    hh_link_sample_t s = mk_sample(2, -50.0f);

    HH_ASSERT_OK(hh_neighbor_on_beacon(&f.nm, &b, &s, f.vc.now));
    hh_dispatcher_drain(&f.bus);

    HH_ASSERT_EQ_INT(hh_neighbor_count(&f.nm), 1);
    HH_ASSERT_EQ_INT(o.ups, 1);
    HH_ASSERT_EQ_INT(o.last_up, 2);

    const hh_neighbor_t *n = hh_neighbor_get(&f.nm, 2);
    HH_ASSERT(n != NULL);
    HH_ASSERT_EQ_INT(n->last_seq, 1);
    HH_ASSERT(n->routing_capable);
    HH_ASSERT_NEAR(n->last_sample.rssi, -50.0, 0.01);
}

static void test_repeat_beacon_refreshes_without_duplicate_up(void)
{
    fix_t f; obs_t o; fix_init(&f, &o);
    hh_link_sample_t s = mk_sample(2, -50.0f);
    hh_beacon_t b1 = mk_beacon(2, 1), b2 = mk_beacon(2, 2);

    hh_neighbor_on_beacon(&f.nm, &b1, &s, f.vc.now);
    vclock_advance(&f.vc, 1000);
    hh_neighbor_on_beacon(&f.nm, &b2, &s, f.vc.now);
    hh_dispatcher_drain(&f.bus);

    /* One NeighborUp only: a refresh is not a new neighbor. */
    HH_ASSERT_EQ_INT(o.ups, 1);
    HH_ASSERT_EQ_INT(hh_neighbor_count(&f.nm), 1);
    HH_ASSERT_EQ_INT(hh_neighbor_get(&f.nm, 2)->last_heard, f.vc.now);
}

static void test_capability_change_emits_changed(void)
{
    fix_t f; obs_t o; fix_init(&f, &o);
    hh_link_sample_t s = mk_sample(2, -50.0f);
    hh_beacon_t b = mk_beacon(2, 1);

    hh_neighbor_on_beacon(&f.nm, &b, &s, f.vc.now);
    vclock_advance(&f.vc, 1000);
    b.sequence_no = 2;
    b.capabilities = HH_CAP_LEAF_ONLY;   /* capability changed */
    hh_neighbor_on_beacon(&f.nm, &b, &s, f.vc.now);
    hh_dispatcher_drain(&f.bus);

    HH_ASSERT_EQ_INT(o.changes, 1);
    HH_ASSERT_EQ_INT(o.last_changed_attrs, HH_NBR_ATTR_CAPABILITIES);
    HH_ASSERT_EQ_INT(hh_neighbor_get(&f.nm, 2)->capabilities, HH_CAP_LEAF_ONLY);
}

static void test_expiry_is_cadence_based(void)
{
    fix_t f; obs_t o; fix_init(&f, &o);
    hh_link_sample_t s = mk_sample(2, -50.0f);
    hh_beacon_t b = mk_beacon(2, 1);

    /* Establish an observed cadence of 500ms, faster than our own 1000ms. */
    hh_neighbor_on_beacon(&f.nm, &b, &s, f.vc.now);
    for (int i = 2; i <= 5; i++) {
        vclock_advance(&f.vc, 500);
        b.sequence_no = (hh_seq_t)i;
        hh_neighbor_on_beacon(&f.nm, &b, &s, f.vc.now);
    }
    HH_ASSERT_NEAR(hh_neighbor_get(&f.nm, 2)->observed_interval_ms, 500, 60);

    /* Deadline is allowed_loss+1 intervals of THAT neighbor's cadence, so an
     * expiry fires sooner than our own slower interval would imply. */
    vclock_advance(&f.vc, 500 * (f.cfg.neighbor_allowed_loss + 1) + 10);
    HH_ASSERT_EQ_INT(hh_neighbor_tick(&f.nm, f.vc.now), 1);
    hh_dispatcher_drain(&f.bus);
    HH_ASSERT_EQ_INT(o.downs, 1);
    HH_ASSERT_EQ_INT(o.last_down, 2);
    HH_ASSERT_EQ_INT(hh_neighbor_count(&f.nm), 0);
}

static void test_neighbor_survives_allowed_losses(void)
{
    fix_t f; obs_t o; fix_init(&f, &o);
    hh_link_sample_t s = mk_sample(2, -50.0f);
    hh_beacon_t b = mk_beacon(2, 1);

    hh_neighbor_on_beacon(&f.nm, &b, &s, f.vc.now);
    /* a neighbor is not deleted on the first miss. */
    vclock_advance(&f.vc, f.cfg.beacon_interval_ms * f.cfg.neighbor_allowed_loss);
    HH_ASSERT_EQ_INT(hh_neighbor_tick(&f.nm, f.vc.now), 0);
    HH_ASSERT_EQ_INT(hh_neighbor_count(&f.nm), 1);
    hh_dispatcher_drain(&f.bus);
    HH_ASSERT_EQ_INT(o.downs, 0);
}

static void test_missed_beacons_counted_from_sequence_gap(void)
{
    fix_t f; obs_t o; fix_init(&f, &o);
    hh_link_sample_t s = mk_sample(2, -50.0f);
    hh_beacon_t b = mk_beacon(2, 10);

    hh_neighbor_on_beacon(&f.nm, &b, &s, f.vc.now);
    vclock_advance(&f.vc, 1000);
    b.sequence_no = 14;      /* 11, 12, 13 were lost */
    hh_neighbor_on_beacon(&f.nm, &b, &s, f.vc.now);

    /* Loss visible from the sequence gap is a primary link-health signal. */
    HH_ASSERT_EQ_INT(hh_neighbor_get(&f.nm, 2)->beacons_missed, 3);
}

static void test_lru_eviction_when_table_full(void)
{
    fix_t f; obs_t o; fix_init(&f, &o);
    hh_link_sample_t s;
    f.cfg.max_neighbors = 3;

    /* Fill the table, each neighbor heard at a distinct time. */
    for (hh_node_id_t id = 2; id <= 4; id++) {
        hh_beacon_t b = mk_beacon(id, 1);
        s = mk_sample(id, -50.0f);
        hh_neighbor_on_beacon(&f.nm, &b, &s, f.vc.now);
        vclock_advance(&f.vc, 100);
    }
    HH_ASSERT_EQ_INT(hh_neighbor_count(&f.nm), 3);

    /* Refresh node 2 so node 3 becomes least-recently-heard. */
    {
        hh_beacon_t b = mk_beacon(2, 2);
        s = mk_sample(2, -50.0f);
        vclock_advance(&f.vc, 100);
        hh_neighbor_on_beacon(&f.nm, &b, &s, f.vc.now);
    }
    /* A fifth neighbor must evict the LRU entry (node 3), not an arbitrary one. */
    {
        hh_beacon_t b = mk_beacon(5, 1);
        s = mk_sample(5, -50.0f);
        vclock_advance(&f.vc, 100);
        hh_neighbor_on_beacon(&f.nm, &b, &s, f.vc.now);
    }
    HH_ASSERT_EQ_INT(hh_neighbor_count(&f.nm), 3);
    HH_ASSERT(hh_neighbor_get(&f.nm, 3) == NULL);   /* evicted */
    HH_ASSERT(hh_neighbor_get(&f.nm, 2) != NULL);
    HH_ASSERT(hh_neighbor_get(&f.nm, 5) != NULL);
    HH_ASSERT_EQ_INT(f.nm.evictions, 1);
    hh_dispatcher_drain(&f.bus);
    HH_ASSERT_EQ_INT(o.downs, 1);   /* eviction announces a NeighborDown */
}

static void test_explicit_remove_emits_down(void)
{
    fix_t f; obs_t o; fix_init(&f, &o);
    hh_beacon_t b = mk_beacon(2, 1);
    hh_link_sample_t s = mk_sample(2, -50.0f);

    hh_neighbor_on_beacon(&f.nm, &b, &s, f.vc.now);
    HH_ASSERT_OK(hh_neighbor_remove(&f.nm, 2, f.vc.now));
    hh_dispatcher_drain(&f.bus);
    HH_ASSERT_EQ_INT(o.downs, 1);
    HH_ASSERT_EQ_INT(hh_neighbor_count(&f.nm), 0);
    HH_ASSERT_ERR(hh_neighbor_remove(&f.nm, 2, f.vc.now), HH_ERR_NOTFOUND);
}

static void test_self_and_invalid_beacons_rejected(void)
{
    fix_t f; obs_t o; fix_init(&f, &o);
    hh_link_sample_t s = mk_sample(1, -50.0f);
    hh_beacon_t self = mk_beacon(1, 1);       /* our own node id */
    hh_beacon_t zero = mk_beacon(0, 1);

    HH_ASSERT_ERR(hh_neighbor_on_beacon(&f.nm, &self, &s, f.vc.now), HH_ERR_INVAL);
    HH_ASSERT_ERR(hh_neighbor_on_beacon(&f.nm, &zero, &s, f.vc.now), HH_ERR_INVAL);
    HH_ASSERT_EQ_INT(hh_neighbor_count(&f.nm), 0);
}

static void test_sample_recording_emits_metric_change(void)
{
    fix_t f; obs_t o; fix_init(&f, &o);
    hh_beacon_t b = mk_beacon(2, 1);
    hh_link_sample_t s = mk_sample(2, -50.0f);

    hh_neighbor_on_beacon(&f.nm, &b, &s, f.vc.now);
    s.rssi = -80.0f;
    HH_ASSERT_OK(hh_neighbor_record_sample(&f.nm, &s, f.vc.now));
    hh_dispatcher_drain(&f.bus);

    HH_ASSERT_EQ_INT(o.last_changed_attrs, HH_NBR_ATTR_METRICS);
    HH_ASSERT_NEAR(hh_neighbor_get(&f.nm, 2)->last_sample.rssi, -80.0, 0.01);

    /* A sample for an unknown neighbor is not silently accepted. */
    s.neighbor_id = 99;
    HH_ASSERT_ERR(hh_neighbor_record_sample(&f.nm, &s, f.vc.now), HH_ERR_NOTFOUND);
}

static void test_list_snapshot(void)
{
    fix_t f; obs_t o; fix_init(&f, &o);
    hh_node_id_t ids[8];
    for (hh_node_id_t id = 2; id <= 5; id++) {
        hh_beacon_t b = mk_beacon(id, 1);
        hh_link_sample_t s = mk_sample(id, -50.0f);
        hh_neighbor_on_beacon(&f.nm, &b, &s, f.vc.now);
    }
    HH_ASSERT_EQ_INT(hh_neighbor_list(&f.nm, ids, 8), 4);
    /* A bounded caller buffer truncates rather than overflowing. */
    HH_ASSERT_EQ_INT(hh_neighbor_list(&f.nm, ids, 2), 2);
}

HH_TEST_MAIN_BEGIN("neighbor")
    HH_RUN(test_first_beacon_creates_neighbor_and_emits_up);
    HH_RUN(test_repeat_beacon_refreshes_without_duplicate_up);
    HH_RUN(test_capability_change_emits_changed);
    HH_RUN(test_expiry_is_cadence_based);
    HH_RUN(test_neighbor_survives_allowed_losses);
    HH_RUN(test_missed_beacons_counted_from_sequence_gap);
    HH_RUN(test_lru_eviction_when_table_full);
    HH_RUN(test_explicit_remove_emits_down);
    HH_RUN(test_self_and_invalid_beacons_rejected);
    HH_RUN(test_sample_recording_emits_metric_change);
    HH_RUN(test_list_snapshot);
HH_TEST_MAIN_END()
