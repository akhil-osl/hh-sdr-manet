/*
 * PDU v2 codec against the worked examples (vectors A to E) of MAC frame
 * specification v2, part 6. Links hhsdr_mac only.
 */
#include "hhsdr/mac/pdu.h"
#include "hh_test.h"
#include <string.h>

#define NET 0x0010u

static const uint8_t vec_a[] = {
    0x41, 0xA8, 0x21, 0x10, 0x00, 0xFF, 0xFF, 0x01, 0x00, 0x10, 0x00, 0x00, 0x00, 0x00, 0x03,
    0x20, 0x00, 0x00, 0xF0, 0x02, 0x00, 0x05, 0xA0, 0x07, 0x48, 0x45, 0x4C, 0x4C, 0x4F,
    0x5F, 0x1A, 0xD6, 0x46 };
static const uint8_t vec_b[] = {
    0x41, 0xA8, 0x40, 0x10, 0x00, 0xFF, 0xFF, 0x02, 0x00, 0x18, 0x00, 0x00, 0x00, 0x01, 0x03,
    0x00, 0x02, 0x00, 0x01, 0x00, 0xE8, 0x00, 0x00, 0x00, 0x80, 0x0A, 0x29, 0xE0, 0xAC };
static const uint8_t vec_c[] = {
    0x41, 0xA8, 0x22, 0x10, 0x00, 0xFF, 0xFF, 0x01, 0x00, 0x10, 0x10, 0x00, 0x00, 0x00, 0x03,
    0x20, 0x00, 0x00, 0x80, 0xA4, 0x7F, 0x01, 0x01, 0x00, 0xBC, 0xA4, 0x7F, 0x01, 0x00, 0x04,
    0x02, 0x20, 0x02, 0x00, 0x9C, 0xEC, 0xEA, 0xE6 };
static const uint8_t vec_d[] = {
    0x41, 0xA8, 0x01, 0x10, 0x00, 0xFF, 0xFF, 0x03, 0x00, 0x14, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x20, 0xFF, 0xFF, 0x0B, 0x00, 0x01, 0xE7, 0x91, 0x3C, 0x5A, 0x01, 0x01, 0x03, 0xDE, 0xFD,
    0x0F, 0xE2, 0x55, 0x5B, 0x6A };
static const uint8_t vec_e[] = {
    0x41, 0xA8, 0x23, 0x10, 0x00, 0xFF, 0xFF, 0x01, 0x00, 0x10, 0x00, 0x00, 0x00, 0x00, 0x03,
    0x20, 0x00, 0x00, 0x20, 0x03, 0x00, 0x0F, 0x00, 0x02, 0xE7, 0x91, 0x3C, 0x5A, 0xBE, 0xA4,
    0x7F, 0x01, 0x03, 0x00, 0x40, 0x00, 0x4E, 0x01, 0x92, 0xEB, 0x30, 0xE7 };

static const uint8_t hello[] = { 'H', 'E', 'L', 'L', 'O' };
static const uint8_t cmd_d[] = { 0x01, 0xE7, 0x91, 0x3C, 0x5A, 0x01, 0x01, 0x03, 0xDE, 0xFD, 0x0F };
static const uint8_t cmd_e[] = { 0x02, 0xE7, 0x91, 0x3C, 0x5A, 0xBE, 0xA4, 0x7F, 0x01, 0x03, 0x00, 0x40,
                                 0x00, 0x4E, 0x01 };

static void base(hh_pdu_t *p, uint8_t seq, uint16_t src, uint8_t tq)
{
    memset(p, 0, sizeof *p);
    p->seq = seq;
    p->net_id = NET;
    p->src = src;
    p->time_quality = tq;
    p->slot_bytes = 3;
}

static void build_a(hh_pdu_t *p)
{
    base(p, 0x21, 1, 0x10);
    p->claims[0] = 0x20;
    p->n_msg = 1;
    p->msg[0] = (hh_pdu_msg_t){ .proto = HH_PDU_PROTO_TEST, .dst = 2, .cls = 4, .ack_req = true,
                                .link_seq = 7, .data = hello, .len = 5 };
}

static void build_b(hh_pdu_t *p)
{
    base(p, 0x40, 2, 0x18);
    p->claims[1] = 0x02;
    p->n_back = 1;
    p->back[0] = (hh_pdu_back_t){ .neighbor = 1, .start = 232, .bitmap = 0x80000000u };
}

static void build_c(hh_pdu_t *p)
{
    base(p, 0x22, 1, 0x10);
    p->claims[0] = 0x20;
    p->has_beacon = true;
    p->beacon.founding_superframe = 25142400u;
    p->beacon.founder = 1;
    p->beacon.superframe = 25142460u;
    p->beacon.entry_slot = 0;
    p->beacon.window = 4;
    p->beacon.members = 2;
    p->beacon.heard[0] = 0x20;
    p->beacon.heard[1] = 0x02;
}

static void build_d(hh_pdu_t *p)
{
    base(p, 0x01, 3, 0x14);
    p->slot_bytes = 0;
    p->n_msg = 1;
    p->msg[0] = (hh_pdu_msg_t){ .proto = HH_PDU_PROTO_COMMAND, .dst = HH_PDU_ADDR_BROADCAST, .cls = 0,
                                .data = cmd_d, .len = sizeof cmd_d };
}

static void build_e(hh_pdu_t *p)
{
    base(p, 0x23, 1, 0x10);
    p->claims[0] = 0x20;
    p->n_msg = 1;
    p->msg[0] = (hh_pdu_msg_t){ .proto = HH_PDU_PROTO_COMMAND, .dst = 3, .cls = 0,
                                .data = cmd_e, .len = sizeof cmd_e };
}

static void check_encode(void (*build)(hh_pdu_t *), const uint8_t *want, size_t want_len)
{
    hh_pdu_t p;
    uint8_t buf[256];
    size_t n = 0;
    build(&p);
    HH_ASSERT_OK(hh_pdu_encode(&p, buf, sizeof buf, &n));
    HH_ASSERT_EQ_INT(n, want_len);
    HH_ASSERT(memcmp(buf, want, want_len) == 0);
    HH_ASSERT_EQ_INT(hh_pdu_size(&p), want_len);
}

static void test_crc_check_value(void)
{
    HH_ASSERT_EQ_INT(hh_crc32((const uint8_t *)"123456789", 9), 0xCBF43926u);
}

static void test_encode_vectors(void)
{
    check_encode(build_a, vec_a, sizeof vec_a);
    check_encode(build_b, vec_b, sizeof vec_b);
    check_encode(build_c, vec_c, sizeof vec_c);
    check_encode(build_d, vec_d, sizeof vec_d);
    check_encode(build_e, vec_e, sizeof vec_e);
}

static void test_decode_a(void)
{
    hh_pdu_t p;
    HH_ASSERT_OK(hh_pdu_decode(vec_a, sizeof vec_a, &p));
    HH_ASSERT_EQ_INT(p.seq, 0x21);
    HH_ASSERT_EQ_INT(p.net_id, NET);
    HH_ASSERT_EQ_INT(p.src, 1);
    HH_ASSERT_EQ_INT(p.slot_bytes, 3);
    HH_ASSERT_EQ_INT(p.claims[0], 0x20);
    HH_ASSERT_EQ_INT(p.n_msg, 1);
    HH_ASSERT_EQ_INT(p.msg[0].proto, HH_PDU_PROTO_TEST);
    HH_ASSERT_EQ_INT(p.msg[0].dst, 2);
    HH_ASSERT_EQ_INT(p.msg[0].cls, 4);
    HH_ASSERT(p.msg[0].ack_req);
    HH_ASSERT_EQ_INT(p.msg[0].link_seq, 7);
    HH_ASSERT_EQ_INT(p.msg[0].len, 5);
    HH_ASSERT(memcmp(p.msg[0].data, "HELLO", 5) == 0);
}

static void test_decode_b_c(void)
{
    hh_pdu_t p;
    HH_ASSERT_OK(hh_pdu_decode(vec_b, sizeof vec_b, &p));
    HH_ASSERT_EQ_INT(p.n_back, 1);
    HH_ASSERT_EQ_INT(p.back[0].neighbor, 1);
    HH_ASSERT_EQ_INT(p.back[0].start, 232);
    HH_ASSERT_EQ_INT(p.back[0].bitmap, 0x80000000u);
    HH_ASSERT_EQ_INT(p.claims[1], 0x02);
    HH_ASSERT_EQ_INT(p.n_msg, 0);

    HH_ASSERT_OK(hh_pdu_decode(vec_c, sizeof vec_c, &p));
    HH_ASSERT(p.has_beacon);
    HH_ASSERT_EQ_INT(p.beacon.founding_superframe, 25142400);
    HH_ASSERT_EQ_INT(p.beacon.superframe, 25142460);
    HH_ASSERT_EQ_INT(p.beacon.window, 4);
    HH_ASSERT_EQ_INT(p.beacon.members, 2);
    HH_ASSERT_EQ_INT(p.beacon.heard[1], 0x02);
}

static void test_decode_d_e(void)
{
    hh_pdu_t p;
    HH_ASSERT_OK(hh_pdu_decode(vec_d, sizeof vec_d, &p));
    HH_ASSERT_EQ_INT(p.slot_bytes, 0);
    HH_ASSERT_EQ_INT(p.msg[0].proto, HH_PDU_PROTO_COMMAND);
    HH_ASSERT_EQ_INT(p.msg[0].dst, HH_PDU_ADDR_BROADCAST);
    HH_ASSERT_EQ_INT(p.msg[0].len, 11);
    HH_ASSERT(memcmp(p.msg[0].data, cmd_d, sizeof cmd_d) == 0);

    HH_ASSERT_OK(hh_pdu_decode(vec_e, sizeof vec_e, &p));
    HH_ASSERT_EQ_INT(p.msg[0].dst, 3);
    HH_ASSERT_EQ_INT(p.msg[0].len, 15);
    HH_ASSERT(memcmp(p.msg[0].data, cmd_e, sizeof cmd_e) == 0);
}

static void test_round_trip_fragment_and_lists(void)
{
    hh_pdu_t p, q;
    uint8_t buf[128], data[16];
    size_t n;
    for (unsigned i = 0; i < sizeof data; i++) data[i] = (uint8_t)i;

    base(&p, 9, 5, 0x20);
    p.more_data = true;
    p.breakin = true;
    p.breakin_hops = 3;
    p.breakin_invoker = 2;
    p.n_link = 2;
    p.link[0] = (hh_pdu_link_t){ 1, 10, 80 };
    p.link[1] = (hh_pdu_link_t){ 2, 255, 255 };
    p.has_qreport = true;
    p.qreport[4] = 12;
    p.n_msg = 1;
    p.msg[0] = (hh_pdu_msg_t){ .proto = HH_PDU_PROTO_IPV6, .dst = 2, .cls = 4, .ack_req = true,
                               .link_seq = 8, .frag = true, .dgram_size = 1280, .dgram_tag = 0x42,
                               .frag_offset = 34, .data = data, .len = sizeof data };
    HH_ASSERT_OK(hh_pdu_encode(&p, buf, sizeof buf, &n));
    HH_ASSERT_OK(hh_pdu_decode(buf, n, &q));
    HH_ASSERT(q.more_data && q.breakin);
    HH_ASSERT_EQ_INT(q.breakin_hops, 3);
    HH_ASSERT_EQ_INT(q.breakin_invoker, 2);
    HH_ASSERT_EQ_INT(q.n_link, 2);
    HH_ASSERT_EQ_INT(q.link[0].snr, 80);
    HH_ASSERT_EQ_INT(q.qreport[4], 12);
    HH_ASSERT(q.msg[0].frag && q.msg[0].ack_req);
    HH_ASSERT_EQ_INT(q.msg[0].dgram_size, 1280);
    HH_ASSERT_EQ_INT(q.msg[0].dgram_tag, 0x42);
    HH_ASSERT_EQ_INT(q.msg[0].frag_offset, 34);
    HH_ASSERT(memcmp(q.msg[0].data, data, sizeof data) == 0);
}

static void test_overhead_matches_spec(void)
{
    hh_pdu_t p;
    build_a(&p);
    p.n_msg = 0;
    /* Header 9, control block 6 + 3, CRC 4 (spec 4.3: 9 bytes with 20 slots). */
    HH_ASSERT_EQ_INT(hh_pdu_overhead(&p), 9 + 9 + 4);
    HH_ASSERT_EQ_INT(hh_pdu_size(&p), 22);
}

static void test_rejects(void)
{
    hh_pdu_t p;
    uint8_t buf[sizeof vec_a], small[8];
    size_t n;

    memcpy(buf, vec_a, sizeof buf);
    buf[20] ^= 1;   /* corrupt a byte: CRC fails */
    HH_ASSERT_ERR(hh_pdu_decode(buf, sizeof buf, &p), HH_ERR_INVAL);
    HH_ASSERT_ERR(hh_pdu_decode(vec_a, sizeof vec_a - 1, &p), HH_ERR_INVAL);

    /* Security bit set, with a valid CRC: refused as unsupported. */
    memcpy(buf, vec_a, sizeof buf);
    buf[0] = 0x49;
    n = sizeof buf - 4;
    {
        uint32_t c = hh_crc32(buf, n);
        buf[n] = (uint8_t)c; buf[n + 1] = (uint8_t)(c >> 8);
        buf[n + 2] = (uint8_t)(c >> 16); buf[n + 3] = (uint8_t)(c >> 24);
    }
    HH_ASSERT_ERR(hh_pdu_decode(buf, sizeof buf, &p), HH_ERR_UNSUPPORTED);

    /* Message length runs past the end of the PDU. */
    memcpy(buf, vec_a, sizeof buf);
    buf[21] = 0xFF;   /* word low byte: length 255, more than the bytes left */
    {
        uint32_t c = hh_crc32(buf, sizeof buf - 4);
        n = sizeof buf - 4;
        buf[n] = (uint8_t)c; buf[n + 1] = (uint8_t)(c >> 8);
        buf[n + 2] = (uint8_t)(c >> 16); buf[n + 3] = (uint8_t)(c >> 24);
    }
    HH_ASSERT_ERR(hh_pdu_decode(buf, sizeof buf, &p), HH_ERR_INVAL);

    build_a(&p);
    HH_ASSERT_ERR(hh_pdu_encode(&p, small, sizeof small, &n), HH_ERR_NOMEM);
    p.msg[0].cls = 8;
    HH_ASSERT_ERR(hh_pdu_encode(&p, buf, sizeof buf, &n), HH_ERR_INVAL);
}

HH_TEST_MAIN_BEGIN("pdu")
    HH_RUN(test_crc_check_value);
    HH_RUN(test_encode_vectors);
    HH_RUN(test_decode_a);
    HH_RUN(test_decode_b_c);
    HH_RUN(test_decode_d_e);
    HH_RUN(test_round_trip_fragment_and_lists);
    HH_RUN(test_overhead_matches_spec);
    HH_RUN(test_rejects);
HH_TEST_MAIN_END()
