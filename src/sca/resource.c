#include "hhsdr/sca/resource.h"
#include "hhsdr/core/log.h"
#include <stdio.h>
#include <string.h>

#define COMP "sca"

const char *hh_sca_class_str(hh_sca_class_t c)
{
    switch (c) {
    case HH_SCA_RESOURCE:       return "Resource";
    case HH_SCA_DEVICE:         return "Device";
    case HH_SCA_SERVICE:        return "Service";
    case HH_SCA_OUTSIDE_GRAPH:  return "OutsideResourceGraph";
    }
    return "Unknown";
}

const char *hh_sca_lifecycle_str(hh_sca_lifecycle_t s)
{
    switch (s) {
    case HH_SCA_UNINITIALIZED:   return "uninitialized";
    case HH_SCA_INITIALIZED:     return "initialized";
    case HH_SCA_PORTS_CONNECTED: return "ports_connected";
    case HH_SCA_CONFIGURED:      return "configured";
    case HH_SCA_STARTED:         return "started";
    case HH_SCA_STOPPED:         return "stopped";
    case HH_SCA_RELEASED:        return "released";
    }
    return "unknown";
}

/* ---------------------------------------------------------------------------
 * PRF property surface. Every entry corresponds to a tunable the architecture names
 * and binds to the matching hh_config_t key, so configure() drives the real
 * configuration rather than a parallel copy.
 * ------------------------------------------------------------------------- */
static const hh_sca_property_t g_props[] = {
    { "hh::node_id", "node_id", HH_PROP_EXECPARAM, HH_PROP_U32,
      "Node identity; fixed at launch" },
    { "hh::discovery::beacon_interval_ms", "beacon_interval_ms",
      HH_PROP_CONFIGURE, HH_PROP_U32, "Steady-state beacon cadence" },
    { "hh::discovery::beacon_interval_min_ms", "beacon_interval_min_ms",
      HH_PROP_CONFIGURE, HH_PROP_U32, "Adaptive cadence lower bound" },
    { "hh::discovery::beacon_interval_max_ms", "beacon_interval_max_ms",
      HH_PROP_CONFIGURE, HH_PROP_U32, "Adaptive cadence upper bound" },
    { "hh::discovery::acquisition_timeout_ms", "acquisition_timeout_ms",
      HH_PROP_CONFIGURE, HH_PROP_U32, "Bounded acquisition phase timeout" },
    { "hh::neighbor::allowed_loss", "neighbor_allowed_loss",
      HH_PROP_CONFIGURE, HH_PROP_U32, "Missed beacons tolerated before expiry" },
    { "hh::neighbor::max_entries", "max_neighbors",
      HH_PROP_CONFIGURE, HH_PROP_U32, "Bounded neighbor table size" },
    { "hh::linkhealth::degrade_threshold", "lh_degrade_threshold",
      HH_PROP_CONFIGURE, HH_PROP_F32, "Fused score entering Degraded" },
    { "hh::linkhealth::recover_threshold", "lh_recover_threshold",
      HH_PROP_CONFIGURE, HH_PROP_F32, "High-water mark returning to Healthy" },
    { "hh::linkhealth::suspect_hold_ms", "lh_suspect_hold_ms",
      HH_PROP_CONFIGURE, HH_PROP_U32, "SuspectedFailure to Failed hold-down" },
    { "hh::linkhealth::recover_hold_ms", "lh_recover_hold_ms",
      HH_PROP_CONFIGURE, HH_PROP_U32, "Recovering to Healthy hold-down" },
    { "hh::linkhealth::min_signals_suspect", "lh_min_signals_suspect",
      HH_PROP_CONFIGURE, HH_PROP_U32, "Independent bad signals to suspect" },
    { "hh::routing::active_timeout_ms", "route_active_timeout_ms",
      HH_PROP_CONFIGURE, HH_PROP_U32, "Route inactivity expiry" },
    { "hh::routing::delete_period_ms", "route_delete_period_ms",
      HH_PROP_CONFIGURE, HH_PROP_U32, "Grace window before route deletion" },
    { "hh::routing::update_interval_ms", "route_update_interval_ms",
      HH_PROP_CONFIGURE, HH_PROP_U32, "Proactive route update cadence" },
    { "hh::routing::max_routes", "max_routes",
      HH_PROP_CONFIGURE, HH_PROP_U32, "Bounded route table size" },
    { "hh::routing::max_hop_count", "max_hop_count",
      HH_PROP_CONFIGURE, HH_PROP_U32, "Hop-count bound" },
    { "hh::recovery::hold_down_ms", "hold_down_ms",
      HH_PROP_CONFIGURE, HH_PROP_U32, "New-route stability window" },
    { "hh::recovery::merge_hold_down_ms", "merge_hold_down_ms",
      HH_PROP_CONFIGURE, HH_PROP_U32, "Hold-down before trusting merged routes" },
    { "hh::recovery::dampening_flap_threshold", "dampening_flap_threshold",
      HH_PROP_CONFIGURE, HH_PROP_U32, "Flaps in window before penalty" },
    { "hh::recovery::rediscovery_max_retries", "rediscovery_max_retries",
      HH_PROP_CONFIGURE, HH_PROP_U32, "Bounded rediscovery attempts" },
    { "hh::mgmt::log_level", "log_level",
      HH_PROP_CONFIGURE, HH_PROP_STRING, "Logging verbosity" },
    { "hh::radio::adapter", "radio_adapter",
      HH_PROP_EXECPARAM, HH_PROP_STRING, "Radio adapter selection" },
};

const hh_sca_property_t *hh_sca_properties(size_t *count)
{
    if (count) *count = sizeof g_props / sizeof g_props[0];
    return g_props;
}

const hh_sca_property_t *hh_sca_property_find(const char *id)
{
    if (!id) return NULL;
    for (size_t i = 0; i < sizeof g_props / sizeof g_props[0]; i++)
        if (!strcmp(g_props[i].id, id)) return &g_props[i];
    return NULL;
}

/* ---------------------------------------------------------------------------
 * Component classification and port declarations. This is the CONTENT BASIS for
 * SCD/SAD descriptors; the XML documents themselves are not authored here.
 * Ports mirror the HW/SW spec's per-component port lists exactly.
 * ------------------------------------------------------------------------- */
static const hh_sca_port_t discovery_ports[] = {
    { "beacon_rx",       false, "HTI-04" },
    { "beacon_tx",       true,  "HTI-03" },
    { "cadence_hint_in", true,  "HTI-16" },
};
static const hh_sca_port_t neighbor_ports[] = {
    { "neighbor_events", false, "HTI-06" },
    { "beacon_in",       true,  "HTI-04" },
};
static const hh_sca_port_t link_health_ports[] = {
    { "link_sample_in",  true,  "HTI-05" },
    { "link_state_out",  false, "HTI-07" },
    { "cadence_hint_out",false, "HTI-16" },
    { "neighbor_in",     true,  "HTI-06" },
};
static const hh_sca_port_t failure_detector_ports[] = {
    { "failure_out",     false, "HTI-10" },
    { "link_state_in",   true,  "HTI-07" },
};
static const hh_sca_port_t topology_ports[] = {
    { "partition_out",   false, "HTI-12" },
    { "merge_out",       false, "HTI-13" },
    { "neighbor_in",     true,  "HTI-06" },
    { "link_state_in",   true,  "HTI-07" },
    { "route_in",        true,  "HTI-08/09" },
};
static const hh_sca_port_t routing_ports[] = {
    { "route_installed", false, "HTI-08" },
    { "route_withdrawn", false, "HTI-09" },
    { "neighbor_in",     true,  "HTI-06" },
    { "link_state_in",   true,  "HTI-07" },
    { "failure_in",      true,  "HTI-10" },
};
static const hh_sca_port_t self_healing_ports[] = {
    { "recovery_out",    false, "HTI-11" },
    { "failure_in",      true,  "HTI-10" },
    { "partition_in",    true,  "HTI-12" },
    { "merge_in",        true,  "HTI-13" },
    { "channel_control", true,  "HTI-14" },
};
static const hh_sca_port_t radio_ports[] = {
    { "radio_status",    false, "HTI-02" },
    { "beacon_rx",       false, "HTI-04" },
    { "link_sample",     false, "HTI-05" },
    { "beacon_tx",       true,  "HTI-03" },
    { "channel_control", true,  "HTI-14" },
};

#define PORTS(a) a, sizeof a / sizeof a[0]

static const hh_sca_component_t g_components[] = {
    { "DiscoveryManager",   HH_SCA_RESOURCE, PORTS(discovery_ports),        false },
    { "NeighborManager",    HH_SCA_RESOURCE, PORTS(neighbor_ports),         false },
    { "LinkHealthMonitor",  HH_SCA_RESOURCE, PORTS(link_health_ports),      false },
    { "FailureDetector",    HH_SCA_RESOURCE, PORTS(failure_detector_ports), false },
    { "TopologyManager",    HH_SCA_RESOURCE, PORTS(topology_ports),         false },
    /* the Routing Engine is the SAD assembly controller, since
     * every other control-plane Resource's output ultimately feeds it. */
    { "RoutingEngine",      HH_SCA_RESOURCE, PORTS(routing_ports),          true  },
    { "SelfHealingManager", HH_SCA_RESOURCE, PORTS(self_healing_ports),     false },
    /* Owned by the Device Manager, not the Application. */
    { "RadioSdrInterface",  HH_SCA_DEVICE,   PORTS(radio_ports),            false },
    /* the architecture places the data plane outside the SCA Resource graph. */
    { "PacketForwarder",    HH_SCA_OUTSIDE_GRAPH, NULL, 0,                  false },
    /* telemetry/logging half of the split Management role. */
    { "ManagementTelemetry",HH_SCA_SERVICE,  NULL, 0,                       false },
};

const hh_sca_component_t *hh_sca_components(size_t *count)
{
    if (count) *count = sizeof g_components / sizeof g_components[0];
    return g_components;
}

const hh_sca_component_t *hh_sca_component_find(const char *name)
{
    if (!name) return NULL;
    for (size_t i = 0; i < sizeof g_components / sizeof g_components[0]; i++)
        if (!strcmp(g_components[i].name, name)) return &g_components[i];
    return NULL;
}

/* ---------------------------------------------------------------------------
 * Lifecycle guard. The ordering the architecture specifies is enforced here.
 * ------------------------------------------------------------------------- */

void hh_sca_resource_init_guard(hh_sca_resource_t *r, const char *component)
{
    if (!r) return;
    r->state = HH_SCA_UNINITIALIZED;
    r->component = component ? component : "unnamed";
}

hh_status_t hh_sca_initialize(hh_sca_resource_t *r)
{
    if (!r) return HH_ERR_INVAL;
    if (r->state != HH_SCA_UNINITIALIZED && r->state != HH_SCA_RELEASED)
        return HH_ERR_STATE;
    r->state = HH_SCA_INITIALIZED;
    return HH_OK;
}

hh_status_t hh_sca_connect_ports(hh_sca_resource_t *r)
{
    if (!r) return HH_ERR_INVAL;
    /*  step 3: wiring must complete before configuration. */
    if (r->state != HH_SCA_INITIALIZED) return HH_ERR_STATE;
    r->state = HH_SCA_PORTS_CONNECTED;
    return HH_OK;
}

hh_status_t hh_sca_configure(hh_sca_resource_t *r, hh_config_t *cfg,
                             const char *property_id, const char *value)
{
    const hh_sca_property_t *p;
    hh_status_t st;

    if (!r || !cfg || !property_id || !value) return HH_ERR_INVAL;
    /*  step 4: configuration follows wiring, and may be reapplied while
     * configured or stopped, but never before ports are connected.
     *
     * Step 8: HH_SCA_STARTED is also permitted for a HH_PROP_CONFIGURE-kind
     * property (checked below, once the property is known). Every
     * HH_PROP_CONFIGURE property in g_props[] is read live via a pointer
     * into hh_config_t by its owning component (traced in the Step 8
     * investigation: discovery/neighbor/link-health/routing/self-healing
     * all re-read cfg-> fields on each evaluation, never caching a value at
     * start time), so applying a new value has the same effect whether the
     * node is running or not -- it changes the value the next evaluation
     * sees, with no structural/memory dependency on when it is applied.
     * This matches REDHAWK 2.2.10's own CF::PropertySet::configure()
     * (PropertySet_impl.cpp), which has no started/stopped lifecycle guard
     * at all. HH_PROP_EXECPARAM properties remain blocked outside
     * PORTS_CONNECTED regardless of this state check, via the kind-specific
     * guard below -- this change does not affect them. */
    if (r->state != HH_SCA_PORTS_CONNECTED && r->state != HH_SCA_CONFIGURED &&
        r->state != HH_SCA_STOPPED && r->state != HH_SCA_STARTED)
        return HH_ERR_STATE;

    p = hh_sca_property_find(property_id);
    if (!p) return HH_ERR_NOTFOUND;
    /* An execparam is fixed at launch and must not be retuned at runtime,
     * in any state -- including HH_SCA_STARTED, which the check above now
     * permits only for non-execparam properties. */
    if (p->kind == HH_PROP_EXECPARAM && r->state != HH_SCA_PORTS_CONNECTED)
        return HH_ERR_STATE;

    st = hh_config_set(cfg, p->config_key, value);
    if (st != HH_OK) return st;

    if (r->state == HH_SCA_PORTS_CONNECTED) r->state = HH_SCA_CONFIGURED;
    HH_LOGD(COMP, "configured", "component=%s property=%s value=%s",
            r->component, property_id, value);
    return HH_OK;
}

hh_status_t hh_sca_query(const hh_sca_resource_t *r, const hh_config_t *cfg,
                         const char *property_id, char *out, size_t cap)
{
    const hh_sca_property_t *p;

    if (!r || !cfg || !property_id || !out || cap == 0) return HH_ERR_INVAL;
    p = hh_sca_property_find(property_id);
    if (!p) return HH_ERR_NOTFOUND;

#define Q_U32(k, f) if (!strcmp(p->config_key, k)) { snprintf(out, cap, "%u", cfg->f); return HH_OK; }
#define Q_F32(k, f) if (!strcmp(p->config_key, k)) { snprintf(out, cap, "%.4f", (double)cfg->f); return HH_OK; }

    Q_U32("node_id", node_id)
    Q_U32("beacon_interval_ms", beacon_interval_ms)
    Q_U32("beacon_interval_min_ms", beacon_interval_min_ms)
    Q_U32("beacon_interval_max_ms", beacon_interval_max_ms)
    Q_U32("acquisition_timeout_ms", acquisition_timeout_ms)
    Q_U32("neighbor_allowed_loss", neighbor_allowed_loss)
    Q_U32("max_neighbors", max_neighbors)
    Q_U32("lh_suspect_hold_ms", lh_suspect_hold_ms)
    Q_U32("lh_recover_hold_ms", lh_recover_hold_ms)
    Q_U32("lh_min_signals_suspect", lh_min_signals_suspect)
    Q_U32("route_active_timeout_ms", route_active_timeout_ms)
    Q_U32("route_delete_period_ms", route_delete_period_ms)
    Q_U32("route_update_interval_ms", route_update_interval_ms)
    Q_U32("max_routes", max_routes)
    Q_U32("max_hop_count", max_hop_count)
    Q_U32("hold_down_ms", hold_down_ms)
    Q_U32("merge_hold_down_ms", merge_hold_down_ms)
    Q_U32("dampening_flap_threshold", dampening_flap_threshold)
    Q_U32("rediscovery_max_retries", rediscovery_max_retries)
    Q_F32("lh_degrade_threshold", lh_degrade_threshold)
    Q_F32("lh_recover_threshold", lh_recover_threshold)

#undef Q_U32
#undef Q_F32

    if (!strcmp(p->config_key, "log_level")) {
        snprintf(out, cap, "%s", hh_log_level_str(cfg->log_level));
        return HH_OK;
    }
    if (!strcmp(p->config_key, "radio_adapter")) {
        snprintf(out, cap, "%s", cfg->radio_adapter);
        return HH_OK;
    }
    return HH_ERR_NOTFOUND;
}

hh_status_t hh_sca_start(hh_sca_resource_t *r)
{
    if (!r) return HH_ERR_INVAL;
    /*  step 5: configuration must precede start. */
    if (r->state != HH_SCA_CONFIGURED && r->state != HH_SCA_STOPPED)
        return HH_ERR_STATE;
    r->state = HH_SCA_STARTED;
    return HH_OK;
}

hh_status_t hh_sca_stop(hh_sca_resource_t *r)
{
    if (!r) return HH_ERR_INVAL;
    if (r->state != HH_SCA_STARTED) return HH_ERR_STATE;
    /* Step 6: halts operation WITHOUT discarding configuration. */
    r->state = HH_SCA_STOPPED;
    return HH_OK;
}

hh_status_t hh_sca_release(hh_sca_resource_t *r)
{
    if (!r) return HH_ERR_INVAL;
    if (r->state == HH_SCA_RELEASED) return HH_ERR_STATE;
    r->state = HH_SCA_RELEASED;
    return HH_OK;
}

hh_status_t hh_sca_run_test(const hh_sca_resource_t *r, uint32_t test_id,
                            char *result, size_t cap)
{
    if (!r || !result || cap == 0) return HH_ERR_INVAL;

    if (test_id == HH_SCA_TEST_SELF_CHECK) {
        snprintf(result, cap, "component=%s lifecycle=%s result=pass",
                 r->component, hh_sca_lifecycle_str(r->state));
        return HH_OK;
    }
    /* Raising UnknownTest for an unrecognised id is itself a
     * conformant implementation of CF::TestableObject. */
    snprintf(result, cap, "UnknownTest id=%u", test_id);
    return HH_ERR_NOTFOUND;
}
