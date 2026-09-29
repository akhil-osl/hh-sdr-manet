/*
 * The OpenCPI backend, against the stub agent.
 *
 * Two halves:
 *
 *  1. In-process: hh_oc_backend_init against a deliberately broken agent, which
 *     is the only way to cover the paths that matter most in the field --
 *     agent missing, agent of the wrong protocol version, agent that dies, and
 *     agent that never answers. Each must fail with a reason, and must leave a
 *     backend that is still callable and still honest rather than one that
 *     crashes on the next request.
 *
 *  2. End to end: the real radiod binary, started with -b opencpi against the
 *     stub agent, driven entirely through librc over its socket. That is the
 *     whole chain -- client, protocol codec, daemon, backend, agent -- with only
 *     OpenCPI itself replaced, and it is the same shape as
 *     test_radiod_daemon.c's coverage of the mock path.
 *
 * The stub's registry uses invented worker names (w1, w2). That is deliberate:
 * every assertion below passes without any real worker or property existing,
 * which is what demonstrates that radiod and the protocol carry no knowledge of
 * qpsk_tx, qpsk_rx or any of their properties.
 */
#include "hhsdr/librc/rc_client.h"
#include "hhsdr/radiod/opencpi.h"
#include "hh_test.h"
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define TEST_SOCK "/tmp/hh-radiod-opencpi-test.sock"

/* Reply bound for the cases that are SUPPOSED to answer. Generous on purpose:
 * these assertions are about behaviour, not about latency, and a tight bound
 * here would turn a loaded build machine into a test failure. The one case that
 * wants a short bound is the timeout test, which passes its own. */
#define HEALTHY_REPLY_MS 10000u

/* ======================================================================= */
/* Part 1 — the backend in process                                          */
/* ======================================================================= */

static void oc_init_with(const char *agent, const char *mode, uint32_t reply_ms,
                         hh_oc_backend_t *b, hh_radio_t *radio,
                         hh_radiod_props_t *props, hh_status_t *st)
{
    hh_oc_config_t cfg;
    hh_oc_config_defaults(&cfg);
    snprintf(cfg.agent_path, sizeof cfg.agent_path, "%s", agent);
    cfg.reply_timeout_ms = reply_ms;
    cfg.start_timeout_ms = reply_ms;
    if (mode) setenv("HH_STUB_AGENT_MODE", mode, 1);
    else      unsetenv("HH_STUB_AGENT_MODE");
    *st = hh_oc_backend_init(b, &cfg, radio, props);
    unsetenv("HH_STUB_AGENT_MODE");
}

static void test_agent_missing(void)
{
    hh_oc_backend_t b;
    hh_radio_t radio;
    hh_radiod_props_t props;
    hh_rc_response_t resp;
    hh_status_t st;

    /* The ordinary state of a host with no OpenCPI installation. */
    oc_init_with("/nonexistent/rh_agent", NULL, 1000u, &b, &radio, &props, &st);
    HH_ASSERT_ERR(st, HH_ERR_IO);
    HH_ASSERT(!hh_oc_backend_agent_alive(&b));
    HH_ASSERT_MSG(hh_oc_backend_last_error(&b)[0] != '\0',
                  "a failed init must say why");

    /* Still callable, and still honest: no NULL vtable, no crash. */
    memset(&resp, 0, sizeof resp);
    HH_ASSERT_ERR(props.ops->list(props.self, NULL, &resp), HH_ERR_UNSUPPORTED);
    HH_ASSERT_ERR(radio.ops->open(radio.self), HH_ERR_IO);

    /* And status reports a radio that is not operational rather than one whose
     * counters happen to read zero. */
    {
        hh_radio_status_t rs;
        memset(&rs, 0xFF, sizeof rs);
        HH_ASSERT_OK(radio.ops->get_status(radio.self, &rs));
        HH_ASSERT(!rs.operational);
    }
    hh_oc_backend_release(&b);
}

static void test_agent_version_mismatch(void)
{
    hh_oc_backend_t b;
    hh_radio_t radio;
    hh_radiod_props_t props;
    hh_status_t st;

    /* An agent speaking a protocol radiod does not understand must be refused,
     * not used. Misreading its replies would be worse than not talking to it. */
    oc_init_with(STUB_AGENT_PATH, "badversion", HEALTHY_REPLY_MS, &b, &radio, &props, &st);
    HH_ASSERT_ERR(st, HH_ERR_UNSUPPORTED);
    HH_ASSERT(!hh_oc_backend_agent_alive(&b));
    HH_ASSERT(strstr(hh_oc_backend_last_error(&b), "protocol") != NULL);
    hh_oc_backend_release(&b);
}

static void test_agent_silent_times_out(void)
{
    hh_oc_backend_t b;
    hh_radio_t radio;
    hh_radiod_props_t props;
    hh_status_t st;

    /* The bound exists so a wedged agent cannot wedge the control plane. A
     * short one here keeps the test fast; the real default is 3 s. */
    oc_init_with(STUB_AGENT_PATH, "silent", 300u, &b, &radio, &props, &st);
    HH_ASSERT_ERR(st, HH_ERR_IO);
    HH_ASSERT(strstr(hh_oc_backend_last_error(&b), "within") != NULL);
    /* A timed-out agent is of unknown state, so it is dropped rather than kept. */
    HH_ASSERT(!hh_oc_backend_agent_alive(&b));
    hh_oc_backend_release(&b);
}

static void test_agent_death_is_noticed(void)
{
    hh_oc_backend_t b;
    hh_radio_t radio;
    hh_radiod_props_t props;
    hh_status_t st;

    /* The agent answers hello and exits, so init fails at the registry read. */
    oc_init_with(STUB_AGENT_PATH, "die", HEALTHY_REPLY_MS, &b, &radio, &props, &st);
    HH_ASSERT_ERR(st, HH_ERR_IO);
    HH_ASSERT(strstr(hh_oc_backend_last_error(&b), "exited") != NULL);
    hh_oc_backend_release(&b);
}

static void test_registry_and_unsupported_radio_operations(void)
{
    hh_oc_backend_t b;
    hh_radio_t radio;
    hh_radiod_props_t props;
    hh_rc_response_t resp;
    hh_status_t st;

    oc_init_with(STUB_AGENT_PATH, NULL, HEALTHY_REPLY_MS, &b, &radio, &props, &st);
    HH_ASSERT_OK(st);
    HH_ASSERT(hh_oc_backend_agent_alive(&b));

    /* The registry came from the agent, not from a table in radiod. */
    memset(&resp, 0, sizeof resp);
    HH_ASSERT_OK(props.ops->list(props.self, NULL, &resp));
    HH_ASSERT_EQ_STR(resp.items, "w1,w2");
    HH_ASSERT_EQ_INT(resp.count, 2);

    memset(&resp, 0, sizeof resp);
    HH_ASSERT_OK(props.ops->list(props.self, "w1", &resp));
    HH_ASSERT_EQ_INT(resp.count, 3);
    HH_ASSERT(strstr(resp.items, "live:ulong:rw") != NULL);
    HH_ASSERT(strstr(resp.items, "startonly:ulong:rw-start") != NULL);
    HH_ASSERT(strstr(resp.items, "counter:ulonglong:ro") != NULL);

    memset(&resp, 0, sizeof resp);
    HH_ASSERT_ERR(props.ops->list(props.self, "nosuch", &resp), HH_ERR_NOTFOUND);

    /* The data-plane and channel operations are refused, deliberately and
     * distinguishably: NOT_IMPLEMENTED for the data plane (out of the control
     * contract's scope) and UNSUPPORTED for a channel plan that does not exist. */
    HH_ASSERT_ERR(radio.ops->set_channel(radio.self, 4), HH_ERR_UNSUPPORTED);
    HH_ASSERT_ERR(radio.ops->transmit(radio.self, NULL), HH_ERR_NOT_IMPLEMENTED);
    HH_ASSERT_ERR(radio.ops->set_rx_callback(radio.self, NULL, NULL),
                  HH_ERR_NOT_IMPLEMENTED);
    HH_ASSERT_ERR(radio.ops->get_link_metrics(radio.self, 1, NULL),
                  HH_ERR_NOT_IMPLEMENTED);

    hh_oc_backend_release(&b);
    HH_ASSERT(!hh_oc_backend_agent_alive(&b));
}

static void test_access_rules_and_staging(void)
{
    hh_oc_backend_t b;
    hh_radio_t radio;
    hh_radiod_props_t props;
    hh_rc_response_t resp;
    hh_status_t st;

    oc_init_with(STUB_AGENT_PATH, NULL, HEALTHY_REPLY_MS, &b, &radio, &props, &st);
    HH_ASSERT_OK(st);

    /* Unwritten writable reads back as "keep", not as 0. */
    memset(&resp, 0, sizeof resp);
    HH_ASSERT_OK(props.ops->get(props.self, "w1", "live", &resp));
    HH_ASSERT_EQ_STR(resp.value, "keep");
    HH_ASSERT_EQ_STR(resp.access, "rw");

    /* Read-only before start: refused, with a reason, rather than reporting a
     * stale counter as if it were current. */
    memset(&resp, 0, sizeof resp);
    HH_ASSERT_ERR(props.ops->get(props.self, "w1", "counter", &resp), HH_ERR_STATE);
    HH_ASSERT(strstr(resp.text, "live counter") != NULL);

    /* Read-only is never writable. */
    memset(&resp, 0, sizeof resp);
    HH_ASSERT_ERR(props.ops->set(props.self, "w1", "counter", "1", false, &resp),
                  HH_ERR_UNSUPPORTED);

    /* Unknown names are NOTFOUND, both ways. */
    memset(&resp, 0, sizeof resp);
    HH_ASSERT_ERR(props.ops->get(props.self, "w1", "nosuch", &resp), HH_ERR_NOTFOUND);
    memset(&resp, 0, sizeof resp);
    HH_ASSERT_ERR(props.ops->set(props.self, "nosuch", "p", "1", false, &resp),
                  HH_ERR_NOTFOUND);

    /* Stopped: writes are staged, and read back. */
    memset(&resp, 0, sizeof resp);
    HH_ASSERT_OK(props.ops->set(props.self, "w1", "live", "42", false, &resp));
    HH_ASSERT(strstr(resp.text, "staged") != NULL);
    memset(&resp, 0, sizeof resp);
    HH_ASSERT_OK(props.ops->get(props.self, "w1", "live", &resp));
    HH_ASSERT_EQ_STR(resp.value, "42");

    /* A start-time-only property IS writable while stopped. */
    memset(&resp, 0, sizeof resp);
    HH_ASSERT_OK(props.ops->set(props.self, "w1", "startonly", "7", false, &resp));

    /* Now start the session. */
    HH_ASSERT_OK(radio.ops->open(radio.self));

    /* Running: a live property is applied rather than staged... */
    memset(&resp, 0, sizeof resp);
    HH_ASSERT_OK(props.ops->set(props.self, "w1", "live", "43", true, &resp));
    HH_ASSERT(strstr(resp.text, "applied") != NULL);

    /* ...and a start-time-only property is REFUSED, not queued. Silently
     * deferring it would leave the operator believing it had taken effect. */
    memset(&resp, 0, sizeof resp);
    HH_ASSERT_ERR(props.ops->set(props.self, "w1", "startonly", "8", true, &resp),
                  HH_ERR_STATE);
    HH_ASSERT(strstr(resp.text, "stopped") != NULL);
    memset(&resp, 0, sizeof resp);
    HH_ASSERT_OK(props.ops->get(props.self, "w1", "startonly", &resp));
    HH_ASSERT_MSG(strcmp(resp.value, "7") == 0,
                  "a refused write must not change the value, got %s", resp.value);

    /* Read-only is readable now, and status samples every read-only property. */
    memset(&resp, 0, sizeof resp);
    HH_ASSERT_OK(props.ops->get(props.self, "w1", "counter", &resp));
    HH_ASSERT_EQ_STR(resp.value, "4242");
    memset(&resp, 0, sizeof resp);
    HH_ASSERT_OK(props.ops->status(props.self, true, &resp));
    HH_ASSERT_EQ_INT(resp.count, 1);
    HH_ASSERT(strstr(resp.items, "w1.counter:4242") != NULL);

    /* operational tracks the session, not the link. */
    {
        hh_radio_status_t rs;
        memset(&rs, 0, sizeof rs);
        HH_ASSERT_OK(radio.ops->get_status(radio.self, &rs));
        HH_ASSERT(rs.operational);
    }

    HH_ASSERT_OK(radio.ops->close(radio.self));
    memset(&resp, 0, sizeof resp);
    HH_ASSERT_ERR(props.ops->status(props.self, false, &resp), HH_ERR_STATE);

    hh_oc_backend_release(&b);
}

static void test_failure_kinds_map_to_status(void)
{
    hh_oc_backend_t b;
    hh_radio_t radio;
    hh_radiod_props_t props;
    hh_rc_response_t resp;
    hh_status_t st;

    oc_init_with(STUB_AGENT_PATH, NULL, HEALTHY_REPLY_MS, &b, &radio, &props, &st);
    HH_ASSERT_OK(st);

    /* The agent STATES the kind of each failure; radiod maps it rather than
     * inferring it from the message. An earlier revision searched the text for
     * words like "range", which reported a rejected VALUE as an I/O error --
     * sending an operator to look at the hardware instead of at what they typed.
     *
     * A bad value is the caller's mistake, so it must read as EINVAL. */
    memset(&resp, 0, sizeof resp);
    HH_ASSERT_ERR(props.ops->set(props.self, "w1", "live", "notanumber", false, &resp),
                  HH_ERR_INVAL);
    HH_ASSERT_MSG(strstr(resp.text, "integer") != NULL,
                  "the agent's reason must reach the caller, got \"%s\"", resp.text);

    /* And a rejected write must not have changed anything. */
    memset(&resp, 0, sizeof resp);
    HH_ASSERT_OK(props.ops->get(props.self, "w1", "live", &resp));
    HH_ASSERT_EQ_STR(resp.value, "keep");

    hh_oc_backend_release(&b);
}

static void test_open_is_idempotent_and_close_recovers(void)
{
    hh_oc_backend_t b;
    hh_radio_t radio;
    hh_radiod_props_t props;
    hh_status_t st;

    oc_init_with(STUB_AGENT_PATH, NULL, HEALTHY_REPLY_MS, &b, &radio, &props, &st);
    HH_ASSERT_OK(st);
    HH_ASSERT_OK(radio.ops->open(radio.self));
    HH_ASSERT_OK(radio.ops->open(radio.self));   /* second start is a no-op */
    HH_ASSERT_OK(radio.ops->close(radio.self));
    HH_ASSERT_OK(radio.ops->close(radio.self));  /* second stop is a no-op */
    /* And a session can be opened again afterwards, which is what a stop /
     * set a start-time property / start cycle depends on. */
    HH_ASSERT_OK(radio.ops->open(radio.self));
    hh_oc_backend_release(&b);
}

/* ======================================================================= */
/* Part 2 — end to end through the real daemon                              */
/* ======================================================================= */

static void wait_for_socket(const char *path)
{
    for (int i = 0; i < 300; i++) {
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
        execl(RADIOD_BINARY_PATH, "radiod", "-s", sock_path,
              "-b", "opencpi", "-a", STUB_AGENT_PATH, "-v", "error", (char *)NULL);
        _exit(127);
    }
    return pid;
}

static void stop_radiod(pid_t pid)
{
    int status;
    if (pid <= 0) return;
    kill(pid, SIGTERM);
    waitpid(pid, &status, 0);
}

static hh_rc_response_t call(hh_rc_client_t *c, hh_rc_cmd_t cmd, const char *worker,
                             const char *prop, const char *value)
{
    hh_rc_request_t req;
    hh_rc_response_t resp;
    memset(&req, 0, sizeof req);
    memset(&resp, 0, sizeof resp);
    req.cmd = cmd;
    if (cmd == HH_RC_CMD_CONFIGURE) req.node_id = 1;
    if (worker) snprintf(req.worker, sizeof req.worker, "%s", worker);
    if (prop)   snprintf(req.prop, sizeof req.prop, "%s", prop);
    if (value)  snprintf(req.value, sizeof req.value, "%s", value);
    if (hh_rc_client_call(c, &req, &resp) != HH_OK) {
        hh_tests_failed++;
        fprintf(stderr, "FAIL %s: transport error on %s\n", hh_current_test,
                hh_rc_cmd_str(cmd));
    }
    return resp;
}

static void test_end_to_end_through_the_socket(void)
{
    hh_rc_client_t c;
    hh_rc_response_t r;
    pid_t pid;

    unlink(TEST_SOCK);
    unsetenv("HH_STUB_AGENT_MODE");
    pid = spawn_radiod(TEST_SOCK);
    wait_for_socket(TEST_SOCK);
    HH_ASSERT_OK(hh_rc_client_connect(&c, TEST_SOCK));

    /* Introspection before the radio is up. */
    r = call(&c, HH_RC_CMD_OPENCPI_LIST, NULL, NULL, NULL);
    HH_ASSERT(r.ok);
    HH_ASSERT_EQ_STR(r.items, "w1,w2");

    r = call(&c, HH_RC_CMD_OPENCPI_LIST, "w1", NULL, NULL);
    HH_ASSERT(r.ok);
    HH_ASSERT_EQ_INT(r.count, 3);

    /* Stage, then bring the application up. */
    r = call(&c, HH_RC_CMD_OPENCPI_SET, "w1", "live", "11");
    HH_ASSERT(r.ok);
    r = call(&c, HH_RC_CMD_OPENCPI_GET, "w1", "live", NULL);
    HH_ASSERT_EQ_STR(r.value, "11");

    HH_ASSERT(call(&c, HH_RC_CMD_INIT, NULL, NULL, NULL).ok);
    HH_ASSERT(call(&c, HH_RC_CMD_CONFIGURE, NULL, NULL, NULL).ok);
    HH_ASSERT(call(&c, HH_RC_CMD_START, NULL, NULL, NULL).ok);

    /* HTI-02 status now reports an operational radio. */
    r = call(&c, HH_RC_CMD_STATUS, NULL, NULL, NULL);
    HH_ASSERT(r.ok);
    HH_ASSERT(r.operational);
    HH_ASSERT_EQ_INT(r.state, HH_RC_STATE_RUNNING);

    /* Live write, refused start-time write, read-only read, aggregate status --
     * all over the socket, all with the backend's own reason text intact. */
    r = call(&c, HH_RC_CMD_OPENCPI_SET, "w1", "live", "12");
    HH_ASSERT(r.ok);
    HH_ASSERT(strstr(r.text, "applied") != NULL);

    r = call(&c, HH_RC_CMD_OPENCPI_SET, "w1", "startonly", "9");
    HH_ASSERT(!r.ok);
    HH_ASSERT_EQ_INT(r.reason, HH_ERR_STATE);
    HH_ASSERT_MSG(strstr(r.text, "stopped") != NULL,
                  "the reason must survive the wire, got \"%s\"", r.text);

    r = call(&c, HH_RC_CMD_OPENCPI_GET, "w1", "counter", NULL);
    HH_ASSERT(r.ok);
    HH_ASSERT_EQ_STR(r.value, "4242");

    r = call(&c, HH_RC_CMD_OPENCPI_STATUS, NULL, NULL, NULL);
    HH_ASSERT(r.ok);
    HH_ASSERT(strstr(r.items, "w1.counter:4242") != NULL);

    /* set_channel has no meaning for this radio and says so rather than
     * pretending to retune. */
    r = call(&c, HH_RC_CMD_SET_CHANNEL, NULL, NULL, NULL);
    HH_ASSERT(!r.ok);
    HH_ASSERT_EQ_INT(r.reason, HH_ERR_UNSUPPORTED);

    /* Stop, and the start-time property becomes writable again. */
    HH_ASSERT(call(&c, HH_RC_CMD_STOP, NULL, NULL, NULL).ok);
    r = call(&c, HH_RC_CMD_OPENCPI_SET, "w1", "startonly", "9");
    HH_ASSERT_MSG(r.ok, "a start-time property must be writable once stopped");

    hh_rc_client_close(&c);
    stop_radiod(pid);
    unlink(TEST_SOCK);
}

static void test_end_to_end_when_opencpi_is_unavailable(void)
{
    hh_rc_client_t c;
    hh_rc_response_t r;
    pid_t pid;

    /* radiod must still come up and still serve clients when the agent cannot
     * be started -- otherwise a broken OpenCPI installation presents as a
     * daemon that is simply absent, with nothing to diagnose it by.
     *
     * The agent is made unusable by pointing it at the silent mode, which is
     * indistinguishable from a hung one. */
    unlink(TEST_SOCK);
    setenv("HH_STUB_AGENT_MODE", "die", 1);
    pid = spawn_radiod(TEST_SOCK);
    wait_for_socket(TEST_SOCK);
    unsetenv("HH_STUB_AGENT_MODE");

    HH_ASSERT_MSG(hh_rc_client_connect(&c, TEST_SOCK) == HH_OK,
                  "radiod must serve clients even with no working agent");

    /* Property verbs report the absence, rather than answering with invented
     * data or dropping the connection. */
    r = call(&c, HH_RC_CMD_OPENCPI_LIST, NULL, NULL, NULL);
    HH_ASSERT(!r.ok);
    HH_ASSERT_EQ_INT(r.reason, HH_ERR_UNSUPPORTED);

    /* And bringing the radio up fails, leaving radiod FAULTED -- a state an
     * operator can see and act on.
     *
     * The failure line itself carries only `reason`, which is the response
     * format the protocol has always used for a refusal; the resulting state is
     * read with `status`, which is also what an operator would do. */
    HH_ASSERT(call(&c, HH_RC_CMD_INIT, NULL, NULL, NULL).ok);
    HH_ASSERT(call(&c, HH_RC_CMD_CONFIGURE, NULL, NULL, NULL).ok);
    r = call(&c, HH_RC_CMD_START, NULL, NULL, NULL);
    HH_ASSERT(!r.ok);
    HH_ASSERT_EQ_INT(r.reason, HH_ERR_IO);

    /* Status still answers, reports the fault, and says the radio is not
     * operational rather than reporting zeroed counters as if they were real. */
    r = call(&c, HH_RC_CMD_STATUS, NULL, NULL, NULL);
    HH_ASSERT(r.ok);
    HH_ASSERT_EQ_INT(r.state, HH_RC_STATE_FAULTED);
    HH_ASSERT(!r.operational);

    hh_rc_client_close(&c);
    stop_radiod(pid);
    unlink(TEST_SOCK);
}

HH_TEST_MAIN_BEGIN("radiod_opencpi")
    HH_RUN(test_agent_missing);
    HH_RUN(test_agent_version_mismatch);
    HH_RUN(test_agent_silent_times_out);
    HH_RUN(test_agent_death_is_noticed);
    HH_RUN(test_registry_and_unsupported_radio_operations);
    HH_RUN(test_access_rules_and_staging);
    HH_RUN(test_failure_kinds_map_to_status);
    HH_RUN(test_open_is_idempotent_and_close_recovers);
    HH_RUN(test_end_to_end_through_the_socket);
    HH_RUN(test_end_to_end_when_opencpi_is_unavailable);
HH_TEST_MAIN_END()
