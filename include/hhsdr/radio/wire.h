/*
 * Beacon wire format — fixed binary encode/decode.
 *
 * the architecture rejects the prototype's ASCII pipe-delimited wire format; this is the
 * fixed-layout binary replacement. The 
 * records the byte layout as TBD pending a PHY decision, so THIS LAYOUT IS THIS
 * IMPLEMENTATION'S CHOICE, not a specification contract. It is versioned
 * (protocol_version) so it can be replaced without ambiguity once the PHY
 * framing is fixed.
 *
 * Encoding rules: little-endian, no padding, no floats on the wire (metrics are
 * scaled integers so the format does not depend on host float representation).
 */
#ifndef HHSDR_RADIO_WIRE_H
#define HHSDR_RADIO_WIRE_H

#include "hhsdr/core/events.h"
#include "hhsdr/radio/radio.h"

#define HH_PROTOCOL_VERSION 1u
#define HH_BEACON_WIRE_LEN  48u   /* fixed; asserted by the encoder */

/* Encode a beacon into buf. Returns bytes written, or 0 on error. */
size_t hh_beacon_encode(const hh_beacon_t *b, uint8_t *buf, size_t cap);

/* Decode a beacon. Returns HH_ERR_INVAL on a short/malformed buffer and
 * HH_ERR_UNSUPPORTED when protocol_version does not match. */
hh_status_t hh_beacon_decode(const uint8_t *buf, size_t len, hh_beacon_t *out);

/* Routing update (OGM-style,: originator-style, bounded to one hop). */
typedef struct {
    hh_node_id_t originator;   /* node this entry describes    */
    hh_seq_t     sequence_no;
    uint8_t      hop_count;
    float        metric;       /* sender's composite metric    */
} hh_route_update_entry_t;

#define HH_ROUTE_UPDATE_MAX_ENTRIES 24

typedef struct {
    hh_node_id_t            sender;
    uint8_t                 count;
    hh_route_update_entry_t entries[HH_ROUTE_UPDATE_MAX_ENTRIES];
} hh_route_update_t;

size_t      hh_route_update_encode(const hh_route_update_t *u, uint8_t *buf, size_t cap);
hh_status_t hh_route_update_decode(const uint8_t *buf, size_t len, hh_route_update_t *out);

#endif /* HHSDR_RADIO_WIRE_H */
