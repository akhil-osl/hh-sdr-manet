/* Sequence-number arithmetic: the freshness rule the whole control plane relies on. */
#include "hhsdr/core/seq.h"
#include "hh_test.h"

static void test_basic_ordering(void)
{
    HH_ASSERT(hh_seq_gt(5, 4));
    HH_ASSERT(!hh_seq_gt(4, 5));
    HH_ASSERT(!hh_seq_gt(5, 5));
    HH_ASSERT(hh_seq_lt(4, 5));
    HH_ASSERT(hh_seq_ge(5, 5));
    HH_ASSERT(hh_seq_le(5, 5));
}

static void test_wraparound(void)
{
    /* The point of serial arithmetic: 0 is newer than UINT32_MAX. */
    HH_ASSERT(hh_seq_gt(0u, 0xFFFFFFFFu));
    HH_ASSERT(!hh_seq_gt(0xFFFFFFFFu, 0u));
    HH_ASSERT(hh_seq_gt(5u, 0xFFFFFFF0u));
    HH_ASSERT(hh_seq_lt(0xFFFFFFF0u, 5u));
}

static void test_half_space_is_ambiguous(void)
{
    /* Exactly half the space apart: neither direction may claim newer, or a
     * replayed ancient beacon could be accepted as fresh. */
    HH_ASSERT(!hh_seq_gt(0u, 0x80000000u));
    HH_ASSERT(!hh_seq_gt(0x80000000u, 0u));
}

static void test_monotonic_walk_across_wrap(void)
{
    /* Walk a counter through the wrap point; each step must be strictly newer. */
    hh_seq_t s = 0xFFFFFFFAu;
    for (int i = 0; i < 12; i++) {
        hh_seq_t next = s + 1u;
        HH_ASSERT_MSG(hh_seq_gt(next, s), "step %d: %u not newer than %u", i, next, s);
        s = next;
    }
    HH_ASSERT_EQ_INT(s, 6u);
}

HH_TEST_MAIN_BEGIN("seq")
    HH_RUN(test_basic_ordering);
    HH_RUN(test_wraparound);
    HH_RUN(test_half_space_is_ambiguous);
    HH_RUN(test_monotonic_walk_across_wrap);
HH_TEST_MAIN_END()
