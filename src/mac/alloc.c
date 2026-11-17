/* Demand table and slot allocation. See include/hhsdr/mac/alloc.h. */
#include "hhsdr/mac/alloc.h"
#include "hhsdr/mac/mac.h"
#include <string.h>

_Static_assert(HH_ALLOC_CLASSES == HH_MAC_CLASS_COUNT, "one report byte per MAC class");

/* Empties the table. */
void hh_alloc_table_init(hh_alloc_table_t *t)
{
    if (t) memset(t, 0, sizeof *t);
}

/* Returns the index of a node's entry, or -1. */
static int index_of(const hh_alloc_table_t *t, uint16_t node)
{
    for (unsigned i = 0; i < HH_ALLOC_NODES_MAX; i++)
        if (t->entry[i].used && t->entry[i].node == node) return (int)i;
    return -1;
}

/* Stores the latest report of a node; a report older than the stored one is ignored. */
hh_status_t hh_alloc_note(hh_alloc_table_t *t, uint16_t node, const uint8_t depth[HH_ALLOC_CLASSES],
                          uint64_t slot)
{
    int i;

    if (!t || !depth || node == 0) return HH_ERR_INVAL;
    i = index_of(t, node);
    if (i < 0) {
        for (unsigned k = 0; k < HH_ALLOC_NODES_MAX && i < 0; k++)
            if (!t->entry[k].used) i = (int)k;
        if (i < 0) return HH_ERR_NOMEM;
        t->entry[i].used = true;
        t->entry[i].node = node;
    } else if (slot < t->entry[i].seen_slot) {
        return HH_OK;
    }
    memcpy(t->entry[i].depth, depth, HH_ALLOC_CLASSES);
    t->entry[i].seen_slot = slot;
    return HH_OK;
}

/* Returns the latest report of a node, or NULL if none was heard. */
const hh_alloc_demand_t *hh_alloc_find(const hh_alloc_table_t *t, uint16_t node)
{
    int i;

    if (!t || node == 0) return NULL;
    i = index_of(t, node);
    return i < 0 ? NULL : &t->entry[i];
}

/* Returns the slots since the report was sent. */
uint64_t hh_alloc_age(const hh_alloc_demand_t *d, uint64_t now_slot)
{
    if (!d || now_slot <= d->seen_slot) return 0;
    return now_slot - d->seen_slot;
}

/* Allocation classes: index into the per-member arrays below. */
enum { A_VOICE = 0, A_VIDEO, A_DATA, A_CLASSES };

static const unsigned mac_class[A_CLASSES] = { HH_MAC_CLASS_VOICE, HH_MAC_CLASS_VIDEO, HH_MAC_CLASS_DATA };

typedef struct {
    uint16_t node;
    uint32_t want[A_CLASSES];    /* slots wanted beyond the keep-alive slot */
    uint32_t got[A_CLASSES];
    uint32_t quota;              /* slots this member gets in all */
} member_t;

/* Fills p with placeholder values. TODO: every value below, from the network owner. */
void hh_alloc_params_default(hh_alloc_params_t *p, uint16_t slots_per_frame)
{
    if (!p) return;
    memset(p, 0, sizeof *p);
    p->slots_per_frame = slots_per_frame;
    p->voice_reserve = 4;                                   /* TODO: reserved voice slots */
    for (unsigned c = 0; c < HH_ALLOC_CLASSES; c++) p->msgs_per_slot[c] = 1;   /* TODO: depends on slot capacity */
    p->weight_video = 2;                                    /* TODO: class weights */
    p->weight_data = 1;
    p->join_reserve = 1;                                    /* TODO: idle slots kept for a joining radio */
    p->node_cap = (uint8_t)(slots_per_frame * 2u / 5u);     /* TODO: per-node cap, two fifths of the frame */
    if (p->node_cap == 0) p->node_cap = 1;
    p->stale_slots = 5u * slots_per_frame;                  /* TODO: five frames */
    p->gone_slots = 250u * slots_per_frame;                 /* TODO: five superframes of 50 frames */
}

/* Checks that the parameters can be used. */
hh_status_t hh_alloc_params_check(const hh_alloc_params_t *p)
{
    if (!p || p->slots_per_frame == 0 || p->slots_per_frame > HH_ALLOC_SLOTS_MAX) return HH_ERR_INVAL;
    for (unsigned c = 0; c < A_CLASSES; c++)
        if (p->msgs_per_slot[mac_class[c]] == 0) return HH_ERR_INVAL;
    if ((unsigned)p->weight_video + p->weight_data == 0) return HH_ERR_INVAL;
    return HH_OK;
}

/* Returns the number of set bits. */
static unsigned bits(uint64_t m)
{
    unsigned n = 0;
    for (; m; m &= m - 1u) n++;
    return n;
}

/* Adds a member in node order and works out what it wants beyond its keep-alive slot. The table holds at most 32 nodes. */
static void add_member(const hh_alloc_params_t *p, member_t *m, unsigned *n, const hh_alloc_demand_t *d, uint64_t age)
{
    member_t x;
    unsigned i = *n;
    uint32_t first = 1;   /* the keep-alive slot covers the first slot of demand, highest class first */

    memset(&x, 0, sizeof x);
    x.node = d->node;
    if (age <= p->stale_slots) {
        for (unsigned c = 0; c < A_CLASSES; c++) {
            uint32_t mps = p->msgs_per_slot[mac_class[c]];
            uint32_t need = ((uint32_t)d->depth[mac_class[c]] + mps - 1u) / mps;
            uint32_t take = need < first ? need : first;
            x.want[c] = need - take;
            first -= take;
        }
    }
    while (i > 0 && m[i - 1].node > x.node) {
        m[i] = m[i - 1];
        i--;
    }
    m[i] = x;
    (*n)++;
}

/* Slots a member holds so far: the keep-alive slot and what it was given. */
static unsigned held(const member_t *x)
{
    return 1u + x->got[A_VOICE] + x->got[A_VIDEO] + x->got[A_DATA];
}

/* Gives one slot at a time to each member that still wants more in class c and holds fewer than cap,
 * starting at member rot. Returns the slots given. */
static unsigned deal_class(member_t *m, unsigned n, unsigned c, unsigned pool, unsigned rot, unsigned cap)
{
    unsigned given = 0;
    bool progress = true;

    while (pool && progress) {
        progress = false;
        for (unsigned k = 0; k < n && pool; k++) {
            member_t *x = &m[(rot + k) % n];
            if (x->got[c] < x->want[c] && held(x) < cap) {
                x->got[c]++;
                pool--;
                given++;
                progress = true;
            }
        }
    }
    return given;
}

/* Computes the slot map from the demand table, the same way at every node. */
hh_status_t hh_alloc_compute(const hh_alloc_params_t *p, const hh_alloc_table_t *t, uint64_t now_slot,
                             uint32_t superframe, hh_alloc_map_t *out)
{
    member_t m[HH_ALLOC_NODES_MAX];
    hh_alloc_map_t map;
    uint64_t valid, avail;
    unsigned n = 0, navail, pool, rot, cursor, cap, quota_sum = 0, placed = 0, spare_end, idle;
    uint32_t tv = 0, td = 0;
    unsigned pv, pd;

    if (!t || !out || hh_alloc_params_check(p) != HH_OK) return HH_ERR_INVAL;
    valid = p->slots_per_frame >= 64u ? ~0ull : ((1ull << p->slots_per_frame) - 1u);
    avail = valid & ~p->fixed_slots;
    navail = bits(avail);

    for (unsigned i = 0; i < HH_ALLOC_NODES_MAX; i++) {
        uint64_t age;
        if (!t->entry[i].used) continue;
        age = hh_alloc_age(&t->entry[i], now_slot);
        if (age > p->gone_slots) continue;
        add_member(p, m, &n, &t->entry[i], age);
    }
    memset(&map, 0, sizeof map);
    if (n == 0) {
        *out = map;
        return HH_OK;
    }
    if (n > navail) return HH_ERR_NOMEM;
    rot = superframe % n;
    cap = p->node_cap ? p->node_cap : HH_ALLOC_SLOTS_MAX;
    pool = navail - n;   /* slots beyond one keep-alive slot per member */
    idle = p->join_reserve < pool ? p->join_reserve : pool;   /* held back from demand too, for a joining radio */
    pool -= idle;

    /* Voice first, from its reserve, lowest node id first. */
    {
        unsigned vpool = p->voice_reserve < pool ? p->voice_reserve : pool;
        for (unsigned i = 0; i < n && vpool; i++) {
            unsigned g = m[i].want[A_VOICE] < vpool ? m[i].want[A_VOICE] : vpool;
            if (g > cap - 1u) g = cap - 1u;
            m[i].got[A_VOICE] = g;
            vpool -= g;
            pool -= g;
        }
    }

    /* The rest between video and data by weight, then fairly between the nodes of each class. */
    for (unsigned i = 0; i < n; i++) {
        tv += m[i].want[A_VIDEO];
        td += m[i].want[A_DATA];
    }
    if (tv == 0 && td == 0) {
        pv = pd = 0;
    } else if (td == 0) {
        pv = pool < tv ? pool : tv;
        pd = 0;
    } else if (tv == 0) {
        pv = 0;
        pd = pool < td ? pool : td;
    } else {
        unsigned left, add;
        pv = pool * p->weight_video / ((unsigned)p->weight_video + p->weight_data);
        pd = pool - pv;
        if (pv > tv) pv = tv;
        if (pd > td) pd = td;
        left = pool - pv - pd;
        add = left < tv - pv ? left : tv - pv;
        pv += add;
        left -= add;
        add = left < td - pd ? left : td - pd;
        pd += add;
    }
    {
        unsigned left = pool;
        left -= deal_class(m, n, A_VIDEO, pv, rot, cap);
        left -= deal_class(m, n, A_DATA, pd, rot, cap);
        /* What a capped node could not take goes to whichever class still has takers. */
        left -= deal_class(m, n, A_VIDEO, left, rot, cap);
        deal_class(m, n, A_DATA, left, rot, cap);
    }

    for (unsigned i = 0; i < n; i++) {
        m[i].quota = held(&m[i]);
        quota_sum += m[i].quota;
    }
    /* Slots nobody asked for go round-robin to every member under the cap. A slot nobody may take stays idle. */
    spare_end = navail - idle;
    for (bool progress = true; quota_sum < spare_end && progress;) {
        progress = false;
        for (unsigned k = 0; k < n && quota_sum < spare_end; k++) {
            member_t *x = &m[(rot + k) % n];
            if (x->quota < cap) {
                x->quota++;
                quota_sum++;
                progress = true;
            }
        }
    }

    /* Place the slots in slot order, dealing to the members with quota left. */
    cursor = rot;
    for (unsigned s = 0; s < p->slots_per_frame && placed < quota_sum; s++) {
        if (!((avail >> s) & 1u)) continue;
        while (m[cursor % n].quota == 0) cursor++;
        map.owner[s] = m[cursor % n].node;
        m[cursor % n].quota--;
        placed++;
        cursor++;
    }
    *out = map;
    return HH_OK;
}

/* Voice slots wanted beyond the keep-alive slot for an offered load of msgs messages per frame. */
static unsigned voice_extra(const hh_alloc_params_t *p, unsigned msgs)
{
    unsigned per = p->msgs_per_slot[HH_MAC_CLASS_VOICE];
    unsigned need = (msgs + per - 1u) / per;

    return need > 1u ? need - 1u : 0u;
}

/* Adds up the voice slots the table asks for, with extra_msgs more messages per frame offered by node.
 * members counts the nodes that share the slots, node included. over_cap is set when the cap would
 * cut node's own voice. */
static void voice_demand(const hh_alloc_params_t *p, const hh_alloc_table_t *t, uint64_t now_slot, uint16_t node,
                         unsigned extra_msgs, unsigned *used, unsigned *members, bool *over_cap)
{
    unsigned cap = p->node_cap ? p->node_cap : HH_ALLOC_SLOTS_MAX;
    bool listed = false;

    *used = 0;
    *members = 0;
    *over_cap = false;
    for (unsigned i = 0; i < HH_ALLOC_NODES_MAX; i++) {
        const hh_alloc_demand_t *d = &t->entry[i];
        uint64_t age;
        unsigned msgs, want;

        if (!d->used) continue;
        age = hh_alloc_age(d, now_slot);
        if (age > p->gone_slots) continue;
        (*members)++;
        msgs = age <= p->stale_slots ? d->depth[HH_MAC_CLASS_VOICE] : 0u;
        if (d->node == node) {
            msgs += extra_msgs;
            listed = true;
        }
        want = voice_extra(p, msgs);
        if (d->node == node && want > cap - 1u) *over_cap = true;
        *used += want < cap - 1u ? want : cap - 1u;
    }
    if (node && !listed && extra_msgs) {   /* a node that has not reported yet joins the members with its first call */
        unsigned want = voice_extra(p, extra_msgs);
        (*members)++;
        if (want > cap - 1u) *over_cap = true;
        *used += want < cap - 1u ? want : cap - 1u;
    }
}

/* Voice slots the reserve can give: the reserve itself, or what is left after one slot for each member. */
static unsigned voice_pool(const hh_alloc_params_t *p, unsigned members)
{
    uint64_t valid = p->slots_per_frame >= 64u ? ~0ull : ((1ull << p->slots_per_frame) - 1u);
    unsigned navail = bits(valid & ~p->fixed_slots);
    unsigned room = members < navail ? navail - members : 0u;

    return p->voice_reserve < room ? p->voice_reserve : room;
}

/* Returns the voice slots of the reserve that no voice demand in the table takes. */
unsigned hh_alloc_voice_free(const hh_alloc_params_t *p, const hh_alloc_table_t *t, uint64_t now_slot)
{
    unsigned used, members, pool;
    bool over;

    if (!t || hh_alloc_params_check(p) != HH_OK) return 0;
    voice_demand(p, t, now_slot, 0, 0, &used, &members, &over);
    pool = voice_pool(p, members);
    return pool > used ? pool - used : 0u;
}

/* Returns true if node can offer msgs more voice messages per frame and every voice slot it needs is still given. */
bool hh_alloc_voice_admit(const hh_alloc_params_t *p, const hh_alloc_table_t *t, uint64_t now_slot, uint16_t node,
                          unsigned msgs)
{
    unsigned used, members;
    bool over;

    if (!t || node == 0 || msgs == 0 || hh_alloc_params_check(p) != HH_OK) return false;
    voice_demand(p, t, now_slot, node, msgs, &used, &members, &over);
    return !over && used <= voice_pool(p, members);
}
