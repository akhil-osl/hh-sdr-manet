/*
 * radiod fault registry tests.
 *
 * Two layers: the registry in isolation, then the registry as radiod actually
 * drives it, so the wiring is covered and not just the data structure.
 */
#include "hhsdr/radiod/events.h"
#include "hhsdr/radiod/mock_backend.h"
#include "hhsdr/radiod/radiod.h"
#include "hh_test.h"
#include <string.h>

/* ---- registry in isolation ---- */

static void test_starts_empty(void)
{
    hh_radiod_faults_t f;
    hh_radiod_faults_init(&f);

    HH_ASSERT(!hh_radiod_faults_any_active(&f));
    HH_ASSERT_EQ_INT(hh_radiod_faults_active_count(&f), 0);
    HH_ASSERT_EQ_INT(f.generation, 0);
    HH_ASSERT_EQ_INT(f.asserted_total, 0);
    HH_ASSERT_EQ_INT(f.cleared_total, 0);
}

static void test_assert_records_when_and_how_often(void)
{
    hh_radiod_faults_t f;
    const hh_radiod_fault_record_t *r;

    hh_radiod_faults_init(&f);
    hh_radiod_faults_assert(&f, HH_RC_FAULT_TX_FAILURE, 1000);

    r = hh_radiod_faults_get(&f, HH_RC_FAULT_TX_FAILURE);
    HH_ASSERT(r != NULL);
    HH_ASSERT(r->active);
    HH_ASSERT_EQ_INT(r->count, 1);
    HH_ASSERT_EQ_INT(r->first_seen, 1000);
    HH_ASSERT_EQ_INT(r->last_seen, 1000);
    HH_ASSERT_EQ_INT(hh_radiod_faults_active_count(&f), 1);
    HH_ASSERT_EQ_INT(f.generation, 1);
}

/* A backend re-reporting a standing condition every poll must not look like a
 * storm of distinct faults. */
static void test_reassert_is_not_a_new_event(void)
{
    hh_radiod_faults_t f;
    const hh_radiod_fault_record_t *r;

    hh_radiod_faults_init(&f);
    hh_radiod_faults_assert(&f, HH_RC_FAULT_RX_SILENCE, 500);
    hh_radiod_faults_assert(&f, HH_RC_FAULT_RX_SILENCE, 600);
    hh_radiod_faults_assert(&f, HH_RC_FAULT_RX_SILENCE, 700);

    r = hh_radiod_faults_get(&f, HH_RC_FAULT_RX_SILENCE);
    HH_ASSERT_EQ_INT(r->count, 3);        /* occurrences counted          */
    HH_ASSERT_EQ_INT(r->first_seen, 500); /* onset preserved              */
    HH_ASSERT_EQ_INT(r->last_seen, 700);  /* recency advanced             */
    HH_ASSERT_EQ_INT(f.generation, 1);    /* but only one state change    */
    HH_ASSERT_EQ_INT(f.asserted_total, 1);
}

static void test_clear_marks_inactive_but_keeps_history(void)
{
    hh_radiod_faults_t f;
    const hh_radiod_fault_record_t *r;

    hh_radiod_faults_init(&f);
    hh_radiod_faults_assert(&f, HH_RC_FAULT_HW_FAULT, 100);
    hh_radiod_faults_clear(&f, HH_RC_FAULT_HW_FAULT, 250);

    r = hh_radiod_faults_get(&f, HH_RC_FAULT_HW_FAULT);
    HH_ASSERT(!r->active);
    HH_ASSERT_EQ_INT(r->cleared_at, 250);
    /* History survives the clear -- "it happened once and is now fine" must
     * stay distinguishable from "it never happened". */
    HH_ASSERT_EQ_INT(r->count, 1);
    HH_ASSERT_EQ_INT(r->first_seen, 100);
    HH_ASSERT(!hh_radiod_faults_any_active(&f));
    HH_ASSERT_EQ_INT(f.cleared_total, 1);
}

static void test_clearing_inactive_fault_is_noop(void)
{
    hh_radiod_faults_t f;
    hh_radiod_faults_init(&f);

    hh_radiod_faults_clear(&f, HH_RC_FAULT_TX_FAILURE, 100);
    HH_ASSERT_EQ_INT(f.generation, 0);
    HH_ASSERT_EQ_INT(f.cleared_total, 0);
}

static void test_none_is_not_a_fault(void)
{
    hh_radiod_faults_t f;
    hh_radiod_faults_init(&f);

    hh_radiod_faults_assert(&f, HH_RC_FAULT_NONE, 100);
    HH_ASSERT(!hh_radiod_faults_any_active(&f));
    HH_ASSERT_EQ_INT(f.generation, 0);
    HH_ASSERT(hh_radiod_faults_get(&f, HH_RC_FAULT_NONE) == NULL);
}

static void test_multiple_kinds_tracked_independently(void)
{
    hh_radiod_faults_t f;
    hh_radiod_faults_init(&f);

    hh_radiod_faults_assert(&f, HH_RC_FAULT_TX_FAILURE, 10);
    hh_radiod_faults_assert(&f, HH_RC_FAULT_RX_SILENCE, 20);
    hh_radiod_faults_assert(&f, HH_RC_FAULT_BACKEND_IO, 30);
    HH_ASSERT_EQ_INT(hh_radiod_faults_active_count(&f), 3);

    hh_radiod_faults_clear(&f, HH_RC_FAULT_RX_SILENCE, 40);
    HH_ASSERT_EQ_INT(hh_radiod_faults_active_count(&f), 2);
    HH_ASSERT(hh_radiod_faults_get(&f, HH_RC_FAULT_TX_FAILURE)->active);
    HH_ASSERT(!hh_radiod_faults_get(&f, HH_RC_FAULT_RX_SILENCE)->active);

    hh_radiod_faults_clear_all(&f, 50);
    HH_ASSERT_EQ_INT(hh_radiod_faults_active_count(&f), 0);
}

/* Generation must change when state changes back and forth, so a polling
 * client can tell "nothing happened" from "happened and recovered". */
static void test_generation_detects_flap(void)
{
    hh_radiod_faults_t f;
    uint64_t g0, g1, g2;

    hh_radiod_faults_init(&f);
    g0 = f.generation;
    hh_radiod_faults_assert(&f, HH_RC_FAULT_TX_FAILURE, 100);
    g1 = f.generation;
    hh_radiod_faults_clear(&f, HH_RC_FAULT_TX_FAILURE, 200);
    g2 = f.generation;

    HH_ASSERT(g1 > g0);
    HH_ASSERT(g2 > g1);
}

static void test_null_safe(void)
{
    hh_radiod_faults_init(NULL);
    hh_radiod_faults_assert(NULL, HH_RC_FAULT_HW_FAULT, 1);
    hh_radiod_faults_clear(NULL, HH_RC_FAULT_HW_FAULT, 1);
    hh_radiod_faults_clear_all(NULL, 1);
    HH_ASSERT(!hh_radiod_faults_any_active(NULL));
    HH_ASSERT_EQ_INT(hh_radiod_faults_active_count(NULL), 0);
    HH_ASSERT(hh_radiod_faults_get(NULL, HH_RC_FAULT_HW_FAULT) == NULL);
}

/* ---- registry as radiod drives it ---- */

static void drive(hh_radiod_t *d, hh_rc_cmd_t cmd, hh_node_id_t node_id,
                  uint32_t channel, hh_rc_fault_t fault)
{
    hh_rc_request_t req;
    hh_rc_response_t resp;
    memset(&req, 0, sizeof req);
    req.cmd = cmd; req.node_id = node_id; req.channel = channel; req.fault = fault;
    hh_radiod_handle_request(d, &req, &resp);
}

static void test_inject_and_clear_through_radiod(void)
{
    hh_mock_backend_t backend;
    hh_radio_t radio;
    hh_radiod_t d;
    const hh_radiod_faults_t *f;

    hh_mock_backend_init(&backend, &radio);
    HH_ASSERT_OK(hh_radiod_init(&d, &radio, hh_clock_monotonic()));
    hh_radiod_set_fault_hook(&d, NULL, NULL);

    drive(&d, HH_RC_CMD_INIT, 0, 0, 0);
    drive(&d, HH_RC_CMD_CONFIGURE, 1, 0, 0);
    drive(&d, HH_RC_CMD_START, 0, 0, 0);

    f = hh_radiod_faults(&d);
    HH_ASSERT(f != NULL);
    HH_ASSERT(!hh_radiod_faults_any_active(f));

    drive(&d, HH_RC_CMD_INJECT_FAULT, 0, 0, HH_RC_FAULT_TX_FAILURE);
    HH_ASSERT(hh_radiod_faults_get(f, HH_RC_FAULT_TX_FAILURE)->active);
    HH_ASSERT_EQ_INT(hh_radiod_faults_active_count(f), 1);

    /* CLEAR_FAULT carries no kind, so it clears everything. */
    drive(&d, HH_RC_CMD_CLEAR_FAULT, 0, 0, 0);
    HH_ASSERT(!hh_radiod_faults_any_active(f));
    HH_ASSERT_EQ_INT(hh_radiod_faults_get(f, HH_RC_FAULT_TX_FAILURE)->count, 1);

    hh_radiod_release(&d);
}

/* A backend that refuses to open must be recorded, not merely reflected in
 * the lifecycle state. */
static void test_failed_start_records_hw_fault(void)
{
    hh_mock_backend_t backend;
    hh_radio_t radio;
    hh_radiod_t d;
    const hh_radiod_faults_t *f;

    hh_mock_backend_init(&backend, &radio);
    HH_ASSERT_OK(hh_radiod_init(&d, &radio, hh_clock_monotonic()));

    /* An hh_radio_t with no ops fails open() with HH_ERR_INVAL. */
    radio.ops = NULL;

    drive(&d, HH_RC_CMD_INIT, 0, 0, 0);
    drive(&d, HH_RC_CMD_CONFIGURE, 1, 0, 0);
    drive(&d, HH_RC_CMD_START, 0, 0, 0);

    f = hh_radiod_faults(&d);
    HH_ASSERT(hh_radiod_faults_get(f, HH_RC_FAULT_HW_FAULT)->active);

    hh_radiod_release(&d);
}

HH_TEST_MAIN_BEGIN("radiod fault registry")
    HH_RUN(test_starts_empty);
    HH_RUN(test_assert_records_when_and_how_often);
    HH_RUN(test_reassert_is_not_a_new_event);
    HH_RUN(test_clear_marks_inactive_but_keeps_history);
    HH_RUN(test_clearing_inactive_fault_is_noop);
    HH_RUN(test_none_is_not_a_fault);
    HH_RUN(test_multiple_kinds_tracked_independently);
    HH_RUN(test_generation_detects_flap);
    HH_RUN(test_null_safe);
    HH_RUN(test_inject_and_clear_through_radiod);
    HH_RUN(test_failed_start_records_hw_fault);
HH_TEST_MAIN_END()
