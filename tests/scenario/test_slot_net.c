/*
 * Slot-timed multi-node scenarios: real hh_mac and hh_slot per node, simulated
 * air, stand-in routing. Virtual clock, no sleeps.
 */
#include "slot_net.h"
#include "hh_test.h"
#include <stdlib.h>
#include <string.h>

static slot_net_t *make(unsigned nodes, void (*tweak)(sn_config_t *))
{
    sn_config_t c;
    sn_config_default(&c, (uint16_t)nodes);
    if (tweak) tweak(&c);
    return sn_create(&c);
}

static bool identity(const sn_flow_result_t *r)
{
    return r->delivered + r->queue_drop + r->sched_reject + r->collision + r->channel +
           r->corruption + r->deadline == r->sent;
}

static void add_flow(slot_net_t *n, unsigned src, unsigned dst, unsigned size, unsigned every, unsigned start)
{
    sn_flow_t f = { (uint8_t)src, (uint8_t)dst, (uint16_t)size, every, start };
    sn_add_flow(n, &f);
}

static void test_create_validation(void)
{
    sn_config_t c;
    sn_config_default(&c, 1);
    HH_ASSERT(sn_create(&c) == NULL);
    sn_config_default(&c, 21);   /* more nodes than slots in the frame */
    HH_ASSERT(sn_create(&c) == NULL);
    sn_config_default(&c, 4);
    c.capacity = 10;
    HH_ASSERT(sn_create(&c) == NULL);
}

static void test_multihop_exclusive_ownership(void)
{
    sn_result_t r;
    slot_net_t *n = make(4, NULL);
    HH_ASSERT(n != NULL);
    sn_topology_line(n);
    add_flow(n, 1, 4, 40, 100, 500);
    sn_run(n, 5000, 1000);
    sn_result(n, &r);
    HH_ASSERT_EQ_INT(r.total.sent, 45);
    HH_ASSERT_EQ_INT(r.total.delivered, 45);
    HH_ASSERT_EQ_INT(r.total.hops_max, 3);
    HH_ASSERT_EQ_INT(r.collision_events, 0);
    HH_ASSERT_EQ_INT(r.tx_outside_own_slot, 0);
    HH_ASSERT(r.total.app_delay_ms[3] <= 60);   /* at most one frame per hop */
    HH_ASSERT(identity(&r.total));
    /* One PDU in every own slot of every node: 6 s * 50 frames/s * 4 nodes. */
    HH_ASSERT_EQ_INT(r.pdus, 6000 / 20 * 4);
    HH_ASSERT_EQ_INT(r.slot.own_slots_missed, 0);
    sn_destroy(n);
}

static void loss_tweak(sn_config_t *c) { c->loss_permille = 300; c->seed = 7; }

static void test_channel_loss_is_counted(void)
{
    sn_result_t r;
    slot_net_t *n = make(4, loss_tweak);
    HH_ASSERT(n != NULL);
    sn_topology_line(n);
    add_flow(n, 1, 4, 40, 50, 500);
    sn_run(n, 8000, 1000);
    sn_result(n, &r);
    HH_ASSERT(r.total.channel > 0);
    HH_ASSERT(r.total.delivered < r.total.sent);
    HH_ASSERT_EQ_INT(r.total.collision, 0);
    HH_ASSERT_EQ_INT(r.total.deadline, 0);
    HH_ASSERT(identity(&r.total));
    HH_ASSERT(r.channel_events > 0);
    sn_destroy(n);
}

static void corrupt_tweak(sn_config_t *c) { c->corrupt_permille = 500; c->seed = 3; }

static void test_corruption_is_counted(void)
{
    sn_result_t r;
    slot_net_t *n = make(2, corrupt_tweak);
    HH_ASSERT(n != NULL);
    sn_topology_line(n);
    add_flow(n, 1, 2, 40, 50, 100);
    sn_run(n, 4000, 500);
    sn_result(n, &r);
    HH_ASSERT(r.total.corruption > 0);
    HH_ASSERT(r.total.delivered > 0);
    HH_ASSERT(r.slot.rx_bad >= r.total.corruption);   /* the receiver's CRC check caught them */
    HH_ASSERT(identity(&r.total));
    sn_destroy(n);
}

static void tight_tweak(sn_config_t *c) { c->capacity = 70; }

static void test_routing_traffic_uses_slot_capacity(void)
{
    sn_result_t r;
    slot_net_t *n = make(2, tight_tweak);
    HH_ASSERT(n != NULL);
    sn_topology_line(n);
    /* 70 bytes leave room for one 45-byte message per PDU. A data message every
     * frame fills node 1's slot, so a routing message delays the data behind it. */
    add_flow(n, 1, 2, 40, 20, 100);
    sn_run(n, 10000, 1000);
    sn_result(n, &r);
    HH_ASSERT(r.slot.routing_bytes > 0);
    HH_ASSERT_EQ_INT(r.slot.routing_bytes % 45, 0);
    HH_ASSERT(r.total.app_delay_ms[3] >= r.total.app_delay_ms[0] + 20);
    HH_ASSERT(r.slot.control_bytes + r.slot.user_bytes + r.slot.overhead_bytes <= r.slot.capacity_bytes);
    HH_ASSERT(identity(&r.total));
    sn_destroy(n);
}

static void test_shared_slot_collides_only_where_heard(void)
{
    sn_result_t r;
    slot_net_t *n = make(4, NULL);

    /* Nodes 1 and 3 share slot 0 on a line: node 2 hears both. */
    HH_ASSERT(n != NULL);
    sn_topology_line(n);
    sn_set_slot(n, 3, 0);
    sn_run(n, 3000, 0);
    sn_result(n, &r);
    HH_ASSERT(r.collision_events > 0);
    sn_destroy(n);

    /* Nodes 1 and 4 share slot 0: no receiver hears both, so slot reuse is clean. */
    n = make(4, NULL);
    HH_ASSERT(n != NULL);
    sn_topology_line(n);
    sn_set_slot(n, 4, 0);
    sn_run(n, 3000, 0);
    sn_result(n, &r);
    HH_ASSERT_EQ_INT(r.collision_events, 0);
    sn_destroy(n);
}

static void test_collision_loss_is_classified(void)
{
    sn_result_t r;
    slot_net_t *n = make(3, NULL);
    HH_ASSERT(n != NULL);
    sn_topology_line(n);
    sn_set_slot(n, 3, 0);   /* nodes 1 and 3 collide at node 2 */
    add_flow(n, 1, 2, 40, 100, 500);
    sn_run(n, 4000, 500);
    sn_result(n, &r);
    HH_ASSERT(r.total.collision > 0);
    HH_ASSERT_EQ_INT(r.total.delivered + r.total.collision, r.total.sent);
    HH_ASSERT(identity(&r.total));
    sn_destroy(n);
}

static void test_reroute_after_link_cut(void)
{
    sn_result_t r;
    char *buf = NULL;
    size_t len = 0;
    sn_config_t c;
    slot_net_t *n;

    sn_config_default(&c, 4);
    c.log = open_memstream(&buf, &len);
    n = sn_create(&c);
    HH_ASSERT(n != NULL);
    sn_topology_ring(n);
    add_flow(n, 1, 3, 40, 50, 200);
    sn_schedule(n, 2000, SN_EV_LINK_DOWN, 1, 2);
    sn_schedule(n, 2200, SN_EV_REROUTE, 0, 0);
    sn_run(n, 6000, 1000);
    sn_result(n, &r);
    fflush(c.log);
    HH_ASSERT(r.total.channel > 0);                       /* sent into the dead link before the reroute */
    HH_ASSERT(r.total.delivered > r.total.sent * 8 / 10); /* and delivered again after it */
    HH_ASSERT(r.route_changes > 12);                      /* 12 installed at the start, then more */
    HH_ASSERT(strstr(buf, "ev=route_change") != NULL);
    HH_ASSERT(strstr(buf, "node=1 dst=3 old=2 new=4") != NULL);
    HH_ASSERT(strstr(buf, "ev=slot_tx") != NULL);
    HH_ASSERT(identity(&r.total));
    sn_destroy(n);
    fclose(c.log);
    free(buf);
}

static void small_tweak(sn_config_t *c) { c->capacity = 100; }

static void test_oversize_is_a_scheduler_rejection(void)
{
    sn_result_t r;
    slot_net_t *n = make(2, small_tweak);
    HH_ASSERT(n != NULL);
    sn_topology_line(n);
    add_flow(n, 1, 2, 80, 100, 200);   /* the largest packet at capacity 100 is 73 bytes */
    sn_run(n, 2000, 200);
    sn_result(n, &r);
    HH_ASSERT(r.total.sent > 0);
    HH_ASSERT_EQ_INT(r.total.sched_reject, r.total.sent);
    HH_ASSERT_EQ_INT(r.total.delivered, 0);
    sn_destroy(n);
}

static void big_tweak(sn_config_t *c) { c->slots_per_frame = 32; }

static void test_net_size_is_configurable(void)
{
    sn_result_t r;
    slot_net_t *n = make(32, big_tweak);
    HH_ASSERT(n != NULL);
    sn_topology_mesh(n);
    add_flow(n, 1, 32, 40, 100, 500);
    add_flow(n, 17, 5, 40, 100, 500);
    sn_run(n, 4000, 500);
    sn_result(n, &r);
    HH_ASSERT_EQ_INT(r.total.delivered, r.total.sent);
    HH_ASSERT_EQ_INT(r.collision_events, 0);
    HH_ASSERT_EQ_INT(r.tx_outside_own_slot, 0);
    HH_ASSERT_EQ_INT(r.total.hops_max, 1);
    sn_destroy(n);
}

HH_TEST_MAIN_BEGIN("slot_net")
    HH_RUN(test_create_validation);
    HH_RUN(test_multihop_exclusive_ownership);
    HH_RUN(test_channel_loss_is_counted);
    HH_RUN(test_corruption_is_counted);
    HH_RUN(test_routing_traffic_uses_slot_capacity);
    HH_RUN(test_shared_slot_collides_only_where_heard);
    HH_RUN(test_collision_loss_is_classified);
    HH_RUN(test_reroute_after_link_cut);
    HH_RUN(test_oversize_is_a_scheduler_rejection);
    HH_RUN(test_net_size_is_configurable);
HH_TEST_MAIN_END()
