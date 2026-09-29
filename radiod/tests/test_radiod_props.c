/*
 * radiod's routing of the worker-property verbs.
 *
 * Exercised in-process, against a props vtable defined in this file, so what is
 * under test is radiod's own behaviour and nothing else:
 *
 *   - with no property surface attached (which is what the mock backend and a
 *     radiod built without OpenCPI both look like), the verbs are understood
 *     and refused with HH_ERR_UNSUPPORTED plus a sentence saying why. This is
 *     the "OpenCPI is unavailable" path.
 *   - the `running` flag radiod passes is its own lifecycle state, which is the
 *     one thing the backend cannot know for itself and the whole basis of
 *     start-time-only enforcement.
 *   - a backend's status and its failure detail are relayed verbatim.
 *   - radiod reads no worker or property name: the names used here are
 *     deliberately not the real ones, so any dependence on qpsk_tx/qpsk_rx
 *     would show up as a failure.
 */
#include "hhsdr/radiod/radiod.h"
#include "hhsdr/radiod/mock_backend.h"
#include "hh_test.h"
#include <stdio.h>
#include <string.h>

/* ---- a recording props backend ---- */

typedef struct {
    int  list_calls, get_calls, set_calls, status_calls;
    bool last_running;
    char last_worker[HH_RC_MAX_NAME];
    char last_prop[HH_RC_MAX_NAME];
    char last_value[HH_RC_MAX_VALUE];
    hh_status_t next_status;      /* what the vtable should return */
} spy_t;

static hh_status_t spy_list(void *self, const char *worker, hh_rc_response_t *resp)
{
    spy_t *s = self;
    s->list_calls++;
    snprintf(s->last_worker, sizeof s->last_worker, "%s", worker ? worker : "");
    if (s->next_status != HH_OK) {
        snprintf(resp->text, sizeof resp->text, "no such worker");
        return s->next_status;
    }
    snprintf(resp->items, sizeof resp->items, "alpha,beta");
    resp->count = 2;
    return HH_OK;
}

static hh_status_t spy_get(void *self, const char *worker, const char *prop,
                          hh_rc_response_t *resp)
{
    spy_t *s = self;
    s->get_calls++;
    snprintf(s->last_worker, sizeof s->last_worker, "%s", worker);
    snprintf(s->last_prop, sizeof s->last_prop, "%s", prop);
    if (s->next_status != HH_OK) return s->next_status;
    snprintf(resp->worker, sizeof resp->worker, "%s", worker);
    snprintf(resp->prop, sizeof resp->prop, "%s", prop);
    snprintf(resp->value, sizeof resp->value, "1234");
    snprintf(resp->ptype, sizeof resp->ptype, "ulong");
    snprintf(resp->access, sizeof resp->access, "rw");
    return HH_OK;
}

static hh_status_t spy_set(void *self, const char *worker, const char *prop,
                           const char *value, bool running, hh_rc_response_t *resp)
{
    spy_t *s = self;
    s->set_calls++;
    s->last_running = running;
    snprintf(s->last_worker, sizeof s->last_worker, "%s", worker);
    snprintf(s->last_prop, sizeof s->last_prop, "%s", prop);
    snprintf(s->last_value, sizeof s->last_value, "%s", value);
    if (s->next_status != HH_OK) {
        snprintf(resp->text, sizeof resp->text, "refused for a stated reason");
        return s->next_status;
    }
    snprintf(resp->worker, sizeof resp->worker, "%s", worker);
    snprintf(resp->prop, sizeof resp->prop, "%s", prop);
    snprintf(resp->value, sizeof resp->value, "%s", value);
    return HH_OK;
}

static hh_status_t spy_status(void *self, bool running, hh_rc_response_t *resp)
{
    spy_t *s = self;
    s->status_calls++;
    s->last_running = running;
    if (s->next_status != HH_OK) return s->next_status;
    snprintf(resp->items, sizeof resp->items, "alpha.count:7");
    resp->count = 1;
    return HH_OK;
}

static const hh_radiod_props_ops_t g_spy_ops = {
    .name = "spy", .list = spy_list, .get = spy_get, .set = spy_set, .status = spy_status
};

/* ---- fixtures ---- */

typedef struct {
    hh_mock_backend_t backend;
    hh_radio_t        radio;
    hh_radiod_t       d;
    spy_t             spy;
    hh_radiod_props_t props;
} fixture_t;

static void fixture_init(fixture_t *f, bool attach_props)
{
    memset(f, 0, sizeof *f);
    hh_mock_backend_init(&f->backend, &f->radio);
    HH_ASSERT_OK(hh_radiod_init(&f->d, &f->radio, hh_clock_monotonic()));
    if (attach_props) {
        f->props.ops  = &g_spy_ops;
        f->props.self = &f->spy;
        hh_radiod_set_props(&f->d, &f->props);
    }
}

static hh_rc_response_t ask(hh_radiod_t *d, hh_rc_cmd_t cmd, const char *worker,
                            const char *prop, const char *value)
{
    hh_rc_request_t req;
    hh_rc_response_t resp;
    memset(&req, 0, sizeof req);
    memset(&resp, 0, sizeof resp);
    req.cmd = cmd;
    if (worker) snprintf(req.worker, sizeof req.worker, "%s", worker);
    if (prop)   snprintf(req.prop, sizeof req.prop, "%s", prop);
    if (value)  snprintf(req.value, sizeof req.value, "%s", value);
    hh_radiod_handle_request(d, &req, &resp);
    return resp;
}

/* Walk the lifecycle up to RUNNING. */
static void bring_up(hh_radiod_t *d)
{
    hh_rc_request_t req;
    hh_rc_response_t resp;
    memset(&req, 0, sizeof req);
    req.cmd = HH_RC_CMD_INIT;      hh_radiod_handle_request(d, &req, &resp);
    req.cmd = HH_RC_CMD_CONFIGURE; req.node_id = 1;
    hh_radiod_handle_request(d, &req, &resp);
    memset(&req, 0, sizeof req);
    req.cmd = HH_RC_CMD_START;     hh_radiod_handle_request(d, &req, &resp);
}

/* ---- tests ---- */

static void test_no_props_backend_is_unsupported(void)
{
    fixture_t f;
    hh_rc_response_t r;

    fixture_init(&f, false);
    bring_up(&f.d);

    /* Understood, and refused, with the backend named. This is exactly what an
     * operator sees on a radiod running the mock backend, or one deployed
     * without an rh_agent -- not a parse error and not a crash. */
    r = ask(&f.d, HH_RC_CMD_OPENCPI_LIST, NULL, NULL, NULL);
    HH_ASSERT(!r.ok);
    HH_ASSERT_EQ_INT(r.reason, HH_ERR_UNSUPPORTED);
    HH_ASSERT_MSG(strstr(r.text, "mock") != NULL,
                  "the refusal should name the backend, got \"%s\"", r.text);

    r = ask(&f.d, HH_RC_CMD_OPENCPI_GET, "w", "p", NULL);
    HH_ASSERT_EQ_INT(r.reason, HH_ERR_UNSUPPORTED);
    r = ask(&f.d, HH_RC_CMD_OPENCPI_SET, "w", "p", "1");
    HH_ASSERT_EQ_INT(r.reason, HH_ERR_UNSUPPORTED);
    r = ask(&f.d, HH_RC_CMD_OPENCPI_STATUS, NULL, NULL, NULL);
    HH_ASSERT_EQ_INT(r.reason, HH_ERR_UNSUPPORTED);

    /* Every one of those counted as a rejection, so `stats` tells the truth. */
    r = ask(&f.d, HH_RC_CMD_STATS, NULL, NULL, NULL);
    HH_ASSERT(r.requests_rejected >= 4);
    hh_radiod_release(&f.d);
}

static void test_reachable_before_init(void)
{
    fixture_t f;
    hh_rc_response_t r;

    /* Staging a configuration before the radio is brought up is the normal
     * order of work, so these verbs must be legal from CREATED. If they were
     * gated on the lifecycle, a start-time-only property could never be
     * written in the only state it may be written in. */
    fixture_init(&f, true);
    r = ask(&f.d, HH_RC_CMD_OPENCPI_LIST, NULL, NULL, NULL);
    HH_ASSERT_MSG(r.ok, "opencpi list must work from CREATED");
    r = ask(&f.d, HH_RC_CMD_OPENCPI_SET, "alpha", "gain", "3");
    HH_ASSERT(r.ok);
    HH_ASSERT(!f.spy.last_running);
    hh_radiod_release(&f.d);
}

static void test_running_flag_follows_lifecycle(void)
{
    fixture_t f;
    hh_rc_response_t r;

    fixture_init(&f, true);

    /* Before start: not running. */
    (void)ask(&f.d, HH_RC_CMD_OPENCPI_SET, "alpha", "gain", "1");
    HH_ASSERT_MSG(!f.spy.last_running, "running must be false before start");

    bring_up(&f.d);
    (void)ask(&f.d, HH_RC_CMD_OPENCPI_SET, "alpha", "gain", "2");
    HH_ASSERT_MSG(f.spy.last_running, "running must be true in RUNNING");
    (void)ask(&f.d, HH_RC_CMD_OPENCPI_STATUS, NULL, NULL, NULL);
    HH_ASSERT(f.spy.last_running);

    /* After stop: not running again, which is what re-enables a start-time
     * property write. */
    {
        hh_rc_request_t req;
        hh_rc_response_t resp;
        memset(&req, 0, sizeof req);
        req.cmd = HH_RC_CMD_STOP;
        hh_radiod_handle_request(&f.d, &req, &resp);
    }
    r = ask(&f.d, HH_RC_CMD_OPENCPI_SET, "alpha", "gain", "3");
    HH_ASSERT(r.ok);
    HH_ASSERT_MSG(!f.spy.last_running, "running must be false after stop");
    hh_radiod_release(&f.d);
}

static void test_arguments_are_passed_through_verbatim(void)
{
    fixture_t f;
    hh_rc_response_t r;

    fixture_init(&f, true);

    r = ask(&f.d, HH_RC_CMD_OPENCPI_LIST, "alpha", NULL, NULL);
    HH_ASSERT(r.ok);
    HH_ASSERT_EQ_STR(f.spy.last_worker, "alpha");
    HH_ASSERT_EQ_STR(r.items, "alpha,beta");
    HH_ASSERT_EQ_INT(r.count, 2);

    r = ask(&f.d, HH_RC_CMD_OPENCPI_GET, "alpha", "gain", NULL);
    HH_ASSERT(r.ok);
    HH_ASSERT_EQ_STR(f.spy.last_prop, "gain");
    HH_ASSERT_EQ_STR(r.value, "1234");
    HH_ASSERT_EQ_STR(r.ptype, "ulong");

    r = ask(&f.d, HH_RC_CMD_OPENCPI_SET, "beta", "mode", "0x40");
    HH_ASSERT(r.ok);
    HH_ASSERT_EQ_STR(f.spy.last_value, "0x40");
    hh_radiod_release(&f.d);
}

static void test_missing_arguments_rejected_before_the_backend(void)
{
    fixture_t f;
    hh_rc_response_t r;

    /* A get with no property, or a set with no value, is malformed. Catching it
     * here keeps every backend from having to. */
    fixture_init(&f, true);
    r = ask(&f.d, HH_RC_CMD_OPENCPI_GET, "alpha", NULL, NULL);
    HH_ASSERT(!r.ok);
    HH_ASSERT_EQ_INT(r.reason, HH_ERR_INVAL);
    r = ask(&f.d, HH_RC_CMD_OPENCPI_SET, "alpha", "gain", NULL);
    HH_ASSERT(!r.ok);
    HH_ASSERT_EQ_INT(r.reason, HH_ERR_INVAL);
    HH_ASSERT_EQ_INT(f.spy.get_calls, 0);
    HH_ASSERT_EQ_INT(f.spy.set_calls, 0);
    hh_radiod_release(&f.d);
}

static void test_backend_failure_and_detail_are_relayed(void)
{
    fixture_t f;
    hh_rc_response_t r;

    fixture_init(&f, true);
    f.spy.next_status = HH_ERR_STATE;

    /* The backend's status code AND its sentence both reach the client. The
     * sentence is the difference between "ESTATE" and a reason an operator can
     * act on. */
    r = ask(&f.d, HH_RC_CMD_OPENCPI_SET, "alpha", "gain", "1");
    HH_ASSERT(!r.ok);
    HH_ASSERT_EQ_INT(r.reason, HH_ERR_STATE);
    HH_ASSERT_EQ_STR(r.text, "refused for a stated reason");

    f.spy.next_status = HH_ERR_NOTFOUND;
    r = ask(&f.d, HH_RC_CMD_OPENCPI_LIST, "nosuch", NULL, NULL);
    HH_ASSERT_EQ_INT(r.reason, HH_ERR_NOTFOUND);
    HH_ASSERT_EQ_STR(r.text, "no such worker");
    hh_radiod_release(&f.d);
}

static void test_detach_restores_unsupported(void)
{
    fixture_t f;
    hh_rc_response_t r;

    fixture_init(&f, true);
    r = ask(&f.d, HH_RC_CMD_OPENCPI_LIST, NULL, NULL, NULL);
    HH_ASSERT(r.ok);

    hh_radiod_set_props(&f.d, NULL);
    r = ask(&f.d, HH_RC_CMD_OPENCPI_LIST, NULL, NULL, NULL);
    HH_ASSERT(!r.ok);
    HH_ASSERT_EQ_INT(r.reason, HH_ERR_UNSUPPORTED);
    hh_radiod_release(&f.d);
}

static void test_released_refuses_property_verbs(void)
{
    fixture_t f;
    hh_rc_response_t r;

    fixture_init(&f, true);
    bring_up(&f.d);
    {
        hh_rc_request_t req;
        hh_rc_response_t resp;
        memset(&req, 0, sizeof req);
        req.cmd = HH_RC_CMD_SHUTDOWN;
        hh_radiod_handle_request(&f.d, &req, &resp);
    }
    r = ask(&f.d, HH_RC_CMD_OPENCPI_GET, "alpha", "gain", NULL);
    HH_ASSERT(!r.ok);
    HH_ASSERT_EQ_INT(r.reason, HH_ERR_STATE);
    hh_radiod_release(&f.d);
}

HH_TEST_MAIN_BEGIN("radiod_props")
    HH_RUN(test_no_props_backend_is_unsupported);
    HH_RUN(test_reachable_before_init);
    HH_RUN(test_running_flag_follows_lifecycle);
    HH_RUN(test_arguments_are_passed_through_verbatim);
    HH_RUN(test_missing_arguments_rejected_before_the_backend);
    HH_RUN(test_backend_failure_and_detail_are_relayed);
    HH_RUN(test_detach_restores_unsupported);
    HH_RUN(test_released_refuses_property_verbs);
HH_TEST_MAIN_END()
