/*
 * Telemetry (HTI-01/02/15) and SCA 2.2.2 compatibility tests.
 *
 * Telemetry must report honestly -- especially that no radio backend exists --
 * and must not sit on the fast path. The SCA layer must ENFORCE the 
 * lifecycle ordering rather than merely documenting it, and must classify
 * components exactly as does.
 */
#include "hhsdr/manet/telemetry.h"
#include "hhsdr/radio/hw_adapter.h"
#include "hhsdr/sca/resource.h"
#include "hh_test.h"
#include "netsim.h"
#include <string.h>

/* ---------------- Telemetry ---------------- */

static void test_status_reflects_converged_network(void)
{
    netsim_t s;
    hh_node_status_t st;
    char buf[2048];

    netsim_init(&s, 900);
    netsim_add_node(&s, 1);
    netsim_add_node(&s, 2);
    netsim_add_node(&s, 3);
    netsim_link_up(&s, 1, 2, -55.0f);
    netsim_link_up(&s, 2, 3, -55.0f);
    netsim_start_all(&s);
    netsim_run(&s, 3000, 10);

    hh_telemetry_node_status(&netsim_node(&s, 1)->node, &st);
    HH_ASSERT_EQ_INT(st.node_id, 1);
    HH_ASSERT_EQ_STR(st.state, "running");
    HH_ASSERT_EQ_INT(st.neighbor_count, 1);
    HH_ASSERT(st.route_count >= 2);          /* node 2 and node 3 */
    HH_ASSERT(st.beacons_sent > 0);
    HH_ASSERT(st.beacons_rx_accepted > 0);
    HH_ASSERT(!st.partitioned);
    HH_ASSERT_EQ_INT(st.events_dropped, 0);

    /* The formatted record is parseable key=value. */
    HH_ASSERT(hh_telemetry_format_status(&st, buf, sizeof buf) > 0);
    HH_ASSERT(strstr(buf, "node=1") != NULL);
    HH_ASSERT(strstr(buf, "state=running") != NULL);
    HH_ASSERT(strstr(buf, "neighbors=1") != NULL);
}

static void test_neighbor_and_route_reports(void)
{
    netsim_t s;
    char buf[4096];

    netsim_init(&s, 901);
    netsim_add_node(&s, 1);
    netsim_add_node(&s, 2);
    netsim_link_up(&s, 1, 2, -55.0f);
    netsim_start_all(&s);
    netsim_run(&s, 2000, 10);

    HH_ASSERT(hh_telemetry_format_neighbors(&netsim_node(&s, 1)->node, buf, sizeof buf) > 0);
    HH_ASSERT(strstr(buf, "neighbor=2") != NULL);
    HH_ASSERT(strstr(buf, "state=Healthy") != NULL);

    HH_ASSERT(hh_telemetry_format_routes(&netsim_node(&s, 1)->node, buf, sizeof buf) > 0);
    HH_ASSERT(strstr(buf, "dst=2") != NULL);
    HH_ASSERT(strstr(buf, "snapshot_version=") != NULL);
}

static void test_telemetry_reports_missing_radio_backend_honestly(void)
{
    hh_config_t cfg;
    vclock_t vc;
    hh_hw_adapter_t hw;
    hh_radio_t radio;
    hh_node_t node;
    hh_node_status_t st;

    hh_config_defaults(&cfg);
    cfg.node_id = 1;
    cfg.log_level = HH_LOG_ERROR;
    vclock_init(&vc, 1000);
    hh_hw_adapter_init(&hw, &radio);
    HH_ASSERT_OK(hh_node_init(&node, &cfg, &vc.clock, &radio));

    /* With the stub adapter, telemetry must say the hardware is unavailable
     * rather than presenting plausible-looking counters. */
    hh_telemetry_node_status(&node, &st);
    HH_ASSERT(!st.radio_available);
    HH_ASSERT(!st.radio_operational);
}

static void test_telemetry_reads_snapshot_without_disturbing_it(void)
{
    netsim_t s;
    const hh_route_snapshot_t *before, *after;
    char buf[4096];

    netsim_init(&s, 902);
    netsim_add_node(&s, 1);
    netsim_add_node(&s, 2);
    netsim_link_up(&s, 1, 2, -55.0f);
    netsim_start_all(&s);
    netsim_run(&s, 2000, 10);

    /* Telemetry reads the same lock-free snapshot the forwarder uses and must
     * not cause a republish, so it adds no contention to the fast path. */
    before = hh_routing_snapshot(&netsim_node(&s, 1)->node.routing);
    hh_telemetry_format_routes(&netsim_node(&s, 1)->node, buf, sizeof buf);
    after = hh_routing_snapshot(&netsim_node(&s, 1)->node.routing);
    HH_ASSERT(before == after);
    HH_ASSERT_EQ_INT(before->version, after->version);
}

/* ---------------- SCA ---------------- */

static void test_component_classification_matches_doc1(void)
{
    const hh_sca_component_t *c;
    size_t count;

    hh_sca_components(&count);
    HH_ASSERT_EQ_INT(count, 10);   /* the ten components of */

    /* All seven control-plane components are Resources. */
    const char *resources[] = { "DiscoveryManager", "NeighborManager",
        "LinkHealthMonitor", "FailureDetector", "TopologyManager",
        "RoutingEngine", "SelfHealingManager" };
    for (size_t i = 0; i < 7; i++) {
        c = hh_sca_component_find(resources[i]);
        HH_ASSERT_MSG(c != NULL, "missing component %s", resources[i]);
        HH_ASSERT_EQ_INT(c->sca_class, HH_SCA_RESOURCE);
    }

    /* The radio is a Device, owned by the Device Manager, not the Application. */
    c = hh_sca_component_find("RadioSdrInterface");
    HH_ASSERT_EQ_INT(c->sca_class, HH_SCA_DEVICE);

    /* The data plane is explicitly outside the Resource graph. */
    c = hh_sca_component_find("PacketForwarder");
    HH_ASSERT_EQ_INT(c->sca_class, HH_SCA_OUTSIDE_GRAPH);
    HH_ASSERT_EQ_INT(c->port_count, 0);

    c = hh_sca_component_find("ManagementTelemetry");
    HH_ASSERT_EQ_INT(c->sca_class, HH_SCA_SERVICE);
}

static void test_routing_engine_is_the_only_assembly_controller(void)
{
    const hh_sca_component_t *all;
    size_t count, controllers = 0;

    all = hh_sca_components(&count);
    for (size_t i = 0; i < count; i++)
        if (all[i].is_assembly_controller) {
            controllers++;
            HH_ASSERT_EQ_STR(all[i].name, "RoutingEngine");
        }
    /* exactly one assembly controller, and it is the Routing Engine. */
    HH_ASSERT_EQ_INT(controllers, 1);
}

static void test_every_port_maps_to_a_specified_interface(void)
{
    const hh_sca_component_t *all;
    size_t count;

    all = hh_sca_components(&count);
    for (size_t i = 0; i < count; i++) {
        for (size_t p = 0; p < all[i].port_count; p++) {
            const hh_sca_port_t *port = &all[i].ports[p];
            /* No port may exist without a numbered interface behind it: that is
             * what keeps the port list traceable rather than invented. */
            HH_ASSERT_MSG(port->hti_id && strncmp(port->hti_id, "HTI-", 4) == 0,
                          "%s port %s has no HTI id", all[i].name, port->name);
        }
    }
}

static void test_lifecycle_ordering_is_enforced(void)
{
    hh_sca_resource_t r;
    hh_config_t cfg;

    hh_config_defaults(&cfg);
    cfg.node_id = 1;
    hh_sca_resource_init_guard(&r, "RoutingEngine");

    /* Configure before ports are connected must fail. */
    HH_ASSERT_ERR(hh_sca_configure(&r, &cfg, "hh::routing::max_hop_count", "8"),
                  HH_ERR_STATE);
    /* Start before configure must fail (step 4 -> 5). */
    HH_ASSERT_ERR(hh_sca_start(&r), HH_ERR_STATE);

    HH_ASSERT_OK(hh_sca_initialize(&r));
    HH_ASSERT_ERR(hh_sca_start(&r), HH_ERR_STATE);

    HH_ASSERT_OK(hh_sca_connect_ports(&r));
    HH_ASSERT_OK(hh_sca_configure(&r, &cfg, "hh::routing::max_hop_count", "8"));
    HH_ASSERT_EQ_INT(cfg.max_hop_count, 8);
    HH_ASSERT_OK(hh_sca_start(&r));
    HH_ASSERT_EQ_INT(r.state, HH_SCA_STARTED);

    /* stop() halts without discarding configuration, so restart needs no
     * reconfigure. */
    HH_ASSERT_OK(hh_sca_stop(&r));
    HH_ASSERT_EQ_INT(cfg.max_hop_count, 8);
    HH_ASSERT_OK(hh_sca_start(&r));

    HH_ASSERT_OK(hh_sca_stop(&r));
    HH_ASSERT_OK(hh_sca_release(&r));
    HH_ASSERT_ERR(hh_sca_release(&r), HH_ERR_STATE);
}

static void test_execparam_cannot_be_retuned_at_runtime(void)
{
    hh_sca_resource_t r;
    hh_config_t cfg;

    hh_config_defaults(&cfg);
    cfg.node_id = 1;
    hh_sca_resource_init_guard(&r, "DiscoveryManager");
    hh_sca_initialize(&r);
    hh_sca_connect_ports(&r);

    /* An execparam is settable at launch... */
    HH_ASSERT_OK(hh_sca_configure(&r, &cfg, "hh::node_id", "42"));
    HH_ASSERT_EQ_INT(cfg.node_id, 42);

    /* ...but not retunable once running configuration has been applied. */
    HH_ASSERT_ERR(hh_sca_configure(&r, &cfg, "hh::node_id", "43"), HH_ERR_STATE);
    HH_ASSERT_EQ_INT(cfg.node_id, 42);

    /* A configure-kind property remains tunable. */
    HH_ASSERT_OK(hh_sca_configure(&r, &cfg, "hh::discovery::beacon_interval_ms", "700"));
    HH_ASSERT_EQ_INT(cfg.beacon_interval_ms, 700);
}

static void test_property_surface_binds_to_real_configuration(void)
{
    const hh_sca_property_t *props;
    size_t count;
    hh_config_t cfg;

    hh_config_defaults(&cfg);
    cfg.node_id = 1;
    props = hh_sca_properties(&count);
    HH_ASSERT(count > 0);

    /* Every declared property must bind to a key the real configuration
     * accepts, otherwise the PRF surface would be decorative. */
    for (size_t i = 0; i < count; i++) {
        hh_status_t st = hh_config_set(&cfg, props[i].config_key,
            props[i].type == HH_PROP_STRING
                ? (strcmp(props[i].config_key, "log_level") ? "hw" : "info")
                : (props[i].type == HH_PROP_F32 ? "0.5" : "5"));
        HH_ASSERT_MSG(st != HH_ERR_NOTFOUND,
                      "property %s binds to unknown config key %s",
                      props[i].id, props[i].config_key);
    }
}

static void test_bounded_table_size_properties_are_configure_not_allocation(void)
{
    /*
     * hh::neighbor::max_entries and hh::routing::max_routes were previously
     * classified HH_PROP_ALLOCATION even though they are ordinary runtime
     * bounds (like hh::routing::max_hop_count), not Resource capacity-
     * allocation requests -- CF::Resource has no allocateCapacity mechanism
     * at all in real REDHAWK 2.2.10 (only CF::Device does, via
     * Device_impl::allocateCapacity()), and this repository's own
     * hh_sca_configure()/hh_sca_query() never branch on HH_PROP_ALLOCATION
     * vs HH_PROP_CONFIGURE -- the only kind ever checked is
     * HH_PROP_EXECPARAM. This asserts the corrected classification. */
    const hh_sca_property_t *p;

    p = hh_sca_property_find("hh::neighbor::max_entries");
    HH_ASSERT(p != NULL);
    HH_ASSERT_EQ_INT(p->kind, HH_PROP_CONFIGURE);
    HH_ASSERT_EQ_STR(p->config_key, "max_neighbors");

    p = hh_sca_property_find("hh::routing::max_routes");
    HH_ASSERT(p != NULL);
    HH_ASSERT_EQ_INT(p->kind, HH_PROP_CONFIGURE);
    HH_ASSERT_EQ_STR(p->config_key, "max_routes");

    /* No property anywhere in the surface is HH_PROP_ALLOCATION anymore --
     * this component is a Resource, and a Resource has no allocation-kind
     * property in real REDHAWK usage (Step 6 investigation). */
    {
        const hh_sca_property_t *all;
        size_t count;
        all = hh_sca_properties(&count);
        for (size_t i = 0; i < count; i++)
            HH_ASSERT_MSG(all[i].kind != HH_PROP_ALLOCATION,
                          "property %s is still classified HH_PROP_ALLOCATION",
                          all[i].id);
    }
}

static void test_bounded_table_size_properties_configure_and_query_correctly(void)
{
    /* Reclassifying kind must not change configure()/query() behavior: both
     * properties must still round-trip through the same mechanism every
     * other configure-kind property uses. */
    hh_sca_resource_t r;
    hh_config_t cfg;
    char out[64];

    hh_config_defaults(&cfg);
    cfg.node_id = 1;
    hh_sca_resource_init_guard(&r, "NeighborManager");
    hh_sca_initialize(&r);
    hh_sca_connect_ports(&r);

    HH_ASSERT_OK(hh_sca_configure(&r, &cfg, "hh::neighbor::max_entries", "10"));
    HH_ASSERT_EQ_INT(cfg.max_neighbors, 10);
    HH_ASSERT_OK(hh_sca_query(&r, &cfg, "hh::neighbor::max_entries", out, sizeof out));
    HH_ASSERT_EQ_STR(out, "10");

    HH_ASSERT_OK(hh_sca_configure(&r, &cfg, "hh::routing::max_routes", "20"));
    HH_ASSERT_EQ_INT(cfg.max_routes, 20);
    HH_ASSERT_OK(hh_sca_query(&r, &cfg, "hh::routing::max_routes", out, sizeof out));
    HH_ASSERT_EQ_STR(out, "20");

    /* Both are configure-kind (not execparam), so -- unlike hh::node_id --
     * they remain retunable after stop(), exactly like any other
     * HH_PROP_CONFIGURE property. As of Step 8, hh_sca_configure() also
     * permits HH_PROP_CONFIGURE properties while HH_SCA_STARTED
     * (src/sca/resource.c): the Step 8 investigation traced every
     * configure-kind property (including these two) to a live pointer read
     * with no structural/memory dependency on node state, matching real
     * REDHAWK 2.2.10's own CF::PropertySet::configure(), which has no
     * started/stopped restriction at all. Only HH_PROP_EXECPARAM properties
     * remain blocked outside PORTS_CONNECTED, in every state including
     * STARTED (see test_execparam_cannot_be_retuned_at_runtime above). */
    HH_ASSERT_OK(hh_sca_start(&r));
    HH_ASSERT_OK(hh_sca_configure(&r, &cfg, "hh::neighbor::max_entries", "15"));
    HH_ASSERT_EQ_INT(cfg.max_neighbors, 15);
    HH_ASSERT_ERR(hh_sca_configure(&r, &cfg, "hh::node_id", "99"), HH_ERR_STATE);

    HH_ASSERT_OK(hh_sca_stop(&r));
    HH_ASSERT_OK(hh_sca_configure(&r, &cfg, "hh::neighbor::max_entries", "18"));
    HH_ASSERT_EQ_INT(cfg.max_neighbors, 18);

    /* hh_config_validate()'s existing zero-rejection is unaffected by the
     * classification change: it always checked the raw field, never the
     * PRF kind. */
    HH_ASSERT_OK(hh_config_validate(&cfg));   /* still valid: node_id, etc. all set */
    cfg.max_neighbors = 0;
    HH_ASSERT_ERR(hh_config_validate(&cfg), HH_ERR_INVAL);
}

static void test_query_round_trips_configured_values(void)
{
    hh_sca_resource_t r;
    hh_config_t cfg;
    char out[64];

    hh_config_defaults(&cfg);
    cfg.node_id = 1;
    hh_sca_resource_init_guard(&r, "LinkHealthMonitor");
    hh_sca_initialize(&r);
    hh_sca_connect_ports(&r);

    HH_ASSERT_OK(hh_sca_configure(&r, &cfg, "hh::linkhealth::suspect_hold_ms", "1234"));
    HH_ASSERT_OK(hh_sca_query(&r, &cfg, "hh::linkhealth::suspect_hold_ms", out, sizeof out));
    HH_ASSERT_EQ_STR(out, "1234");

    HH_ASSERT_OK(hh_sca_query(&r, &cfg, "hh::mgmt::log_level", out, sizeof out));
    HH_ASSERT_EQ_STR(out, "info");

    HH_ASSERT_ERR(hh_sca_query(&r, &cfg, "hh::no::such::property", out, sizeof out),
                  HH_ERR_NOTFOUND);
}

static void test_run_test_reports_unknown_test_conformantly(void)
{
    hh_sca_resource_t r;
    char out[128];

    hh_sca_resource_init_guard(&r, "FailureDetector");
    hh_sca_initialize(&r);

    HH_ASSERT_OK(hh_sca_run_test(&r, HH_SCA_TEST_SELF_CHECK, out, sizeof out));
    HH_ASSERT(strstr(out, "result=pass") != NULL);

    /* UnknownTest for an unrecognised id is conformant. */
    HH_ASSERT_ERR(hh_sca_run_test(&r, 9999, out, sizeof out), HH_ERR_NOTFOUND);
    HH_ASSERT(strstr(out, "UnknownTest") != NULL);
}

HH_TEST_MAIN_BEGIN("telemetry_sca")
    HH_RUN(test_status_reflects_converged_network);
    HH_RUN(test_neighbor_and_route_reports);
    HH_RUN(test_telemetry_reports_missing_radio_backend_honestly);
    HH_RUN(test_telemetry_reads_snapshot_without_disturbing_it);
    HH_RUN(test_component_classification_matches_doc1);
    HH_RUN(test_routing_engine_is_the_only_assembly_controller);
    HH_RUN(test_every_port_maps_to_a_specified_interface);
    HH_RUN(test_lifecycle_ordering_is_enforced);
    HH_RUN(test_execparam_cannot_be_retuned_at_runtime);
    HH_RUN(test_property_surface_binds_to_real_configuration);
    HH_RUN(test_bounded_table_size_properties_are_configure_not_allocation);
    HH_RUN(test_bounded_table_size_properties_configure_and_query_correctly);
    HH_RUN(test_query_round_trips_configured_values);
    HH_RUN(test_run_test_reports_unknown_test_conformantly);
HH_TEST_MAIN_END()
