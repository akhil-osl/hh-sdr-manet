/* radiod frame ring: order, wrap, full-drop, empty, and size checks. */
#include "hhsdr/radiod/ring.h"
#include "hh_test.h"
#include <string.h>

#define SLOT 8u
#define N 4u

static uint8_t mem[SLOT * N];
static uint32_t lens[N];

static void test_init_rejects_bad_geometry(void)
{
    hh_ring_t r;
    HH_ASSERT_ERR(hh_ring_init(&r, mem, lens, SLOT, 3), HH_ERR_INVAL);
    HH_ASSERT_ERR(hh_ring_init(&r, mem, lens, 0, N), HH_ERR_INVAL);
    HH_ASSERT_ERR(hh_ring_init(&r, NULL, lens, SLOT, N), HH_ERR_INVAL);
    HH_ASSERT_OK(hh_ring_init(&r, mem, lens, SLOT, N));
}

static void test_fifo_and_wrap(void)
{
    hh_ring_t r;
    uint8_t out[SLOT];
    uint32_t len = 0;

    HH_ASSERT_OK(hh_ring_init(&r, mem, lens, SLOT, N));
    HH_ASSERT_ERR(hh_ring_pop(&r, out, sizeof out, &len), HH_ERR_AGAIN);

    /* Cycle well past the slot count so indices wrap several times. */
    for (uint8_t i = 0; i < 20; i++) {
        uint8_t in[3] = { i, (uint8_t)(i + 1), (uint8_t)(i + 2) };
        HH_ASSERT_OK(hh_ring_push(&r, in, sizeof in));
        HH_ASSERT_OK(hh_ring_pop(&r, out, sizeof out, &len));
        HH_ASSERT_EQ_INT(len, 3);
        HH_ASSERT(memcmp(in, out, 3) == 0);
    }
    HH_ASSERT_EQ_INT(hh_ring_used(&r), 0);
}

static void test_full_drops_and_keeps_queued(void)
{
    hh_ring_t r;
    uint8_t out[SLOT];
    uint32_t len;

    HH_ASSERT_OK(hh_ring_init(&r, mem, lens, SLOT, N));
    for (uint8_t i = 0; i < N; i++)
        HH_ASSERT_OK(hh_ring_push(&r, &i, 1));
    HH_ASSERT_ERR(hh_ring_push(&r, "x", 1), HH_ERR_NOMEM);
    HH_ASSERT_EQ_INT(hh_ring_full_drops(&r), 1);
    HH_ASSERT_EQ_INT(hh_ring_used(&r), N);

    /* The oldest frame is intact: the full push did not overwrite it. */
    HH_ASSERT_OK(hh_ring_pop(&r, out, sizeof out, &len));
    HH_ASSERT_EQ_INT(out[0], 0);
    HH_ASSERT_OK(hh_ring_push(&r, "y", 1));
}

static void test_size_checks(void)
{
    hh_ring_t r;
    uint8_t big[SLOT + 1] = {0};
    uint8_t small[2];
    uint32_t len;

    HH_ASSERT_OK(hh_ring_init(&r, mem, lens, SLOT, N));
    HH_ASSERT_ERR(hh_ring_push(&r, big, sizeof big), HH_ERR_INVAL);
    HH_ASSERT_OK(hh_ring_push(&r, big, 5));
    HH_ASSERT_ERR(hh_ring_pop(&r, small, sizeof small, &len), HH_ERR_INVAL);
    HH_ASSERT_EQ_INT(hh_ring_used(&r), 1); /* frame stays queued */
}

HH_TEST_MAIN_BEGIN("ring")
    HH_RUN(test_init_rejects_bad_geometry);
    HH_RUN(test_fifo_and_wrap);
    HH_RUN(test_full_drops_and_keeps_queued);
    HH_RUN(test_size_checks);
HH_TEST_MAIN_END()
