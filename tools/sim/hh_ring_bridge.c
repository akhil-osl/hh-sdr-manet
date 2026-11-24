/*
 * hh-ring-bridge: like hh-slot-bridge, but every PDU goes through the frame
 * rings and the fake PL (tests/sim/ring_net) instead of straight over the
 * simulated air. Development tooling only, never linked into a daemon.
 *
 * Each node is a network namespace that already exists (ip netns add n1 ...).
 * The bridge creates a TAP interface in each one. Ethernet frames that Linux
 * writes to a TAP lose their Ethernet header and enter that node's MAC; the
 * MAC's PDU goes into the node's TX ring, the fake PL pulls it once per TDMA
 * frame, and it reaches the neighbours through their RX rings. The next hop is
 * the destination link address, 02-00-00-00-hh-ll (frame specification v2 4.5).
 *
 * It runs in real time, because the processes on the TAPs do. Needs root
 * (CAP_SYS_ADMIN and CAP_NET_ADMIN), for example a privileged container.
 *
 * Slot capacity is a placeholder. Fragmentation is excluded, so the TAP MTU
 * must not exceed the maximum payload printed at start.
 */
#define _GNU_SOURCE
#include "ring_net.h"
#include "slot_bridge_frames.h"
#include "slot_net.h"
#include <errno.h>
#include <fcntl.h>
#include <linux/if_tun.h>
#include <net/if.h>
#include <poll.h>
#include <sched.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#define MAX_FRAME 2048u

typedef struct {
    int      fd;
    uint64_t tap_rx;
    uint64_t dropped[4];          /* indexed by br_result_t */
    uint64_t mac_accepted, mac_queue_full, mac_too_large, mac_other;
    uint64_t delivered, write_errors;
} port_t;

typedef struct {
    rn_config_t cfg;
    const char *topology, *prefix, *ifname, *pcap_path, *summary_path, *control_path;
    unsigned    duration_s;
} opts_t;

static ring_net_t *g_net;
static port_t g_port[RN_MAX_NODES];
static volatile sig_atomic_t g_stop;

/* Prints the command line options. */
static void usage(void)
{
    fputs("usage: hh-ring-bridge [options]   (root; namespaces n1..nN must exist)\n"
          "  --nodes N            nodes, 2 to 32 (default 4)\n"
          "  --topology T         line | ring | mesh (default line)\n"
          "  --slots N            slots per frame (default 20, must be >= nodes)\n"
          "  --frames N           frames per superframe (default 50)\n"
          "  --slot-ms N          slot length in ms (default 1)\n"
          "  --capacity N         PDU bytes per slot, up to 2048 (default 256, a placeholder)\n"
          "  --ring-slots N       places in each ring, a power of two (default 32)\n"
          "  --ring-path FMT      keep each node's rings in a file, FMT has one %u for the node\n"
          "                       number, for example /dev/shm/hh-ring-%u (default: rings in memory)\n"
          "  --ring-create        create those files here; without it radiod must have made them\n"
          "  --lead N             TDMA frames between building a PDU and sending it (default 1)\n"
          "  --loss N             link loss in permille (default 0)\n"
          "  --seed N             random seed (default 1)\n"
          "  --netns-prefix P     namespace name prefix (default n)\n"
          "  --ifname NAME        TAP name in each namespace (default tap0)\n"
          "  --control FILE       FIFO read for lines 'down A B' and 'up A B'\n"
          "  --duration S         stop after S seconds (default 0 = until SIGINT or SIGTERM)\n"
          "  --pcap FILE          capture of every PDU put on the air\n"
          "  --summary FILE       write the summary here as well\n", stderr);
}

/* Reads the command line into the options. Returns false on an error. */
static int parse(int argc, char **argv, opts_t *o)
{
    memset(o, 0, sizeof *o);
    rn_config_default(&o->cfg, 4);
    o->topology = "line"; o->prefix = "n"; o->ifname = "tap0";
    for (int i = 1; i < argc; i++) {
        const char *k = argv[i], *v = i + 1 < argc ? argv[i + 1] : NULL;
        if (!strcmp(k, "-h") || !strcmp(k, "--help")) return 0;
        if (!strcmp(k, "--ring-create")) { o->cfg.ring_create = 1; continue; }
        if (!v) { fprintf(stderr, "missing value for %s\n", k); return 0; }
        i++;
        if (!strcmp(k, "--nodes")) o->cfg.nodes = (uint16_t)atoi(v);
        else if (!strcmp(k, "--topology")) o->topology = v;
        else if (!strcmp(k, "--slots")) o->cfg.slots_per_frame = (uint16_t)atoi(v);
        else if (!strcmp(k, "--frames")) o->cfg.frames_per_superframe = (uint16_t)atoi(v);
        else if (!strcmp(k, "--slot-ms")) o->cfg.slot_ms = (uint16_t)atoi(v);
        else if (!strcmp(k, "--capacity")) o->cfg.capacity = (uint16_t)atoi(v);
        else if (!strcmp(k, "--ring-slots")) o->cfg.ring_slots = (uint16_t)atoi(v);
        else if (!strcmp(k, "--lead")) o->cfg.lead_frames = (uint16_t)atoi(v);
        else if (!strcmp(k, "--ring-path")) o->cfg.ring_path_fmt = v;
        else if (!strcmp(k, "--loss")) o->cfg.loss_permille = (uint32_t)atoi(v);
        else if (!strcmp(k, "--seed")) o->cfg.seed = (uint32_t)atoi(v);
        else if (!strcmp(k, "--netns-prefix")) o->prefix = v;
        else if (!strcmp(k, "--ifname")) o->ifname = v;
        else if (!strcmp(k, "--control")) o->control_path = v;
        else if (!strcmp(k, "--duration")) o->duration_s = (unsigned)atoi(v);
        else if (!strcmp(k, "--pcap")) o->pcap_path = v;
        else if (!strcmp(k, "--summary")) o->summary_path = v;
        else { fprintf(stderr, "unknown option %s\n", k); return 0; }
    }
    return 1;
}

/* Returns the monotonic time in ns. */
static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/* Stops the main loop on SIGINT or SIGTERM. */
static void on_signal(int sig) { (void)sig; g_stop = 1; }

/* Creates a non-blocking TAP interface inside the namespace of node idx (0-based). */
static int open_tap(const opts_t *o, unsigned idx, int home_ns)
{
    char path[128];
    struct ifreq ifr;
    int ns, fd = -1;

    snprintf(path, sizeof path, "/var/run/netns/%s%u", o->prefix, idx + 1);
    ns = open(path, O_RDONLY);
    if (ns < 0) { fprintf(stderr, "%s: %s (create it with: ip netns add %s%u)\n", path, strerror(errno), o->prefix, idx + 1); return -1; }
    if (setns(ns, CLONE_NEWNET) < 0) {
        fprintf(stderr, "setns %s: %s (needs CAP_SYS_ADMIN, e.g. a privileged container)\n", path, strerror(errno));
        close(ns);
        return -1;
    }
    close(ns);
    fd = open("/dev/net/tun", O_RDWR);
    if (fd < 0) fprintf(stderr, "/dev/net/tun: %s\n", strerror(errno));
    else {
        memset(&ifr, 0, sizeof ifr);
        ifr.ifr_flags = IFF_TAP | IFF_NO_PI;
        snprintf(ifr.ifr_name, IFNAMSIZ, "%s", o->ifname);
        if (ioctl(fd, TUNSETIFF, &ifr) < 0 || fcntl(fd, F_SETFL, O_NONBLOCK) < 0) {
            fprintf(stderr, "%s in %s%u: %s\n", o->ifname, o->prefix, idx + 1, strerror(errno));
            close(fd);
            fd = -1;
        }
    }
    if (setns(home_ns, CLONE_NEWNET) < 0) { perror("setns back"); if (fd >= 0) close(fd); return -1; }
    return fd;
}

/* Sends a frame read from node idx's TAP into that node's MAC, and counts the outcome. */
static void from_tap(unsigned idx, const uint8_t *frame, size_t len)
{
    port_t *p = &g_port[idx];
    br_tx_t t;
    br_result_t r = br_from_eth(frame, len, &t);

    p->tap_rx++;
    if (r != BR_OK) { p->dropped[r]++; return; }
    switch (rn_send(g_net, idx + 1, t.kind, t.cls, t.dst, t.ip, t.ip_len)) {
    case HH_MAC_ACCEPTED:  p->mac_accepted++; break;
    case HH_MAC_TRY_AGAIN: p->mac_queue_full++; break;
    case HH_MAC_TOO_LARGE: p->mac_too_large++; break;
    default:               p->mac_other++; break;
    }
}

/* Writes a packet the MAC delivered to node's TAP, with its Ethernet header rebuilt. */
static void to_tap(void *ctx, unsigned node, const hh_mac_rx_frame_t *f)
{
    uint8_t buf[MAX_FRAME];
    port_t *p = &g_port[node - 1];
    size_t n;
    (void)ctx;

    n = br_to_eth(buf, sizeof buf, node, f->src, f->dst, f->payload, f->len);
    if (!n) { p->write_errors++; return; }
    if (write(p->fd, buf, n) == (ssize_t)n) p->delivered++;
    else p->write_errors++;
}

/* Runs one control line: "down A B" or "up A B" changes whether two nodes hear each other. */
static void control_line(const char *line)
{
    char cmd[16];
    unsigned a, b;
    if (sscanf(line, "%15s %u %u", cmd, &a, &b) != 3) return;
    if (!strcmp(cmd, "down")) rn_link(g_net, a, b, false);
    else if (!strcmp(cmd, "up")) rn_link(g_net, a, b, true);
}

/* Reads available control lines from the FIFO. */
static void read_control(int fd)
{
    static char acc[256];
    static size_t used;
    ssize_t n = read(fd, acc + used, sizeof acc - used - 1);
    char *nl;

    if (n <= 0) return;
    used += (size_t)n;
    acc[used] = 0;
    while ((nl = strchr(acc, '\n'))) {
        *nl = 0;
        control_line(acc);
        used -= (size_t)(nl + 1 - acc);
        memmove(acc, nl + 1, used + 1);
    }
    if (used >= sizeof acc - 1) used = 0;
}

/* Prints the parameters, per-node counters and the ring and air counters. */
static void print_summary(FILE *o, const opts_t *op, uint64_t run_ms)
{
    hh_mac_status_t st;
    unsigned max_payload = 0;
    rn_stats_t rs;

    if (hh_mac_status(rn_mac(g_net, 1), &st) == HH_MAC_OK) max_payload = st.max_payload;
    rn_stats(g_net, &rs);
    fprintf(o, "== parameters ==\n");
    fprintf(o, "mode=real-time path=rings nodes=%u topology=%s slots_per_frame=%u frames_per_superframe=%u slot_ms=%u run_ms=%llu\n",
            op->cfg.nodes, op->topology, op->cfg.slots_per_frame, op->cfg.frames_per_superframe,
            op->cfg.slot_ms, (unsigned long long)run_ms);
    fprintf(o, "capacity_bytes=%u capacity_is_placeholder=yes max_packet_bytes=%u fragmentation=excluded ring_slots=%u lead_frames=%u\n",
            op->cfg.capacity, max_payload, op->cfg.ring_slots, op->cfg.lead_frames);
    fprintf(o, "routing=real (processes on the TAP interfaces) link_loss_permille=%u seed=%u rings=%s\n",
            op->cfg.loss_permille, op->cfg.seed, op->cfg.ring_path_fmt ? op->cfg.ring_path_fmt : "memory");
    fprintf(o, "== nodes ==\n");
    for (unsigned i = 0; i < op->cfg.nodes; i++) {
        const port_t *p = &g_port[i];
        const hh_slot_stats_t *s = rn_slot_stats(g_net, i + 1);
        fprintf(o, "node=%u tap_rx=%llu dropped_short=%llu dropped_not_ipv4=%llu dropped_bad_mac=%llu "
                   "mac_accepted=%llu mac_queue_full=%llu mac_too_large=%llu mac_other=%llu "
                   "delivered_to_tap=%llu tap_write_errors=%llu own_slots=%llu own_slots_missed=%llu "
                   "rx_pdus=%llu tx_failed=%llu\n",
                i + 1, (unsigned long long)p->tap_rx, (unsigned long long)p->dropped[BR_DROP_SHORT],
                (unsigned long long)p->dropped[BR_DROP_NOT_IPV4], (unsigned long long)p->dropped[BR_DROP_BAD_MAC],
                (unsigned long long)p->mac_accepted, (unsigned long long)p->mac_queue_full,
                (unsigned long long)p->mac_too_large, (unsigned long long)p->mac_other,
                (unsigned long long)p->delivered, (unsigned long long)p->write_errors,
                (unsigned long long)s->own_slots, (unsigned long long)s->own_slots_missed,
                (unsigned long long)s->rx_pdus, (unsigned long long)s->tx_failed);
    }
    fprintf(o, "== rings and air ==\n");
    fprintf(o, "pdus_on_air=%llu control_only_slots=%llu collision_events=%llu channel_events=%llu "
               "tx_ring_full=%llu rx_ring_full=%llu pull_stale=%llu rx_frames_to_mac=%llu\n",
            (unsigned long long)rs.pdus_on_air, (unsigned long long)rs.control_only,
            (unsigned long long)rs.collision_events, (unsigned long long)rs.channel_events,
            (unsigned long long)rs.tx_ring_full, (unsigned long long)rs.rx_ring_full,
            (unsigned long long)rs.pull_stale, (unsigned long long)rs.rx_to_mac);
}

/* Creates the TAPs, runs the slots in real time until stopped, then prints the summary. */
int main(int argc, char **argv)
{
    opts_t op;
    int home_ns, control_fd = -1;
    FILE *pcap = NULL, *summary = NULL;
    struct pollfd pf[RN_MAX_NODES + 1];
    uint64_t start, next, slot_ns;
    unsigned nfds;

    if (!parse(argc, argv, &op)) { usage(); return 2; }
    if (op.pcap_path) {
        if (!(pcap = fopen(op.pcap_path, "wb"))) { perror(op.pcap_path); return 1; }
        sn_pcap_header(pcap);
        op.cfg.pdu_hook = sn_pcap_hook;
        op.cfg.hook_ctx = pcap;
    }
    op.cfg.rx_hook = to_tap;
    g_net = rn_create(&op.cfg);
    if (!g_net) { fputs("invalid configuration: nodes 2 to 32, nodes <= slots per frame <= 64, ring slots a power of two, ring files present and big enough\n", stderr); return 2; }
    if (!strcmp(op.topology, "line")) rn_topology_line(g_net);
    else if (!strcmp(op.topology, "ring")) rn_topology_ring(g_net);
    else if (!strcmp(op.topology, "mesh")) rn_topology_mesh(g_net);
    else { fprintf(stderr, "unknown topology %s\n", op.topology); return 2; }

    home_ns = open("/proc/self/ns/net", O_RDONLY);
    if (home_ns < 0) { perror("/proc/self/ns/net"); return 1; }
    for (unsigned i = 0; i < op.cfg.nodes; i++) {
        g_port[i].fd = open_tap(&op, i, home_ns);
        if (g_port[i].fd < 0) return 1;
        pf[i].fd = g_port[i].fd;
        pf[i].events = POLLIN;
    }
    nfds = op.cfg.nodes;
    if (op.control_path) {
        control_fd = open(op.control_path, O_RDWR | O_NONBLOCK);
        if (control_fd < 0) { perror(op.control_path); return 1; }
        pf[nfds].fd = control_fd;
        pf[nfds].events = POLLIN;
        nfds++;
    }
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    {
        hh_mac_status_t st;
        unsigned max_payload = hh_mac_status(rn_mac(g_net, 1), &st) == HH_MAC_OK ? st.max_payload : 0;
        struct timespec wall;
        clock_gettime(CLOCK_REALTIME, &wall);
        fprintf(stderr, "hh-ring-bridge: %u nodes up, max packet %u bytes (set the TAP MTU to at most this), unix_ms=%lld\n",
                op.cfg.nodes, max_payload, (long long)wall.tv_sec * 1000 + wall.tv_nsec / 1000000);
    }

    slot_ns = (uint64_t)op.cfg.slot_ms * 1000000ull;
    start = now_ns();
    next = start;
    while (!g_stop) {
        uint64_t t = now_ns();
        if (t >= next) {
            rn_set_time(g_net, (uint32_t)((t - start) / 1000000ull));
            rn_step(g_net);
            next += slot_ns;
            if (next <= t) next = t + slot_ns;   /* fell behind: do not try to catch up the wall clock */
            if (op.duration_s && t - start >= (uint64_t)op.duration_s * 1000000000ull) break;
        } else {
            struct timespec to = { (time_t)((next - t) / 1000000000ull), (long)((next - t) % 1000000000ull) };
            if (ppoll(pf, nfds, &to, NULL) > 0) {
                for (unsigned i = 0; i < op.cfg.nodes; i++) {
                    uint8_t buf[MAX_FRAME];
                    ssize_t n;
                    for (int k = 0; k < 64 && (pf[i].revents & POLLIN) && (n = read(pf[i].fd, buf, sizeof buf)) > 0; k++)
                        from_tap(i, buf, (size_t)n);
                }
                if (control_fd >= 0 && (pf[op.cfg.nodes].revents & POLLIN)) read_control(control_fd);
            }
        }
    }

    print_summary(stdout, &op, (now_ns() - start) / 1000000ull);
    if (op.summary_path && (summary = fopen(op.summary_path, "w"))) {
        print_summary(summary, &op, (now_ns() - start) / 1000000ull);
        fclose(summary);
    }
    rn_destroy(g_net);
    if (pcap) fclose(pcap);
    return 0;
}
