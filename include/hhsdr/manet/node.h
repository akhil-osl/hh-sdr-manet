/*
 * Node — the assembled MANET control plane plus data plane.
 *
 * Owns the ten components and wires them through the event dispatcher, never
 * through direct nested calls between subsystems. Each component
 * subscribes to the event types it consumes; the node's job is the wiring, the
 * lifecycle, and the periodic tick, not any MANET logic of its own.
 *
 * Wiring mirrors the HW/SW spec's interface map exactly:
 *   Radio      -> Discovery         (HTI-04 beacon rx)
 *   Radio      -> LinkHealth        (HTI-05 metric samples)
 *   Discovery  -> Neighbor          (validated beacon)
 *   Neighbor   -> Topo/Route/LH     (HTI-06 fan-out)
 *   LinkHealth -> FD/Route/Topo     (HTI-07 fan-out)
 *   LinkHealth -> Discovery         (HTI-16 adaptive cadence)
 *   FD         -> SelfHealing       (HTI-10)
 *   Topo       -> SelfHealing       (HTI-12/13)
 *   Route      -> Forwarder/Topo    (HTI-08/09)
 *
 * SCA lifecycle: initialize -> connect ports -> configure -> start
 * -> stop -> release. hh_node_init performs initialize and port connection;
 * hh_node_configure applies properties; hh_node_start begins operation.
 */
#ifndef HHSDR_MANET_NODE_H
#define HHSDR_MANET_NODE_H

#include "hhsdr/dataplane/forwarder.h"
#include "hhsdr/manet/discovery.h"
#include "hhsdr/manet/failure_detector.h"
#include "hhsdr/manet/neighbor.h"
#include "hhsdr/manet/routing.h"
#include "hhsdr/manet/self_healing.h"
#include "hhsdr/manet/topology.h"

typedef enum {
    HH_NODE_CREATED = 0,   /* constructed, not yet initialized */
    HH_NODE_INITIALIZED,   /* known internal state, ports wired */
    HH_NODE_CONFIGURED,
    HH_NODE_RUNNING,
    HH_NODE_STOPPED,
    HH_NODE_RELEASED
} hh_node_state_t;

typedef struct {
    hh_config_t       cfg;
    const hh_clock_t *clock;
    hh_radio_t       *radio;
    hh_node_state_t   state;

    hh_dispatcher_t       bus;
    hh_discovery_t        discovery;
    hh_neighbor_mgr_t     neighbors;
    hh_link_health_t      link_health;
    hh_routing_t          routing;
    hh_failure_detector_t failure_detector;
    hh_topology_t         topology;
    hh_self_healing_t     self_healing;
    hh_forwarder_t        forwarder;

    hh_time_ms_t last_route_update_at;
    uint64_t     ticks;
    uint64_t     route_updates_sent;
    uint64_t     route_updates_rx;
} hh_node_t;

/* Lifecycle. */
hh_status_t hh_node_init(hh_node_t *n, const hh_config_t *cfg,
                         const hh_clock_t *clock, hh_radio_t *radio);
hh_status_t hh_node_configure(hh_node_t *n, const hh_config_t *cfg);
hh_status_t hh_node_start(hh_node_t *n);
hh_status_t hh_node_stop(hh_node_t *n);
hh_status_t hh_node_release(hh_node_t *n);

/* One control-loop iteration: service the radio, run timers, drain events. */
hh_status_t hh_node_tick(hh_node_t *n, hh_time_ms_t now);

/* Application data entry point (payload is opaque bytes). */
hh_status_t hh_node_send(hh_node_t *n, hh_node_id_t dst, const uint8_t *payload,
                         uint16_t len, hh_time_ms_t now);

/* Inbound frame from the radio adapter. Public so a test harness can deliver
 * frames through exactly the path real hardware uses. */
void hh_node_on_frame(hh_node_t *n, const hh_frame_t *f, const hh_link_sample_t *m);

const char *hh_node_state_str(hh_node_state_t s);

#endif /* HHSDR_MANET_NODE_H */
