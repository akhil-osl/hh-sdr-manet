/*
 * hh-slot-bridge frame conversion: node id and link address mapping, routing
 * classification, Ethernet header removal and rebuild. No root needed.
 */
#include "slot_bridge_frames.h"
#include "hh_test.h"
#include <string.h>

/* Builds an Ethernet + IPv4 frame. proto 17 gets a UDP header with dport. */
static size_t make_frame(uint8_t *f, const uint8_t dmac[6], uint16_t src_id, unsigned proto,
                         unsigned dport, const uint8_t dip[4], unsigned payload_len)
{
    size_t n = 0;
    memcpy(f, dmac, 6);
    br_mac_for_node(f + 6, src_id);
    f[12] = 0x08; f[13] = 0x00;
    memset(f + 14, 0, 20);
    f[14] = 0x45;
    f[14 + 9] = (uint8_t)proto;
    memcpy(f + 14 + 16, dip, 4);
    n = 34;
    if (proto == 17) { f[34 + 2] = (uint8_t)(dport >> 8); f[34 + 3] = (uint8_t)dport; n += 8; }
    memset(f + n, 0xAB, payload_len);
    return n + payload_len;
}

/* Node id and link address map both ways, and only 02-00-00-00-hh-ll is accepted. */
static void test_mac_mapping(void)
{
    uint8_t mac[6];
    uint16_t id = 0;
    br_mac_for_node(mac, 0x0102);
    HH_ASSERT_EQ_INT(mac[0], 0x02);
    HH_ASSERT_EQ_INT(mac[4], 0x01);
    HH_ASSERT_EQ_INT(mac[5], 0x02);
    HH_ASSERT(br_node_for_mac(mac, &id));
    HH_ASSERT_EQ_INT(id, 0x0102);
    mac[1] = 1;
    HH_ASSERT(!br_node_for_mac(mac, &id));
    br_mac_for_node(mac, 0);
    HH_ASSERT(!br_node_for_mac(mac, &id));
}

/* An OLSRv2 hello to the MANET multicast group is routing control, sent as broadcast. */
static void test_olsr_multicast_is_routing_broadcast(void)
{
    uint8_t f[200];
    const uint8_t mc[6] = { 0x01, 0x00, 0x5E, 0x00, 0x00, 0x6D };
    const uint8_t ip[4] = { 224, 0, 0, 109 };
    size_t n = make_frame(f, mc, 1, 17, 269, ip, 30);
    br_tx_t t;
    HH_ASSERT_EQ_INT(br_from_eth(f, n, &t), BR_OK);
    HH_ASSERT_EQ_INT(t.kind, HH_FRAME_ROUTING);
    HH_ASSERT_EQ_INT(t.cls, HH_MAC_CLASS_CONTROL);
    HH_ASSERT_EQ_INT(t.dst, HH_MAC_BROADCAST);
    HH_ASSERT_EQ_INT(t.ip_len, n - 14);
    HH_ASSERT(t.ip == f + 14);
}

/* IP protocol 138 is routing control too. */
static void test_ip_protocol_138_is_routing(void)
{
    uint8_t f[200], mac[6];
    const uint8_t ip[4] = { 10, 99, 0, 2 };
    size_t n;
    br_tx_t t;
    br_mac_for_node(mac, 2);
    n = make_frame(f, mac, 1, 138, 0, ip, 20);
    HH_ASSERT_EQ_INT(br_from_eth(f, n, &t), BR_OK);
    HH_ASSERT_EQ_INT(t.kind, HH_FRAME_ROUTING);
    HH_ASSERT_EQ_INT(t.dst, 2);
}

/* Unicast user data goes to the node named by the destination link address. */
static void test_unicast_data_goes_to_next_hop(void)
{
    uint8_t f[200], mac[6];
    const uint8_t ip[4] = { 10, 99, 0, 4 };   /* final destination is not the next hop */
    size_t n;
    br_tx_t t;
    br_mac_for_node(mac, 3);
    n = make_frame(f, mac, 1, 17, 5001, ip, 60);
    HH_ASSERT_EQ_INT(br_from_eth(f, n, &t), BR_OK);
    HH_ASSERT_EQ_INT(t.kind, HH_FRAME_DATA);
    HH_ASSERT_EQ_INT(t.cls, HH_MAC_CLASS_DATA);
    HH_ASSERT_EQ_INT(t.dst, 3);
}

/* A non-first IP fragment has no UDP header, so it is not classified by port. */
static void test_ip_fragment_is_not_classified_by_port(void)
{
    uint8_t f[200], mac[6];
    const uint8_t ip[4] = { 10, 99, 0, 2 };
    size_t n;
    br_tx_t t;
    br_mac_for_node(mac, 2);
    n = make_frame(f, mac, 1, 17, 269, ip, 20);
    f[14 + 7] = 10;   /* fragment offset 10 */
    HH_ASSERT_EQ_INT(br_from_eth(f, n, &t), BR_OK);
    HH_ASSERT_EQ_INT(t.kind, HH_FRAME_DATA);
}

/* ARP, IPv6, short frames and foreign unicast addresses are refused with a reason. */
static void test_rejects(void)
{
    uint8_t f[200], mac[6];
    const uint8_t ip[4] = { 10, 99, 0, 2 };
    size_t n;
    br_tx_t t;

    br_mac_for_node(mac, 2);
    n = make_frame(f, mac, 1, 17, 1, ip, 10);
    f[12] = 0x08; f[13] = 0x06;   /* ARP */
    HH_ASSERT_EQ_INT(br_from_eth(f, n, &t), BR_DROP_NOT_IPV4);
    f[12] = 0x86; f[13] = 0xDD;   /* IPv6 */
    HH_ASSERT_EQ_INT(br_from_eth(f, n, &t), BR_DROP_NOT_IPV4);
    HH_ASSERT_EQ_INT(br_from_eth(f, 20, &t), BR_DROP_SHORT);
    n = make_frame(f, mac, 1, 17, 1, ip, 10);
    f[0] = 0x00; f[1] = 0x11;     /* not 02-00-00-00-hh-ll */
    HH_ASSERT_EQ_INT(br_from_eth(f, n, &t), BR_DROP_BAD_MAC);
}

/* Header removal and rebuild give back the same unicast frame. */
static void test_round_trip_unicast(void)
{
    uint8_t f[200], g[200], mac[6];
    const uint8_t ip[4] = { 10, 99, 0, 3 };
    size_t n, m;
    br_tx_t t;
    br_mac_for_node(mac, 3);
    n = make_frame(f, mac, 1, 17, 5001, ip, 40);
    HH_ASSERT_EQ_INT(br_from_eth(f, n, &t), BR_OK);
    m = br_to_eth(g, sizeof g, 3, 1, t.dst, t.ip, t.ip_len);
    HH_ASSERT_EQ_INT(m, n);
    HH_ASSERT(memcmp(f, g, n) == 0);
}

/* A delivered broadcast gets the multicast or broadcast link address of its IP destination. */
static void test_to_eth_broadcast_addresses(void)
{
    uint8_t ip[28] = { 0x45 }, g[100];
    HH_ASSERT_EQ_INT(br_to_eth(g, sizeof g, 2, 1, HH_MAC_BROADCAST, ip, sizeof ip), 14 + sizeof ip);
    HH_ASSERT_EQ_INT(g[0], 0xFF);                       /* 0.0.0.0 is not multicast */
    ip[16] = 224; ip[17] = 0; ip[18] = 0; ip[19] = 109;
    HH_ASSERT_EQ_INT(br_to_eth(g, sizeof g, 2, 1, HH_MAC_BROADCAST, ip, sizeof ip), 14 + sizeof ip);
    HH_ASSERT_EQ_INT(g[0], 0x01);
    HH_ASSERT_EQ_INT(g[2], 0x5E);
    HH_ASSERT_EQ_INT(g[5], 0x6D);
    HH_ASSERT_EQ_INT(br_to_eth(g, 20, 2, 1, HH_MAC_BROADCAST, ip, sizeof ip), 0);   /* does not fit */
    ip[0] = 0x65;
    HH_ASSERT_EQ_INT(br_to_eth(g, sizeof g, 2, 1, HH_MAC_BROADCAST, ip, sizeof ip), 0);   /* not IPv4 */
}

HH_TEST_MAIN_BEGIN("bridge_frames")
    HH_RUN(test_mac_mapping);
    HH_RUN(test_olsr_multicast_is_routing_broadcast);
    HH_RUN(test_ip_protocol_138_is_routing);
    HH_RUN(test_unicast_data_goes_to_next_hop);
    HH_RUN(test_ip_fragment_is_not_classified_by_port);
    HH_RUN(test_rejects);
    HH_RUN(test_round_trip_unicast);
    HH_RUN(test_to_eth_broadcast_addresses);
HH_TEST_MAIN_END()
