#include "simui.h"
#include "hhsdr/manet/telemetry.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* ------------------------------------------------------------------ names */

const char *simui_name(hh_node_id_t id)
{
    static char buf[8][8];
    static unsigned turn;
    char *b = buf[turn++ % 8];

    if (id >= 1 && id <= 26) { b[0] = (char)('A' + id - 1); b[1] = '\0'; }
    else snprintf(b, 8, "#%u", id);
    return b;
}

hh_node_id_t simui_id_from_index(size_t index) { return (hh_node_id_t)(index + 1); }

/* ----------------------------------------------------------------- events */

static simui_events_t *g_ev;

static hh_time_ms_t ev_now(void)
{
    return (g_ev && g_ev->sim) ? g_ev->sim->vc.now : 0;
}

static void stamp(char *out, size_t cap, hh_time_ms_t now)
{
    hh_time_ms_t rel = (g_ev && now >= g_ev->t0) ? now - g_ev->t0 : 0;
    snprintf(out, cap, "[%02llu.%03llu]",
             (unsigned long long)(rel / 1000), (unsigned long long)(rel % 1000));
}

/* Pull one key's value out of a structured log record. */
static bool field(const char *line, const char *key, char *out, size_t cap)
{
    char pat[64];
    const char *p, *e;
    size_t n;

    snprintf(pat, sizeof pat, "%s=", key);
    p = strstr(line, pat);
    if (!p) return false;
    p += strlen(pat);
    e = strchr(p, ' ');
    n = e ? (size_t)(e - p) : strlen(p);
    if (n >= cap) n = cap - 1;
    memcpy(out, p, n);
    out[n] = '\0';
    return true;
}

static hh_node_id_t field_id(const char *line, const char *key)
{
    char v[32];
    if (!field(line, key, v, sizeof v)) return HH_NODE_ID_INVALID;
    return (hh_node_id_t)strtoul(v, NULL, 10);
}

/*
 * Translate a production log record into an operator-facing line.
 *
 * This is a RENDERING of a real event, not a synthesized one: if the stack did
 * not emit the record, nothing is printed. Records with no operator meaning are
 * ignored rather than dressed up.
 */
static void render_event(const char *line)
{
    char ts[16], ev[64], comp[32], who[12];
    hh_node_id_t node, nbr, dst, hop, emitter;

    if (!field(line, "event", ev, sizeof ev)) return;
    if (!field(line, "comp", comp, sizeof comp)) return;
    stamp(ts, sizeof ts, ev_now());

    /* Attribute the record to the node whose tick produced it. Most records
     * carry no node= field because a component only ever logs about its own
     * node, so the simulator's current-node marker is the reliable source. */
    emitter = (g_ev && g_ev->sim) ? g_ev->sim->current_node : HH_NODE_ID_INVALID;
    if (emitter != HH_NODE_ID_INVALID) snprintf(who, sizeof who, "%-2s", simui_name(emitter));
    else                               snprintf(who, sizeof who, "%-2s", "*");

    node = field_id(line, "node");
    (void)node;
    nbr  = field_id(line, "neighbor");
    dst  = field_id(line, "dst");
    hop  = field_id(line, "next_hop");

    if (!strcmp(ev, "neighbor_up")) {
        printf("  %s %s NEIGHBOR UP      discovered %s\n", ts, who, simui_name(nbr));
    } else if (!strcmp(ev, "neighbor_down")) {
        char why[32] = "";
        field(line, "reason", why, sizeof why);
        printf("  %s %s NEIGHBOR DOWN    lost %s (%s)\n", ts, who, simui_name(nbr), why);
    } else if (!strcmp(ev, "link_state_changed")) {
        char oldv[32] = "", newv[32] = "", cause[32] = "";
        field(line, "old", oldv, sizeof oldv);
        field(line, "new", newv, sizeof newv);
        field(line, "cause", cause, sizeof cause);
        printf("  %s %s LINK %-11s %s: %s -> %s (cause: %s)\n", ts, who,
               !strcmp(newv, "Failed") ? "FAILED" :
               !strcmp(newv, "Degraded") ? "DEGRADED" :
               !strcmp(newv, "SuspectedFailure") ? "SUSPECT" :
               !strcmp(newv, "Recovering") ? "RECOVERING" : "HEALTHY",
               simui_name(nbr), oldv, newv, cause);
    } else if (!strcmp(ev, "route_installed")) {
        printf("  %s %s ROUTE INSTALLED  to %s via %s\n", ts, who, simui_name(dst), simui_name(hop));
    } else if (!strcmp(ev, "route_switched_to_alternate")) {
        hh_node_id_t failed = field_id(line, "failed_next_hop");
        hh_node_id_t nh = field_id(line, "new_next_hop");
        printf("  %s %s ROUTE SWITCHED   to %s: %s -> %s (alternate)\n", ts, who,
               simui_name(dst), simui_name(failed), simui_name(nh));
    } else if (!strcmp(ev, "route_withdrawn")) {
        char why[32] = "";
        field(line, "reason", why, sizeof why);
        printf("  %s %s ROUTE INVALIDATED to %s (%s)\n", ts, who, simui_name(dst), why);
    } else if (!strcmp(ev, "failure_confirmed")) {
        char cause[32] = "";
        field(line, "cause", cause, sizeof cause);
        printf("  %s %s FAILURE CONFIRMED %s (cause: %s)\n", ts, who,
               simui_name(field_id(line, "node")), cause);
    } else if (!strcmp(ev, "recovery_started")) {
        char cause[32] = "";
        field(line, "cause", cause, sizeof cause);
        printf("  %s %s RECOVERY STARTED target %s (cause: %s)\n", ts, who,
               simui_name(field_id(line, "target")), cause);
    } else if (!strcmp(ev, "recovery_completed")) {
        char strat[32] = "";
        field(line, "strategy", strat, sizeof strat);
        printf("  %s %s RECOVERY DONE    target %s (%s)\n", ts, who,
               simui_name(field_id(line, "target")), strat);
    } else if (!strcmp(ev, "recovery_strategy")) {
        char strat[32] = "";
        field(line, "strategy", strat, sizeof strat);
        printf("  %s %s RECOVERY PLAN    target %s: %s\n", ts, who,
               simui_name(field_id(line, "target")), strat);
    } else if (!strcmp(ev, "partition_detected")) {
        printf("  %s %s PARTITION        isolated (%s unreachable)\n", ts, who,
               simui_name(field_id(line, "branch_root")));
    } else if (!strcmp(ev, "network_merged")) {
        printf("  %s %s NETWORK MERGED   rejoined via %s\n", ts, who,
               simui_name(field_id(line, "rejoined_neighbor")));
    } else if (!strcmp(ev, "phase_change")) {
        char reason[32] = "";
        field(line, "reason", reason, sizeof reason);
        printf("  %s %s DISCOVERY        entered steady state (%s)\n", ts, who, reason);
    } else if (!strcmp(ev, "cadence_changed")) {
        char oldms[16] = "", newms[16] = "";
        field(line, "old_ms", oldms, sizeof oldms);
        field(line, "new_ms", newms, sizeof newms);
        printf("  %s %s CADENCE ADAPTED  %sms -> %sms\n", ts, who, oldms, newms);
    } else if (!strcmp(ev, "next_hop_damped")) {
        printf("  %s %s DAMPENED         %s penalised after repeated flaps\n", ts, who,
               simui_name(field_id(line, "next_hop")));
    } else if (!strcmp(ev, "channel_changed")) {
        char oldc[16] = "", newc[16] = "";
        field(line, "old", oldc, sizeof oldc);
        field(line, "new", newc, sizeof newc);
        printf("  %s %s CHANNEL CHANGED  %s -> %s (RF interference)\n", ts, who, oldc, newc);
    }
    /* Anything else is internal detail with no operator meaning. */
}

static void log_sink(hh_log_level_t lvl, const char *line, void *ctx)
{
    (void)lvl; (void)ctx;
    if (!g_ev) return;
    g_ev->count++;
    if (g_ev->enabled) render_event(line);
}

/*
 * Re-assert the capture level.
 *
 * hh_node_configure() applies each node's configured log level globally, so a
 * node started or reconfigured after attach would otherwise silence the event
 * stream. Calling this from the run loop keeps capture working regardless of
 * the order the caller attaches and starts nodes in.
 */
void simui_events_refresh(void)
{
    if (g_ev && hh_log_level() < HH_LOG_INFO) hh_log_set_level(HH_LOG_INFO);
}

void simui_events_attach(simui_events_t *ev, const netsim_t *s, bool enabled)
{
    ev->enabled = enabled;
    ev->sim = s;
    ev->t0 = s->vc.now;
    ev->count = 0;
    g_ev = ev;
    /* INFO carries every state transition the architecture specifies. */
    hh_log_set_level(HH_LOG_INFO);
    hh_log_set_sink(log_sink, NULL);
}

void simui_events_detach(void)
{
    hh_log_set_sink(NULL, NULL);
    hh_log_set_level(HH_LOG_ERROR);
    g_ev = NULL;
}

void simui_action(const netsim_t *s, const char *fmt, ...)
{
    char ts[16];
    va_list ap;
    hh_time_ms_t now = s ? s->vc.now : 0;

    stamp(ts, sizeof ts, now);
    printf("  %s >> ", ts);
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
}

/* ---------------------------------------------------------------- traffic */

void simui_traffic_init(simui_traffic_t *t, netsim_t *s, bool trace)
{
    memset(t, 0, sizeof *t);
    t->sim = s;
    t->trace = trace;
}

/* Data frame layout owned by the forwarder: src(4) dst(4) ttl(1) hops(1). */
#define FWD_HDR 10u

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/*
 * Frame observer: fires for every frame the virtual medium moves. Data frames
 * carrying our tag are matched back to the packet that was sent, so the hop
 * list is the path the production forwarder actually chose.
 */
static void on_frame(hh_node_id_t from, hh_node_id_t to, const hh_frame_t *f,
                     bool delivered, hh_time_ms_t now, void *ctx)
{
    simui_traffic_t *t = ctx;
    uint32_t tag;
    char ts[16];

    if (f->kind != HH_FRAME_DATA || f->len < FWD_HDR + 4) return;
    tag = rd32(f->data + FWD_HDR);

    for (size_t i = 0; i < t->count; i++) {
        simui_packet_t *p = &t->packets[i];
        if (!p->used || p->id != tag) continue;

        stamp(ts, sizeof ts, now);
        if (!delivered) {
            t->frames_dropped_in_flight++;
            if (t->trace)
                printf("  %s   DROP     packet %03u on %s -> %s (link loss)\n",
                       ts, p->id, simui_name(from), simui_name(to));
            return;
        }
        if (p->hop_count < SIMUI_MAX_HOPS) p->hops[p->hop_count++] = to;

        if (to == p->dst) {
            p->delivered = true;
            p->delivered_at = now;
            if (t->trace)
                printf("  %s   DELIVER  packet %03u -> %s (%u hops, %llums)\n",
                       ts, p->id, simui_name(to), p->hop_count,
                       (unsigned long long)(now - p->sent_at));
        } else if (t->trace) {
            printf("  %s   FORWARD  packet %03u  %s -> %s\n",
                   ts, p->id, simui_name(from), simui_name(to));
        }
        return;
    }
}

hh_status_t simui_send(simui_traffic_t *t, hh_node_id_t from, hh_node_id_t to)
{
    sim_node_t *n = netsim_node(t->sim, from);
    simui_packet_t *p;
    uint8_t payload[16];
    uint32_t tag;
    hh_status_t st;

    if (!n || t->count >= SIMUI_MAX_PACKETS) return HH_ERR_NOMEM;

    p = &t->packets[t->count];
    memset(p, 0, sizeof *p);
    tag = (uint32_t)t->count + 1;
    p->used = true;
    p->id = tag;
    p->src = from;
    p->dst = to;
    p->sent_at = t->sim->vc.now;
    t->count++;

    /* Tag first, then payload bytes. The tag lets a delivered frame be matched
     * back to its send without the forwarder knowing anything about tracing. */
    payload[0] = (uint8_t)tag;
    payload[1] = (uint8_t)(tag >> 8);
    payload[2] = (uint8_t)(tag >> 16);
    payload[3] = (uint8_t)(tag >> 24);
    memcpy(payload + 4, "HHSDR-TEST-D", 12);

    netsim_set_frame_observer(t->sim, on_frame, t);

    /* Goes through the production data plane: route lookup, TTL, forwarding. */
    st = hh_node_send(&n->node, to, payload, sizeof payload, t->sim->vc.now);
    if (st == HH_OK && t->trace) {
        char ts[16];
        stamp(ts, sizeof ts, p->sent_at);
        printf("  %s   SEND     packet %03u  %s -> %s\n",
               ts, p->id, simui_name(from), simui_name(to));
    } else if (st != HH_OK) {
        char ts[16];
        stamp(ts, sizeof ts, p->sent_at);
        if (t->trace)
            printf("  %s   NO ROUTE packet %03u  %s -> %s (%s)\n",
                   ts, p->id, simui_name(from), simui_name(to), hh_status_str(st));
    }
    return st;
}

void simui_send_burst(simui_traffic_t *t, hh_node_id_t from, hh_node_id_t to,
                      uint32_t count, uint32_t interval_ms, uint32_t settle_ms)
{
    for (uint32_t i = 0; i < count; i++) {
        simui_send(t, from, to);
        netsim_run(t->sim, interval_ms, 10);
    }
    netsim_run(t->sim, settle_ms, 10);
}

/* -------------------------------------------------------------- reporting */

void simui_print_header(const char *title)
{
    printf("\n");
    printf("================================================================\n");
    printf("  %s\n", title);
    printf("================================================================\n");
}

void simui_print_rule(void)
{
    printf("  ----------------------------------------------------------\n");
}

void simui_print_node_states(netsim_t *s)
{
    printf("\n");
    for (size_t i = 0; i < s->node_count; i++) {
        sim_node_t *n = &s->nodes[i];
        const hh_route_snapshot_t *snap;
        hh_node_id_t ids[HH_MAX_NEIGHBORS];
        size_t nn;

        if (!n->used) continue;
        printf("  Node %s%s\n", simui_name(n->id), n->alive ? "" : "  [OFFLINE]");

        nn = hh_neighbor_list(&n->node.neighbors, ids, HH_MAX_NEIGHBORS);
        printf("    neighbors: ");
        if (nn == 0) printf("(none)");
        for (size_t k = 0; k < nn; k++) {
            const hh_link_t *l = hh_link_health_get(&n->node.link_health, ids[k]);
            printf("%s(%s) ", simui_name(ids[k]),
                   l ? hh_link_state_str(l->state) : "?");
        }
        printf("\n");

        snap = hh_routing_snapshot(&n->node.routing);
        printf("    routes:");
        {
            size_t shown = 0;
            for (size_t k = 0; snap && k < snap->count; k++) {
                const hh_route_entry_t *e = &snap->entries[k];
                if (!e->valid) continue;
                printf("\n      %s -> via %s (%u hop%s, metric %.2f%s)",
                       simui_name(e->destination), simui_name(e->next_hop),
                       e->hop_count, e->hop_count == 1 ? "" : "s",
                       (double)e->metric,
                       e->has_alt ? ", alt available" : "");
                shown++;
            }
            if (!shown) printf(" (none)");
        }
        printf("\n\n");
    }
}

void simui_print_topology(netsim_t *s)
{
    printf("\n  CURRENT TOPOLOGY (physical links)\n\n");
    for (size_t i = 0; i < s->node_count; i++) {
        sim_node_t *a = &s->nodes[i];
        bool any = false;
        if (!a->used) continue;
        printf("    %s%s ", simui_name(a->id), a->alive ? "" : "(down)");
        for (size_t j = 0; j < s->node_count; j++) {
            if (i == j || !s->nodes[j].used) continue;
            if (s->links[i][j].up) {
                sim_link_t *l = &s->links[i][j];
                if (!any) { printf("--"); any = true; }
                printf(" %s", simui_name(s->nodes[j].id));
                if (l->loss > 0.0f) printf("(loss %.0f%%)", (double)(l->loss * 100.0f));
                if (l->rssi < -80.0f) printf("(weak %.0fdBm)", (double)l->rssi);
            }
        }
        if (!any) printf("-- (isolated)");
        printf("\n");
    }
    printf("\n");
}

void simui_print_routes_from(netsim_t *s, hh_node_id_t from)
{
    sim_node_t *n = netsim_node(s, from);
    const hh_route_snapshot_t *snap;
    size_t shown = 0;

    if (!n) return;
    snap = hh_routing_snapshot(&n->node.routing);
    printf("\n  Routes from %s:\n", simui_name(from));
    for (size_t k = 0; snap && k < snap->count; k++) {
        const hh_route_entry_t *e = &snap->entries[k];
        if (!e->valid) continue;
        printf("    %s -> %s\n", simui_name(e->destination), simui_name(e->next_hop));
        shown++;
    }
    if (!shown) printf("    (no routes)\n");
    printf("\n");
}

/*
 * Walk the real next-hop chain to show the end-to-end path. Each step reads the
 * published snapshot of the node that would actually forward, so this is the
 * path packets take, not an independent computation.
 */
void simui_print_path(netsim_t *s, hh_node_id_t from, hh_node_id_t to)
{
    hh_node_id_t cur = from;
    int guard = 0;

    printf("  route: %s", simui_name(from));
    while (cur != to && guard++ < 16) {
        hh_node_id_t nh = netsim_next_hop(s, cur, to);
        if (nh == HH_NODE_ID_INVALID) { printf(" -> ??? (no route)"); return; }
        printf(" -> %s", simui_name(nh));
        cur = nh;
    }
    if (cur != to) printf(" (loop guard hit)");
    printf("\n");
}

void simui_print_link_health(netsim_t *s, hh_node_id_t from)
{
    sim_node_t *n = netsim_node(s, from);
    hh_node_id_t ids[HH_MAX_NEIGHBORS];
    size_t nn;

    if (!n) return;
    nn = hh_neighbor_list(&n->node.neighbors, ids, HH_MAX_NEIGHBORS);
    printf("\n  Link health at %s:\n", simui_name(from));
    if (!nn) { printf("    (no neighbors)\n\n"); return; }
    for (size_t k = 0; k < nn; k++) {
        const hh_link_t *l = hh_link_health_get(&n->node.link_health, ids[k]);
        const hh_neighbor_t *nb = hh_neighbor_get(&n->node.neighbors, ids[k]);
        if (!l || !nb) continue;
        printf("    %s  state=%-16s score=%.3f  rssi=%.0fdBm snr=%.0fdB\n",
               simui_name(ids[k]), hh_link_state_str(l->state), (double)l->score,
               (double)nb->last_sample.rssi, (double)nb->last_sample.snr);
    }
    printf("\n");
}

void simui_print_traffic_stats(const simui_traffic_t *t, netsim_t *s)
{
    uint64_t delivered = 0, lost = 0;
    hh_time_ms_t sum = 0, lo = (hh_time_ms_t)-1, hi = 0;
    uint64_t forwarded = 0, no_route = 0, installs = 0, withdrawals = 0;
    uint64_t failures = 0, recoveries = 0;

    for (size_t i = 0; i < t->count; i++) {
        const simui_packet_t *p = &t->packets[i];
        if (!p->used) continue;
        if (p->delivered) {
            hh_time_ms_t lat = p->delivered_at - p->sent_at;
            delivered++;
            sum += lat;
            if (lat < lo) lo = lat;
            if (lat > hi) hi = lat;
        } else lost++;
    }

    /* Counters come from the production components, not a parallel tally. */
    for (size_t i = 0; i < s->node_count; i++) {
        const sim_node_t *n = &s->nodes[i];
        if (!n->used) continue;
        forwarded   += n->node.forwarder.forwarded;
        no_route    += n->node.forwarder.dropped_no_route;
        installs    += n->node.routing.installs;
        withdrawals += n->node.routing.withdrawals;
        failures    += n->node.failure_detector.confirmations;
        recoveries  += n->node.self_healing.recoveries_started;
    }

    printf("\n");
    simui_print_rule();
    printf("  TRAFFIC STATISTICS\n");
    simui_print_rule();
    printf("    packets generated   : %zu\n", t->count);
    printf("    packets delivered   : %llu\n", (unsigned long long)delivered);
    printf("    packets lost        : %llu\n", (unsigned long long)lost);
    printf("    packet loss         : %.1f%%\n",
           t->count ? 100.0 * (double)lost / (double)t->count : 0.0);
    if (delivered) {
        printf("    latency avg         : %llu ms\n", (unsigned long long)(sum / delivered));
        printf("    latency min         : %llu ms\n", (unsigned long long)lo);
        printf("    latency max         : %llu ms\n", (unsigned long long)hi);
    } else {
        printf("    latency             : n/a (nothing delivered)\n");
    }
    printf("\n");
    printf("    hops forwarded      : %llu   (production forwarder counter)\n",
           (unsigned long long)forwarded);
    printf("    dropped, no route   : %llu\n", (unsigned long long)no_route);
    printf("    frames lost on links: %llu   (virtual medium)\n",
           (unsigned long long)t->frames_dropped_in_flight);
    printf("    routes installed    : %llu\n", (unsigned long long)installs);
    printf("    routes withdrawn    : %llu\n", (unsigned long long)withdrawals);
    printf("    failures confirmed  : %llu\n", (unsigned long long)failures);
    printf("    recoveries started  : %llu\n", (unsigned long long)recoveries);
    simui_print_rule();
    printf("\n");
}

/* ------------------------------------------------------------- step mode */

void simui_step_begin(simui_stepper_t *st, const char *description)
{
    st->step_no++;
    printf("\n[STEP %u] %s\n", st->step_no, description);
    if (st->enabled) {
        printf("          press Enter to continue...");
        fflush(stdout);
        while (1) {
            int c = getchar();
            if (c == '\n' || c == EOF) break;
        }
    }
}
