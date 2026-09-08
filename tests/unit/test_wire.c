/* Beacon and route-update wire format: round-trip, versioning, malformed input. */
#include "hhsdr/radio/wire.h"
#include "hh_test.h"
#include <string.h>

static hh_beacon_t sample_beacon(void)
{
    hh_beacon_t b;
    memset(&b, 0, sizeof b);
    b.node_id = 0xDEADBEEFu;
    b.protocol_version = HH_PROTOCOL_VERSION;
    b.sequence_no = 12345u;
    b.timestamp = 0x0102030405060708ull;
    b.capabilities = HH_CAP_ROUTING_CAPABLE | HH_CAP_RELAY_HIGH_CAP;
    b.radio_caps = 0xAABBCCDDu;
    b.supported_waveforms = 0x5u;
    b.channel_freq = 2412000000.0f;
    b.routing_capable = true;
    return b;
}

static void test_beacon_roundtrip(void)
{
    uint8_t buf[HH_BEACON_WIRE_LEN];
    hh_beacon_t in = sample_beacon(), out;

    HH_ASSERT_EQ_INT(hh_beacon_encode(&in, buf, sizeof buf), HH_BEACON_WIRE_LEN);
    HH_ASSERT_OK(hh_beacon_decode(buf, sizeof buf, &out));

    HH_ASSERT_EQ_INT(out.node_id, in.node_id);
    HH_ASSERT_EQ_INT(out.sequence_no, in.sequence_no);
    HH_ASSERT_EQ_INT(out.timestamp, in.timestamp);
    HH_ASSERT_EQ_INT(out.capabilities, in.capabilities);
    HH_ASSERT_EQ_INT(out.radio_caps, in.radio_caps);
    HH_ASSERT_EQ_INT(out.supported_waveforms, in.supported_waveforms);
    HH_ASSERT(out.routing_capable);
    HH_ASSERT_NEAR(out.channel_freq, in.channel_freq, 1.0);
}

static void test_beacon_wire_is_endian_explicit(void)
{
    /* The layout is a contract: assert the first bytes rather than only that a
     * round-trip works, so an accidental layout change is caught. */
    uint8_t buf[HH_BEACON_WIRE_LEN];
    hh_beacon_t b = sample_beacon();
    b.node_id = 0x01020304u;
    HH_ASSERT(hh_beacon_encode(&b, buf, sizeof buf) == HH_BEACON_WIRE_LEN);
    HH_ASSERT_EQ_INT(buf[0], 0x04);   /* little-endian */
    HH_ASSERT_EQ_INT(buf[1], 0x03);
    HH_ASSERT_EQ_INT(buf[2], 0x02);
    HH_ASSERT_EQ_INT(buf[3], 0x01);
}

static void test_optional_fields_degrade_gracefully(void)
{
    uint8_t buf[HH_BEACON_WIRE_LEN];
    hh_beacon_t in = sample_beacon(), out;

    /* position and power are optional and must degrade gracefully. */
    in.position_valid = false;
    in.power_valid = false;
    HH_ASSERT(hh_beacon_encode(&in, buf, sizeof buf) > 0);
    HH_ASSERT_OK(hh_beacon_decode(buf, sizeof buf, &out));
    HH_ASSERT(!out.position_valid);
    HH_ASSERT(!out.power_valid);

    in.position_valid = true;
    in.position_x = 12.5f;
    in.power_valid = true;
    in.power_battery = 0.75f;
    HH_ASSERT(hh_beacon_encode(&in, buf, sizeof buf) > 0);
    HH_ASSERT_OK(hh_beacon_decode(buf, sizeof buf, &out));
    HH_ASSERT(out.position_valid);
    HH_ASSERT_NEAR(out.position_x, 12.5, 0.11);
    HH_ASSERT(out.power_valid);
    HH_ASSERT_NEAR(out.power_battery, 0.75, 0.001);
}

static void test_version_mismatch_rejected(void)
{
    uint8_t buf[HH_BEACON_WIRE_LEN];
    hh_beacon_t in = sample_beacon(), out;
    in.protocol_version = HH_PROTOCOL_VERSION + 7;
    HH_ASSERT(hh_beacon_encode(&in, buf, sizeof buf) > 0);
    /* Wire-compatibility check must fire before any field is trusted. */
    HH_ASSERT_ERR(hh_beacon_decode(buf, sizeof buf, &out), HH_ERR_UNSUPPORTED);
}

static void test_malformed_input_rejected(void)
{
    uint8_t buf[HH_BEACON_WIRE_LEN];
    hh_beacon_t in = sample_beacon(), out;

    HH_ASSERT(hh_beacon_encode(&in, buf, sizeof buf) > 0);
    /* Truncated frame. */
    HH_ASSERT_ERR(hh_beacon_decode(buf, 10, &out), HH_ERR_INVAL);
    HH_ASSERT_ERR(hh_beacon_decode(NULL, sizeof buf, &out), HH_ERR_INVAL);

    /* node_id 0 is the invalid/broadcast sentinel and must not decode. */
    in.node_id = 0;
    HH_ASSERT(hh_beacon_encode(&in, buf, sizeof buf) > 0);
    HH_ASSERT_ERR(hh_beacon_decode(buf, sizeof buf, &out), HH_ERR_INVAL);
}

static void test_encode_rejects_small_buffer(void)
{
    uint8_t small[8];
    hh_beacon_t in = sample_beacon();
    HH_ASSERT_EQ_INT(hh_beacon_encode(&in, small, sizeof small), 0);
}

static void test_route_update_roundtrip(void)
{
    uint8_t buf[512];
    hh_route_update_t in, out;
    size_t n;

    memset(&in, 0, sizeof in);
    in.sender = 11;
    in.count = 3;
    for (int i = 0; i < 3; i++) {
        in.entries[i].originator  = (hh_node_id_t)(100 + i);
        in.entries[i].sequence_no = (hh_seq_t)(50 + i);
        in.entries[i].hop_count   = (uint8_t)(i + 1);
        in.entries[i].metric      = 0.25f * (float)(i + 1);
    }
    n = hh_route_update_encode(&in, buf, sizeof buf);
    HH_ASSERT(n > 0);
    HH_ASSERT_OK(hh_route_update_decode(buf, n, &out));

    HH_ASSERT_EQ_INT(out.sender, 11);
    HH_ASSERT_EQ_INT(out.count, 3);
    for (int i = 0; i < 3; i++) {
        HH_ASSERT_EQ_INT(out.entries[i].originator, 100 + i);
        HH_ASSERT_EQ_INT(out.entries[i].sequence_no, 50 + i);
        HH_ASSERT_EQ_INT(out.entries[i].hop_count, i + 1);
        HH_ASSERT_NEAR(out.entries[i].metric, 0.25 * (i + 1), 0.001);
    }
}

static void test_route_update_rejects_overlong_and_truncated(void)
{
    uint8_t buf[512];
    hh_route_update_t in, out;
    size_t n;

    memset(&in, 0, sizeof in);
    in.sender = 1;
    in.count = HH_ROUTE_UPDATE_MAX_ENTRIES + 1;
    HH_ASSERT_EQ_INT(hh_route_update_encode(&in, buf, sizeof buf), 0);

    in.count = 3;
    n = hh_route_update_encode(&in, buf, sizeof buf);
    HH_ASSERT(n > 0);
    /* Claimed count exceeds the bytes present. */
    HH_ASSERT_ERR(hh_route_update_decode(buf, n - 1, &out), HH_ERR_INVAL);
    HH_ASSERT_ERR(hh_route_update_decode(buf, 2, &out), HH_ERR_INVAL);
}

HH_TEST_MAIN_BEGIN("wire")
    HH_RUN(test_beacon_roundtrip);
    HH_RUN(test_beacon_wire_is_endian_explicit);
    HH_RUN(test_optional_fields_degrade_gracefully);
    HH_RUN(test_version_mismatch_rejected);
    HH_RUN(test_malformed_input_rejected);
    HH_RUN(test_encode_rejects_small_buffer);
    HH_RUN(test_route_update_roundtrip);
    HH_RUN(test_route_update_rejects_overlong_and_truncated);
HH_TEST_MAIN_END()
