/*
 * test_shm_ring: the shared-memory RX frame ring across two processes.
 * The parent plays radiod (creates, consumes); a forked child plays
 * rh_agent (attaches, produces 5000 frames). Checks order, content, sizes,
 * full-ring refusal, attach errors and cleanup.
 */
#include "hhsdr/radiod/shm_ring.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static int g_fail;
#define CHECK(c) do { if (!(c)) { g_fail++; printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); } } while (0)

#define N_FRAMES 5000u

static void fill(uint8_t *f, uint32_t seq, uint32_t len)
{
    for (uint32_t i = 0; i < len; i++) f[i] = (uint8_t)(seq * 7u + i);
    memcpy(f, &seq, sizeof seq);
}

int main(void)
{
    char path[64];
    hh_shm_ring_t owner, other;
    uint8_t f[HH_RX_FRAME_RING_SLOT], g[HH_RX_FRAME_RING_SLOT];
    uint32_t n = 0;
    pid_t pid;

    snprintf(path, sizeof path, "/tmp/test_shm_ring.%ld", (long)getpid());

    CHECK(hh_shm_ring_attach(&other, path) == HH_ERR_NOTFOUND);
    CHECK(hh_shm_ring_create(&owner, path, HH_RX_FRAME_RING_SLOT, 60u) == HH_ERR_INVAL);
    CHECK(hh_shm_ring_create(&owner, path, HH_RX_FRAME_RING_SLOT, HH_RX_FRAME_RING_COUNT) == HH_OK);
    CHECK(hh_shm_ring_count(&owner) == 64u && hh_shm_ring_slot_size(&owner) == 280u);
    CHECK(hh_shm_ring_pop(&owner, g, sizeof g, &n) == HH_ERR_AGAIN);

    /* full ring in one process: 64 accepted, the 65th refused and counted */
    for (uint32_t i = 0; i < 64u; i++) { fill(f, i, 280u); CHECK(hh_shm_ring_push(&owner, f, 280u) == HH_OK); }
    CHECK(hh_shm_ring_push(&owner, f, 280u) == HH_ERR_NOMEM && hh_shm_ring_drops(&owner) == 1u);
    CHECK(hh_shm_ring_push(&owner, f, 281u) == HH_ERR_INVAL);
    for (uint32_t i = 0; i < 64u; i++) CHECK(hh_shm_ring_pop(&owner, g, sizeof g, &n) == HH_OK && n == 280u);
    CHECK(hh_shm_ring_used(&owner) == 0u);

    /* two processes: child = producer (rh_agent), parent = consumer (radiod) */
    pid = fork();
    if (pid == 0) {
        hh_shm_ring_t a;
        if (hh_shm_ring_attach(&a, path) != HH_OK) _exit(2);
        for (uint32_t i = 0; i < N_FRAMES; ) {
            const uint32_t len = (i % 3u == 0u) ? 208u : 280u;   /* both PL frame sizes */
            fill(f, i, len);
            if (hh_shm_ring_push(&a, f, len) == HH_OK) i++;
            else usleep(50);
        }
        hh_shm_ring_close(&a);                                /* not owner: file stays */
        _exit(0);
    }
    {
        uint32_t got = 0, bad = 0, spins = 0;
        while (got < N_FRAMES && spins < 2000000u) {
            if (hh_shm_ring_pop(&owner, g, sizeof g, &n) != HH_OK) { spins++; usleep(10); continue; }
            const uint32_t want = (got % 3u == 0u) ? 208u : 280u;
            fill(f, got, want);
            if (n != want || memcmp(f, g, n) != 0) bad++;
            got++;
        }
        int st = 0;
        waitpid(pid, &st, 0);
        CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0);
        CHECK(got == N_FRAMES && bad == 0u);
        printf("two processes: %u frames (208 / 280 B) in order, %u bad\n", got, bad);
    }

    /* a non-ring file is refused */
    {
        char p2[80];
        FILE *fp;
        snprintf(p2, sizeof p2, "%s.junk", path);
        fp = fopen(p2, "w");
        if (fp) { fputs("not a ring file, definitely not a ring file at all, no", fp); fclose(fp); }
        CHECK(hh_shm_ring_attach(&other, p2) == HH_ERR_INVAL);
        unlink(p2);
    }

    hh_shm_ring_close(&owner);
    CHECK(access(path, F_OK) != 0);                        /* owner removed it */
    printf("%s (%d failure(s))\n", g_fail ? "FAIL" : "PASS", g_fail);
    return g_fail ? 1 : 0;
}
