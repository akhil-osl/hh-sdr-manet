/*
 * Network entry in the slot-timed network: real hh_mac, hh_slot and hh_entry
 * per node, simulated air where two senders in one slot destroy each other's
 * PDU at the receivers. Several nodes join at once. Each scenario prints one
 * key=value line with the join time and the collision count. Virtual clock,
 * no sleeps.
 */
#include "slot_net.h"
#include "hhsdr/core/log.h"
#include "hh_test.h"
#include <stdlib.h>
#include <string.h>

#define MAX_RUN_MS 120000u

typedef struct {
    const char *name;
    unsigned    nodes, joiners;
    unsigned    slots_per_frame;
    uint32_t    seed;
    uint32_t    loss_permille;
    const hh_entry_params_t *params;   /* NULL = the proposed values */
} scn_t;

typedef struct {
    uint32_t requests;          /* entry requests sent */
    uint32_t collision_slots;   /* entry slots with two or more requests */
    uint32_t collided_requests;
    uint32_t cur_t, cur_n;
    bool     have;
} track_t;

typedef struct {
    unsigned joined;
    uint32_t join_ms[SN_MAX_NODES];     /* per joiner, from the start of its entry to its first burst */
    uint32_t grant_ms[SN_MAX_NODES];    /* per joiner, from the start of its entry to the accepted grant */
    uint32_t all_joined_ms;             /* when the last joiner sent its first burst */
    uint32_t requests, collision_slots, collided_requests, timeouts, cycles, ignored;
    uint64_t tx_outside_own_slot;
    bool     slots_unique;
} res_t;

/* Closes the count of the slot being tracked. */
static void flush(track_t *k)
{
    if (k->have && k->cur_n >= 2) {
        k->collision_slots++;
        k->collided_requests += k->cur_n;
    }
    k->have = false;
    k->cur_n = 0;
}

/* Called for every PDU sent: counts entry requests and the slots in which two or more collide. */
static void hook(void *ctx, uint32_t t, const uint8_t *pdu, size_t len)
{
    track_t *k = ctx;
    hh_pdu_t p;
    unsigned reqs = 0;

    if (hh_pdu_decode(pdu, len, &p) != HH_OK) return;
    for (unsigned i = 0; i < p.n_msg; i++)
        if (p.msg[i].proto == HH_PDU_PROTO_COMMAND && p.msg[i].len && p.msg[i].data[0] == HH_ENTRY_CMD_REQUEST) reqs++;
    if (!reqs) return;
    if (k->have && t != k->cur_t) flush(k);
    k->cur_t = t;
    k->have = true;
    k->cur_n += reqs;
    k->requests += reqs;
}

/* Builds the network: the last `joiners` nodes start without a slot, one shared entry slot per frame. */
static slot_net_t *build(const scn_t *s, track_t *trk)
{
    sn_config_t c;
    slot_net_t *n;

    sn_config_default(&c, (uint16_t)s->nodes);
    c.slots_per_frame = (uint16_t)s->slots_per_frame;
    c.seed = s->seed;
    c.loss_permille = s->loss_permille;
    c.hello_interval_ms = 0;
    c.tc_interval_ms = 0;
    c.entry_slots = 1ull << (s->slots_per_frame - 1u);
    c.joiners = s->joiners;
    c.entry_params = s->params;
    c.pdu_hook = hook;
    c.hook_ctx = trk;
    n = sn_create(&c);
    if (n) sn_topology_mesh(n);   /* every radio hears every other */
    return n;
}

/* Orders two 32-bit values for qsort. */
static int cmp_u32(const void *a, const void *b)
{
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return x < y ? -1 : x > y;
}

/* Returns true if every pair of nodes owns different slots, none of them the entry slot. */
static bool slots_unique(const slot_net_t *n, unsigned nodes, uint64_t entry)
{
    uint64_t seen = entry;
    for (unsigned i = 1; i <= nodes; i++) {
        uint64_t m = sn_own_slots(n, i);
        if (seen & m) return false;
        seen |= m;
    }
    return true;
}

/* Starts every joiner at time 0 and runs until all have sent their first burst, or the limit. */
static void run(slot_net_t *n, const scn_t *s, track_t *trk, uint32_t limit_ms, res_t *r)
{
    unsigned first = s->nodes - s->joiners + 1u;
    uint32_t t, tail = 0;

    memset(r, 0, sizeof *r);
    for (t = 0; t < limit_ms; t++) {
        sn_set_time(n, t);
        if (t == 0)
            for (unsigned i = first; i <= s->nodes; i++) HH_ASSERT_OK(sn_join(n, i));
        sn_step(n);
        r->joined = 0;
        for (unsigned i = first; i <= s->nodes; i++)
            if (sn_entry(n, i)->st.joined) r->joined++;
        if (r->joined == s->joiners && ++tail > 3000u) break;   /* keep running: a double grant would collide */
    }
    flush(trk);

    for (unsigned i = first; i <= s->nodes; i++) {
        const hh_entry_t *e = sn_entry(n, i);
        r->join_ms[i - first] = e->st.joined ? (uint32_t)(e->st.t_joined - e->st.t_start) : 0;
        r->grant_ms[i - first] = e->st.t_granted ? (uint32_t)(e->st.t_granted - e->st.t_start) : 0;
        if (e->st.joined && e->st.t_joined > r->all_joined_ms) r->all_joined_ms = (uint32_t)e->st.t_joined;
        r->timeouts += e->st.timeouts;
        r->cycles += e->st.cycles;
        r->ignored += e->st.ignored_responses;
    }
    r->requests = trk->requests;
    r->collision_slots = trk->collision_slots;
    r->collided_requests = trk->collided_requests;
    {
        sn_result_t out;
        sn_result(n, &out);
        r->tx_outside_own_slot = out.tx_outside_own_slot;
    }
    r->slots_unique = slots_unique(n, s->nodes, 1ull << (s->slots_per_frame - 1u));
}

/* Prints the result of a scenario as one key=value line. */
static void report(const scn_t *s, const res_t *r)
{
    uint32_t sorted[SN_MAX_NODES], gs[SN_MAX_NODES];
    unsigned n = s->joiners;

    memcpy(sorted, r->join_ms, n * sizeof sorted[0]);
    memcpy(gs, r->grant_ms, n * sizeof gs[0]);
    qsort(sorted, n, sizeof sorted[0], cmp_u32);
    qsort(gs, n, sizeof gs[0], cmp_u32);
    printf("entry_sim scenario=%s nodes=%u joiners=%u slots_per_frame=%u seed=%u loss_permille=%u joined=%u "
           "grant_ms_median=%u grant_ms_max=%u join_ms_min=%u join_ms_median=%u join_ms_max=%u requests=%u "
           "collision_slots=%u collided_requests=%u timeouts=%u cycles=%u tx_outside_own_slot=%llu "
           "slots_unique=%d\n",
           s->name, s->nodes, s->joiners, s->slots_per_frame, s->seed, s->loss_permille, r->joined, gs[n / 2],
           gs[n - 1], sorted[0], sorted[n / 2], sorted[n - 1], r->requests, r->collision_slots,
           r->collided_requests, r->timeouts, r->cycles, (unsigned long long)r->tx_outside_own_slot,
           r->slots_unique);
}

/* Runs one scenario and returns its result. */
static void scenario(const scn_t *s, res_t *r)
{
    track_t trk;
    slot_net_t *n;

    memset(&trk, 0, sizeof trk);
    n = build(s, &trk);
    HH_ASSERT(n != NULL);
    if (!n) return;
    run(n, s, &trk, MAX_RUN_MS, r);
    report(s, r);
    sn_destroy(n);
}

/* One node joins an existing one: no collision, the lowest free slot, a few seconds. */
static void test_single_joiner(void)
{
    scn_t s = { "single", 2, 1, 20, 1, 0, NULL };
    res_t r;
    scenario(&s, &r);
    HH_ASSERT_EQ_INT(r.joined, 1);
    HH_ASSERT_EQ_INT(r.collision_slots, 0);
    HH_ASSERT(r.slots_unique);
    HH_ASSERT_EQ_INT((int)r.tx_outside_own_slot, 0);
    HH_ASSERT(r.join_ms[0] >= 2000u);        /* not before the listen time */
    HH_ASSERT(r.join_ms[0] < 8000u);
}

/* Three nodes join one member at the same time. */
static void test_three_join_at_once(void)
{
    scn_t s = { "four_nodes", 4, 3, 20, 1, 0, NULL };
    res_t r;
    scenario(&s, &r);
    HH_ASSERT_EQ_INT(r.joined, 3);
    HH_ASSERT(r.slots_unique);
    HH_ASSERT_EQ_INT((int)r.tx_outside_own_slot, 0);
    HH_ASSERT(r.requests >= 3u);
}

/* Six nodes join two members: two radios can answer, only the first answers. */
static void test_six_join_two_members(void)
{
    scn_t s = { "eight_nodes", 8, 6, 20, 1, 0, NULL };
    res_t r;
    scenario(&s, &r);
    HH_ASSERT_EQ_INT(r.joined, 6);
    HH_ASSERT(r.slots_unique);
    HH_ASSERT_EQ_INT((int)r.tx_outside_own_slot, 0);
}

/* The largest field net: 28 nodes join 4 members at once, 32 in all. */
static void test_thirty_two_nodes(void)
{
    scn_t s = { "thirty_two_nodes", 32, 28, 40, 1, 0, NULL };
    res_t r;
    scenario(&s, &r);
    HH_ASSERT_EQ_INT(r.joined, 28);
    HH_ASSERT(r.slots_unique);
    HH_ASSERT_EQ_INT((int)r.tx_outside_own_slot, 0);
    HH_ASSERT(r.collision_slots > 0);        /* one entry slot per frame: contention is real */
}

/* A third of all PDUs are lost, grants included: the timeout and the new request id recover. */
static void test_lost_grants_recover(void)
{
    scn_t s = { "lossy", 4, 3, 20, 1, 300, NULL };
    res_t r;
    scenario(&s, &r);
    HH_ASSERT_EQ_INT(r.joined, 3);
    HH_ASSERT(r.slots_unique);
    HH_ASSERT(r.timeouts > 0);
}

/* Different seeds give different join times; none is stuck in lockstep. */
static void test_seeds_differ(void)
{
    uint32_t last[5];
    unsigned distinct = 0;

    for (unsigned i = 0; i < 5; i++) {
        scn_t s = { "seeds", 4, 3, 20, i + 1u, 0, NULL };
        res_t r;
        scenario(&s, &r);
        HH_ASSERT_EQ_INT(r.joined, 3);
        HH_ASSERT(r.slots_unique);
        last[i] = 0;
        for (unsigned j = 0; j < s.joiners; j++) last[i] += r.grant_ms[j];   /* join times are bound to superframes */
    }
    for (unsigned i = 0; i < 5; i++) {
        bool seen = false;
        for (unsigned j = 0; j < i; j++) if (last[j] == last[i]) seen = true;
        if (!seen) distinct++;
    }
    HH_ASSERT(distinct >= 3);
}

/* Control run: a window of one opportunity makes every joiner send at the same time, again and again.
 * Nobody joins, which is why the window must grow. */
static void test_no_backoff_never_resolves(void)
{
    hh_entry_params_t p;
    scn_t s = { "no_backoff", 4, 3, 20, 1, 0, NULL };
    res_t r;

    hh_entry_params_default(&p);
    p.be_min = 0;
    p.be_max = 0;
    s.params = &p;
    scenario(&s, &r);
    HH_ASSERT_EQ_INT(r.joined, 0);
    HH_ASSERT(r.collision_slots >= 15u);
    HH_ASSERT_EQ_INT(r.collided_requests, r.requests);
}

/* A member leaves; its slot is free again and a later node takes it. */
static void test_leave_frees_slot_for_later_joiner(void)
{
    scn_t s = { "leave_rejoin", 4, 3, 20, 1, 0, NULL };
    track_t trk;
    slot_net_t *n;
    res_t r;
    uint64_t freed;
    uint32_t t;

    memset(&trk, 0, sizeof trk);
    n = build(&s, &trk);
    HH_ASSERT(n != NULL);
    if (!n) return;
    /* Nodes 2 and 3 join first; node 4 waits. */
    for (t = 0; t < 40000u; t++) {
        sn_set_time(n, t);
        if (t == 0) {
            HH_ASSERT_OK(sn_join(n, 2));
            HH_ASSERT_OK(sn_join(n, 3));
        }
        sn_step(n);
        if (sn_entry(n, 2)->st.joined && sn_entry(n, 3)->st.joined) break;
    }
    HH_ASSERT(sn_entry(n, 2)->st.joined && sn_entry(n, 3)->st.joined);
    freed = sn_own_slots(n, 2);
    HH_ASSERT(freed != 0);

    HH_ASSERT_OK(sn_leave(n, 2));
    for (uint32_t k = 0; k < 8000u; k++) {      /* leave, release and a quiet net */
        sn_set_time(n, ++t);
        sn_step(n);
    }
    HH_ASSERT_EQ_INT((int)sn_own_slots(n, 2), 0);
    HH_ASSERT_EQ_INT(hh_entry_state(sn_entry(n, 2)), HH_ENTRY_OFF);

    HH_ASSERT_OK(sn_join(n, 4));
    for (uint32_t k = 0; k < 40000u && !sn_entry(n, 4)->st.joined; k++) {
        sn_set_time(n, ++t);
        sn_step(n);
    }
    flush(&trk);
    HH_ASSERT(sn_entry(n, 4)->st.joined);
    HH_ASSERT(sn_own_slots(n, 4) == freed);     /* the slot node 2 gave up */
    memset(&r, 0, sizeof r);
    printf("entry_sim scenario=leave_rejoin freed_slot_mask=%llx taken_by_node4=%llx rejoin_ms=%u\n",
           (unsigned long long)freed, (unsigned long long)sn_own_slots(n, 4),
           (unsigned)(sn_entry(n, 4)->st.t_joined - sn_entry(n, 4)->st.t_start));
    sn_destroy(n);
}

HH_TEST_MAIN_BEGIN("entry_net")
    hh_log_set_level(HH_LOG_ERROR);
    HH_RUN(test_single_joiner);
    HH_RUN(test_three_join_at_once);
    HH_RUN(test_six_join_two_members);
    HH_RUN(test_thirty_two_nodes);
    HH_RUN(test_lost_grants_recover);
    HH_RUN(test_seeds_differ);
    HH_RUN(test_no_backoff_never_resolves);
    HH_RUN(test_leave_frees_slot_for_later_joiner);
HH_TEST_MAIN_END()
