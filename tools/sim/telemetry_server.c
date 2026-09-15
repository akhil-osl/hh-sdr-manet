#include "telemetry_server.h"
#include "ts_log.h"
#include "hhsdr/manet/telemetry.h"
#include "hhsdr/manet/neighbor.h"
#include "hhsdr/manet/link_health.h"
#include "hhsdr/manet/routing.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define TS_MAX_CLIENTS 8
#define TS_LINE_CAP    4096
#define TS_RECV_BUF_CAP 1024

struct hh_telemetry_server {
    int listen_fd;
    int client_fds[TS_MAX_CLIENTS];
    /* Partial-line buffer per client for inbound commands — a command line
     * can arrive split across TCP segments, same reassembly concern the
     * Python side already handles for its half of this protocol. */
    char recv_buf[TS_MAX_CLIENTS][TS_RECV_BUF_CAP];
    size_t recv_len[TS_MAX_CLIENTS];
    size_t client_count;
    bool in_use;
};

/* No file in this codebase uses heap allocation (see
 * docs/DEPLOYMENT-ARCHITECTURE.md); this tool follows the same convention
 * even though it is dev/test-only and the rule doesn't strictly bind it.
 * One server instance is all any caller needs. */
static hh_telemetry_server_t g_server;

static void set_nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags >= 0) fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

hh_telemetry_server_t *telemetry_server_start(uint16_t port)
{
    hh_telemetry_server_t *ts;
    struct sockaddr_in addr;
    int yes = 1;
    int fd;

    if (g_server.in_use) {
        hh_ts_log("telemetry_server: already running (one instance per process)\n");
        return NULL;
    }

    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        hh_ts_log("telemetry_server: socket() failed: %s\n", strerror(errno));
        return NULL;
    }
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes);

    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = inet_addr("127.0.0.1"); /* localhost only, see header note */
    addr.sin_port = htons(port);

    if (bind(fd, (struct sockaddr *)&addr, sizeof addr) != 0) {
        hh_ts_log("telemetry_server: bind(127.0.0.1:%u) failed: %s\n",
                (unsigned)port, strerror(errno));
        close(fd);
        return NULL;
    }
    if (listen(fd, TS_MAX_CLIENTS) != 0) {
        hh_ts_log("telemetry_server: listen() failed: %s\n", strerror(errno));
        close(fd);
        return NULL;
    }
    set_nonblocking(fd);

    ts = &g_server;
    memset(ts, 0, sizeof *ts);
    ts->listen_fd = fd;
    ts->in_use = true;
    for (size_t i = 0; i < TS_MAX_CLIENTS; i++) ts->client_fds[i] = -1;

    hh_ts_log("telemetry_server: listening on 127.0.0.1:%u (plaintext — "
                    "see docs/GUI-INTEGRATION-PLAN.md 5a-1)\n", (unsigned)port);
    return ts;
}

void telemetry_server_accept(hh_telemetry_server_t *ts)
{
    if (!ts) return;

    for (;;) {
        struct sockaddr_in peer;
        socklen_t peer_len = sizeof peer;
        char peer_str[INET_ADDRSTRLEN] = "?";
        int fd = accept(ts->listen_fd, (struct sockaddr *)&peer, &peer_len);
        if (fd < 0) break; /* EAGAIN/EWOULDBLOCK: nothing pending */

        inet_ntop(AF_INET, &peer.sin_addr, peer_str, sizeof peer_str);
        set_nonblocking(fd);
        {
            int nodelay = 1;
            setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof nodelay);
        }

        if (ts->client_count >= TS_MAX_CLIENTS) {
            hh_ts_log("telemetry_server: rejected %s:%u — at capacity (%d clients)\n",
                    peer_str, (unsigned)ntohs(peer.sin_port), TS_MAX_CLIENTS);
            close(fd); /* full: reject rather than grow unbounded */
            continue;
        }
        for (size_t i = 0; i < TS_MAX_CLIENTS; i++) {
            if (ts->client_fds[i] == -1) {
                ts->client_fds[i] = fd;
                ts->client_count++;
                hh_ts_log("telemetry_server: client connected from %s:%u (%zu/%d)\n",
                        peer_str, (unsigned)ntohs(peer.sin_port), ts->client_count, TS_MAX_CLIENTS);
                break;
            }
        }
    }
}

static void drop_client(hh_telemetry_server_t *ts, size_t idx, const char *reason)
{
    close(ts->client_fds[idx]);
    ts->client_fds[idx] = -1;
    ts->client_count--;
    hh_ts_log("telemetry_server: client disconnected (%s) (%zu/%d remaining)\n",
            reason, ts->client_count, TS_MAX_CLIENTS);
}

void telemetry_server_broadcast(hh_telemetry_server_t *ts, const char *line, size_t len)
{
    if (!ts) return;
    for (size_t i = 0; i < TS_MAX_CLIENTS; i++) {
        ssize_t w;
        if (ts->client_fds[i] == -1) continue;
        w = send(ts->client_fds[i], line, len, MSG_NOSIGNAL);
        /* A slow client that can't absorb a line right now, or one that has
         * gone away, is dropped rather than allowed to block the publisher —
         * this must never become back-pressure on the simulation loop. */
        if (w < 0 && errno != EAGAIN && errno != EWOULDBLOCK)
            drop_client(ts, i, strerror(errno));
        else if (w == 0)
            drop_client(ts, i, "peer closed");
    }
}

/* Hand-rolled extraction for the one inbound shape this protocol accepts:
 * {"cmd":"send","msg_id":"...","from":N,"to":N,"payload":"..."}
 * No JSON library exists in this codebase (see the hh_telemetry_format_*
 * functions this mirrors); a strict parser for one flat, known schema is
 * simpler and has a smaller failure surface than pulling one in. Unknown
 * or malformed lines are rejected, not partially accepted. */
static bool extract_str_field(const char *line, const char *key, char *out, size_t out_cap)
{
    char pat[32];
    const char *p;
    const char *start, *end;
    size_t len;

    /* Tolerate optional whitespace after ':' — e.g. Python's json.dumps()
     * emits {"key": "value"} with a space, not {"key":"value"}. */
    snprintf(pat, sizeof pat, "\"%s\":", key);
    p = strstr(line, pat);
    if (!p) return false;
    p += strlen(pat);
    while (*p == ' ') p++;
    if (*p != '"') return false;
    start = p + 1;
    end = strchr(start, '"');
    if (!end) return false;
    len = (size_t)(end - start);
    if (len >= out_cap) len = out_cap - 1;
    memcpy(out, start, len);
    out[len] = '\0';
    return true;
}

static bool extract_uint_field(const char *line, const char *key, unsigned long *out)
{
    char pat[32];
    const char *p;
    char *endp;

    snprintf(pat, sizeof pat, "\"%s\":", key);
    p = strstr(line, pat);
    if (!p) return false;
    p += strlen(pat);
    while (*p == ' ') p++;
    *out = strtoul(p, &endp, 10);
    return endp != p;
}

static bool parse_send_cmd(const char *line, ts_send_cmd_t *out)
{
    char kind[16];
    unsigned long from, to;

    memset(out, 0, sizeof *out);
    if (!extract_str_field(line, "cmd", kind, sizeof kind)) return false;
    if (strcmp(kind, "send") != 0) return false;
    if (!extract_str_field(line, "msg_id", out->msg_id, sizeof out->msg_id)) return false;
    if (!extract_uint_field(line, "from", &from)) return false;
    if (!extract_uint_field(line, "to", &to)) return false;
    if (!extract_str_field(line, "payload", out->payload, sizeof out->payload)) return false;

    out->from = (hh_node_id_t)from;
    out->to = (hh_node_id_t)to;
    out->payload_len = (uint16_t)strlen(out->payload);
    out->valid = true;
    return true;
}

size_t telemetry_server_poll_commands(hh_telemetry_server_t *ts,
                                       ts_send_cmd_t *out, size_t max_cmds)
{
    size_t found = 0;

    if (!ts) return 0;

    for (size_t i = 0; i < TS_MAX_CLIENTS && found < max_cmds; i++) {
        char chunk[512];
        ssize_t n;

        if (ts->client_fds[i] == -1) continue;

        n = recv(ts->client_fds[i], chunk, sizeof chunk, 0);
        if (n < 0) {
            if (errno != EAGAIN && errno != EWOULDBLOCK)
                drop_client(ts, i, strerror(errno));
            continue;
        }
        if (n == 0) { drop_client(ts, i, "peer closed"); continue; }

        if (ts->recv_len[i] + (size_t)n >= TS_RECV_BUF_CAP) {
            /* Command line too long or client never sends '\n': drop the
             * buffer rather than let one bad client leak memory-equivalent
             * state forever. */
            hh_ts_log("telemetry_server: client %zu command buffer overflow, resetting\n", i);
            ts->recv_len[i] = 0;
            continue;
        }
        memcpy(ts->recv_buf[i] + ts->recv_len[i], chunk, (size_t)n);
        ts->recv_len[i] += (size_t)n;

        for (;;) {
            char *nl = memchr(ts->recv_buf[i], '\n', ts->recv_len[i]);
            size_t line_len;
            char line[TS_RECV_BUF_CAP];

            if (!nl) break;
            line_len = (size_t)(nl - ts->recv_buf[i]);
            if (line_len >= sizeof line) line_len = sizeof line - 1;
            memcpy(line, ts->recv_buf[i], line_len);
            line[line_len] = '\0';

            memmove(ts->recv_buf[i], nl + 1, ts->recv_len[i] - line_len - 1);
            ts->recv_len[i] -= line_len + 1;

            if (found < max_cmds) {
                if (parse_send_cmd(line, &out[found])) {
                    found++;
                } else if (line_len > 0) {
                    hh_ts_log("telemetry_server: discarding malformed command: %s\n", line);
                }
            }
        }
    }
    return found;
}

/* One JSON object for a single simulated node's telemetry + its immediate
 * neighbor/link-state list. Field names mirror hh_node_status_t directly —
 * this is an interim schema (see telemetry.h), not a specification contract. */
static size_t format_node_json(const sim_node_t *sn, char *buf, size_t cap)
{
    hh_node_status_t st;
    size_t used = 0;
    int w;
    hh_node_id_t nbr_ids[HH_MAX_NEIGHBORS];
    size_t nbr_count;

    hh_telemetry_node_status(&sn->node, &st);

    w = snprintf(buf + used, cap - used,
        "{\"schema_version\":\"1.0\",\"node_id\":%u,\"state\":\"%s\","
        "\"neighbor_count\":%u,\"route_count\":%u,\"topology_nodes\":%u,"
        "\"reachable_nodes\":%u,\"partitioned\":%s,\"beacon_interval_ms\":%u,"
        "\"beacons_sent\":%llu,\"beacons_rx_accepted\":%llu,"
        "\"neighbor_ups\":%llu,\"neighbor_downs\":%llu,"
        "\"link_transitions\":%llu,\"failures_confirmed\":%llu,"
        "\"recoveries_started\":%llu,\"recoveries_completed\":%llu,"
        "\"routes_installed\":%llu,\"routes_withdrawn\":%llu,"
        "\"packets_forwarded\":%llu,\"packets_delivered_local\":%llu,"
        "\"packets_dropped_no_route\":%llu,\"events_dropped\":%llu,"
        "\"radio_available\":%s,\"radio_operational\":%s,\"radio_channel\":%u,"
        "\"neighbors\":[",
        st.node_id, st.state,
        st.neighbor_count, st.route_count, st.topology_nodes, st.reachable_nodes,
        st.partitioned ? "true" : "false", st.beacon_interval_ms,
        (unsigned long long)st.beacons_sent, (unsigned long long)st.beacons_rx_accepted,
        (unsigned long long)st.neighbor_ups, (unsigned long long)st.neighbor_downs,
        (unsigned long long)st.link_transitions, (unsigned long long)st.failures_confirmed,
        (unsigned long long)st.recoveries_started, (unsigned long long)st.recoveries_completed,
        (unsigned long long)st.routes_installed, (unsigned long long)st.routes_withdrawn,
        (unsigned long long)st.packets_forwarded, (unsigned long long)st.packets_delivered_local,
        (unsigned long long)st.packets_dropped_no_route, (unsigned long long)st.events_dropped,
        st.radio_available ? "true" : "false", st.radio_operational ? "true" : "false",
        st.radio_channel);
    if (w < 0 || (size_t)w >= cap - used) return 0;
    used += (size_t)w;

    nbr_count = hh_neighbor_list(&sn->node.neighbors, nbr_ids, HH_MAX_NEIGHBORS);
    for (size_t i = 0; i < nbr_count; i++) {
        const hh_link_t *l = hh_link_health_get(&sn->node.link_health, nbr_ids[i]);
        w = snprintf(buf + used, cap - used, "%s{\"id\":%u,\"link_state\":\"%s\"}",
                     i == 0 ? "" : ",", nbr_ids[i],
                     l ? hh_link_state_str(l->state) : "unknown");
        if (w < 0 || (size_t)w >= cap - used) return 0;
        used += (size_t)w;
    }

    w = snprintf(buf + used, cap - used, "],\"routes\":[");
    if (w < 0 || (size_t)w >= cap - used) return 0;
    used += (size_t)w;

    /* Published route snapshot — the same lock-free pointer the forwarder
     * reads, so this adds no contention to the fast path (see routing.h). */
    {
        const hh_route_snapshot_t *snap = hh_routing_snapshot(&sn->node.routing);
        if (snap) {
            for (size_t i = 0; i < snap->count; i++) {
                const hh_route_entry_t *e = &snap->entries[i];
                w = snprintf(buf + used, cap - used,
                    "%s{\"dst\":%u,\"next_hop\":%u,\"hop_count\":%u,\"metric\":%.3f,"
                    "\"sequence_no\":%u,\"valid\":%s,\"alt_next_hop\":%u,\"has_alt\":%s}",
                    i == 0 ? "" : ",", e->destination, e->next_hop, e->hop_count,
                    (double)e->metric, e->sequence_no, e->valid ? "true" : "false",
                    e->alt_next_hop, e->has_alt ? "true" : "false");
                if (w < 0 || (size_t)w >= cap - used) return 0;
                used += (size_t)w;
            }
        }
    }

    w = snprintf(buf + used, cap - used, "]}\n");
    if (w < 0 || (size_t)w >= cap - used) return 0;
    used += (size_t)w;

    return used;
}

void telemetry_server_publish(hh_telemetry_server_t *ts, netsim_t *sim)
{
    char line[TS_LINE_CAP];

    if (!ts || !sim || ts->client_count == 0) return;

    for (size_t i = 0; i < sim->node_count; i++) {
        const sim_node_t *sn = &sim->nodes[i];
        size_t len;
        if (!sn->used || !sn->alive) continue;

        len = format_node_json(sn, line, sizeof line);
        if (len == 0) {
            /* wouldn't fit in TS_LINE_CAP: skip rather than truncate invalid
             * JSON. Should not happen at HH_MAX_NEIGHBORS scale; logged so a
             * silently-dropped node is visible instead of just "missing". */
            hh_ts_log("telemetry_server: node %u status line exceeds %d bytes, skipped\n",
                    sn->id, TS_LINE_CAP);
            continue;
        }
        telemetry_server_broadcast(ts, line, len);
    }
}

size_t telemetry_server_client_count(const hh_telemetry_server_t *ts)
{
    return ts ? ts->client_count : 0;
}

void telemetry_server_stop(hh_telemetry_server_t *ts)
{
    if (!ts) return;
    for (size_t i = 0; i < TS_MAX_CLIENTS; i++)
        if (ts->client_fds[i] != -1) close(ts->client_fds[i]);
    close(ts->listen_fd);
    ts->in_use = false;
}
