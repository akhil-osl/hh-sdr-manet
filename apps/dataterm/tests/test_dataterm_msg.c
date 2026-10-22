/* Message codec: sample bytes, CRC, and every decode rejection. */
#include "hh_test.h"
#include "hhsdr/dataterm/msg.h"

static const uint8_t SAMPLE_DATA[] = {
    0x01, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x02,
    0x00, 0x00, 0x00, 0x07, 0x00, 0x05, 0x9b, 0x10, 'H', 'E', 'L', 'L', 'O'
};
static const uint8_t SAMPLE_ACK[] = {
    0x01, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x01,
    0x00, 0x00, 0x00, 0x07, 0x00, 0x00, 0x69, 0x25
};

static void test_crc_check_value(void)
{
    HH_ASSERT_EQ_INT(hh_dt_crc16((const uint8_t *)"123456789", 9, 0xFFFF), 0x29B1);
}

static void test_encode_matches_sample_data(void)
{
    hh_dt_hdr_t h = { .type = HH_DT_TYPE_DATA, .flags = HH_DT_FLAG_ACK_REQ, .attempt = 0,
                      .src = 1, .dst = 2, .seq = 7, .len = 5 };
    uint8_t out[64];
    size_t n = 0;
    HH_ASSERT_OK(hh_dt_encode(&h, (const uint8_t *)"HELLO", out, sizeof out, &n));
    HH_ASSERT_EQ_INT(n, sizeof SAMPLE_DATA);
    HH_ASSERT(memcmp(out, SAMPLE_DATA, n) == 0);
}

static void test_encode_matches_sample_ack(void)
{
    hh_dt_hdr_t h = { .type = HH_DT_TYPE_ACK, .src = 2, .dst = 1, .seq = 7 };
    uint8_t out[64];
    size_t n = 0;
    HH_ASSERT_OK(hh_dt_encode(&h, NULL, out, sizeof out, &n));
    HH_ASSERT_EQ_INT(n, sizeof SAMPLE_ACK);
    HH_ASSERT(memcmp(out, SAMPLE_ACK, n) == 0);
}

static void test_decode_sample(void)
{
    hh_dt_hdr_t h;
    const uint8_t *p = NULL;
    HH_ASSERT_EQ_INT(hh_dt_decode(SAMPLE_DATA, sizeof SAMPLE_DATA, &h, &p), HH_DT_RX_OK);
    HH_ASSERT_EQ_INT(h.type, HH_DT_TYPE_DATA);
    HH_ASSERT_EQ_INT(h.flags, HH_DT_FLAG_ACK_REQ);
    HH_ASSERT_EQ_INT(h.src, 1);
    HH_ASSERT_EQ_INT(h.dst, 2);
    HH_ASSERT_EQ_INT(h.seq, 7);
    HH_ASSERT_EQ_INT(h.len, 5);
    HH_ASSERT(memcmp(p, "HELLO", 5) == 0);
}

static void test_round_trip_large_values(void)
{
    uint8_t data[HH_DT_PAYLOAD_MAX];
    for (size_t i = 0; i < sizeof data; i++) data[i] = (uint8_t)(i * 7);
    hh_dt_hdr_t h = { .type = HH_DT_TYPE_DATA, .attempt = 3, .src = 0xFFFFFFFEu,
                      .dst = 0x80000001u, .seq = 0xFFFFFFFFu, .len = HH_DT_PAYLOAD_MAX };
    uint8_t out[HH_DT_MSG_MAX];
    size_t n = 0;
    HH_ASSERT_OK(hh_dt_encode(&h, data, out, sizeof out, &n));
    hh_dt_hdr_t g;
    const uint8_t *p;
    HH_ASSERT_EQ_INT(hh_dt_decode(out, n, &g, &p), HH_DT_RX_OK);
    HH_ASSERT_EQ_INT(g.src, 0xFFFFFFFEu);
    HH_ASSERT_EQ_INT(g.dst, 0x80000001u);
    HH_ASSERT_EQ_INT(g.seq, 0xFFFFFFFFu);
    HH_ASSERT_EQ_INT(g.attempt, 3);
    HH_ASSERT(memcmp(p, data, sizeof data) == 0);
}

static void test_encode_rejects(void)
{
    hh_dt_hdr_t h = { .type = HH_DT_TYPE_DATA, .src = 1, .dst = 2, .seq = 1, .len = 5 };
    uint8_t out[24];
    size_t n;
    HH_ASSERT_ERR(hh_dt_encode(&h, (const uint8_t *)"HELLO", out, sizeof out, &n), HH_ERR_NOMEM);
    h.len = HH_DT_PAYLOAD_MAX + 1;
    HH_ASSERT_ERR(hh_dt_encode(&h, (const uint8_t *)"HELLO", out, sizeof out, &n), HH_ERR_INVAL);
    h.len = 5;
    HH_ASSERT_ERR(hh_dt_encode(&h, NULL, out, sizeof out, &n), HH_ERR_INVAL);
}

static void test_decode_rejects(void)
{
    uint8_t b[sizeof SAMPLE_DATA];
    HH_ASSERT_EQ_INT(hh_dt_decode(SAMPLE_DATA, 19, NULL, NULL), HH_DT_RX_SHORT);
    HH_ASSERT_EQ_INT(hh_dt_decode(SAMPLE_DATA, sizeof SAMPLE_DATA - 1, NULL, NULL),
                     HH_DT_RX_LENGTH);

    memcpy(b, SAMPLE_DATA, sizeof b);
    b[22] ^= 0x01;
    HH_ASSERT_EQ_INT(hh_dt_decode(b, sizeof b, NULL, NULL), HH_DT_RX_CRC);

    /* Valid CRC but unknown version or type: re-encode by hand. */
    memcpy(b, SAMPLE_DATA, sizeof b);
    b[0] = 2;
    b[18] = b[19] = 0;
    uint16_t crc = hh_dt_crc16(b, sizeof b, 0xFFFF);
    b[18] = (uint8_t)(crc >> 8); b[19] = (uint8_t)crc;
    HH_ASSERT_EQ_INT(hh_dt_decode(b, sizeof b, NULL, NULL), HH_DT_RX_VERSION);

    memcpy(b, SAMPLE_DATA, sizeof b);
    b[1] = 9;
    b[18] = b[19] = 0;
    crc = hh_dt_crc16(b, sizeof b, 0xFFFF);
    b[18] = (uint8_t)(crc >> 8); b[19] = (uint8_t)crc;
    HH_ASSERT_EQ_INT(hh_dt_decode(b, sizeof b, NULL, NULL), HH_DT_RX_TYPE);
}

HH_TEST_MAIN_BEGIN("dataterm_msg")
    HH_RUN(test_crc_check_value);
    HH_RUN(test_encode_matches_sample_data);
    HH_RUN(test_encode_matches_sample_ack);
    HH_RUN(test_decode_sample);
    HH_RUN(test_round_trip_large_values);
    HH_RUN(test_encode_rejects);
    HH_RUN(test_decode_rejects);
HH_TEST_MAIN_END()
