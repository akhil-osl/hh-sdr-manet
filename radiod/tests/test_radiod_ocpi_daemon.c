/*
 * End-to-end: client -> UNIX socket -> radiod process -> OpenCPI application.
 *
 * Spawns the real radiod binary configured with `backend = ocpi` and drives
 * it through librc only, as radioctl or an ATP script would. It proves that
 * radiod's unchanged state machine owns a real OpenCPI application: start
 * creates and starts it, stop releases it, and a failed start leaves radiod
 * faulted with nothing running.
 *
 * Real time, like test_ocpi_backend: registered only with HH_WITH_OPENCPI.
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

#define SOCK_PATH  "/tmp/hh-radiod-ocpi-test.sock"
#define CFG_PATH   "hh_radiod_ocpi_test.conf"
#define INPUT_FILE "hh_ocpi_test_in.bin"

static void sleep_ms(long ms)
{
    struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

static void make_input_file(void)
{
    FILE *f = fopen(INPUT_FILE, "wb");
    unsigned char block[4096] = { 0 };
    if (!f) return;
    for (int i = 0; i < 16; i++) fwrite(block, 1, sizeof block, f);
    fclose(f);
}

static void write_config(const char *app)
{
    FILE *f = fopen(CFG_PATH, "w");
    if (!f) return;
    fprintf(f, "backend = ocpi\nocpi_app = %s\nlog_level = error\n", app);
    fclose(f);
}

static pid_t spawn_radiod(void)
{
    pid_t pid = fork();
    if (pid == 0) {
        execl(RADIOD_BINARY_PATH, "radiod", "-c", CFG_PATH, "-s", SOCK_PATH, (char *)NULL);
        _exit(127);
    }
    return pid;
}

static bool wait_for_socket(void)
{
    for (int i = 0; i < 300; i++) {
        hh_rc_client_t c;
        if (hh_rc_client_connect(&c, SOCK_PATH) == HH_OK) { hh_rc_client_close(&c); return true; }
        sleep_ms(10);
    }
    return false;
}

static void stop_radiod(pid_t pid)
{
    int status;
    if (pid <= 0) return;
    kill(pid, SIGTERM);
    waitpid(pid, &status, 0);
}

static hh_rc_response_t call(hh_rc_client_t *c, hh_rc_cmd_t cmd, hh_node_id_t node_id,
                             uint32_t channel)
{
    hh_rc_request_t req;
    hh_rc_response_t resp;
    memset(&req, 0, sizeof req);
    memset(&resp, 0, sizeof resp);
    req.cmd = cmd; req.node_id = node_id; req.channel = channel;
    if (hh_rc_client_call(c, &req, &resp) != HH_OK) {
        hh_tests_failed++;
        fprintf(stderr, "FAIL %s: transport error on %s\n", hh_current_test, hh_rc_cmd_str(cmd));
    }
    return resp;
}

static void test_radiod_owns_opencpi_application(void)
{
    hh_rc_client_t c;
    hh_rc_response_t r;
    pid_t pid;

    write_config(HH_OCPI_FIXTURE_DIR "/loop_app.xml");
    unlink(SOCK_PATH);
    pid = spawn_radiod();
    if (!wait_for_socket()) { stop_radiod(pid); HH_FAIL("radiod did not come up"); }
    HH_ASSERT_OK(hh_rc_client_connect(&c, SOCK_PATH));

    HH_ASSERT(call(&c, HH_RC_CMD_INIT, 0, 0).ok);
    HH_ASSERT(call(&c, HH_RC_CMD_CONFIGURE, 42, 0).ok);
    HH_ASSERT(call(&c, HH_RC_CMD_START, 0, 0).ok);

    r = call(&c, HH_RC_CMD_STATUS, 0, 0);
    HH_ASSERT_EQ_INT(r.state, HH_RC_STATE_RUNNING);
    HH_ASSERT(r.operational);

    /* Still running after a few control-loop ticks of backend polling. */
    sleep_ms(100);
    r = call(&c, HH_RC_CMD_STATUS, 0, 0);
    HH_ASSERT(r.operational);

    /* Refused, not faked: no channel plan exists (U-14). */
    r = call(&c, HH_RC_CMD_SET_CHANNEL, 0, 3);
    HH_ASSERT(!r.ok);
    HH_ASSERT_EQ_INT(r.reason, HH_ERR_UNSUPPORTED);

    HH_ASSERT(call(&c, HH_RC_CMD_STOP, 0, 0).ok);
    r = call(&c, HH_RC_CMD_STATUS, 0, 0);
    HH_ASSERT_EQ_INT(r.state, HH_RC_STATE_STOPPED);
    HH_ASSERT(!r.operational);

    /* stop -> start re-creates the application. */
    HH_ASSERT(call(&c, HH_RC_CMD_START, 0, 0).ok);
    HH_ASSERT(call(&c, HH_RC_CMD_STATUS, 0, 0).operational);

    HH_ASSERT(call(&c, HH_RC_CMD_SHUTDOWN, 0, 0).ok);
    hh_rc_client_close(&c);
    stop_radiod(pid);
    unlink(SOCK_PATH);
}

static void test_failed_start_faults_radiod(void)
{
    hh_rc_client_t c;
    hh_rc_response_t r;
    pid_t pid;

    write_config(HH_OCPI_FIXTURE_DIR "/does_not_exist.xml");
    unlink(SOCK_PATH);
    pid = spawn_radiod();
    if (!wait_for_socket()) { stop_radiod(pid); HH_FAIL("radiod did not come up"); }
    HH_ASSERT_OK(hh_rc_client_connect(&c, SOCK_PATH));

    HH_ASSERT(call(&c, HH_RC_CMD_INIT, 0, 0).ok);
    HH_ASSERT(call(&c, HH_RC_CMD_CONFIGURE, 42, 0).ok);
    r = call(&c, HH_RC_CMD_START, 0, 0);
    HH_ASSERT(!r.ok);
    HH_ASSERT_EQ_INT(r.reason, HH_ERR_IO);

    r = call(&c, HH_RC_CMD_STATUS, 0, 0);
    HH_ASSERT_EQ_INT(r.state, HH_RC_STATE_FAULTED);
    HH_ASSERT(!r.operational);

    hh_rc_client_close(&c);
    stop_radiod(pid);
    unlink(SOCK_PATH);
}

HH_TEST_MAIN_BEGIN("radiod_ocpi_daemon_e2e")
    make_input_file();
    HH_RUN(test_radiod_owns_opencpi_application);
    HH_RUN(test_failed_start_faults_radiod);
    unlink(CFG_PATH);
HH_TEST_MAIN_END()
