/*
 * radiod mock backend: hh_radio_ops_t contract compliance and fault
 * injection paths (TX failure, HW fault, backend I/O failure).
 */
#include "hhsdr/radiod/mock_backend.h"
#include "hh_test.h"
#include <string.h>

static void test_lifecycle_and_transmit(void)
{
    hh_mock_backend_t m;
    hh_radio_t r;
    hh_frame_t f;
    hh_radio_status_t st;

    hh_mock_backend_init(&m, &r);
    memset(&f, 0, sizeof f);

    HH_ASSERT_ERR(hh_radio_transmit(&r, &f), HH_ERR_STATE); /* before open */
    HH_ASSERT_OK(hh_radio_open(&r));
    HH_ASSERT_OK(hh_radio_transmit(&r, &f));
    HH_ASSERT_OK(hh_radio_get_status(&r, &st));
    HH_ASSERT(st.operational);
    HH_ASSERT_EQ_INT(st.frames_tx, 1);
}

static void test_tx_failure_fault(void)
{
    hh_mock_backend_t m;
    hh_radio_t r;
    hh_frame_t f;
    hh_radio_status_t st;

    hh_mock_backend_init(&m, &r);
    memset(&f, 0, sizeof f);
    hh_radio_open(&r);

    hh_mock_backend_inject_fault(&m, HH_RC_FAULT_TX_FAILURE);
    HH_ASSERT_ERR(hh_radio_transmit(&r, &f), HH_ERR_IO);
    HH_ASSERT_OK(hh_radio_get_status(&r, &st));
    HH_ASSERT_EQ_INT(st.tx_errors, 1);

    hh_mock_backend_inject_fault(&m, HH_RC_FAULT_NONE);
    HH_ASSERT_OK(hh_radio_transmit(&r, &f));
}

static void test_hw_fault_marks_not_operational(void)
{
    hh_mock_backend_t m;
    hh_radio_t r;
    hh_radio_status_t st;

    hh_mock_backend_init(&m, &r);
    hh_radio_open(&r);
    HH_ASSERT_OK(hh_radio_get_status(&r, &st));
    HH_ASSERT(st.operational);

    hh_mock_backend_inject_fault(&m, HH_RC_FAULT_HW_FAULT);
    HH_ASSERT_OK(hh_radio_get_status(&r, &st));
    HH_ASSERT(!st.operational);
    HH_ASSERT_EQ_INT(hh_mock_backend_active_fault(&m), HH_RC_FAULT_HW_FAULT);
}

static void test_backend_io_fault_blocks_channel_change(void)
{
    hh_mock_backend_t m;
    hh_radio_t r;

    hh_mock_backend_init(&m, &r);
    hh_radio_open(&r);
    HH_ASSERT_OK(hh_radio_set_channel(&r, 4));

    hh_mock_backend_inject_fault(&m, HH_RC_FAULT_BACKEND_IO);
    HH_ASSERT_ERR(hh_radio_set_channel(&r, 5), HH_ERR_IO);
}

static void test_set_channel_before_open_fails(void)
{
    hh_mock_backend_t m;
    hh_radio_t r;

    hh_mock_backend_init(&m, &r);
    HH_ASSERT_ERR(hh_radio_set_channel(&r, 1), HH_ERR_STATE);
}

static void test_link_metrics_unsupported(void)
{
    hh_mock_backend_t m;
    hh_radio_t r;
    hh_link_sample_t s;

    hh_mock_backend_init(&m, &r);
    hh_radio_open(&r);
    HH_ASSERT_ERR(hh_radio_get_link_metrics(&r, 1, &s), HH_ERR_UNSUPPORTED);
}

HH_TEST_MAIN_BEGIN("mock_backend")
    HH_RUN(test_lifecycle_and_transmit);
    HH_RUN(test_tx_failure_fault);
    HH_RUN(test_hw_fault_marks_not_operational);
    HH_RUN(test_backend_io_fault_blocks_channel_change);
    HH_RUN(test_set_channel_before_open_fails);
    HH_RUN(test_link_metrics_unsupported);
HH_TEST_MAIN_END()
