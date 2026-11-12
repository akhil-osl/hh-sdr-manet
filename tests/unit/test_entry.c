/*
 * Network entry: the MAC commands against vectors D and E and the reject and
 * leave bodies of MAC frame specification v2 part 6, then the joining and
 * member rules (part 5 transmit rules 14 and 15, receive rule 11) driven slot
 * by slot with no clock. Links hhsdr_mac only.
 */
#include "hhsdr/core/log.h"
#include "hhsdr/mac/entry.h"
#include "hhsdr/mac/pdu.h"
#include "hh_test.h"
#include <string.h>

#define NET        0x0010u
#define SPF        20u
#define FPS        50u
#define ENTRY_SLOT 19u
#define SUPER      (SPF * FPS)

static const uint8_t vec_d[] = {
    0x41, 0xA8, 0x01, 0x10, 0x00, 0xFF, 0xFF, 0x03, 0x00, 0x14, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x20, 0xFF, 0xFF, 0x0B, 0x00, 0x01, 0xE7, 0x91, 0x3C, 0x5A, 0x01, 0x01, 0x03, 0xDE, 0xFD,
    0x0F, 0xE2, 0x55, 0x5B, 0x6A };
static const uint8_t vec_e[] = {
    0x41, 0xA8, 0x23, 0x10, 0x00, 0xFF, 0xFF, 0x01, 0x00, 0x10, 0x00, 0x00, 0x00, 0x00, 0x03,
    0x20, 0x00, 0x00, 0x20, 0x03, 0x00, 0x0F, 0x00, 0x02, 0xE7, 0x91, 0x3C, 0x5A, 0xBE, 0xA4,
    0x7F, 0x01, 0x03, 0x00, 0x40, 0x00, 0x4E, 0x01, 0x92, 0xEB, 0x30, 0xE7 };
static const uint8_t body_d[] = { 0x01, 0xE7, 0x91, 0x3C, 0x5A, 0x01, 0x01, 0x03, 0xDE, 0xFD, 0x0F };
static const uint8_t body_e[] = { 0x02, 0xE7, 0x91, 0x3C, 0x5A, 0xBE, 0xA4, 0x7F, 0x01, 0x03, 0x00, 0x40,
                                  0x00, 0x4E, 0x01 };
static const uint8_t body_reject[] = { 0x03, 0xE7, 0x91, 0x3C, 0x5A, 0x01 };
static const uint8_t body_leave[] = { 0x04, 0xBF, 0xA4, 0x7F, 0x01, 0x03, 0x00, 0x02, 0x00 };

/* ---- helpers ---- */

/* Position of an absolute slot in the 20-slot, 50-frame layout. */
static hh_slot_pos_t pos_of(uint64_t abs)
{
    hh_slot_pos_t p;
    p.abs_slot = abs;
    p.superframe = (uint32_t)(abs / SUPER);
    p.frame = (uint16_t)((abs % SUPER) / SPF);
    p.slot = (uint16_t)(abs % SPF);
    return p;
}

/* Starts an entry object for a radio with the given own slots. */
static void make(hh_entry_t *e, unsigned id, uint64_t own, uint32_t seed)
{
    hh_slot_config_t c;
    hh_entry_params_t p;
    hh_slot_config_default(&c, id, NET);
    c.own_slots = own;
    c.entry_slots = 1ull << ENTRY_SLOT;
    hh_entry_params_default(&p);
    HH_ASSERT_OK(hh_entry_init(e, &p, &c, seed));
}

/* Encodes a command and hands it to a radio as if heard in the given slot. */
static void deliver(hh_entry_t *e, uint64_t abs, uint16_t src, uint16_t dst, const hh_entry_cmd_t *c,
                    hh_entry_act_t *act)
{
    uint8_t b[HH_ENTRY_CMD_MAX];
    size_t n = 0;
    hh_slot_pos_t p = pos_of(abs);
    HH_ASSERT_OK(hh_entry_cmd_encode(c, b, sizeof b, &n));
    hh_entry_on_command(e, &p, src, dst, b, n, act);
}

/* A joining radio driven one slot at a time. */
typedef struct {
    hh_entry_t      e;
    uint64_t        abs;
    bool            failed;
    hh_mac_entry_fail_t reason;
    hh_entry_cmd_t  req;
} jd_t;

/* Starts a joining radio at slot 0. */
static void jd_start(jd_t *d, unsigned id, uint32_t seed)
{
    hh_slot_pos_t p = pos_of(0);
    memset(d, 0, sizeof *d);
    make(&d->e, id, 0, seed);
    HH_ASSERT_OK(hh_entry_join(&d->e, &p));
}

/* Runs slots until the radio sends a request. Returns its slot, or UINT64_MAX after max slots. */
static uint64_t jd_run(jd_t *d, uint64_t max)
{
    for (uint64_t i = 0; i < max; i++) {
        hh_slot_pos_t p = pos_of(d->abs);
        hh_entry_act_t act;
        hh_entry_tick(&d->e, &p, &act);
        if (act.failed) { d->failed = true; d->reason = act.fail_reason; }
        d->abs++;
        if (p.slot == ENTRY_SLOT && hh_entry_on_opportunity(&d->e, &p, &d->req)) return p.abs_slot;
    }
    return UINT64_MAX;
}

/* Builds the PDU of a vector from its fields and compares the bytes. */
static void check_pdu(const uint8_t *body, size_t blen, uint16_t src, uint8_t seq, uint8_t tq, uint8_t lbytes,
                      uint8_t claim0, uint16_t dst, const uint8_t *vec, size_t vlen)
{
    hh_pdu_t p;
    uint8_t buf[64];
    size_t n = 0;
    memset(&p, 0, sizeof p);
    p.seq = seq;
    p.net_id = NET;
    p.src = src;
    p.time_quality = tq;
    p.slot_bytes = lbytes;
    p.claims[0] = claim0;
    p.n_msg = 1;
    p.msg[0] = (hh_pdu_msg_t){ .proto = HH_PDU_PROTO_COMMAND, .dst = dst, .cls = 0, .data = body,
                               .len = (uint16_t)blen };
    HH_ASSERT_OK(hh_pdu_encode(&p, buf, sizeof buf, &n));
    HH_ASSERT_EQ_INT((int)n, (int)vlen);
    HH_ASSERT(memcmp(buf, vec, vlen) == 0);
}

/* ---- codec ---- */

/* Vector D: the entry request, as a body and as the whole PDU. */
static void test_vector_d_request(void)
{
    hh_entry_cmd_t c, back;
    uint8_t b[HH_ENTRY_CMD_MAX];
    size_t n = 0;

    memset(&c, 0, sizeof c);
    c.id = HH_ENTRY_CMD_REQUEST;
    c.u.request = (hh_entry_request_t){ .request_id = 0x5A3C91E7u, .attempt = 1, .slots_wanted = 1,
                                        .bitmap_bytes = 3, .free_slots = 0x0FFDDEu };
    HH_ASSERT_OK(hh_entry_cmd_encode(&c, b, sizeof b, &n));
    HH_ASSERT_EQ_INT((int)n, (int)sizeof body_d);
    HH_ASSERT(memcmp(b, body_d, n) == 0);
    check_pdu(b, n, 3, 0x01, 0x14, 0, 0, HH_PDU_ADDR_BROADCAST, vec_d, sizeof vec_d);

    HH_ASSERT_OK(hh_entry_cmd_decode(body_d, sizeof body_d, &back));
    HH_ASSERT_EQ_INT(back.id, HH_ENTRY_CMD_REQUEST);
    HH_ASSERT(back.u.request.request_id == 0x5A3C91E7u);
    HH_ASSERT_EQ_INT(back.u.request.attempt, 1);
    HH_ASSERT_EQ_INT(back.u.request.slots_wanted, 1);
    HH_ASSERT(back.u.request.free_slots == 0x0FFDDEu);   /* all free except 0, 5 and 9 */
}

/* Vector E: the entry grant of slot 14 with an arrival offset of +33.4 us. */
static void test_vector_e_grant(void)
{
    hh_entry_cmd_t c, back;
    uint8_t b[HH_ENTRY_CMD_MAX];
    size_t n = 0;

    memset(&c, 0, sizeof c);
    c.id = HH_ENTRY_CMD_GRANT;
    c.u.grant = (hh_entry_grant_t){ .request_id = 0x5A3C91E7u, .apply_at = 25142462u, .bitmap_bytes = 3,
                                    .slots = 1ull << 14, .arrival_offset = 334 };
    HH_ASSERT_OK(hh_entry_cmd_encode(&c, b, sizeof b, &n));
    HH_ASSERT_EQ_INT((int)n, (int)sizeof body_e);
    HH_ASSERT(memcmp(b, body_e, n) == 0);
    check_pdu(b, n, 1, 0x23, 0x10, 3, 0x20, 3, vec_e, sizeof vec_e);

    HH_ASSERT_OK(hh_entry_cmd_decode(body_e, sizeof body_e, &back));
    HH_ASSERT(back.u.grant.apply_at == 25142462u);
    HH_ASSERT(back.u.grant.slots == (1ull << 14));
    HH_ASSERT_EQ_INT(back.u.grant.arrival_offset, 334);
}

/* The reject and leave bodies of part 6. */
static void test_reject_and_leave_bodies(void)
{
    hh_entry_cmd_t c, back;
    uint8_t b[HH_ENTRY_CMD_MAX];
    size_t n = 0;

    memset(&c, 0, sizeof c);
    c.id = HH_ENTRY_CMD_REJECT;
    c.u.reject = (hh_entry_reject_t){ .request_id = 0x5A3C91E7u, .reason = HH_ENTRY_REJECT_FULL };
    HH_ASSERT_OK(hh_entry_cmd_encode(&c, b, sizeof b, &n));
    HH_ASSERT_EQ_INT((int)n, (int)sizeof body_reject);
    HH_ASSERT(memcmp(b, body_reject, n) == 0);
    HH_ASSERT_OK(hh_entry_cmd_decode(body_reject, sizeof body_reject, &back));
    HH_ASSERT_EQ_INT(back.u.reject.reason, 1);

    memset(&c, 0, sizeof c);
    c.id = HH_ENTRY_CMD_LEAVE;
    c.u.leave = (hh_entry_leave_t){ .release_at = 25142463u, .bitmap_bytes = 3, .slots = 1ull << 9 };
    HH_ASSERT_OK(hh_entry_cmd_encode(&c, b, sizeof b, &n));
    HH_ASSERT_EQ_INT((int)n, (int)sizeof body_leave);
    HH_ASSERT(memcmp(b, body_leave, n) == 0);
    HH_ASSERT_OK(hh_entry_cmd_decode(body_leave, sizeof body_leave, &back));
    HH_ASSERT(back.u.leave.release_at == 25142463u);
    HH_ASSERT(back.u.leave.slots == (1ull << 9));
}

/* Wrong lengths are refused, an unknown command is reported so it can be skipped. */
static void test_decode_refusals(void)
{
    hh_entry_cmd_t c;
    uint8_t b[HH_ENTRY_CMD_MAX];

    HH_ASSERT_ERR(hh_entry_cmd_decode(body_d, sizeof body_d - 1, &c), HH_ERR_INVAL);
    HH_ASSERT_ERR(hh_entry_cmd_decode(body_e, sizeof body_e + 1, &c), HH_ERR_INVAL);
    HH_ASSERT_ERR(hh_entry_cmd_decode(body_reject, 5, &c), HH_ERR_INVAL);
    HH_ASSERT_ERR(hh_entry_cmd_decode(body_leave, 5, &c), HH_ERR_INVAL);
    memcpy(b, body_d, sizeof body_d);
    b[7] = 9;   /* bitmap longer than the PDU codec allows */
    HH_ASSERT_ERR(hh_entry_cmd_decode(b, sizeof body_d, &c), HH_ERR_INVAL);
    b[0] = 0x07;
    HH_ASSERT_ERR(hh_entry_cmd_decode(b, sizeof body_d, &c), HH_ERR_UNSUPPORTED);
    HH_ASSERT_ERR(hh_entry_cmd_decode(b, 0, &c), HH_ERR_INVAL);
}

/* ---- joining side ---- */

/* The first request waits for the listen time and a back-off drawn from the first window. */
static void test_first_request_after_listen_and_backoff(void)
{
    jd_t d;
    uint64_t at;
    hh_slot_pos_t p = pos_of(100);

    jd_start(&d, 5, 1);
    /* Another radio heard in slots 0 and 3, claiming slot 7. */
    hh_entry_on_pdu(&d.e, &p, 1, 0);   /* heard in slot 100 % 20 = 0 */
    p = pos_of(103);
    hh_entry_on_pdu(&d.e, &p, 2, 1ull << 7);

    at = jd_run(&d, 3 * SUPER);
    HH_ASSERT(at != UINT64_MAX);
    HH_ASSERT(at >= 2u * SUPER);                                  /* after the listen time */
    HH_ASSERT(at <= 2u * SUPER + 16u * SPF);                       /* inside the first window */
    HH_ASSERT_EQ_INT(d.req.id, HH_ENTRY_CMD_REQUEST);
    HH_ASSERT_EQ_INT(d.req.u.request.attempt, 1);
    HH_ASSERT_EQ_INT(d.req.u.request.slots_wanted, 1);
    HH_ASSERT_EQ_INT(d.req.u.request.bitmap_bytes, 3);
    HH_ASSERT(!(d.req.u.request.free_slots & (1ull << 0)));       /* heard in use */
    HH_ASSERT(!(d.req.u.request.free_slots & (1ull << 3)));
    HH_ASSERT(!(d.req.u.request.free_slots & (1ull << 7)));       /* claimed */
    HH_ASSERT(!(d.req.u.request.free_slots & (1ull << ENTRY_SLOT)));
    HH_ASSERT(d.req.u.request.free_slots & (1ull << 1));
    HH_ASSERT_EQ_INT(hh_entry_state(&d.e), HH_ENTRY_WAIT_GRANT);
}

/* Radios with the same seed but different ids draw different back-offs (the named failure). */
static void test_backoff_differs_per_radio(void)
{
    uint64_t first[8];
    unsigned distinct = 0;

    for (unsigned i = 0; i < 8; i++) {
        jd_t d;
        jd_start(&d, i + 2, 1);
        first[i] = jd_run(&d, 3 * SUPER);
        HH_ASSERT(first[i] != UINT64_MAX);
    }
    for (unsigned i = 0; i < 8; i++) {
        bool seen = false;
        for (unsigned j = 0; j < i; j++) if (first[j] == first[i]) seen = true;
        if (!seen) distinct++;
    }
    HH_ASSERT(distinct >= 4);
}

/* No response: the window grows to its limit, five requests end a cycle, three cycles give up. */
static void test_timeout_window_cycles_and_holdoff(void)
{
    jd_t d;
    uint8_t be_seen[16];
    unsigned n = 0;
    uint64_t at, prev = 0;
    hh_entry_params_t p;

    hh_entry_params_default(&p);
    jd_start(&d, 5, 3);
    while (n < p.r_max * p.c_max) {
        at = jd_run(&d, 4 * SUPER);
        HH_ASSERT(at != UINT64_MAX);
        if (n) HH_ASSERT(at >= prev + p.t_grant * SPF);            /* waited for the response time */
        prev = at;
        be_seen[n++] = d.e.be;
    }
    HH_ASSERT_EQ_INT(be_seen[0], 4);
    HH_ASSERT_EQ_INT(be_seen[1], 5);
    HH_ASSERT_EQ_INT(be_seen[2], 6);
    HH_ASSERT_EQ_INT(be_seen[3], 6);                               /* capped at the largest exponent */
    HH_ASSERT_EQ_INT(be_seen[5], 6);                               /* kept into the second cycle */
    HH_ASSERT(!d.failed);

    /* The last request goes unanswered: the entry fails and the radio holds off. */
    HH_ASSERT_EQ_INT(jd_run(&d, p.t_grant * SPF + 2) == UINT64_MAX, 1);
    HH_ASSERT(d.failed);
    HH_ASSERT_EQ_INT(d.reason, HH_MAC_ENTRY_RETRIES_EXHAUSTED);
    HH_ASSERT_EQ_INT(hh_entry_state(&d.e), HH_ENTRY_HOLDOFF);
    HH_ASSERT_EQ_INT((int)d.e.st.requests_tx, 15);
    HH_ASSERT_EQ_INT((int)d.e.st.timeouts, 15);

    /* After the hold-off a new episode starts at the smallest window with attempt 1. */
    at = jd_run(&d, (uint64_t)(p.t_holdoff + p.n_listen + 2) * SUPER);
    HH_ASSERT(at != UINT64_MAX);
    HH_ASSERT_EQ_INT(d.e.be, 4);
    HH_ASSERT_EQ_INT(d.req.u.request.attempt, 1);
}

/* Only a response to the latest request is accepted; the first valid grant wins (rule 26). */
static void test_response_matching(void)
{
    jd_t d;
    hh_entry_cmd_t g, first;
    hh_entry_act_t act;
    uint64_t at;

    jd_start(&d, 5, 7);
    at = jd_run(&d, 3 * SUPER);
    first = d.req;
    /* No answer: the request times out and a second one goes out with a new id. */
    at = jd_run(&d, 4 * SUPER);
    HH_ASSERT(at != UINT64_MAX);
    HH_ASSERT(d.req.u.request.request_id != first.u.request.request_id);
    HH_ASSERT_EQ_INT(d.req.u.request.attempt, 2);

    memset(&g, 0, sizeof g);
    g.id = HH_ENTRY_CMD_GRANT;
    g.u.grant = (hh_entry_grant_t){ .request_id = first.u.request.request_id, .apply_at = 9, .bitmap_bytes = 3,
                                    .slots = 1ull << 4, .arrival_offset = HH_ENTRY_OFFSET_NONE };
    deliver(&d.e, at + 1, 1, 5, &g, &act);                         /* late grant for the old attempt */
    HH_ASSERT_EQ_INT(hh_entry_state(&d.e), HH_ENTRY_WAIT_GRANT);
    g.u.grant.request_id = d.req.u.request.request_id;
    deliver(&d.e, at + 1, 1, 6, &g, &act);                         /* right id, other radio */
    HH_ASSERT_EQ_INT(hh_entry_state(&d.e), HH_ENTRY_WAIT_GRANT);
    HH_ASSERT_EQ_INT((int)d.e.st.ignored_responses, 1);

    g.u.grant.slots = 1ull << ENTRY_SLOT;                          /* the entry slot cannot be granted */
    deliver(&d.e, at + 1, 1, 5, &g, &act);
    HH_ASSERT_EQ_INT(hh_entry_state(&d.e), HH_ENTRY_WAIT_GRANT);
    HH_ASSERT_EQ_INT((int)d.e.st.bad_grants, 1);

    g.u.grant.slots = 1ull << 4;
    deliver(&d.e, at + 1, 1, 5, &g, &act);
    HH_ASSERT_EQ_INT(hh_entry_state(&d.e), HH_ENTRY_GRANTED);
    g.u.grant.slots = 1ull << 6;
    deliver(&d.e, at + 2, 2, 5, &g, &act);                         /* a second grant: ignored */
    HH_ASSERT(d.e.granted == (1ull << 4));
}

/* A grant is applied at its superframe; the first own-slot burst is the confirmation. */
static void test_grant_applied_at_superframe(void)
{
    jd_t d;
    hh_entry_cmd_t g;
    hh_entry_act_t act;
    uint64_t at;
    hh_slot_pos_t p;

    jd_start(&d, 5, 9);
    at = jd_run(&d, 3 * SUPER);
    memset(&g, 0, sizeof g);
    g.id = HH_ENTRY_CMD_GRANT;
    g.u.grant = (hh_entry_grant_t){ .request_id = d.req.u.request.request_id, .apply_at = 4, .bitmap_bytes = 3,
                                    .slots = 1ull << 4, .arrival_offset = HH_ENTRY_OFFSET_NONE };
    deliver(&d.e, at + 1, 1, 5, &g, &act);
    HH_ASSERT_EQ_INT(hh_entry_state(&d.e), HH_ENTRY_GRANTED);

    p = pos_of(4u * SUPER - 1);
    hh_entry_tick(&d.e, &p, &act);
    HH_ASSERT(!act.own_changed);
    p = pos_of(4u * SUPER);
    hh_entry_tick(&d.e, &p, &act);
    HH_ASSERT(act.own_changed);
    HH_ASSERT(act.own_slots == (1ull << 4));
    HH_ASSERT_EQ_INT(hh_entry_state(&d.e), HH_ENTRY_MEMBER);
    HH_ASSERT(!d.e.st.joined);
    p = pos_of(4u * SUPER + 4);
    hh_entry_on_own_tx(&d.e, &p);
    HH_ASSERT(d.e.st.joined);
    HH_ASSERT(d.e.st.t_joined == 4u * SUPER + 4);
}

/* A grant applied after apply_at + T_confirm must not be used: the radio asks again. */
static void test_late_grant_lapses(void)
{
    jd_t d;
    hh_entry_cmd_t g;
    hh_entry_act_t act;
    uint64_t at;
    hh_slot_pos_t p;

    jd_start(&d, 5, 9);
    at = jd_run(&d, 3 * SUPER);
    memset(&g, 0, sizeof g);
    g.id = HH_ENTRY_CMD_GRANT;
    g.u.grant = (hh_entry_grant_t){ .request_id = d.req.u.request.request_id, .apply_at = 4, .bitmap_bytes = 3,
                                    .slots = 1ull << 4, .arrival_offset = HH_ENTRY_OFFSET_NONE };
    deliver(&d.e, at + 1, 1, 5, &g, &act);
    p = pos_of(7u * SUPER);   /* after apply_at + t_confirm */
    hh_entry_tick(&d.e, &p, &act);
    HH_ASSERT(!act.own_changed);
    HH_ASSERT_EQ_INT(hh_entry_state(&d.e), HH_ENTRY_BACKOFF);
}

/* Rejections: full starts a new cycle, the others stop the entry. */
static void test_rejections(void)
{
    hh_entry_cmd_t r;
    hh_entry_act_t act;
    uint64_t at;
    uint8_t reasons[3] = { HH_ENTRY_REJECT_FULL, HH_ENTRY_REJECT_NOT_AUTHORISED, 9 };

    for (unsigned i = 0; i < 3; i++) {
        jd_t d;
        jd_start(&d, 5, 11);
        at = jd_run(&d, 3 * SUPER);
        memset(&r, 0, sizeof r);
        r.id = HH_ENTRY_CMD_REJECT;
        r.u.reject = (hh_entry_reject_t){ .request_id = d.req.u.request.request_id, .reason = reasons[i] };
        deliver(&d.e, at + 1, 1, 5, &r, &act);
        if (i == 0) {
            HH_ASSERT_EQ_INT(hh_entry_state(&d.e), HH_ENTRY_LISTEN);
            HH_ASSERT_EQ_INT((int)d.e.st.cycles, 1);
        } else {
            HH_ASSERT_EQ_INT(hh_entry_state(&d.e), HH_ENTRY_REFUSED);
            HH_ASSERT_EQ_INT(act.failed, i == 1);
        }
        HH_ASSERT_EQ_INT(jd_run(&d, 3 * SUPER) == UINT64_MAX, i != 0);   /* refused: no more requests */
    }
}

/* ---- member side ---- */

/* A request as the joining radio would send it. */
static hh_entry_cmd_t request(uint32_t id, uint64_t free_slots, uint8_t wanted)
{
    hh_entry_cmd_t c;
    memset(&c, 0, sizeof c);
    c.id = HH_ENTRY_CMD_REQUEST;
    c.u.request = (hh_entry_request_t){ .request_id = id, .attempt = 1, .slots_wanted = wanted, .bitmap_bytes = 3,
                                        .free_slots = free_slots };
    return c;
}

/* Takes the next command a member sends in its own slot. */
static bool take(hh_entry_t *m, uint64_t abs, hh_entry_cmd_t *out, uint16_t *dst)
{
    uint8_t b[HH_ENTRY_CMD_MAX];
    size_t n = 0;
    hh_slot_pos_t p = pos_of(abs);
    if (!hh_entry_next_command(m, &p, 100, b, &n, dst)) return false;
    return hh_entry_cmd_decode(b, n, out) == HH_OK;
}

#define ALL_FREE 0x7FFFFull   /* slots 0 to 18 */

/* A member grants the lowest free slot that the joiner listed, to the joiner's address. */
static void test_member_grants_lowest_free(void)
{
    hh_entry_t m;
    hh_entry_cmd_t c, g;
    hh_entry_act_t act;
    uint16_t dst = 0;
    uint64_t t = 7u * SUPER + ENTRY_SLOT;
    hh_slot_pos_t p;

    make(&m, 1, 1u << 0, 1);
    p = pos_of(t - 5);
    hh_entry_on_pdu(&m, &p, 2, 1u << 1);               /* radio 2 owns slot 1 */
    c = request(0xAABB0001u, ALL_FREE & ~(1ull << 2), 1);   /* the joiner heard slot 2 in use */
    deliver(&m, t, 7, HH_PDU_ADDR_BROADCAST, &c, &act);

    HH_ASSERT(take(&m, t + 1, &g, &dst));
    HH_ASSERT_EQ_INT(g.id, HH_ENTRY_CMD_GRANT);
    HH_ASSERT_EQ_INT(dst, 7);
    HH_ASSERT(g.u.grant.request_id == 0xAABB0001u);
    HH_ASSERT(g.u.grant.slots == (1ull << 3));         /* not 0 (own), 1 (radio 2), 2 (joiner heard it) */
    HH_ASSERT(g.u.grant.apply_at >= 7u + 1u);
    HH_ASSERT(!take(&m, t + 2, &g, &dst));             /* answered once */
}

/* A repeated request gets the same answer and no slot is granted twice (rule 14). */
static void test_member_duplicate_and_no_double_grant(void)
{
    hh_entry_t m;
    hh_entry_cmd_t c, g1, g2, g3;
    hh_entry_act_t act;
    uint16_t dst;
    uint64_t t = 7u * SUPER + ENTRY_SLOT;

    make(&m, 1, 1u << 0, 1);
    c = request(0x11110001u, ALL_FREE, 1);
    deliver(&m, t, 7, HH_PDU_ADDR_BROADCAST, &c, &act);
    HH_ASSERT(take(&m, t + 1, &g1, &dst));

    deliver(&m, t + SPF, 7, HH_PDU_ADDR_BROADCAST, &c, &act);   /* same source, same request id */
    HH_ASSERT(take(&m, t + SPF + 1, &g2, &dst));
    HH_ASSERT(g2.u.grant.slots == g1.u.grant.slots);
    HH_ASSERT(g2.u.grant.apply_at == g1.u.grant.apply_at);       /* the same answer again */
    HH_ASSERT_EQ_INT((int)m.st.duplicates, 1);

    c = request(0x22220001u, ALL_FREE, 1);                       /* another radio, same free list */
    deliver(&m, t + 2 * SPF, 8, HH_PDU_ADDR_BROADCAST, &c, &act);
    HH_ASSERT(take(&m, t + 2 * SPF + 1, &g3, &dst));
    HH_ASSERT_EQ_INT(dst, 8);
    HH_ASSERT(g3.u.grant.slots != g1.u.grant.slots);
    HH_ASSERT((g3.u.grant.slots & g1.u.grant.slots) == 0);
}

/* A new request id from the same radio releases its earlier unconfirmed grant. */
static void test_member_new_request_releases_old_grant(void)
{
    hh_entry_t m;
    hh_entry_cmd_t c, g1, g2;
    hh_entry_act_t act;
    uint16_t dst;
    uint64_t t = 7u * SUPER + ENTRY_SLOT;

    make(&m, 1, 1u << 0, 1);
    c = request(0x11110001u, ALL_FREE, 1);
    deliver(&m, t, 7, HH_PDU_ADDR_BROADCAST, &c, &act);
    HH_ASSERT(take(&m, t + 1, &g1, &dst));
    c = request(0x11110002u, ALL_FREE, 1);                       /* the grant was lost: asks again */
    deliver(&m, t + 3 * SPF, 7, HH_PDU_ADDR_BROADCAST, &c, &act);
    HH_ASSERT(take(&m, t + 3 * SPF + 1, &g2, &dst));
    HH_ASSERT(g2.u.grant.request_id == 0x11110002u);
    HH_ASSERT(g2.u.grant.slots == g1.u.grant.slots);             /* the same slot is free again */
}

/* Full net, a request outside an entry slot, and a bad bitmap length. */
static void test_member_rejects_and_refusals(void)
{
    hh_entry_t m;
    hh_entry_cmd_t c, r;
    hh_entry_act_t act;
    uint16_t dst;
    uint64_t t = 7u * SUPER + ENTRY_SLOT;

    make(&m, 1, 1u << 0, 1);
    c = request(1, 0, 1);                                        /* the joiner heard no free slot */
    deliver(&m, t, 7, HH_PDU_ADDR_BROADCAST, &c, &act);
    HH_ASSERT(take(&m, t + 1, &r, &dst));
    HH_ASSERT_EQ_INT(r.id, HH_ENTRY_CMD_REJECT);
    HH_ASSERT_EQ_INT(r.u.reject.reason, HH_ENTRY_REJECT_FULL);

    c = request(2, ALL_FREE, 1);
    deliver(&m, t - 4, 8, HH_PDU_ADDR_BROADCAST, &c, &act);      /* not an entry slot */
    HH_ASSERT(!take(&m, t + 2, &r, &dst));
    HH_ASSERT_EQ_INT((int)m.st.bad_commands, 1);

    c = request(3, ALL_FREE, 0);                                 /* asks for no slot */
    deliver(&m, t + SPF, 9, HH_PDU_ADDR_BROADCAST, &c, &act);
    HH_ASSERT(take(&m, t + SPF + 1, &r, &dst));
    HH_ASSERT_EQ_INT(r.u.reject.reason, HH_ENTRY_REJECT_BAD_REQUEST);
}

/* Two members hear a request: the second stays silent once it hears the first answer, and keeps the slot reserved. */
static void test_member_arbitration(void)
{
    hh_entry_t a, b;
    hh_entry_cmd_t c, g, g2;
    hh_entry_act_t act;
    uint16_t dst;
    uint64_t t = 7u * SUPER + ENTRY_SLOT;

    make(&a, 1, 1u << 0, 1);
    make(&b, 2, 1u << 1, 1);
    {
        hh_slot_pos_t p = pos_of(t - 5);
        hh_entry_on_pdu(&a, &p, 2, 1u << 1);                     /* each knows the other's slot */
        hh_entry_on_pdu(&b, &p, 1, 1u << 0);
    }
    c = request(0x33330001u, ALL_FREE, 1);
    deliver(&a, t, 7, HH_PDU_ADDR_BROADCAST, &c, &act);
    deliver(&b, t, 7, HH_PDU_ADDR_BROADCAST, &c, &act);
    HH_ASSERT(take(&a, t + 1, &g, &dst));                        /* a's own slot comes first */
    deliver(&b, t + 1, 1, dst, &g, &act);                        /* b hears the grant */
    HH_ASSERT(!take(&b, t + 2, &g2, &dst));

    c = request(0x44440001u, ALL_FREE, 1);                       /* a second joiner: b must not reuse the slot */
    deliver(&b, t + SPF, 8, HH_PDU_ADDR_BROADCAST, &c, &act);
    HH_ASSERT(take(&b, t + SPF + 2, &g2, &dst));
    HH_ASSERT((g2.u.grant.slots & g.u.grant.slots) == 0);
}

/* A grant that is never used lapses and the slot can be granted again; a used one is kept. */
static void test_member_grant_lapses_or_is_confirmed(void)
{
    hh_entry_t m;
    hh_entry_cmd_t c, g, g2;
    hh_entry_act_t act;
    uint16_t dst;
    hh_slot_pos_t p;
    uint64_t t = 7u * SUPER + ENTRY_SLOT;

    make(&m, 1, 1u << 0, 1);
    c = request(0x55550001u, ALL_FREE, 1);
    deliver(&m, t, 7, HH_PDU_ADDR_BROADCAST, &c, &act);
    HH_ASSERT(take(&m, t + 1, &g, &dst));

    c = request(0x66660001u, ALL_FREE, 1);
    deliver(&m, t + SPF, 8, HH_PDU_ADDR_BROADCAST, &c, &act);
    HH_ASSERT(take(&m, t + SPF + 1, &g2, &dst));
    HH_ASSERT((g2.u.grant.slots & g.u.grant.slots) == 0);

    /* Radio 7 is heard in its slot: confirmed. Radio 8 stays silent: its grant lapses. */
    p = pos_of(g.u.grant.apply_at * SUPER + 3);
    hh_entry_on_pdu(&m, &p, 7, g.u.grant.slots);
    p = pos_of((g2.u.grant.apply_at + 3u) * SUPER);
    hh_entry_tick(&m, &p, &act);
    HH_ASSERT_EQ_INT((int)m.st.lapsed_grants, 1);

    c = request(0x77770001u, ALL_FREE, 1);
    deliver(&m, p.abs_slot + ENTRY_SLOT - p.slot, 9, HH_PDU_ADDR_BROADCAST, &c, &act);
    HH_ASSERT(take(&m, p.abs_slot + ENTRY_SLOT - p.slot + 1, &g, &dst));
    HH_ASSERT(g.u.grant.slots == g2.u.grant.slots);               /* the lapsed slot, not radio 7's */
}

/* The leave goes out in n_leave consecutive superframes, then the slot is released everywhere (rule 15). */
static void test_leave(void)
{
    hh_entry_t a, b;
    hh_entry_cmd_t c, g;
    hh_entry_act_t act;
    hh_slot_pos_t p;
    uint16_t dst;
    unsigned sent = 0, first_sf = 0, last_sf = 0;
    hh_entry_params_t pr;
    uint32_t release;

    hh_entry_params_default(&pr);
    release = 5u + pr.n_leave + 1u;
    make(&a, 1, 1u << 3, 1);
    make(&b, 2, 1u << 0, 1);
    p = pos_of(5u * SUPER + 3);
    hh_entry_on_pdu(&b, &p, 1, 1u << 3);                           /* b knows radio 1 owns slot 3 */
    p = pos_of(5u * SUPER + 100);
    HH_ASSERT_OK(hh_entry_leave(&a, &p));

    for (uint32_t sf = 5; sf < release; sf++) {
        for (unsigned frame = 0; frame < FPS; frame++) {
            uint64_t abs = (uint64_t)sf * SUPER + frame * SPF + 3;
            if (!take(&a, abs, &c, &dst)) continue;
            HH_ASSERT_EQ_INT(c.id, HH_ENTRY_CMD_LEAVE);
            HH_ASSERT_EQ_INT(dst, HH_PDU_ADDR_BROADCAST);
            HH_ASSERT(c.u.leave.slots == (1ull << 3));
            HH_ASSERT(c.u.leave.release_at == release);
            if (!sent++) first_sf = sf;
            last_sf = sf;
            deliver(&b, abs, 1, dst, &c, &act);
        }
    }
    HH_ASSERT_EQ_INT((int)sent, (int)pr.n_leave);                  /* once per superframe */
    HH_ASSERT_EQ_INT((int)first_sf, 6);                            /* from the superframe after the start */
    HH_ASSERT_EQ_INT((int)last_sf, (int)(release - 1));
    HH_ASSERT_EQ_INT(hh_entry_state(&a), HH_ENTRY_MEMBER);         /* still sending until the release */

    /* Before the release the slot is still taken in b's view. */
    c = request(0x88880001u, 1ull << 3, 1);
    deliver(&b, (release - 1) * SUPER + ENTRY_SLOT, 9, HH_PDU_ADDR_BROADCAST, &c, &act);
    HH_ASSERT(take(&b, (release - 1) * SUPER + ENTRY_SLOT + 1, &g, &dst));
    HH_ASSERT_EQ_INT(g.id, HH_ENTRY_CMD_REJECT);

    p = pos_of((uint64_t)release * SUPER);
    hh_entry_tick(&a, &p, &act);
    HH_ASSERT(act.own_changed && act.own_slots == 0);
    HH_ASSERT_EQ_INT(hh_entry_state(&a), HH_ENTRY_OFF);
    hh_entry_tick(&b, &p, &act);

    c = request(0x88880002u, 1ull << 3, 1);
    deliver(&b, (release + 1) * SUPER + ENTRY_SLOT, 10, HH_PDU_ADDR_BROADCAST, &c, &act);
    HH_ASSERT(take(&b, (release + 1) * SUPER + ENTRY_SLOT + 1, &g, &dst));
    HH_ASSERT_EQ_INT(g.id, HH_ENTRY_CMD_GRANT);
    HH_ASSERT(g.u.grant.slots == (1ull << 3));                     /* the released slot */
}

/* Setup is validated. */
static void test_init_validation(void)
{
    hh_entry_t e;
    hh_slot_config_t c;
    hh_entry_params_t p;

    hh_slot_config_default(&c, 1, NET);
    hh_entry_params_default(&p);
    HH_ASSERT_ERR(hh_entry_init(&e, &p, &c, 1), HH_ERR_INVAL);       /* no entry slot */
    c.entry_slots = 1ull << 25;
    HH_ASSERT_ERR(hh_entry_init(&e, &p, &c, 1), HH_ERR_INVAL);       /* outside the frame */
    c.entry_slots = 1ull << 4;
    c.own_slots = 1ull << 4;
    HH_ASSERT_ERR(hh_entry_init(&e, &p, &c, 1), HH_ERR_INVAL);       /* shared with an own slot */
    c.own_slots = 0;
    p.be_min = 7;
    HH_ASSERT_ERR(hh_entry_init(&e, &p, &c, 1), HH_ERR_INVAL);       /* minimum above maximum */
    hh_entry_params_default(&p);
    HH_ASSERT_OK(hh_entry_init(&e, &p, &c, 1));
    HH_ASSERT_EQ_INT(hh_entry_state(&e), HH_ENTRY_OFF);
    c.own_slots = 1;
    HH_ASSERT_OK(hh_entry_init(&e, &p, &c, 1));
    HH_ASSERT_EQ_INT(hh_entry_state(&e), HH_ENTRY_MEMBER);
    {
        hh_slot_pos_t pos = pos_of(0);
        HH_ASSERT_ERR(hh_entry_join(&e, &pos), HH_ERR_STATE);        /* a member cannot join */
    }
}

HH_TEST_MAIN_BEGIN("entry")
    hh_log_set_level(HH_LOG_ERROR);
    HH_RUN(test_vector_d_request);
    HH_RUN(test_vector_e_grant);
    HH_RUN(test_reject_and_leave_bodies);
    HH_RUN(test_decode_refusals);
    HH_RUN(test_first_request_after_listen_and_backoff);
    HH_RUN(test_backoff_differs_per_radio);
    HH_RUN(test_timeout_window_cycles_and_holdoff);
    HH_RUN(test_response_matching);
    HH_RUN(test_grant_applied_at_superframe);
    HH_RUN(test_late_grant_lapses);
    HH_RUN(test_rejections);
    HH_RUN(test_member_grants_lowest_free);
    HH_RUN(test_member_duplicate_and_no_double_grant);
    HH_RUN(test_member_new_request_releases_old_grant);
    HH_RUN(test_member_rejects_and_refusals);
    HH_RUN(test_member_arbitration);
    HH_RUN(test_member_grant_lapses_or_is_confirmed);
    HH_RUN(test_leave);
    HH_RUN(test_init_validation);
HH_TEST_MAIN_END()
