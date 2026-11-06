/*
 * hh-slot-sim: runs the slot-timed multi-node network (tests/sim/slot_net) and
 * prints a summary. Development tooling only, never linked into a daemon.
 *
 * Routing is the stand-in of slot_net, not OLSRd2, so every routing figure is
 * modelled. The slot capacity default is a placeholder.
 *
 * Captures use DLT_USER2 (149), one PDU v2 per record, no outer header. This
 * is a test-tooling convention, not an air format.
 */
#include "slot_net.h"
#include <stdlib.h>
#include <string.h>

#define DLT_USER2 149u

typedef struct {
    sn_config_t cfg;
    unsigned    gen_s, drain_s;
    const char *topology;
    unsigned    flow_src, flow_dst, size, interval_ms, start_ms;
    int         cut_a, cut_b;
    unsigned    cut_at_ms, reroute_ms;
    const char *log_path, *pcap_path, *summary_path;
} opts_t;

/* Prints the command line options. */
static void usage(void)
{
    fputs("usage: hh-slot-sim [options]\n"
          "  --nodes N            nodes, 2 to 32 (default 4)\n"
          "  --topology T         line | ring | mesh (default line)\n"
          "  --slots N            slots per frame (default 20, must be >= nodes)\n"
          "  --frames N           frames per superframe (default 50)\n"
          "  --slot-ms N          slot length in ms (default 1)\n"
          "  --capacity N         PDU bytes per slot (default 128, a placeholder)\n"
          "  --gen-s N            traffic seconds (default 20)\n"
          "  --drain-s N          drain seconds after traffic stops (default 2)\n"
          "  --flow A,B           one flow from node A to node B (default 1,N)\n"
          "  --size N             application packet bytes (default 40)\n"
          "  --interval-ms N      packet interval (default 100)\n"
          "  --start-ms N         first packet after N ms (default 500)\n"
          "  --loss N             link loss in permille (default 0)\n"
          "  --corrupt N          corrupted PDUs in permille (default 0)\n"
          "  --seed N             random seed (default 1)\n"
          "  --hello MS,BYTES     routing stand-in HELLO (default 2000,40; 0,0 = off)\n"
          "  --tc MS,BYTES        routing stand-in TC (default 5000,40; 0,0 = off)\n"
          "  --cut A,B,AT,DELAY   cut link A-B at AT ms, reroute DELAY ms later\n"
          "  --log FILE           key=value event log\n"
          "  --pcap FILE          capture of every PDU\n"
          "  --summary FILE       write the summary here as well\n", stderr);
}

/* Reads two comma-separated numbers. */
static bool pair(const char *s, unsigned *a, unsigned *b)
{
    return sscanf(s, "%u,%u", a, b) == 2;
}

/* Reads the command line into the options. Returns false on an error. */
static bool parse(int argc, char **argv, opts_t *o)
{
    unsigned a, b, c, d;
    memset(o, 0, sizeof *o);
    sn_config_default(&o->cfg, 4);
    o->gen_s = 20; o->drain_s = 2; o->topology = "line";
    o->size = 40; o->interval_ms = 100; o->start_ms = 500; o->cut_a = -1;
    for (int i = 1; i < argc; i++) {
        const char *k = argv[i];
        const char *v = i + 1 < argc ? argv[i + 1] : NULL;
        if (!strcmp(k, "-h") || !strcmp(k, "--help")) return false;
        if (!v) { fprintf(stderr, "missing value for %s\n", k); return false; }
        i++;
        if (!strcmp(k, "--nodes")) o->cfg.nodes = (uint16_t)atoi(v);
        else if (!strcmp(k, "--topology")) o->topology = v;
        else if (!strcmp(k, "--slots")) o->cfg.slots_per_frame = (uint16_t)atoi(v);
        else if (!strcmp(k, "--frames")) o->cfg.frames_per_superframe = (uint16_t)atoi(v);
        else if (!strcmp(k, "--slot-ms")) o->cfg.slot_ms = (uint16_t)atoi(v);
        else if (!strcmp(k, "--capacity")) o->cfg.capacity = (uint16_t)atoi(v);
        else if (!strcmp(k, "--gen-s")) o->gen_s = (unsigned)atoi(v);
        else if (!strcmp(k, "--drain-s")) o->drain_s = (unsigned)atoi(v);
        else if (!strcmp(k, "--flow")) { if (!pair(v, &o->flow_src, &o->flow_dst)) return false; }
        else if (!strcmp(k, "--size")) o->size = (unsigned)atoi(v);
        else if (!strcmp(k, "--interval-ms")) o->interval_ms = (unsigned)atoi(v);
        else if (!strcmp(k, "--start-ms")) o->start_ms = (unsigned)atoi(v);
        else if (!strcmp(k, "--loss")) o->cfg.loss_permille = (uint32_t)atoi(v);
        else if (!strcmp(k, "--corrupt")) o->cfg.corrupt_permille = (uint32_t)atoi(v);
        else if (!strcmp(k, "--seed")) o->cfg.seed = (uint32_t)atoi(v);
        else if (!strcmp(k, "--hello")) { if (!pair(v, &a, &b)) return false; o->cfg.hello_interval_ms = a; o->cfg.hello_bytes = (uint16_t)b; }
        else if (!strcmp(k, "--tc")) { if (!pair(v, &a, &b)) return false; o->cfg.tc_interval_ms = a; o->cfg.tc_bytes = (uint16_t)b; }
        else if (!strcmp(k, "--cut")) {
            if (sscanf(v, "%u,%u,%u,%u", &a, &b, &c, &d) != 4) return false;
            o->cut_a = (int)a; o->cut_b = (int)b; o->cut_at_ms = c; o->reroute_ms = d;
        }
        else if (!strcmp(k, "--log")) o->log_path = v;
        else if (!strcmp(k, "--pcap")) o->pcap_path = v;
        else if (!strcmp(k, "--summary")) o->summary_path = v;
        else { fprintf(stderr, "unknown option %s\n", k); return false; }
    }
    if (!o->flow_src) { o->flow_src = 1; o->flow_dst = o->cfg.nodes; }
    return true;
}

/* Writes the pcap file header for link type 149. */
static void pcap_header(FILE *f)
{
    uint32_t h[6] = { 0xA1B2C3D4u, 0x00040002u, 0, 0, 65535u, DLT_USER2 };
    uint16_t ver[2] = { 2, 4 };
    fwrite(&h[0], 4, 1, f);
    fwrite(ver, 2, 2, f);
    fwrite(&h[2], 4, 4, f);   /* thiszone, sigfigs, snaplen, network */
}

/* Writes one PDU to the capture file. */
static void pcap_hook(void *ctx, uint32_t t_ms, const uint8_t *pdu, size_t len)
{
    FILE *f = ctx;
    uint32_t r[4] = { t_ms / 1000u, (t_ms % 1000u) * 1000u, (uint32_t)len, (uint32_t)len };
    fwrite(r, 4, 4, f);
    fwrite(pdu, 1, len, f);
}

/* Prints a percentage line. */
static void pct(FILE *o, const char *name, uint64_t num, uint64_t den)
{
    fprintf(o, "%s=%.2f%%\n", name, den ? 100.0 * (double)num / (double)den : 0.0);
}

/* Prints the throughput, loss and delay lines of one flow. */
static void print_flow(FILE *o, const char *name, const sn_flow_result_t *r, uint32_t gen_ms)
{
    fprintf(o, "%s sent=%u delivered=%u delivered_bytes=%llu throughput_Bps=%.1f hops_max=%u\n", name,
            r->sent, r->delivered, (unsigned long long)r->delivered_bytes,
            gen_ms ? (double)r->delivered_bytes * 1000.0 / gen_ms : 0.0, r->hops_max);
    fprintf(o, "%s loss queue_drop=%u scheduler_rejection=%u collision=%u channel=%u corruption=%u deadline=%u\n",
            name, r->queue_drop, r->sched_reject, r->collision, r->channel, r->corruption, r->deadline);
    fprintf(o, "%s mac_delay_ms min=%u median=%u p95=%u max=%u\n", name, r->mac_delay_ms[0],
            r->mac_delay_ms[1], r->mac_delay_ms[2], r->mac_delay_ms[3]);
    fprintf(o, "%s app_delay_ms min=%u median=%u p95=%u max=%u\n", name, r->app_delay_ms[0],
            r->app_delay_ms[1], r->app_delay_ms[2], r->app_delay_ms[3]);
}

/* Prints the run parameters, traffic results, slot counts and slot accounting. */
static void print_summary(FILE *o, const opts_t *op, slot_net_t *net, const sn_result_t *r)
{
    const hh_slot_stats_t *s = &r->slot;
    hh_mac_status_t st;
    unsigned max_payload = 0;
    unsigned frame_ms = op->cfg.slots_per_frame * op->cfg.slot_ms;

    if (hh_mac_status(sn_mac(net, 1), &st) == HH_MAC_OK) max_payload = st.max_payload;
    fprintf(o, "== parameters ==\n");
    fprintf(o, "nodes=%u topology=%s slots_per_frame=%u frames_per_superframe=%u slot_ms=%u frame_ms=%u\n",
            op->cfg.nodes, op->topology, op->cfg.slots_per_frame, op->cfg.frames_per_superframe,
            op->cfg.slot_ms, frame_ms);
    fprintf(o, "capacity_bytes=%u capacity_is_placeholder=yes max_packet_bytes=%u fragmentation=excluded\n",
            op->cfg.capacity, max_payload);
    fprintf(o, "routing=stand-in (not OLSRd2, results modelled) hello_ms=%u hello_bytes=%u tc_ms=%u tc_bytes=%u\n",
            op->cfg.hello_interval_ms, op->cfg.hello_bytes, op->cfg.tc_interval_ms, op->cfg.tc_bytes);
    fprintf(o, "flow=%u->%u size=%u interval_ms=%u start_ms=%u gen_ms=%u drain_ms=%u\n", op->flow_src,
            op->flow_dst, op->size, op->interval_ms, op->start_ms, r->gen_ms, r->run_ms - r->gen_ms);
    fprintf(o, "link_loss_permille=%u corrupt_permille=%u seed=%u\n", op->cfg.loss_permille,
            op->cfg.corrupt_permille, op->cfg.seed);
    fprintf(o, "== traffic ==\n");
    print_flow(o, "total", &r->total, r->gen_ms);
    fprintf(o, "== slots ==\n");
    fprintf(o, "pdus=%llu own_slots=%llu own_slots_missed=%llu tx_outside_own_slot=%llu empty_pdus=%llu\n",
            (unsigned long long)r->pdus, (unsigned long long)s->own_slots,
            (unsigned long long)s->own_slots_missed, (unsigned long long)r->tx_outside_own_slot,
            (unsigned long long)s->pdus_empty);
    fprintf(o, "collision_events=%llu channel_events=%llu corrupt_events=%llu route_changes=%llu\n",
            (unsigned long long)r->collision_events, (unsigned long long)r->channel_events,
            (unsigned long long)r->corrupt_events, (unsigned long long)r->route_changes);
    fprintf(o, "== slot accounting (whole run, warm-up included) ==\n");
    fprintf(o, "capacity_bytes_total=%llu pdu_bytes=%llu shared_overhead_bytes=%llu control_bytes=%llu "
               "routing_bytes=%llu user_bytes=%llu\n",
            (unsigned long long)s->capacity_bytes, (unsigned long long)s->pdu_bytes,
            (unsigned long long)s->overhead_bytes, (unsigned long long)s->control_bytes,
            (unsigned long long)s->routing_bytes, (unsigned long long)s->user_bytes);
    pct(o, "routing_payload_share_of_capacity", s->routing_bytes, s->capacity_bytes);
    pct(o, "routing_transmitted_share_of_pdu_bytes", s->routing_bytes, s->pdu_bytes);
    pct(o, "control_payload_share_of_capacity", s->control_bytes, s->capacity_bytes);
    pct(o, "shared_overhead_share_of_capacity", s->overhead_bytes, s->capacity_bytes);
    pct(o, "unused_capacity", s->capacity_bytes - s->pdu_bytes, s->capacity_bytes);
    fprintf(o, "own_slots_with_routing=%llu of %llu\n", (unsigned long long)s->slots_with_routing,
            (unsigned long long)s->pdus_tx);
    pct(o, "routing_slot_occupancy", s->slots_with_routing, s->pdus_tx);
    if (r->n_flows > 1) for (unsigned i = 0; i < r->n_flows; i++) {
        char nm[16];
        snprintf(nm, sizeof nm, "flow%u", i);
        print_flow(o, nm, &r->flow[i], r->gen_ms);
    }
}

/* Parses the options, runs the simulation and prints the summary. */
int main(int argc, char **argv)
{
    opts_t op;
    slot_net_t *net;
    sn_result_t res;
    sn_flow_t flow;
    FILE *pcap = NULL, *summary = NULL;

    if (!parse(argc, argv, &op)) { usage(); return 2; }
    if (op.log_path && !(op.cfg.log = fopen(op.log_path, "w"))) { perror(op.log_path); return 1; }
    if (op.pcap_path) {
        if (!(pcap = fopen(op.pcap_path, "wb"))) { perror(op.pcap_path); return 1; }
        pcap_header(pcap);
        op.cfg.pdu_hook = pcap_hook;
        op.cfg.hook_ctx = pcap;
    }
    net = sn_create(&op.cfg);
    if (!net) {
        fputs("invalid configuration: nodes 2 to 32, nodes <= slots per frame <= 64, capacity large enough for the PDU overhead\n", stderr);
        return 2;
    }
    if (!strcmp(op.topology, "line")) sn_topology_line(net);
    else if (!strcmp(op.topology, "ring")) sn_topology_ring(net);
    else if (!strcmp(op.topology, "mesh")) sn_topology_mesh(net);
    else { fprintf(stderr, "unknown topology %s\n", op.topology); return 2; }

    flow = (sn_flow_t){ (uint8_t)op.flow_src, (uint8_t)op.flow_dst, (uint16_t)op.size, op.interval_ms, op.start_ms };
    if (!sn_add_flow(net, &flow)) { fputs("invalid flow\n", stderr); return 2; }
    if (op.cut_a >= 0) {
        sn_schedule(net, op.cut_at_ms, SN_EV_LINK_DOWN, (unsigned)op.cut_a, (unsigned)op.cut_b);
        sn_schedule(net, op.cut_at_ms + op.reroute_ms, SN_EV_REROUTE, 0, 0);
    }

    sn_run(net, op.gen_s * 1000u, op.drain_s * 1000u);
    sn_result(net, &res);
    print_summary(stdout, &op, net, &res);
    if (op.summary_path && (summary = fopen(op.summary_path, "w"))) {
        print_summary(summary, &op, net, &res);
        fclose(summary);
    }
    sn_destroy(net);
    if (op.cfg.log) fclose(op.cfg.log);
    if (pcap) fclose(pcap);
    return 0;
}
