/*
 * MANET_Node SCA adapter -- implementation.
 *
 * Every function here does exactly one of two things: (1) drive the
 * existing hh_sca_resource_t lifecycle guard (src/sca/resource.c), or
 * (2) call straight through to hh_node_t (src/manet/node.c). No MANET
 * algorithm logic is reimplemented or duplicated here -- see
 * manet_sca_adapter.h for the full scope statement.
 */
#include "manet_sca_adapter.h"
#include <stdio.h>
#include <string.h>

static const char *COMPONENT_NAME = "MANET_Node";

hh_status_t manet_sca_initialize(manet_sca_adapter_t *a, const hh_clock_t *clock,
                                 hh_radio_t *radio)
{
    if (!a || !clock || !radio) return HH_ERR_INVAL;

    memset(a, 0, sizeof *a);
    hh_sca_resource_init_guard(&a->guard, COMPONENT_NAME);
    hh_config_defaults(&a->cfg);
    a->cfg_seeded = true;

    hh_status_t st = hh_sca_initialize(&a->guard);
    if (st != HH_OK) return st;

    /*
     * hh_node_init() performs the MANET stack's own "initialize + connect
     * ports" (see node.h's own lifecycle comment); this call is what
     * carries the SCA guard from INITIALIZED to PORTS_CONNECTED, since the
     * node's internal wiring (dispatcher subscriptions) IS the port
     * connection this phase has -- there are no separate CORBA ports to
     * connect yet.
     */
    st = hh_node_init(&a->node, &a->cfg, clock, radio);
    if (st != HH_OK) return st;

    return hh_sca_connect_ports(&a->guard);
}

hh_status_t manet_sca_release_object(manet_sca_adapter_t *a)
{
    if (!a) return HH_ERR_INVAL;
    hh_status_t st = hh_node_release(&a->node);
    if (st != HH_OK) return st;
    return hh_sca_release(&a->guard);
}

hh_status_t manet_sca_run_test(const manet_sca_adapter_t *a, uint32_t test_id,
                               char *result, size_t cap)
{
    if (!a) return HH_ERR_INVAL;
    return hh_sca_run_test(&a->guard, test_id, result, cap);
}

hh_status_t manet_sca_configure(manet_sca_adapter_t *a,
                                const manet_sca_propval_t *props, size_t count,
                                size_t *applied)
{
    if (!a || (!props && count > 0)) return HH_ERR_INVAL;
    if (applied) *applied = 0;

    for (size_t i = 0; i < count; i++) {
        hh_status_t st = hh_sca_configure(&a->guard, &a->cfg, props[i].id, props[i].value);
        if (st != HH_OK) return st;
        if (applied) (*applied)++;
    }

    /*
     * hh_sca_configure() writes into a->cfg but hh_node_t already copied
     * its own hh_config_t by value in hh_node_init() (node.h: `hh_config_t
     * cfg;` is a member, not a pointer). Push the updated values into the
     * node the same way hh_node_configure() documents: applying properties
     * follows port connection and precedes start.
     */
    return hh_node_configure(&a->node, &a->cfg);
}

hh_status_t manet_sca_query(const manet_sca_adapter_t *a, const char *property_id,
                            char *out, size_t cap)
{
    if (!a) return HH_ERR_INVAL;
    return hh_sca_query(&a->guard, &a->cfg, property_id, out, cap);
}

const hh_sca_property_t *manet_sca_properties(size_t *count)
{
    return hh_sca_properties(count);
}

hh_status_t manet_sca_get_port(const char *name, manet_sca_port_t *out)
{
    if (!name || !out) return HH_ERR_INVAL;

    if (!strcmp(name, "radio_frame_in"))  { *out = MANET_PORT_RADIO_FRAME_IN;  return HH_OK; }
    if (!strcmp(name, "radio_frame_out")) { *out = MANET_PORT_RADIO_FRAME_OUT; return HH_OK; }
    if (!strcmp(name, "app_data_in"))     { *out = MANET_PORT_APP_DATA_IN;     return HH_OK; }
    if (!strcmp(name, "status_out"))      { *out = MANET_PORT_STATUS_OUT;      return HH_OK; }

    return HH_ERR_NOTFOUND; /* CF::PortSupplier::UnknownPort, in a real binding */
}

void manet_sca_on_frame(manet_sca_adapter_t *a, const hh_frame_t *frame,
                        const hh_link_sample_t *metrics)
{
    if (!a || !frame) return;
    hh_node_on_frame(&a->node, frame, metrics);
}

hh_status_t manet_sca_send(manet_sca_adapter_t *a, hh_node_id_t dst,
                           const uint8_t *payload, uint16_t len, hh_time_ms_t now)
{
    if (!a) return HH_ERR_INVAL;
    return hh_node_send(&a->node, dst, payload, len, now);
}

void manet_sca_status(const manet_sca_adapter_t *a, hh_node_status_t *out)
{
    if (!a || !out) return;
    hh_telemetry_node_status(&a->node, out);
}

const char *manet_sca_identifier(const manet_sca_adapter_t *a, char *buf, size_t cap)
{
    if (!a || !buf || cap == 0) return NULL;
    if (a->cfg.node_id == HH_NODE_ID_INVALID) {
        snprintf(buf, cap, "%s.unconfigured", COMPONENT_NAME);
    } else {
        snprintf(buf, cap, "%s.%u", COMPONENT_NAME, (unsigned)a->cfg.node_id);
    }
    return buf;
}

hh_status_t manet_sca_start(manet_sca_adapter_t *a)
{
    if (!a) return HH_ERR_INVAL;
    hh_status_t st = hh_sca_start(&a->guard);
    if (st != HH_OK) return st;
    return hh_node_start(&a->node);
}

hh_status_t manet_sca_stop(manet_sca_adapter_t *a)
{
    if (!a) return HH_ERR_INVAL;
    hh_status_t st = hh_sca_stop(&a->guard);
    if (st != HH_OK) return st;
    return hh_node_stop(&a->node);
}

hh_status_t manet_sca_tick(manet_sca_adapter_t *a, hh_time_ms_t now)
{
    if (!a) return HH_ERR_INVAL;
    return hh_node_tick(&a->node, now);
}
