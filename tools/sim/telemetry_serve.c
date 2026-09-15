/*
 * hh-manet-telemetry-serve — long-running netsim driver for the MA-OI GUI
 * integration (docs/GUI-INTEGRATION-PLAN.md).
 *
 * DEVELOPMENT/TEST TOOLING ONLY. Unlike hh-manet-sim (which runs one
 * scenario to completion and exits), this keeps a netsim_t alive
 * indefinitely, stepping it forever and publishing telemetry to any
 * connected TCP client after every step. No hardware required — every
 * node is the real production stack over a mock radio, same as
 * hh-manet-sim.
 *
 * Nodes join one at a time, `--join-interval-ms` apart, and each new node
 * links to every node that already joined (a full mesh, not a chain) —
 * this is so a GUI watching the telemetry stream can visibly show nodes
 * appearing over time rather than all at once, while still giving routing
 * and self-healing more than one path to exploit.
 *
 * Never linked into hh-manet, and shares no code path with it beyond the
 * production library both link.
 */
#include "netsim.h"
#include "simui.h"
#include "telemetry_server.h"
#include "ts_log.h"
#include "hhsdr/manet/routing.h"

#include <errno.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static volatile sig_atomic_t g_stop = 0;
static void on_signal(int sig) { (void)sig; g_stop = 1; }

#define PENDING_MAX 32
#define DELIVERY_TIMEOUT_MS 5000

/* One in-flight application message: hh_node_send() only confirms the
 * local node accepted it for sending, not that it arrived. Real arrival
 * is observed by watching the destination's forwarder.delivered_local
 * counter for an increase — the same counter telemetry already exports as
 * packets_delivered_local — with a timeout if it never moves. */
typedef struct {
    bool         used;
    char         msg_id[TS_MSG_ID_LEN];
    hh_node_id_t dst;
    uint64_t     delivered_baseline;
    hh_time_ms_t sent_at;
    hh_time_ms_t deadline;
} pending_msg_t;

static void send_json_line(hh_telemetry_server_t *ts, const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    int w;

    va_start(ap, fmt);
    w = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (w < 0) return;
    if ((size_t)w >= sizeof buf) w = (int)sizeof buf - 1;
    telemetry_server_broadcast(ts, buf, (size_t)w);
}

static const char *frame_kind_str(hh_frame_kind_t k)
{
    switch (k) {
    case HH_FRAME_BEACON:  return "beacon";
    case HH_FRAME_DATA:    return "data";
    case HH_FRAME_ROUTING: return "routing";
    default:               return "unknown";
    }
}

/* Frame observer (netsim.h): fires for every frame the virtual medium
 * actually moves or drops, one hop at a time — the same hook simui.c's
 * --trace uses. `from`/`to` are this hop's endpoints; f->src/f->dst are
 * the frame's original wire src/dst, which differ from from/to once a
 * frame is being forwarded through an intermediate node. Data-plane
 * frames only, to keep the log readable — beacon/routing control traffic
 * is already visible via the route-change log and telemetry counters. */
static void on_frame(hh_node_id_t from, hh_node_id_t to, const hh_frame_t *f,
                      bool delivered, hh_time_ms_t now, void *ctx)
{
    (void)ctx;
    if (f->kind != HH_FRAME_DATA) return;
    hh_ts_log("telemetry_serve: t=%llums hop %u -> %u [%s] wire_src=%u wire_dst=%u "
              "len=%u %s\n",
              (unsigned long long)now, from, to, frame_kind_str(f->kind),
              f->src, f->dst, f->len, delivered ? "delivered" : "DROPPED");
}

typedef struct {
    uint16_t port;
    unsigned nodes;
    uint64_t seed;
    uint32_t step_ms;
    uint32_t join_interval_ms;
    const char *log_file;
} opts_t;

static void usage(const char *argv0)
{
    fprintf(stderr,
        "usage: %s [--port N] [--nodes N] [--seed N] [--step-ms N] [--join-interval-ms N] [--log-file PATH]\n"
        "  --port              TCP port to serve telemetry on (default 5566)\n"
        "  --nodes             number of simulated nodes, full mesh (default 4, max 8)\n"
        "  --seed              netsim PRNG seed (default 1)\n"
        "  --step-ms           virtual time advanced per tick (default 100)\n"
        "  --join-interval-ms  real time between each node joining (default 3000)\n"
        "  --log-file PATH     append every log line to PATH too (stderr always gets them)\n"
        "Nodes join one at a time so a viewer can see the network form.\n"
        "Runs until interrupted (Ctrl-C / SIGTERM).\n", argv0);
}

/* Per-node route-table counters snapshotted each tick, so a change (an
 * install/withdrawal/replacement) can be logged as an event instead of
 * dumping the whole table every tick. */
typedef struct {
    hh_node_id_t id;
    uint64_t installs, withdrawals, replacements, rejected_stale;
    bool seen;
} route_watch_t;

static void log_route_changes(route_watch_t *watch, size_t watch_count, const netsim_t *s)
{
    for (size_t i = 0; i < s->node_count; i++) {
        const sim_node_t *sn = &s->nodes[i];
        const hh_routing_t *r;
        route_watch_t *w = NULL;

        if (!sn->used || !sn->alive) continue;
        r = &sn->node.routing;

        for (size_t j = 0; j < watch_count; j++) {
            if (watch[j].id == sn->id) { w = &watch[j]; break; }
        }
        if (!w) continue; /* watch array is sized to SIM_MAX_NODES, always found */

        if (!w->seen) {
            w->seen = true;
            w->installs = r->installs;
            w->withdrawals = r->withdrawals;
            w->replacements = r->replacements;
            w->rejected_stale = r->rejected_stale;
            continue; /* first sighting: baseline, nothing to report yet */
        }

        if (r->installs != w->installs || r->withdrawals != w->withdrawals ||
            r->replacements != w->replacements) {
            const hh_route_snapshot_t *snap = hh_routing_snapshot(r);

            hh_ts_log("telemetry_serve: node %u routing changed — "
                      "installs=%llu(+%llu) withdrawals=%llu(+%llu) "
                      "replacements=%llu(+%llu) route_count=%zu\n",
                      sn->id,
                      (unsigned long long)r->installs,
                      (unsigned long long)(r->installs - w->installs),
                      (unsigned long long)r->withdrawals,
                      (unsigned long long)(r->withdrawals - w->withdrawals),
                      (unsigned long long)r->replacements,
                      (unsigned long long)(r->replacements - w->replacements),
                      hh_routing_count(r));

            /* Same data telemetry already exports as JSON (see
             * format_node_json in telemetry_server.c) — printed here too,
             * as an aligned table, at the moment it changes, so watching
             * this one log file is enough without a second process
             * tailing the socket. */
            if (snap && snap->count > 0) {
                hh_ts_log("  node %u route table (%zu entries):\n", sn->id, snap->count);
                hh_ts_log("    %-6s %-6s %-6s %-8s %-8s %-6s\n",
                          "dst", "via", "hops", "metric", "seq", "state");
                for (size_t k = 0; k < snap->count; k++) {
                    const hh_route_entry_t *e = &snap->entries[k];
                    hh_ts_log("    %-6u %-6u %-6u %-8.2f %-8u %-6s\n",
                              e->destination, e->next_hop, e->hop_count,
                              (double)e->metric, e->sequence_no,
                              e->valid ? "OK" : "INVALID");
                }
            }

            w->installs = r->installs;
            w->withdrawals = r->withdrawals;
            w->replacements = r->replacements;
            w->rejected_stale = r->rejected_stale;
        }
    }
}

int main(int argc, char **argv)
{
    opts_t o;
    netsim_t s;
    hh_telemetry_server_t *ts;
    unsigned n;
    unsigned joined = 0;
    hh_time_ms_t next_join_at;
    uint64_t tick = 0;
    time_t last_summary = 0;
    route_watch_t route_watch[SIM_MAX_NODES];
    pending_msg_t pending[PENDING_MAX];
    ts_send_cmd_t cmds[8];

    memset(&o, 0, sizeof o);
    o.port = 5566;
    o.nodes = 4;
    o.seed = 1;
    o.step_ms = 100;
    o.join_interval_ms = 3000;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--port") && i + 1 < argc)     o.port = (uint16_t)strtoul(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--nodes") && i + 1 < argc) o.nodes = (unsigned)strtoul(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--seed") && i + 1 < argc)  o.seed = strtoull(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--step-ms") && i + 1 < argc) o.step_ms = (uint32_t)strtoul(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--join-interval-ms") && i + 1 < argc)
            o.join_interval_ms = (uint32_t)strtoul(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--log-file") && i + 1 < argc) o.log_file = argv[++i];
        else if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) { usage(argv[0]); return 0; }
        else { hh_ts_log("unknown option: %s\n\n", argv[i]); usage(argv[0]); return 2; }
    }

    n = o.nodes;
    if (n < 2) n = 2;
    if (n > 8) n = 8;

    if (o.log_file) {
        if (!hh_ts_log_open(o.log_file)) {
            fprintf(stderr, "telemetry_serve: could not open log file %s: %s\n",
                    o.log_file, strerror(errno));
            return 1;
        }
        hh_ts_log("telemetry_serve: logging to %s\n", o.log_file);
    }

    memset(route_watch, 0, sizeof route_watch);
    for (size_t i = 0; i < SIM_MAX_NODES; i++)
        route_watch[i].id = simui_id_from_index(i);
    memset(pending, 0, sizeof pending);

    hh_log_set_level(HH_LOG_ERROR);

    ts = telemetry_server_start(o.port);
    if (!ts) return 1;

    netsim_init(&s, o.seed);
    netsim_set_frame_observer(&s, on_frame, NULL);

    hh_ts_log("hh-manet-telemetry-serve: %u nodes (joining %ums apart), seed=%llu, "
                    "step=%ums, port=%u — Ctrl-C to stop\n",
            n, (unsigned)o.join_interval_ms, (unsigned long long)o.seed, o.step_ms, (unsigned)o.port);

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    /* First node joins immediately; each subsequent node joins
     * join_interval_ms of real time later and links to the previous one. */
    next_join_at = 0;

    /* netsim's clock is virtual (see tests/sim/netsim.c) and unrelated to
     * wall-clock time, so an unpaced loop would spin a full CPU core and
     * publish far faster than any viewer needs. Pace one step per
     * step_ms of real time instead. */
    while (!g_stop) {
        struct timespec ts_sleep;
        time_t now = time(NULL);

        telemetry_server_accept(ts);

        {
            size_t ncmds = telemetry_server_poll_commands(ts, cmds, 8);
            for (size_t i = 0; i < ncmds; i++) {
                ts_send_cmd_t *c = &cmds[i];
                sim_node_t *src = netsim_node(&s, c->from);
                sim_node_t *dst_node = netsim_node(&s, c->to);
                hh_status_t st;
                size_t slot;

                if (!src || !dst_node) {
                    send_json_line(ts,
                        "{\"type\":\"send_ack\",\"msg_id\":\"%s\",\"status\":\"rejected\","
                        "\"reason\":\"unknown node\"}\n", c->msg_id);
                    hh_ts_log("telemetry_serve: send %s rejected — unknown node (from=%u to=%u)\n",
                              c->msg_id, c->from, c->to);
                    continue;
                }

                st = hh_node_send(&src->node, c->to, (const uint8_t *)c->payload,
                                   c->payload_len, s.vc.now);
                if (st != HH_OK) {
                    send_json_line(ts,
                        "{\"type\":\"send_ack\",\"msg_id\":\"%s\",\"status\":\"rejected\","
                        "\"reason\":\"hh_node_send status %d\"}\n", c->msg_id, (int)st);
                    hh_ts_log("telemetry_serve: send %s rejected — hh_node_send status %d "
                              "(from=%u to=%u)\n", c->msg_id, (int)st, c->from, c->to);
                    continue;
                }

                send_json_line(ts,
                    "{\"type\":\"send_ack\",\"msg_id\":\"%s\",\"status\":\"accepted\"}\n",
                    c->msg_id);
                hh_ts_log("telemetry_serve: send %s accepted (from=%u to=%u, %u bytes)\n",
                          c->msg_id, c->from, c->to, c->payload_len);

                slot = PENDING_MAX;
                for (size_t j = 0; j < PENDING_MAX; j++) {
                    if (!pending[j].used) { slot = j; break; }
                }
                if (slot == PENDING_MAX) {
                    /* Table full: we already sent "accepted", so report the
                     * outcome as failed rather than silently never resolving. */
                    send_json_line(ts,
                        "{\"type\":\"send_result\",\"msg_id\":\"%s\",\"status\":\"failed\","
                        "\"reason\":\"pending table full\"}\n", c->msg_id);
                    continue;
                }
                pending[slot].used = true;
                strncpy(pending[slot].msg_id, c->msg_id, TS_MSG_ID_LEN - 1);
                pending[slot].msg_id[TS_MSG_ID_LEN - 1] = '\0';
                pending[slot].dst = c->to;
                pending[slot].delivered_baseline = dst_node->node.forwarder.delivered_local;
                pending[slot].sent_at = s.vc.now;
                pending[slot].deadline = s.vc.now + DELIVERY_TIMEOUT_MS;
            }
        }

        if (joined < n && s.vc.now >= next_join_at) {
            hh_node_id_t id = simui_id_from_index(joined);
            hh_status_t st;

            netsim_add_node(&s, id);
            st = hh_node_configure(&netsim_node(&s, id)->node, NULL);
            if (st == HH_OK) st = hh_node_start(&netsim_node(&s, id)->node);
            if (st != HH_OK) {
                hh_ts_log("telemetry_serve: node %u failed to start (status %d)\n",
                        id, (int)st);
            } else if (joined == 0) {
                hh_ts_log("telemetry_serve: node %u joined (%u/%u)\n", id, joined + 1, n);
            } else {
                /* Full mesh: link the new node to every node that already
                 * joined, not just the previous one — otherwise this is a
                 * chain (1-2-3-4) and there is nothing for routing/
                 * self-healing to exploit beyond a single path per hop. */
                char linked[64] = "";
                size_t linked_len = 0;
                for (unsigned j = 0; j < joined; j++) {
                    hh_node_id_t other = simui_id_from_index(j);
                    int w;
                    netsim_link_up(&s, other, id, -55.0f);
                    w = snprintf(linked + linked_len, sizeof linked - linked_len,
                                 "%s%u", j == 0 ? "" : ",", other);
                    if (w > 0 && (size_t)w < sizeof linked - linked_len) linked_len += (size_t)w;
                }
                hh_ts_log("telemetry_serve: node %u joined, linked to node(s) [%s] (%u/%u)\n",
                        id, linked, joined + 1, n);
            }
            joined++;
            next_join_at = s.vc.now + o.join_interval_ms;
        }

        netsim_step(&s, o.step_ms);
        telemetry_server_publish(ts, &s);
        log_route_changes(route_watch, SIM_MAX_NODES, &s);
        tick++;

        for (size_t i = 0; i < PENDING_MAX; i++) {
            pending_msg_t *p = &pending[i];
            sim_node_t *dst_node;
            uint64_t now_delivered;

            if (!p->used) continue;
            dst_node = netsim_node(&s, p->dst);
            now_delivered = dst_node ? dst_node->node.forwarder.delivered_local : 0;

            if (dst_node && now_delivered > p->delivered_baseline) {
                send_json_line(ts,
                    "{\"type\":\"send_result\",\"msg_id\":\"%s\",\"status\":\"delivered\","
                    "\"latency_ms\":%llu}\n",
                    p->msg_id, (unsigned long long)(s.vc.now - p->sent_at));
                hh_ts_log("telemetry_serve: send %s delivered (latency %llums)\n",
                          p->msg_id, (unsigned long long)(s.vc.now - p->sent_at));
                p->used = false;
            } else if (s.vc.now >= p->deadline) {
                send_json_line(ts,
                    "{\"type\":\"send_result\",\"msg_id\":\"%s\",\"status\":\"failed\","
                    "\"reason\":\"delivery timeout\"}\n", p->msg_id);
                hh_ts_log("telemetry_serve: send %s failed — delivery timeout (%ums)\n",
                          p->msg_id, DELIVERY_TIMEOUT_MS);
                p->used = false;
            }
        }

        /* One summary line per second of real time — visibility into a
         * long-running process without a line-per-tick flood. */
        if (now != last_summary) {
            // hh_ts_log("telemetry_serve: t=%llums tick=%llu nodes=%u/%u clients=%zu\n",
            //         (unsigned long long)s.vc.now, (unsigned long long)tick,
            //         joined, n, telemetry_server_client_count(ts));
            last_summary = now;
        }

        ts_sleep.tv_sec = o.step_ms / 1000;
        ts_sleep.tv_nsec = (long)(o.step_ms % 1000) * 1000000L;
        nanosleep(&ts_sleep, NULL);
    }

    hh_ts_log("\nhh-manet-telemetry-serve: shutting down\n");
    telemetry_server_stop(ts);
    hh_ts_log_close();
    return 0;
}
