/*
 * radiod configuration tests.
 *
 * Covers the defaults, per-key parsing, validation invariants, and file
 * loading with line-accurate error reporting — the last matters because an
 * operator's only clue to a bad config file is the line number radiod prints.
 */
#include "hhsdr/radiod/config.h"
#include "hhsdr/protocol/rc.h"
#include "hh_test.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Write a temporary config file and return its path in `path`. */
static void write_cfg(char *path, size_t cap, const char *body)
{
    snprintf(path, cap, "/tmp/hh-radiod-cfg-%d.conf", (int)getpid());
    FILE *f = fopen(path, "w");
    if (!f) return;
    fputs(body, f);
    fclose(f);
}

static void test_defaults_preserve_prior_behaviour(void)
{
    hh_radiod_config_t cfg;
    hh_radiod_config_defaults(&cfg);

    /* The defaults must match what radiod did before it took a config file,
     * so adding configuration changed no deployed behaviour. */
    HH_ASSERT_EQ_STR(cfg.sock_path, HH_RC_DEFAULT_SOCK_PATH);
    HH_ASSERT_EQ_INT(cfg.tick_interval_ms, 10);
    HH_ASSERT_EQ_INT(cfg.client_idle_timeout_ms, 0);   /* disabled */
    HH_ASSERT_EQ_INT(cfg.log_level, HH_LOG_INFO);
    HH_ASSERT_OK(hh_radiod_config_validate(&cfg));
}

static void test_set_known_keys(void)
{
    hh_radiod_config_t cfg;
    hh_radiod_config_defaults(&cfg);

    HH_ASSERT_OK(hh_radiod_config_set(&cfg, "sock_path", "/run/radiod.sock"));
    HH_ASSERT_EQ_STR(cfg.sock_path, "/run/radiod.sock");

    HH_ASSERT_OK(hh_radiod_config_set(&cfg, "tick_interval_ms", "25"));
    HH_ASSERT_EQ_INT(cfg.tick_interval_ms, 25);

    HH_ASSERT_OK(hh_radiod_config_set(&cfg, "client_idle_timeout_ms", "60000"));
    HH_ASSERT_EQ_INT(cfg.client_idle_timeout_ms, 60000);

    HH_ASSERT_OK(hh_radiod_config_set(&cfg, "log_level", "debug"));
    HH_ASSERT_EQ_INT(cfg.log_level, HH_LOG_DEBUG);
}

static void test_unknown_key_is_distinguishable(void)
{
    hh_radiod_config_t cfg;
    hh_radiod_config_defaults(&cfg);

    /* NOTFOUND (unknown key) must be distinct from INVAL (bad value), so the
     * caller can tell a typo from a malformed number. */
    HH_ASSERT_ERR(hh_radiod_config_set(&cfg, "no_such_key", "1"), HH_ERR_NOTFOUND);
    HH_ASSERT_ERR(hh_radiod_config_set(&cfg, "tick_interval_ms", "abc"), HH_ERR_INVAL);
    HH_ASSERT_ERR(hh_radiod_config_set(&cfg, "tick_interval_ms", "12x"), HH_ERR_INVAL);
    HH_ASSERT_ERR(hh_radiod_config_set(&cfg, "tick_interval_ms", ""), HH_ERR_INVAL);
    HH_ASSERT_ERR(hh_radiod_config_set(&cfg, "log_level", "verbose"), HH_ERR_INVAL);
    HH_ASSERT_ERR(hh_radiod_config_set(&cfg, "sock_path", ""), HH_ERR_INVAL);
}

static void test_validation_invariants(void)
{
    hh_radiod_config_t cfg;

    /* A zero tick would spin the control loop without yielding. */
    hh_radiod_config_defaults(&cfg);
    cfg.tick_interval_ms = 0;
    HH_ASSERT_ERR(hh_radiod_config_validate(&cfg), HH_ERR_INVAL);

    /* An idle timeout below one tick could expire a client before it is ever
     * serviced. */
    hh_radiod_config_defaults(&cfg);
    cfg.tick_interval_ms = 50;
    cfg.client_idle_timeout_ms = 10;
    HH_ASSERT_ERR(hh_radiod_config_validate(&cfg), HH_ERR_INVAL);

    /* Zero means "no timeout" and stays valid at any tick. */
    hh_radiod_config_defaults(&cfg);
    cfg.tick_interval_ms = 50;
    cfg.client_idle_timeout_ms = 0;
    HH_ASSERT_OK(hh_radiod_config_validate(&cfg));

    /* An empty socket path cannot be bound. */
    hh_radiod_config_defaults(&cfg);
    cfg.sock_path[0] = '\0';
    HH_ASSERT_ERR(hh_radiod_config_validate(&cfg), HH_ERR_INVAL);
}

static void test_load_file_with_comments(void)
{
    char path[128];
    hh_radiod_config_t cfg;
    int err_line = 0;

    write_cfg(path, sizeof path,
        "# radiod configuration\n"
        "\n"
        "sock_path = /run/test-radiod.sock   # trailing comment\n"
        "; semicolon comment line\n"
        "tick_interval_ms       = 20\n"
        "client_idle_timeout_ms = 30000\n"
        "log_level = warn\n");

    hh_radiod_config_defaults(&cfg);
    HH_ASSERT_OK(hh_radiod_config_load_file(&cfg, path, &err_line));
    HH_ASSERT_EQ_STR(cfg.sock_path, "/run/test-radiod.sock");
    HH_ASSERT_EQ_INT(cfg.tick_interval_ms, 20);
    HH_ASSERT_EQ_INT(cfg.client_idle_timeout_ms, 30000);
    HH_ASSERT_EQ_INT(cfg.log_level, HH_LOG_WARN);
    HH_ASSERT_OK(hh_radiod_config_validate(&cfg));

    unlink(path);
}

static void test_load_file_reports_offending_line(void)
{
    char path[128];
    hh_radiod_config_t cfg;
    int err_line = 0;

    write_cfg(path, sizeof path,
        "# comment\n"
        "tick_interval_ms = 20\n"
        "bogus_key = 1\n");          /* line 3 */

    hh_radiod_config_defaults(&cfg);
    HH_ASSERT_ERR(hh_radiod_config_load_file(&cfg, path, &err_line), HH_ERR_INVAL);
    HH_ASSERT_EQ_INT(err_line, 3);

    /* A line with no '=' at all is also reported by number. */
    write_cfg(path, sizeof path, "tick_interval_ms = 20\ngarbage\n");
    err_line = 0;
    hh_radiod_config_defaults(&cfg);
    HH_ASSERT_ERR(hh_radiod_config_load_file(&cfg, path, &err_line), HH_ERR_INVAL);
    HH_ASSERT_EQ_INT(err_line, 2);

    unlink(path);
}

static void test_missing_file_is_io_error(void)
{
    hh_radiod_config_t cfg;
    hh_radiod_config_defaults(&cfg);
    HH_ASSERT_ERR(hh_radiod_config_load_file(&cfg, "/nonexistent/radiod.conf", NULL),
                  HH_ERR_IO);
}

static void test_null_arguments_rejected(void)
{
    hh_radiod_config_t cfg;
    hh_radiod_config_defaults(&cfg);
    HH_ASSERT_ERR(hh_radiod_config_validate(NULL), HH_ERR_INVAL);
    HH_ASSERT_ERR(hh_radiod_config_set(NULL, "log_level", "info"), HH_ERR_INVAL);
    HH_ASSERT_ERR(hh_radiod_config_set(&cfg, NULL, "info"), HH_ERR_INVAL);
    HH_ASSERT_ERR(hh_radiod_config_set(&cfg, "log_level", NULL), HH_ERR_INVAL);
    HH_ASSERT_ERR(hh_radiod_config_load_file(NULL, "/tmp/x", NULL), HH_ERR_INVAL);
    HH_ASSERT_ERR(hh_radiod_config_load_file(&cfg, NULL, NULL), HH_ERR_INVAL);
}

static void test_backend_selection(void)
{
    hh_radiod_config_t cfg;
    hh_radiod_config_defaults(&cfg);

    /* The mock stays the default, so existing deployments are unchanged. */
    HH_ASSERT_EQ_INT(cfg.backend, HH_RADIOD_BACKEND_MOCK);
    HH_ASSERT_OK(hh_radiod_config_set(&cfg, "backend", "ocpi"));
    HH_ASSERT_EQ_INT(cfg.backend, HH_RADIOD_BACKEND_OCPI);
    HH_ASSERT_ERR(hh_radiod_config_set(&cfg, "backend", "fpga"), HH_ERR_INVAL);

    /* The OpenCPI backend has nothing to open without an application. */
    HH_ASSERT_ERR(hh_radiod_config_validate(&cfg), HH_ERR_INVAL);
    HH_ASSERT_OK(hh_radiod_config_set(&cfg, "ocpi_app", "/opt/app/app.xml"));
    HH_ASSERT_OK(hh_radiod_config_validate(&cfg));
    HH_ASSERT_EQ_STR(cfg.ocpi.app_path, "/opt/app/app.xml");
    HH_ASSERT_ERR(hh_radiod_config_set(&cfg, "ocpi_app", ""), HH_ERR_INVAL);
}

static void test_ocpi_keys_from_file(void)
{
    char path[64];
    hh_radiod_config_t cfg;
    int line = 0;

    write_cfg(path, sizeof path,
              "backend = ocpi\n"
              "ocpi_app = app.xml\n"
              "ocpi_library_path = /opt/artifacts\n"
              "ocpi_property = src.fileName=/opt/in.bin\n"
              "ocpi_property = sink.stopOnEOF=false\n");
    hh_radiod_config_defaults(&cfg);
    HH_ASSERT_OK(hh_radiod_config_load_file(&cfg, path, &line));
    HH_ASSERT_EQ_STR(cfg.ocpi.library_path, "/opt/artifacts");
    /* Repeated keys accumulate, in file order. */
    HH_ASSERT_EQ_INT(cfg.ocpi.prop_count, 2);
    HH_ASSERT_EQ_STR(cfg.ocpi.props[0].instance, "src");
    HH_ASSERT_EQ_STR(cfg.ocpi.props[0].value, "/opt/in.bin");
    HH_ASSERT_EQ_STR(cfg.ocpi.props[1].property, "stopOnEOF");

    /* A malformed property points at its own line. */
    write_cfg(path, sizeof path, "backend = ocpi\nocpi_property = nodot\n");
    hh_radiod_config_defaults(&cfg);
    HH_ASSERT_ERR(hh_radiod_config_load_file(&cfg, path, &line), HH_ERR_INVAL);
    HH_ASSERT_EQ_INT(line, 2);
    unlink(path);
}

HH_TEST_MAIN_BEGIN("radiod configuration")
    HH_RUN(test_defaults_preserve_prior_behaviour);
    HH_RUN(test_set_known_keys);
    HH_RUN(test_unknown_key_is_distinguishable);
    HH_RUN(test_validation_invariants);
    HH_RUN(test_load_file_with_comments);
    HH_RUN(test_load_file_reports_offending_line);
    HH_RUN(test_missing_file_is_io_error);
    HH_RUN(test_null_arguments_rejected);
    HH_RUN(test_backend_selection);
    HH_RUN(test_ocpi_keys_from_file);
HH_TEST_MAIN_END()
