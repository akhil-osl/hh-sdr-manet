/*
 * radiod state machine: valid lifecycle, invalid transitions, and recovery
 * from an injected fault. Exercises hh_radiod_handle_request directly
 * (no socket) so state-machine behavior is tested independent of IPC
 * framing; tests/radiod/test_radiod_daemon.c covers the socket path.
 */
#include "hhsdr/radiod/mock_backend.h"
#include "hhsdr/radiod/radiod.h"
#include "hh_test.h"
#include <string.h>

static hh_time_ms_t g_now;
static hh_time_ms_t fake_now(void *ctx) { (void)ctx; return g_now; }
static const hh_clock_t g_clock = { fake_now, NULL };

static void fault_hook(void *ctx, hh_rc_fault_t fault)
{
    hh_mock_backend_inject_fault((hh_mock_backend_t *)ctx, fault);
}

static void setup(hh_mock_backend_t *m, hh_radio_t *r, hh_radiod_t *d)
{
    g_now = 0;
    hh_mock_backend_init(m, r);
    HH_ASSERT_OK(hh_radiod_init(d, r, &g_clock));
    hh_radiod_set_fault_hook(d, fault_hook, m);
}

static hh_rc_response_t call(hh_radiod_t *d, hh_rc_cmd_t cmd, hh_node_id_t node_id,
                             uint32_t channel, hh_rc_fault_t fault)
{
    hh_rc_request_t req;
    hh_rc_response_t resp;
    memset(&req, 0, sizeof req);
    req.cmd = cmd; req.node_id = node_id; req.channel = channel; req.fault = fault;
    hh_radiod_handle_request(d, &req, &resp);
    return resp;
}

static void test_response_state_reflects_current_state(void)
{
    hh_mock_backend_t m; hh_radio_t r; hh_radiod_t d;
    setup(&m, &r, &d);

    /* Non-STATUS responses must also echo the resulting state, not just
     * leave it zero-initialized to CREATED. */
    hh_rc_response_t resp = call(&d, HH_RC_CMD_INIT, 0, 0, 0);
    HH_ASSERT(resp.ok);
    HH_ASSERT_EQ_INT(resp.state, HH_RC_STATE_INITIALIZED);

    resp = call(&d, HH_RC_CMD_CONFIGURE, 1, 0, 0);
    HH_ASSERT(resp.ok);
    HH_ASSERT_EQ_INT(resp.state, HH_RC_STATE_CONFIGURED);

    resp = call(&d, HH_RC_CMD_START, 0, 0, 0);
    HH_ASSERT(resp.ok);
    HH_ASSERT_EQ_INT(resp.state, HH_RC_STATE_RUNNING);
}

static void test_full_valid_lifecycle(void)
{
    hh_mock_backend_t m; hh_radio_t r; hh_radiod_t d;
    setup(&m, &r, &d);

    HH_ASSERT(call(&d, HH_RC_CMD_INIT, 0, 0, 0).ok);
    HH_ASSERT_EQ_INT(d.state, HH_RC_STATE_INITIALIZED);

    HH_ASSERT(call(&d, HH_RC_CMD_CONFIGURE, 1, 0, 0).ok);
    HH_ASSERT_EQ_INT(d.state, HH_RC_STATE_CONFIGURED);

    HH_ASSERT(call(&d, HH_RC_CMD_START, 0, 0, 0).ok);
    HH_ASSERT_EQ_INT(d.state, HH_RC_STATE_RUNNING);

    HH_ASSERT(call(&d, HH_RC_CMD_SET_CHANNEL, 0, 6, 0).ok);

    hh_rc_response_t st = call(&d, HH_RC_CMD_STATUS, 0, 0, 0);
    HH_ASSERT(st.ok);
    HH_ASSERT(st.operational);
    HH_ASSERT_EQ_INT(st.channel, 6);

    HH_ASSERT(call(&d, HH_RC_CMD_STOP, 0, 0, 0).ok);
    HH_ASSERT_EQ_INT(d.state, HH_RC_STATE_STOPPED);

    HH_ASSERT(call(&d, HH_RC_CMD_SHUTDOWN, 0, 0, 0).ok);
    HH_ASSERT_EQ_INT(d.state, HH_RC_STATE_RELEASED);
    HH_ASSERT(hh_radiod_shutdown_requested(&d));
}

static void test_start_before_configuration_rejected(void)
{
    hh_mock_backend_t m; hh_radio_t r; hh_radiod_t d;
    setup(&m, &r, &d);

    hh_rc_response_t resp = call(&d, HH_RC_CMD_START, 0, 0, 0);
    HH_ASSERT(!resp.ok);
    HH_ASSERT_EQ_INT(resp.reason, HH_ERR_STATE);
    HH_ASSERT_EQ_INT(d.state, HH_RC_STATE_CREATED);
}

static void test_start_twice_rejected(void)
{
    hh_mock_backend_t m; hh_radio_t r; hh_radiod_t d;
    setup(&m, &r, &d);
    call(&d, HH_RC_CMD_INIT, 0, 0, 0);
    call(&d, HH_RC_CMD_CONFIGURE, 1, 0, 0);
    HH_ASSERT(call(&d, HH_RC_CMD_START, 0, 0, 0).ok);

    hh_rc_response_t resp = call(&d, HH_RC_CMD_START, 0, 0, 0);
    HH_ASSERT(!resp.ok);
    HH_ASSERT_EQ_INT(resp.reason, HH_ERR_STATE);
}

static void test_stop_while_not_running_rejected(void)
{
    hh_mock_backend_t m; hh_radio_t r; hh_radiod_t d;
    setup(&m, &r, &d);
    call(&d, HH_RC_CMD_INIT, 0, 0, 0);

    hh_rc_response_t resp = call(&d, HH_RC_CMD_STOP, 0, 0, 0);
    HH_ASSERT(!resp.ok);
    HH_ASSERT_EQ_INT(resp.reason, HH_ERR_STATE);
}

static void test_configure_in_invalid_state_rejected(void)
{
    hh_mock_backend_t m; hh_radio_t r; hh_radiod_t d;
    setup(&m, &r, &d);
    /* CREATED: configure requires INITIALIZED first. */
    hh_rc_response_t resp = call(&d, HH_RC_CMD_CONFIGURE, 1, 0, 0);
    HH_ASSERT(!resp.ok);
    HH_ASSERT_EQ_INT(resp.reason, HH_ERR_STATE);
}

static void test_invalid_configuration_rejected(void)
{
    hh_mock_backend_t m; hh_radio_t r; hh_radiod_t d;
    setup(&m, &r, &d);
    call(&d, HH_RC_CMD_INIT, 0, 0, 0);

    /* node_id = HH_NODE_ID_INVALID (0) is not a valid configuration. */
    hh_rc_response_t resp = call(&d, HH_RC_CMD_CONFIGURE, HH_NODE_ID_INVALID, 0, 0);
    HH_ASSERT(!resp.ok);
    HH_ASSERT_EQ_INT(resp.reason, HH_ERR_INVAL);
    HH_ASSERT_EQ_INT(d.state, HH_RC_STATE_INITIALIZED); /* unchanged */
}

static void test_command_while_faulted_then_recovery(void)
{
    hh_mock_backend_t m; hh_radio_t r; hh_radiod_t d;
    setup(&m, &r, &d);
    call(&d, HH_RC_CMD_INIT, 0, 0, 0);
    call(&d, HH_RC_CMD_CONFIGURE, 1, 0, 0);
    call(&d, HH_RC_CMD_START, 0, 0, 0);

    /* Inject a hardware fault; radiod transitions to FAULTED. */
    hh_rc_response_t resp = call(&d, HH_RC_CMD_INJECT_FAULT, 0, 0, HH_RC_FAULT_HW_FAULT);
    HH_ASSERT(resp.ok);
    HH_ASSERT_EQ_INT(d.state, HH_RC_STATE_FAULTED);

    /* SET_CHANNEL is not a permitted command while faulted. */
    hh_rc_response_t rejected = call(&d, HH_RC_CMD_SET_CHANNEL, 0, 2, 0);
    HH_ASSERT(!rejected.ok);
    HH_ASSERT_EQ_INT(rejected.reason, HH_ERR_STATE);

    /* Recovery: clear the fault, returning to RUNNING. */
    hh_rc_response_t cleared = call(&d, HH_RC_CMD_CLEAR_FAULT, 0, 0, 0);
    HH_ASSERT(cleared.ok);
    HH_ASSERT_EQ_INT(d.state, HH_RC_STATE_RUNNING);

    /* STOP is explicitly permitted from FAULTED per the state machine. */
    setup(&m, &r, &d);
    call(&d, HH_RC_CMD_INIT, 0, 0, 0);
    call(&d, HH_RC_CMD_CONFIGURE, 1, 0, 0);
    call(&d, HH_RC_CMD_START, 0, 0, 0);
    call(&d, HH_RC_CMD_INJECT_FAULT, 0, 0, HH_RC_FAULT_HW_FAULT);
    HH_ASSERT(call(&d, HH_RC_CMD_STOP, 0, 0, 0).ok);
    HH_ASSERT_EQ_INT(d.state, HH_RC_STATE_STOPPED);
}

static void test_shutdown_from_each_valid_state(void)
{
    hh_mock_backend_t m; hh_radio_t r; hh_radiod_t d;

    /* From CREATED. */
    setup(&m, &r, &d);
    HH_ASSERT(call(&d, HH_RC_CMD_SHUTDOWN, 0, 0, 0).ok);
    HH_ASSERT_EQ_INT(d.state, HH_RC_STATE_RELEASED);

    /* From INITIALIZED. */
    setup(&m, &r, &d);
    call(&d, HH_RC_CMD_INIT, 0, 0, 0);
    HH_ASSERT(call(&d, HH_RC_CMD_SHUTDOWN, 0, 0, 0).ok);
    HH_ASSERT_EQ_INT(d.state, HH_RC_STATE_RELEASED);

    /* From CONFIGURED. */
    setup(&m, &r, &d);
    call(&d, HH_RC_CMD_INIT, 0, 0, 0);
    call(&d, HH_RC_CMD_CONFIGURE, 1, 0, 0);
    HH_ASSERT(call(&d, HH_RC_CMD_SHUTDOWN, 0, 0, 0).ok);
    HH_ASSERT_EQ_INT(d.state, HH_RC_STATE_RELEASED);

    /* From RUNNING. */
    setup(&m, &r, &d);
    call(&d, HH_RC_CMD_INIT, 0, 0, 0);
    call(&d, HH_RC_CMD_CONFIGURE, 1, 0, 0);
    call(&d, HH_RC_CMD_START, 0, 0, 0);
    HH_ASSERT(call(&d, HH_RC_CMD_SHUTDOWN, 0, 0, 0).ok);
    HH_ASSERT_EQ_INT(d.state, HH_RC_STATE_RELEASED);

    /* From STOPPED. */
    setup(&m, &r, &d);
    call(&d, HH_RC_CMD_INIT, 0, 0, 0);
    call(&d, HH_RC_CMD_CONFIGURE, 1, 0, 0);
    call(&d, HH_RC_CMD_START, 0, 0, 0);
    call(&d, HH_RC_CMD_STOP, 0, 0, 0);
    HH_ASSERT(call(&d, HH_RC_CMD_SHUTDOWN, 0, 0, 0).ok);
    HH_ASSERT_EQ_INT(d.state, HH_RC_STATE_RELEASED);

    /* From FAULTED. */
    setup(&m, &r, &d);
    call(&d, HH_RC_CMD_INIT, 0, 0, 0);
    call(&d, HH_RC_CMD_CONFIGURE, 1, 0, 0);
    call(&d, HH_RC_CMD_START, 0, 0, 0);
    call(&d, HH_RC_CMD_INJECT_FAULT, 0, 0, HH_RC_FAULT_HW_FAULT);
    HH_ASSERT(call(&d, HH_RC_CMD_SHUTDOWN, 0, 0, 0).ok);
    HH_ASSERT_EQ_INT(d.state, HH_RC_STATE_RELEASED);
}

static void test_shutdown_twice_rejected(void)
{
    hh_mock_backend_t m; hh_radio_t r; hh_radiod_t d;
    setup(&m, &r, &d);
    HH_ASSERT(call(&d, HH_RC_CMD_SHUTDOWN, 0, 0, 0).ok);

    hh_rc_response_t resp = call(&d, HH_RC_CMD_SHUTDOWN, 0, 0, 0);
    HH_ASSERT(!resp.ok);
    HH_ASSERT_EQ_INT(resp.reason, HH_ERR_STATE);
}

static void test_status_unavailable_before_init(void)
{
    hh_mock_backend_t m; hh_radio_t r; hh_radiod_t d;
    setup(&m, &r, &d);
    hh_rc_response_t resp = call(&d, HH_RC_CMD_STATUS, 0, 0, 0);
    HH_ASSERT(!resp.ok);
    HH_ASSERT_EQ_INT(resp.reason, HH_ERR_STATE);
}

static void test_requests_counted(void)
{
    hh_mock_backend_t m; hh_radio_t r; hh_radiod_t d;
    setup(&m, &r, &d);
    call(&d, HH_RC_CMD_INIT, 0, 0, 0);
    call(&d, HH_RC_CMD_START, 0, 0, 0); /* rejected: not yet configured */

    hh_rc_response_t stats = call(&d, HH_RC_CMD_STATS, 0, 0, 0);
    HH_ASSERT(stats.ok);
    HH_ASSERT_EQ_INT(stats.requests_total, 3);
    HH_ASSERT_EQ_INT(stats.requests_rejected, 1);
}

HH_TEST_MAIN_BEGIN("radiod_state_machine")
    HH_RUN(test_response_state_reflects_current_state);
    HH_RUN(test_full_valid_lifecycle);
    HH_RUN(test_start_before_configuration_rejected);
    HH_RUN(test_start_twice_rejected);
    HH_RUN(test_stop_while_not_running_rejected);
    HH_RUN(test_configure_in_invalid_state_rejected);
    HH_RUN(test_invalid_configuration_rejected);
    HH_RUN(test_command_while_faulted_then_recovery);
    HH_RUN(test_shutdown_from_each_valid_state);
    HH_RUN(test_shutdown_twice_rejected);
    HH_RUN(test_status_unavailable_before_init);
    HH_RUN(test_requests_counted);
HH_TEST_MAIN_END()
