/*
 * End-to-end radiod process test:
 *   test client -> UNIX socket IPC -> radiod process -> mock backend
 *
 * Spawns the real `radiod` binary as a child process (not an in-process
 * mock object) and drives it exclusively through hh_rc_client_t over the
 * socket, exercising the actual IPC framing and the actual daemon binary.
 */
#include "hhsdr/librc/rc_client.h"
#include "hh_test.h"
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define TEST_SOCK_PATH "/tmp/hh-radiod-test.sock"

static void wait_for_socket(const char *path)
{
    for (int i = 0; i < 200; i++) {
        hh_rc_client_t c;
        if (hh_rc_client_connect(&c, path) == HH_OK) { hh_rc_client_close(&c); return; }
        struct timespec ts = { 0, 10 * 1000 * 1000 };
        nanosleep(&ts, NULL);
    }
}

static pid_t spawn_radiod(const char *sock_path)
{
    pid_t pid = fork();
    if (pid == 0) {
        execl(RADIOD_BINARY_PATH, "radiod", "-s", sock_path, "-v", "error", (char *)NULL);
        _exit(127); /* only reached if exec fails */
    }
    return pid;
}

static void stop_radiod(pid_t pid)
{
    if (pid <= 0) return;
    kill(pid, SIGTERM);
    int status;
    waitpid(pid, &status, 0);
}

static hh_rc_response_t call(hh_rc_client_t *c, hh_rc_cmd_t cmd, hh_node_id_t node_id,
                             uint32_t channel, hh_rc_fault_t fault)
{
    hh_rc_request_t req;
    hh_rc_response_t resp;
    memset(&req, 0, sizeof req);
    memset(&resp, 0, sizeof resp);
    req.cmd = cmd; req.node_id = node_id; req.channel = channel; req.fault = fault;
    if (hh_rc_client_call(c, &req, &resp) != HH_OK) {
        hh_tests_failed++;
        fprintf(stderr, "FAIL %s: hh_rc_client_call transport error\n", hh_current_test);
    }
    return resp;
}

static void test_full_path_lifecycle_and_status(void)
{
    unlink(TEST_SOCK_PATH);
    pid_t pid = spawn_radiod(TEST_SOCK_PATH);
    wait_for_socket(TEST_SOCK_PATH);

    hh_rc_client_t c;
    HH_ASSERT_OK(hh_rc_client_connect(&c, TEST_SOCK_PATH));

    HH_ASSERT(call(&c, HH_RC_CMD_INIT, 0, 0, 0).ok);
    HH_ASSERT(call(&c, HH_RC_CMD_CONFIGURE, 1, 0, 0).ok);
    HH_ASSERT(call(&c, HH_RC_CMD_START, 0, 0, 0).ok);
    HH_ASSERT(call(&c, HH_RC_CMD_SET_CHANNEL, 0, 4, 0).ok);

    hh_rc_response_t st = call(&c, HH_RC_CMD_STATUS, 0, 0, 0);
    HH_ASSERT(st.ok);
    HH_ASSERT(st.operational);
    HH_ASSERT_EQ_INT(st.channel, 4);
    HH_ASSERT_EQ_INT(st.state, HH_RC_STATE_RUNNING);

    HH_ASSERT(call(&c, HH_RC_CMD_SET_CHANNEL, 0, 9, 0).ok);
    st = call(&c, HH_RC_CMD_STATUS, 0, 0, 0);
    HH_ASSERT_EQ_INT(st.channel, 9);

    hh_rc_response_t stats = call(&c, HH_RC_CMD_STATS, 0, 0, 0);
    HH_ASSERT(stats.ok);
    HH_ASSERT(stats.requests_total >= 5);

    /* Invalid transition over the real socket path: start while running. */
    hh_rc_response_t bad = call(&c, HH_RC_CMD_START, 0, 0, 0);
    HH_ASSERT(!bad.ok);
    HH_ASSERT_EQ_INT(bad.reason, HH_ERR_STATE);

    /* Injected fault over the real socket path, then recovery. */
    HH_ASSERT(call(&c, HH_RC_CMD_INJECT_FAULT, 0, 0, HH_RC_FAULT_HW_FAULT).ok);
    st = call(&c, HH_RC_CMD_STATUS, 0, 0, 0);
    HH_ASSERT_EQ_INT(st.state, HH_RC_STATE_FAULTED);
    HH_ASSERT(!st.operational);

    HH_ASSERT(call(&c, HH_RC_CMD_CLEAR_FAULT, 0, 0, 0).ok);
    st = call(&c, HH_RC_CMD_STATUS, 0, 0, 0);
    HH_ASSERT_EQ_INT(st.state, HH_RC_STATE_RUNNING);

    HH_ASSERT(call(&c, HH_RC_CMD_STOP, 0, 0, 0).ok);
    HH_ASSERT(call(&c, HH_RC_CMD_SHUTDOWN, 0, 0, 0).ok);

    hh_rc_client_close(&c);
    stop_radiod(pid);
    unlink(TEST_SOCK_PATH);
}

static void test_client_disconnect_does_not_crash_daemon(void)
{
    unlink(TEST_SOCK_PATH);
    pid_t pid = spawn_radiod(TEST_SOCK_PATH);
    wait_for_socket(TEST_SOCK_PATH);

    hh_rc_client_t c1;
    HH_ASSERT_OK(hh_rc_client_connect(&c1, TEST_SOCK_PATH));
    HH_ASSERT(call(&c1, HH_RC_CMD_INIT, 0, 0, 0).ok);
    hh_rc_client_close(&c1); /* abrupt disconnect, no shutdown command sent */

    struct timespec ts = { 0, 50 * 1000 * 1000 };
    nanosleep(&ts, NULL);

    /* Daemon must still be alive and servicing a fresh connection. */
    hh_rc_client_t c2;
    HH_ASSERT_OK(hh_rc_client_connect(&c2, TEST_SOCK_PATH));
    hh_rc_response_t resp = call(&c2, HH_RC_CMD_STATUS, 0, 0, 0);
    HH_ASSERT(resp.ok); /* INITIALIZED survives the first client's exit */
    hh_rc_client_close(&c2);

    stop_radiod(pid);
    unlink(TEST_SOCK_PATH);
}

static void test_daemon_shutdown_on_command(void)
{
    unlink(TEST_SOCK_PATH);
    pid_t pid = spawn_radiod(TEST_SOCK_PATH);
    wait_for_socket(TEST_SOCK_PATH);

    hh_rc_client_t c;
    HH_ASSERT_OK(hh_rc_client_connect(&c, TEST_SOCK_PATH));
    HH_ASSERT(call(&c, HH_RC_CMD_SHUTDOWN, 0, 0, 0).ok);
    hh_rc_client_close(&c);

    int status;
    pid_t waited = waitpid(pid, &status, 0);
    HH_ASSERT_EQ_INT(waited, pid);
    HH_ASSERT(WIFEXITED(status));
    HH_ASSERT_EQ_INT(WEXITSTATUS(status), 0);

    unlink(TEST_SOCK_PATH);
}

static void test_client_connect_fails_when_daemon_absent(void)
{
    unlink(TEST_SOCK_PATH);
    hh_rc_client_t c;
    HH_ASSERT_ERR(hh_rc_client_connect(&c, TEST_SOCK_PATH), HH_ERR_IO);
}

HH_TEST_MAIN_BEGIN("radiod_daemon_e2e")
    HH_RUN(test_full_path_lifecycle_and_status);
    HH_RUN(test_client_disconnect_does_not_crash_daemon);
    HH_RUN(test_daemon_shutdown_on_command);
    HH_RUN(test_client_connect_fails_when_daemon_absent);
HH_TEST_MAIN_END()
