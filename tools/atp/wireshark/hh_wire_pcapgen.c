/*
 * hh_wire_pcapgen — golden captures for validating the Wireshark dissector.
 *
 * Writes beacon and routing-update frames produced by the REAL encoder
 * (src/radio/wire.c) into pcap files, plus a JSON file saying what each frame
 * must decode to. The expected values come from the real decoder, so the
 * dissector is checked against the C implementation itself rather than
 * against a second, hand-maintained description of the layout that could
 * drift from it.
 *
 * Link types: DLT_USER0 (147) carries beacons, DLT_USER1 (148) carries routing
 * updates. The frame kind is not on the wire — hh_frame_t holds it as a struct
 * field, and no over-the-air frame header is defined (unknown.md U-16) — so
 * the capture file's link type stands in for it. USER0..USER15 are reserved by
 * tcpdump.org for exactly this kind of private use; this is a test-tooling
 * convention, NOT an air format.
 *
 * No libpcap dependency: the classic pcap file format is two fixed headers,
 * written here directly.
 *
 * usage: hh_wire_pcapgen OUTDIR
 *   writes OUTDIR/beacons.pcap, OUTDIR/routes.pcap, OUTDIR/expected.json
 *
 * Development tooling only; never linked into a production target.
 */
#include "hhsdr/radio/wire.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#define DLT_USER0 147u
#define DLT_USER1 148u

/* Fixed capture timestamp so the files are byte-identical on every run. */
#define CAPTURE_EPOCH_S 1790000000u

static void put_le32(FILE *f, uint32_t v)
{
    uint8_t b[4] = { (uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24) };
    fwrite(b, 1, sizeof b, f);
}

static void put_le16(FILE *f, uint16_t v)
{
    uint8_t b[2] = { (uint8_t)v, (uint8_t)(v >> 8) };
    fwrite(b, 1, sizeof b, f);
}

static FILE *pcap_open(const char *dir, const char *name, uint32_t linktype)
{
    char path[512];
    FILE *f;

    snprintf(path, sizeof path, "%s/%s", dir, name);
    f = fopen(path, "wb");
    if (!f) { perror(path); return NULL; }
    /* Classic pcap global header, little-endian, microsecond timestamps. */
    put_le32(f, 0xa1b2c3d4u);   /* magic          */
    put_le16(f, 2);             /* version major  */
    put_le16(f, 4);             /* version minor  */
    put_le32(f, 0);             /* thiszone       */
    put_le32(f, 0);             /* sigfigs        */
    put_le32(f, 65535);         /* snaplen        */
    put_le32(f, linktype);
    return f;
}

static void pcap_write(FILE *f, unsigned frame_no, const uint8_t *buf, size_t len)
{
    put_le32(f, CAPTURE_EPOCH_S + frame_no);
    put_le32(f, 0);
    put_le32(f, (uint32_t)len);
    put_le32(f, (uint32_t)len);
    fwrite(buf, 1, len, f);
}

/* ---- expected.json ---------------------------------------------------------
 * One object per frame. "status" is the decoder's verdict, mapped to the words
 * the dissector uses in its expert info:
 *   ok | unsupported_version | malformed | invalid_node
 * "fields" is present only when the decoder accepted the frame.
 * timestamp_ms is written as a string: it is a full uint64, and JSON tools
 * (jq among them) read numbers as doubles, which cannot hold it exactly. */

static int g_first = 1;

static void json_begin(FILE *j, const char *file, unsigned frame, const char *status)
{
    fprintf(j, "%s\n  {\"file\":\"%s\",\"frame\":%u,\"status\":\"%s\"",
            g_first ? "" : ",", file, frame, status);
    g_first = 0;
}

static const char *status_word(hh_status_t st)
{
    switch (st) {
    case HH_OK:              return "ok";
    case HH_ERR_UNSUPPORTED: return "unsupported_version";
    default:                 return "malformed";
    }
}

static void emit_beacon(FILE *pc, FILE *j, unsigned frame, const uint8_t *buf, size_t len)
{
    hh_beacon_t b;
    hh_status_t st = hh_beacon_decode(buf, len, &b);
    const char *status = status_word(st);

    /* The decoder reports node_id 0 as HH_ERR_INVAL with every other field
     * already filled in; the dissector names that case separately, because
     * "this frame is well-formed but claims the invalid node id" is a
     * different finding from "this frame is cut short". */
    if (st == HH_ERR_INVAL && len >= HH_BEACON_WIRE_LEN && b.node_id == HH_NODE_ID_INVALID)
        status = "invalid_node";

    pcap_write(pc, frame, buf, len);
    json_begin(j, "beacons.pcap", frame, status);
    if (st == HH_OK) {
        fprintf(j, ",\"fields\":{"
                "\"node_id\":%" PRIu32 ",\"protocol_version\":%u,\"sequence_no\":%" PRIu32 ","
                "\"timestamp_ms\":\"%" PRIu64 "\",\"capabilities\":%" PRIu32 ",\"radio_caps\":%" PRIu32 ","
                "\"supported_waveforms\":%" PRIu32 ",\"channel_freq_hz\":%.0f,"
                "\"routing_capable\":%d,\"position_valid\":%d,\"power_valid\":%d,"
                "\"position_x\":%.1f,\"position_y\":%.1f,\"position_z\":%.1f,"
                "\"power_battery\":%.4f}",
                b.node_id, (unsigned)b.protocol_version, b.sequence_no,
                b.timestamp, b.capabilities, b.radio_caps,
                b.supported_waveforms, (double)b.channel_freq,
                b.routing_capable, b.position_valid, b.power_valid,
                (double)b.position_x, (double)b.position_y, (double)b.position_z,
                (double)b.power_battery);
    }
    fputc('}', j);
}

static void emit_route(FILE *pc, FILE *j, unsigned frame, const uint8_t *buf, size_t len)
{
    hh_route_update_t u;
    hh_status_t st = hh_route_update_decode(buf, len, &u);
    const char *status = status_word(st);

    /* Same distinction as for beacons: a complete header naming sender 0. */
    if (st == HH_ERR_INVAL && len >= 5u &&
        (buf[0] | buf[1] | buf[2] | buf[3]) == 0)
        status = "invalid_node";

    pcap_write(pc, frame, buf, len);
    json_begin(j, "routes.pcap", frame, status);
    if (st == HH_OK) {
        fprintf(j, ",\"fields\":{\"sender\":%" PRIu32 ",\"count\":%u,\"entries\":[",
                u.sender, (unsigned)u.count);
        for (uint8_t i = 0; i < u.count; i++) {
            const hh_route_update_entry_t *e = &u.entries[i];
            fprintf(j, "%s{\"originator\":%" PRIu32 ",\"sequence_no\":%" PRIu32 ","
                    "\"hop_count\":%u,\"metric\":%.4f}",
                    i ? "," : "", e->originator, e->sequence_no,
                    (unsigned)e->hop_count, (double)e->metric);
        }
        fputs("]}", j);
    }
    fputc('}', j);
}

static hh_beacon_t base_beacon(void)
{
    hh_beacon_t b;
    memset(&b, 0, sizeof b);
    b.node_id = 0x0000002Au;
    b.protocol_version = HH_PROTOCOL_VERSION;
    b.sequence_no = 1000u;
    b.timestamp = 1790000000123ull;
    b.capabilities = 0x3u;
    b.radio_caps = 0xAABBCCDDu;
    b.supported_waveforms = 0x5u;
    b.channel_freq = 225000000.0f;
    b.routing_capable = true;
    return b;
}

static int write_beacons(const char *dir, FILE *j)
{
    uint8_t buf[HH_BEACON_WIRE_LEN];
    unsigned n = 0;
    hh_beacon_t b;
    FILE *pc = pcap_open(dir, "beacons.pcap", DLT_USER0);
    if (!pc) return 1;

    /* 1: every optional field present, negative coordinates. */
    b = base_beacon();
    b.position_valid = true;
    b.position_x = -123.4f; b.position_y = 56.7f; b.position_z = 8.9f;
    b.power_valid = true;
    b.power_battery = 0.8765f;
    hh_beacon_encode(&b, buf, sizeof buf);
    emit_beacon(pc, j, ++n, buf, sizeof buf);

    /* 2: optional fields absent — flags clear, slots zero. */
    b = base_beacon();
    b.routing_capable = false;
    b.sequence_no = 1001u;
    hh_beacon_encode(&b, buf, sizeof buf);
    emit_beacon(pc, j, ++n, buf, sizeof buf);

    /* 3: every integer at its maximum. */
    b = base_beacon();
    b.node_id = 0xFFFFFFFFu;
    b.sequence_no = 0xFFFFFFFFu;
    b.timestamp = 0xFFFFFFFFFFFFFFFFull;
    b.capabilities = b.radio_caps = b.supported_waveforms = 0xFFFFFFFFu;
    hh_beacon_encode(&b, buf, sizeof buf);
    emit_beacon(pc, j, ++n, buf, sizeof buf);

    /* 4: a future protocol version. Encoded as v1, then the version field
     * (bytes 4..5, little-endian) is overwritten, as a newer sender would. */
    b = base_beacon();
    hh_beacon_encode(&b, buf, sizeof buf);
    buf[4] = 2; buf[5] = 0;
    emit_beacon(pc, j, ++n, buf, sizeof buf);

    /* 5: truncated frame. */
    b = base_beacon();
    hh_beacon_encode(&b, buf, sizeof buf);
    emit_beacon(pc, j, ++n, buf, 20);

    /* 6: well-formed, but claiming the invalid/broadcast node id 0. */
    b = base_beacon();
    b.node_id = HH_NODE_ID_INVALID;
    hh_beacon_encode(&b, buf, sizeof buf);
    emit_beacon(pc, j, ++n, buf, sizeof buf);

    fclose(pc);
    return 0;
}

static int write_routes(const char *dir, FILE *j)
{
    uint8_t buf[5u + HH_ROUTE_UPDATE_MAX_ENTRIES * 11u];
    unsigned n = 0;
    size_t len;
    hh_route_update_t u;
    FILE *pc = pcap_open(dir, "routes.pcap", DLT_USER1);
    if (!pc) return 1;

    /* 1: no entries. */
    memset(&u, 0, sizeof u);
    u.sender = 7u;
    len = hh_route_update_encode(&u, buf, sizeof buf);
    emit_route(pc, j, ++n, buf, len);

    /* 2: one entry. */
    u.count = 1;
    u.entries[0] = (hh_route_update_entry_t){ .originator = 9u, .sequence_no = 55u,
                                              .hop_count = 2, .metric = 0.25f };
    len = hh_route_update_encode(&u, buf, sizeof buf);
    emit_route(pc, j, ++n, buf, len);

    /* 3: the maximum number of entries. */
    u.count = HH_ROUTE_UPDATE_MAX_ENTRIES;
    for (uint8_t i = 0; i < u.count; i++)
        u.entries[i] = (hh_route_update_entry_t){ .originator = 100u + i,
                                                  .sequence_no = 1000u * i,
                                                  .hop_count = (uint8_t)(i % 5),
                                                  .metric = (float)i / 24.0f };
    len = hh_route_update_encode(&u, buf, sizeof buf);
    emit_route(pc, j, ++n, buf, len);

    /* 4: count above the maximum (the encoder refuses it, so patch it in). */
    u.count = 1;
    len = hh_route_update_encode(&u, buf, sizeof buf);
    buf[4] = HH_ROUTE_UPDATE_MAX_ENTRIES + 1;
    emit_route(pc, j, ++n, buf, len);

    /* 5: count says three entries, bytes for two. */
    u.count = 3;
    len = hh_route_update_encode(&u, buf, sizeof buf);
    emit_route(pc, j, ++n, buf, len - 11u);

    /* 6: sender 0. */
    u.sender = HH_NODE_ID_INVALID;
    u.count = 0;
    len = hh_route_update_encode(&u, buf, sizeof buf);
    emit_route(pc, j, ++n, buf, len);

    fclose(pc);
    return 0;
}

int main(int argc, char **argv)
{
    char path[512];
    FILE *j;
    int rc;

    if (argc != 2) {
        fprintf(stderr, "usage: %s OUTDIR\n", argv[0]);
        return 2;
    }
    snprintf(path, sizeof path, "%s/expected.json", argv[1]);
    j = fopen(path, "w");
    if (!j) { perror(path); return 1; }

    fputc('[', j);
    rc = write_beacons(argv[1], j) || write_routes(argv[1], j);
    fputs("\n]\n", j);
    fclose(j);
    return rc;
}
