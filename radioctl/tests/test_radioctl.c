/*
 * End-to-end radioctl test:
 *   radioctl binary -> librc -> UNIX socket -> radiod binary -> mock backend
 *
 * Both real binaries are executed as child processes. Nothing here links the
 * daemon or reaches into radiod's internals, which is the point: this is the
 * public control path the architecture requires test automation to use, driven
 * exactly as an operator or an ATB/BIT script would drive it.
 *
 * Assertions are made on radioctl's exit status and on its stdout, because
 * those are the CLI's actual contract with its callers.
 */
#include "hh_test.h"
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "hhsdr/librc/rc_client.h"

#define TEST_SOCK_PATH "/tmp/hh-radioctl-test.sock"

#define EXIT_USAGE     2
#define EXIT_TRANSPORT 3
#define EXIT_DENIED    4

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
        _exit(127);
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

/* Run radioctl with up to three trailing arguments, capturing stdout. A NULL
 * argument ends the list. Returns the process exit status, or -1 if it could
 * not be run.
 *
 * argv is built explicitly rather than with execl(), because passing a NULL
 * in the middle of execl()'s variadic list is undefined behaviour. */
static int run_radioctl(char *out, size_t cap, const char *a, const char *b, const char *c)
{
    int fds[2];
    if (pipe(fds) != 0) return -1;

    /* Writable copies: execv takes char *const[], and the project builds with
     * -Wcast-qual, so casting away const on string literals is not an option. */
    char a0[] = "radioctl";
    char a1[] = "-s";
    char a2[] = TEST_SOCK_PATH;
    char a3[64], a4[64], a5[64];
    char *argv[8];
    int argn = 0;

    argv[argn++] = a0;
    argv[argn++] = a1;
    argv[argn++] = a2;
    if (a) { snprintf(a3, sizeof a3, "%s", a); argv[argn++] = a3; }
    if (b) { snprintf(a4, sizeof a4, "%s", b); argv[argn++] = a4; }
    if (c) { snprintf(a5, sizeof a5, "%s", c); argv[argn++] = a5; }
    argv[argn] = NULL;

    pid_t pid = fork();
    if (pid == 0) {
        close(fds[0]);
        dup2(fds[1], STDOUT_FILENO);
        close(fds[1]);
        execv(RADIOCTL_BINARY_PATH, argv);
        _exit(127);
    }
    close(fds[1]);

    size_t got = 0;
    if (out && cap) {
        ssize_t r;
        while (got + 1 < cap && (r = read(fds[0], out + got, cap - got - 1)) > 0)
            got += (size_t)r;
        out[got] = '\0';
    }
    /* Drain anything left so the child never blocks on a full pipe. */
    char sink[256];
    while (read(fds[0], sink, sizeof sink) > 0) { }
    close(fds[0]);

    int status = 0;
    waitpid(pid, &status, 0);
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

/* Drive the whole documented lifecycle through the CLI alone. */
static void test_lifecycle_through_cli(void)
{
    char out[1024];

    unlink(TEST_SOCK_PATH);
    pid_t pid = spawn_radiod(TEST_SOCK_PATH);
    wait_for_socket(TEST_SOCK_PATH);

    HH_ASSERT_EQ_INT(run_radioctl(out, sizeof out, "init", NULL, NULL), 0);
    HH_ASSERT(strstr(out, "state=initialized") != NULL);

    HH_ASSERT_EQ_INT(run_radioctl(out, sizeof out, "configure", "7", NULL), 0);
    HH_ASSERT(strstr(out, "state=configured") != NULL);

    HH_ASSERT_EQ_INT(run_radioctl(out, sizeof out, "start", NULL, NULL), 0);
    HH_ASSERT(strstr(out, "state=running") != NULL);

    /* Hyphenated CLI spelling maps onto the protocol's set_channel verb. */
    HH_ASSERT_EQ_INT(run_radioctl(out, sizeof out, "set-channel", "4", NULL), 0);

    HH_ASSERT_EQ_INT(run_radioctl(out, sizeof out, "status", NULL, NULL), 0);
    HH_ASSERT(strstr(out, "state=running")  != NULL);
    HH_ASSERT(strstr(out, "operational=1")  != NULL);
    HH_ASSERT(strstr(out, "channel=4")      != NULL);

    HH_ASSERT_EQ_INT(run_radioctl(out, sizeof out, "stats", NULL, NULL), 0);
    HH_ASSERT(strstr(out, "requests_total=") != NULL);
    HH_ASSERT(strstr(out, "frames_tx=")      != NULL);

    HH_ASSERT_EQ_INT(run_radioctl(out, sizeof out, "stop", NULL, NULL), 0);
    HH_ASSERT(strstr(out, "state=stopped") != NULL);

    HH_ASSERT_EQ_INT(run_radioctl(out, sizeof out, "shutdown", NULL, NULL), 0);

    stop_radiod(pid);
    unlink(TEST_SOCK_PATH);
}

/* Fault injection and recovery, driven entirely from the CLI. */
static void test_fault_cycle_through_cli(void)
{
    char out[1024];

    unlink(TEST_SOCK_PATH);
    pid_t pid = spawn_radiod(TEST_SOCK_PATH);
    wait_for_socket(TEST_SOCK_PATH);

    HH_ASSERT_EQ_INT(run_radioctl(NULL, 0, "init", NULL, NULL), 0);
    HH_ASSERT_EQ_INT(run_radioctl(NULL, 0, "configure", "3", NULL), 0);
    HH_ASSERT_EQ_INT(run_radioctl(NULL, 0, "start", NULL, NULL), 0);

    HH_ASSERT_EQ_INT(run_radioctl(out, sizeof out, "inject-fault", "hw_fault", NULL), 0);

    HH_ASSERT_EQ_INT(run_radioctl(out, sizeof out, "status", NULL, NULL), 0);
    HH_ASSERT(strstr(out, "state=faulted")  != NULL);
    HH_ASSERT(strstr(out, "operational=0")  != NULL);

    HH_ASSERT_EQ_INT(run_radioctl(out, sizeof out, "clear-fault", NULL, NULL), 0);
    HH_ASSERT_EQ_INT(run_radioctl(out, sizeof out, "status", NULL, NULL), 0);
    HH_ASSERT(strstr(out, "state=running") != NULL);

    HH_ASSERT_EQ_INT(run_radioctl(NULL, 0, "shutdown", NULL, NULL), 0);
    stop_radiod(pid);
    unlink(TEST_SOCK_PATH);
}

/* A rejection is a successful exchange carrying a refusal, and must be
 * reported distinctly from a transport failure. */
static void test_rejected_request_exit_status(void)
{
    unlink(TEST_SOCK_PATH);
    pid_t pid = spawn_radiod(TEST_SOCK_PATH);
    wait_for_socket(TEST_SOCK_PATH);

    /* start before init is an invalid transition; radiod answers with err. */
    HH_ASSERT_EQ_INT(run_radioctl(NULL, 0, "start", NULL, NULL), EXIT_DENIED);

    HH_ASSERT_EQ_INT(run_radioctl(NULL, 0, "init", NULL, NULL), 0);
    HH_ASSERT_EQ_INT(run_radioctl(NULL, 0, "shutdown", NULL, NULL), 0);
    stop_radiod(pid);
    unlink(TEST_SOCK_PATH);
}

/* With no daemon listening, the CLI must fail as transport, not as a rejection. */
static void test_no_daemon_exit_status(void)
{
    unlink(TEST_SOCK_PATH);
    HH_ASSERT_EQ_INT(run_radioctl(NULL, 0, "status", NULL, NULL), EXIT_TRANSPORT);
}

/* Malformed invocations are refused locally, without contacting radiod. */
static void test_usage_errors(void)
{
    unlink(TEST_SOCK_PATH);
    HH_ASSERT_EQ_INT(run_radioctl(NULL, 0, "not_a_command", NULL, NULL), EXIT_USAGE);
    HH_ASSERT_EQ_INT(run_radioctl(NULL, 0, "configure", NULL, NULL), EXIT_USAGE);
    HH_ASSERT_EQ_INT(run_radioctl(NULL, 0, "set-channel", NULL, NULL), EXIT_USAGE);
    HH_ASSERT_EQ_INT(run_radioctl(NULL, 0, "inject-fault", "bogus_kind", NULL), EXIT_USAGE);
}

HH_TEST_MAIN_BEGIN("radioctl (CLI end-to-end)")
    HH_RUN(test_lifecycle_through_cli);
    HH_RUN(test_fault_cycle_through_cli);
    HH_RUN(test_rejected_request_exit_status);
    HH_RUN(test_no_daemon_exit_status);
    HH_RUN(test_usage_errors);
HH_TEST_MAIN_END()
