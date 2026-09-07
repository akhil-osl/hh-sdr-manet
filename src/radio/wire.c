#include "hhsdr/radio/wire.h"
#include <string.h>

/* ---- little-endian primitives ---- */
static void put_u8 (uint8_t **p, uint8_t v)  { *(*p)++ = v; }
static void put_u16(uint8_t **p, uint16_t v) { put_u8(p,(uint8_t)v); put_u8(p,(uint8_t)(v>>8)); }
static void put_u32(uint8_t **p, uint32_t v) { put_u16(p,(uint16_t)v); put_u16(p,(uint16_t)(v>>16)); }
static void put_u64(uint8_t **p, uint64_t v) { put_u32(p,(uint32_t)v); put_u32(p,(uint32_t)(v>>32)); }

static uint8_t  get_u8 (const uint8_t **p) { return *(*p)++; }
static uint16_t get_u16(const uint8_t **p) { uint16_t a=get_u8(p); return (uint16_t)(a|((uint16_t)get_u8(p)<<8)); }
static uint32_t get_u32(const uint8_t **p) { uint32_t a=get_u16(p); return a|((uint32_t)get_u16(p)<<16); }
static uint64_t get_u64(const uint8_t **p) { uint64_t a=get_u32(p); return a|((uint64_t)get_u32(p)<<32); }

/* Metrics travel as scaled integers so the wire format does not depend on host
 * float representation. Scale choices are part of this implementation's format. */
static int16_t  enc_dbm(float v)      { return (int16_t)(v * 10.0f);   }   /* 0.1 dB   */
static float    dec_dbm(int16_t v)    { return (float)v / 10.0f;       }
static uint16_t enc_frac(float v)     { if (v<0) v=0; if (v>1) v=1; return (uint16_t)(v*10000.0f); }
static float    dec_frac(uint16_t v)  { return (float)v / 10000.0f;    }

size_t hh_beacon_encode(const hh_beacon_t *b, uint8_t *buf, size_t cap)
{
    uint8_t *p = buf;
    uint8_t flags;

    if (!b || !buf || cap < HH_BEACON_WIRE_LEN) return 0;

    put_u32(&p, b->node_id);                    /*  4 */
    put_u16(&p, b->protocol_version);           /*  2 */
    put_u32(&p, b->sequence_no);                /*  4 */
    put_u64(&p, b->timestamp);                  /*  8 */
    put_u32(&p, b->capabilities);               /*  4 */
    put_u32(&p, b->radio_caps);                 /*  4 */
    put_u32(&p, b->supported_waveforms);        /*  4 */
    put_u32(&p, (uint32_t)(b->channel_freq));   /*  4 (Hz, integral)          */

    flags = 0;
    if (b->routing_capable) flags |= 0x01;
    if (b->position_valid)  flags |= 0x02;
    if (b->power_valid)     flags |= 0x04;
    put_u8(&p, flags);                          /*  1 */

    /* Optional fields occupy their slots regardless, keeping the frame fixed
     * length; the flags above say whether they carry meaning (Doc 1 §4:
     * "must degrade gracefully when absent"). */
    put_u16(&p, (uint16_t)enc_dbm(b->position_x));  /* 2 */
    put_u16(&p, (uint16_t)enc_dbm(b->position_y));  /* 2 */
    put_u16(&p, (uint16_t)enc_dbm(b->position_z));  /* 2 */
    put_u16(&p, enc_frac(b->power_battery));        /* 2 */
    put_u8(&p, 0);                                  /* 1 reserved */
    /* total = 44; pad to the declared fixed length */
    while ((size_t)(p - buf) < HH_BEACON_WIRE_LEN) put_u8(&p, 0);

    return (size_t)(p - buf);
}

hh_status_t hh_beacon_decode(const uint8_t *buf, size_t len, hh_beacon_t *out)
{
    const uint8_t *p = buf;
    uint8_t flags;

    if (!buf || !out || len < HH_BEACON_WIRE_LEN) return HH_ERR_INVAL;
    memset(out, 0, sizeof *out);

    out->node_id          = get_u32(&p);
    out->protocol_version = get_u16(&p);
    /* Wire-compatibility check before interpreting any further field. */
    if (out->protocol_version != HH_PROTOCOL_VERSION) return HH_ERR_UNSUPPORTED;

    out->sequence_no        = get_u32(&p);
    out->timestamp          = get_u64(&p);
    out->capabilities       = get_u32(&p);
    out->radio_caps         = get_u32(&p);
    out->supported_waveforms= get_u32(&p);
    out->channel_freq       = (float)get_u32(&p);

    flags = get_u8(&p);
    out->routing_capable = (flags & 0x01) != 0;
    out->position_valid  = (flags & 0x02) != 0;
    out->power_valid     = (flags & 0x04) != 0;

    out->position_x    = dec_dbm((int16_t)get_u16(&p));
    out->position_y    = dec_dbm((int16_t)get_u16(&p));
    out->position_z    = dec_dbm((int16_t)get_u16(&p));
    out->power_battery = dec_frac(get_u16(&p));

    /* A node claiming id 0 is malformed: 0 is the invalid/broadcast sentinel. */
    if (out->node_id == HH_NODE_ID_INVALID) return HH_ERR_INVAL;
    return HH_OK;
}

size_t hh_route_update_encode(const hh_route_update_t *u, uint8_t *buf, size_t cap)
{
    uint8_t *p = buf;
    size_t need;

    if (!u || !buf) return 0;
    if (u->count > HH_ROUTE_UPDATE_MAX_ENTRIES) return 0;
    need = 5u + (size_t)u->count * 11u;
    if (cap < need) return 0;

    put_u32(&p, u->sender);
    put_u8(&p, u->count);
    for (uint8_t i = 0; i < u->count; i++) {
        const hh_route_update_entry_t *e = &u->entries[i];
        put_u32(&p, e->originator);
        put_u32(&p, e->sequence_no);
        put_u8(&p, e->hop_count);
        put_u16(&p, enc_frac(e->metric > 1.0f ? 1.0f : e->metric));
    }
    return (size_t)(p - buf);
}

hh_status_t hh_route_update_decode(const uint8_t *buf, size_t len, hh_route_update_t *out)
{
    const uint8_t *p = buf;
    uint8_t count;

    if (!buf || !out || len < 5u) return HH_ERR_INVAL;
    memset(out, 0, sizeof *out);

    out->sender = get_u32(&p);
    count = get_u8(&p);
    if (count > HH_ROUTE_UPDATE_MAX_ENTRIES) return HH_ERR_INVAL;
    if (len < 5u + (size_t)count * 11u) return HH_ERR_INVAL;
    if (out->sender == HH_NODE_ID_INVALID) return HH_ERR_INVAL;

    out->count = count;
    for (uint8_t i = 0; i < count; i++) {
        hh_route_update_entry_t *e = &out->entries[i];
        e->originator  = get_u32(&p);
        e->sequence_no = get_u32(&p);
        e->hop_count   = get_u8(&p);
        e->metric      = dec_frac(get_u16(&p));
    }
    return HH_OK;
}
