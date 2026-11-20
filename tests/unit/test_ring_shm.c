/*
 * Shared-memory file for the rings: the owner creates it, another handle opens
 * it by path, and both work on the same memory. Frames go through the ring
 * interface of ring.h; the owner is not in the path.
 */
#include "hhsdr/ring/shm.h"
#include "hh_test.h"
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* A unique path in /tmp for this process. */
static const char *path(void)
{
    static char p[64];
    snprintf(p, sizeof p, "/tmp/hh-ring-shm-%d", (int)getpid());
    return p;
}

/* True if a file exists at p. */
static int exists(const char *p)
{
    struct stat sb;
    return stat(p, &sb) == 0;
}

/* A frame written through one mapping is read through the other, in both rings. */
static void test_two_processes_share_memory(void)
{
    hh_ring_shm_t own, cli;
    hh_ring_region_t ro, rc;
    hh_ring_slot_t s;
    const uint8_t msg[5] = { 1, 2, 3, 4, 5 };

    HH_ASSERT_OK(hh_ring_shm_create(&own, &ro, path(), 8, 8, 288));
    HH_ASSERT_OK(hh_ring_shm_open(&cli, &rc, path()));
    HH_ASSERT(ro.rx.slots != rc.rx.slots);                   /* two separate mappings */
    HH_ASSERT_EQ_INT(rc.rx.count, 8);
    HH_ASSERT_EQ_INT(rc.tx.slot_size, 288);

    /* RX: written on one side, read on the other. */
    HH_ASSERT_OK(hh_ring_produce_begin(&ro.rx, &s));
    memcpy(s.frame, msg, sizeof msg);
    hh_ring_produce_commit(&ro.rx, &s, 0);
    HH_ASSERT_OK(hh_ring_consume_peek(&rc.rx, &s));
    HH_ASSERT(memcmp(s.frame, msg, sizeof msg) == 0);
    hh_ring_consume_release(&rc.rx, &s, 0);

    /* TX: the other direction. */
    HH_ASSERT_OK(hh_ring_produce_begin(&rc.tx, &s));
    memcpy(s.frame, msg, sizeof msg);
    hh_ring_produce_commit(&rc.tx, &s, 0);
    HH_ASSERT_OK(hh_ring_consume_peek(&ro.tx, &s));
    HH_ASSERT(memcmp(s.frame, msg, sizeof msg) == 0);
    hh_ring_consume_release(&ro.tx, &s, 0);

    hh_ring_shm_close(&cli);
    HH_ASSERT(exists(path()));                               /* a client does not remove it */
    hh_ring_shm_close(&own);
    HH_ASSERT(!exists(path()));                              /* the owner does */
}

/* A stale file from an earlier run is replaced by a fresh layout. */
static void test_stale_file_replaced(void)
{
    hh_ring_shm_t a, b;
    hh_ring_region_t ra, rb;
    hh_ring_slot_t s;

    HH_ASSERT_OK(hh_ring_shm_create(&a, &ra, path(), 8, 8, 288));
    HH_ASSERT_OK(hh_ring_produce_begin(&ra.rx, &s));
    hh_ring_produce_commit(&ra.rx, &s, 0);
    a.owner = 0;                                             /* the owner died without cleaning up */
    hh_ring_shm_close(&a);
    HH_ASSERT(exists(path()));

    HH_ASSERT_OK(hh_ring_shm_create(&b, &rb, path(), 16, 4, 320));
    HH_ASSERT_EQ_INT(rb.rx.count, 16);
    HH_ASSERT_EQ_INT(rb.tx.count, 4);
    HH_ASSERT_EQ_INT(hh_ring_view_used(&rb.rx), 0);          /* empty, not the old frame */
    hh_ring_shm_close(&b);
}

/* Bad geometry, bad paths, a missing file and a file that is not a ring are refused. */
static void test_refusals(void)
{
    hh_ring_shm_t h;
    hh_ring_region_t r;
    FILE *f;
    char longp[200];

    HH_ASSERT_ERR(hh_ring_shm_create(&h, &r, path(), 7, 8, 288), HH_ERR_INVAL);   /* not a power of two */
    HH_ASSERT_ERR(hh_ring_shm_create(&h, &r, path(), 8, 8, 100), HH_ERR_INVAL);   /* not a multiple of 32 */
    HH_ASSERT(!exists(path()));
    HH_ASSERT_ERR(hh_ring_shm_create(&h, &r, "", 8, 8, 288), HH_ERR_INVAL);
    memset(longp, 'a', sizeof longp - 1);
    longp[sizeof longp - 1] = 0;
    HH_ASSERT_ERR(hh_ring_shm_create(&h, &r, longp, 8, 8, 288), HH_ERR_INVAL);
    HH_ASSERT_ERR(hh_ring_shm_create(NULL, &r, path(), 8, 8, 288), HH_ERR_INVAL);
    HH_ASSERT_ERR(hh_ring_shm_create(&h, &r, "/nonexistent-dir/x", 8, 8, 288), HH_ERR_IO);

    HH_ASSERT_ERR(hh_ring_shm_open(&h, &r, path()), HH_ERR_IO);                   /* no such file */
    f = fopen(path(), "wb");
    HH_ASSERT(f != NULL);
    fwrite("not a ring file, only text", 1, 26, f);
    fclose(f);
    HH_ASSERT_ERR(hh_ring_shm_open(&h, &r, path()), HH_ERR_INVAL);
    unlink(path());
    HH_ASSERT_ERR(hh_ring_shm_open(&h, &r, NULL), HH_ERR_INVAL);
}

/* Closing a handle that was never opened, or twice, is harmless. */
static void test_close_safe(void)
{
    hh_ring_shm_t h;
    hh_ring_region_t r;

    memset(&h, 0, sizeof h);
    hh_ring_shm_close(&h);
    hh_ring_shm_close(NULL);
    HH_ASSERT_OK(hh_ring_shm_create(&h, &r, path(), 8, 8, 288));
    hh_ring_shm_close(&h);
    hh_ring_shm_close(&h);
    HH_ASSERT(!exists(path()));
}

HH_TEST_MAIN_BEGIN("ring_shm")
    HH_RUN(test_two_processes_share_memory);
    HH_RUN(test_stale_file_replaced);
    HH_RUN(test_refusals);
    HH_RUN(test_close_safe);
HH_TEST_MAIN_END()
