/*
 * Simulation presentation layer — DEVELOPMENT/TEST TOOLING ONLY.
 *
 * Lives under tools/ and links against tests/sim + the production library. It
 * is never linked into hh-manet, and production code has no knowledge of it.
 *
 * Everything printed here is derived from real state:
 *   - Events come from the production structured-log stream via hh_log_set_sink,
 *     so a printed event happened because a component emitted it.
 *   - Hop traces come from netsim's frame observer, which fires on the frames
 *     the virtual medium actually moves.
 *   - Neighbor/route/topology views read the live tables and the published
 *     route snapshot.
 *
 * Nothing is fabricated or modelled separately from the stack under test.
 */
#ifndef HH_SIMUI_H
#define HH_SIMUI_H

#include "netsim.h"

#define SIMUI_MAX_PACKETS 512
#define SIMUI_MAX_HOPS    12

/* ---- node naming: ids 1..26 render as A..Z for readability ---- */
const char *simui_name(hh_node_id_t id);
hh_node_id_t simui_id_from_index(size_t index);   /* 0 -> 1 (A), 1 -> 2 (B) ... */

/* ---- timestamped event log, fed by the production log sink ---- */
typedef struct {
    bool         enabled;      /* print events as they occur */
    hh_time_ms_t t0;           /* time base for [ss.mmm] stamps */
    const netsim_t *sim;       /* for current virtual time */
    uint64_t     count;
} simui_events_t;

/* Attach to the production log stream. Only records the stack actually emits
 * are shown; nothing is synthesized. */
void simui_events_attach(simui_events_t *ev, const netsim_t *s, bool enabled);
void simui_events_detach(void);

/* Re-assert the capture level after anything that may have changed it
 * (hh_node_configure applies a node's configured level globally). */
void simui_events_refresh(void);

/* Print an operator-facing line on the same timeline as captured events.
 * Used for things the harness itself does (breaking a link, starting a node). */
void simui_action(const netsim_t *s, const char *fmt, ...);

/* ---- per-packet tracking, driven by the netsim frame observer ---- */
typedef struct {
    uint32_t     id;
    hh_node_id_t src, dst;
    hh_time_ms_t sent_at;
    hh_time_ms_t delivered_at;
    bool         delivered;
    hh_node_id_t hops[SIMUI_MAX_HOPS];
    uint8_t      hop_count;
    bool         used;
} simui_packet_t;

typedef struct {
    simui_packet_t packets[SIMUI_MAX_PACKETS];
    size_t         count;
    netsim_t      *sim;
    bool           trace;          /* print each hop as it happens */
    uint64_t       frames_dropped_in_flight;
} simui_traffic_t;

void simui_traffic_init(simui_traffic_t *t, netsim_t *s, bool trace);

/* Send one application packet through the production data plane. The payload
 * carries a 4-byte tag so a delivery can be matched back to its send. */
hh_status_t simui_send(simui_traffic_t *t, hh_node_id_t from, hh_node_id_t to);

/* Send `count` packets spaced `interval_ms` apart, running the sim between
 * them so forwarding actually progresses. */
void simui_send_burst(simui_traffic_t *t, hh_node_id_t from, hh_node_id_t to,
                      uint32_t count, uint32_t interval_ms, uint32_t settle_ms);

/* ---- reporting ---- */
void simui_print_header(const char *title);
void simui_print_rule(void);
void simui_print_node_states(netsim_t *s);
void simui_print_topology(netsim_t *s);
void simui_print_routes_from(netsim_t *s, hh_node_id_t from);
void simui_print_path(netsim_t *s, hh_node_id_t from, hh_node_id_t to);
void simui_print_link_health(netsim_t *s, hh_node_id_t from);
void simui_print_traffic_stats(const simui_traffic_t *t, netsim_t *s);

/* ---- step mode ---- */
typedef struct {
    bool     enabled;         /* --step: pause between phases */
    unsigned step_no;
} simui_stepper_t;

void simui_step_begin(simui_stepper_t *st, const char *description);

#endif /* HH_SIMUI_H */
