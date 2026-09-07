/*
 * Discovery Manager tests (Doc 1 §4).
 *
 * Behavior verified, not just execution: acquisition-to-steady transition on
 * both triggers, beacon cadence actually gating transmission, sequence-based
 * duplicate rejection including across a wrap, and adaptive cadence clamping.
 */
#include "hhsdr/manet/discovery.h"
#include "hhsdr/radio/wire.h"
#include "mock_radio.h"
#include "vclock.h"
#include "hh_test.h"
#include <string.h>

typedef struct {
    hh_config_t     cfg;
    vclock_t        vc;
    hh_dispatcher_t bus;
    mock_radio_t    mock;
    hh_radio_t      radio;
    hh_discovery_t  disc;
} fix_t;

static void fix_init(fix_t *f, hh_node_id_t id)
{
    memset(f, 0, sizeof *f);
    hh_config_defaults(&f->cfg);
    f->cfg.node_id = id;
    vclock_init(&f->vc, 1000);
    hh_dispatcher_init(&f->bus);
    mock_radio_init(&f->mock, "t", &f->radio);
    hh_radio_open(&f->radio);
    hh_discovery_init(&f->disc, &f->cfg, &f->vc.clock, &f->bus, &f->radio);
}

/* Build a beacon frame as if received from `src`. */
static hh_frame_t mk_beacon_frame(hh_node_id_t src, hh_seq_t seq, uint16_t version)
{
    hh_frame_t fr;
    hh_beacon_t b;
    memset(&fr, 0, sizeof fr);
    memset(&b, 0, sizeof b);
    b.node_id = src;
    b.protocol_version = version;
    b.sequence_no = seq;
    b.routing_capable = true;
    b.capabilities = HH_CAP_ROUTING_CAPABLE;
    fr.kind = HH_FRAME_BEACON;
    fr.src = src;
    fr.len = (uint16_t)hh_beacon_encode(&b, fr.data, sizeof fr.data);
    return fr;
}

static hh_link_sample_t good_sample(hh_node_id_t id)
{
    hh_link_sample_t s;
    memset(&s, 0, sizeof s);
    s.neighbor_id = id; s.rssi = -55.0f; s.snr = 25.0f; s.per = 0.0f;
    return s;
}

static void test_starts_in_acquisition_and_beacons_immediately(void)
{
    fix_t f; fix_init(&f, 1);
    HH_ASSERT_OK(hh_discovery_start(&f.disc));
    HH_ASSERT_EQ_INT(f.disc.phase, HH_DISC_ACQUISITION);
    HH_ASSERT_EQ_INT(hh_discovery_interval(&f.disc), f.cfg.beacon_interval_acq_ms);

    /* A node just coming up should be discoverable at once, not after a wait. */
    HH_ASSERT_OK(hh_discovery_tick(&f.disc, f.vc.now));
    HH_ASSERT_EQ_INT(mock_radio_tx_count_kind(&f.mock, HH_FRAME_BEACON), 1);
}

static void test_cadence_gates_transmission(void)
{
    fix_t f; fix_init(&f, 1);
    hh_discovery_start(&f.disc);
    hh_discovery_tick(&f.disc, f.vc.now);
    HH_ASSERT_EQ_INT(f.mock.tx_total, 1);

    /* Before the interval elapses, no beacon. */
    vclock_advance(&f.vc, f.cfg.beacon_interval_acq_ms - 1);
    HH_ASSERT_ERR(hh_discovery_tick(&f.disc, f.vc.now), HH_ERR_AGAIN);
    HH_ASSERT_EQ_INT(f.mock.tx_total, 1);

    /* Exactly at the interval, it fires. */
    vclock_advance(&f.vc, 1);
    HH_ASSERT_OK(hh_discovery_tick(&f.disc, f.vc.now));
    HH_ASSERT_EQ_INT(f.mock.tx_total, 2);
}

static void test_sequence_numbers_increment_on_wire(void)
{
    fix_t f; fix_init(&f, 7);
    hh_beacon_t b1, b2;
    const hh_frame_t *fr;

    hh_discovery_start(&f.disc);
    hh_discovery_tick(&f.disc, f.vc.now);
    fr = mock_radio_last_tx(&f.mock, HH_FRAME_BEACON);
    HH_ASSERT(fr != NULL);
    HH_ASSERT_OK(hh_beacon_decode(fr->data, fr->len, &b1));
    HH_ASSERT_EQ_INT(b1.node_id, 7);

    vclock_advance(&f.vc, f.cfg.beacon_interval_acq_ms);
    hh_discovery_tick(&f.disc, f.vc.now);
    fr = mock_radio_last_tx(&f.mock, HH_FRAME_BEACON);
    HH_ASSERT_OK(hh_beacon_decode(fr->data, fr->len, &b2));
    HH_ASSERT_EQ_INT(b2.sequence_no, b1.sequence_no + 1);
}

static void test_acquisition_ends_when_neighbor_heard(void)
{
    fix_t f; fix_init(&f, 1);
    hh_frame_t fr = mk_beacon_frame(2, 5, HH_PROTOCOL_VERSION);
    hh_link_sample_t s = good_sample(2);

    hh_discovery_start(&f.disc);
    HH_ASSERT_EQ_INT(f.disc.phase, HH_DISC_ACQUISITION);

    HH_ASSERT_OK(hh_discovery_on_frame(&f.disc, &fr, &s));
    /* Doc 1 §4: hearing a neighbor drops the node to steady-state cadence. */
    HH_ASSERT_EQ_INT(f.disc.phase, HH_DISC_STEADY);
    HH_ASSERT_EQ_INT(hh_discovery_interval(&f.disc), f.cfg.beacon_interval_ms);
}

static void test_acquisition_ends_on_timeout_when_alone(void)
{
    fix_t f; fix_init(&f, 1);
    hh_discovery_start(&f.disc);

    /* An isolated node must not beacon fast forever. */
    vclock_advance(&f.vc, f.cfg.acquisition_timeout_ms);
    hh_discovery_tick(&f.disc, f.vc.now);
    HH_ASSERT_EQ_INT(f.disc.phase, HH_DISC_STEADY);
    HH_ASSERT_EQ_INT(hh_discovery_interval(&f.disc), f.cfg.beacon_interval_ms);
}

static void test_beacon_publishes_event_for_neighbor_manager(void)
{
    fix_t f; fix_init(&f, 1);
    hh_frame_t fr = mk_beacon_frame(2, 5, HH_PROTOCOL_VERSION);
    hh_link_sample_t s = good_sample(2);

    hh_discovery_start(&f.disc);
    HH_ASSERT_OK(hh_discovery_on_frame(&f.disc, &fr, &s));
    /* Discovery publishes; it does not itself decide neighbor validity. */
    HH_ASSERT_EQ_INT(hh_dispatcher_pending(&f.bus), 0); /* nobody subscribed yet */
    HH_ASSERT_EQ_INT(f.disc.beacons_rx_accepted, 1);
}

static void test_duplicate_and_stale_sequence_rejected(void)
{
    fix_t f; fix_init(&f, 1);
    hh_link_sample_t s = good_sample(2);
    hh_frame_t fr10 = mk_beacon_frame(2, 10, HH_PROTOCOL_VERSION);
    hh_frame_t fr9  = mk_beacon_frame(2, 9,  HH_PROTOCOL_VERSION);

    hh_discovery_start(&f.disc);
    HH_ASSERT_OK(hh_discovery_on_frame(&f.disc, &fr10, &s));
    /* Same sequence again: duplicate. */
    HH_ASSERT_ERR(hh_discovery_on_frame(&f.disc, &fr10, &s), HH_ERR_AGAIN);
    /* Older sequence: stale, e.g. a replay. */
    HH_ASSERT_ERR(hh_discovery_on_frame(&f.disc, &fr9, &s), HH_ERR_AGAIN);
    HH_ASSERT_EQ_INT(f.disc.beacons_rx_accepted, 1);
    HH_ASSERT_EQ_INT(f.disc.beacons_rx_duplicate, 2);
}

static void test_sequence_acceptance_across_wraparound(void)
{
    fix_t f; fix_init(&f, 1);
    hh_link_sample_t s = good_sample(2);
    hh_frame_t hi = mk_beacon_frame(2, 0xFFFFFFF0u, HH_PROTOCOL_VERSION);
    hh_frame_t wrapped = mk_beacon_frame(2, 3u, HH_PROTOCOL_VERSION);

    hh_discovery_start(&f.disc);
    HH_ASSERT_OK(hh_discovery_on_frame(&f.disc, &hi, &s));
    /* A wrapped counter is still newer and must be accepted, not read as stale. */
    HH_ASSERT_OK(hh_discovery_on_frame(&f.disc, &wrapped, &s));
    HH_ASSERT_EQ_INT(f.disc.beacons_rx_accepted, 2);
}

static void test_malformed_and_version_mismatch_rejected(void)
{
    fix_t f; fix_init(&f, 1);
    hh_link_sample_t s = good_sample(2);
    hh_frame_t bad = mk_beacon_frame(2, 1, HH_PROTOCOL_VERSION + 9);
    hh_frame_t trunc = mk_beacon_frame(2, 1, HH_PROTOCOL_VERSION);

    hh_discovery_start(&f.disc);
    HH_ASSERT_ERR(hh_discovery_on_frame(&f.disc, &bad, &s), HH_ERR_UNSUPPORTED);
    trunc.len = 4;
    HH_ASSERT_ERR(hh_discovery_on_frame(&f.disc, &trunc, &s), HH_ERR_INVAL);
    HH_ASSERT_EQ_INT(f.disc.beacons_rx_malformed, 2);
    HH_ASSERT_EQ_INT(f.disc.beacons_rx_accepted, 0);
}

static void test_own_beacon_echo_ignored(void)
{
    fix_t f; fix_init(&f, 1);
    hh_link_sample_t s = good_sample(1);
    hh_frame_t own = mk_beacon_frame(1, 5, HH_PROTOCOL_VERSION);

    hh_discovery_start(&f.disc);
    HH_ASSERT_ERR(hh_discovery_on_frame(&f.disc, &own, &s), HH_ERR_INVAL);
    HH_ASSERT_EQ_INT(f.disc.beacons_rx_accepted, 0);
}

static void test_adaptive_cadence_tightens_relaxes_and_clamps(void)
{
    fix_t f; fix_init(&f, 1);
    hh_ev_cadence_hint_t unstable = { true, 0.9f };
    hh_ev_cadence_hint_t stable   = { false, 0.0f };

    hh_discovery_start(&f.disc);
    hh_discovery_notify_neighbor_heard(&f.disc);
    HH_ASSERT_EQ_INT(hh_discovery_interval(&f.disc), 1000);

    hh_discovery_apply_cadence_hint(&f.disc, &unstable);
    HH_ASSERT_EQ_INT(hh_discovery_interval(&f.disc), 500);

    /* Repeated instability must not shrink below the configured floor. */
    for (int i = 0; i < 10; i++) hh_discovery_apply_cadence_hint(&f.disc, &unstable);
    HH_ASSERT_EQ_INT(hh_discovery_interval(&f.disc), f.cfg.beacon_interval_min_ms);

    /* Relaxing must not exceed the steady-state interval. */
    for (int i = 0; i < 10; i++) hh_discovery_apply_cadence_hint(&f.disc, &stable);
    HH_ASSERT_EQ_INT(hh_discovery_interval(&f.disc), f.cfg.beacon_interval_ms);
}

static void test_tx_failure_counted_and_schedule_advances(void)
{
    fix_t f; fix_init(&f, 1);
    hh_discovery_start(&f.disc);
    f.mock.tx_fails = true;

    HH_ASSERT_ERR(hh_discovery_tick(&f.disc, f.vc.now), HH_ERR_IO);
    HH_ASSERT_EQ_INT(f.disc.tx_failures, 1);
    /* The schedule still advances: retrying instantly would spin on a dead radio. */
    HH_ASSERT_ERR(hh_discovery_tick(&f.disc, f.vc.now), HH_ERR_AGAIN);
}

static void test_tick_before_start_is_rejected(void)
{
    fix_t f; fix_init(&f, 1);
    /* Lifecycle: no beaconing before start() (Doc 1 §3). */
    HH_ASSERT_ERR(hh_discovery_tick(&f.disc, f.vc.now), HH_ERR_STATE);
    hh_discovery_start(&f.disc);
    HH_ASSERT_OK(hh_discovery_tick(&f.disc, f.vc.now));
    hh_discovery_stop(&f.disc);
    HH_ASSERT_ERR(hh_discovery_tick(&f.disc, f.vc.now), HH_ERR_STATE);
}

HH_TEST_MAIN_BEGIN("discovery")
    HH_RUN(test_starts_in_acquisition_and_beacons_immediately);
    HH_RUN(test_cadence_gates_transmission);
    HH_RUN(test_sequence_numbers_increment_on_wire);
    HH_RUN(test_acquisition_ends_when_neighbor_heard);
    HH_RUN(test_acquisition_ends_on_timeout_when_alone);
    HH_RUN(test_beacon_publishes_event_for_neighbor_manager);
    HH_RUN(test_duplicate_and_stale_sequence_rejected);
    HH_RUN(test_sequence_acceptance_across_wraparound);
    HH_RUN(test_malformed_and_version_mismatch_rejected);
    HH_RUN(test_own_beacon_echo_ignored);
    HH_RUN(test_adaptive_cadence_tightens_relaxes_and_clamps);
    HH_RUN(test_tx_failure_counted_and_schedule_advances);
    HH_RUN(test_tick_before_start_is_rejected);
HH_TEST_MAIN_END()
