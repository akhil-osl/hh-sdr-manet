/* Frame conversion for hh-slot-bridge. See slot_bridge_frames.h. */
#include "slot_bridge_frames.h"
#include <string.h>

#define ETHERTYPE_IPV4 0x0800u
#define IP_MIN_HDR     20u
#define IP_PROTO_UDP   17u
#define IP_PROTO_MANET 138u    /* RFC 5498 */
#define UDP_PORT_MANET 269u    /* RFC 5498 */

/* Writes the link address of a node id. */
void br_mac_for_node(uint8_t mac[6], uint16_t id)
{
    mac[0] = 0x02; mac[1] = 0; mac[2] = 0; mac[3] = 0;
    mac[4] = (uint8_t)(id >> 8);
    mac[5] = (uint8_t)id;
}

/* Reads a node id from a link address. False if it is not 02-00-00-00-hh-ll. */
int br_node_for_mac(const uint8_t mac[6], uint16_t *id)
{
    if (mac[0] != 0x02 || mac[1] || mac[2] || mac[3]) return 0;
    *id = (uint16_t)((mac[4] << 8) | mac[5]);
    return *id != 0;
}

/* Returns true if the IP packet is OLSRv2 traffic: UDP port 269 or IP protocol 138. */
static int is_manet(const uint8_t *ip, uint16_t len)
{
    unsigned ihl = (unsigned)(ip[0] & 0x0F) * 4u;
    unsigned frag_off = (unsigned)(((ip[6] & 0x1F) << 8) | ip[7]);

    if (ip[9] == IP_PROTO_MANET) return 1;
    /* The UDP header is only in the first fragment. */
    if (ip[9] != IP_PROTO_UDP || frag_off != 0 || ihl < IP_MIN_HDR || len < ihl + 8u) return 0;
    return (unsigned)((ip[ihl + 2] << 8) | ip[ihl + 3]) == UDP_PORT_MANET;
}

/* Turns an Ethernet frame from the TAP into what the MAC sends. */
br_result_t br_from_eth(const uint8_t *frame, size_t len, br_tx_t *out)
{
    uint16_t id;

    if (len < BR_ETH_LEN + IP_MIN_HDR) return BR_DROP_SHORT;
    if ((unsigned)((frame[12] << 8) | frame[13]) != ETHERTYPE_IPV4) return BR_DROP_NOT_IPV4;
    if ((frame[BR_ETH_LEN] >> 4) != 4) return BR_DROP_NOT_IPV4;

    if (frame[0] & 1u) out->dst = HH_MAC_BROADCAST;   /* multicast or broadcast */
    else if (br_node_for_mac(frame, &id)) out->dst = id;
    else return BR_DROP_BAD_MAC;

    out->ip = frame + BR_ETH_LEN;
    out->ip_len = (uint16_t)(len - BR_ETH_LEN);
    if (is_manet(out->ip, out->ip_len)) {
        out->kind = HH_FRAME_ROUTING;
        out->cls = HH_MAC_CLASS_CONTROL;
    } else {
        out->kind = HH_FRAME_DATA;
        out->cls = HH_MAC_CLASS_DATA;
    }
    return BR_OK;
}

/* Builds the Ethernet frame for a packet the MAC delivered. */
size_t br_to_eth(uint8_t *out, size_t cap, hh_node_id_t self, hh_node_id_t src, hh_node_id_t dst,
                 const uint8_t *ip, uint16_t ip_len)
{
    if (ip_len < IP_MIN_HDR || (ip[0] >> 4) != 4 || cap < BR_ETH_LEN + (size_t)ip_len) return 0;
    if (dst == HH_MAC_BROADCAST) {
        if ((ip[16] & 0xF0) == 0xE0) {   /* IPv4 multicast: 01-00-5e plus 23 bits of the group */
            out[0] = 0x01; out[1] = 0x00; out[2] = 0x5E;
            out[3] = ip[17] & 0x7F; out[4] = ip[18]; out[5] = ip[19];
        } else {
            memset(out, 0xFF, 6);
        }
    } else {
        br_mac_for_node(out, (uint16_t)self);
    }
    br_mac_for_node(out + 6, (uint16_t)src);
    out[12] = ETHERTYPE_IPV4 >> 8;
    out[13] = ETHERTYPE_IPV4 & 0xFF;
    memcpy(out + BR_ETH_LEN, ip, ip_len);
    return BR_ETH_LEN + (size_t)ip_len;
}

/* Names a conversion result for logs. */
const char *br_result_str(br_result_t r)
{
    switch (r) {
    case BR_OK:           return "ok";
    case BR_DROP_SHORT:   return "short";
    case BR_DROP_NOT_IPV4: return "not_ipv4";
    case BR_DROP_BAD_MAC: return "bad_mac";
    }
    return "?";
}
