/*
 * Send order by class: highest first, voice capped, data never starved.
 * The fake clock is a plain variable, so nothing sleeps. Each test prints
 * the order the scheduler chose, which is the priority log.
 */
#include "hh_test.h"
#include "hhsdr/dataterm/sched.h"
#include <stdio.h>

static hh_dt_sched_t g_s;
static uint16_t      g_w[HH_DT_CLASS_COUNT];

static void setup(uint32_t data_every, uint32_t burst, uint32_t refill_ms)
{
    hh_dt_sched_config_t cfg = { data_every, burst, refill_ms };
    memset(g_w, 0, sizeof g_w);
    hh_dt_sched_init(&g_s, &cfg, 0);
}

static void fill(uint16_t voice, uint16_t signal, uint16_t video, uint16_t data)
{
    g_w[HH_DT_CLASS_VOICE] = voice;
    g_w[HH_DT_CLASS_SIGNAL] = signal;
    g_w[HH_DT_CLASS_VIDEO] = video;
    g_w[HH_DT_CLASS_DATA] = data;
}

/* Picks up to n times at time t, logs and returns the order as a string. */
static int run(hh_time_ms_t t, int n, char *order)
{
    int got = 0;
    printf("    t=%llu order:", (unsigned long long)t);
    for (int i = 0; i < n; i++) {
        int c = hh_dt_sched_pick(&g_s, g_w, t);
        if (c < 0) break;
        g_w[c]--;
        order[got++] = "VSMD"[c];
        printf(" %s", hh_dt_class_str((hh_dt_class_t)c));
    }
    order[got] = '\0';
    printf("%s\n", got ? "" : " (nothing)");
    return got;
}

static void test_dscp_values(void)
{
    HH_ASSERT_EQ_INT(hh_dt_class_dscp(HH_DT_CLASS_VOICE), 46);
    HH_ASSERT_EQ_INT(hh_dt_class_dscp(HH_DT_CLASS_SIGNAL), 40);
    HH_ASSERT_EQ_INT(hh_dt_class_dscp(HH_DT_CLASS_VIDEO), 34);
    HH_ASSERT_EQ_INT(hh_dt_class_dscp(HH_DT_CLASS_DATA), 0);
}

static void test_highest_class_leaves_first(void)
{
    char o[64];
    setup(100, 100, 1);
    fill(2, 1, 2, 3);
    HH_ASSERT_EQ_INT(run(0, 20, o), 8);
    HH_ASSERT_EQ_STR(o, "VVSMMDDD");
}

static void test_voice_arriving_late_overtakes_waiting_data(void)
{
    char o[64];
    setup(100, 100, 1);
    fill(0, 0, 0, 4);
    run(0, 2, o);
    HH_ASSERT_EQ_STR(o, "DD");
    g_w[HH_DT_CLASS_VOICE] = 1;
    run(0, 1, o);
    HH_ASSERT_EQ_STR(o, "V");
}

static void test_data_not_starved_by_continuous_voice(void)
{
    char o[64];
    setup(4, 100, 1);  /* a voice cap that never bites, to isolate the data share */
    fill(30, 0, 0, 3);
    run(0, 12, o);
    HH_ASSERT_EQ_STR(o, "VVVDVVVDVVVD");
}

static void test_voice_cap_lets_others_through(void)
{
    char o[64];
    setup(100, 3, 1000);  /* 3 voice sends, then one more per second */
    fill(10, 0, 2, 2);
    run(0, 7, o);
    HH_ASSERT_EQ_STR(o, "VVVMMDD");
    HH_ASSERT_EQ_INT(run(0, 1, o), 0);   /* voice is capped and nothing else waits */
    HH_ASSERT_EQ_INT(run(999, 1, o), 0);
    HH_ASSERT_EQ_INT(run(1000, 1, o), 1);
    HH_ASSERT_EQ_STR(o, "V");
}

static void test_voice_tokens_refill_up_to_burst(void)
{
    char o[64];
    setup(100, 2, 10);
    fill(10, 0, 0, 0);
    run(0, 5, o);
    HH_ASSERT_EQ_STR(o, "VV");
    run(100000, 5, o);  /* a long pause refills to the burst, not beyond */
    HH_ASSERT_EQ_STR(o, "VV");
}

static void test_nothing_waiting(void)
{
    char o[64];
    setup(8, 8, 20);
    HH_ASSERT_EQ_INT(run(0, 3, o), 0);
    HH_ASSERT_EQ_INT(hh_dt_sched_pick(NULL, g_w, 0), -1);
}

static void test_bad_config(void)
{
    hh_dt_sched_t s;
    hh_dt_sched_config_t c = { 0, 8, 20 };
    HH_ASSERT_ERR(hh_dt_sched_init(&s, &c, 0), HH_ERR_INVAL);
    c = (hh_dt_sched_config_t){ 8, 0, 20 };
    HH_ASSERT_ERR(hh_dt_sched_init(&s, &c, 0), HH_ERR_INVAL);
    c = (hh_dt_sched_config_t){ 8, 8, 0 };
    HH_ASSERT_ERR(hh_dt_sched_init(&s, &c, 0), HH_ERR_INVAL);
}

HH_TEST_MAIN_BEGIN("dataterm send order")
    HH_RUN(test_dscp_values);
    HH_RUN(test_highest_class_leaves_first);
    HH_RUN(test_voice_arriving_late_overtakes_waiting_data);
    HH_RUN(test_data_not_starved_by_continuous_voice);
    HH_RUN(test_voice_cap_lets_others_through);
    HH_RUN(test_voice_tokens_refill_up_to_burst);
    HH_RUN(test_nothing_waiting);
    HH_RUN(test_bad_config);
HH_TEST_MAIN_END()
