/*
 * rc.h wire-encoding tests: request/response parse and format round trips.
 */
#include "hhsdr/protocol/rc.h"
#include "hh_test.h"
#include <string.h>

static void test_request_roundtrip_configure(void)
{
    hh_rc_request_t req, out;
    char buf[HH_RC_MAX_LINE];

    memset(&req, 0, sizeof req);
    req.cmd = HH_RC_CMD_CONFIGURE;
    req.node_id = 7;
    req.channel = 3;

    HH_ASSERT(hh_rc_request_format(&req, buf, sizeof buf) > 0);
    HH_ASSERT_OK(hh_rc_request_parse(buf, &out));
    HH_ASSERT_EQ_INT(out.cmd, HH_RC_CMD_CONFIGURE);
    HH_ASSERT_EQ_INT(out.node_id, 7);
    HH_ASSERT_EQ_INT(out.channel, 3);
}

static void test_request_parse_simple_commands(void)
{
    hh_rc_request_t out;
    HH_ASSERT_OK(hh_rc_request_parse("status\n", &out));
    HH_ASSERT_EQ_INT(out.cmd, HH_RC_CMD_STATUS);
    HH_ASSERT_OK(hh_rc_request_parse("start", &out)); /* no trailing newline */
    HH_ASSERT_EQ_INT(out.cmd, HH_RC_CMD_START);
}

static void test_request_parse_fault_kind(void)
{
    hh_rc_request_t out;
    HH_ASSERT_OK(hh_rc_request_parse("inject_fault kind=tx_failure\n", &out));
    HH_ASSERT_EQ_INT(out.cmd, HH_RC_CMD_INJECT_FAULT);
    HH_ASSERT_EQ_INT(out.fault, HH_RC_FAULT_TX_FAILURE);
}

static void test_request_parse_rejects_malformed(void)
{
    hh_rc_request_t out;
    HH_ASSERT_ERR(hh_rc_request_parse("", &out), HH_ERR_INVAL);
    HH_ASSERT_ERR(hh_rc_request_parse("bogus_cmd\n", &out), HH_ERR_INVAL);
    HH_ASSERT_ERR(hh_rc_request_parse("configure node_id\n", &out), HH_ERR_INVAL); /* no '=' */
    HH_ASSERT_ERR(hh_rc_request_parse("configure unknown_key=1\n", &out), HH_ERR_INVAL);
    HH_ASSERT_ERR(hh_rc_request_parse("inject_fault kind=not_a_fault\n", &out), HH_ERR_INVAL);
    HH_ASSERT_ERR(hh_rc_request_parse(NULL, &out), HH_ERR_INVAL);
}

static void test_response_roundtrip_status(void)
{
    hh_rc_response_t resp, out;
    char buf[HH_RC_MAX_LINE];

    memset(&resp, 0, sizeof resp);
    resp.ok = true;
    resp.cmd = HH_RC_CMD_STATUS;
    resp.state = HH_RC_STATE_RUNNING;
    resp.operational = true;
    resp.channel = 5;
    resp.frequency_hz = 2412000000.0f;
    resp.waveform_id = 2;

    HH_ASSERT(hh_rc_response_format(&resp, buf, sizeof buf) > 0);
    HH_ASSERT_OK(hh_rc_response_parse(buf, &out));
    HH_ASSERT(out.ok);
    HH_ASSERT_EQ_INT(out.cmd, HH_RC_CMD_STATUS);
    HH_ASSERT_EQ_INT(out.state, HH_RC_STATE_RUNNING);
    HH_ASSERT(out.operational);
    HH_ASSERT_EQ_INT(out.channel, 5);
    HH_ASSERT_NEAR(out.frequency_hz, 2412000000.0, 1.0);
}

static void test_response_roundtrip_stats(void)
{
    hh_rc_response_t resp, out;
    char buf[HH_RC_MAX_LINE];

    memset(&resp, 0, sizeof resp);
    resp.ok = true;
    resp.cmd = HH_RC_CMD_STATS;
    resp.frames_tx = 10;
    resp.frames_rx = 20;
    resp.tx_errors = 1;
    resp.rx_errors = 2;
    resp.requests_total = 30;
    resp.requests_rejected = 3;

    HH_ASSERT(hh_rc_response_format(&resp, buf, sizeof buf) > 0);
    HH_ASSERT_OK(hh_rc_response_parse(buf, &out));
    HH_ASSERT_EQ_INT(out.frames_tx, 10);
    HH_ASSERT_EQ_INT(out.frames_rx, 20);
    HH_ASSERT_EQ_INT(out.tx_errors, 1);
    HH_ASSERT_EQ_INT(out.rx_errors, 2);
    HH_ASSERT_EQ_INT(out.requests_total, 30);
    HH_ASSERT_EQ_INT(out.requests_rejected, 3);
}

static void test_response_error_roundtrip(void)
{
    hh_rc_response_t resp, out;
    char buf[HH_RC_MAX_LINE];

    memset(&resp, 0, sizeof resp);
    resp.ok = false;
    resp.cmd = HH_RC_CMD_START;
    resp.reason = HH_ERR_STATE;

    HH_ASSERT(hh_rc_response_format(&resp, buf, sizeof buf) > 0);
    HH_ASSERT_OK(hh_rc_response_parse(buf, &out));
    HH_ASSERT(!out.ok);
    HH_ASSERT_EQ_INT(out.cmd, HH_RC_CMD_START);
    HH_ASSERT_EQ_INT(out.reason, HH_ERR_STATE);
}

static void test_response_parse_rejects_malformed(void)
{
    hh_rc_response_t out;
    HH_ASSERT_ERR(hh_rc_response_parse("", &out), HH_ERR_INVAL);
    HH_ASSERT_ERR(hh_rc_response_parse("maybe status\n", &out), HH_ERR_INVAL);
    HH_ASSERT_ERR(hh_rc_response_parse("ok bogus_cmd\n", &out), HH_ERR_INVAL);
}

static void test_cmd_and_fault_str_are_total(void)
{
    for (int i = 0; i < HH_RC_CMD__MAX; i++) {
        HH_ASSERT(strcmp(hh_rc_cmd_str((hh_rc_cmd_t)i), "unknown") != 0);
    }
    HH_ASSERT_EQ_STR(hh_rc_cmd_str((hh_rc_cmd_t)999), "unknown");
    HH_ASSERT_EQ_STR(hh_rc_fault_str((hh_rc_fault_t)999), "unknown");
    HH_ASSERT_EQ_STR(hh_rc_state_str((hh_rc_state_t)999), "unknown");
}

HH_TEST_MAIN_BEGIN("rc_protocol")
    HH_RUN(test_request_roundtrip_configure);
    HH_RUN(test_request_parse_simple_commands);
    HH_RUN(test_request_parse_fault_kind);
    HH_RUN(test_request_parse_rejects_malformed);
    HH_RUN(test_response_roundtrip_status);
    HH_RUN(test_response_roundtrip_stats);
    HH_RUN(test_response_error_roundtrip);
    HH_RUN(test_response_parse_rejects_malformed);
    HH_RUN(test_cmd_and_fault_str_are_total);
HH_TEST_MAIN_END()
