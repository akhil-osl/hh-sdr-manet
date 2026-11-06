/* MAC PDU v2 codec. See include/hhsdr/mac/pdu.h. */
#include "hhsdr/mac/pdu.h"
#include <string.h>

#define HDR_LEN      9u   /* Frame Control, sequence, Net ID, destination, source */
#define CTL_FIXED    6u   /* control block without the bitmap and lists */
#define BACK_LEN     7u
#define LINK_LEN     4u
#define QREPORT_LEN  5u
#define BEACON_FIXED 13u
#define CRC_LEN      4u
#define SUB_LEN      5u
#define FRAG_LEN     5u

#define FC_SECURITY  0x0008u
#define FC_MORE_DATA 0x0010u
#define FC_ELEMENTS  0x0200u

uint32_t hh_crc32(const uint8_t *data, size_t len)
{
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++)
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

size_t hh_pdu_msg_size(const hh_pdu_msg_t *m)
{
    return SUB_LEN + (m->ack_req ? 1u : 0u) + (m->frag ? FRAG_LEN : 0u) + m->len;
}

static bool valid(const hh_pdu_t *p)
{
    if (p->slot_bytes > HH_PDU_BITMAP_MAX || p->n_back > HH_PDU_BACK_MAX ||
        p->n_link > HH_PDU_LINK_MAX || p->n_msg > HH_PDU_MSG_MAX || p->breakin_hops > 7u)
        return false;
    for (unsigned i = 0; i < p->n_msg; i++) {
        const hh_pdu_msg_t *m = &p->msg[i];
        if (m->len > HH_PDU_MAX_LEN || m->cls > 7u || (m->len && !m->data)) return false;
        if (m->frag && (m->dgram_size > 2047u)) return false;
    }
    return true;
}

size_t hh_pdu_overhead(const hh_pdu_t *p)
{
    size_t n = HDR_LEN + CTL_FIXED + p->slot_bytes + BACK_LEN * (size_t)p->n_back +
               LINK_LEN * (size_t)p->n_link + CRC_LEN;
    if (p->has_beacon) n += BEACON_FIXED + p->slot_bytes;
    if (p->has_qreport) n += QREPORT_LEN;
    return n;
}

size_t hh_pdu_size(const hh_pdu_t *p)
{
    size_t n;
    if (!valid(p)) return 0;
    n = hh_pdu_overhead(p);
    for (unsigned i = 0; i < p->n_msg; i++) n += hh_pdu_msg_size(&p->msg[i]);
    return n <= HH_PDU_MAX_LEN ? n : 0;
}

typedef struct {
    uint8_t *b;
    size_t   n;
} wr_t;

static void w8(wr_t *w, uint8_t v) { w->b[w->n++] = v; }
static void w16(wr_t *w, uint16_t v) { w8(w, (uint8_t)v); w8(w, (uint8_t)(v >> 8)); }
static void w32(wr_t *w, uint32_t v) { w16(w, (uint16_t)v); w16(w, (uint16_t)(v >> 16)); }
static void wn(wr_t *w, const uint8_t *s, size_t len) { if (len) memcpy(w->b + w->n, s, len); w->n += len; }

hh_status_t hh_pdu_encode(const hh_pdu_t *p, uint8_t *buf, size_t cap, size_t *out_len)
{
    wr_t w = { buf, 0 };
    size_t total;

    if (!p || !buf || !out_len) return HH_ERR_INVAL;
    total = hh_pdu_size(p);
    if (!total) return HH_ERR_INVAL;
    if (total > cap) return HH_ERR_NOMEM;

    w16(&w, (uint16_t)(HH_PDU_FRAME_CONTROL | (p->more_data ? FC_MORE_DATA : 0u)));
    w8(&w, p->seq);
    w16(&w, p->net_id);
    w16(&w, HH_PDU_ADDR_BROADCAST);
    w16(&w, p->src);

    w8(&w, p->time_quality);
    w8(&w, (uint8_t)((p->breakin ? 0x01u : 0u) | (p->breakin_hops << 1) |
                     (p->has_beacon ? 0x10u : 0u) | (p->has_qreport ? 0x20u : 0u)));
    w16(&w, p->breakin_invoker);
    w8(&w, (uint8_t)(p->n_back | (p->n_link << 4)));
    w8(&w, p->slot_bytes);
    wn(&w, p->claims, p->slot_bytes);
    for (unsigned i = 0; i < p->n_back; i++) {
        w16(&w, p->back[i].neighbor);
        w8(&w, p->back[i].start);
        w32(&w, p->back[i].bitmap);
    }
    for (unsigned i = 0; i < p->n_link; i++) {
        w16(&w, p->link[i].neighbor);
        w8(&w, p->link[i].error_rate);
        w8(&w, p->link[i].snr);
    }
    if (p->has_beacon) {
        w32(&w, p->beacon.founding_superframe);
        w16(&w, p->beacon.founder);
        w32(&w, p->beacon.superframe);
        w8(&w, p->beacon.entry_slot);
        w8(&w, p->beacon.window);
        w8(&w, p->beacon.members);
        wn(&w, p->beacon.heard, p->slot_bytes);
    }
    if (p->has_qreport) wn(&w, p->qreport, QREPORT_LEN);

    for (unsigned i = 0; i < p->n_msg; i++) {
        const hh_pdu_msg_t *m = &p->msg[i];
        w8(&w, m->proto);
        w16(&w, m->dst);
        w16(&w, (uint16_t)(m->len | ((unsigned)m->cls << 11) |
                           (m->frag ? 0x4000u : 0u) | (m->ack_req ? 0x8000u : 0u)));
        if (m->ack_req) w8(&w, m->link_seq);
        if (m->frag) {
            w16(&w, m->dgram_size);
            w16(&w, m->dgram_tag);
            w8(&w, m->frag_offset);
        }
        wn(&w, m->data, m->len);
    }

    w32(&w, hh_crc32(buf, w.n));
    *out_len = w.n;
    return HH_OK;
}

typedef struct {
    const uint8_t *b;
    size_t         n;
    size_t         end;
    bool           bad;
} rd_t;

static bool need(rd_t *r, size_t k)
{
    if (r->bad || r->end - r->n < k) { r->bad = true; return false; }
    return true;
}

static uint8_t r8(rd_t *r) { return need(r, 1) ? r->b[r->n++] : 0u; }
static uint16_t r16(rd_t *r) { uint16_t lo = r8(r); return (uint16_t)(lo | ((uint16_t)r8(r) << 8)); }
static uint32_t r32(rd_t *r) { uint32_t lo = r16(r); return lo | ((uint32_t)r16(r) << 16); }
static void rn(rd_t *r, uint8_t *d, size_t k) { if (need(r, k)) { memcpy(d, r->b + r->n, k); r->n += k; } }

hh_status_t hh_pdu_decode(const uint8_t *buf, size_t len, hh_pdu_t *p)
{
    rd_t r;
    uint16_t fc;
    uint8_t flags, counts;
    uint32_t crc;

    if (!buf || !p) return HH_ERR_INVAL;
    if (len < HDR_LEN + CTL_FIXED + CRC_LEN || len > HH_PDU_MAX_LEN) return HH_ERR_INVAL;

    crc = (uint32_t)buf[len - 4] | ((uint32_t)buf[len - 3] << 8) |
          ((uint32_t)buf[len - 2] << 16) | ((uint32_t)buf[len - 1] << 24);
    if (hh_crc32(buf, len - CRC_LEN) != crc) return HH_ERR_INVAL;

    memset(p, 0, sizeof *p);
    r.b = buf; r.n = 0; r.end = len - CRC_LEN; r.bad = false;

    fc = r16(&r);
    if (fc & (FC_SECURITY | FC_ELEMENTS)) return HH_ERR_UNSUPPORTED;
    if ((fc & (uint16_t)~FC_MORE_DATA) != HH_PDU_FRAME_CONTROL) return HH_ERR_INVAL;
    p->more_data = (fc & FC_MORE_DATA) != 0;
    p->seq = r8(&r);
    p->net_id = r16(&r);
    (void)r16(&r);   /* PDU destination, always broadcast */
    p->src = r16(&r);

    p->time_quality = r8(&r);
    flags = r8(&r);
    p->breakin = (flags & 0x01u) != 0;
    p->breakin_hops = (flags >> 1) & 7u;
    p->has_beacon = (flags & 0x10u) != 0;
    p->has_qreport = (flags & 0x20u) != 0;
    p->breakin_invoker = r16(&r);
    counts = r8(&r);
    p->n_back = counts & 0x0Fu;
    p->n_link = counts >> 4;
    p->slot_bytes = r8(&r);
    if (p->slot_bytes > HH_PDU_BITMAP_MAX) return HH_ERR_INVAL;
    rn(&r, p->claims, p->slot_bytes);
    for (unsigned i = 0; i < p->n_back; i++) {
        p->back[i].neighbor = r16(&r);
        p->back[i].start = r8(&r);
        p->back[i].bitmap = r32(&r);
    }
    for (unsigned i = 0; i < p->n_link; i++) {
        p->link[i].neighbor = r16(&r);
        p->link[i].error_rate = r8(&r);
        p->link[i].snr = r8(&r);
    }
    if (p->has_beacon) {
        p->beacon.founding_superframe = r32(&r);
        p->beacon.founder = r16(&r);
        p->beacon.superframe = r32(&r);
        p->beacon.entry_slot = r8(&r);
        p->beacon.window = r8(&r);
        p->beacon.members = r8(&r);
        rn(&r, p->beacon.heard, p->slot_bytes);
    }
    if (p->has_qreport) rn(&r, p->qreport, QREPORT_LEN);

    while (!r.bad && r.n < r.end) {
        hh_pdu_msg_t *m;
        uint16_t word;
        if (p->n_msg == HH_PDU_MSG_MAX) return HH_ERR_NOMEM;
        m = &p->msg[p->n_msg];
        m->proto = r8(&r);
        m->dst = r16(&r);
        word = r16(&r);
        m->len = word & 0x07FFu;
        m->cls = (word >> 11) & 7u;
        m->frag = (word & 0x4000u) != 0;
        m->ack_req = (word & 0x8000u) != 0;
        if (m->ack_req) m->link_seq = r8(&r);
        if (m->frag) {
            m->dgram_size = r16(&r) & 0x07FFu;
            m->dgram_tag = r16(&r);
            m->frag_offset = r8(&r);
        }
        if (!need(&r, m->len)) break;
        m->data = buf + r.n;
        r.n += m->len;
        p->n_msg++;
    }
    return r.bad ? HH_ERR_INVAL : HH_OK;
}
