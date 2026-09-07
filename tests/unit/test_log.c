/* Structured logging: tests assert on log records, so the format is itself a contract. */
#include "hhsdr/core/log.h"
#include "hh_test.h"
#include <string.h>

#define CAP_MAX 16
static char g_lines[CAP_MAX][512];
static int  g_count;

static void capture(hh_log_level_t lvl, const char *line, void *ctx)
{
    (void)lvl; (void)ctx;
    if (g_count < CAP_MAX) snprintf(g_lines[g_count], sizeof g_lines[0], "%s", line);
    g_count++;
}

static void reset(hh_log_level_t lvl)
{
    g_count = 0;
    hh_log_set_sink(capture, NULL);
    hh_log_set_level(lvl);
}

static void test_record_format_is_key_value(void)
{
    reset(HH_LOG_INFO);
    HH_LOGI("neighbor", "neighbor_up", "neighbor=%u seq=%u", 3u, 9u);
    HH_ASSERT_EQ_INT(g_count, 1);
    HH_ASSERT_EQ_STR(g_lines[0],
        "lvl=info comp=neighbor event=neighbor_up neighbor=3 seq=9");
}

static void test_level_filters_records(void)
{
    reset(HH_LOG_WARN);
    HH_LOGI("x", "info_event", "a=%d", 1);
    HH_LOGD("x", "debug_event", "a=%d", 1);
    HH_ASSERT_EQ_INT(g_count, 0);

    HH_LOGW("x", "warn_event", "a=%d", 1);
    HH_LOGE("x", "error_event", "a=%d", 1);
    HH_ASSERT_EQ_INT(g_count, 2);
    HH_ASSERT(strstr(g_lines[0], "event=warn_event") != NULL);
    HH_ASSERT(strstr(g_lines[1], "lvl=error") != NULL);
}

static void test_level_parse_roundtrip(void)
{
    const hh_log_level_t all[] = { HH_LOG_ERROR, HH_LOG_WARN, HH_LOG_INFO,
                                   HH_LOG_DEBUG, HH_LOG_TRACE };
    for (size_t i = 0; i < sizeof all / sizeof all[0]; i++) {
        hh_log_level_t got;
        HH_ASSERT(hh_log_level_parse(hh_log_level_str(all[i]), &got));
        HH_ASSERT_EQ_INT(got, all[i]);
    }
    hh_log_level_t got;
    HH_ASSERT(!hh_log_level_parse("nonsense", &got));
    HH_ASSERT(!hh_log_level_parse(NULL, &got));
}

static void test_long_record_is_truncated_not_overflowed(void)
{
    char big[900];
    memset(big, 'x', sizeof big - 1);
    big[sizeof big - 1] = '\0';
    reset(HH_LOG_INFO);
    HH_LOGI("comp", "big", "data=%s", big);
    HH_ASSERT_EQ_INT(g_count, 1);
    HH_ASSERT(strlen(g_lines[0]) < 512);
    HH_ASSERT(strstr(g_lines[0], "event=big") != NULL);
}

HH_TEST_MAIN_BEGIN("log")
    HH_RUN(test_record_format_is_key_value);
    HH_RUN(test_level_filters_records);
    HH_RUN(test_level_parse_roundtrip);
    HH_RUN(test_long_record_is_truncated_not_overflowed);
HH_TEST_MAIN_END()
