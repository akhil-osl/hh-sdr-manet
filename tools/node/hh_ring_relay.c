/*
 * hh-ring-relay: owns two ring files and copies the TX ring of each into the RX
 * ring of the other, so two hh-node processes on one PC can talk without a radio.
 * With --create it makes the files, as radiod does on a board; without it the
 * files must exist. Development tooling only.
 *
 *   hh-ring-relay --a /tmp/ring_a --b /tmp/ring_b --create --duration 60
 */
#define _GNU_SOURCE
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "hhsdr/ring/shm.h"
#include "ring_relay.h"

static volatile sig_atomic_t g_stop;

/* Stops the main loop on SIGINT or SIGTERM. */
static void on_signal(int sig) { (void)sig; g_stop = 1; }

/* Prints the usage text. */
static void usage(void)
{
    fputs("usage: hh-ring-relay --a FILE --b FILE [--create] [--slots N] [--slot-size N] [--duration S]\n"
          "  --a, --b       the two ring files\n"
          "  --create       create the files here (default: they exist)\n"
          "  --slots N      places in each ring, a power of two (default 64)\n"
          "  --slot-size N  bytes per place, a multiple of 32 (default 288)\n"
          "  --duration S   stop after S seconds (default 0 = until SIGINT or SIGTERM)\n", stderr);
}

/* Opens or creates one ring file. Returns 0 on success. */
static int attach(hh_ring_shm_t *h, hh_ring_region_t *r, const char *path, int create,
                  uint32_t slots, uint32_t slot_size)
{
    hh_status_t st = create ? hh_ring_shm_create(h, r, path, slots, slots, slot_size)
                            : hh_ring_shm_open(h, r, path);
    if (st != HH_OK) fprintf(stderr, "%s: ring file not %s (status %d)\n", path, create ? "created" : "opened", (int)st);
    return st == HH_OK ? 0 : -1;
}

/* Runs the relay until stopped, then prints the counters. */
int main(int argc, char **argv)
{
    const char *pa = NULL, *pb = NULL;
    int create = 0;
    uint32_t slots = 64, slot_size = 288, duration = 0;
    hh_ring_shm_t ha, hb;
    hh_ring_region_t ra, rb;
    relay_dir_t ab = {0}, ba = {0};
    time_t start;

    memset(&ha, 0, sizeof ha);
    memset(&hb, 0, sizeof hb);
    for (int i = 1; i < argc; i++) {
        const char *k = argv[i];
        if (!strcmp(k, "--create")) { create = 1; continue; }
        if (!strcmp(k, "-h") || !strcmp(k, "--help") || i + 1 >= argc) { usage(); return 2; }
        if (!strcmp(k, "--a")) pa = argv[++i];
        else if (!strcmp(k, "--b")) pb = argv[++i];
        else if (!strcmp(k, "--slots")) slots = (uint32_t)atoi(argv[++i]);
        else if (!strcmp(k, "--slot-size")) slot_size = (uint32_t)atoi(argv[++i]);
        else if (!strcmp(k, "--duration")) duration = (uint32_t)atoi(argv[++i]);
        else { usage(); return 2; }
    }
    if (!pa || !pb) { usage(); return 2; }
    if (attach(&ha, &ra, pa, create, slots, slot_size) || attach(&hb, &rb, pb, create, slots, slot_size)) {
        hh_ring_shm_close(&ha);
        hh_ring_shm_close(&hb);
        return 1;
    }
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    fprintf(stderr, "hh-ring-relay: %s <-> %s, %u places of %u bytes\n", pa, pb, ra.tx.count, ra.tx.slot_size);
    start = time(NULL);
    while (!g_stop && !(duration && (uint32_t)(time(NULL) - start) >= duration)) {
        relay_move(&ra, &rb, &ab, 16);
        relay_move(&rb, &ra, &ba, 16);
        usleep(200);
    }
    printf("a_to_b copied=%llu rx_full=%llu too_big=%llu\n", (unsigned long long)ab.copied,
           (unsigned long long)ab.rx_full, (unsigned long long)ab.too_big);
    printf("b_to_a copied=%llu rx_full=%llu too_big=%llu\n", (unsigned long long)ba.copied,
           (unsigned long long)ba.rx_full, (unsigned long long)ba.too_big);
    hh_ring_shm_close(&ha);
    hh_ring_shm_close(&hb);
    return 0;
}
