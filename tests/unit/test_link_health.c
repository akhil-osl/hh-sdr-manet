/*
 * Link Health Monitor tests (Doc 1 §7).
 *
 * The properties that matter architecturally, each tested for real behavior:
 *  - hysteresis: a link cannot oscillate on noise
 *  - no single signal alone moves a link past Degraded
 *  - hold-down actually gates SuspectedFailure -> Failed
 *  - cause classification distinguishes the five patterns Doc 1 describes
 *  - absent metrics are excluded from fusion, not read as zero
 */
#include "hhsdr/manet/link_health.h"
#include "hh_test.h"
#include "vclock.h"
#include <string.h>

typedef struct {
    hh_config_t      cfg;
    vclock_t         vc;
    hh_dispatcher_t  bus;
    hh_link_health_t lh;
} fix_t;

typedef struct {
    int count;
    hh_ev_link_state_t last;
    int cadence_hints;
    bool last_unstable;
} obs_t;

static void observe(const hh_event_t *ev, void *ctx)
{
    obs_t *o = ctx;
    if (ev->type == HH_EV_LINK_STATE_CHANGED) { o->count++; o->last = ev->u.link_state; }
    else if (ev->type == HH_EV_CADENCE_HINT)  { o->cadence_hints++;
                                                o->last_unstable = ev->u.cadence.unstable; }
}

static void fix_init(fix_t *f, obs_t *o)
{
    memset(f, 0, sizeof *f);
    memset(o, 0, sizeof *o);
    hh_config_defaults(&f->cfg);
    f->cfg.node_id = 1;
    vclock_init(&f->vc, 10000);
    hh_dispatcher_init(&f->bus);
    hh_dispatcher_subscribe(&f->bus, "obs",
        HH_EV_MASK(HH_EV_LINK_STATE_CHANGED) | HH_EV_MASK(HH_EV_CADENCE_HINT),
        observe, o);
    hh_link_health_init(&f->lh, &f->cfg, &f->vc.clock, &f->bus);
    hh_link_health_add(&f->lh, 2, f->vc.now);
}

static hh_link_sample_t sample(hh_node_id_t id, float rssi, float snr, float per)
{
    hh_link_sample_t s;
    memset(&s, 0, sizeof s);
    s.neighbor_id = id; s.rssi = rssi; s.snr = snr; s.per = per;
    return s;
}

/* Feed n samples so the EWMA settles; keeps the beacon timer fresh. */
static void feed(fix_t *f, hh_link_sample_t s, int n, hh_time_ms_t step)
{
    for (int i = 0; i < n; i++) {
        hh_link_health_on_beacon(&f->lh, s.neighbor_id, f->vc.now);
        hh_link_health_on_sample(&f->lh, &s, f->vc.now);
        vclock_advance(&f->vc, step);
    }
}

static void test_starts_healthy(void)
{
    fix_t f; obs_t o; fix_init(&f, &o);
    HH_ASSERT_EQ_INT(hh_link_health_state(&f.lh, 2), HH_LINK_HEALTHY);
    HH_ASSERT_NEAR(hh_link_health_score(&f.lh, 2), 1.0, 0.001);
}

static void test_good_link_stays_healthy(void)
{
    fix_t f; obs_t o; fix_init(&f, &o);
    feed(&f, sample(2, -50.0f, 25.0f, 0.0f), 20, 100);
    HH_ASSERT_EQ_INT(hh_link_health_state(&f.lh, 2), HH_LINK_HEALTHY);
    hh_dispatcher_drain(&f.bus);
    HH_ASSERT_EQ_INT(o.count, 0);   /* no spurious transitions */
}

static void test_single_bad_sample_does_not_transition(void)
{
    fix_t f; obs_t o; fix_init(&f, &o);
    feed(&f, sample(2, -50.0f, 25.0f, 0.0f), 10, 100);
    /* One terrible sample: EWMA must absorb it (Doc 1 §7 requires sustained
     * evidence, not one bad sample). */
    feed(&f, sample(2, -95.0f, 1.0f, 0.9f), 1, 100);
    HH_ASSERT_EQ_INT(hh_link_health_state(&f.lh, 2), HH_LINK_HEALTHY);
}

static void test_sustained_degradation_enters_degraded(void)
{
    fix_t f; obs_t o; fix_init(&f, &o);
    feed(&f, sample(2, -50.0f, 25.0f, 0.0f), 5, 100);
    /* Marginal but not failing: exactly one signal bad. */
    feed(&f, sample(2, -88.0f, 20.0f, 0.05f), 15, 100);

    HH_ASSERT_EQ_INT(hh_link_health_state(&f.lh, 2), HH_LINK_DEGRADED);
    hh_dispatcher_drain(&f.bus);
    HH_ASSERT_EQ_INT(o.last.new_state, HH_LINK_DEGRADED);
    HH_ASSERT_EQ_INT(o.last.old_state, HH_LINK_HEALTHY);
    HH_ASSERT_EQ_INT(o.last.neighbor, 2);
}

static void test_single_signal_cannot_pass_degraded(void)
{
    fix_t f; obs_t o; fix_init(&f, &o);
    /* RSSI alone catastrophically bad, everything else perfect. */
    feed(&f, sample(2, -99.0f, 30.0f, 0.0f), 40, 100);

    /* Doc 1 §7: "No single signal alone can move a link past Degraded." */
    HH_ASSERT_EQ_INT(hh_link_health_state(&f.lh, 2), HH_LINK_DEGRADED);
}

static void test_two_signals_reach_suspected_then_failed_after_holddown(void)
{
    fix_t f; obs_t o; fix_init(&f, &o);
    hh_link_sample_t bad = sample(2, -95.0f, 2.0f, 0.5f);   /* RSSI+SNR+PER bad */

    feed(&f, bad, 5, 100);
    HH_ASSERT_EQ_INT(hh_link_health_state(&f.lh, 2), HH_LINK_SUSPECTED_FAILURE);

    /* Must NOT advance to Failed before the hold-down elapses. */
    vclock_advance(&f.vc, f.cfg.lh_suspect_hold_ms / 2);
    hh_link_health_tick(&f.lh, f.vc.now);
    HH_ASSERT_EQ_INT(hh_link_health_state(&f.lh, 2), HH_LINK_SUSPECTED_FAILURE);

    vclock_advance(&f.vc, f.cfg.lh_suspect_hold_ms);
    hh_link_health_tick(&f.lh, f.vc.now);
    HH_ASSERT_EQ_INT(hh_link_health_state(&f.lh, 2), HH_LINK_FAILED);
}

static void test_suspected_recovers_before_holddown(void)
{
    fix_t f; obs_t o; fix_init(&f, &o);
    feed(&f, sample(2, -95.0f, 2.0f, 0.5f), 5, 100);
    HH_ASSERT_EQ_INT(hh_link_health_state(&f.lh, 2), HH_LINK_SUSPECTED_FAILURE);

    /* Signals recover before the hold time: back to Degraded, not Failed. */
    feed(&f, sample(2, -50.0f, 25.0f, 0.0f), 2, 100);
    HH_ASSERT_EQ_INT(hh_link_health_state(&f.lh, 2), HH_LINK_DEGRADED);
}

static void test_hysteresis_requires_high_water_mark_to_recover(void)
{
    fix_t f; obs_t o; fix_init(&f, &o);
    feed(&f, sample(2, -50.0f, 25.0f, 0.0f), 5, 100);
    feed(&f, sample(2, -88.0f, 20.0f, 0.05f), 15, 100);
    HH_ASSERT_EQ_INT(hh_link_health_state(&f.lh, 2), HH_LINK_DEGRADED);

    /* Score between the degrade and recover thresholds: must stay Degraded,
     * which is exactly what stops oscillation across the boundary. */
    while (hh_link_health_score(&f.lh, 2) < f.cfg.lh_degrade_threshold) {
        feed(&f, sample(2, -78.0f, 16.0f, 0.02f), 1, 100);
    }
    HH_ASSERT(hh_link_health_score(&f.lh, 2) >= f.cfg.lh_degrade_threshold);
    HH_ASSERT(hh_link_health_score(&f.lh, 2) < f.cfg.lh_recover_threshold);
    HH_ASSERT_EQ_INT(hh_link_health_state(&f.lh, 2), HH_LINK_DEGRADED);

    /* Only past the stricter high-water mark does it return to Healthy. */
    feed(&f, sample(2, -45.0f, 30.0f, 0.0f), 20, 100);
    HH_ASSERT(hh_link_health_score(&f.lh, 2) >= f.cfg.lh_recover_threshold);
    HH_ASSERT_EQ_INT(hh_link_health_state(&f.lh, 2), HH_LINK_HEALTHY);
}

static void test_failed_to_recovering_on_fresh_beacon_then_healthy(void)
{
    fix_t f; obs_t o; fix_init(&f, &o);
    feed(&f, sample(2, -95.0f, 2.0f, 0.5f), 5, 100);
    vclock_advance(&f.vc, f.cfg.lh_suspect_hold_ms + 100);
    hh_link_health_tick(&f.lh, f.vc.now);
    HH_ASSERT_EQ_INT(hh_link_health_state(&f.lh, 2), HH_LINK_FAILED);

    /* A fresh valid beacon is the only exit from Failed. */
    hh_link_health_on_beacon(&f.lh, 2, f.vc.now);
    HH_ASSERT_EQ_INT(hh_link_health_state(&f.lh, 2), HH_LINK_RECOVERING);

    /* Recovering must survive the hold-down window with a good score. */
    feed(&f, sample(2, -45.0f, 30.0f, 0.0f), 5, 100);
    HH_ASSERT_EQ_INT(hh_link_health_state(&f.lh, 2), HH_LINK_RECOVERING);

    vclock_advance(&f.vc, f.cfg.lh_recover_hold_ms);
    feed(&f, sample(2, -45.0f, 30.0f, 0.0f), 1, 10);
    HH_ASSERT_EQ_INT(hh_link_health_state(&f.lh, 2), HH_LINK_HEALTHY);
}

static void test_relapse_during_recovering_returns_to_failed(void)
{
    fix_t f; obs_t o; fix_init(&f, &o);
    feed(&f, sample(2, -95.0f, 2.0f, 0.5f), 5, 100);
    vclock_advance(&f.vc, f.cfg.lh_suspect_hold_ms + 100);
    hh_link_health_tick(&f.lh, f.vc.now);
    hh_link_health_on_beacon(&f.lh, 2, f.vc.now);
    HH_ASSERT_EQ_INT(hh_link_health_state(&f.lh, 2), HH_LINK_RECOVERING);

    /* Relapse before hold-down completes. Samples are fed WITHOUT a beacon:
     * a beacon would itself re-trigger Failed -> Recovering, which is correct
     * behavior but would confuse what this test is isolating. */
    {
        hh_link_sample_t bad = sample(2, -95.0f, 2.0f, 0.6f);
        for (int i = 0; i < 2; i++) {
            hh_link_health_on_sample(&f.lh, &bad, f.vc.now);
            vclock_advance(&f.vc, 100);
        }
    }
    HH_ASSERT_EQ_INT(hh_link_health_state(&f.lh, 2), HH_LINK_FAILED);
    /* One completed Failed -> Recovering cycle counts as one flap, which is
     * what route dampening later penalises (Doc 1 §8). */
    HH_ASSERT_EQ_INT(hh_link_health_get(&f.lh, 2)->flap_count, 1);
}

static void test_repeated_flaps_accumulate_in_window(void)
{
    fix_t f; obs_t o; fix_init(&f, &o);
    hh_link_sample_t bad = sample(2, -95.0f, 2.0f, 0.6f);

    /* Drive three full Failed -> Recovering -> Failed cycles. Route dampening
     * keys off this count, so it must track cycles, not transitions. */
    for (int cycle = 0; cycle < 3; cycle++) {
        for (int i = 0; i < 5; i++) {
            hh_link_health_on_sample(&f.lh, &bad, f.vc.now);
            vclock_advance(&f.vc, 100);
        }
        vclock_advance(&f.vc, f.cfg.lh_suspect_hold_ms + 100);
        hh_link_health_tick(&f.lh, f.vc.now);
        HH_ASSERT_EQ_INT(hh_link_health_state(&f.lh, 2), HH_LINK_FAILED);

        hh_link_health_on_beacon(&f.lh, 2, f.vc.now);
        HH_ASSERT_EQ_INT(hh_link_health_state(&f.lh, 2), HH_LINK_RECOVERING);
    }
    HH_ASSERT_EQ_INT(hh_link_health_get(&f.lh, 2)->flap_count, 3);
    HH_ASSERT(hh_link_health_get(&f.lh, 2)->flap_count >= f.cfg.dampening_flap_threshold);
}

static void test_cause_hint_rf_interference_when_multiple_links_show_phy_errors(void)
{
    fix_t f; obs_t o; fix_init(&f, &o);
    hh_link_sample_t s2, s3;

    hh_link_health_add(&f.lh, 3, f.vc.now);
    /* PHY/CRC errors on two neighbors with RSSI still present => the channel. */
    s2 = sample(2, -60.0f, 4.0f, 0.4f); s2.phy_errors = 20;
    s3 = sample(3, -60.0f, 4.0f, 0.4f); s3.phy_errors = 20;

    for (int i = 0; i < 6; i++) {
        hh_link_health_on_beacon(&f.lh, 2, f.vc.now);
        hh_link_health_on_beacon(&f.lh, 3, f.vc.now);
        hh_link_health_on_sample(&f.lh, &s2, f.vc.now);
        hh_link_health_on_sample(&f.lh, &s3, f.vc.now);
        vclock_advance(&f.vc, 100);
    }
    hh_dispatcher_drain(&f.bus);
    HH_ASSERT_EQ_INT(o.last.cause_hint, HH_CAUSE_RF_INTERFERENCE);
}

static void test_cause_hint_mobility_on_falling_rssi_with_loss(void)
{
    fix_t f; obs_t o; fix_init(&f, &o);
    /* RSSI trending down with PER rising in step (Doc 1 §7 mobility pattern). */
    for (int i = 0; i < 12; i++) {
        hh_link_sample_t s = sample(2, -55.0f - (float)i * 4.0f, 20.0f - (float)i,
                                    0.02f * (float)i);
        hh_link_health_on_beacon(&f.lh, 2, f.vc.now);
        hh_link_health_on_sample(&f.lh, &s, f.vc.now);
        vclock_advance(&f.vc, 100);
    }
    hh_dispatcher_drain(&f.bus);
    HH_ASSERT_EQ_INT(o.last.cause_hint, HH_CAUSE_MOBILITY);
}

static void test_cause_hint_asymmetric_when_beacons_land_but_acks_fail(void)
{
    fix_t f; obs_t o; fix_init(&f, &o);
    hh_link_sample_t s = sample(2, -60.0f, 18.0f, 0.0f);
    s.ack_valid = true;
    s.ack_success = false;      /* data ACKs failing, beacons fine */

    feed(&f, s, 20, 100);
    /* Doc 1 §7: this stays Degraded rather than reading as a full failure. */
    HH_ASSERT_EQ_INT(hh_link_health_state(&f.lh, 2), HH_LINK_DEGRADED);
    hh_dispatcher_drain(&f.bus);
    HH_ASSERT_EQ_INT(o.last.cause_hint, HH_CAUSE_ASYMMETRIC_LINK);
}

static void test_silence_drives_degradation_without_samples(void)
{
    fix_t f; obs_t o; fix_init(&f, &o);
    feed(&f, sample(2, -50.0f, 25.0f, 0.0f), 5, 100);
    HH_ASSERT_EQ_INT(hh_link_health_state(&f.lh, 2), HH_LINK_HEALTHY);

    /* No further samples at all: a link that goes quiet must not hold its last
     * good score forever. */
    for (int i = 0; i < 20; i++) {
        vclock_advance(&f.vc, f.cfg.beacon_interval_ms);
        hh_link_health_tick(&f.lh, f.vc.now);
    }
    HH_ASSERT(hh_link_health_state(&f.lh, 2) != HH_LINK_HEALTHY);
}

static void test_absent_signals_excluded_from_fusion(void)
{
    fix_t f; obs_t o; fix_init(&f, &o);
    /* ack_valid/latency_valid false: a radio that cannot report them must not
     * be penalised as though they were zero. */
    hh_link_sample_t s = sample(2, -50.0f, 25.0f, 0.0f);
    HH_ASSERT(!s.ack_valid);
    HH_ASSERT(!s.latency_valid);
    feed(&f, s, 10, 100);
    HH_ASSERT_NEAR(hh_link_health_score(&f.lh, 2), 1.0, 0.05);
    HH_ASSERT_EQ_INT(hh_link_health_state(&f.lh, 2), HH_LINK_HEALTHY);
}

static void test_cadence_hint_published_on_edge_only(void)
{
    fix_t f; obs_t o; fix_init(&f, &o);
    feed(&f, sample(2, -50.0f, 25.0f, 0.0f), 5, 100);
    hh_dispatcher_drain(&f.bus);
    HH_ASSERT_EQ_INT(o.cadence_hints, 0);

    /* Going unstable publishes exactly one hint, not one per sample. */
    feed(&f, sample(2, -95.0f, 2.0f, 0.5f), 10, 100);
    hh_dispatcher_drain(&f.bus);
    HH_ASSERT_EQ_INT(o.cadence_hints, 1);
    HH_ASSERT(o.last_unstable);
}

static void test_unknown_neighbor_rejected(void)
{
    fix_t f; obs_t o; fix_init(&f, &o);
    hh_link_sample_t s = sample(99, -50.0f, 25.0f, 0.0f);
    HH_ASSERT_ERR(hh_link_health_on_sample(&f.lh, &s, f.vc.now), HH_ERR_NOTFOUND);
    HH_ASSERT_ERR(hh_link_health_on_beacon(&f.lh, 99, f.vc.now), HH_ERR_NOTFOUND);
}

HH_TEST_MAIN_BEGIN("link_health")
    HH_RUN(test_starts_healthy);
    HH_RUN(test_good_link_stays_healthy);
    HH_RUN(test_single_bad_sample_does_not_transition);
    HH_RUN(test_sustained_degradation_enters_degraded);
    HH_RUN(test_single_signal_cannot_pass_degraded);
    HH_RUN(test_two_signals_reach_suspected_then_failed_after_holddown);
    HH_RUN(test_suspected_recovers_before_holddown);
    HH_RUN(test_hysteresis_requires_high_water_mark_to_recover);
    HH_RUN(test_failed_to_recovering_on_fresh_beacon_then_healthy);
    HH_RUN(test_relapse_during_recovering_returns_to_failed);
    HH_RUN(test_repeated_flaps_accumulate_in_window);
    HH_RUN(test_cause_hint_rf_interference_when_multiple_links_show_phy_errors);
    HH_RUN(test_cause_hint_mobility_on_falling_rssi_with_loss);
    HH_RUN(test_cause_hint_asymmetric_when_beacons_land_but_acks_fail);
    HH_RUN(test_silence_drives_degradation_without_samples);
    HH_RUN(test_absent_signals_excluded_from_fusion);
    HH_RUN(test_cadence_hint_published_on_edge_only);
    HH_RUN(test_unknown_neighbor_rejected);
HH_TEST_MAIN_END()
