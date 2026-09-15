/*
 * Node lifecycle and frame-ingest tests.
 *
 * Covers what test_integration.c's happy-path lifecycle test does not:
 * NULL/invalid-argument rejection on every hh_node_* entry point, hh_node_send
 * boundary cases, and the HH_FRAME_ROUTING ingest path (handle_route_update)
 * in isolation -- self-sender echo, self-originator filtering, and hop-count
 * saturation at HH_HOP_INFINITY, none of which any existing test asserts on
 * directly (only reachable indirectly through netsim).
 */
#include "hhsdr/manet/node.h"
#include "hhsdr/manet/telemetry.h"
#include "hhsdr/radio/wire.h"
#include "mock_radio.h"
#include "hh_test.h"
#include "vclock.h"
#include <string.h>

typedef struct {
    hh_config_t  cfg;
    vclock_t     vc;
    mock_radio_t mock;
    hh_radio_t   radio;
    hh_node_t    node;
} fix_t;

static void fix_init(fix_t *f, hh_node_id_t id)
{
    memset(f, 0, sizeof *f);
    hh_config_defaults(&f->cfg);
    f->cfg.node_id = id;
    f->cfg.log_level = HH_LOG_ERROR;
    vclock_init(&f->vc, 1000);
    mock_radio_init(&f->mock, "node", &f->radio);
    HH_ASSERT_OK(hh_node_init(&f->node, &f->cfg, &f->vc.clock, &f->radio));
}

/* ---------------- Lifecycle: NULL / invalid-argument handling ---------------- */

static void test_init_rejects_null_arguments(void)
{
    hh_config_t cfg;
    vclock_t vc;
    mock_radio_t mock;
    hh_radio_t radio;
    hh_node_t node;

    hh_config_defaults(&cfg);
    cfg.node_id = 1;
    vclock_init(&vc, 0);
    mock_radio_init(&mock, "n", &radio);

    HH_ASSERT_ERR(hh_node_init(NULL, &cfg, &vc.clock, &radio), HH_ERR_INVAL);
    HH_ASSERT_ERR(hh_node_init(&node, NULL, &vc.clock, &radio), HH_ERR_INVAL);
    HH_ASSERT_ERR(hh_node_init(&node, &cfg, NULL, &radio), HH_ERR_INVAL);
    HH_ASSERT_ERR(hh_node_init(&node, &cfg, &vc.clock, NULL), HH_ERR_INVAL);
}

static void test_init_rejects_invalid_config(void)
{
    hh_config_t cfg;
    vclock_t vc;
    mock_radio_t mock;
    hh_radio_t radio;
    hh_node_t node;

    hh_config_defaults(&cfg);
    cfg.node_id = 1;
    /* Violate the hysteresis invariant hh_config_validate enforces. */
    cfg.lh_recover_threshold = cfg.lh_degrade_threshold;
    vclock_init(&vc, 0);
    mock_radio_init(&mock, "n", &radio);

    HH_ASSERT(hh_node_init(&node, &cfg, &vc.clock, &radio) != HH_OK);
}

static void test_configure_rejects_null_and_wrong_state(void)
{
    fix_t f; fix_init(&f, 1);

    HH_ASSERT_ERR(hh_node_configure(NULL, NULL), HH_ERR_INVAL);

    /* configure() is valid from INITIALIZED... */
    HH_ASSERT_OK(hh_node_configure(&f.node, NULL));
    HH_ASSERT_EQ_INT(f.node.state, HH_NODE_CONFIGURED);

    /* ...but not again from CONFIGURED: only INITIALIZED or STOPPED admit it. */
    HH_ASSERT_ERR(hh_node_configure(&f.node, NULL), HH_ERR_STATE);
}

static void test_configure_with_null_cfg_keeps_current(void)
{
    fix_t f; fix_init(&f, 1);
    uint32_t before = f.node.cfg.beacon_interval_ms;

    HH_ASSERT_OK(hh_node_configure(&f.node, NULL));
    HH_ASSERT_EQ_INT(f.node.cfg.beacon_interval_ms, before);
}

static void test_configure_rejects_invalid_config(void)
{
    fix_t f; fix_init(&f, 1);
    hh_config_t bad = f.cfg;
    bad.lh_recover_threshold = bad.lh_degrade_threshold;   /* invalid */

    HH_ASSERT(hh_node_configure(&f.node, &bad) != HH_OK);
    /* Rejected config must not have been applied. */
    HH_ASSERT_EQ_INT(f.node.state, HH_NODE_INITIALIZED);
}

static void test_start_rejects_null_and_wrong_state(void)
{
    fix_t f; fix_init(&f, 1);

    HH_ASSERT_ERR(hh_node_start(NULL), HH_ERR_INVAL);
    /* start() before configure() is rejected. */
    HH_ASSERT_ERR(hh_node_start(&f.node), HH_ERR_STATE);

    hh_node_configure(&f.node, NULL);
    HH_ASSERT_OK(hh_node_start(&f.node));
    HH_ASSERT_EQ_INT(f.node.state, HH_NODE_RUNNING);
    /* Starting an already-running node is rejected. */
    HH_ASSERT_ERR(hh_node_start(&f.node), HH_ERR_STATE);
}

static void test_stop_rejects_null_and_wrong_state(void)
{
    fix_t f; fix_init(&f, 1);

    HH_ASSERT_ERR(hh_node_stop(NULL), HH_ERR_INVAL);
    /* Stopping a node that never started is rejected. */
    HH_ASSERT_ERR(hh_node_stop(&f.node), HH_ERR_STATE);

    hh_node_configure(&f.node, NULL);
    hh_node_start(&f.node);
    HH_ASSERT_OK(hh_node_stop(&f.node));
    HH_ASSERT_EQ_INT(f.node.state, HH_NODE_STOPPED);
    /* Double-stop is rejected. */
    HH_ASSERT_ERR(hh_node_stop(&f.node), HH_ERR_STATE);
}

static void test_release_rejects_null_and_is_idempotent_guard(void)
{
    fix_t f; fix_init(&f, 1);

    HH_ASSERT_ERR(hh_node_release(NULL), HH_ERR_INVAL);

    hh_node_configure(&f.node, NULL);
    hh_node_start(&f.node);
    /* release() while running stops first, then releases -- no separate
     * stop() call required. */
    HH_ASSERT_OK(hh_node_release(&f.node));
    HH_ASSERT_EQ_INT(f.node.state, HH_NODE_RELEASED);
}

static void test_tick_rejects_null_and_not_running(void)
{
    fix_t f; fix_init(&f, 1);

    HH_ASSERT_ERR(hh_node_tick(NULL, f.vc.now), HH_ERR_INVAL);
    /* Not yet configured/started. */
    HH_ASSERT_ERR(hh_node_tick(&f.node, f.vc.now), HH_ERR_STATE);

    hh_node_configure(&f.node, NULL);
    /* Configured but not started. */
    HH_ASSERT_ERR(hh_node_tick(&f.node, f.vc.now), HH_ERR_STATE);

    hh_node_start(&f.node);
    HH_ASSERT_OK(hh_node_tick(&f.node, f.vc.now));

    hh_node_stop(&f.node);
    HH_ASSERT_ERR(hh_node_tick(&f.node, f.vc.now), HH_ERR_STATE);
}

static void test_state_str_covers_every_state_and_unknown(void)
{
    HH_ASSERT_EQ_STR(hh_node_state_str(HH_NODE_CREATED), "created");
    HH_ASSERT_EQ_STR(hh_node_state_str(HH_NODE_INITIALIZED), "initialized");
    HH_ASSERT_EQ_STR(hh_node_state_str(HH_NODE_CONFIGURED), "configured");
    HH_ASSERT_EQ_STR(hh_node_state_str(HH_NODE_RUNNING), "running");
    HH_ASSERT_EQ_STR(hh_node_state_str(HH_NODE_STOPPED), "stopped");
    HH_ASSERT_EQ_STR(hh_node_state_str(HH_NODE_RELEASED), "released");
    HH_ASSERT_EQ_STR(hh_node_state_str((hh_node_state_t)999), "unknown");
}

/* ---------------- hh_node_send: NULL, state, and boundary cases ---------------- */

static void test_send_rejects_null_and_not_running(void)
{
    fix_t f; fix_init(&f, 1);
    uint8_t payload[] = { 1, 2, 3 };

    HH_ASSERT_ERR(hh_node_send(NULL, 2, payload, sizeof payload, f.vc.now), HH_ERR_INVAL);
    /* Node is only INITIALIZED so far: sending must not be allowed. */
    HH_ASSERT_ERR(hh_node_send(&f.node, 2, payload, sizeof payload, f.vc.now),
                  HH_ERR_STATE);

    hh_node_configure(&f.node, NULL);
    hh_node_start(&f.node);
    /* Now delegates to the forwarder; no route yet so it buffers. */
    HH_ASSERT_ERR(hh_node_send(&f.node, 2, payload, sizeof payload, f.vc.now),
                  HH_ERR_AGAIN);

    hh_node_stop(&f.node);
    HH_ASSERT_ERR(hh_node_send(&f.node, 2, payload, sizeof payload, f.vc.now),
                  HH_ERR_STATE);
}

static void test_send_boundary_payload_lengths(void)
{
    fix_t f; fix_init(&f, 1);
    uint8_t payload[HH_RADIO_MAX_FRAME];

    hh_node_configure(&f.node, NULL);
    hh_node_start(&f.node);

    /* Zero-length payload is a valid, empty send (e.g. a keepalive): the
     * forwarder only rejects a NULL payload paired with a nonzero length. It
     * still buffers pending a route, same as any other miss. */
    HH_ASSERT_ERR(hh_node_send(&f.node, 2, payload, 0, f.vc.now), HH_ERR_AGAIN);
    /* A NULL payload with a nonzero length is rejected. */
    HH_ASSERT_ERR(hh_node_send(&f.node, 2, NULL, 1, f.vc.now), HH_ERR_INVAL);
    /* A payload at/over the frame cap is rejected, not truncated. */
    HH_ASSERT_ERR(hh_node_send(&f.node, 2, payload, HH_RADIO_MAX_FRAME, f.vc.now),
                  HH_ERR_INVAL);
    /* Sending to ourselves is a local delivery, not a route lookup. */
    HH_ASSERT_OK(hh_node_send(&f.node, 1, payload, 1, f.vc.now));
    HH_ASSERT_EQ_INT(f.node.forwarder.delivered_local, 1);
}

/* ---------------- hh_node_on_frame: NULL guards ---------------- */

static void test_on_frame_ignores_null_arguments(void)
{
    fix_t f; fix_init(&f, 1);
    hh_frame_t fr;
    hh_link_sample_t s;
    memset(&fr, 0, sizeof fr);
    memset(&s, 0, sizeof s);

    /* Must not crash; must not be mistaken for a valid frame either. */
    hh_node_on_frame(NULL, &fr, &s);
    hh_node_on_frame(&f.node, NULL, &s);
    hh_node_on_frame(&f.node, &fr, NULL);
    HH_ASSERT_EQ_INT(f.node.route_updates_rx, 0);
}

/* ---------------- hh_node_on_frame: HH_FRAME_ROUTING (handle_route_update) ---------------- */

/* Build a routing-update frame as if received on the wire from `sender`. */
static hh_frame_t mk_routing_frame(hh_node_id_t sender, const hh_route_update_entry_t *entries,
                                   uint8_t count)
{
    hh_route_update_t u;
    hh_frame_t fr;
    memset(&u, 0, sizeof u);
    u.sender = sender;
    u.count = count;
    for (uint8_t i = 0; i < count; i++) u.entries[i] = entries[i];

    memset(&fr, 0, sizeof fr);
    fr.kind = HH_FRAME_ROUTING;
    fr.src  = sender;
    fr.len  = (uint16_t)hh_route_update_encode(&u, fr.data, sizeof fr.data);
    return fr;
}

static void establish_neighbor(fix_t *f, hh_node_id_t id, float rssi)
{
    hh_beacon_t b;
    hh_link_sample_t s;
    memset(&b, 0, sizeof b);
    b.node_id = id; b.sequence_no = 1; b.routing_capable = true;
    memset(&s, 0, sizeof s);
    s.neighbor_id = id; s.rssi = rssi; s.snr = 25.0f;
    hh_neighbor_on_beacon(&f->node.neighbors, &b, &s, f->vc.now);
    hh_link_health_add(&f->node.link_health, id, f->vc.now);
    for (int i = 0; i < 4; i++) hh_link_health_on_sample(&f->node.link_health, &s, f->vc.now);
    hh_routing_on_neighbor_up(&f->node.routing, id, f->vc.now);
}

static void test_routing_frame_installs_route_via_sender(void)
{
    fix_t f; fix_init(&f, 1);
    hh_route_update_entry_t e = { .originator = 9, .sequence_no = 20, .hop_count = 2,
                                  .metric = 0.1f };
    hh_frame_t fr;
    hh_link_sample_t m;
    memset(&m, 0, sizeof m);

    hh_node_configure(&f.node, NULL);
    hh_node_start(&f.node);
    establish_neighbor(&f, 2, -50.0f);

    fr = mk_routing_frame(2, &e, 1);
    hh_node_on_frame(&f.node, &fr, &m);

    HH_ASSERT_EQ_INT(f.node.route_updates_rx, 1);
    const hh_route_t *r = hh_routing_get(&f.node.routing, 9);
    HH_ASSERT(r != NULL);
    HH_ASSERT_EQ_INT(r->next_hop, 2);
    /* Distance vector: one more hop through the sender than advertised. */
    HH_ASSERT_EQ_INT(r->hop_count, 3);
}

static void test_routing_frame_from_self_is_ignored(void)
{
    fix_t f; fix_init(&f, 1);
    hh_route_update_entry_t e = { .originator = 9, .sequence_no = 20, .hop_count = 2,
                                  .metric = 0.1f };
    hh_frame_t fr;
    hh_link_sample_t m;
    memset(&m, 0, sizeof m);

    hh_node_configure(&f.node, NULL);
    hh_node_start(&f.node);

    /* A route update whose sender is our own node id -- an echo -- must not be
     * processed at all. */
    fr = mk_routing_frame(1, &e, 1);
    hh_node_on_frame(&f.node, &fr, &m);

    HH_ASSERT_EQ_INT(f.node.route_updates_rx, 0);
    HH_ASSERT(hh_routing_get(&f.node.routing, 9) == NULL);
}

static void test_routing_frame_entry_for_self_is_skipped(void)
{
    fix_t f; fix_init(&f, 1);
    /* A neighbor advertising a route back to us as its originator. */
    hh_route_update_entry_t e = { .originator = 1, .sequence_no = 20, .hop_count = 1,
                                  .metric = 0.0f };
    hh_frame_t fr;
    hh_link_sample_t m;
    memset(&m, 0, sizeof m);

    hh_node_configure(&f.node, NULL);
    hh_node_start(&f.node);
    establish_neighbor(&f, 2, -50.0f);

    fr = mk_routing_frame(2, &e, 1);
    hh_node_on_frame(&f.node, &fr, &m);

    /* The frame was consumed, but the self-originator entry must never install
     * a route to ourselves. */
    HH_ASSERT_EQ_INT(f.node.route_updates_rx, 1);
    HH_ASSERT(hh_routing_get(&f.node.routing, 1) == NULL);
}

static void test_routing_frame_hop_count_saturates_at_infinity(void)
{
    fix_t f; fix_init(&f, 1);
    /* Already at (or one below) infinity: adding the one-hop-through-sender
     * cost must saturate rather than wrap back to a small, attractive value. */
    hh_route_update_entry_t e = { .originator = 9, .sequence_no = 20,
                                  .hop_count = HH_HOP_INFINITY, .metric = 0.0f };
    hh_frame_t fr;
    hh_link_sample_t m;
    memset(&m, 0, sizeof m);

    hh_node_configure(&f.node, NULL);
    hh_node_start(&f.node);
    establish_neighbor(&f, 2, -50.0f);
    /* First install a real route so the poisoned update has something to
     * withdraw and the saturation path is observable. */
    hh_routing_offer(&f.node.routing, 9, 2, 19, 2, 0.0f, f.vc.now);
    HH_ASSERT(hh_routing_get(&f.node.routing, 9)->valid);

    fr = mk_routing_frame(2, &e, 1);
    hh_node_on_frame(&f.node, &fr, &m);

    HH_ASSERT_EQ_INT(f.node.route_updates_rx, 1);
    /* Poisoned reverse withdraws the route rather than it wrapping to a
     * falsely attractive near-zero hop count. */
    HH_ASSERT(!hh_routing_get(&f.node.routing, 9)->valid);
}

static void test_routing_frame_from_unknown_sender_rejected_by_routing(void)
{
    fix_t f; fix_init(&f, 1);
    /* Sender is not in our neighbor table: routing must validate the next hop
     * and reject rather than installing a route via a stranger. */
    hh_route_update_entry_t e = { .originator = 9, .sequence_no = 20, .hop_count = 2,
                                  .metric = 0.1f };
    hh_frame_t fr;
    hh_link_sample_t m;
    memset(&m, 0, sizeof m);

    hh_node_configure(&f.node, NULL);
    hh_node_start(&f.node);

    fr = mk_routing_frame(2, &e, 1);
    hh_node_on_frame(&f.node, &fr, &m);

    HH_ASSERT_EQ_INT(f.node.route_updates_rx, 1);
    HH_ASSERT(hh_routing_get(&f.node.routing, 9) == NULL);
}

static void test_malformed_routing_frame_is_dropped(void)
{
    fix_t f; fix_init(&f, 1);
    hh_frame_t fr;
    hh_link_sample_t m;
    memset(&fr, 0, sizeof fr);
    memset(&m, 0, sizeof m);

    hh_node_configure(&f.node, NULL);
    hh_node_start(&f.node);

    fr.kind = HH_FRAME_ROUTING;
    fr.len = 1;   /* shorter than a valid encoded update */
    hh_node_on_frame(&f.node, &fr, &m);

    /* Decode failure must return early without counting a received update. */
    HH_ASSERT_EQ_INT(f.node.route_updates_rx, 0);
}

/* ---------------- Telemetry topology formatter (untested elsewhere) ---------------- */

static void test_telemetry_format_topology_reports_tracked_nodes(void)
{
    fix_t f; fix_init(&f, 1);
    char buf[2048];

    hh_node_configure(&f.node, NULL);
    hh_node_start(&f.node);
    establish_neighbor(&f, 2, -50.0f);
    hh_topology_on_neighbor_up(&f.node.topology, 2, f.vc.now);
    hh_topology_on_route_installed(&f.node.topology, 9, 2, 2, f.vc.now);

    HH_ASSERT(hh_telemetry_format_topology(&f.node, buf, sizeof buf) > 0);
    HH_ASSERT(strstr(buf, "node=2") != NULL);
    HH_ASSERT(strstr(buf, "neighbor=1") != NULL);
    HH_ASSERT(strstr(buf, "node=9") != NULL);
    HH_ASSERT(strstr(buf, "hops=2") != NULL);
}

static void test_telemetry_format_topology_rejects_null_and_empty(void)
{
    fix_t f; fix_init(&f, 1);
    char buf[64];

    HH_ASSERT_EQ_INT(hh_telemetry_format_topology(NULL, buf, sizeof buf), 0);
    HH_ASSERT_EQ_INT(hh_telemetry_format_topology(&f.node, NULL, sizeof buf), 0);
    HH_ASSERT_EQ_INT(hh_telemetry_format_topology(&f.node, buf, 0), 0);
    /* No nodes tracked yet: an empty, valid string. */
    HH_ASSERT_EQ_INT(hh_telemetry_format_topology(&f.node, buf, sizeof buf), 0);
    HH_ASSERT_EQ_STR(buf, "");
}

HH_TEST_MAIN_BEGIN("node")
    HH_RUN(test_init_rejects_null_arguments);
    HH_RUN(test_init_rejects_invalid_config);
    HH_RUN(test_configure_rejects_null_and_wrong_state);
    HH_RUN(test_configure_with_null_cfg_keeps_current);
    HH_RUN(test_configure_rejects_invalid_config);
    HH_RUN(test_start_rejects_null_and_wrong_state);
    HH_RUN(test_stop_rejects_null_and_wrong_state);
    HH_RUN(test_release_rejects_null_and_is_idempotent_guard);
    HH_RUN(test_tick_rejects_null_and_not_running);
    HH_RUN(test_state_str_covers_every_state_and_unknown);
    HH_RUN(test_send_rejects_null_and_not_running);
    HH_RUN(test_send_boundary_payload_lengths);
    HH_RUN(test_on_frame_ignores_null_arguments);
    HH_RUN(test_routing_frame_installs_route_via_sender);
    HH_RUN(test_routing_frame_from_self_is_ignored);
    HH_RUN(test_routing_frame_entry_for_self_is_skipped);
    HH_RUN(test_routing_frame_hop_count_saturates_at_infinity);
    HH_RUN(test_routing_frame_from_unknown_sender_rejected_by_routing);
    HH_RUN(test_malformed_routing_frame_is_dropped);
    HH_RUN(test_telemetry_format_topology_reports_tracked_nodes);
    HH_RUN(test_telemetry_format_topology_rejects_null_and_empty);
HH_TEST_MAIN_END()
