/* Configuration: defaults, validation invariants, parsing, file loading. */
#include "hhsdr/core/config.h"
#include "hh_test.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

static void test_defaults_are_valid_once_node_id_set(void)
{
    hh_config_t cfg;
    hh_config_defaults(&cfg);
    /* node_id is deliberately unset by default: a node must be told who it is. */
    HH_ASSERT_ERR(hh_config_validate(&cfg), HH_ERR_INVAL);
    cfg.node_id = 1;
    HH_ASSERT_OK(hh_config_validate(&cfg));
}

static void test_hysteresis_invariant_enforced(void)
{
    hh_config_t cfg;
    hh_config_defaults(&cfg);
    cfg.node_id = 1;
    /* Doc 1 §7: exit threshold must be stricter than entry, else a link can
     * oscillate across the boundary on noise alone. */
    HH_ASSERT(cfg.lh_recover_threshold > cfg.lh_degrade_threshold);
    cfg.lh_recover_threshold = cfg.lh_degrade_threshold;
    HH_ASSERT_ERR(hh_config_validate(&cfg), HH_ERR_INVAL);
    cfg.lh_recover_threshold = cfg.lh_degrade_threshold - 0.1f;
    HH_ASSERT_ERR(hh_config_validate(&cfg), HH_ERR_INVAL);
}

static void test_cadence_bounds_invariant(void)
{
    hh_config_t cfg;
    hh_config_defaults(&cfg);
    cfg.node_id = 1;
    cfg.beacon_interval_min_ms = 5000;   /* min > max */
    HH_ASSERT_ERR(hh_config_validate(&cfg), HH_ERR_INVAL);

    hh_config_defaults(&cfg);
    cfg.node_id = 1;
    cfg.beacon_interval_ms = 10000;      /* steady-state outside [min,max] */
    HH_ASSERT_ERR(hh_config_validate(&cfg), HH_ERR_INVAL);
}

static void test_set_typed_values(void)
{
    hh_config_t cfg;
    hh_config_defaults(&cfg);

    HH_ASSERT_OK(hh_config_set(&cfg, "node_id", "42"));
    HH_ASSERT_EQ_INT(cfg.node_id, 42);

    HH_ASSERT_OK(hh_config_set(&cfg, "lh_degrade_threshold", "0.25"));
    HH_ASSERT_NEAR(cfg.lh_degrade_threshold, 0.25, 1e-6);

    HH_ASSERT_OK(hh_config_set(&cfg, "routing_capable", "false"));
    HH_ASSERT(!cfg.routing_capable);

    HH_ASSERT_OK(hh_config_set(&cfg, "log_level", "debug"));
    HH_ASSERT_EQ_INT(cfg.log_level, HH_LOG_DEBUG);

    HH_ASSERT_OK(hh_config_set(&cfg, "radio_adapter", "mock"));
    HH_ASSERT_EQ_STR(cfg.radio_adapter, "mock");

    HH_ASSERT_OK(hh_config_set(&cfg, "max_hop_count", "8"));
    HH_ASSERT_EQ_INT(cfg.max_hop_count, 8);
}

static void test_set_rejects_bad_input(void)
{
    hh_config_t cfg;
    hh_config_defaults(&cfg);

    HH_ASSERT_ERR(hh_config_set(&cfg, "no_such_key", "1"), HH_ERR_NOTFOUND);
    HH_ASSERT_ERR(hh_config_set(&cfg, "node_id", "notanumber"), HH_ERR_INVAL);
    HH_ASSERT_ERR(hh_config_set(&cfg, "node_id", "12abc"), HH_ERR_INVAL);
    HH_ASSERT_ERR(hh_config_set(&cfg, "log_level", "shouty"), HH_ERR_INVAL);
    HH_ASSERT_ERR(hh_config_set(&cfg, "max_hop_count", "0"), HH_ERR_INVAL);
    HH_ASSERT_ERR(hh_config_set(&cfg, "max_hop_count", "999"), HH_ERR_INVAL);
    /* Value that would overflow the fixed adapter-name buffer. */
    HH_ASSERT_ERR(hh_config_set(&cfg, "radio_adapter",
        "0123456789012345678901234567890123456789"), HH_ERR_INVAL);
}

static void test_load_file(void)
{
    hh_config_t cfg;
    char path[] = "/tmp/hh_cfg_XXXXXX";
    int fd = mkstemp(path);
    FILE *f;
    int err_line = 0;

    HH_ASSERT(fd >= 0);
    f = fdopen(fd, "w");
    HH_ASSERT(f != NULL);
    fprintf(f,
        "# node identity\n"
        "node_id = 7\n"
        "\n"
        "beacon_interval_ms=500   ; inline comment\n"
        "  log_level =  trace \n");
    fclose(f);

    hh_config_defaults(&cfg);
    HH_ASSERT_OK(hh_config_load_file(&cfg, path, &err_line));
    HH_ASSERT_EQ_INT(cfg.node_id, 7);
    HH_ASSERT_EQ_INT(cfg.beacon_interval_ms, 500);
    HH_ASSERT_EQ_INT(cfg.log_level, HH_LOG_TRACE);
    /* Untouched keys keep their defaults. */
    HH_ASSERT_EQ_INT(cfg.max_neighbors, 64);
    unlink(path);
}

static void test_load_file_reports_bad_line(void)
{
    hh_config_t cfg;
    char path[] = "/tmp/hh_cfg_XXXXXX";
    int fd = mkstemp(path);
    FILE *f = fdopen(fd, "w");
    int err_line = 0;

    HH_ASSERT(f != NULL);
    fprintf(f, "node_id = 3\n\nthis line has no equals sign\n");
    fclose(f);

    hh_config_defaults(&cfg);
    HH_ASSERT_ERR(hh_config_load_file(&cfg, path, &err_line), HH_ERR_INVAL);
    HH_ASSERT_EQ_INT(err_line, 3);
    unlink(path);
}

static void test_load_missing_file(void)
{
    hh_config_t cfg;
    hh_config_defaults(&cfg);
    HH_ASSERT_ERR(hh_config_load_file(&cfg, "/nonexistent/hh.conf", NULL), HH_ERR_IO);
}

HH_TEST_MAIN_BEGIN("config")
    HH_RUN(test_defaults_are_valid_once_node_id_set);
    HH_RUN(test_hysteresis_invariant_enforced);
    HH_RUN(test_cadence_bounds_invariant);
    HH_RUN(test_set_typed_values);
    HH_RUN(test_set_rejects_bad_input);
    HH_RUN(test_load_file);
    HH_RUN(test_load_file_reports_bad_line);
    HH_RUN(test_load_missing_file);
HH_TEST_MAIN_END()
