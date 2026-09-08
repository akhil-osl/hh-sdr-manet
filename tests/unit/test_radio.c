/*
 * Radio abstraction boundary tests.
 *
 * Two things are verified here:
 *  1. The hardware adapter is honest — it reports NOT_IMPLEMENTED rather than
 *     pretending to work, so a missing FPGA contract cannot hide.
 *  2. The mock adapter satisfies the same contract, which is what lets the
 *     production stack run under test with no simulation-specific code path.
 */
#include "hhsdr/radio/hw_adapter.h"
#include "hhsdr/radio/radio.h"
#include "mock_radio.h"
#include "hh_test.h"
#include <string.h>

static void test_hw_adapter_reports_not_implemented(void)
{
    hh_hw_adapter_t hw;
    hh_radio_t r;
    hh_radio_status_t st;
    hh_link_sample_t s;
    hh_frame_t f;

    memset(&f, 0, sizeof f);
    hh_hw_adapter_init(&hw, &r);
    HH_ASSERT(hh_hw_adapter_is_stub());

    /* Every hardware-dependent operation fails loudly and identifiably. */
    HH_ASSERT_ERR(hh_radio_open(&r), HH_ERR_NOT_IMPLEMENTED);
    HH_ASSERT_ERR(hh_radio_transmit(&r, &f), HH_ERR_NOT_IMPLEMENTED);
    HH_ASSERT_ERR(hh_radio_set_rx_callback(&r, NULL, NULL), HH_ERR_NOT_IMPLEMENTED);
    HH_ASSERT_ERR(hh_radio_get_status(&r, &st), HH_ERR_NOT_IMPLEMENTED);
    HH_ASSERT_ERR(hh_radio_get_link_metrics(&r, 2, &s), HH_ERR_NOT_IMPLEMENTED);
    HH_ASSERT_ERR(hh_radio_set_channel(&r, 3), HH_ERR_NOT_IMPLEMENTED);
    HH_ASSERT_ERR(hh_radio_poll(&r, 0), HH_ERR_NOT_IMPLEMENTED);

    /* Status reports "not operational" rather than fabricated counters. */
    HH_ASSERT(!st.operational);
}

typedef struct { int count; hh_frame_t last; hh_link_sample_t last_m; } rx_rec_t;

static void on_rx(const hh_frame_t *f, const hh_link_sample_t *m, void *ctx)
{
    rx_rec_t *r = ctx;
    r->count++;
    r->last = *f;
    r->last_m = *m;
}

static void test_mock_implements_same_contract(void)
{
    mock_radio_t m;
    hh_radio_t r;
    hh_frame_t f;
    hh_radio_status_t st;

    mock_radio_init(&m, "n1", &r);
    memset(&f, 0, sizeof f);
    f.kind = HH_FRAME_BEACON; f.src = 1; f.len = 4;

    /* Transmit before open must fail: the adapter enforces its own lifecycle. */
    HH_ASSERT_ERR(hh_radio_transmit(&r, &f), HH_ERR_IO);
    HH_ASSERT_OK(hh_radio_open(&r));
    HH_ASSERT_OK(hh_radio_transmit(&r, &f));
    HH_ASSERT_EQ_INT(mock_radio_tx_count_kind(&m, HH_FRAME_BEACON), 1);

    HH_ASSERT_OK(hh_radio_get_status(&r, &st));
    HH_ASSERT(st.operational);
    HH_ASSERT_EQ_INT(st.frames_tx, 1);
}

static void test_mock_delivers_rx_at_virtual_time(void)
{
    mock_radio_t m;
    hh_radio_t r;
    rx_rec_t rec = {0};
    hh_frame_t f;
    hh_link_sample_t s;

    mock_radio_init(&m, "n1", &r);
    hh_radio_open(&r);
    hh_radio_set_rx_callback(&r, on_rx, &rec);

    memset(&f, 0, sizeof f);
    f.kind = HH_FRAME_BEACON; f.src = 2;
    memset(&s, 0, sizeof s);
    s.neighbor_id = 2; s.rssi = -60.0f; s.snr = 20.0f;

    mock_radio_enqueue_rx(&m, &f, &s, 1000);

    /* Delivery is deterministic in virtual time: nothing before deliver_at. */
    hh_radio_poll(&r, 500);
    HH_ASSERT_EQ_INT(rec.count, 0);
    hh_radio_poll(&r, 1000);
    HH_ASSERT_EQ_INT(rec.count, 1);
    HH_ASSERT_EQ_INT(rec.last.src, 2);
    HH_ASSERT_NEAR(rec.last_m.rssi, -60.0, 0.01);
    /* Delivered exactly once. */
    hh_radio_poll(&r, 2000);
    HH_ASSERT_EQ_INT(rec.count, 1);
}

static void test_mock_models_radio_failure(void)
{
    mock_radio_t m;
    hh_radio_t r;
    rx_rec_t rec = {0};
    hh_frame_t f;
    hh_link_sample_t s;

    mock_radio_init(&m, "n1", &r);
    hh_radio_open(&r);
    hh_radio_set_rx_callback(&r, on_rx, &rec);
    memset(&f, 0, sizeof f); memset(&s, 0, sizeof s);
    mock_radio_enqueue_rx(&m, &f, &s, 100);

    /* Own-radio failure: no tx, no rx, status reports it. */
    m.operational = false;
    HH_ASSERT_ERR(hh_radio_transmit(&r, &f), HH_ERR_IO);
    hh_radio_poll(&r, 200);
    HH_ASSERT_EQ_INT(rec.count, 0);
}

static void test_mock_link_metrics_and_channel(void)
{
    mock_radio_t m;
    hh_radio_t r;
    hh_link_sample_t s, got;

    mock_radio_init(&m, "n1", &r);
    hh_radio_open(&r);

    memset(&s, 0, sizeof s);
    s.neighbor_id = 5; s.rssi = -70.0f; s.per = 0.1f;
    mock_radio_set_peer_metrics(&m, 5, &s);

    HH_ASSERT_OK(hh_radio_get_link_metrics(&r, 5, &got));
    HH_ASSERT_NEAR(got.rssi, -70.0, 0.01);
    HH_ASSERT_ERR(hh_radio_get_link_metrics(&r, 99, &got), HH_ERR_NOTFOUND);

    HH_ASSERT_OK(hh_radio_set_channel(&r, 11));
    HH_ASSERT_EQ_INT(m.channel, 11);

    /* An adapter without channel agility must say so, not silently ignore. */
    m.supports_channel_change = false;
    HH_ASSERT_ERR(hh_radio_set_channel(&r, 12), HH_ERR_UNSUPPORTED);
}

static void test_null_handle_is_safe(void)
{
    hh_frame_t f;
    memset(&f, 0, sizeof f);
    /* A partially-populated adapter must degrade, never crash. */
    HH_ASSERT_ERR(hh_radio_open(NULL), HH_ERR_INVAL);
    HH_ASSERT_ERR(hh_radio_transmit(NULL, &f), HH_ERR_INVAL);
}

HH_TEST_MAIN_BEGIN("radio")
    HH_RUN(test_hw_adapter_reports_not_implemented);
    HH_RUN(test_mock_implements_same_contract);
    HH_RUN(test_mock_delivers_rx_at_virtual_time);
    HH_RUN(test_mock_models_radio_failure);
    HH_RUN(test_mock_link_metrics_and_channel);
    HH_RUN(test_null_handle_is_safe);
HH_TEST_MAIN_END()
