/*
 * Slot-timed multi-node scenarios: real hh_mac and hh_slot per node, simulated
 * air, stand-in routing. Virtual clock, no sleeps.
 */
#include "slot_net.h"
#include "hh_test.h"
#include <stdlib.h>
#include <string.h>

/* Creates a network with the default configuration, optionally changed by tweak. */
static slot_net_t *make(unsigned nodes, void (*tweak)(sn_config_t *))
{
    sn_config_t c;
    sn_config_default(&c, (uint16_t)nodes);
    if (tweak) tweak(&c);
    return sn_create(&c);
}

/* Checks that delivered plus every loss category equals packets sent. */
static bool identity(const sn_flow_result_t *r)
{
    return r->delivered + r->queue_drop + r->sched_reject + r->collision + r->channel +
           r->corruption + r->deadline == r->sent;
}

/* Adds one traffic flow to the network. */
static void add_flow(slot_net_t *n, unsigned src, unsigned dst, unsigned size, unsigned every, unsigned start)
{
    sn_flow_t f = { (uint8_t)src, (uint8_t)dst, (uint16_t)size, every, start, 0 };
    sn_add_flow(n, &f);
}

/* Invalid network configurations are refused. */
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

/* Traffic crosses three hops with one owner per slot: no collisions, nothing outside own slots. */
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

/* Configuration change: 30 percent link loss. */
static void loss_tweak(sn_config_t *c) { c->loss_permille = 300; c->seed = 7; }

/* Link loss is counted as channel loss and the totals add up. */
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

/* Configuration change: half of the PDUs arrive corrupted. */
static void corrupt_tweak(sn_config_t *c) { c->corrupt_permille = 500; c->seed = 3; }

/* Corrupted PDUs are caught by the CRC check and counted. */
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

/* Configuration change: a capacity that fits only one message per PDU. */
static void tight_tweak(sn_config_t *c) { c->capacity = 75; }

/* A routing message takes a slot from data traffic and delays it. */
static void test_routing_traffic_uses_slot_capacity(void)
{
    sn_result_t r;
    slot_net_t *n = make(2, tight_tweak);
    HH_ASSERT(n != NULL);
    sn_topology_line(n);
    /* 75 bytes (27 of overhead) leave room for one 45-byte message per PDU. A data message every
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

/* A shared slot collides only at a receiver that hears both senders. */
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

/* Packets lost to a collision are counted as collision loss. */
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

/* After a link is cut, packets are lost until the reroute, then delivered by the other path. */
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

/* Configuration change: capacity 100. */
static void small_tweak(sn_config_t *c) { c->capacity = 100; }

/* A packet larger than one PDU is refused and counted as a scheduler rejection. */
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

/* Configuration change: 32 slots per frame. */
static void big_tweak(sn_config_t *c) { c->slots_per_frame = 32; }

/* A 32-node network works with 32 slots per frame. */
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

/* Every other node's demand table follows what a sender still has queued after each own slot. */
static void test_demand_follows_sender_queue(void)
{
    sn_config_t c;
    slot_net_t *n;
    uint8_t payload[60] = { 0 };
    const hh_alloc_demand_t *d;

    sn_config_default(&c, 3);
    c.hello_interval_ms = 0;
    c.tc_interval_ms = 0;
    n = sn_create(&c);
    HH_ASSERT(n != NULL);
    sn_topology_mesh(n);
    for (int i = 0; i < 10; i++)
        HH_ASSERT_EQ_INT(sn_send(n, 1, HH_FRAME_DATA, HH_MAC_CLASS_DATA, 2, payload, 60), HH_MAC_ACCEPTED);

    sn_set_time(n, 0);
    sn_step(n);   /* slot 0: node 1 sends one 65-byte message, nine stay queued: ten offered */
    for (unsigned heard = 2; heard <= 3; heard++) {
        d = hh_alloc_find(sn_demand(n, heard), 1);
        HH_ASSERT(d != NULL);
        HH_ASSERT_EQ_INT(d->depth[HH_MAC_CLASS_DATA], 10);
        HH_ASSERT_EQ_INT(d->depth[HH_MAC_CLASS_VOICE], 0);
        HH_ASSERT_EQ_INT((int)d->seen_slot, 0);
    }
    d = hh_alloc_find(sn_demand(n, 1), 1);   /* a node keeps its own last report too */
    HH_ASSERT(d != NULL);
    HH_ASSERT_EQ_INT(d->depth[HH_MAC_CLASS_DATA], 10);

    for (uint32_t t = 1; t < 200; t++) {   /* node 1 sends one message per frame, the last one at slot 180 */
        sn_set_time(n, t);
        sn_step(n);
    }
    d = hh_alloc_find(sn_demand(n, 2), 1);
    HH_ASSERT(d != NULL);
    HH_ASSERT_EQ_INT(d->depth[HH_MAC_CLASS_DATA], 1);   /* nothing left, one message sent in the last frame */
    HH_ASSERT_EQ_INT((int)d->seen_slot, 180);
    d = hh_alloc_find(sn_demand(n, 2), 3);   /* an idle node reports zero in its own slot */
    HH_ASSERT(d != NULL);
    HH_ASSERT_EQ_INT(d->depth[HH_MAC_CLASS_DATA], 0);
    HH_ASSERT_EQ_INT((int)d->seen_slot, 182);
    sn_destroy(n);
}

typedef struct {
    unsigned    slots[5];       /* slots owned by nodes 1 to 4 at the end; index 0 unused */
    uint64_t    owned;          /* union of the slot masks */
    uint64_t    overlap;        /* slots owned by two nodes */
    sn_result_t r;
    uint64_t    clashes, changes, no_slot, errors;
} greedy_t;

/* Four nodes. Node 1 sends 1000 packets a second, far more than any slot share can carry. Node 2 sends data,
 * node 3 voice and node 4 video, each one message a frame. Runs with the given per-node cap, 0 for none.
 * The run ends before the superframe boundary at 5000 ms, so the map applied at 4000 ms is the one counted. */
static void greedy_run(unsigned cap, uint32_t loss_permille, greedy_t *g)
{
    sn_config_t c;
    hh_alloc_params_t ap;
    slot_net_t *n;
    sn_flow_t voice = { 3, 4, 24, 20, 0, HH_MAC_CLASS_VOICE };
    sn_flow_t video = { 4, 1, 40, 20, 0, HH_MAC_CLASS_VIDEO };

    memset(g, 0, sizeof *g);
    hh_alloc_params_default(&ap, 20);
    ap.node_cap = (uint8_t)cap;
    ap.join_reserve = 0;
    sn_config_default(&c, 4);
    c.hello_interval_ms = 0;
    c.tc_interval_ms = 0;
    c.alloc = &ap;
    c.loss_permille = loss_permille;
    n = sn_create(&c);
    HH_ASSERT(n != NULL);
    sn_topology_mesh(n);
    add_flow(n, 1, 2, 60, 1, 0);    /* flow 0: the greedy node */
    add_flow(n, 2, 3, 40, 20, 0);   /* flow 1: data */
    sn_add_flow(n, &voice);         /* flow 2: voice */
    sn_add_flow(n, &video);         /* flow 3: video */
    sn_run(n, 4500, 400);
    sn_result(n, &g->r);
    for (unsigned node = 1; node <= 4; node++) {
        uint64_t m = sn_own_slots(n, node);
        g->overlap |= m & g->owned;
        g->owned |= m;
        for (uint64_t b = m; b; b &= b - 1u) g->slots[node]++;
        g->clashes += sn_slot_stats(n, node)->alloc_clashes;
        g->changes += sn_slot_stats(n, node)->alloc_changes;
        g->no_slot += sn_slot_stats(n, node)->alloc_no_slot;
        g->errors += sn_slot_stats(n, node)->alloc_errors;
    }
    sn_destroy(n);
}

/* Checks what must hold in every greedy run: one owner per slot, no transmission outside an own slot, no
 * collision, no clash, and every message of the three modest flows delivered. */
static void greedy_common(const greedy_t *g)
{
    HH_ASSERT_EQ_INT((int)g->overlap, 0);
    HH_ASSERT_EQ_INT((int)g->r.tx_outside_own_slot, 0);
    HH_ASSERT_EQ_INT((int)g->r.collision_events, 0);
    HH_ASSERT_EQ_INT((int)g->clashes, 0);
    HH_ASSERT(g->changes >= 1);
    for (unsigned f = 1; f <= 3; f++) {
        HH_ASSERT(g->r.flow[f].sent > 100);
        HH_ASSERT_EQ_INT((int)g->r.flow[f].delivered, (int)g->r.flow[f].sent);
    }
    for (unsigned node = 2; node <= 4; node++) HH_ASSERT(g->slots[node] >= 1);
}

/* Without a cap the greedy node takes every slot the others do not ask for: all nodes reach the same map. */
static void test_allocation_follows_demand(void)
{
    greedy_t g;

    greedy_run(0, 0, &g);
    greedy_common(&g);
    HH_ASSERT_EQ_INT((int)g.owned, 0xFFFFF);   /* all 20 slots are owned */
    HH_ASSERT_EQ_INT(g.slots[1], 17);
    for (unsigned node = 2; node <= 4; node++) HH_ASSERT_EQ_INT(g.slots[node], 1);
    printf("greedy_node cap=none greedy_slots=%u others=%u,%u,%u\n", g.slots[1], g.slots[2], g.slots[3], g.slots[4]);
}

/* With the cap the greedy node is held at it, the freed slots go to the others, and their traffic is carried. */
static void test_greedy_node_is_held_at_the_cap(void)
{
    greedy_t g;

    greedy_run(8, 0, &g);
    greedy_common(&g);
    HH_ASSERT_EQ_INT((int)g.owned, 0xFFFFF);
    HH_ASSERT_EQ_INT(g.slots[1], 8);
    for (unsigned node = 2; node <= 4; node++) HH_ASSERT_EQ_INT(g.slots[node], 4);
    printf("greedy_node cap=8 greedy_slots=%u others=%u,%u,%u delivered=%u,%u,%u of %u,%u,%u\n", g.slots[1],
           g.slots[2], g.slots[3], g.slots[4], g.r.flow[1].delivered, g.r.flow[2].delivered, g.r.flow[3].delivered,
           g.r.flow[1].sent, g.r.flow[2].sent, g.r.flow[3].sent);
}

/* With steady demand the maps stay the same at every node while up to half of all PDUs are lost, because every
 * node repeats its report each frame and the table keeps the latest. The end state is one common, complete map. */
static void test_allocation_under_loss(void)
{
    static const uint32_t loss[] = { 0, 50, 100, 200, 300, 500 };

    for (unsigned i = 0; i < sizeof loss / sizeof loss[0]; i++) {
        greedy_t g;
        greedy_run(8, loss[i], &g);
        HH_ASSERT_EQ_INT((int)g.overlap, 0);
        HH_ASSERT_EQ_INT((int)g.owned, 0xFFFFF);
        HH_ASSERT_EQ_INT((int)g.r.tx_outside_own_slot, 0);
        HH_ASSERT_EQ_INT(g.slots[1], 8);                    /* the cap holds the greedy node */
        for (unsigned node = 2; node <= 4; node++) HH_ASSERT(g.slots[node] >= 1);
        printf("loss_run loss_permille=%u slots=%u,%u,%u,%u overlap=0x%llx owned=0x%llx collisions=%llu clashes=%llu "
               "changes=%llu no_slot=%llu errors=%llu delivered=%u,%u,%u of %u,%u,%u\n",
               loss[i], g.slots[1], g.slots[2], g.slots[3], g.slots[4], (unsigned long long)g.overlap,
               (unsigned long long)g.owned, (unsigned long long)g.r.collision_events, (unsigned long long)g.clashes,
               (unsigned long long)g.changes, (unsigned long long)g.no_slot, (unsigned long long)g.errors,
               g.r.flow[1].delivered, g.r.flow[2].delivered, g.r.flow[3].delivered, g.r.flow[1].sent,
               g.r.flow[2].sent, g.r.flow[3].sent);
    }
}

typedef struct {
    unsigned slots[5];
    uint64_t owned, overlap, clashes, no_slot, changes, collisions, outside;
} partition_t;

/* Node 1 floods from the start. Node 4 starts flooding at 2500 ms and, if cut is set, loses every link from 2600
 * to 3600 ms, so for a superframe it allocates on reports it can no longer refresh, and the others on its old
 * ones. The run ends at 8900 ms, five superframes after the links return. */
static void partition_run(bool cut, partition_t *out)
{
    sn_config_t c;
    hh_alloc_params_t ap;
    slot_net_t *n;
    sn_result_t r;
    sn_flow_t flood4 = { 4, 1, 60, 1, 2500, 0 };

    memset(out, 0, sizeof *out);
    hh_alloc_params_default(&ap, 20);
    ap.join_reserve = 0;
    sn_config_default(&c, 4);
    c.hello_interval_ms = 0;
    c.tc_interval_ms = 0;
    c.alloc = &ap;
    n = sn_create(&c);
    HH_ASSERT(n != NULL);
    sn_topology_mesh(n);
    add_flow(n, 1, 2, 60, 1, 0);
    add_flow(n, 2, 3, 40, 20, 0);
    sn_add_flow(n, &flood4);
    for (unsigned a = 1; a <= 3 && cut; a++) {
        sn_schedule(n, 2600, SN_EV_LINK_DOWN, a, 4);
        sn_schedule(n, 3600, SN_EV_LINK_UP, a, 4);
    }
    sn_run(n, 8500, 400);
    sn_result(n, &r);
    for (unsigned node = 1; node <= 4; node++) {
        uint64_t m = sn_own_slots(n, node);
        out->overlap |= m & out->owned;
        out->owned |= m;
        for (uint64_t b = m; b; b &= b - 1u) out->slots[node]++;
        out->clashes += sn_slot_stats(n, node)->alloc_clashes;
        out->no_slot += sn_slot_stats(n, node)->alloc_no_slot;
        out->changes += sn_slot_stats(n, node)->alloc_changes;
    }
    out->collisions = r.collision_events;
    out->outside = r.tx_outside_own_slot;
    sn_destroy(n);
}

/* A node that is cut off allocates on stale data, the others on its old reports. The clash rule settles the
 * overlap that follows, and once the links return every node is back to one common map. */
static void test_partition_recovers(void)
{
    partition_t cut, whole;

    partition_run(false, &whole);
    partition_run(true, &cut);
    printf("partition cut=no  slots=%u,%u,%u,%u overlap=0x%llx owned=0x%llx collisions=%llu clashes=%llu changes=%llu\n",
           whole.slots[1], whole.slots[2], whole.slots[3], whole.slots[4], (unsigned long long)whole.overlap,
           (unsigned long long)whole.owned, (unsigned long long)whole.collisions, (unsigned long long)whole.clashes,
           (unsigned long long)whole.changes);
    printf("partition cut=yes slots=%u,%u,%u,%u overlap=0x%llx owned=0x%llx collisions=%llu clashes=%llu changes=%llu\n",
           cut.slots[1], cut.slots[2], cut.slots[3], cut.slots[4], (unsigned long long)cut.overlap,
           (unsigned long long)cut.owned, (unsigned long long)cut.collisions, (unsigned long long)cut.clashes,
           (unsigned long long)cut.changes);

    HH_ASSERT_EQ_INT((int)whole.clashes, 0);   /* without the cut the maps never disagree */
    HH_ASSERT(cut.clashes >= 1);               /* with it they do, and the rule fires */
    for (int i = 0; i < 2; i++) {
        const partition_t *p = i ? &cut : &whole;
        HH_ASSERT_EQ_INT((int)p->overlap, 0);          /* in the end no slot has two owners */
        HH_ASSERT_EQ_INT((int)p->owned, 0xFFFFF);      /* and every slot has one */
        HH_ASSERT_EQ_INT((int)p->outside, 0);
        for (unsigned node = 1; node <= 4; node++) HH_ASSERT(p->slots[node] >= 1);
        HH_ASSERT(p->slots[1] <= 8 && p->slots[4] <= 8);   /* the cap held for both greedy nodes */
    }
    HH_ASSERT_EQ_INT((int)(cut.slots[1] + cut.slots[2] + cut.slots[3] + cut.slots[4]), 20);
}

typedef struct {
    unsigned nodes, joiners, join_reserve;
    uint32_t loss_permille;
    uint32_t run_ms;
} entry_cfg_t;

typedef struct {
    unsigned joined;                 /* joiners that sent their first burst */
    uint64_t owned, overlap;
    unsigned count[SN_MAX_NODES + 1];
    uint32_t no_free;
    uint64_t errors, tx_outside;
} entry_res_t;

/* Builds a net of members and joiners with allocation and entry both on and one greedy member. */
static slot_net_t *entry_net(const entry_cfg_t *e, hh_alloc_params_t *ap)
{
    sn_config_t c;
    slot_net_t *n;

    hh_alloc_params_default(ap, 20);
    ap->node_cap = 0;
    ap->join_reserve = (uint8_t)e->join_reserve;
    sn_config_default(&c, (uint16_t)e->nodes);
    c.hello_interval_ms = 0;
    c.tc_interval_ms = 0;
    c.entry_slots = 1ull << 19;
    c.joiners = e->joiners;
    c.alloc = ap;
    c.loss_permille = e->loss_permille;
    n = sn_create(&c);
    if (!n) return NULL;
    sn_topology_mesh(n);
    add_flow(n, 1, 2, 60, 4, 0);   /* the greedy member: five messages a frame, within the packet limit of the sim */
    add_flow(n, 2, 3, 40, 20, 0);
    return n;
}

/* Reads the slot sets, the joins and the counters of every node. */
static void entry_read(slot_net_t *n, unsigned nodes, entry_res_t *r)
{
    memset(r, 0, sizeof *r);
    for (unsigned node = 1; node <= nodes; node++) {
        uint64_t m = sn_own_slots(n, node);
        r->overlap |= m & r->owned;
        r->owned |= m;
        for (uint64_t x = m; x; x &= x - 1u) r->count[node]++;
        r->errors += sn_slot_stats(n, node)->alloc_errors;
        r->no_free += sn_entry(n, node)->st.no_free_slot;
        if (sn_entry(n, node)->st.joined) r->joined++;
    }
}

/* Runs the net from the start, all joiners starting at once. */
static void entry_run(const entry_cfg_t *e, entry_res_t *r)
{
    hh_alloc_params_t ap;
    slot_net_t *n = entry_net(e, &ap);

    HH_ASSERT(n != NULL);
    for (unsigned j = e->nodes - e->joiners + 1; j <= e->nodes; j++) HH_ASSERT_OK(sn_join(n, j));
    sn_advance(n, e->run_ms);
    entry_read(n, e->nodes, r);
    sn_destroy(n);
}

/* A radio joins while the allocator runs: it is granted a slot from the idle reserve, and then takes its share. */
static void test_entry_with_allocation(void)
{
    entry_cfg_t e = { 4, 1, 1, 0, 12000 };
    entry_res_t r;

    entry_run(&e, &r);
    HH_ASSERT_EQ_INT((int)r.joined, 1);
    HH_ASSERT_EQ_INT((int)r.overlap, 0);
    HH_ASSERT_EQ_INT((int)r.errors, 0);
    HH_ASSERT((r.owned >> 19) == 0);   /* the entry slot is never owned */
    for (unsigned node = 1; node <= 4; node++) HH_ASSERT(r.count[node] >= 1);
    printf("entry_alloc joined=1 slots=%u,%u,%u,%u overlap=0\n", r.count[1], r.count[2], r.count[3], r.count[4]);
}

/* With no idle slot the map owns every slot, so the members have none to grant and the joiner is refused. */
static void test_entry_needs_the_reserve(void)
{
    entry_cfg_t e = { 4, 1, 0, 0, 12000 };
    entry_res_t r;

    entry_run(&e, &r);
    HH_ASSERT_EQ_INT((int)r.joined, 0);
    HH_ASSERT(r.no_free > 0);
    HH_ASSERT_EQ_INT((int)r.overlap, 0);
    printf("entry_alloc join_reserve=0 joined=0 refused_full=%u\n", r.no_free);
}

/* Three radios join at once, over a lossy channel. Each loss rate must end with every joiner in, no slot owned twice. */
static void test_entry_several_joiners_under_loss(void)
{
    static const uint32_t loss[] = { 0, 100, 250 };

    for (unsigned i = 0; i < sizeof loss / sizeof loss[0]; i++) {
        entry_cfg_t e = { 7, 3, 3, loss[i], 40000 };
        entry_res_t r;

        entry_run(&e, &r);
        HH_ASSERT_EQ_INT((int)r.joined, 3);
        HH_ASSERT_EQ_INT((int)r.overlap, 0);
        HH_ASSERT_EQ_INT((int)r.errors, 0);
        for (unsigned node = 1; node <= 7; node++) HH_ASSERT(r.count[node] >= 1);
        printf("entry_alloc joiners=3 loss=%u joined=%u overlap=0 slots=%u,%u,%u,%u,%u,%u,%u\n", loss[i], r.joined,
               r.count[1], r.count[2], r.count[3], r.count[4], r.count[5], r.count[6], r.count[7]);
    }
}

/* A member leaves and later joins again while the allocator runs: its slots are freed, then it takes a share again. */
static void test_entry_leave_and_rejoin_with_allocation(void)
{
    entry_cfg_t e = { 5, 1, 2, 0, 0 };
    hh_alloc_params_t ap;
    slot_net_t *n = entry_net(&e, &ap);
    entry_res_t r;

    HH_ASSERT(n != NULL);

    HH_ASSERT_OK(sn_join(n, 5));
    sn_advance(n, 12000);
    HH_ASSERT(sn_entry(n, 5)->st.joined);
    HH_ASSERT(sn_own_slots(n, 5) != 0);

    HH_ASSERT_OK(sn_leave(n, 5));
    sn_advance(n, 15000);
    entry_read(n, 5, &r);
    HH_ASSERT_EQ_INT(hh_entry_state(sn_entry(n, 5)), HH_ENTRY_OFF);
    HH_ASSERT_EQ_INT((int)sn_own_slots(n, 5), 0);
    HH_ASSERT_EQ_INT((int)r.overlap, 0);
    HH_ASSERT_EQ_INT((int)r.errors, 0);
    for (unsigned node = 1; node <= 4; node++) HH_ASSERT(r.count[node] >= 1);

    HH_ASSERT_OK(sn_join(n, 5));
    sn_advance(n, 20000);
    entry_read(n, 5, &r);
    HH_ASSERT_EQ_INT(hh_entry_state(sn_entry(n, 5)), HH_ENTRY_MEMBER);
    HH_ASSERT(r.count[5] >= 1);
    HH_ASSERT_EQ_INT((int)r.overlap, 0);
    HH_ASSERT_EQ_INT((int)r.errors, 0);
    printf("entry_alloc leave_rejoin back_in=1 slots=%u,%u,%u,%u,%u overlap=0\n", r.count[1], r.count[2], r.count[3],
           r.count[4], r.count[5]);
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
    HH_RUN(test_demand_follows_sender_queue);
    HH_RUN(test_allocation_follows_demand);
    HH_RUN(test_greedy_node_is_held_at_the_cap);
    HH_RUN(test_allocation_under_loss);
    HH_RUN(test_partition_recovers);
    HH_RUN(test_entry_with_allocation);
    HH_RUN(test_entry_needs_the_reserve);
    HH_RUN(test_entry_several_joiners_under_loss);
    HH_RUN(test_entry_leave_and_rejoin_with_allocation);
HH_TEST_MAIN_END()
