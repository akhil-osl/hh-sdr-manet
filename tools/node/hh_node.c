/*
 * hh-node: the MAC process of one node. It opens a TAP interface, runs the slot
 * scheduler in real time, and moves PDUs through the frame rings. On a board
 * radiod owns the ring file and rh_agent moves frames between the rings and the
 * radio. On a PC hh-ring-relay stands in for both.
 *
 * Ethernet frames that Linux writes to the TAP lose their Ethernet header and
 * enter the MAC. Packets from the MAC get an Ethernet header and go to the TAP.
 * The next hop is the destination link address 02-00-00-00-hh-ll (frame
 * specification v2 section 4.5). The program sets no address and no MTU: the
 * start script does, with the maximum packet size printed at start.
 *
 * Development tooling: it reuses the conversion code of the simulator bridge.
 * Slot capacity and the slot plan are placeholders.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/if_tun.h>
#include <net/if.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#include "hhsdr/core/clock.h"
#include "hhsdr/core/log.h"
#include "hhsdr/mac/mac.h"
#include "hhsdr/mac/slot.h"
#include "hhsdr/ring/mac_driver.h"
#include "hhsdr/ring/shm.h"
#include "slot_bridge_frames.h"

#define MAX_FRAME 2048u

typedef struct {
    uint16_t    node, net, slots, frames, slot_ms, capacity, lead, width;
    uint64_t    own_slots;
    int         own_given;
    const char *rings, *ifname, *summary;
    unsigned    duration_s;
} opts_t;

typedef struct {
    int      fd;
    uint16_t id;
    uint64_t tap_rx, dropped[4], accepted, queue_full, too_large, other, delivered, write_errors;
} port_t;

static volatile sig_atomic_t g_stop;
static port_t g_port;

/* Stops the main loop on SIGINT or SIGTERM. */
static void on_signal(int sig) { (void)sig; g_stop = 1; }

/* Prints the usage text. */
static void usage(void)
{
    fputs("usage: hh-node --node ID --rings FILE [options]   (needs CAP_NET_ADMIN to create the TAP)\n"
          "  --node ID        on-air address, 1 to 61439\n"
          "  --rings FILE     the ring file made by radiod or hh-ring-relay\n"
          "  --net N          net id (default 1)\n"
          "  --own-slots HEX  slot bitmap, bit n = own slot n (default: bit ID-1)\n"
          "  --slots N        slots per frame (default 20)\n"
          "  --frames N       frames per superframe (default 50)\n"
          "  --slot-ms N      slot length in ms (default 1)\n"
          "  --capacity N     PDU bytes per slot, a placeholder (default 128)\n"
          "  --width N        fixed ring frame width in bytes (204 on the boards, FEC rate 3/4): pads each PDU\n"
          "                   and carries its length in the last 2 bytes; sets --capacity to N - 2 (default 0 = off)\n"
          "  --lead N         TDMA frames between building a PDU and sending it (default 1)\n"
          "  --ifname NAME    TAP name (default manet0)\n"
          "  --duration S     stop after S seconds (default 0 = until SIGINT or SIGTERM)\n"
          "  --summary FILE   write the summary here as well\n", stderr);
}

/* Reads the command line. Returns 0 if it is incomplete or unknown. */
static int parse(int argc, char **argv, opts_t *o)
{
    memset(o, 0, sizeof *o);
    o->net = 1; o->slots = 20; o->frames = 50; o->slot_ms = 1; o->capacity = 128; o->lead = 1;
    o->ifname = "manet0";
    for (int i = 1; i < argc; i += 2) {
        const char *k = argv[i], *v = i + 1 < argc ? argv[i + 1] : NULL;
        if (!v) return 0;
        if (!strcmp(k, "--node")) o->node = (uint16_t)atoi(v);
        else if (!strcmp(k, "--rings")) o->rings = v;
        else if (!strcmp(k, "--net")) o->net = (uint16_t)atoi(v);
        else if (!strcmp(k, "--own-slots")) { o->own_slots = strtoull(v, NULL, 16); o->own_given = 1; }
        else if (!strcmp(k, "--slots")) o->slots = (uint16_t)atoi(v);
        else if (!strcmp(k, "--frames")) o->frames = (uint16_t)atoi(v);
        else if (!strcmp(k, "--slot-ms")) o->slot_ms = (uint16_t)atoi(v);
        else if (!strcmp(k, "--capacity")) o->capacity = (uint16_t)atoi(v);
        else if (!strcmp(k, "--width")) o->width = (uint16_t)atoi(v);
        else if (!strcmp(k, "--lead")) o->lead = (uint16_t)atoi(v);
        else if (!strcmp(k, "--ifname")) o->ifname = v;
        else if (!strcmp(k, "--duration")) o->duration_s = (unsigned)atoi(v);
        else if (!strcmp(k, "--summary")) o->summary = v;
        else return 0;
    }
    if (!o->node || o->node > 0xEFFFu || !o->rings) return 0;
    if (!o->own_given) o->own_slots = 1ull << ((o->node - 1u) % 64u);
    return 1;
}

/* Returns the monotonic time in nanoseconds. */
static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/* Creates a non-blocking TAP interface. Returns its file descriptor, or -1. */
static int open_tap(const char *name)
{
    struct ifreq ifr;
    int fd = open("/dev/net/tun", O_RDWR);

    if (fd < 0) { fprintf(stderr, "/dev/net/tun: %s\n", strerror(errno)); return -1; }
    memset(&ifr, 0, sizeof ifr);
    ifr.ifr_flags = IFF_TAP | IFF_NO_PI;
    snprintf(ifr.ifr_name, IFNAMSIZ, "%s", name);
    if (ioctl(fd, TUNSETIFF, &ifr) < 0 || fcntl(fd, F_SETFL, O_NONBLOCK) < 0) {
        fprintf(stderr, "%s: %s\n", name, strerror(errno));
        close(fd);
        return -1;
    }
    return fd;
}

/* Sends a frame read from the TAP into the MAC, and counts the outcome. */
static void from_tap(hh_mac_t *mac, const uint8_t *frame, size_t len)
{
    br_tx_t t;
    br_result_t r = br_from_eth(frame, len, &t);

    g_port.tap_rx++;
    if (r != BR_OK) { g_port.dropped[r]++; return; }
    switch (hh_mac_send(mac, t.kind, t.cls, t.dst, 0, t.ip, t.ip_len)) {
    case HH_MAC_ACCEPTED:  g_port.accepted++; break;
    case HH_MAC_TRY_AGAIN: g_port.queue_full++; break;
    case HH_MAC_TOO_LARGE: g_port.too_large++; break;
    default:               g_port.other++; break;
    }
}

/* MAC receive callback: writes the packet to the TAP with its Ethernet header rebuilt. */
static bool on_mac_rx(void *ctx, const hh_mac_indication_t *ind)
{
    uint8_t buf[MAX_FRAME];
    const hh_mac_rx_frame_t *f = ind->frame;
    size_t n;
    (void)ctx;

    if (ind->type != HH_MAC_IND_FRAME) return true;
    n = br_to_eth(buf, sizeof buf, g_port.id, f->src, f->dst, f->payload, f->len);
    if (!n) { g_port.write_errors++; return true; }
    if (write(g_port.fd, buf, n) == (ssize_t)n) g_port.delivered++;
    else g_port.write_errors++;
    return true;
}

/* Prints the parameters and the counters. */
static void print_summary(FILE *o, const opts_t *op, const hh_slot_t *sl, const hh_ring_mac_t *drv,
                          unsigned max_payload, uint64_t run_ms)
{
    fprintf(o, "node=%u net=%u rings=%s own_slots=0x%llx slots_per_frame=%u frames_per_superframe=%u slot_ms=%u "
               "capacity_bytes=%u capacity_is_placeholder=yes max_packet_bytes=%u lead_frames=%u ring_width=%u run_ms=%llu\n",
            op->node, op->net, op->rings, (unsigned long long)op->own_slots, op->slots, op->frames,
            op->slot_ms, op->capacity, max_payload, op->lead, op->width, (unsigned long long)run_ms);
    fprintf(o, "tap_rx=%llu dropped_short=%llu dropped_not_ipv4=%llu dropped_bad_mac=%llu mac_accepted=%llu "
               "mac_queue_full=%llu mac_too_large=%llu mac_other=%llu delivered_to_tap=%llu tap_write_errors=%llu\n",
            (unsigned long long)g_port.tap_rx, (unsigned long long)g_port.dropped[BR_DROP_SHORT],
            (unsigned long long)g_port.dropped[BR_DROP_NOT_IPV4], (unsigned long long)g_port.dropped[BR_DROP_BAD_MAC],
            (unsigned long long)g_port.accepted, (unsigned long long)g_port.queue_full,
            (unsigned long long)g_port.too_large, (unsigned long long)g_port.other,
            (unsigned long long)g_port.delivered, (unsigned long long)g_port.write_errors);
    fprintf(o, "own_slots_used=%llu own_slots_missed=%llu rx_pdus=%llu tx_failed=%llu ring_tx_frames=%llu "
               "ring_tx_full=%llu ring_tx_too_big=%llu ring_rx_frames=%llu ring_rx_crc_bad=%llu ring_rx_bad_descriptor=%llu ring_rx_bad_width=%llu\n",
            (unsigned long long)sl->st.own_slots, (unsigned long long)sl->st.own_slots_missed,
            (unsigned long long)sl->st.rx_pdus, (unsigned long long)sl->st.tx_failed,
            (unsigned long long)drv->st.tx_frames, (unsigned long long)drv->st.tx_ring_full,
            (unsigned long long)drv->st.tx_too_big, (unsigned long long)drv->st.rx_frames,
            (unsigned long long)drv->st.rx_crc_bad, (unsigned long long)drv->st.rx_bad_descriptor,
            (unsigned long long)drv->st.rx_bad_width);
}

/* Starts the MAC, the scheduler and the ring driver, then runs until stopped. */
int main(int argc, char **argv)
{
    opts_t op;
    hh_ring_shm_t shm;
    hh_ring_region_t region;
    hh_mac_config_t mc;
    hh_slot_config_t sc;
    hh_mac_t mac;
    hh_slot_t sl;
    hh_ring_mac_t drv;
    hh_mac_status_t st;
    const hh_clock_t *clk = hh_clock_monotonic();
    struct pollfd pf;
    uint64_t start, next, slot_ns;
    unsigned max_payload = 0;
    FILE *summary;

    if (!parse(argc, argv, &op)) { usage(); return 2; }
    if (op.width) {
        if (op.width < 4) { fputs("--width must be at least 4\n", stderr); return 2; }
        op.capacity = (uint16_t)(op.width - 2);
    }
    hh_log_set_level(HH_LOG_ERROR);
    memset(&shm, 0, sizeof shm);
    if (hh_ring_shm_open(&shm, &region, op.rings) != HH_OK) {
        fprintf(stderr, "%s: ring file not opened\n", op.rings);
        return 1;
    }
    if (region.tx.slot_size < op.capacity || region.rx.slot_size < op.capacity) {
        fprintf(stderr, "ring places are %u bytes, smaller than --capacity %u\n", region.tx.slot_size, op.capacity);
        hh_ring_shm_close(&shm);
        return 1;
    }
    hh_mac_config_default(&mc, op.node, clk);
    mc.receive = on_mac_rx;
    mc.receive_ctx = NULL;
    mc.report_period_sf = 0;
    if (hh_mac_init(&mac, &mc) != HH_MAC_OK || hh_mac_open(&mac) != HH_MAC_OK) { fputs("MAC start failed\n", stderr); return 1; }
    hh_mac_set_state(&mac, HH_MAC_STEADY);
    hh_slot_config_default(&sc, op.node, op.net);
    sc.slots_per_frame = op.slots;
    sc.frames_per_superframe = op.frames;
    sc.slot_ms = op.slot_ms;
    sc.capacity = op.capacity;
    sc.own_slots = op.own_slots;
    sc.logic_adds_crc = true;          /* the PL appends and checks the CRC-32 */
    sc.epoch_ms = hh_now(clk);
    if (hh_slot_init(&sl, &sc, clk, &mac, hh_ring_mac_tx, &drv) != HH_OK ||
        hh_ring_mac_init(&drv, &region, &sl, op.lead) != HH_OK ||
        hh_ring_mac_set_width(&drv, op.width) != HH_OK) { fputs("scheduler start failed\n", stderr); return 1; }

    g_port.id = op.node;
    g_port.fd = open_tap(op.ifname);
    if (g_port.fd < 0) { hh_ring_shm_close(&shm); return 1; }
    pf.fd = g_port.fd;
    pf.events = POLLIN;
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    if (hh_mac_status(&mac, &st) == HH_MAC_OK) max_payload = st.max_payload;
    fprintf(stderr, "hh-node: node %u up on %s, max packet %u bytes (set the MTU to at most this)\n",
            op.node, op.ifname, max_payload);

    slot_ns = (uint64_t)op.slot_ms * 1000000ull;
    start = now_ns();
    next = start;
    while (!g_stop) {
        uint64_t t = now_ns();
        if (t >= next) {
            hh_ring_mac_poll(&drv);
            hh_slot_poll(&sl);
            next += slot_ns;
            if (next <= t) next = t + slot_ns;   /* fell behind: do not try to catch up */
            if (op.duration_s && t - start >= (uint64_t)op.duration_s * 1000000000ull) break;
        } else {
            struct timespec to = { (time_t)((next - t) / 1000000000ull), (long)((next - t) % 1000000000ull) };
            if (ppoll(&pf, 1, &to, NULL) > 0 && (pf.revents & POLLIN)) {
                uint8_t buf[MAX_FRAME];
                ssize_t n;
                for (int k = 0; k < 64 && (n = read(g_port.fd, buf, sizeof buf)) > 0; k++)
                    from_tap(&mac, buf, (size_t)n);
            }
        }
    }
    print_summary(stdout, &op, &sl, &drv, max_payload, (now_ns() - start) / 1000000ull);
    if (op.summary && (summary = fopen(op.summary, "w"))) {
        print_summary(summary, &op, &sl, &drv, max_payload, (now_ns() - start) / 1000000ull);
        fclose(summary);
    }
    close(g_port.fd);
    hh_ring_shm_close(&shm);
    return 0;
}
