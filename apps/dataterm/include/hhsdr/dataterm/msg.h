/*
 * Data terminal message: a 20-byte header and a payload, carried as one UDP
 * datagram. Big endian, packed by hand.
 *
 *   0 version   1 type   2 flags   3 attempt
 *   4 source node id (4)
 *   8 destination node id (4)
 *  12 sequence number (4)
 *  16 payload length (2)   18 CRC-16 (2)
 *  20 payload
 *
 * CRC-16-CCITT (poly 0x1021, init 0xFFFF) over header and payload, with the
 * CRC field taken as 0.
 */
#ifndef HHSDR_DATATERM_MSG_H
#define HHSDR_DATATERM_MSG_H

#include "hhsdr/core/types.h"

#define HH_DT_VERSION       1u
#define HH_DT_HDR_LEN       20u
/* Ethernet 1500 minus IPv4 20, UDP 8 and our header: the buffer bound. */
#define HH_DT_PAYLOAD_MAX   1452u
#define HH_DT_MSG_MAX       (HH_DT_HDR_LEN + HH_DT_PAYLOAD_MAX)

#define HH_DT_FLAG_ACK_REQ  0x01u

typedef enum {
    HH_DT_TYPE_DATA = 0x01,
    HH_DT_TYPE_ACK  = 0x02
} hh_dt_type_t;

typedef struct {
    uint8_t      version;
    uint8_t      type;
    uint8_t      flags;
    uint8_t      attempt;
    hh_node_id_t src;
    hh_node_id_t dst;
    hh_seq_t     seq;
    uint16_t     len;
} hh_dt_hdr_t;

typedef enum {
    HH_DT_RX_OK = 0,
    HH_DT_RX_SHORT,      /* shorter than the header           */
    HH_DT_RX_LENGTH,     /* length field disagrees with size  */
    HH_DT_RX_CRC,
    HH_DT_RX_VERSION,
    HH_DT_RX_TYPE
} hh_dt_rx_err_t;

const char *hh_dt_rx_err_str(hh_dt_rx_err_t e);

uint16_t hh_dt_crc16(const uint8_t *p, size_t n, uint16_t crc);

/* Writes header and payload to out. Sets version and CRC itself. */
hh_status_t hh_dt_encode(const hh_dt_hdr_t *h, const uint8_t *payload,
                         uint8_t *out, size_t cap, size_t *out_len);

/* On HH_DT_RX_OK, *payload points into in. */
hh_dt_rx_err_t hh_dt_decode(const uint8_t *in, size_t n, hh_dt_hdr_t *h,
                            const uint8_t **payload);

#endif /* HHSDR_DATATERM_MSG_H */
