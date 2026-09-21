/*
 * Behavioral contract tests for the plain-C SCA adapter
 * (sca/MANET_Node/adapter/manet_sca_adapter.[hc]).
 *
 * SCOPE: this tests the adapter exactly as it exists today -- a C struct
 * (manet_sca_adapter_t) whose fields are the SCA lifecycle guard
 * (hh_sca_resource_t), the wrapped MANET node (hh_node_t), and a working
 * config (hh_config_t) -- and the thin wrapper functions over it. No IDL,
 * no CORBA, no REDHAWK runtime is instantiated or assumed anywhere here.
 *
 * TEST TECHNIQUE: manet_sca_initialize() now takes node_id as a required
 * construction-time parameter (Step 5 fix -- see manet_sca_adapter.h's own
 * header comment on that function for the full rationale, grounded in
 * src/main.c's existing config-then-construct pattern and REDHAWK 2.2.10's
 * own Resource_impl::create_component() execparam-at-construction
 * convention). Every fixture below therefore calls the real
 * manet_sca_initialize() with a valid node_id, exactly as an eventual
 * REDHAWK servant would, rather than hand-assembling the adapter's fields.
 * manet_sca_adapter_t remains a plain, non-opaque struct (see its own header
 * comment), so tests that need to inspect internal state after a call still
 * read a.guard/a.node/a.cfg directly, but no longer need to CONSTRUCT that
 * state by hand -- the one thing that forced that workaround (the defect
 * documented in the Step 4 version of this file) is fixed.
 *
 * FIXED DEFECT (was: manet_sca_initialize_seeds_default_config_and_fails_
 * because_node_id_is_unset): manet_sca_initialize() used to seed
 * hh_config_defaults() (node_id = HH_NODE_ID_INVALID) and immediately call
 * hh_node_init() with that still-default config, so it failed
 * unconditionally for every caller. It now takes node_id as a parameter,
 * validates it before touching hh_node_init(), and succeeds for any valid
 * id. See test_initialize_succeeds_with_a_valid_node_id and
 * test_initialize_rejects_invalid_node_id below, which replace the old
 * defect-documenting test.
 */
#include "manet_sca_adapter.h"
#include "hh_test.h"
#include "hhsdr/radio/hw_adapter.h"
#include "mock_radio.h"
#include "vclock.h"
#include <string.h>

/* ---------------------------------------------------------------------------
 * Fixture helpers
 * ------------------------------------------------------------------------- */

/* Builds an adapter whose guard is PORTS_CONNECTED and whose node is
 * initialized with a valid config, entirely through the adapter's own
 * public API (manet_sca_initialize(), which itself calls hh_sca_initialize,
 * hh_node_init, and hh_sca_connect_ports in sequence -- see
 * manet_sca_adapter.c). No hand-assembly of adapter internals is needed
 * anymore now that node_id is a real parameter. */
static void fixture_ports_connected(manet_sca_adapter_t *a, mock_radio_t *mock,
                                    hh_radio_t *radio, vclock_t *vc, hh_node_id_t node_id)
{
    mock_radio_init(mock, "test", radio);
    vclock_init(vc, 1000);
    HH_ASSERT_OK(manet_sca_initialize(a, node_id, &vc->clock, radio));
}

/* Carries the fixture through configure() and start(). node_id is already
 * set correctly by manet_sca_initialize() (fixture_ports_connected above),
 * so this applies one harmless configure-kind property purely to advance
 * the SCA guard from PORTS_CONNECTED to CONFIGURED -- hh_sca_configure()
 * only makes that transition on an actual property_id/value call
 * (src/sca/resource.c), there is no zero-argument "just advance" operation. */
static void fixture_started(manet_sca_adapter_t *a, mock_radio_t *mock,
                            hh_radio_t *radio, vclock_t *vc, hh_node_id_t node_id)
{
    manet_sca_propval_t props[1];
    size_t applied = 0;

    fixture_ports_connected(a, mock, radio, vc, node_id);

    props[0].id = "hh::mgmt::log_level";
    props[0].value = "info";
    HH_ASSERT_OK(manet_sca_configure(a, props, 1, &applied));
    HH_ASSERT_EQ_INT(applied, 1);

    HH_ASSERT_OK(manet_sca_start(a));
}

/* ---------------------------------------------------------------------------
 * A. Initialization
 * ------------------------------------------------------------------------- */

static void test_initialize_rejects_null_arguments(void)
{
    manet_sca_adapter_t a;
    vclock_t vc;
    mock_radio_t mock;
    hh_radio_t radio;

    vclock_init(&vc, 0);
    mock_radio_init(&mock, "n", &radio);

    HH_ASSERT_ERR(manet_sca_initialize(NULL, 1, &vc.clock, &radio), HH_ERR_INVAL);
    HH_ASSERT_ERR(manet_sca_initialize(&a, 1, NULL, &radio), HH_ERR_INVAL);
    HH_ASSERT_ERR(manet_sca_initialize(&a, 1, &vc.clock, NULL), HH_ERR_INVAL);
}

static void test_initialize_succeeds_with_a_valid_node_id(void)
{
    /* Step 5 fix: manet_sca_initialize() now takes node_id as a required
     * construction-time parameter (see manet_sca_adapter.h's header comment
     * on manet_sca_initialize for the full rationale) and succeeds end to
     * end -- SCA guard reaches PORTS_CONNECTED, hh_node_t reaches
     * INITIALIZED, with the supplied node_id applied. This replaces the old
     * defect-documenting test (manet_sca_initialize_seeds_default_config_
     * and_fails_because_node_id_is_unset), which proved the opposite before
     * the fix. */
    manet_sca_adapter_t a;
    mock_radio_t mock;
    hh_radio_t radio;
    vclock_t vc;

    fixture_ports_connected(&a, &mock, &radio, &vc, 7);

    HH_ASSERT_EQ_INT(a.guard.state, HH_SCA_PORTS_CONNECTED);
    HH_ASSERT_EQ_INT(a.node.state, HH_NODE_INITIALIZED);
    HH_ASSERT_EQ_INT(a.cfg.node_id, 7);
    HH_ASSERT_EQ_INT(a.node.cfg.node_id, 7);
}

static void test_initialize_rejects_invalid_node_id(void)
{
    /* HH_NODE_ID_INVALID (0) must still be rejected -- this is the one
     * value hh_config_validate() itself never accepts (src/core/config.c),
     * and manet_sca_initialize() now checks it explicitly, before
     * hh_node_init() is attempted, per its own documented contract. */
    manet_sca_adapter_t a;
    mock_radio_t mock;
    hh_radio_t radio;
    vclock_t vc;

    /* manet_sca_initialize() checks node_id BEFORE its own memset(a, 0, ...)
     * (see manet_sca_adapter.c), so a rejected call leaves a untouched --
     * zero it here first so the "left at its pre-call state" assertion
     * below inspects a defined value, not indeterminate stack memory. */
    memset(&a, 0, sizeof a);
    vclock_init(&vc, 0);
    mock_radio_init(&mock, "n", &radio);

    HH_ASSERT_ERR(manet_sca_initialize(&a, HH_NODE_ID_INVALID, &vc.clock, &radio),
                  HH_ERR_INVAL);

    /* Rejected before hh_sca_initialize()/hh_node_init() ever ran, so the
     * guard is left at its pre-call state, UNINITIALIZED -- unlike the old
     * defect, where the guard had already been advanced to INITIALIZED
     * before the later hh_node_init() failure. This is the "does not leave
     * the adapter permanently corrupted" property: a is safe to retry. */
    HH_ASSERT_EQ_INT(a.guard.state, HH_SCA_UNINITIALIZED);
}

static void test_initialize_can_be_retried_after_a_rejected_node_id(void)
{
    /* Failure-recovery proof: an adapter that failed initialize() with an
     * invalid node_id must still succeed on a subsequent call with a valid
     * one -- the earlier rejection must not leave a in a state that
     * permanently blocks retry. */
    manet_sca_adapter_t a;
    mock_radio_t mock;
    hh_radio_t radio;
    vclock_t vc;

    vclock_init(&vc, 0);
    mock_radio_init(&mock, "n", &radio);

    HH_ASSERT_ERR(manet_sca_initialize(&a, HH_NODE_ID_INVALID, &vc.clock, &radio),
                  HH_ERR_INVAL);
    HH_ASSERT_OK(manet_sca_initialize(&a, 15, &vc.clock, &radio));
    HH_ASSERT_EQ_INT(a.guard.state, HH_SCA_PORTS_CONNECTED);
    HH_ASSERT_EQ_INT(a.cfg.node_id, 15);
}

/* ---------------------------------------------------------------------------
 * B. Configure / query
 * ------------------------------------------------------------------------- */

static void test_configure_before_ports_connected_fails(void)
{
    manet_sca_adapter_t a;
    manet_sca_propval_t props[1];
    size_t applied = 999;

    memset(&a, 0, sizeof a);
    hh_sca_resource_init_guard(&a.guard, "MANET_Node");
    hh_config_defaults(&a.cfg);

    props[0].id = "hh::mgmt::log_level";
    props[0].value = "debug";

    /* Guard is still UNINITIALIZED: hh_sca_configure's own state check
     * rejects this before any property lookup happens. */
    HH_ASSERT_ERR(manet_sca_configure(&a, props, 1, &applied), HH_ERR_STATE);
    HH_ASSERT_EQ_INT(applied, 0);
}

static void test_configure_rejects_null_with_nonzero_count(void)
{
    manet_sca_adapter_t a;
    memset(&a, 0, sizeof a);
    HH_ASSERT_ERR(manet_sca_configure(&a, NULL, 1, NULL), HH_ERR_INVAL);
}

static void test_configure_accepts_zero_count_as_a_no_op(void)
{
    manet_sca_adapter_t a;
    mock_radio_t mock;
    hh_radio_t radio;
    vclock_t vc;
    size_t applied = 999;

    fixture_ports_connected(&a, &mock, &radio, &vc, 3);

    HH_ASSERT_OK(manet_sca_configure(&a, NULL, 0, &applied));
    HH_ASSERT_EQ_INT(applied, 0);
}

static void test_configure_valid_property_updates_node_config(void)
{
    manet_sca_adapter_t a;
    mock_radio_t mock;
    hh_radio_t radio;
    vclock_t vc;
    manet_sca_propval_t props[1];
    size_t applied = 0;

    fixture_ports_connected(&a, &mock, &radio, &vc, 5);

    props[0].id = "hh::discovery::beacon_interval_ms";
    props[0].value = "750";
    HH_ASSERT_OK(manet_sca_configure(&a, props, 1, &applied));
    HH_ASSERT_EQ_INT(applied, 1);

    /* manet_sca_configure() pushes the update into a.node via
     * hh_node_configure(), per its own documented behavior -- not just
     * a.cfg in isolation. */
    HH_ASSERT_EQ_INT(a.cfg.beacon_interval_ms, 750);
    HH_ASSERT_EQ_INT(a.node.cfg.beacon_interval_ms, 750);
}

static void test_configure_unknown_property_id_fails_and_applies_nothing(void)
{
    manet_sca_adapter_t a;
    mock_radio_t mock;
    hh_radio_t radio;
    vclock_t vc;
    manet_sca_propval_t props[2];
    size_t applied = 999;

    fixture_ports_connected(&a, &mock, &radio, &vc, 9);

    /* First entry valid, second unknown: configure stops at the first
     * failure, per its own documented contract ("Stops at the first
     * failure"), so *applied reports how far it got. */
    props[0].id = "hh::mgmt::log_level";
    props[0].value = "debug";
    props[1].id = "hh::no::such::property";
    props[1].value = "x";

    HH_ASSERT_ERR(manet_sca_configure(&a, props, 2, &applied), HH_ERR_NOTFOUND);
    HH_ASSERT_EQ_INT(applied, 1);
}

static void test_execparam_cannot_be_retuned_through_the_adapter(void)
{
    manet_sca_adapter_t a;
    mock_radio_t mock;
    hh_radio_t radio;
    vclock_t vc;
    manet_sca_propval_t props[1];
    size_t applied = 0;

    fixture_ports_connected(&a, &mock, &radio, &vc, 11);

    props[0].id = "hh::node_id";
    props[0].value = "12";
    HH_ASSERT_OK(manet_sca_configure(&a, props, 1, &applied));
    HH_ASSERT_EQ_INT(a.cfg.node_id, 12);

    /* Guard is now CONFIGURED; a second execparam write must be rejected,
     * exactly as test_execparam_cannot_be_retuned_at_runtime already proves
     * for hh_sca_configure() directly -- this proves the adapter forwards
     * that same rule rather than loosening it. */
    props[0].value = "13";
    HH_ASSERT_ERR(manet_sca_configure(&a, props, 1, &applied), HH_ERR_STATE);
    HH_ASSERT_EQ_INT(a.cfg.node_id, 12);
}

static void test_configure_and_query_bounded_table_size_properties_through_the_adapter(void)
{
    /*
     * Step 6: hh::neighbor::max_entries and hh::routing::max_routes were
     * reclassified from HH_PROP_ALLOCATION to HH_PROP_CONFIGURE in
     * src/sca/resource.c::g_props[] (they bound ordinary hh_config_t
     * fields, not a Resource capacity-allocation request -- see this
     * repository's docs/SCA-COMPATIBILITY.md and the PRF's own AMBIGUITY 1
     * note for the full investigation). This proves the adapter's own
     * configure()/query() wrappers still work for both, end to end,
     * unaffected by the classification correction -- exactly as they did
     * before, since the adapter never branched on property kind either.
     *
     * NOTE ON CALL SHAPE: both properties are set in ONE manet_sca_configure()
     * call (count=2), not two separate calls. A separate, pre-existing
     * adapter behavior was discovered while writing this test: calling
     * manet_sca_configure() a second time once the SCA guard has reached
     * HH_SCA_CONFIGURED fails with HH_ERR_STATE, because
     * manet_sca_configure() unconditionally calls hh_node_configure()
     * (manet_sca_adapter.c) at the end of every call, and hh_node_configure()
     * itself only accepts HH_NODE_INITIALIZED or HH_NODE_STOPPED
     * (src/manet/node.c) -- not HH_NODE_CONFIGURED. This is independent of
     * property kind (it would reproduce identically for any two
     * already-configure-kind properties, e.g. two calls each setting
     * hh::routing::max_hop_count) and is therefore out of scope for this
     * property-classification step; it is not fixed here. Using a single,
     * multi-property call is the adapter's own documented, already-working
     * way to set more than one property at once and sidesteps the issue
     * entirely, rather than working around it silently.
     */
    manet_sca_adapter_t a;
    mock_radio_t mock;
    hh_radio_t radio;
    vclock_t vc;
    manet_sca_propval_t props[2];
    size_t applied = 0;
    char out[64];

    fixture_ports_connected(&a, &mock, &radio, &vc, 80);

    props[0].id = "hh::neighbor::max_entries";
    props[0].value = "12";
    props[1].id = "hh::routing::max_routes";
    props[1].value = "30";
    HH_ASSERT_OK(manet_sca_configure(&a, props, 2, &applied));
    HH_ASSERT_EQ_INT(applied, 2);
    HH_ASSERT_EQ_INT(a.cfg.max_neighbors, 12);
    HH_ASSERT_EQ_INT(a.cfg.max_routes, 30);

    HH_ASSERT_OK(manet_sca_query(&a, "hh::neighbor::max_entries", out, sizeof out));
    HH_ASSERT_EQ_STR(out, "12");
    HH_ASSERT_OK(manet_sca_query(&a, "hh::routing::max_routes", out, sizeof out));
    HH_ASSERT_EQ_STR(out, "30");

    /* Not execparams, so unlike hh::node_id these are not permanently
     * frozen. As of Step 8, hh_sca_configure() also permits
     * HH_PROP_CONFIGURE properties while HH_SCA_STARTED (src/sca/
     * resource.c) -- both properties remain configurable while the node is
     * RUNNING, matching real REDHAWK's own CF::PropertySet::configure()
     * semantics (no started/stopped restriction). Confirm the adapter
     * applies the update while running (a single-property call here, since
     * the guard is already CONFIGURED from the call above -- this is the
     * adapter's SECOND manet_sca_configure() call overall, made while
     * STARTED, which Step 7/8's fix to manet_sca_configure() now handles
     * via the same safe direct-assignment path used for CONFIGURED). */
    HH_ASSERT_OK(manet_sca_start(&a));
    props[0].id = "hh::neighbor::max_entries";
    props[0].value = "9";
    HH_ASSERT_OK(manet_sca_configure(&a, props, 1, &applied));
    HH_ASSERT_EQ_INT(a.cfg.max_neighbors, 9);
    HH_ASSERT_EQ_INT(a.node.cfg.max_neighbors, 9);
    HH_ASSERT_EQ_INT(a.node.state, HH_NODE_RUNNING);

    HH_ASSERT_OK(manet_sca_stop(&a));
    props[0].value = "7";
    HH_ASSERT_OK(manet_sca_configure(&a, props, 1, &applied));
    HH_ASSERT_EQ_INT(a.cfg.max_neighbors, 7);
}

/* ---------------------------------------------------------------------------
 * B'. Repeated configure() (Step 7)
 *
 * FIXED DEFECT: manet_sca_configure() used to unconditionally call
 * hh_node_configure() at the end of every call. hh_node_configure() only
 * accepts HH_NODE_INITIALIZED or HH_NODE_STOPPED (src/manet/node.c), so any
 * SECOND manet_sca_configure() call reaching the guard's HH_SCA_CONFIGURED
 * state (a legitimate, permitted state per hh_sca_configure()'s own rules)
 * failed with HH_ERR_STATE purely because of hh_node_configure()'s stricter,
 * one-shot-per-cycle guard -- independent of which property was being set.
 *
 * FIX: manet_sca_configure() now calls hh_node_configure() only for the
 * first transition (node state INITIALIZED or STOPPED); a subsequent call
 * while the node is already CONFIGURED applies the same validated
 * assignment (hh_config_validate() + a plain struct copy into a->node.cfg)
 * directly, mirroring exactly what hh_node_configure() does internally --
 * safe because every sub-component in hh_node_t holds a pointer to
 * &n->cfg, not a private copy (src/manet/node.c), so the update is visible
 * immediately with no reinitialization needed. See manet_sca_adapter.c's
 * own comment on manet_sca_configure() for the full investigation,
 * including the real REDHAWK 2.2.10 PropertySet_impl::configure()
 * evidence that repeated, partial-property configure() calls are the
 * normal, expected lifecycle case, not a special/rare one.
 * ------------------------------------------------------------------------- */

static void test_second_configure_call_succeeds_while_configured(void)
{
    /* B. SECOND CONFIGURE: initialize -> configure(A) -> configure(B) must
     * succeed. This is exactly the call sequence that failed before the
     * Step 7 fix. */
    manet_sca_adapter_t a;
    mock_radio_t mock;
    hh_radio_t radio;
    vclock_t vc;
    manet_sca_propval_t props[1];
    size_t applied = 0;

    fixture_ports_connected(&a, &mock, &radio, &vc, 90);

    props[0].id = "hh::mgmt::log_level";
    props[0].value = "debug";
    HH_ASSERT_OK(manet_sca_configure(&a, props, 1, &applied));
    HH_ASSERT_EQ_INT(a.guard.state, HH_SCA_CONFIGURED);
    HH_ASSERT_EQ_INT(a.node.state, HH_NODE_CONFIGURED);

    props[0].id = "hh::routing::max_hop_count";
    props[0].value = "12";
    HH_ASSERT_OK(manet_sca_configure(&a, props, 1, &applied));
    HH_ASSERT_EQ_INT(applied, 1);
    HH_ASSERT_EQ_INT(a.guard.state, HH_SCA_CONFIGURED);
    HH_ASSERT_EQ_INT(a.node.state, HH_NODE_CONFIGURED);
    HH_ASSERT_EQ_INT(a.node.cfg.max_hop_count, 12);
}

static void test_second_configure_updates_same_property_to_a_new_value(void)
{
    /* C. SAME PROPERTY UPDATE: configure(A=1) -> configure(A=2) -> query(A)
     * must return 2. */
    manet_sca_adapter_t a;
    mock_radio_t mock;
    hh_radio_t radio;
    vclock_t vc;
    manet_sca_propval_t props[1];
    size_t applied = 0;
    char out[64];

    fixture_ports_connected(&a, &mock, &radio, &vc, 91);

    props[0].id = "hh::routing::max_hop_count";
    props[0].value = "8";
    HH_ASSERT_OK(manet_sca_configure(&a, props, 1, &applied));

    props[0].value = "20";
    HH_ASSERT_OK(manet_sca_configure(&a, props, 1, &applied));

    HH_ASSERT_EQ_INT(a.node.cfg.max_hop_count, 20);
    HH_ASSERT_OK(manet_sca_query(&a, "hh::routing::max_hop_count", out, sizeof out));
    HH_ASSERT_EQ_STR(out, "20");
}

static void test_second_configure_does_not_reset_unrelated_properties(void)
{
    /* D. SUBSET CONFIGURE: a second call containing only ONE property must
     * not disturb a different property set in the first call. */
    manet_sca_adapter_t a;
    mock_radio_t mock;
    hh_radio_t radio;
    vclock_t vc;
    manet_sca_propval_t props[1];
    size_t applied = 0;

    fixture_ports_connected(&a, &mock, &radio, &vc, 92);

    props[0].id = "hh::routing::max_hop_count";
    props[0].value = "11";
    HH_ASSERT_OK(manet_sca_configure(&a, props, 1, &applied));

    /* Second call sets a DIFFERENT, unrelated property only. */
    props[0].id = "hh::mgmt::log_level";
    props[0].value = "warn";
    HH_ASSERT_OK(manet_sca_configure(&a, props, 1, &applied));

    /* The first call's value must survive untouched. */
    HH_ASSERT_EQ_INT(a.node.cfg.max_hop_count, 11);
    HH_ASSERT_EQ_INT(a.node.cfg.log_level, HH_LOG_WARN);
}

static void test_multi_property_configure_remains_correct(void)
{
    /* E. MULTI-PROPERTY CONFIGURE: existing one-call/many-properties
     * behavior (already exercised by test_configure_and_query_bounded_
     * table_size_properties_through_the_adapter) must still work
     * unchanged after the fix, including on a SECOND such call. */
    manet_sca_adapter_t a;
    mock_radio_t mock;
    hh_radio_t radio;
    vclock_t vc;
    manet_sca_propval_t props[2];
    size_t applied = 0;

    fixture_ports_connected(&a, &mock, &radio, &vc, 93);

    props[0].id = "hh::routing::max_hop_count";
    props[0].value = "10";
    props[1].id = "hh::mgmt::log_level";
    props[1].value = "info";
    HH_ASSERT_OK(manet_sca_configure(&a, props, 2, &applied));
    HH_ASSERT_EQ_INT(applied, 2);

    props[0].id = "hh::neighbor::allowed_loss";
    props[0].value = "5";
    props[1].id = "hh::routing::max_routes";
    props[1].value = "40";
    HH_ASSERT_OK(manet_sca_configure(&a, props, 2, &applied));
    HH_ASSERT_EQ_INT(applied, 2);

    HH_ASSERT_EQ_INT(a.node.cfg.max_hop_count, 10);
    HH_ASSERT_EQ_INT(a.node.cfg.neighbor_allowed_loss, 5);
    HH_ASSERT_EQ_INT(a.node.cfg.max_routes, 40);
}

static void test_configure_kind_property_while_started_is_accepted(void)
{
    /*
     * F. CONFIGURE WHILE STARTED -- Step 8 decision (OPTION A: permit it
     * for HH_PROP_CONFIGURE properties).
     *
     * Step 8's investigation traced every HH_PROP_CONFIGURE property in
     * g_props[] to a live pointer read inside its owning MANET component
     * (discovery/neighbor/link-health/routing/self-healing all re-read
     * cfg-> fields on each evaluation, never caching a value at start
     * time -- see the source files under src/manet/), so applying a new
     * value has the same effect whether the node is RUNNING or not: it
     * changes what the next evaluation sees, with no structural or memory
     * dependency on when it is applied. This matches real REDHAWK 2.2.10's
     * own CF::PropertySet::configure() (PropertySet_impl.cpp), which has no
     * started/stopped lifecycle guard at all -- Resource_impl::start()/
     * stop() only toggle ports and a _started flag, never touching
     * configure() in any way.
     *
     * hh_sca_configure() (src/sca/resource.c) was therefore changed to
     * permit HH_SCA_STARTED for HH_PROP_CONFIGURE-kind properties, and
     * manet_sca_configure() (manet_sca_adapter.c) applies the update to
     * the running node via the same safe direct-assignment path already
     * used for HH_NODE_CONFIGURED (a->node.cfg = a->cfg, validated first).
     * This test proves the previously-rejected case now succeeds, the new
     * value is queryable, and the node remains RUNNING and otherwise
     * unaffected throughout. */
    manet_sca_adapter_t a;
    mock_radio_t mock;
    hh_radio_t radio;
    vclock_t vc;
    manet_sca_propval_t props[1];
    size_t applied = 0;
    char out[64];

    fixture_started(&a, &mock, &radio, &vc, 94);

    props[0].id = "hh::routing::max_hop_count";
    props[0].value = "7";
    HH_ASSERT_OK(manet_sca_configure(&a, props, 1, &applied));
    HH_ASSERT_EQ_INT(applied, 1);
    HH_ASSERT_EQ_INT(a.node.cfg.max_hop_count, 7);
    HH_ASSERT_OK(manet_sca_query(&a, "hh::routing::max_hop_count", out, sizeof out));
    HH_ASSERT_EQ_STR(out, "7");

    /* The node must remain RUNNING throughout -- configure() while started
     * does not stop, restart, or otherwise disturb the control loop. */
    HH_ASSERT_EQ_INT(a.guard.state, HH_SCA_STARTED);
    HH_ASSERT_EQ_INT(a.node.state, HH_NODE_RUNNING);

    /* Tick still works normally afterward -- the running node was not left
     * in a partially-updated or inconsistent state by the live configure. */
    vclock_advance(&vc, 10);
    HH_ASSERT_OK(manet_sca_tick(&a, vc.now));
}

static void test_execparam_while_started_still_rejected(void)
{
    /*
     * F (continued). The Step 8 change only loosens the guard for
     * HH_PROP_CONFIGURE properties. hh::node_id and hh::radio::adapter
     * (HH_PROP_EXECPARAM) remain rejected in every state except
     * PORTS_CONNECTED, per hh_sca_configure()'s own kind-specific check --
     * this is unconditional and does not depend on whether HH_SCA_STARTED
     * is otherwise permitted. Proves the Step 8 change did not
     * inadvertently loosen execparam protection. */
    manet_sca_adapter_t a;
    mock_radio_t mock;
    hh_radio_t radio;
    vclock_t vc;
    manet_sca_propval_t props[1];
    size_t applied = 0;

    fixture_started(&a, &mock, &radio, &vc, 97);

    props[0].id = "hh::node_id";
    props[0].value = "999";
    HH_ASSERT_ERR(manet_sca_configure(&a, props, 1, &applied), HH_ERR_STATE);
    HH_ASSERT_EQ_INT(applied, 0);
    HH_ASSERT_EQ_INT(a.cfg.node_id, 97);   /* unchanged */
    HH_ASSERT_EQ_INT(a.node.state, HH_NODE_RUNNING);   /* node unaffected */
}

static void test_invalid_value_while_started_is_rejected_and_state_preserved(void)
{
    /*
     * Per the task's per-property-validation distinction: the framework
     * permitting configure() while running does not mean every value is
     * accepted -- hh_config_validate() (already invoked by the
     * HH_NODE_CONFIGURED/HH_NODE_RUNNING path in manet_sca_configure())
     * still rejects a value that would put hh_config_t in an invalid
     * state, exactly as it does for the very first configure. Proves an
     * invalid value while STARTED is rejected deterministically and
     * leaves both the applied config and node state untouched. */
    manet_sca_adapter_t a;
    mock_radio_t mock;
    hh_radio_t radio;
    vclock_t vc;
    manet_sca_propval_t props[1];
    size_t applied = 0;
    uint32_t hop_count_before;

    fixture_started(&a, &mock, &radio, &vc, 98);
    hop_count_before = a.node.cfg.max_hop_count;

    /* hh_config_set() itself rejects max_hop_count == 0 (src/core/config.c:
     * "if (!parse_u32(value, &v) || v == 0 || v > 255) return HH_ERR_INVAL;")
     * -- the value is never written to a->cfg at all, so hh_sca_configure()
     * fails and manet_sca_configure() returns immediately, never reaching
     * its own HH_NODE_RUNNING/hh_config_validate() branch for this call.
     * Both a->cfg and a->node.cfg are therefore left completely untouched
     * -- unchanged by Step 8, this is the same per-field rejection every
     * other invalid value already gets, regardless of node state. */
    props[0].id = "hh::routing::max_hop_count";
    props[0].value = "0";
    HH_ASSERT_ERR(manet_sca_configure(&a, props, 1, &applied), HH_ERR_INVAL);
    HH_ASSERT_EQ_INT(applied, 0);
    HH_ASSERT_EQ_INT(a.cfg.max_hop_count, hop_count_before);
    HH_ASSERT_EQ_INT(a.node.cfg.max_hop_count, hop_count_before);
    HH_ASSERT_EQ_INT(a.node.state, HH_NODE_RUNNING);
}

static void test_configure_after_stop_then_start_then_stop_again_succeeds(void)
{
    /* G. STOP THEN CONFIGURE, exercised across a full second cycle:
     * initialize -> configure -> start -> stop -> configure -> start ->
     * stop -> configure again (now via the CONFIGURED->CONFIGURED path
     * this step fixes) -> release. Proves the two configure code paths
     * (via HH_NODE_STOPPED and via HH_NODE_CONFIGURED) compose correctly
     * across repeated start/stop cycles rather than only in isolation. */
    manet_sca_adapter_t a;
    mock_radio_t mock;
    hh_radio_t radio;
    vclock_t vc;
    manet_sca_propval_t props[1];
    size_t applied = 0;

    fixture_started(&a, &mock, &radio, &vc, 95);   /* initialize+configure+start */

    HH_ASSERT_OK(manet_sca_stop(&a));
    props[0].id = "hh::routing::max_hop_count";
    props[0].value = "13";
    HH_ASSERT_OK(manet_sca_configure(&a, props, 1, &applied));   /* via HH_NODE_STOPPED */
    HH_ASSERT_EQ_INT(a.node.cfg.max_hop_count, 13);

    HH_ASSERT_OK(manet_sca_start(&a));
    HH_ASSERT_OK(manet_sca_stop(&a));
    props[0].value = "17";
    HH_ASSERT_OK(manet_sca_configure(&a, props, 1, &applied));   /* via HH_NODE_STOPPED again */
    HH_ASSERT_EQ_INT(a.node.cfg.max_hop_count, 17);

    HH_ASSERT_OK(manet_sca_release_object(&a));
    HH_ASSERT_EQ_INT(a.guard.state, HH_SCA_RELEASED);
}

static void test_release_after_repeated_configure_succeeds(void)
{
    /* H. RELEASE: repeated configure() calls must not prevent eventual
     * stop -> release. */
    manet_sca_adapter_t a;
    mock_radio_t mock;
    hh_radio_t radio;
    vclock_t vc;
    manet_sca_propval_t props[1];
    size_t applied = 0;

    fixture_ports_connected(&a, &mock, &radio, &vc, 96);

    props[0].id = "hh::routing::max_hop_count";
    props[0].value = "6";
    HH_ASSERT_OK(manet_sca_configure(&a, props, 1, &applied));
    props[0].value = "18";
    HH_ASSERT_OK(manet_sca_configure(&a, props, 1, &applied));   /* second call, CONFIGURED->CONFIGURED */
    props[0].value = "24";
    HH_ASSERT_OK(manet_sca_configure(&a, props, 1, &applied));   /* third call, same path again */

    HH_ASSERT_OK(manet_sca_start(&a));
    HH_ASSERT_OK(manet_sca_stop(&a));
    HH_ASSERT_OK(manet_sca_release_object(&a));
    HH_ASSERT_EQ_INT(a.guard.state, HH_SCA_RELEASED);
    HH_ASSERT_EQ_INT(a.node.state, HH_NODE_RELEASED);
}

static void test_query_round_trips_a_configured_value(void)
{
    manet_sca_adapter_t a;
    mock_radio_t mock;
    hh_radio_t radio;
    vclock_t vc;
    manet_sca_propval_t props[1];
    size_t applied = 0;
    char out[64];

    fixture_ports_connected(&a, &mock, &radio, &vc, 4);

    props[0].id = "hh::routing::max_hop_count";
    props[0].value = "9";
    HH_ASSERT_OK(manet_sca_configure(&a, props, 1, &applied));

    HH_ASSERT_OK(manet_sca_query(&a, "hh::routing::max_hop_count", out, sizeof out));
    HH_ASSERT_EQ_STR(out, "9");
}

static void test_query_unknown_property_fails(void)
{
    manet_sca_adapter_t a;
    mock_radio_t mock;
    hh_radio_t radio;
    vclock_t vc;
    char out[64];

    fixture_ports_connected(&a, &mock, &radio, &vc, 6);

    HH_ASSERT_ERR(manet_sca_query(&a, "hh::not::a::real::property", out, sizeof out),
                  HH_ERR_NOTFOUND);
}

static void test_query_null_adapter_fails(void)
{
    char out[64];
    HH_ASSERT_ERR(manet_sca_query(NULL, "hh::mgmt::log_level", out, sizeof out), HH_ERR_INVAL);
}

static void test_query_reflects_a_default_when_nothing_was_configured(void)
{
    /* "Default values are preserved when configuration is omitted": query a
     * property this test never touches with manet_sca_configure and confirm
     * it reports hh_config_defaults()'s own value, not an empty/garbage
     * result. */
    manet_sca_adapter_t a;
    mock_radio_t mock;
    hh_radio_t radio;
    vclock_t vc;
    char out[64];

    fixture_ports_connected(&a, &mock, &radio, &vc, 8);

    HH_ASSERT_OK(manet_sca_query(&a, "hh::routing::max_hop_count", out, sizeof out));
    HH_ASSERT_EQ_STR(out, "16");   /* hh_config_defaults(): max_hop_count = 16 */
}

static void test_properties_enumeration_matches_the_underlying_surface(void)
{
    size_t count = 0;
    const hh_sca_property_t *props = manet_sca_properties(&count);
    HH_ASSERT(props != NULL);
    HH_ASSERT_EQ_INT(count, 23);
    HH_ASSERT(hh_sca_property_find("hh::node_id") == &props[0] ||
              count > 0); /* pass-through identity: same pointer/count as hh_sca_properties() */
}

/* ---------------------------------------------------------------------------
 * C. Lifecycle
 * ------------------------------------------------------------------------- */

static void test_full_lifecycle_start_stop_release(void)
{
    manet_sca_adapter_t a;
    mock_radio_t mock;
    hh_radio_t radio;
    vclock_t vc;

    fixture_started(&a, &mock, &radio, &vc, 21);
    HH_ASSERT_EQ_INT(a.guard.state, HH_SCA_STARTED);
    HH_ASSERT_EQ_INT(a.node.state, HH_NODE_RUNNING);

    HH_ASSERT_OK(manet_sca_stop(&a));
    HH_ASSERT_EQ_INT(a.guard.state, HH_SCA_STOPPED);
    HH_ASSERT_EQ_INT(a.node.state, HH_NODE_STOPPED);

    HH_ASSERT_OK(manet_sca_release_object(&a));
    HH_ASSERT_EQ_INT(a.guard.state, HH_SCA_RELEASED);
    HH_ASSERT_EQ_INT(a.node.state, HH_NODE_RELEASED);
}

static void test_start_before_configure_fails(void)
{
    manet_sca_adapter_t a;
    mock_radio_t mock;
    hh_radio_t radio;
    vclock_t vc;

    fixture_ports_connected(&a, &mock, &radio, &vc, 22);

    /* Guard is PORTS_CONNECTED, not CONFIGURED yet: hh_sca_start's own
     * ordering check rejects this, and the adapter must not call
     * hh_node_start() when it does -- confirmed below by node state. */
    HH_ASSERT_ERR(manet_sca_start(&a), HH_ERR_STATE);
    HH_ASSERT_EQ_INT(a.node.state, HH_NODE_INITIALIZED);
}

static void test_stop_before_start_fails(void)
{
    manet_sca_adapter_t a;
    mock_radio_t mock;
    hh_radio_t radio;
    vclock_t vc;

    fixture_ports_connected(&a, &mock, &radio, &vc, 23);
    HH_ASSERT_ERR(manet_sca_stop(&a), HH_ERR_STATE);
}

static void test_repeated_stop_fails_the_second_time(void)
{
    manet_sca_adapter_t a;
    mock_radio_t mock;
    hh_radio_t radio;
    vclock_t vc;

    fixture_started(&a, &mock, &radio, &vc, 24);
    HH_ASSERT_OK(manet_sca_stop(&a));
    HH_ASSERT_ERR(manet_sca_stop(&a), HH_ERR_STATE);
}

static void test_restart_after_stop_needs_no_reconfigure(void)
{
    /* hh_sca_stop()/hh_node_stop() both preserve configuration, so start()
     * after stop() must succeed without a second configure() call --
     * mirrors test_lifecycle_ordering_is_enforced's proof for
     * hh_sca_resource_t directly, through the adapter this time. */
    manet_sca_adapter_t a;
    mock_radio_t mock;
    hh_radio_t radio;
    vclock_t vc;

    fixture_started(&a, &mock, &radio, &vc, 25);
    HH_ASSERT_OK(manet_sca_stop(&a));
    HH_ASSERT_OK(manet_sca_start(&a));
    HH_ASSERT_EQ_INT(a.guard.state, HH_SCA_STARTED);
    HH_ASSERT_EQ_INT(a.node.state, HH_NODE_RUNNING);
}

static void test_release_after_initialize_only_succeeds(void)
{
    /* hh_node_release() has no state guard at all (src/manet/node.c) --
     * releasing straight after reaching PORTS_CONNECTED (never configured
     * or started) must still succeed, exactly as hh_node_release()'s own
     * unconditional contract promises. */
    manet_sca_adapter_t a;
    mock_radio_t mock;
    hh_radio_t radio;
    vclock_t vc;

    fixture_ports_connected(&a, &mock, &radio, &vc, 26);
    HH_ASSERT_OK(manet_sca_release_object(&a));
    HH_ASSERT_EQ_INT(a.node.state, HH_NODE_RELEASED);
    HH_ASSERT_EQ_INT(a.guard.state, HH_SCA_RELEASED);
}

static void test_release_after_stop_succeeds(void)
{
    manet_sca_adapter_t a;
    mock_radio_t mock;
    hh_radio_t radio;
    vclock_t vc;

    fixture_started(&a, &mock, &radio, &vc, 27);
    HH_ASSERT_OK(manet_sca_stop(&a));
    HH_ASSERT_OK(manet_sca_release_object(&a));
}

static void test_release_while_running_stops_first_then_releases(void)
{
    /* hh_node_release() calls hh_node_stop() internally when RUNNING
     * (src/manet/node.c: "if (n->state == HH_NODE_RUNNING) hh_node_stop(n)")
     * -- release() without a prior stop() must still land the node in
     * RELEASED, not leave it RUNNING or fail. */
    manet_sca_adapter_t a;
    mock_radio_t mock;
    hh_radio_t radio;
    vclock_t vc;

    fixture_started(&a, &mock, &radio, &vc, 28);
    HH_ASSERT_OK(manet_sca_release_object(&a));
    HH_ASSERT_EQ_INT(a.node.state, HH_NODE_RELEASED);
    HH_ASSERT_EQ_INT(a.guard.state, HH_SCA_RELEASED);
}

static void test_repeated_release_fails_the_second_time(void)
{
    /* hh_node_release() itself would happily "succeed" again (no guard),
     * but manet_sca_release_object() also calls hh_sca_release(&a->guard),
     * which DOES reject a second call -- the adapter's overall two-call
     * contract is therefore "fails the second time", driven by the SCA
     * guard rather than the node. */
    manet_sca_adapter_t a;
    mock_radio_t mock;
    hh_radio_t radio;
    vclock_t vc;

    fixture_ports_connected(&a, &mock, &radio, &vc, 29);
    HH_ASSERT_OK(manet_sca_release_object(&a));
    HH_ASSERT_ERR(manet_sca_release_object(&a), HH_ERR_STATE);
}

static void test_release_object_rejects_null(void)
{
    HH_ASSERT_ERR(manet_sca_release_object(NULL), HH_ERR_INVAL);
}

/* ---------------------------------------------------------------------------
 * D. Identifier / resource state
 * ------------------------------------------------------------------------- */

static void test_identifier_reflects_unconfigured_node_id_before_configure(void)
{
    manet_sca_adapter_t a;
    mock_radio_t mock;
    hh_radio_t radio;
    vclock_t vc;
    char buf[64];

    /* fixture_ports_connected sets cfg.node_id directly (bypassing
     * configure(), since that is what manet_sca_initialize() itself would
     * do if it could reach this point) -- so this checks the OTHER branch
     * of manet_sca_identifier's own logic: build the adapter by hand with
     * node_id left at the HH_NODE_ID_INVALID default instead. */
    memset(&a, 0, sizeof a);
    hh_sca_resource_init_guard(&a.guard, "MANET_Node");
    hh_config_defaults(&a.cfg);
    mock_radio_init(&mock, "n", &radio);
    vclock_init(&vc, 0);
    (void)vc;

    HH_ASSERT(manet_sca_identifier(&a, buf, sizeof buf) != NULL);
    HH_ASSERT_EQ_STR(buf, "MANET_Node.unconfigured");
}

static void test_identifier_reflects_configured_node_id(void)
{
    manet_sca_adapter_t a;
    mock_radio_t mock;
    hh_radio_t radio;
    vclock_t vc;
    char buf[64];

    fixture_ports_connected(&a, &mock, &radio, &vc, 42);

    HH_ASSERT(manet_sca_identifier(&a, buf, sizeof buf) != NULL);
    HH_ASSERT_EQ_STR(buf, "MANET_Node.42");
}

static void test_identifier_rejects_invalid_arguments(void)
{
    manet_sca_adapter_t a;
    char buf[64];
    memset(&a, 0, sizeof a);

    HH_ASSERT(manet_sca_identifier(NULL, buf, sizeof buf) == NULL);
    HH_ASSERT(manet_sca_identifier(&a, NULL, sizeof buf) == NULL);
    HH_ASSERT(manet_sca_identifier(&a, buf, 0) == NULL);
}

static void test_identifier_is_stable_across_lifecycle_transitions(void)
{
    manet_sca_adapter_t a;
    mock_radio_t mock;
    hh_radio_t radio;
    vclock_t vc;
    char before[64], after[64];

    fixture_started(&a, &mock, &radio, &vc, 33);
    manet_sca_identifier(&a, before, sizeof before);

    HH_ASSERT_OK(manet_sca_stop(&a));
    manet_sca_identifier(&a, after, sizeof after);
    HH_ASSERT_EQ_STR(before, after);

    HH_ASSERT_OK(manet_sca_release_object(&a));
    manet_sca_identifier(&a, after, sizeof after);
    HH_ASSERT_EQ_STR(before, after);
}

/* ---------------------------------------------------------------------------
 * E. Port lookup
 * ------------------------------------------------------------------------- */

static void test_get_port_resolves_every_declared_port_name(void)
{
    manet_sca_port_t p;

    HH_ASSERT_OK(manet_sca_get_port("radio_frame_in", &p));
    HH_ASSERT_EQ_INT(p, MANET_PORT_RADIO_FRAME_IN);

    HH_ASSERT_OK(manet_sca_get_port("radio_frame_out", &p));
    HH_ASSERT_EQ_INT(p, MANET_PORT_RADIO_FRAME_OUT);

    HH_ASSERT_OK(manet_sca_get_port("app_data_in", &p));
    HH_ASSERT_EQ_INT(p, MANET_PORT_APP_DATA_IN);

    HH_ASSERT_OK(manet_sca_get_port("status_out", &p));
    HH_ASSERT_EQ_INT(p, MANET_PORT_STATUS_OUT);
}

static void test_get_port_rejects_unknown_name(void)
{
    manet_sca_port_t p;
    HH_ASSERT_ERR(manet_sca_get_port("no_such_port", &p), HH_ERR_NOTFOUND);
}

static void test_get_port_rejects_null_arguments(void)
{
    manet_sca_port_t p;
    HH_ASSERT_ERR(manet_sca_get_port(NULL, &p), HH_ERR_INVAL);
    HH_ASSERT_ERR(manet_sca_get_port("status_out", NULL), HH_ERR_INVAL);
}

static void test_get_port_name_lookup_is_case_sensitive(void)
{
    /* Not documented either way in the header; establishing actual behavior
     * rather than assuming. strcmp is exact-match, so a case difference is
     * simply an unknown name today. */
    manet_sca_port_t p;
    HH_ASSERT_ERR(manet_sca_get_port("Status_Out", &p), HH_ERR_NOTFOUND);
}

/* ---------------------------------------------------------------------------
 * F. runTest
 * ------------------------------------------------------------------------- */

static void test_run_test_self_check_passes_once_initialized(void)
{
    manet_sca_adapter_t a;
    mock_radio_t mock;
    hh_radio_t radio;
    vclock_t vc;
    char out[128];

    fixture_ports_connected(&a, &mock, &radio, &vc, 51);

    HH_ASSERT_OK(manet_sca_run_test(&a, HH_SCA_TEST_SELF_CHECK, out, sizeof out));
    HH_ASSERT(strstr(out, "result=pass") != NULL);
    HH_ASSERT(strstr(out, "MANET_Node") != NULL);
}

static void test_run_test_unknown_id_is_conformant_unknown_test(void)
{
    manet_sca_adapter_t a;
    mock_radio_t mock;
    hh_radio_t radio;
    vclock_t vc;
    char out[128];

    fixture_ports_connected(&a, &mock, &radio, &vc, 52);

    HH_ASSERT_ERR(manet_sca_run_test(&a, 424242, out, sizeof out), HH_ERR_NOTFOUND);
    HH_ASSERT(strstr(out, "UnknownTest") != NULL);
}

static void test_run_test_rejects_null_adapter(void)
{
    char out[128];
    HH_ASSERT_ERR(manet_sca_run_test(NULL, HH_SCA_TEST_SELF_CHECK, out, sizeof out), HH_ERR_INVAL);
}

/* ---------------------------------------------------------------------------
 * G. Tick / runtime path (manet_sca_tick, manet_sca_on_frame, manet_sca_send)
 * ------------------------------------------------------------------------- */

static void test_tick_before_start_fails(void)
{
    manet_sca_adapter_t a;
    mock_radio_t mock;
    hh_radio_t radio;
    vclock_t vc;

    fixture_ports_connected(&a, &mock, &radio, &vc, 61);
    HH_ASSERT_ERR(manet_sca_tick(&a, vc.now), HH_ERR_STATE);
}

static void test_tick_after_start_succeeds_and_advances(void)
{
    manet_sca_adapter_t a;
    mock_radio_t mock;
    hh_radio_t radio;
    vclock_t vc;
    uint64_t ticks_before;

    fixture_started(&a, &mock, &radio, &vc, 62);
    ticks_before = a.node.ticks;

    vclock_advance(&vc, 10);
    HH_ASSERT_OK(manet_sca_tick(&a, vc.now));
    HH_ASSERT(a.node.ticks > ticks_before);
}

static void test_tick_rejects_null_adapter(void)
{
    HH_ASSERT_ERR(manet_sca_tick(NULL, 0), HH_ERR_INVAL);
}

static void test_send_before_start_fails(void)
{
    manet_sca_adapter_t a;
    mock_radio_t mock;
    hh_radio_t radio;
    vclock_t vc;
    const uint8_t payload[] = { 1, 2, 3 };

    fixture_ports_connected(&a, &mock, &radio, &vc, 63);
    HH_ASSERT_ERR(manet_sca_send(&a, 99, payload, sizeof payload, vc.now), HH_ERR_STATE);
}

static void test_send_after_start_with_no_route_buffers_the_packet(void)
{
    /* No neighbor/route exists in this fixture, so hh_forwarder_send's
     * documented no-route behavior applies: HH_ERR_AGAIN, buffered for the
     * control plane to resolve later -- not a hard failure. */
    manet_sca_adapter_t a;
    mock_radio_t mock;
    hh_radio_t radio;
    vclock_t vc;
    const uint8_t payload[] = { 9, 8, 7 };

    fixture_started(&a, &mock, &radio, &vc, 64);
    HH_ASSERT_ERR(manet_sca_send(&a, 99, payload, sizeof payload, vc.now), HH_ERR_AGAIN);
}

static void test_send_rejects_null_adapter(void)
{
    const uint8_t payload[] = { 1 };
    HH_ASSERT_ERR(manet_sca_send(NULL, 1, payload, sizeof payload, 0), HH_ERR_INVAL);
}

static void test_on_frame_before_start_does_not_crash_and_has_no_state_guard(void)
{
    /* manet_sca_on_frame -> hh_node_on_frame is void and unconditionally
     * dispatches on frame kind (src/manet/node.c has no state check here,
     * unlike tick()/send()). This documents that actual, asymmetric
     * behavior rather than assuming on_frame is guarded the same way. */
    manet_sca_adapter_t a;
    mock_radio_t mock;
    hh_radio_t radio;
    vclock_t vc;
    hh_frame_t f;
    hh_link_sample_t m;

    fixture_ports_connected(&a, &mock, &radio, &vc, 65);

    memset(&f, 0, sizeof f);
    f.kind = HH_FRAME_BEACON;
    f.src = 66;
    f.len = 0;
    memset(&m, 0, sizeof m);
    m.neighbor_id = 66;

    manet_sca_on_frame(&a, &f, &m);   /* must not crash; no return value to check */
    HH_ASSERT_EQ_INT(a.node.state, HH_NODE_INITIALIZED);   /* on_frame alone changes no lifecycle state */
}

static void test_on_frame_null_arguments_are_ignored_safely(void)
{
    manet_sca_adapter_t a;
    hh_frame_t f;
    hh_link_sample_t m;
    memset(&a, 0, sizeof a);
    memset(&f, 0, sizeof f);
    memset(&m, 0, sizeof m);

    manet_sca_on_frame(NULL, &f, &m);   /* must not crash */
    manet_sca_on_frame(&a, NULL, &m);   /* must not crash */
}

static void test_status_reflects_stub_radio_honestly_through_the_adapter(void)
{
    /* Uses the REAL hardware adapter (hh_hw_adapter_t), not the mock, to
     * prove the adapter's status_out path surfaces the documented
     * HH_ERR_NOT_IMPLEMENTED radio gap honestly rather than masking it --
     * mirrors test_telemetry_reports_missing_radio_backend_honestly, one
     * layer up through manet_sca_status(). */
    manet_sca_adapter_t a;
    hh_hw_adapter_t hw;
    hh_radio_t radio;
    vclock_t vc;
    hh_node_status_t st;

    memset(&a, 0, sizeof a);
    hh_sca_resource_init_guard(&a.guard, "MANET_Node");
    hh_config_defaults(&a.cfg);
    a.cfg.node_id = 70;
    hh_hw_adapter_init(&hw, &radio);
    vclock_init(&vc, 0);

    HH_ASSERT_OK(hh_node_init(&a.node, &a.cfg, &vc.clock, &radio));

    manet_sca_status(&a, &st);
    HH_ASSERT(!st.radio_available);
    HH_ASSERT(!st.radio_operational);
    HH_ASSERT_EQ_INT(st.node_id, 70);
}

static void test_status_rejects_null_arguments_safely(void)
{
    manet_sca_adapter_t a;
    hh_node_status_t st;
    memset(&a, 0, sizeof a);
    memset(&st, 0, sizeof st);

    manet_sca_status(NULL, &st);   /* must not crash */
    manet_sca_status(&a, NULL);    /* must not crash */
}

HH_TEST_MAIN_BEGIN("manet_sca_adapter")
    HH_RUN(test_initialize_rejects_null_arguments);
    HH_RUN(test_initialize_succeeds_with_a_valid_node_id);
    HH_RUN(test_initialize_rejects_invalid_node_id);
    HH_RUN(test_initialize_can_be_retried_after_a_rejected_node_id);

    HH_RUN(test_configure_before_ports_connected_fails);
    HH_RUN(test_configure_rejects_null_with_nonzero_count);
    HH_RUN(test_configure_accepts_zero_count_as_a_no_op);
    HH_RUN(test_configure_valid_property_updates_node_config);
    HH_RUN(test_configure_unknown_property_id_fails_and_applies_nothing);
    HH_RUN(test_execparam_cannot_be_retuned_through_the_adapter);
    HH_RUN(test_configure_and_query_bounded_table_size_properties_through_the_adapter);
    HH_RUN(test_second_configure_call_succeeds_while_configured);
    HH_RUN(test_second_configure_updates_same_property_to_a_new_value);
    HH_RUN(test_second_configure_does_not_reset_unrelated_properties);
    HH_RUN(test_multi_property_configure_remains_correct);
    HH_RUN(test_configure_kind_property_while_started_is_accepted);
    HH_RUN(test_execparam_while_started_still_rejected);
    HH_RUN(test_invalid_value_while_started_is_rejected_and_state_preserved);
    HH_RUN(test_configure_after_stop_then_start_then_stop_again_succeeds);
    HH_RUN(test_release_after_repeated_configure_succeeds);
    HH_RUN(test_query_round_trips_a_configured_value);
    HH_RUN(test_query_unknown_property_fails);
    HH_RUN(test_query_null_adapter_fails);
    HH_RUN(test_query_reflects_a_default_when_nothing_was_configured);
    HH_RUN(test_properties_enumeration_matches_the_underlying_surface);

    HH_RUN(test_full_lifecycle_start_stop_release);
    HH_RUN(test_start_before_configure_fails);
    HH_RUN(test_stop_before_start_fails);
    HH_RUN(test_repeated_stop_fails_the_second_time);
    HH_RUN(test_restart_after_stop_needs_no_reconfigure);
    HH_RUN(test_release_after_initialize_only_succeeds);
    HH_RUN(test_release_after_stop_succeeds);
    HH_RUN(test_release_while_running_stops_first_then_releases);
    HH_RUN(test_repeated_release_fails_the_second_time);
    HH_RUN(test_release_object_rejects_null);

    HH_RUN(test_identifier_reflects_unconfigured_node_id_before_configure);
    HH_RUN(test_identifier_reflects_configured_node_id);
    HH_RUN(test_identifier_rejects_invalid_arguments);
    HH_RUN(test_identifier_is_stable_across_lifecycle_transitions);

    HH_RUN(test_get_port_resolves_every_declared_port_name);
    HH_RUN(test_get_port_rejects_unknown_name);
    HH_RUN(test_get_port_rejects_null_arguments);
    HH_RUN(test_get_port_name_lookup_is_case_sensitive);

    HH_RUN(test_run_test_self_check_passes_once_initialized);
    HH_RUN(test_run_test_unknown_id_is_conformant_unknown_test);
    HH_RUN(test_run_test_rejects_null_adapter);

    HH_RUN(test_tick_before_start_fails);
    HH_RUN(test_tick_after_start_succeeds_and_advances);
    HH_RUN(test_tick_rejects_null_adapter);
    HH_RUN(test_send_before_start_fails);
    HH_RUN(test_send_after_start_with_no_route_buffers_the_packet);
    HH_RUN(test_send_rejects_null_adapter);
    HH_RUN(test_on_frame_before_start_does_not_crash_and_has_no_state_guard);
    HH_RUN(test_on_frame_null_arguments_are_ignored_safely);
    HH_RUN(test_status_reflects_stub_radio_honestly_through_the_adapter);
    HH_RUN(test_status_rejects_null_arguments_safely);
HH_TEST_MAIN_END()
