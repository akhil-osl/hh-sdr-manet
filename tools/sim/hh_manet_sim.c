/*
 * hh-manet-sim — multi-node MANET simulation driver.
 *
 * DEVELOPMENT/TEST TOOLING ONLY. Not part of the production runtime and never
 * linked into hh-manet.
 *
 * Every node here is a real hh_node_t running the production control plane and
 * data plane. The only thing replaced is the hardware boundary: each node gets
 * a mock radio implementing the same hh_radio_ops_t contract the FPGA adapter
 * will implement, and a virtual medium moves frames between them.
 *
 *   Application/driver  (this file)
 *        |
 *   MANET networking    (production, unmodified)
 *        |
 *   Radio/SDR abstraction (production, unmodified)
 *        |
 *   Mock radio          (tests/sim)
 *        |
 *   Virtual network     (tests/sim)
 */
#include "simui.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    const char *scenario;
    unsigned    nodes;
    uint64_t    seed;
    bool        step;
    bool        trace;
    bool        quiet_events;
} opts_t;

/* Node ids are 1..N and render as A..N. */
#define A 1u
#define B 2u
#define C 3u
#define D 4u
#define E 5u

static void add_nodes(netsim_t *s, unsigned count)
{
    for (unsigned i = 0; i < count; i++) netsim_add_node(s, simui_id_from_index(i));
}

static void settle(netsim_t *s, uint32_t ms) { netsim_run(s, ms, 10); }

/* ------------------------------------------------------------ scenario 1 */
static int scenario_basic(const opts_t *o)
{
    netsim_t s;
    simui_events_t ev;
    simui_stepper_t st = { o->step, 0 };
    unsigned n = o->nodes ? o->nodes : 3;

    if (n > 8) n = 8;
    simui_print_header("SCENARIO 1 - BASIC DISCOVERY AND ROUTING");
    printf("\n  Topology: a line of %u nodes, each linked to the next.\n", n);

    netsim_init(&s, o->seed);
    add_nodes(&s, n);
    for (unsigned i = 0; i + 1 < n; i++)
        netsim_link_up(&s, simui_id_from_index(i), simui_id_from_index(i + 1), -55.0f);

    simui_step_begin(&st, "Start all nodes (radios open, beaconing begins)");
    netsim_start_all(&s);
    simui_events_attach(&ev, &s, !o->quiet_events);
    simui_events_refresh();
    simui_action(&s, "%u nodes started", n);

    simui_step_begin(&st, "Beacon exchange and neighbor discovery");
    settle(&s, 1200);

    simui_step_begin(&st, "Route propagation across the line");
    settle(&s, 2500);
    simui_events_detach();

    simui_print_node_states(&s);
    simui_print_topology(&s);

    printf("  End-to-end path check:\n");
    simui_print_path(&s, A, simui_id_from_index(n - 1));
    printf("\n");
    return netsim_has_route(&s, A, simui_id_from_index(n - 1)) ? 0 : 1;
}

/* ------------------------------------------------------------ scenario 2 */
static int scenario_multihop(const opts_t *o)
{
    netsim_t s;
    simui_events_t ev;
    simui_traffic_t tr;
    simui_stepper_t st = { o->step, 0 };

    simui_print_header("SCENARIO 2 - MULTI-HOP DATA TRANSFER");
    printf("\n  Topology:  A <-> B <-> C <-> D\n");
    printf("  Traffic:   A -> D (3 hops)\n");

    netsim_init(&s, o->seed);
    add_nodes(&s, 4);
    netsim_link_up(&s, A, B, -55.0f);
    netsim_link_up(&s, B, C, -55.0f);
    netsim_link_up(&s, C, D, -55.0f);

    simui_step_begin(&st, "Start nodes and converge routes");
    netsim_start_all(&s);
    simui_events_attach(&ev, &s, !o->quiet_events);
    simui_events_refresh();
    settle(&s, 4000);
    simui_events_detach();

    simui_print_node_states(&s);

    simui_step_begin(&st, "Send application data A -> D through the data plane");
    printf("\n");
    simui_print_path(&s, A, D);
    printf("\n");

    simui_traffic_init(&tr, &s, o->trace);
    simui_events_attach(&ev, &s, false);   /* keep hop trace uncluttered */
    simui_send_burst(&tr, A, D, 10, 60, 600);
    simui_events_detach();

    simui_print_traffic_stats(&tr, &s);
    return 0;
}

/* ------------------------------------------------------------ scenario 3 */
static int scenario_alternate(const opts_t *o)
{
    netsim_t s;
    simui_events_t ev;
    simui_traffic_t tr;
    simui_stepper_t st = { o->step, 0 };
    hh_node_id_t first_hop;

    simui_print_header("SCENARIO 3 - ALTERNATE ROUTE SELECTION");
    printf("\n  Topology:     B\n");
    printf("              /   \\\n");
    printf("            A       D\n");
    printf("              \\   /\n");
    printf("                C\n");

    netsim_init(&s, o->seed);
    add_nodes(&s, 4);
    netsim_link_up(&s, A, B, -50.0f);
    netsim_link_up(&s, A, C, -50.0f);
    netsim_link_up(&s, B, D, -50.0f);
    netsim_link_up(&s, C, D, -50.0f);

    simui_step_begin(&st, "Start nodes and converge");
    netsim_start_all(&s);
    simui_events_attach(&ev, &s, !o->quiet_events);
    simui_events_refresh();
    settle(&s, 4000);
    simui_events_detach();

    first_hop = netsim_next_hop(&s, A, D);
    simui_print_node_states(&s);
    printf("  Chosen path:\n");
    simui_print_path(&s, A, D);

    simui_step_begin(&st, "Send traffic over the chosen path");
    simui_traffic_init(&tr, &s, o->trace);
    simui_send_burst(&tr, A, D, 5, 60, 400);

    simui_step_begin(&st, "Break the active path and watch the alternate take over");
    simui_events_attach(&ev, &s, !o->quiet_events);
    simui_events_refresh();
    simui_action(&s, "BREAKING LINK  A <-> %s (the path in use)", simui_name(first_hop));
    netsim_link_down(&s, A, first_hop);
    settle(&s, 5000);
    simui_events_detach();

    printf("\n  Path after recovery:\n");
    simui_print_path(&s, A, D);

    simui_step_begin(&st, "Resume traffic over the alternate path");
    simui_send_burst(&tr, A, D, 5, 60, 600);

    simui_print_node_states(&s);
    simui_print_traffic_stats(&tr, &s);

    printf("  Result: %s\n\n", netsim_has_route(&s, A, D)
        ? "connectivity preserved via the alternate path"
        : "no route to D (alternate unavailable)");
    return netsim_has_route(&s, A, D) ? 0 : 1;
}

/* ------------------------------------------------------------ scenario 4 */
static int scenario_node_failure(const opts_t *o)
{
    netsim_t s;
    simui_events_t ev;
    simui_traffic_t tr;
    simui_stepper_t st = { o->step, 0 };

    simui_print_header("SCENARIO 4 - NODE FAILURE");
    printf("\n  Topology:  A <-> B <-> C <-> D\n");
    printf("  Node B is powered off mid-run; it is the only relay.\n");

    netsim_init(&s, o->seed);
    add_nodes(&s, 4);
    netsim_link_up(&s, A, B, -55.0f);
    netsim_link_up(&s, B, C, -55.0f);
    netsim_link_up(&s, C, D, -55.0f);

    simui_step_begin(&st, "Start nodes and converge");
    netsim_start_all(&s);
    settle(&s, 4000);
    simui_print_node_states(&s);

    simui_step_begin(&st, "Send baseline traffic A -> D");
    simui_traffic_init(&tr, &s, o->trace);
    simui_send_burst(&tr, A, D, 5, 60, 400);

    simui_step_begin(&st, "Power off node B (its radio goes dark)");
    simui_events_attach(&ev, &s, !o->quiet_events);
    simui_events_refresh();
    simui_action(&s, "NODE B POWERED OFF");
    netsim_node_fail(&s, B);

    simui_step_begin(&st, "Neighbor loss, link failure, and route invalidation");
    settle(&s, 6000);
    simui_events_detach();

    simui_print_node_states(&s);
    simui_print_topology(&s);

    simui_step_begin(&st, "Attempt traffic again - A should have no route to D");
    simui_send_burst(&tr, A, D, 3, 60, 400);
    simui_print_traffic_stats(&tr, &s);

    printf("  Result: A -> D is %s (B was the only relay)\n\n",
           netsim_has_route(&s, A, D) ? "still routed" : "correctly unreachable");
    return 0;
}

/* ------------------------------------------------------------ scenario 5 */
static int scenario_degradation(const opts_t *o)
{
    netsim_t s;
    simui_events_t ev;
    simui_stepper_t st = { o->step, 0 };
    const struct { float rssi, snr, per; const char *label; } steps[] = {
        { -50.0f, 25.0f, 0.00f, "excellent" },
        { -75.0f, 18.0f, 0.05f, "marginal" },
        { -88.0f, 10.0f, 0.15f, "poor" },
        { -95.0f,  3.0f, 0.45f, "failing" },
    };

    simui_print_header("SCENARIO 5 - LINK DEGRADATION");
    printf("\n  Topology:  A <-> B\n");
    printf("  RF quality is degraded in stages; the Link Health Monitor\n");
    printf("  fuses RSSI, SNR and PER and drives the state machine.\n");

    netsim_init(&s, o->seed);
    add_nodes(&s, 2);
    netsim_link_up(&s, A, B, -50.0f);

    netsim_start_all(&s);
    settle(&s, 2500);

    simui_events_attach(&ev, &s, !o->quiet_events);
    simui_events_refresh();
    for (size_t i = 0; i < sizeof steps / sizeof steps[0]; i++) {
        char desc[128];
        snprintf(desc, sizeof desc, "Degrade link to %s (rssi %.0fdBm, snr %.0fdB, per %.0f%%)",
                 steps[i].label, (double)steps[i].rssi, (double)steps[i].snr,
                 (double)(steps[i].per * 100.0f));
        simui_step_begin(&st, desc);
        simui_action(&s, "RF now: rssi=%.0fdBm snr=%.0fdB per=%.0f%%",
                     (double)steps[i].rssi, (double)steps[i].snr,
                     (double)(steps[i].per * 100.0f));
        netsim_link_set_quality(&s, A, B, steps[i].rssi, steps[i].snr, steps[i].per);
        settle(&s, 2500);
        simui_print_link_health(&s, A);
    }

    simui_step_begin(&st, "Link goes fully silent");
    simui_action(&s, "LINK DOWN  A <-> B");
    netsim_link_down(&s, A, B);
    settle(&s, 6000);
    simui_events_detach();

    simui_print_link_health(&s, A);
    simui_print_node_states(&s);
    return 0;
}

/* ------------------------------------------------------------ scenario 6 */
static int scenario_recovery(const opts_t *o)
{
    netsim_t s;
    simui_events_t ev;
    simui_traffic_t tr;
    simui_stepper_t st = { o->step, 0 };

    simui_print_header("SCENARIO 6 - FAILURE AND RECOVERY");
    printf("\n  Topology:  A <-> B <-> C\n");
    printf("  The A-B link is broken, detected, then restored.\n");

    netsim_init(&s, o->seed);
    add_nodes(&s, 3);
    netsim_link_up(&s, A, B, -55.0f);
    netsim_link_up(&s, B, C, -55.0f);

    simui_step_begin(&st, "Nodes start and discover each other");
    netsim_start_all(&s);
    simui_events_attach(&ev, &s, !o->quiet_events);
    simui_events_refresh();
    settle(&s, 4000);

    simui_step_begin(&st, "Baseline data transfer A -> C");
    simui_events_detach();
    simui_print_path(&s, A, C);
    simui_traffic_init(&tr, &s, o->trace);
    simui_send_burst(&tr, A, C, 5, 60, 400);

    simui_step_begin(&st, "Break the A-B link");
    simui_events_attach(&ev, &s, !o->quiet_events);
    simui_events_refresh();
    simui_action(&s, "LINK DOWN  A <-> B");
    netsim_link_down(&s, A, B);

    simui_step_begin(&st, "Detection: degradation, confirmation, invalidation");
    settle(&s, 6000);
    simui_print_node_states(&s);

    simui_step_begin(&st, "Restore the A-B link");
    simui_action(&s, "LINK UP    A <-> B");
    netsim_link_up(&s, A, B, -55.0f);

    simui_step_begin(&st, "Rediscovery and route reinstallation");
    settle(&s, 6000);
    simui_events_detach();

    simui_print_node_states(&s);
    printf("  Path after recovery:\n");
    simui_print_path(&s, A, C);

    simui_step_begin(&st, "Data transfer resumes");
    simui_send_burst(&tr, A, C, 5, 60, 600);
    simui_print_traffic_stats(&tr, &s);

    printf("  Result: %s\n\n", netsim_has_route(&s, A, C)
        ? "route recovered and stabilized" : "route did NOT recover");
    return netsim_has_route(&s, A, C) ? 0 : 1;
}

/* ------------------------------------------------------------ scenario 7 */
static int scenario_partition(const opts_t *o)
{
    netsim_t s;
    simui_events_t ev;
    simui_stepper_t st = { o->step, 0 };
    hh_node_id_t ga[] = { A, B }, gb[] = { C, D };

    simui_print_header("SCENARIO 7 - NETWORK PARTITION AND MERGE");
    printf("\n  Start:  A <-> B <-> C <-> D\n");
    printf("  Split:  A <-> B     C <-> D\n");
    printf("  Merge:  A <-> B <-> C <-> D\n");

    netsim_init(&s, o->seed);
    add_nodes(&s, 4);
    netsim_link_up(&s, A, B, -55.0f);
    netsim_link_up(&s, B, C, -55.0f);
    netsim_link_up(&s, C, D, -55.0f);

    simui_step_begin(&st, "Start the full network and converge");
    netsim_start_all(&s);
    settle(&s, 4500);
    simui_print_node_states(&s);
    printf("  Before partition, A -> D:\n");
    simui_print_path(&s, A, D);

    simui_step_begin(&st, "Sever the network into {A,B} and {C,D}");
    simui_events_attach(&ev, &s, !o->quiet_events);
    simui_events_refresh();
    simui_action(&s, "PARTITION  breaking all links between {A,B} and {C,D}");
    netsim_partition(&s, ga, 2, gb, 2);
    settle(&s, 6000);
    simui_events_detach();

    simui_print_node_states(&s);
    simui_print_topology(&s);
    printf("  Cross-partition route A -> D: %s\n",
           netsim_has_route(&s, A, D) ? "STILL PRESENT (unexpected)" : "correctly withdrawn");
    printf("  Intra-partition route A -> B: %s\n",
           netsim_has_route(&s, A, B) ? "intact (each half keeps working)" : "lost");

    simui_step_begin(&st, "Restore the severed edge B <-> C");
    simui_events_attach(&ev, &s, !o->quiet_events);
    simui_events_refresh();
    simui_action(&s, "MERGE  restoring link B <-> C");
    netsim_link_up(&s, B, C, -55.0f);
    settle(&s, 8000);
    simui_events_detach();

    simui_print_node_states(&s);
    printf("  After merge, A -> D:\n");
    simui_print_path(&s, A, D);
    printf("\n  Result: %s\n\n", netsim_has_route(&s, A, D)
        ? "partitions merged and end-to-end route restored"
        : "merge did not restore the route");
    return netsim_has_route(&s, A, D) ? 0 : 1;
}

/* ---------------------------------------------------------------- driver */

static void usage(const char *argv0)
{
    printf(
"HH-SDR MANET simulation driver\n"
"\n"
"usage: %s [--scenario <name>] [options]\n"
"\n"
"scenarios:\n"
"  basic        discovery, neighbor tables, route creation (line topology)\n"
"  multihop     multi-hop data transfer A -> D with per-hop tracing\n"
"  alternate    alternate-route selection after a link break (diamond)\n"
"  nodefail     node powered off; neighbor loss and route invalidation\n"
"  degradation  staged RF degradation through the link-health state machine\n"
"  recovery     link failure, detection, restoration, stabilization\n"
"  partition    network split into two halves, then merged\n"
"  all          run every scenario in sequence\n"
"\n"
"options:\n"
"  --nodes N    node count, where the scenario supports it (basic; default 3)\n"
"  --seed S     PRNG seed for reproducible runs (default 1)\n"
"  --step       pause between phases and wait for Enter\n"
"  --trace      print every per-packet hop\n"
"  --quiet      suppress the event stream, print summaries only\n"
"  --help       this message\n"
"\n"
"Every node runs the real production control plane and data plane; only the\n"
"radio is virtual. Events shown are emitted by the production components.\n",
    argv0);
}

int main(int argc, char **argv)
{
    opts_t o;
    int rc = 0;

    memset(&o, 0, sizeof o);
    o.scenario = "basic";
    o.seed = 1;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--scenario") && i + 1 < argc) o.scenario = argv[++i];
        else if (!strcmp(argv[i], "--nodes") && i + 1 < argc) o.nodes = (unsigned)strtoul(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--seed") && i + 1 < argc)  o.seed = strtoull(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--step"))  o.step = true;
        else if (!strcmp(argv[i], "--trace")) o.trace = true;
        else if (!strcmp(argv[i], "--quiet")) o.quiet_events = true;
        else if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) { usage(argv[0]); return 0; }
        else { fprintf(stderr, "unknown option: %s\n\n", argv[i]); usage(argv[0]); return 2; }
    }

    /* Nodes log at INFO during construction; keep that off the operator view
     * until the event renderer is attached. */
    hh_log_set_level(HH_LOG_ERROR);

    printf("\n");
    printf("  HH-SDR MANET SIMULATION\n");
    printf("  scenario=%s  seed=%llu%s%s\n", o.scenario,
           (unsigned long long)o.seed, o.step ? "  [step mode]" : "",
           o.trace ? "  [packet trace]" : "");
    printf("  Running the production MANET stack over a virtual radio network.\n");

    if (!strcmp(o.scenario, "basic"))            rc = scenario_basic(&o);
    else if (!strcmp(o.scenario, "multihop"))    rc = scenario_multihop(&o);
    else if (!strcmp(o.scenario, "alternate"))   rc = scenario_alternate(&o);
    else if (!strcmp(o.scenario, "nodefail"))    rc = scenario_node_failure(&o);
    else if (!strcmp(o.scenario, "degradation")) rc = scenario_degradation(&o);
    else if (!strcmp(o.scenario, "recovery"))    rc = scenario_recovery(&o);
    else if (!strcmp(o.scenario, "partition"))   rc = scenario_partition(&o);
    else if (!strcmp(o.scenario, "all")) {
        rc |= scenario_basic(&o);
        rc |= scenario_multihop(&o);
        rc |= scenario_alternate(&o);
        rc |= scenario_node_failure(&o);
        rc |= scenario_degradation(&o);
        rc |= scenario_recovery(&o);
        rc |= scenario_partition(&o);
        simui_print_header("ALL SCENARIOS COMPLETE");
        printf("\n  overall result: %s\n\n", rc == 0 ? "OK" : "one or more scenarios failed");
    } else {
        fprintf(stderr, "unknown scenario: %s\n\n", o.scenario);
        usage(argv[0]);
        return 2;
    }
    return rc;
}
