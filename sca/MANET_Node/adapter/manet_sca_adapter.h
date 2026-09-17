/*
 * MANET_Node SCA adapter -- public interface.
 *
 * WHAT THIS IS: a plain-C translation layer between the SCA 2.2.2 CF
 * interface *operations* (Appendix C: CF::LifeCycle, CF::TestableObject,
 * CF::PropertySet, CF::PortSupplier, CF::Resource) and the existing,
 * unmodified MANET C implementation (hh_node_t, include/hhsdr/manet/node.h)
 * plus its existing SCA-shaped lifecycle guard (hh_sca_resource_t,
 * include/hhsdr/sca/resource.h).
 *
 * WHAT THIS IS NOT:
 *  - No CORBA/ORB dependency. Function signatures here take plain C types,
 *    not CORBA::Any / CF::Properties structs, because no IDL compiler
 *    output exists in this repository (see the SCD's AMBIGUITIES note).
 *    A real CF::Resource CORBA servant would wrap THESE functions; this
 *    header is that seam, not the servant itself.
 *  - No DomainManager/DeviceManager/ApplicationFactory dependency. Nothing
 *    here registers with, or expects to be instantiated by, a Core
 *    Framework -- explicitly out of scope for this phase.
 *  - No REDHAWK-specific API. Only the SCA 2.2.2 CF operation *shapes* are
 *    implemented.
 *
 * DESIGN: the MANET algorithm (libhhsdr_core.a) stays completely
 * independent of this adapter. This file depends on hhsdr/manet/node.h and
 * hhsdr/sca/resource.h; nothing under src/manet, src/core, src/dataplane,
 * src/radio is modified or aware that an SCA adapter exists. The adapter
 * can be deleted entirely with zero effect on the production hh-manet
 * daemon or its test suite.
 */
#ifndef MANET_SCA_ADAPTER_H
#define MANET_SCA_ADAPTER_H

#include "hhsdr/manet/node.h"
#include "hhsdr/manet/telemetry.h"
#include "hhsdr/radio/radio.h"
#include "hhsdr/sca/resource.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * One SCA property value as a string, mirroring how hh_sca_configure()
 * already takes a `const char *value` (src/sca/resource.c) and how PRF
 * values are XML text. A real CF::Properties <-> this mapping is a CORBA
 * binding concern deferred to the future IDL/ORB integration noted in the
 * SPD; this adapter stays at the string-value level the existing SCA
 * compatibility layer already uses.
 */
typedef struct {
    const char *id;     /* PRF property id, e.g. "hh::routing::max_hop_count" */
    const char *value;
} manet_sca_propval_t;

/*
 * Adapter instance. Owns exactly the two things a real CF::Resource servant
 * would also need to hold: the SCA lifecycle guard already defined in
 * src/sca/resource.c, and the assembled MANET node it fronts. The radio and
 * clock are supplied by the caller (mirroring src/main.c's own assembly),
 * since this phase does not define how a radio adapter is selected under
 * SCA -- that remains src/main.c's / a future Device's responsibility.
 */
typedef struct {
    hh_sca_resource_t guard;    /* CF::LifeCycle / CF::Resource state machine */
    hh_node_t         node;     /* the wrapped MANET implementation           */
    hh_config_t       cfg;      /* working configuration, pre-configure()     */
    bool               cfg_seeded;
} manet_sca_adapter_t;

/* ---- CF::LifeCycle ---------------------------------------------------- */

/*
 * initialize(): CF::LifeCycle::initialize().
 * Prepares the adapter's internal state (hh_config_defaults + SCA lifecycle
 * guard) but does not yet construct hh_node_t -- construction needs the
 * radio handle and clock, supplied here because no execparam-driven
 * "resource factory" exists in this phase (that is normally an Application
 * Factory's job, explicitly out of scope). Raises no CF::LifeCycle
 * exception type since no ORB/exception mapping exists yet; returns
 * hh_status_t, mirroring every other SCA-adjacent call in this codebase.
 */
hh_status_t manet_sca_initialize(manet_sca_adapter_t *a, const hh_clock_t *clock,
                                 hh_radio_t *radio);

/* releaseObject(): CF::LifeCycle::releaseObject(). Tears the node down via
 * hh_node_release() and marks the SCA guard HH_SCA_RELEASED. */
hh_status_t manet_sca_release_object(manet_sca_adapter_t *a);

/* ---- CF::TestableObject ------------------------------------------------ */

/* runTest(): CF::TestableObject::runTest(testid, inout testValues).
 * testValues is omitted here (see manet_sca_propval_t note above); the
 * existing hh_sca_run_test() self-check (HH_SCA_TEST_SELF_CHECK) is the
 * only test id implemented, matching src/sca/resource.c exactly. Any other
 * testid returns HH_ERR_NOTFOUND, corresponding to CF::TestableObject::
 * UnknownTest. */
hh_status_t manet_sca_run_test(const manet_sca_adapter_t *a, uint32_t test_id,
                               char *result, size_t cap);

/* ---- CF::PropertySet ---------------------------------------------------- */

/*
 * configure(): CF::PropertySet::configure(configProperties).
 * Applies each id/value pair via the existing hh_sca_configure(), which
 * already enforces execparam-vs-runtime rules and lifecycle ordering
 * (src/sca/resource.c). Returns HH_ERR_NOTFOUND for an unknown property id
 * (-> CF::UnknownProperties in a real binding), HH_ERR_STATE for an illegal
 * ordering (-> CF::PropertySet::InvalidConfiguration), HH_OK otherwise.
 * Stops at the first failure and reports how many of `count` entries were
 * applied via *applied, corresponding to a real binding's need to
 * distinguish InvalidConfiguration (zero applied) from PartialConfiguration
 * (some applied) -- the exception TYPE selection itself is left to the
 * future CORBA binding.
 */
hh_status_t manet_sca_configure(manet_sca_adapter_t *a,
                                const manet_sca_propval_t *props, size_t count,
                                size_t *applied);

/* query(): CF::PropertySet::query(inout configProperties).
 * Fills `out` (cap bytes) with the current value of one property id via the
 * existing hh_sca_query(). HH_ERR_NOTFOUND corresponds to
 * CF::UnknownProperties. */
hh_status_t manet_sca_query(const manet_sca_adapter_t *a, const char *property_id,
                            char *out, size_t cap);

/* Enumerate the full property surface (id, kind, type, description), for a
 * caller building a CF::Properties query-all response. Thin pass-through to
 * hh_sca_properties(). */
const hh_sca_property_t *manet_sca_properties(size_t *count);

/* ---- CF::PortSupplier --------------------------------------------------- */

typedef enum {
    MANET_PORT_RADIO_FRAME_IN = 0,  /* provides: hh_node_on_frame()        */
    MANET_PORT_RADIO_FRAME_OUT,     /* uses: hh_radio_transmit() via node  */
    MANET_PORT_APP_DATA_IN,         /* provides: hh_node_send()            */
    MANET_PORT_STATUS_OUT,          /* provides: telemetry export          */
    MANET_PORT__COUNT
} manet_sca_port_t;

/*
 * getPort(name): CF::PortSupplier::getPort(name).
 * Resolves a port name (as declared in MANET_Node.scd.xml: "radio_frame_in",
 * "radio_frame_out", "app_data_in", "status_out") to the manet_sca_port_t
 * enum identifying which of the four boundary operations below it maps to.
 * Returns HH_ERR_NOTFOUND for an unrecognised name, corresponding to
 * CF::PortSupplier::UnknownPort. There is no CORBA Object to return in this
 * phase (see header note); a real binding would return the port servant
 * this enum identifies.
 */
hh_status_t manet_sca_get_port(const char *name, manet_sca_port_t *out);

/* ---- Port-body operations (the actual data movement each port performs) */

/* radio_frame_in: deliver an inbound frame + its measured link sample,
 * exactly as a real radio adapter's rx callback does today
 * (hh_radio_rx_fn, radio.h). */
void manet_sca_on_frame(manet_sca_adapter_t *a, const hh_frame_t *frame,
                        const hh_link_sample_t *metrics);

/* app_data_in: inject application payload for delivery to `dst`. */
hh_status_t manet_sca_send(manet_sca_adapter_t *a, hh_node_id_t dst,
                           const uint8_t *payload, uint16_t len, hh_time_ms_t now);

/* status_out: snapshot current node status (telemetry.h shape). */
void manet_sca_status(const manet_sca_adapter_t *a, hh_node_status_t *out);

/* ---- CF::Resource -------------------------------------------------------- */

/* identifier attribute: stable string naming this resource instance.
 * Sourced from cfg.node_id once configured; "unconfigured" before that. */
const char *manet_sca_identifier(const manet_sca_adapter_t *a, char *buf, size_t cap);

/* start(): CF::Resource::start(). Delegates to hh_node_start() via the SCA
 * guard's ordering (initialize -> connect ports -> configure -> start). */
hh_status_t manet_sca_start(manet_sca_adapter_t *a);

/* stop(): CF::Resource::stop(). Delegates to hh_node_stop(); preserves
 * configuration exactly as hh_sca_stop() documents. */
hh_status_t manet_sca_stop(manet_sca_adapter_t *a);

/* One control-loop iteration. Not a CF interface operation -- this phase
 * defines no CF::Resource-mandated "run" operation, because SCA 2.2.2 does
 * not mandate one either (a Resource's internal execution model is
 * implementation-defined). Exposed so a future ORB-side thread or a test
 * harness can drive the wrapped node the same way src/main.c's control loop
 * does. */
hh_status_t manet_sca_tick(manet_sca_adapter_t *a, hh_time_ms_t now);

#ifdef __cplusplus
}
#endif

#endif /* MANET_SCA_ADAPTER_H */
