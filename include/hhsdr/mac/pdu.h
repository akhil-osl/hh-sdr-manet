/*
 * MAC PDU v2 codec, as in MAC frame specification v2 (part 4): Frame
 * Control, header, control block and messages, closed by a CRC-32. All
 * multi-byte fields are little-endian.
 *
 * Not covered: the security header and authentication tag, extension
 * elements, and the bodies of MAC commands. A PDU with the security or
 * elements bit set is refused with HH_ERR_UNSUPPORTED.
 *
 * No allocation. Decoded message data points into the input buffer.
 */
#ifndef HHSDR_MAC_PDU_H
#define HHSDR_MAC_PDU_H

#include "hhsdr/core/types.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define HH_PDU_FRAME_CONTROL  0xA841u  /* data, 2-byte addresses, version 2 */
#define HH_PDU_MAX_LEN        2047u    /* 11-bit length in the burst header */
#define HH_PDU_BITMAP_MAX     8u       /* slot bitmap bytes: up to 64 slots per frame */
#define HH_PDU_MSG_MAX        16u
#define HH_PDU_BACK_MAX       15u
#define HH_PDU_LINK_MAX       15u
#define HH_PDU_ADDR_BROADCAST 0xFFFFu

/* Protocol IDs, section 4.4. */
#define HH_PDU_PROTO_IPV6     0x01u
#define HH_PDU_PROTO_IPV4     0x02u
#define HH_PDU_PROTO_COMMAND  0x20u
#define HH_PDU_PROTO_BEACON   0x30u
#define HH_PDU_PROTO_ROUTING  0x31u
#define HH_PDU_PROTO_OPAQUE   0x32u
#define HH_PDU_PROTO_TEST     0xF0u

typedef struct {
    uint16_t neighbor;
    uint8_t  start;    /* start sequence */
    uint32_t bitmap;
} hh_pdu_back_t;

typedef struct {
    uint16_t neighbor;
    uint8_t  error_rate;  /* 0.5 % steps, 255 = not valid */
    uint8_t  snr;         /* 0.25 dB steps, 255 = not valid */
} hh_pdu_link_t;

typedef struct {
    uint32_t founding_superframe;
    uint16_t founder;
    uint32_t superframe;
    uint8_t  entry_slot;   /* 0xFF = none */
    uint8_t  window;
    uint8_t  members;
    uint8_t  heard[HH_PDU_BITMAP_MAX];   /* slot_bytes bytes used */
} hh_pdu_beacon_t;

typedef struct {
    uint8_t         proto;
    uint16_t        dst;
    uint8_t         cls;           /* 0 to 7 */
    bool            ack_req;
    uint8_t         link_seq;      /* with ack_req */
    bool            frag;
    uint16_t        dgram_size;    /* with frag */
    uint16_t        dgram_tag;
    uint8_t         frag_offset;   /* units of 8 bytes */
    const uint8_t  *data;
    uint16_t        len;           /* 0 to 2047 */
} hh_pdu_msg_t;

typedef struct {
    uint8_t  seq;
    uint16_t net_id;
    uint16_t src;
    bool     more_data;
    /* Control block, section 4.3. */
    uint8_t  time_quality;
    bool     breakin;
    uint8_t  breakin_hops;         /* 0 to 7 */
    uint16_t breakin_invoker;
    uint8_t  slot_bytes;           /* L, 0 to HH_PDU_BITMAP_MAX */
    uint8_t  claims[HH_PDU_BITMAP_MAX];
    uint8_t  n_back;
    hh_pdu_back_t back[HH_PDU_BACK_MAX];
    uint8_t  n_link;
    hh_pdu_link_t link[HH_PDU_LINK_MAX];
    bool     has_beacon;
    hh_pdu_beacon_t beacon;
    bool     has_qreport;
    uint8_t  qreport[5];
    uint8_t  n_msg;
    hh_pdu_msg_t msg[HH_PDU_MSG_MAX];
} hh_pdu_t;

/* IEEE 802.3 CRC-32 (check value of "123456789": 0xCBF43926). */
uint32_t hh_crc32(const uint8_t *data, size_t len);

/* Bytes the PDU takes on air, CRC included. 0 if it is not encodable. */
size_t hh_pdu_size(const hh_pdu_t *p);

/* Bytes of PDU without messages, so a scheduler knows the room for messages. */
size_t hh_pdu_overhead(const hh_pdu_t *p);

/* Bytes one message takes, sub-header and fragment header included. */
size_t hh_pdu_msg_size(const hh_pdu_msg_t *m);

hh_status_t hh_pdu_encode(const hh_pdu_t *p, uint8_t *buf, size_t cap, size_t *out_len);

/* Checks the CRC and every length. HH_ERR_UNSUPPORTED for security or elements. */
hh_status_t hh_pdu_decode(const uint8_t *buf, size_t len, hh_pdu_t *p);

#endif /* HHSDR_MAC_PDU_H */
