/* Network entry. See include/hhsdr/mac/entry.h. */
#include "hhsdr/mac/entry.h"
#include "hhsdr/core/log.h"
#include <string.h>

#define COMP "entry"

/* ---- bit and byte helpers ---- */

/* Writes v as n bytes, low byte first. */
static void put_le(uint8_t *o, uint64_t v, unsigned n)
{
    for (unsigned i = 0; i < n; i++) o[i] = (uint8_t)(v >> (8u * i));
}

/* Reads n bytes, low byte first. */
static uint64_t get_le(const uint8_t *p, unsigned n)
{
    uint64_t v = 0;
    for (unsigned i = 0; i < n; i++) v |= (uint64_t)p[i] << (8u * i);
    return v;
}

/* Returns the slot mask held in n bytes of a bitmap. */
uint64_t hh_entry_mask_from_bytes(const uint8_t *b, unsigned n)
{
    return get_le(b, n > 8u ? 8u : n);
}

/* Returns the n lowest set bits of m. */
static uint64_t lowest_bits(uint64_t m, unsigned n)
{
    uint64_t r = 0;
    while (n-- && m) {
        uint64_t b = m & (~m + 1u);
        r |= b;
        m &= ~b;
    }
    return r;
}

/* ---- command codec ---- */

/* Writes one command body, section 4.7 of the frame specification. */
hh_status_t hh_entry_cmd_encode(const hh_entry_cmd_t *c, uint8_t *buf, size_t cap, size_t *out_len)
{
    size_t n;

    if (!c || !buf || !out_len) return HH_ERR_INVAL;
    switch (c->id) {
    case HH_ENTRY_CMD_REQUEST: {
        const hh_entry_request_t *r = &c->u.request;
        if (r->bitmap_bytes > HH_PDU_BITMAP_MAX) return HH_ERR_INVAL;
        n = 8u + r->bitmap_bytes;
        if (cap < n) return HH_ERR_NOMEM;
        put_le(buf + 1, r->request_id, 4);
        buf[5] = r->attempt;
        buf[6] = r->slots_wanted;
        buf[7] = r->bitmap_bytes;
        put_le(buf + 8, r->free_slots, r->bitmap_bytes);
        break;
    }
    case HH_ENTRY_CMD_GRANT: {
        const hh_entry_grant_t *g = &c->u.grant;
        if (g->bitmap_bytes > HH_PDU_BITMAP_MAX) return HH_ERR_INVAL;
        n = 12u + g->bitmap_bytes;
        if (cap < n) return HH_ERR_NOMEM;
        put_le(buf + 1, g->request_id, 4);
        put_le(buf + 5, g->apply_at, 4);
        buf[9] = g->bitmap_bytes;
        put_le(buf + 10, g->slots, g->bitmap_bytes);
        put_le(buf + 10 + g->bitmap_bytes, (uint16_t)g->arrival_offset, 2);
        break;
    }
    case HH_ENTRY_CMD_REJECT:
        n = 6u;
        if (cap < n) return HH_ERR_NOMEM;
        put_le(buf + 1, c->u.reject.request_id, 4);
        buf[5] = c->u.reject.reason;
        break;
    case HH_ENTRY_CMD_LEAVE: {
        const hh_entry_leave_t *l = &c->u.leave;
        if (l->bitmap_bytes > HH_PDU_BITMAP_MAX) return HH_ERR_INVAL;
        n = 6u + l->bitmap_bytes;
        if (cap < n) return HH_ERR_NOMEM;
        put_le(buf + 1, l->release_at, 4);
        buf[5] = l->bitmap_bytes;
        put_le(buf + 6, l->slots, l->bitmap_bytes);
        break;
    }
    default:
        return HH_ERR_INVAL;
    }
    buf[0] = c->id;
    *out_len = n;
    return HH_OK;
}

/* Reads one command body. The length must match the command exactly. */
hh_status_t hh_entry_cmd_decode(const uint8_t *buf, size_t len, hh_entry_cmd_t *c)
{
    if (!buf || !c) return HH_ERR_INVAL;
    if (len == 0) return HH_ERR_INVAL;
    memset(c, 0, sizeof *c);
    c->id = buf[0];
    switch (buf[0]) {
    case HH_ENTRY_CMD_REQUEST: {
        hh_entry_request_t *r = &c->u.request;
        if (len < 8u || buf[7] > HH_PDU_BITMAP_MAX || len != 8u + buf[7]) return HH_ERR_INVAL;
        r->request_id = (uint32_t)get_le(buf + 1, 4);
        r->attempt = buf[5];
        r->slots_wanted = buf[6];
        r->bitmap_bytes = buf[7];
        r->free_slots = get_le(buf + 8, r->bitmap_bytes);
        return HH_OK;
    }
    case HH_ENTRY_CMD_GRANT: {
        hh_entry_grant_t *g = &c->u.grant;
        if (len < 12u || buf[9] > HH_PDU_BITMAP_MAX || len != 12u + buf[9]) return HH_ERR_INVAL;
        g->request_id = (uint32_t)get_le(buf + 1, 4);
        g->apply_at = (uint32_t)get_le(buf + 5, 4);
        g->bitmap_bytes = buf[9];
        g->slots = get_le(buf + 10, g->bitmap_bytes);
        g->arrival_offset = (int16_t)get_le(buf + 10 + g->bitmap_bytes, 2);
        return HH_OK;
    }
    case HH_ENTRY_CMD_REJECT:
        if (len != 6u) return HH_ERR_INVAL;
        c->u.reject.request_id = (uint32_t)get_le(buf + 1, 4);
        c->u.reject.reason = buf[5];
        return HH_OK;
    case HH_ENTRY_CMD_LEAVE: {
        hh_entry_leave_t *l = &c->u.leave;
        if (len < 6u || buf[5] > HH_PDU_BITMAP_MAX || len != 6u + buf[5]) return HH_ERR_INVAL;
        l->release_at = (uint32_t)get_le(buf + 1, 4);
        l->bitmap_bytes = buf[5];
        l->slots = get_le(buf + 6, l->bitmap_bytes);
        return HH_OK;
    }
    default:
        return HH_ERR_UNSUPPORTED;
    }
}

/* ---- parameters and setup ---- */

/* Fills p with the proposed values. */
void hh_entry_params_default(hh_entry_params_t *p)
{
    memset(p, 0, sizeof *p);
    p->n_listen = 2;
    p->be_min = 4;
    p->be_max = 6;
    p->r_max = 5;
    p->c_max = 3;
    p->t_grant = 3;
    p->t_holdoff = 5;      /* TODO: set from the time-to-net-ready budget, not decided */
    p->slots_wanted = 1;
    p->apply_lead = 2;     /* TODO: not set by the specification */
    p->t_confirm = 2;      /* TODO: not set by the specification */
    p->n_leave = 3;        /* TODO: not decided */
}

/* Mixes the seed and the node id into a non-zero generator state. */
static uint32_t mix_seed(uint32_t seed, uint32_t id)
{
    uint32_t h = seed * 0x9E3779B1u ^ (id + 0x7F4A7C15u) * 0x85EBCA6Bu;
    h ^= h >> 16;
    h *= 0x7FEB352Du;
    h ^= h >> 15;
    h *= 0x846CA68Bu;
    h ^= h >> 16;
    return h ? h : 1u;
}

/* Returns the next 32-bit value of the per-radio generator (xorshift32). */
static uint32_t rng_next(uint32_t *s)
{
    uint32_t x = *s;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *s = x;
    return x;
}

/* Validates the parameters and the slot layout, then resets the entry state. */
hh_status_t hh_entry_init(hh_entry_t *e, const hh_entry_params_t *p, const hh_slot_config_t *cfg, uint32_t seed)
{
    uint64_t valid;

    if (!e || !p || !cfg) return HH_ERR_INVAL;
    if (cfg->slots_per_frame == 0 || cfg->slots_per_frame > HH_SLOT_FRAME_MAX) return HH_ERR_INVAL;
    valid = cfg->slots_per_frame == 64u ? ~0ull : ((1ull << cfg->slots_per_frame) - 1u);
    if (!cfg->entry_slots || (cfg->entry_slots & ~valid) || (cfg->entry_slots & cfg->own_slots))
        return HH_ERR_INVAL;
    if (p->be_min > p->be_max || p->be_max > 15u || !p->r_max || !p->c_max || !p->t_grant ||
        !p->slots_wanted || !p->n_leave)
        return HH_ERR_INVAL;

    memset(e, 0, sizeof *e);
    e->p = *p;
    e->node_id = (uint16_t)cfg->node_id;
    e->spf = cfg->slots_per_frame;
    e->bitmap_bytes = (uint16_t)((cfg->slots_per_frame + 7u) / 8u);
    e->sp_frame = cfg->slots_per_frame;
    e->sp_super = (uint32_t)cfg->slots_per_frame * cfg->frames_per_superframe;
    e->valid = valid;
    e->entry_slots = cfg->entry_slots;
    e->rng = mix_seed(seed, cfg->node_id);
    e->own = cfg->own_slots;
    e->state = e->own ? HH_ENTRY_MEMBER : HH_ENTRY_OFF;
    return HH_OK;
}

/* Returns the entry state. */
hh_entry_state_t hh_entry_state(const hh_entry_t *e)
{
    return e ? e->state : HH_ENTRY_OFF;
}

/* ---- joining side ---- */

/* Starts listening: the heard-slot record is cleared and the listen end is set. */
static void start_listen(hh_entry_t *e, const hh_slot_pos_t *pos)
{
    e->state = HH_ENTRY_LISTEN;
    e->busy = 0;
    e->req_out = false;
    e->listen_end = pos->abs_slot + (uint64_t)e->p.n_listen * e->sp_super;
}

/* Draws the number of entry opportunities to skip from the current window. */
static void start_backoff(hh_entry_t *e)
{
    e->state = HH_ENTRY_BACKOFF;
    e->backoff = (rng_next(&e->rng) >> 8) % (1u << e->be);
    HH_LOGD(COMP, "backoff", "node=%u be=%u skip=%u", e->node_id, e->be, e->backoff);
}

/* Begins a new entry episode at the smallest window. */
static void new_episode(hh_entry_t *e, const hh_slot_pos_t *pos)
{
    e->be = e->p.be_min;
    e->tries = 0;
    e->cycles = 0;
    e->attempt = 0;
    start_listen(e, pos);
}

/* Gives up after the last cycle: raises the failure and waits before a new episode. */
static void give_up(hh_entry_t *e, const hh_slot_pos_t *pos, hh_entry_act_t *act)
{
    e->state = HH_ENTRY_HOLDOFF;
    e->holdoff_end = pos->abs_slot + (uint64_t)e->p.t_holdoff * e->sp_super;
    act->failed = true;
    act->fail_reason = HH_MAC_ENTRY_RETRIES_EXHAUSTED;
    HH_LOGW(COMP, "entry_gave_up", "node=%u cycles=%u requests=%u", e->node_id, e->cycles, e->st.requests_tx);
}

/* Ends one entry cycle: listen again, or give up after the last cycle. The window is kept. */
static void end_cycle(hh_entry_t *e, const hh_slot_pos_t *pos, hh_entry_act_t *act)
{
    e->tries = 0;
    e->cycles++;
    e->st.cycles++;
    if (e->cycles >= e->p.c_max) give_up(e, pos, act);
    else start_listen(e, pos);
}

/* Counts one failed request: the window grows, then back-off again or a new cycle. */
static void fail_attempt(hh_entry_t *e, const hh_slot_pos_t *pos, hh_entry_act_t *act)
{
    e->req_out = false;
    if (e->be < e->p.be_max) e->be++;
    e->tries++;
    if (e->tries >= e->p.r_max) end_cycle(e, pos, act);
    else start_backoff(e);
}

/* Starts joining. Only a radio that owns no slot may join. */
hh_status_t hh_entry_join(hh_entry_t *e, const hh_slot_pos_t *pos)
{
    if (!e || !pos) return HH_ERR_INVAL;
    if (e->state != HH_ENTRY_OFF && e->state != HH_ENTRY_REFUSED) return HH_ERR_STATE;
    e->st.t_start = pos->abs_slot;
    e->st.joined = false;
    new_episode(e, pos);
    HH_LOGI(COMP, "join_start", "node=%u sf=%u slot=%u", e->node_id, pos->superframe, pos->slot);
    return HH_OK;
}

/* A shared entry slot has come: count it down, or send the request when the count is out. */
bool hh_entry_on_opportunity(hh_entry_t *e, const hh_slot_pos_t *pos, hh_entry_cmd_t *cmd)
{
    hh_entry_request_t *r;
    uint32_t id;

    if (!e || !pos || !cmd || e->state != HH_ENTRY_BACKOFF || !((e->entry_slots >> pos->slot) & 1u)) return false;
    if (e->backoff) {
        e->backoff--;
        return false;
    }
    do { id = rng_next(&e->rng); } while (id == e->req_id);
    e->req_id = id;
    if (e->attempt < 255u) e->attempt++;

    memset(cmd, 0, sizeof *cmd);
    cmd->id = HH_ENTRY_CMD_REQUEST;
    r = &cmd->u.request;
    r->request_id = id;
    r->attempt = e->attempt;
    r->slots_wanted = e->p.slots_wanted;
    r->bitmap_bytes = (uint8_t)e->bitmap_bytes;
    r->free_slots = e->valid & ~e->busy & ~e->entry_slots;

    e->req_out = true;
    e->state = HH_ENTRY_WAIT_GRANT;
    e->deadline = pos->abs_slot + (uint64_t)e->p.t_grant * e->sp_frame;
    e->st.requests_tx++;
    HH_LOGD(COMP, "request_tx", "node=%u id=%08x attempt=%u be=%u", e->node_id, id, e->attempt, e->be);
    return true;
}

/* The grant answers the outstanding request: keep it until the apply-at superframe. */
static void joiner_accept_grant(hh_entry_t *e, const hh_slot_pos_t *pos, const hh_entry_grant_t *g)
{
    if (!g->slots || (g->slots & ~e->valid) || (g->slots & e->entry_slots)) {
        e->st.bad_grants++;
        return;
    }
    e->granted = g->slots;
    e->apply_at = g->apply_at;
    e->req_out = false;
    e->state = HH_ENTRY_GRANTED;
    e->st.t_granted = pos->abs_slot;
    HH_LOGD(COMP, "grant_rx", "node=%u slots=%llx apply_at=%u", e->node_id, (unsigned long long)g->slots,
            g->apply_at);
}

/* The rejection answers the outstanding request: act on the reason. */
static void joiner_accept_reject(hh_entry_t *e, const hh_slot_pos_t *pos, const hh_entry_reject_t *r,
                                 hh_entry_act_t *act)
{
    e->req_out = false;
    e->st.rejects_rx++;
    switch (r->reason) {
    case HH_ENTRY_REJECT_FULL:
        end_cycle(e, pos, act);
        break;
    case HH_ENTRY_REJECT_NOT_AUTHORISED:
        e->state = HH_ENTRY_REFUSED;
        act->failed = true;
        act->fail_reason = HH_MAC_ENTRY_NOT_AUTHORISED;
        break;
    default:   /* bad request, or a value this version does not know */
        e->state = HH_ENTRY_REFUSED;   /* TODO: report the fault; the MAC has no reason code for it */
        break;
    }
}

/* ---- member side ---- */

/* Returns the slots this radio knows to be taken: its own and every peer's claims. */
static uint64_t taken_slots(const hh_entry_t *e)
{
    uint64_t t = e->own;
    for (unsigned i = 0; i < HH_ENTRY_PEERS_MAX; i++)
        if (e->peers[i].used) t |= e->peers[i].claims;
    return t;
}

/* Returns the slots held by grants not yet confirmed, other than the one at skip. */
static uint64_t provisional_slots(const hh_entry_t *e, const hh_entry_prov_t *skip)
{
    uint64_t t = 0;
    for (unsigned i = 0; i < HH_ENTRY_PROV_MAX; i++)
        if (e->prov[i].used && !e->prov[i].is_reject && &e->prov[i] != skip) t |= e->prov[i].slots;
    return t;
}

/* Returns the provisional entry of a joining radio, or NULL. */
static hh_entry_prov_t *find_prov(hh_entry_t *e, uint16_t src)
{
    for (unsigned i = 0; i < HH_ENTRY_PROV_MAX; i++)
        if (e->prov[i].used && e->prov[i].src == src) return &e->prov[i];
    return NULL;
}

/* Returns a free provisional entry, or NULL when the table is full. */
static hh_entry_prov_t *alloc_prov(hh_entry_t *e)
{
    for (unsigned i = 0; i < HH_ENTRY_PROV_MAX; i++)
        if (!e->prov[i].used) {
            memset(&e->prov[i], 0, sizeof e->prov[i]);
            return &e->prov[i];
        }
    return NULL;
}

/* Answers a request: a grant of free slots, or a rejection. A repeated request gets the same answer. */
static void member_on_request(hh_entry_t *e, const hh_slot_pos_t *pos, uint16_t src, uint16_t dst,
                              const hh_entry_request_t *req)
{
    hh_entry_prov_t *p;
    uint64_t cand;

    if (e->state != HH_ENTRY_MEMBER || e->leaving) return;
    if (dst != HH_PDU_ADDR_BROADCAST || !((e->entry_slots >> pos->slot) & 1u)) {
        e->st.bad_commands++;
        return;
    }
    e->st.requests_rx++;
    p = find_prov(e, src);
    if (p && p->request_id == req->request_id) {
        e->st.duplicates++;
        if (p->mine) p->pending = true;
        return;
    }
    if (p) p->used = false;   /* a new request id: the earlier unconfirmed grant is released */
    p = alloc_prov(e);
    if (!p) {
        e->st.table_full++;
        return;
    }
    p->used = true;
    p->mine = true;
    p->pending = true;
    p->src = src;
    p->request_id = req->request_id;
    p->expire_sf = pos->superframe + e->p.apply_lead + e->p.t_confirm + 1u;

    if (!req->slots_wanted || req->bitmap_bytes != e->bitmap_bytes) {
        p->is_reject = true;
        p->reason = HH_ENTRY_REJECT_BAD_REQUEST;
        return;
    }
    cand = req->free_slots & e->valid & ~e->entry_slots & ~taken_slots(e) & ~provisional_slots(e, p);
    if (!cand) {
        e->st.no_free_slot++;
        p->is_reject = true;
        p->reason = HH_ENTRY_REJECT_FULL;
        return;
    }
    p->slots = lowest_bits(cand, req->slots_wanted);
}

/* Another member answered: cancel this radio's own pending answer and keep the slots reserved. */
static void member_heard_answer(hh_entry_t *e, uint16_t joiner, const hh_entry_cmd_t *c)
{
    hh_entry_prov_t *p = find_prov(e, joiner);
    uint32_t id = c->id == HH_ENTRY_CMD_GRANT ? c->u.grant.request_id : c->u.reject.request_id;

    if (p && p->request_id == id) {
        if (p->mine && p->pending) {
            p->pending = false;
            p->mine = false;
            if (c->id == HH_ENTRY_CMD_REJECT) { p->used = false; return; }
            p->is_reject = false;
            p->slots = c->u.grant.slots;
            p->apply_at = c->u.grant.apply_at;
            p->assigned = true;
            p->expire_sf = c->u.grant.apply_at + e->p.t_confirm;
        } else if (c->id == HH_ENTRY_CMD_GRANT) {
            p->slots |= c->u.grant.slots;   /* answered twice: hold both until one lapses */
        }
        return;
    }
    if (c->id != HH_ENTRY_CMD_GRANT) return;
    if (p) p->used = false;
    p = alloc_prov(e);
    if (!p) {
        e->st.table_full++;
        return;
    }
    p->used = true;
    p->src = joiner;
    p->request_id = id;
    p->slots = c->u.grant.slots;
    p->apply_at = c->u.grant.apply_at;
    p->assigned = true;
    p->expire_sf = c->u.grant.apply_at + e->p.t_confirm;
}

/* Handles one MAC command message, whatever its destination. */
void hh_entry_on_command(hh_entry_t *e, const hh_slot_pos_t *pos, uint16_t src, uint16_t dst,
                         const uint8_t *body, size_t len, hh_entry_act_t *act)
{
    hh_entry_cmd_t c;
    hh_status_t rc;
    bool mine;

    if (!act) return;
    memset(act, 0, sizeof *act);
    if (!e || !pos || !body) return;
    rc = hh_entry_cmd_decode(body, len, &c);
    if (rc == HH_ERR_UNSUPPORTED) { e->st.unknown_commands++; return; }
    if (rc != HH_OK) { e->st.bad_commands++; return; }

    switch (c.id) {
    case HH_ENTRY_CMD_REQUEST:
        member_on_request(e, pos, src, dst, &c.u.request);
        break;
    case HH_ENTRY_CMD_GRANT:
    case HH_ENTRY_CMD_REJECT:
        /* Accepted only when it is addressed to this radio and names its request (rule 26). */
        mine = dst == e->node_id;
        if (mine && e->state == HH_ENTRY_WAIT_GRANT && e->req_out) {
            if (c.id == HH_ENTRY_CMD_GRANT && c.u.grant.request_id == e->req_id)
                joiner_accept_grant(e, pos, &c.u.grant);
            else if (c.id == HH_ENTRY_CMD_REJECT && c.u.reject.request_id == e->req_id)
                joiner_accept_reject(e, pos, &c.u.reject, act);
            else
                e->st.ignored_responses++;
        } else if (mine) {
            e->st.ignored_responses++;
        } else {
            if (c.id == HH_ENTRY_CMD_GRANT) e->busy |= c.u.grant.slots;
            if (e->state == HH_ENTRY_MEMBER) member_heard_answer(e, dst, &c);
        }
        break;
    case HH_ENTRY_CMD_LEAVE:
        for (unsigned i = 0; i < HH_ENTRY_PEERS_MAX; i++)
            if (e->peers[i].used && e->peers[i].src == src) {
                e->peers[i].release_at = c.u.leave.release_at;
                e->peers[i].release_slots = c.u.leave.slots;
            }
        break;
    default:
        break;
    }
}

/* A good PDU was heard: record the slot as in use, the sender's claims, and confirm a grant it uses. */
void hh_entry_on_pdu(hh_entry_t *e, const hh_slot_pos_t *pos, uint16_t src, uint64_t claims)
{
    hh_entry_peer_t *peer = NULL, *spare = NULL;
    hh_entry_prov_t *p;

    if (!e || !pos) return;
    if (!((e->entry_slots >> pos->slot) & 1u)) e->busy |= 1ull << pos->slot;
    e->busy |= claims;
    if (!claims) return;

    for (unsigned i = 0; i < HH_ENTRY_PEERS_MAX; i++) {
        if (e->peers[i].used && e->peers[i].src == src) peer = &e->peers[i];
        else if (!e->peers[i].used && !spare) spare = &e->peers[i];
    }
    if (!peer) {
        if (!spare) { e->st.table_full++; return; }
        peer = spare;
        memset(peer, 0, sizeof *peer);
        peer->used = true;
        peer->src = src;
    }
    peer->claims = claims;

    p = find_prov(e, src);
    if (p && !p->is_reject && (p->slots & claims)) p->used = false;   /* confirmed by its first burst */
}

/* Builds the next command for this radio's own-slot PDU: a pending answer, or the leave. */
bool hh_entry_next_command(hh_entry_t *e, const hh_slot_pos_t *pos, uint16_t body_max, uint8_t *buf,
                           size_t *len, uint16_t *dst)
{
    hh_entry_cmd_t c;

    if (!e || !pos || !buf || !len || !dst) return false;
    for (unsigned i = 0; i < HH_ENTRY_PROV_MAX; i++) {
        hh_entry_prov_t *p = &e->prov[i];
        if (!p->used || !p->mine || !p->pending) continue;
        memset(&c, 0, sizeof c);
        if (p->is_reject) {
            c.id = HH_ENTRY_CMD_REJECT;
            c.u.reject.request_id = p->request_id;
            c.u.reject.reason = p->reason;
        } else {
            c.id = HH_ENTRY_CMD_GRANT;
            c.u.grant.request_id = p->request_id;
            c.u.grant.apply_at = p->assigned ? p->apply_at : pos->superframe + e->p.apply_lead;
            c.u.grant.bitmap_bytes = (uint8_t)e->bitmap_bytes;
            c.u.grant.slots = p->slots;
            c.u.grant.arrival_offset = HH_ENTRY_OFFSET_NONE;   /* TODO: the logic does not report it yet */
        }
        if ((c.id == HH_ENTRY_CMD_REJECT ? 6u : 12u + e->bitmap_bytes) > body_max) continue;
        if (hh_entry_cmd_encode(&c, buf, HH_ENTRY_CMD_MAX, len) != HH_OK) continue;
        if (!p->is_reject && !p->assigned) {
            p->apply_at = c.u.grant.apply_at;
            p->assigned = true;
            p->expire_sf = p->apply_at + e->p.t_confirm;
        }
        if (p->is_reject) p->expire_sf = pos->superframe + e->p.t_confirm;
        p->pending = false;
        *dst = p->src;
        if (p->is_reject) e->st.rejects_tx++;
        else e->st.grants_tx++;
        return true;
    }

    /* One leave per superframe, from the superframe after the leave started. */
    if (e->state == HH_ENTRY_MEMBER && e->leaving && e->leave_sent < e->p.n_leave &&
        pos->superframe >= e->leave_release - e->p.n_leave && pos->superframe != e->leave_sf) {
        c.id = HH_ENTRY_CMD_LEAVE;
        memset(&c.u, 0, sizeof c.u);
        c.u.leave.release_at = e->leave_release;
        c.u.leave.bitmap_bytes = (uint8_t)e->bitmap_bytes;
        c.u.leave.slots = e->own;
        if (6u + e->bitmap_bytes > body_max) return false;
        if (hh_entry_cmd_encode(&c, buf, HH_ENTRY_CMD_MAX, len) != HH_OK) return false;
        e->leave_sent++;
        e->leave_sf = pos->superframe;
        *dst = HH_PDU_ADDR_BROADCAST;
        return true;
    }
    return false;
}

/* Starts leaving: the command goes out in n_leave consecutive superframes, starting with the next. */
hh_status_t hh_entry_leave(hh_entry_t *e, const hh_slot_pos_t *pos)
{
    if (!e || !pos) return HH_ERR_INVAL;
    if (e->state != HH_ENTRY_MEMBER || e->leaving || !e->own) return HH_ERR_STATE;
    e->leaving = true;
    e->leave_sent = 0;
    e->leave_sf = 0;
    e->leave_release = pos->superframe + e->p.n_leave + 1u;
    HH_LOGI(COMP, "leave_start", "node=%u sf=%u release_at=%u", e->node_id, pos->superframe, e->leave_release);
    return HH_OK;
}

/* The first own-slot PDU after joining is the confirmation of the grant. */
void hh_entry_on_own_tx(hh_entry_t *e, const hh_slot_pos_t *pos)
{
    if (!e || !pos || !e->first_tx_pending) return;
    e->first_tx_pending = false;
    e->st.t_joined = pos->abs_slot;
    e->st.joined = true;
    HH_LOGI(COMP, "joined", "node=%u slot=%u sf=%u requests=%u", e->node_id, pos->slot, pos->superframe,
            e->st.requests_tx);
}

/* ---- per-slot timers ---- */

/* Runs the timers of this slot: releases, lapsing, timeouts and apply-at. */
void hh_entry_tick(hh_entry_t *e, const hh_slot_pos_t *pos, hh_entry_act_t *act)
{
    if (!act) return;
    memset(act, 0, sizeof *act);
    if (!e || !pos) return;

    for (unsigned i = 0; i < HH_ENTRY_PEERS_MAX; i++) {
        hh_entry_peer_t *q = &e->peers[i];
        if (q->used && q->release_at && pos->superframe >= q->release_at) {
            q->claims &= ~q->release_slots;
            q->release_at = 0;
            if (!q->claims) q->used = false;
        }
    }
    for (unsigned i = 0; i < HH_ENTRY_PROV_MAX; i++) {
        hh_entry_prov_t *p = &e->prov[i];
        if (p->used && pos->superframe > p->expire_sf) {
            if (!p->is_reject) e->st.lapsed_grants++;
            p->used = false;
        }
    }

    switch (e->state) {
    case HH_ENTRY_LISTEN:
        if (pos->abs_slot >= e->listen_end) start_backoff(e);
        break;
    case HH_ENTRY_WAIT_GRANT:
        if (pos->abs_slot >= e->deadline) {
            e->st.timeouts++;
            HH_LOGD(COMP, "grant_timeout", "node=%u be=%u tries=%u", e->node_id, e->be, e->tries + 1u);
            fail_attempt(e, pos, act);
        }
        break;
    case HH_ENTRY_GRANTED:
        if (pos->superframe > e->apply_at + e->p.t_confirm) {
            fail_attempt(e, pos, act);   /* applied too late: the slots must not be used */
        } else if (pos->superframe >= e->apply_at) {
            e->own = e->granted;
            e->state = HH_ENTRY_MEMBER;
            e->first_tx_pending = true;
            act->own_changed = true;
            act->own_slots = e->own;
        }
        break;
    case HH_ENTRY_HOLDOFF:
        if (pos->abs_slot >= e->holdoff_end) new_episode(e, pos);
        break;
    case HH_ENTRY_MEMBER:
        if (e->leaving && pos->superframe >= e->leave_release) {
            e->own = 0;
            e->leaving = false;
            e->state = HH_ENTRY_OFF;
            act->own_changed = true;
            act->own_slots = 0;
        }
        break;
    default:
        break;
    }
}
