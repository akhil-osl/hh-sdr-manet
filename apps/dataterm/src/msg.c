#include "hhsdr/dataterm/msg.h"
#include <string.h>

static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);  p[3] = (uint8_t)v;
}
static uint16_t get16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }
static uint32_t get32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

const char *hh_dt_rx_err_str(hh_dt_rx_err_t e)
{
    switch (e) {
    case HH_DT_RX_OK:      return "ok";
    case HH_DT_RX_SHORT:   return "short";
    case HH_DT_RX_LENGTH:  return "length";
    case HH_DT_RX_CRC:     return "crc";
    case HH_DT_RX_VERSION: return "version";
    case HH_DT_RX_TYPE:    return "type";
    }
    return "?";
}

uint16_t hh_dt_crc16(const uint8_t *p, size_t n, uint16_t crc)
{
    for (size_t i = 0; i < n; i++) {
        crc ^= (uint16_t)(p[i] << 8);
        for (int b = 0; b < 8; b++)
            crc = (crc & 0x8000u) ? (uint16_t)((crc << 1) ^ 0x1021u) : (uint16_t)(crc << 1);
    }
    return crc;
}

hh_status_t hh_dt_encode(const hh_dt_hdr_t *h, const uint8_t *payload,
                         uint8_t *out, size_t cap, size_t *out_len)
{
    if (!h || !out || !out_len || (h->len && !payload)) return HH_ERR_INVAL;
    if (h->len > HH_DT_PAYLOAD_MAX) return HH_ERR_INVAL;
    size_t total = HH_DT_HDR_LEN + h->len;
    if (cap < total) return HH_ERR_NOMEM;

    out[0] = HH_DT_VERSION;
    out[1] = h->type;
    out[2] = h->flags;
    out[3] = h->attempt;
    put32(out + 4, h->src);
    put32(out + 8, h->dst);
    put32(out + 12, h->seq);
    put16(out + 16, h->len);
    put16(out + 18, 0);
    if (h->len) memcpy(out + HH_DT_HDR_LEN, payload, h->len);
    put16(out + 18, hh_dt_crc16(out, total, 0xFFFFu));
    *out_len = total;
    return HH_OK;
}

hh_dt_rx_err_t hh_dt_decode(const uint8_t *in, size_t n, hh_dt_hdr_t *h,
                            const uint8_t **payload)
{
    if (!in || n < HH_DT_HDR_LEN) return HH_DT_RX_SHORT;
    uint16_t len = get16(in + 16);
    if ((size_t)len != n - HH_DT_HDR_LEN) return HH_DT_RX_LENGTH;

    uint16_t want = get16(in + 18);
    static const uint8_t zero[2] = { 0, 0 };
    uint16_t crc = hh_dt_crc16(in, 18, 0xFFFFu);
    crc = hh_dt_crc16(zero, 2, crc);
    crc = hh_dt_crc16(in + HH_DT_HDR_LEN, len, crc);
    if (crc != want) return HH_DT_RX_CRC;

    if (in[0] != HH_DT_VERSION) return HH_DT_RX_VERSION;
    if (in[1] != HH_DT_TYPE_DATA && in[1] != HH_DT_TYPE_ACK) return HH_DT_RX_TYPE;

    if (h) {
        h->version = in[0];
        h->type    = in[1];
        h->flags   = in[2];
        h->attempt = in[3];
        h->src     = get32(in + 4);
        h->dst     = get32(in + 8);
        h->seq     = get32(in + 12);
        h->len     = len;
    }
    if (payload) *payload = in + HH_DT_HDR_LEN;
    return HH_DT_RX_OK;
}
