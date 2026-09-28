/*
 * radiod OpenCPI backend, against a real OpenCPI runtime.
 *
 * Unlike radiod's other tests this one is not driven by a virtual clock: it
 * creates real OpenCPI applications (the XML fixtures in tests/ocpi/, built from the stock
 * file_read/file_write components) and has to give them real time to move
 * data. Waits are short and bounded, and the test is registered only in
 * builds configured with -DHH_WITH_OPENCPI=ON.
 *
 * Needs OCPI_CDK_DIR and OCPI_LIBRARY_PATH in the environment; CMake sets
 * both on the test.
 */
#include "hh_test.h"
#include "hhsdr/core/log.h"
#include "hhsdr/radiod/ocpi_backend.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define INPUT_FILE "hh_ocpi_test_in.bin"

static void sleep_ms(long ms)
{
    struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

/* The fixtures read INPUT_FILE from the working directory. */
static void make_input_file(void)
{
    FILE *f = fopen(INPUT_FILE, "wb");
    unsigned char block[4096];
    for (size_t i = 0; i < sizeof block; i++) block[i] = (unsigned char)i;
    if (!f) { HH_FAIL("cannot create %s", INPUT_FILE); return; }
    for (int i = 0; i < 16; i++) fwrite(block, 1, sizeof block, f);
    fclose(f);
}

static hh_ocpi_config_t config_for(const char *fixture)
{
    hh_ocpi_config_t cfg;
    memset(&cfg, 0, sizeof cfg);
    snprintf(cfg.app_path, sizeof cfg.app_path, "%s/%s", HH_OCPI_FIXTURE_DIR, fixture);
    return cfg;
}

static unsigned long long prop_u64(hh_ocpi_backend_t *b, const char *inst, const char *prop)
{
    char buf[64];
    if (hh_ocpi_backend_get_property(b, inst, prop, buf, sizeof buf) != HH_OK) return 0;
    return strtoull(buf, NULL, 10);
}

static bool operational(hh_radio_t *r)
{
    hh_radio_status_t st;
    return hh_radio_get_status(r, &st) == HH_OK && st.operational;
}

static void test_init_requires_app_path(void)
{
    hh_ocpi_backend_t b;
    hh_ocpi_config_t cfg;
    hh_radio_t r;
    memset(&cfg, 0, sizeof cfg);
    HH_ASSERT_ERR(hh_ocpi_backend_init(&b, &cfg, &r), HH_ERR_INVAL);
}

static void test_property_spec_parsing(void)
{
    hh_ocpi_config_t cfg;
    memset(&cfg, 0, sizeof cfg);
    HH_ASSERT_OK(hh_ocpi_config_add_property(&cfg, "src.fileName=/opt/a.b=c"));
    HH_ASSERT_EQ_STR(cfg.props[0].instance, "src");
    HH_ASSERT_EQ_STR(cfg.props[0].property, "fileName");
    HH_ASSERT_EQ_STR(cfg.props[0].value, "/opt/a.b=c");
    HH_ASSERT_ERR(hh_ocpi_config_add_property(&cfg, "noproperty=1"), HH_ERR_INVAL);
    HH_ASSERT_ERR(hh_ocpi_config_add_property(&cfg, ".prop=1"), HH_ERR_INVAL);
    HH_ASSERT_ERR(hh_ocpi_config_add_property(&cfg, "inst.=1"), HH_ERR_INVAL);
    HH_ASSERT_ERR(hh_ocpi_config_add_property(&cfg, "inst.prop"), HH_ERR_INVAL);
    for (int i = 1; i < HH_OCPI_MAX_PROPS; i++)
        HH_ASSERT_OK(hh_ocpi_config_add_property(&cfg, "a.b=1"));
    HH_ASSERT_ERR(hh_ocpi_config_add_property(&cfg, "a.b=1"), HH_ERR_NOMEM);
}

/* The whole point of the backend: start an application, see it run, stop it,
 * and start it again as radiod does on stop -> start. */
static void test_start_run_stop_restart(void)
{
    hh_ocpi_backend_t b;
    hh_ocpi_config_t cfg = config_for("loop_app.xml");
    hh_radio_t r;
    unsigned long long first, second;

    HH_ASSERT_OK(hh_ocpi_backend_init(&b, &cfg, &r));
    HH_ASSERT(!operational(&r));
    HH_ASSERT_ERR(hh_ocpi_backend_get_property(&b, "src", "bytesRead", (char[8]){0}, 8),
                  HH_ERR_STATE);

    HH_ASSERT_OK(hh_radio_open(&r));
    HH_ASSERT(operational(&r));
    HH_ASSERT_ERR(hh_radio_open(&r), HH_ERR_STATE);   /* already open */

    sleep_ms(100);
    HH_ASSERT_OK(hh_radio_poll(&r, 0));
    first = prop_u64(&b, "src", "bytesRead");
    sleep_ms(100);
    HH_ASSERT_OK(hh_radio_poll(&r, 0));
    second = prop_u64(&b, "src", "bytesRead");
    HH_ASSERT_MSG(first > 0 && second > first,
                  "data must be flowing: bytesRead %llu then %llu", first, second);
    HH_ASSERT(operational(&r));

    HH_ASSERT_OK(hh_radio_close(&r));
    HH_ASSERT(!operational(&r));
    HH_ASSERT_OK(hh_radio_close(&r));                 /* safe twice */

    HH_ASSERT_OK(hh_radio_open(&r));                  /* restart */
    HH_ASSERT(operational(&r));
    HH_ASSERT_OK(hh_radio_close(&r));
}

/* Configured properties are written after initialize and before start. */
static void test_configured_property_is_applied(void)
{
    hh_ocpi_backend_t b;
    hh_ocpi_config_t cfg = config_for("loop_app.xml");
    hh_radio_t r;
    char buf[32];

    HH_ASSERT_OK(hh_ocpi_config_add_property(&cfg, "src.messageSize=2048"));
    HH_ASSERT_OK(hh_ocpi_backend_init(&b, &cfg, &r));
    HH_ASSERT_OK(hh_radio_open(&r));
    HH_ASSERT_OK(hh_ocpi_backend_get_property(&b, "src", "messageSize", buf, sizeof buf));
    HH_ASSERT_EQ_STR(buf, "2048");
    HH_ASSERT_ERR(hh_ocpi_backend_get_property(&b, "src", "no_such_property", buf, sizeof buf),
                  HH_ERR_NOTFOUND);
    HH_ASSERT_OK(hh_radio_close(&r));
}

/* A property OpenCPI rejects fails the start, and leaves nothing running. */
static void test_bad_property_fails_start_cleanly(void)
{
    hh_ocpi_backend_t b;
    hh_ocpi_config_t cfg = config_for("loop_app.xml");
    hh_radio_t r;

    HH_ASSERT_OK(hh_ocpi_config_add_property(&cfg, "src.no_such_property=1"));
    HH_ASSERT_OK(hh_ocpi_backend_init(&b, &cfg, &r));
    HH_ASSERT_ERR(hh_radio_open(&r), HH_ERR_IO);
    HH_ASSERT(!operational(&r));
    HH_ASSERT(b.app == NULL);
    HH_ASSERT(strlen(hh_ocpi_backend_last_error(&b)) > 0);
    HH_ASSERT_OK(hh_radio_close(&r));
}

static void test_missing_application_fails_open(void)
{
    hh_ocpi_backend_t b;
    hh_ocpi_config_t cfg = config_for("does_not_exist.xml");
    hh_radio_t r;

    HH_ASSERT_OK(hh_ocpi_backend_init(&b, &cfg, &r));
    HH_ASSERT_ERR(hh_radio_open(&r), HH_ERR_IO);
    HH_ASSERT(!operational(&r));
    HH_ASSERT(strlen(hh_ocpi_backend_last_error(&b)) > 0);
}

/* An application that ends by itself must stop being reported operational. */
static void test_finished_application_is_not_operational(void)
{
    hh_ocpi_backend_t b;
    hh_ocpi_config_t cfg = config_for("finite_app.xml");
    hh_radio_t r;
    bool ended = false;

    HH_ASSERT_OK(hh_ocpi_backend_init(&b, &cfg, &r));
    HH_ASSERT_OK(hh_radio_open(&r));
    for (int i = 0; i < 100 && !ended; i++) {   /* at most ~2 s */
        HH_ASSERT_OK(hh_radio_poll(&r, 0));
        ended = !operational(&r);
        if (!ended) sleep_ms(20);
    }
    HH_ASSERT_MSG(ended, "finite application never reported finished");
    HH_ASSERT_OK(hh_radio_close(&r));
}

/* What the backend refuses, it refuses loudly and by the documented code. */
static void test_undefined_contracts_are_refused(void)
{
    hh_ocpi_backend_t b;
    hh_ocpi_config_t cfg = config_for("loop_app.xml");
    hh_radio_t r;
    hh_frame_t f;
    hh_link_sample_t m;

    memset(&f, 0, sizeof f);
    HH_ASSERT_OK(hh_ocpi_backend_init(&b, &cfg, &r));
    HH_ASSERT_ERR(hh_radio_set_channel(&r, 3), HH_ERR_UNSUPPORTED);          /* U-14 */
    HH_ASSERT_ERR(hh_radio_transmit(&r, &f), HH_ERR_NOT_IMPLEMENTED);        /* U-15 */
    HH_ASSERT_ERR(hh_radio_get_link_metrics(&r, 1, &m), HH_ERR_UNSUPPORTED);
}

HH_TEST_MAIN_BEGIN("ocpi_backend")
    hh_log_set_level(HH_LOG_ERROR);
    make_input_file();
    HH_RUN(test_init_requires_app_path);
    HH_RUN(test_property_spec_parsing);
    HH_RUN(test_start_run_stop_restart);
    HH_RUN(test_configured_property_is_applied);
    HH_RUN(test_bad_property_fails_start_cleanly);
    HH_RUN(test_missing_application_fails_open);
    HH_RUN(test_finished_application_is_not_operational);
    HH_RUN(test_undefined_contracts_are_refused);
HH_TEST_MAIN_END()
