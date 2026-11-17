/*
 * Frame conversion for hh-slot-bridge: a Linux TAP interface carries Ethernet
 * frames, the MAC carries the IP packet inside. The Ethernet header is removed
 * on the way in and rebuilt on the way out. Development tooling only.
 *
 * Address mapping is the one of MAC frame specification v2 section 4.5: the
 * Linux link address of on-air address hhll is 02-00-00-00-hh-ll.
 * Which frame kind and class an IP packet gets is not decided for the project
 * (unknown.md U-24); this tool sends OLSRv2 packets (UDP port 269 or IP
 * protocol 138) as routing control and everything else as data.
 */
#ifndef HH_SLOT_BRIDGE_FRAMES_H
#define HH_SLOT_BRIDGE_FRAMES_H

#include "hhsdr/mac/mac.h"
#include <stddef.h>
#include <stdint.h>

#define BR_ETH_LEN 14u

typedef enum {
    BR_OK = 0,
    BR_DROP_SHORT,       /* too short for an Ethernet and IPv4 header */
    BR_DROP_NOT_IPV4,    /* ARP, IPv6 and anything else */
    BR_DROP_BAD_MAC      /* unicast destination that is not 02-00-00-00-hh-ll */
} br_result_t;

typedef struct {
    hh_frame_kind_t kind;
    hh_mac_class_t  cls;
    hh_node_id_t    dst;      /* HH_MAC_BROADCAST or a node id */
    const uint8_t  *ip;       /* the IP packet, inside the input frame */
    uint16_t        ip_len;
} br_tx_t;

/* Writes the link address of a node id. */
void br_mac_for_node(uint8_t mac[6], uint16_t id);

/* Reads a node id from a link address. False if it is not 02-00-00-00-hh-ll. */
int br_node_for_mac(const uint8_t mac[6], uint16_t *id);

/* Turns an Ethernet frame from the TAP into what the MAC sends. */
br_result_t br_from_eth(const uint8_t *frame, size_t len, br_tx_t *out);

/* Builds the Ethernet frame for a packet the MAC delivered. Returns its length,
 * or 0 if it does not fit or is not an IPv4 packet. */
size_t br_to_eth(uint8_t *out, size_t cap, hh_node_id_t self, hh_node_id_t src, hh_node_id_t dst,
                 const uint8_t *ip, uint16_t ip_len);

const char *br_result_str(br_result_t r);

#endif /* HH_SLOT_BRIDGE_FRAMES_H */
