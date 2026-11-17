/*
 * Demand table: latest report per node, ordering by slot, a full table,
 * bad arguments, and the age of a report.
 */
#include "hhsdr/mac/alloc.h"
#include "hhsdr/mac/mac.h"
#include "hh_test.h"
#include <string.h>

static hh_alloc_table_t tab;

/* Builds a report with the given data-class count and nothing else. */
static void report(uint8_t out[HH_ALLOC_CLASSES], uint8_t data)
{
    for (unsigned k = 0; k < HH_ALLOC_CLASSES; k++) out[k] = 0;
    out[4] = data;
}

/* A new node is stored and found; an unknown node is not found. */
static void test_store_and_find(void)
{
    uint8_t r[HH_ALLOC_CLASSES];
    const hh_alloc_demand_t *d;

    hh_alloc_table_init(&tab);
    report(r, 7);
    r[2] = 3;
    HH_ASSERT_OK(hh_alloc_note(&tab, 5, r, 100));
    d = hh_alloc_find(&tab, 5);
    HH_ASSERT(d != NULL);
    HH_ASSERT_EQ_INT(d->node, 5);
    HH_ASSERT_EQ_INT(d->depth[2], 3);
    HH_ASSERT_EQ_INT(d->depth[4], 7);
    HH_ASSERT_EQ_INT((int)d->seen_slot, 100);
    HH_ASSERT(hh_alloc_find(&tab, 6) == NULL);
}

/* A newer report replaces the stored one; an older one is ignored. */
static void test_latest_wins(void)
{
    uint8_t r[HH_ALLOC_CLASSES];

    hh_alloc_table_init(&tab);
    report(r, 1);
    HH_ASSERT_OK(hh_alloc_note(&tab, 5, r, 100));
    report(r, 9);
    HH_ASSERT_OK(hh_alloc_note(&tab, 5, r, 120));
    HH_ASSERT_EQ_INT(hh_alloc_find(&tab, 5)->depth[4], 9);
    report(r, 4);
    HH_ASSERT_OK(hh_alloc_note(&tab, 5, r, 110));   /* arrived late: older than the stored one */
    HH_ASSERT_EQ_INT(hh_alloc_find(&tab, 5)->depth[4], 9);
    HH_ASSERT_EQ_INT((int)hh_alloc_find(&tab, 5)->seen_slot, 120);
}

/* The table holds 32 nodes. The 33rd is refused and nothing else changes. */
static void test_table_full(void)
{
    uint8_t r[HH_ALLOC_CLASSES];

    hh_alloc_table_init(&tab);
    for (unsigned n = 1; n <= HH_ALLOC_NODES_MAX; n++) {
        report(r, (uint8_t)n);
        HH_ASSERT_OK(hh_alloc_note(&tab, (uint16_t)n, r, n));
    }
    report(r, 99);
    HH_ASSERT_ERR(hh_alloc_note(&tab, 200, r, 500), HH_ERR_NOMEM);
    HH_ASSERT(hh_alloc_find(&tab, 200) == NULL);
    for (unsigned n = 1; n <= HH_ALLOC_NODES_MAX; n++)
        HH_ASSERT_EQ_INT(hh_alloc_find(&tab, (uint16_t)n)->depth[4], (int)n);
    HH_ASSERT_OK(hh_alloc_note(&tab, 7, r, 600));   /* an existing node still updates */
    HH_ASSERT_EQ_INT(hh_alloc_find(&tab, 7)->depth[4], 99);
}

/* Null arguments and node 0 are refused. */
static void test_bad_arguments(void)
{
    uint8_t r[HH_ALLOC_CLASSES];

    hh_alloc_table_init(&tab);
    report(r, 1);
    HH_ASSERT_ERR(hh_alloc_note(NULL, 5, r, 1), HH_ERR_INVAL);
    HH_ASSERT_ERR(hh_alloc_note(&tab, 5, NULL, 1), HH_ERR_INVAL);
    HH_ASSERT_ERR(hh_alloc_note(&tab, 0, r, 1), HH_ERR_INVAL);
    HH_ASSERT(hh_alloc_find(NULL, 5) == NULL);
    HH_ASSERT(hh_alloc_find(&tab, 0) == NULL);
}

/* Age counts slots since the report, and is 0 when now is not after it. */
static void test_age(void)
{
    uint8_t r[HH_ALLOC_CLASSES];
    const hh_alloc_demand_t *d;

    hh_alloc_table_init(&tab);
    report(r, 1);
    HH_ASSERT_OK(hh_alloc_note(&tab, 5, r, 100));
    d = hh_alloc_find(&tab, 5);
    HH_ASSERT_EQ_INT((int)hh_alloc_age(d, 100), 0);
    HH_ASSERT_EQ_INT((int)hh_alloc_age(d, 160), 60);
    HH_ASSERT_EQ_INT((int)hh_alloc_age(d, 40), 0);
    HH_ASSERT_EQ_INT((int)hh_alloc_age(NULL, 160), 0);
}

/* Parameters of a 20-slot frame whose last slot is the shared entry slot, so 19 slots are allocated. */
static hh_alloc_params_t params20(void)
{
    hh_alloc_params_t p;
    hh_alloc_params_default(&p, 20);
    p.fixed_slots = 1ull << 19;
    p.node_cap = 0;   /* no cap, so the other rules are tested alone */
    p.join_reserve = 0;   /* nor an idle reserve */
    return p;
}

/* Stores a report with the given voice, video and data counts, sent in slot `at`. */
static void put(uint16_t node, uint8_t voice, uint8_t video, uint8_t data, uint64_t at)
{
    uint8_t r[HH_ALLOC_CLASSES] = { 0, 0, voice, video, data };
    HH_ASSERT_OK(hh_alloc_note(&tab, node, r, at));
}

/* Slots of a node in the map. */
static int slots_of(const hh_alloc_map_t *m, uint16_t node)
{
    int n = 0;
    for (unsigned s = 0; s < HH_ALLOC_SLOTS_MAX; s++)
        if (m->owner[s] == node) n++;
    return n;
}

/* The fixed slot is never allocated, every other slot has an owner and every member has a slot. */
static void test_floor_and_fixed_slot(void)
{
    hh_alloc_params_t p = params20();
    hh_alloc_map_t m;
    int total = 0;

    hh_alloc_table_init(&tab);
    for (uint16_t n = 1; n <= 4; n++) put(n, 0, 0, 0, 1000);
    HH_ASSERT_OK(hh_alloc_compute(&p, &tab, 1000, 0, &m));
    HH_ASSERT_EQ_INT(m.owner[19], 0);
    for (unsigned s = 0; s < 19; s++) HH_ASSERT(m.owner[s] >= 1 && m.owner[s] <= 4);
    for (unsigned s = 20; s < HH_ALLOC_SLOTS_MAX; s++) HH_ASSERT_EQ_INT(m.owner[s], 0);
    for (uint16_t n = 1; n <= 4; n++) {
        HH_ASSERT(slots_of(&m, n) >= 1);
        total += slots_of(&m, n);
    }
    HH_ASSERT_EQ_INT(total, 19);
}

/* The same reports stored in a different order give the same map. */
static void test_same_map_whatever_the_arrival_order(void)
{
    hh_alloc_params_t p = params20();
    hh_alloc_table_t other;
    hh_alloc_map_t a, b;
    uint8_t r[HH_ALLOC_CLASSES];

    hh_alloc_table_init(&tab);
    hh_alloc_table_init(&other);
    for (uint16_t n = 1; n <= 6; n++) put(n, (uint8_t)(n % 3), (uint8_t)(n * 4), (uint8_t)(60 - n), 990);
    for (uint16_t n = 6; n >= 1; n--) {
        const hh_alloc_demand_t *d = hh_alloc_find(&tab, n);
        for (unsigned k = 0; k < HH_ALLOC_CLASSES; k++) r[k] = d->depth[k];
        HH_ASSERT_OK(hh_alloc_note(&other, n, r, d->seen_slot));
    }
    for (uint32_t sf = 0; sf < 5; sf++) {
        HH_ASSERT_OK(hh_alloc_compute(&p, &tab, 1000, sf, &a));
        HH_ASSERT_OK(hh_alloc_compute(&p, &other, 1000, sf, &b));
        HH_ASSERT(memcmp(&a, &b, sizeof a) == 0);
    }
}

/* Voice demand is met from the reserve before data gets what is left. */
static void test_voice_first(void)
{
    hh_alloc_params_t p = params20();
    hh_alloc_map_t m;

    hh_alloc_table_init(&tab);
    put(1, 0, 0, 60, 1000);
    put(2, 0, 0, 60, 1000);
    put(3, 3, 0, 0, 1000);   /* three voice messages: the keep-alive slot covers one, two more are wanted */
    put(4, 0, 0, 60, 1000);
    HH_ASSERT_OK(hh_alloc_compute(&p, &tab, 1000, 0, &m));
    HH_ASSERT_EQ_INT(slots_of(&m, 3), 3);
    HH_ASSERT_EQ_INT(slots_of(&m, 1), 6);   /* the other 13 spare slots go one at a time to the data nodes */
    HH_ASSERT_EQ_INT(slots_of(&m, 2), 5);
    HH_ASSERT_EQ_INT(slots_of(&m, 4), 5);
}

/* With one spare slot only, voice gets it, not data. */
static void test_voice_beats_data_when_scarce(void)
{
    hh_alloc_params_t p = params20();
    hh_alloc_map_t m;

    hh_alloc_table_init(&tab);
    for (uint16_t n = 1; n <= 18; n++) put(n, 0, 0, 0, 1000);
    put(1, 0, 0, 50, 1000);
    put(18, 2, 0, 0, 1000);
    HH_ASSERT_OK(hh_alloc_compute(&p, &tab, 1000, 0, &m));
    HH_ASSERT_EQ_INT(slots_of(&m, 18), 2);
    HH_ASSERT_EQ_INT(slots_of(&m, 1), 1);
}

/* Video and data share what is left by weight, 2 to 1 by default. */
static void test_weighted_share(void)
{
    hh_alloc_params_t p = params20();
    hh_alloc_map_t m;

    hh_alloc_table_init(&tab);
    put(1, 0, 100, 0, 1000);
    put(2, 0, 0, 100, 1000);
    HH_ASSERT_OK(hh_alloc_compute(&p, &tab, 1000, 0, &m));
    HH_ASSERT_EQ_INT(slots_of(&m, 1), 12);   /* 17 shared slots: 11 to video, 6 to data, one keep-alive each */
    HH_ASSERT_EQ_INT(slots_of(&m, 2), 7);
}

/* A class nobody uses gives its share to the other. */
static void test_unused_class_share_goes_on(void)
{
    hh_alloc_params_t p = params20();
    hh_alloc_map_t m;

    hh_alloc_table_init(&tab);
    put(1, 0, 0, 100, 1000);
    put(2, 0, 0, 0, 1000);
    HH_ASSERT_OK(hh_alloc_compute(&p, &tab, 1000, 0, &m));
    HH_ASSERT_EQ_INT(slots_of(&m, 1), 18);
    HH_ASSERT_EQ_INT(slots_of(&m, 2), 1);
}

/* Slots nobody asked for are dealt round-robin, and the extra one moves with the superframe. */
static void test_spare_slots_rotate(void)
{
    hh_alloc_params_t p = params20();
    hh_alloc_map_t m;

    hh_alloc_table_init(&tab);
    for (uint16_t n = 1; n <= 3; n++) put(n, 0, 0, 0, 1000);
    for (uint32_t sf = 0; sf < 3; sf++) {
        HH_ASSERT_OK(hh_alloc_compute(&p, &tab, 1000, sf, &m));
        for (uint16_t n = 1; n <= 3; n++)
            HH_ASSERT_EQ_INT(slots_of(&m, n), n == sf + 1 ? 7 : 6);
    }
}

/* An old report counts as no demand, and a node silent for long enough is no longer a member. */
static void test_stale_and_gone(void)
{
    hh_alloc_params_t p = params20();
    hh_alloc_map_t m;

    p.stale_slots = 100;
    p.gone_slots = 1000;
    hh_alloc_table_init(&tab);
    put(1, 0, 0, 50, 1950);   /* age 50: counts */
    put(2, 0, 0, 50, 1800);   /* age 200: stale, floor only */
    put(3, 0, 0, 50, 500);    /* age 1500: gone */
    HH_ASSERT_OK(hh_alloc_compute(&p, &tab, 2000, 0, &m));
    HH_ASSERT_EQ_INT(slots_of(&m, 1), 18);
    HH_ASSERT_EQ_INT(slots_of(&m, 2), 1);
    HH_ASSERT_EQ_INT(slots_of(&m, 3), 0);
}

/* More members than slots to give: refused and the map is not touched. */
static void test_too_many_members(void)
{
    hh_alloc_params_t p;
    hh_alloc_map_t m;

    hh_alloc_params_default(&p, 5);
    p.fixed_slots = 0x18;   /* slots 3 and 4 */
    hh_alloc_table_init(&tab);
    for (uint16_t n = 1; n <= 4; n++) put(n, 0, 0, 0, 1000);
    memset(&m, 0x5A, sizeof m);
    HH_ASSERT_ERR(hh_alloc_compute(&p, &tab, 1000, 0, &m), HH_ERR_NOMEM);
    HH_ASSERT_EQ_INT(m.owner[0], 0x5A5A);
}

/* The idle reserve stays free against any demand, so a joining radio can be granted a slot. The default is one slot. */
static void test_join_reserve_stays_idle(void)
{
    hh_alloc_params_t p = params20();
    hh_alloc_params_t d;
    hh_alloc_map_t m;
    int owned;

    hh_alloc_params_default(&d, 20);
    HH_ASSERT_EQ_INT(d.join_reserve, 1);
    hh_alloc_table_init(&tab);
    put(1, 0, 0, 200, 1000);
    for (uint16_t n = 2; n <= 4; n++) put(n, 0, 0, 0, 1000);

    HH_ASSERT_OK(hh_alloc_compute(&p, &tab, 1000, 0, &m));   /* no reserve: all 19 slots owned */
    owned = 0;
    for (unsigned s = 0; s < 19; s++) owned += m.owner[s] != 0;
    HH_ASSERT_EQ_INT(owned, 19);

    p.join_reserve = 3;
    HH_ASSERT_OK(hh_alloc_compute(&p, &tab, 1000, 0, &m));
    owned = 0;
    for (unsigned s = 0; s < 19; s++) owned += m.owner[s] != 0;
    HH_ASSERT_EQ_INT(owned, 16);                              /* three held back, however greedy node 1 is */
    for (uint16_t n = 2; n <= 4; n++) HH_ASSERT(slots_of(&m, n) >= 1);

    p.join_reserve = 200;                                      /* larger than the frame: members keep their slot */
    HH_ASSERT_OK(hh_alloc_compute(&p, &tab, 1000, 0, &m));
    for (uint16_t n = 1; n <= 4; n++) HH_ASSERT_EQ_INT(slots_of(&m, n), 1);
}

/* One node with endless demand is held at the cap and the other nodes share the rest. */
static void test_cap_holds_a_greedy_node(void)
{
    hh_alloc_params_t p = params20();
    hh_alloc_map_t m;

    p.node_cap = 8;
    hh_alloc_table_init(&tab);
    put(1, 0, 0, 200, 1000);
    for (uint16_t n = 2; n <= 4; n++) put(n, 0, 0, 0, 1000);
    HH_ASSERT_OK(hh_alloc_compute(&p, &tab, 1000, 0, &m));
    HH_ASSERT_EQ_INT(slots_of(&m, 1), 8);
    HH_ASSERT_EQ_INT(slots_of(&m, 2) + slots_of(&m, 3) + slots_of(&m, 4), 11);
    for (uint16_t n = 2; n <= 4; n++) HH_ASSERT(slots_of(&m, n) >= 3);
    printf("greedy_node cap=%u greedy_slots=%d others=%d,%d,%d\n", p.node_cap, slots_of(&m, 1), slots_of(&m, 2),
           slots_of(&m, 3), slots_of(&m, 4));
}

/* What a capped node cannot take goes to the nodes that can. */
static void test_cap_gives_the_rest_to_other_nodes(void)
{
    hh_alloc_params_t p = params20();
    hh_alloc_map_t m;

    p.node_cap = 8;
    hh_alloc_table_init(&tab);
    put(1, 0, 0, 200, 1000);
    put(2, 0, 0, 200, 1000);
    put(3, 0, 0, 0, 1000);
    HH_ASSERT_OK(hh_alloc_compute(&p, &tab, 1000, 0, &m));
    HH_ASSERT_EQ_INT(slots_of(&m, 1), 8);
    HH_ASSERT_EQ_INT(slots_of(&m, 2), 8);
    HH_ASSERT_EQ_INT(slots_of(&m, 3), 3);
}

/* When every node is at the cap, the slots left stay idle. */
static void test_cap_leaves_slots_idle_when_all_are_capped(void)
{
    hh_alloc_params_t p = params20();
    hh_alloc_map_t m;
    int owned = 0;

    p.node_cap = 5;
    hh_alloc_table_init(&tab);
    put(1, 0, 0, 200, 1000);
    put(2, 0, 0, 200, 1000);
    HH_ASSERT_OK(hh_alloc_compute(&p, &tab, 1000, 0, &m));
    HH_ASSERT_EQ_INT(slots_of(&m, 1), 5);
    HH_ASSERT_EQ_INT(slots_of(&m, 2), 5);
    for (unsigned s = 0; s < 19; s++) owned += m.owner[s] != 0;
    HH_ASSERT_EQ_INT(owned, 10);
    HH_ASSERT_EQ_INT(m.owner[19], 0);
}

/* The cap also holds voice, and a cap of one leaves only the keep-alive slot. */
static void test_cap_applies_to_voice_too(void)
{
    hh_alloc_params_t p = params20();
    hh_alloc_map_t m;

    hh_alloc_table_init(&tab);
    put(1, 12, 0, 0, 1000);   /* wants 11 voice slots beyond the keep-alive slot, the reserve is 4 */
    put(2, 0, 0, 0, 1000);
    p.node_cap = 3;
    HH_ASSERT_OK(hh_alloc_compute(&p, &tab, 1000, 0, &m));
    HH_ASSERT_EQ_INT(slots_of(&m, 1), 3);
    p.node_cap = 1;
    HH_ASSERT_OK(hh_alloc_compute(&p, &tab, 1000, 0, &m));
    HH_ASSERT_EQ_INT(slots_of(&m, 1), 1);
    HH_ASSERT_EQ_INT(slots_of(&m, 2), 1);
}

/* The default cap is two fifths of the frame, and never below one. */
static void test_default_cap(void)
{
    hh_alloc_params_t p;

    hh_alloc_params_default(&p, 20);
    HH_ASSERT_EQ_INT(p.node_cap, 8);
    hh_alloc_params_default(&p, 2);
    HH_ASSERT_EQ_INT(p.node_cap, 1);
}

/* The first voice message of a frame rides in the keep-alive slot, further ones draw on the reserve, and a call is refused when it is used up. */
static void test_voice_admission_uses_the_reserve(void)
{
    hh_alloc_params_t p = params20();   /* reserve of 4, one message per slot */

    hh_alloc_table_init(&tab);
    for (uint16_t n = 1; n <= 3; n++) put(n, 0, 0, 0, 1000);
    HH_ASSERT_EQ_INT((int)hh_alloc_voice_free(&p, &tab, 1000), 4);
    put(1, 3, 0, 0, 1000);   /* three messages a frame: two slots from the reserve */
    HH_ASSERT_EQ_INT((int)hh_alloc_voice_free(&p, &tab, 1000), 2);
    put(2, 3, 0, 0, 1000);
    HH_ASSERT_EQ_INT((int)hh_alloc_voice_free(&p, &tab, 1000), 0);
    HH_ASSERT(hh_alloc_voice_admit(&p, &tab, 1000, 3, 1));    /* the first call of a node costs no reserve slot */
    put(3, 1, 0, 0, 1000);
    HH_ASSERT(!hh_alloc_voice_admit(&p, &tab, 1000, 3, 1));   /* its second call would need one */
    HH_ASSERT(!hh_alloc_voice_admit(&p, &tab, 1000, 1, 1));   /* nor can a running node add one */
}

/* Old voice reports count as no voice. */
static void test_voice_admission_ignores_stale_reports(void)
{
    hh_alloc_params_t p = params20();

    p.stale_slots = 100;
    hh_alloc_table_init(&tab);
    put(1, 12, 0, 0, 800);   /* age 200 at slot 1000: stale */
    HH_ASSERT_EQ_INT((int)hh_alloc_voice_free(&p, &tab, 1000), 4);
    HH_ASSERT(hh_alloc_voice_admit(&p, &tab, 1000, 2, 2));
}

/* The cap on a node's slots limits its calls even when the reserve has room. */
static void test_voice_admission_respects_the_cap(void)
{
    hh_alloc_params_t p = params20();

    p.node_cap = 3;   /* a node holds at most the keep-alive slot and two more */
    hh_alloc_table_init(&tab);
    put(1, 3, 0, 0, 1000);   /* two reserve slots in use, two free */
    HH_ASSERT_EQ_INT((int)hh_alloc_voice_free(&p, &tab, 1000), 2);
    HH_ASSERT(!hh_alloc_voice_admit(&p, &tab, 1000, 1, 1));   /* a fourth message needs a third reserve slot */
    HH_ASSERT(hh_alloc_voice_admit(&p, &tab, 1000, 2, 3));    /* another node can still take two */
}

/* Slots left after one for each member limit the reserve. */
static void test_voice_reserve_limited_by_free_slots(void)
{
    hh_alloc_params_t p = params20();   /* 19 slots to give */

    hh_alloc_table_init(&tab);
    for (uint16_t n = 1; n <= 18; n++) put(n, 0, 0, 0, 1000);
    HH_ASSERT_EQ_INT((int)hh_alloc_voice_free(&p, &tab, 1000), 1);   /* 19 - 18 members, below the reserve of 4 */
    HH_ASSERT(hh_alloc_voice_admit(&p, &tab, 1000, 18, 2));
    HH_ASSERT(!hh_alloc_voice_admit(&p, &tab, 1000, 18, 3));
}

/* Every call admitted is given its voice slots by the allocator. Calls are added to one node until the next is refused. */
static void test_admitted_calls_are_served(void)
{
    hh_alloc_params_t p = params20();
    hh_alloc_map_t m;
    unsigned admitted = 0;

    hh_alloc_table_init(&tab);
    for (uint16_t n = 1; n <= 3; n++) put(n, 0, 0, 0, 1000);
    while (hh_alloc_voice_admit(&p, &tab, 1000, 1, 1)) {
        admitted++;
        put(1, (uint8_t)admitted, 0, 0, 1000);
        HH_ASSERT(admitted < 20);
    }
    HH_ASSERT_EQ_INT((int)admitted, 5);   /* the first rides in the keep-alive slot, four use the reserve */
    HH_ASSERT_OK(hh_alloc_compute(&p, &tab, 1000, 0, &m));
    HH_ASSERT(slots_of(&m, 1) >= 5);      /* the keep-alive slot and the four voice slots */
    printf("voice_admission reserve=%u calls_admitted=%u node_slots=%d\n", p.voice_reserve, admitted, slots_of(&m, 1));
}

/* Bad arguments are refused. */
static void test_voice_admission_bad_arguments(void)
{
    hh_alloc_params_t p = params20();

    hh_alloc_table_init(&tab);
    HH_ASSERT(!hh_alloc_voice_admit(&p, NULL, 1000, 1, 1));
    HH_ASSERT(!hh_alloc_voice_admit(&p, &tab, 1000, 0, 1));
    HH_ASSERT(!hh_alloc_voice_admit(&p, &tab, 1000, 1, 0));
    HH_ASSERT_EQ_INT((int)hh_alloc_voice_free(NULL, &tab, 1000), 0);
}

/* An empty table gives an empty map. Unusable parameters are refused. */
static void test_empty_and_bad_params(void)
{
    hh_alloc_params_t p = params20(), bad;
    hh_alloc_map_t m;

    hh_alloc_table_init(&tab);
    memset(&m, 0x5A, sizeof m);
    HH_ASSERT_OK(hh_alloc_compute(&p, &tab, 1000, 0, &m));
    for (unsigned s = 0; s < HH_ALLOC_SLOTS_MAX; s++) HH_ASSERT_EQ_INT(m.owner[s], 0);

    bad = p; bad.slots_per_frame = 0;
    HH_ASSERT_ERR(hh_alloc_compute(&bad, &tab, 1000, 0, &m), HH_ERR_INVAL);
    bad = p; bad.slots_per_frame = 65;
    HH_ASSERT_ERR(hh_alloc_params_check(&bad), HH_ERR_INVAL);
    bad = p; bad.msgs_per_slot[HH_MAC_CLASS_VOICE] = 0;
    HH_ASSERT_ERR(hh_alloc_params_check(&bad), HH_ERR_INVAL);
    bad = p; bad.weight_video = 0; bad.weight_data = 0;
    HH_ASSERT_ERR(hh_alloc_params_check(&bad), HH_ERR_INVAL);
    HH_ASSERT_ERR(hh_alloc_compute(NULL, &tab, 1000, 0, &m), HH_ERR_INVAL);
    HH_ASSERT_ERR(hh_alloc_compute(&p, NULL, 1000, 0, &m), HH_ERR_INVAL);
}

HH_TEST_MAIN_BEGIN("alloc")
    HH_RUN(test_store_and_find);
    HH_RUN(test_latest_wins);
    HH_RUN(test_table_full);
    HH_RUN(test_bad_arguments);
    HH_RUN(test_age);
    HH_RUN(test_floor_and_fixed_slot);
    HH_RUN(test_same_map_whatever_the_arrival_order);
    HH_RUN(test_voice_first);
    HH_RUN(test_voice_beats_data_when_scarce);
    HH_RUN(test_weighted_share);
    HH_RUN(test_unused_class_share_goes_on);
    HH_RUN(test_spare_slots_rotate);
    HH_RUN(test_stale_and_gone);
    HH_RUN(test_cap_holds_a_greedy_node);
    HH_RUN(test_join_reserve_stays_idle);
    HH_RUN(test_cap_gives_the_rest_to_other_nodes);
    HH_RUN(test_cap_leaves_slots_idle_when_all_are_capped);
    HH_RUN(test_cap_applies_to_voice_too);
    HH_RUN(test_default_cap);
    HH_RUN(test_voice_admission_uses_the_reserve);
    HH_RUN(test_voice_admission_ignores_stale_reports);
    HH_RUN(test_voice_admission_respects_the_cap);
    HH_RUN(test_voice_reserve_limited_by_free_slots);
    HH_RUN(test_admitted_calls_are_served);
    HH_RUN(test_voice_admission_bad_arguments);
    HH_RUN(test_too_many_members);
    HH_RUN(test_empty_and_bad_params);
HH_TEST_MAIN_END()
