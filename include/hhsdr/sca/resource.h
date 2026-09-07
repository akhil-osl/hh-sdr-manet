/*
 * SCA 2.2.2 compatibility layer (Doc 1 §3, §12, §14).
 *
 * ==========================================================================
 * WHAT THIS IS, AND WHAT IT IS NOT
 * ==========================================================================
 * This is SCA-COMPATIBLE ARCHITECTURE, not an SCA implementation and not a
 * conformance claim. The distinction matters and Doc 1 §14 draws it explicitly.
 *
 * IMPLEMENTED HERE:
 *  - The CF::LifeCycle / CF::Resource state sequence as an explicit boundary
 *    around each control-plane component: initialize -> connect ports ->
 *    configure -> start -> stop -> releaseObject, with the ordering ENFORCED
 *    rather than merely documented.
 *  - A CF::PropertySet-shaped property surface (configure/query) over the
 *    tunables Doc 1 names, each with a stable string property id and a kind.
 *  - A CF::TestableObject-shaped runTest hook, which Doc 1 §12 notes may
 *    conformantly raise UnknownTest for every id.
 *  - Component classification (Resource / Device / outside-the-graph) matching
 *    Doc 1 §3 exactly.
 *
 * DELIBERATELY NOT IMPLEMENTED (would require inventing what does not exist):
 *  - CORBA ORB, IDL-generated stubs/skeletons, Naming or Event Service. SCA
 *    2.2.2 mandates minimumCORBA; none is present here. The HW/SW spec supplies
 *    IDL struct definitions, but no ORB binding exists.
 *  - Domain Manager, Device Manager, Application Factory as running components.
 *  - SPD/SCD/PRF/SAD/DCD/DMD XML descriptors. Their CONTENT BASIS exists (port
 *    lists, property ids, classification below); the XML documents do not.
 *  - Any hardware Device implementation, since the radio contract is unavailable.
 *
 * See docs/SCA-COMPATIBILITY.md for the full gap list.
 * ==========================================================================
 */
#ifndef HHSDR_SCA_RESOURCE_H
#define HHSDR_SCA_RESOURCE_H

#include "hhsdr/core/config.h"
#include "hhsdr/core/types.h"

/* SCA CF component classification (Doc 1 §3). */
typedef enum {
    HH_SCA_RESOURCE = 0,   /* CF::Resource, deployed by the Application    */
    HH_SCA_DEVICE,         /* CF::Device, owned by the Device Manager      */
    HH_SCA_SERVICE,        /* domain-wide support, no mandated CF lifecycle */
    HH_SCA_OUTSIDE_GRAPH   /* explicitly outside the SCA Resource graph    */
} hh_sca_class_t;

/* CF::LifeCycle / CF::Resource state sequence (Doc 1 §3). */
typedef enum {
    HH_SCA_UNINITIALIZED = 0,
    HH_SCA_INITIALIZED,
    HH_SCA_PORTS_CONNECTED,
    HH_SCA_CONFIGURED,
    HH_SCA_STARTED,
    HH_SCA_STOPPED,
    HH_SCA_RELEASED
} hh_sca_lifecycle_t;

const char *hh_sca_class_str(hh_sca_class_t c);
const char *hh_sca_lifecycle_str(hh_sca_lifecycle_t s);

/* PRF property kinds (Doc 1 §12). */
typedef enum {
    HH_PROP_CONFIGURE = 0,   /* runtime-tunable via configure()    */
    HH_PROP_EXECPARAM,       /* set at launch                      */
    HH_PROP_ALLOCATION       /* used by deployment-time matching   */
} hh_prop_kind_t;

typedef enum {
    HH_PROP_U32 = 0,
    HH_PROP_F32,
    HH_PROP_BOOL,
    HH_PROP_STRING
} hh_prop_type_t;

/* One PRF property: a stable id, a kind, and a binding into hh_config_t. */
typedef struct {
    const char    *id;          /* stable property id, as a PRF would carry */
    const char    *config_key;  /* corresponding hh_config_set key          */
    hh_prop_kind_t kind;
    hh_prop_type_t type;
    const char    *description;
} hh_sca_property_t;

/* The property surface, derived from the tunables Doc 1 §§4-8 name. */
const hh_sca_property_t *hh_sca_properties(size_t *count);
const hh_sca_property_t *hh_sca_property_find(const char *id);

/* A declared port, as an SCD would carry it. */
typedef struct {
    const char *name;
    bool        is_uses;     /* true = uses (outbound), false = provides */
    const char *hti_id;      /* the HW/SW spec interface this realizes   */
} hh_sca_port_t;

/* One SCA-classified component. */
typedef struct {
    const char           *name;
    hh_sca_class_t        sca_class;
    const hh_sca_port_t  *ports;
    size_t                port_count;
    bool                  is_assembly_controller;
} hh_sca_component_t;

const hh_sca_component_t *hh_sca_components(size_t *count);
const hh_sca_component_t *hh_sca_component_find(const char *name);

/*
 * CF::Resource-shaped lifecycle guard. Each control-plane component's public
 * lifecycle is validated through this, so illegal orderings are rejected rather
 * than merely discouraged.
 */
typedef struct {
    hh_sca_lifecycle_t state;
    const char        *component;
} hh_sca_resource_t;

void        hh_sca_resource_init_guard(hh_sca_resource_t *r, const char *component);
hh_status_t hh_sca_initialize(hh_sca_resource_t *r);
hh_status_t hh_sca_connect_ports(hh_sca_resource_t *r);
hh_status_t hh_sca_configure(hh_sca_resource_t *r, hh_config_t *cfg,
                             const char *property_id, const char *value);
hh_status_t hh_sca_query(const hh_sca_resource_t *r, const hh_config_t *cfg,
                         const char *property_id, char *out, size_t cap);
hh_status_t hh_sca_start(hh_sca_resource_t *r);
hh_status_t hh_sca_stop(hh_sca_resource_t *r);
hh_status_t hh_sca_release(hh_sca_resource_t *r);

/* CF::TestableObject::runTest. Doc 1 §12 records that a component without a
 * meaningful built-in test may conformantly raise UnknownTest for every id. */
#define HH_SCA_TEST_SELF_CHECK 1u
hh_status_t hh_sca_run_test(const hh_sca_resource_t *r, uint32_t test_id,
                            char *result, size_t cap);

#endif /* HHSDR_SCA_RESOURCE_H */
