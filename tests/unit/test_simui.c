/*
 * Tests for the simulation tooling itself (tools/sim).
 *
 * The simulator is only useful if what it prints is real. These tests verify
 * that its observability is derived from actual stack behavior:
 *  - the frame observer fires on frames the medium really moved
 *  - packet hop traces match the route the production forwarder chose
 *  - statistics agree with the production components' own counters
 *  - a seeded run is reproducible, and the seed genuinely matters
 *
 * A netsim_t holds full production state for up to 12 nodes and is roughly
 * 6.5 MB, so instances are file-scope statics rather than locals: two of them
 * on the stack exceeds the default 8 MB limit and crashes before any assertion
 * runs. (Found exactly that way while writing these tests.)
 */
#include "hh_test.h"
#include "simui.h"
#include <string.h>

static netsim_t g_sim_a;
static netsim_t g_sim_b;

static void build_line(netsim_t *s, size_t n, uint64_t seed)
{
    netsim_init(s, seed);
    for (size_t i = 0; i < n; i++) netsim_add_node(s, simui_id_from_index(i));
    for (size_t i = 0; i + 1 < n; i++)
        netsim_link_up(s, simui_id_from_index(i), simui_id_from_index(i + 1), -55.0f);
    netsim_start_all(s);
    netsim_run(s, 4000, 10);
}

static void test_node_naming(void)
{
    HH_ASSERT_EQ_STR(simui_name(1), "A");
    HH_ASSERT_EQ_STR(simui_name(4), "D");
    HH_ASSERT_EQ_STR(simui_name(26), "Z");
    /* Beyond the alphabet it must stay unambiguous rather than wrap. */
    HH_ASSERT_EQ_STR(simui_name(27), "#27");
    HH_ASSERT_EQ_INT(simui_id_from_index(0), 1);
    HH_ASSERT_EQ_INT(simui_id_from_index(3), 4);
}

static void test_traffic_traverses_real_multihop_path(void)
{
    netsim_t *s = &g_sim_a;
    simui_traffic_t t;

    build_line(s, 4, 11);
    HH_ASSERT(netsim_has_route(s, 1, 4));

    simui_traffic_init(&t, s, false);
    simui_send_burst(&t, 1, 4, 5, 60, 400);

    HH_ASSERT_EQ_INT(t.count, 5);
    for (size_t i = 0; i < t.count; i++) {
        const simui_packet_t *p = &t.packets[i];
        HH_ASSERT_MSG(p->delivered, "packet %u was not delivered", p->id);
        /* A -> B -> C -> D records three receive hops, the last being the
         * destination. This comes from the frame observer, so it is the path
         * the production forwarder actually used, not a modelled one. */
        HH_ASSERT_EQ_INT(p->hop_count, 3);
        HH_ASSERT_EQ_INT(p->hops[0], 2);
        HH_ASSERT_EQ_INT(p->hops[1], 3);
        HH_ASSERT_EQ_INT(p->hops[2], 4);
        HH_ASSERT(p->delivered_at >= p->sent_at);
    }
}

static void test_stats_agree_with_production_counters(void)
{
    netsim_t *s = &g_sim_a;
    simui_traffic_t t;
    uint64_t forwarded = 0, delivered_local = 0;
    size_t ok = 0;

    build_line(s, 3, 12);
    simui_traffic_init(&t, s, false);
    simui_send_burst(&t, 1, 3, 4, 60, 400);

    for (size_t i = 0; i < s->node_count; i++) {
        forwarded       += s->nodes[i].node.forwarder.forwarded;
        delivered_local += s->nodes[i].node.forwarder.delivered_local;
    }
    /* Each packet is transmitted by A and relayed by B (two forwarder actions)
     * and delivered locally once at C. If the simulator kept its own parallel
     * tally instead of reading the real counters, these would not line up. */
    HH_ASSERT_EQ_INT(forwarded, 8);
    HH_ASSERT_EQ_INT(delivered_local, 4);

    for (size_t i = 0; i < t.count; i++) if (t.packets[i].delivered) ok++;
    HH_ASSERT_EQ_INT(ok, 4);
}

static void test_no_route_is_reported_not_silently_counted(void)
{
    netsim_t *s = &g_sim_a;
    simui_traffic_t t;

    /* Two isolated nodes: no link, so no route can exist. */
    netsim_init(s, 13);
    netsim_add_node(s, 1);
    netsim_add_node(s, 2);
    netsim_start_all(s);
    netsim_run(s, 2000, 10);

    simui_traffic_init(&t, s, false);
    HH_ASSERT(!netsim_has_route(s, 1, 2));
    simui_send(&t, 1, 2);
    netsim_run(s, 500, 10);

    /* The packet is tracked as undelivered, and the production forwarder
     * recorded the miss in its own counter. */
    HH_ASSERT_EQ_INT(t.count, 1);
    HH_ASSERT(!t.packets[0].delivered);
    HH_ASSERT(netsim_node(s, 1)->node.forwarder.dropped_no_route > 0);
}

static void test_frame_observer_sees_losses(void)
{
    netsim_t *s = &g_sim_a;
    simui_traffic_t t;

    build_line(s, 2, 14);
    /* Total loss on the only link: frames are transmitted but never arrive. */
    netsim_link_set_loss(s, 1, 2, 1.0f);

    simui_traffic_init(&t, s, false);
    simui_send_burst(&t, 1, 2, 5, 60, 300);

    HH_ASSERT_MSG(t.frames_dropped_in_flight > 0,
                  "observer did not see the medium dropping frames");
    for (size_t i = 0; i < t.count; i++)
        HH_ASSERT(!t.packets[i].delivered);
}

static void test_seeded_runs_are_reproducible(void)
{
    netsim_t *a = &g_sim_a, *b = &g_sim_b;
    simui_traffic_t ta, tb;

    /* Identical seed and identical inputs must produce identical outcomes,
     * which is what makes a simulator failure debuggable. */
    build_line(a, 3, 4242);
    netsim_link_set_loss(a, 1, 2, 0.25f);
    simui_traffic_init(&ta, a, false);
    simui_send_burst(&ta, 1, 3, 8, 60, 500);

    build_line(b, 3, 4242);
    netsim_link_set_loss(b, 1, 2, 0.25f);
    simui_traffic_init(&tb, b, false);
    simui_send_burst(&tb, 1, 3, 8, 60, 500);

    HH_ASSERT_EQ_INT(ta.count, tb.count);
    HH_ASSERT_EQ_INT(a->frames_dropped, b->frames_dropped);
    HH_ASSERT_EQ_INT(ta.frames_dropped_in_flight, tb.frames_dropped_in_flight);
    for (size_t i = 0; i < ta.count; i++) {
        HH_ASSERT_EQ_INT(ta.packets[i].delivered, tb.packets[i].delivered);
        HH_ASSERT_EQ_INT(ta.packets[i].hop_count, tb.packets[i].hop_count);
        HH_ASSERT_EQ_INT(ta.packets[i].delivered_at, tb.packets[i].delivered_at);
    }
}

static void test_seed_actually_drives_the_loss_pattern(void)
{
    netsim_t *s = &g_sim_a;
    const uint64_t seeds[] = { 1, 3, 31337, 555 };
    uint64_t drops[4];
    bool any_different = false;

    /*
     * The seed must genuinely drive the loss pattern, otherwise reproducibility
     * would hold by accident rather than by construction.
     *
     * Asserting that two specific seeds differ is flaky -- two arbitrary seeds
     * can legitimately produce the same drop count. Sampling several and
     * requiring that they are not ALL identical tests the real property
     * without depending on any particular pair.
     */
    for (size_t k = 0; k < 4; k++) {
        build_line(s, 3, seeds[k]);
        netsim_link_set_loss(s, 1, 2, 0.5f);
        netsim_run(s, 2000, 10);
        drops[k] = s->frames_dropped;
    }
    for (size_t k = 1; k < 4; k++) if (drops[k] != drops[0]) any_different = true;

    HH_ASSERT_MSG(any_different,
                  "every seed produced identical loss (%llu); seed is being ignored",
                  (unsigned long long)drops[0]);
    /* And loss really was applied at all. */
    HH_ASSERT(drops[0] > 0);
}

static void test_step_observer_and_single_step(void)
{
    netsim_t *s = &g_sim_a;
    hh_time_ms_t before;

    netsim_init(s, 15);
    netsim_add_node(s, 1);
    netsim_add_node(s, 2);
    netsim_link_up(s, 1, 2, -55.0f);
    netsim_start_all(s);

    before = s->vc.now;
    netsim_step(s, 10);
    /* One step advances virtual time by exactly one interval. */
    HH_ASSERT_EQ_INT(s->vc.now, before + 10);

    /* Stepping repeatedly reaches the same state netsim_run would. */
    for (int i = 0; i < 200; i++) netsim_step(s, 10);
    HH_ASSERT(netsim_is_neighbor(s, 1, 2));
}

static void test_events_only_come_from_the_stack(void)
{
    netsim_t *s = &g_sim_a;
    simui_events_t ev;
    uint64_t seen;

    netsim_init(s, 16);
    netsim_add_node(s, 1);
    netsim_add_node(s, 2);
    netsim_link_up(s, 1, 2, -55.0f);
    netsim_start_all(s);

    simui_events_attach(&ev, s, false);   /* attached, but not printing */
    simui_events_refresh();               /* node configure reset the level */
    netsim_run(s, 2000, 10);
    HH_ASSERT_MSG(ev.count > 0, "no log records captured from a running stack");

    simui_events_detach();
    seen = ev.count;
    netsim_run(s, 2000, 10);
    /* After detaching, the counter cannot advance: the simulator has no
     * independent source of events, it only observes the production log. */
    HH_ASSERT_EQ_INT(ev.count, seen);
}

HH_TEST_MAIN_BEGIN("simui")
    HH_RUN(test_node_naming);
    HH_RUN(test_traffic_traverses_real_multihop_path);
    HH_RUN(test_stats_agree_with_production_counters);
    HH_RUN(test_no_route_is_reported_not_silently_counted);
    HH_RUN(test_frame_observer_sees_losses);
    HH_RUN(test_seeded_runs_are_reproducible);
    HH_RUN(test_seed_actually_drives_the_loss_pattern);
    HH_RUN(test_step_observer_and_single_step);
    HH_RUN(test_events_only_come_from_the_stack);
HH_TEST_MAIN_END()
