/*
 * radiod and the frame rings: radiod creates the shared-memory file from its
 * configuration, another handle opens it by path and moves frames through the
 * ring interface, and radiod removes the file on release.
 */
#include "hhsdr/radiod/config.h"
#include "hhsdr/radiod/mock_backend.h"
#include "hhsdr/radiod/radiod.h"
#include "hh_test.h"
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static hh_time_ms_t g_now;
static hh_time_ms_t fake_now(void *ctx) { (void)ctx; return g_now; }
static const hh_clock_t g_clock = { fake_now, NULL };

/* A unique path in /tmp for this process. */
static const char *path(void)
{
    static char p[64];
    snprintf(p, sizeof p, "/tmp/hh-radiod-rings-%d", (int)getpid());
    return p;
}

/* True if a file exists at p. */
static int exists(const char *p)
{
    struct stat sb;
    return stat(p, &sb) == 0;
}

/* The ring keys are read, and a bad geometry fails validation. */
static void test_config_keys(void)
{
    hh_radiod_config_t cfg;

    hh_radiod_config_defaults(&cfg);
    HH_ASSERT_EQ_STR(cfg.ring_path, "");                       /* off by default */
    HH_ASSERT_OK(hh_radiod_config_validate(&cfg));

    HH_ASSERT_OK(hh_radiod_config_set(&cfg, "ring_path", "/dev/shm/hh-rings"));
    HH_ASSERT_OK(hh_radiod_config_set(&cfg, "ring_rx_slots", "32"));
    HH_ASSERT_OK(hh_radiod_config_set(&cfg, "ring_tx_slots", "16"));
    HH_ASSERT_OK(hh_radiod_config_set(&cfg, "ring_slot_size", "320"));
    HH_ASSERT_EQ_STR(cfg.ring_path, "/dev/shm/hh-rings");
    HH_ASSERT_EQ_INT(cfg.ring_rx_slots, 32);
    HH_ASSERT_EQ_INT(cfg.ring_tx_slots, 16);
    HH_ASSERT_EQ_INT(cfg.ring_slot_size, 320);
    HH_ASSERT_OK(hh_radiod_config_validate(&cfg));

    HH_ASSERT_OK(hh_radiod_config_set(&cfg, "ring_rx_slots", "30"));   /* not a power of two */
    HH_ASSERT_ERR(hh_radiod_config_validate(&cfg), HH_ERR_INVAL);
    HH_ASSERT_ERR(hh_radiod_config_set(&cfg, "ring_slot_size", "abc"), HH_ERR_INVAL);
}

/* With a path set, radiod creates the rings, a client uses them, release removes the file. */
static void test_radiod_owns_the_rings(void)
{
    hh_mock_backend_t m;
    hh_radio_t r;
    hh_radiod_t d;
    hh_radiod_config_t cfg;
    hh_ring_shm_t cli;
    hh_ring_region_t rc;
    hh_ring_slot_t s;
    const uint8_t msg[3] = { 9, 8, 7 };

    g_now = 0;
    hh_mock_backend_init(&m, &r);
    HH_ASSERT_OK(hh_radiod_init(&d, &r, &g_clock));
    hh_radiod_config_defaults(&cfg);
    snprintf(cfg.ring_path, sizeof cfg.ring_path, "%s", path());
    cfg.ring_rx_slots = 8;
    cfg.ring_tx_slots = 8;
    HH_ASSERT_OK(hh_radiod_configure(&d, &cfg));
    HH_ASSERT(hh_radiod_rings(&d) == NULL);                    /* not yet */

    HH_ASSERT_OK(hh_radiod_open_rings(&d, &cfg));
    HH_ASSERT(hh_radiod_rings(&d) != NULL);
    HH_ASSERT(exists(path()));

    HH_ASSERT_OK(hh_ring_shm_open(&cli, &rc, path()));
    HH_ASSERT_EQ_INT(rc.rx.count, 8);
    HH_ASSERT_OK(hh_ring_produce_begin(&rc.tx, &s));           /* the client writes the TX ring */
    memcpy(s.frame, msg, sizeof msg);
    hh_ring_produce_commit(&rc.tx, &s, 0);
    HH_ASSERT_EQ_INT(hh_ring_view_used(&hh_radiod_rings(&d)->tx), 1);   /* radiod sees it, it did not carry it */
    hh_ring_shm_close(&cli);

    hh_radiod_release(&d);
    HH_ASSERT(!exists(path()));
    HH_ASSERT(hh_radiod_rings(&d) == NULL);
}

/* Without a path, radiod creates nothing. */
static void test_off_by_default(void)
{
    hh_mock_backend_t m;
    hh_radio_t r;
    hh_radiod_t d;
    hh_radiod_config_t cfg;

    hh_mock_backend_init(&m, &r);
    HH_ASSERT_OK(hh_radiod_init(&d, &r, &g_clock));
    hh_radiod_config_defaults(&cfg);
    HH_ASSERT_OK(hh_radiod_open_rings(&d, &cfg));
    HH_ASSERT(hh_radiod_rings(&d) == NULL);
    HH_ASSERT_ERR(hh_radiod_open_rings(NULL, &cfg), HH_ERR_INVAL);
    hh_radiod_release(&d);
}

HH_TEST_MAIN_BEGIN("radiod rings")
    HH_RUN(test_config_keys);
    HH_RUN(test_radiod_owns_the_rings);
    HH_RUN(test_off_by_default);
HH_TEST_MAIN_END()
