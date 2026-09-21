/*
 * radiod — standalone x86 mock radio-control daemon entry point.
 *
 * Wires the mock backend, the rc.h control socket, and the radiod state
 * machine, then runs the control loop. Kept thin, matching src/main.c: no
 * MANET or topology logic lives here.
 */
#include "hhsdr/core/clock.h"
#include "hhsdr/core/log.h"
#include "hhsdr/radiod/mock_backend.h"
#include "hhsdr/radiod/radiod.h"
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static volatile sig_atomic_t g_stop = 0;
static void on_signal(int sig) { (void)sig; g_stop = 1; }

static void fault_hook(void *ctx, hh_rc_fault_t fault)
{
    hh_mock_backend_inject_fault((hh_mock_backend_t *)ctx, fault);
}

static void usage(const char *argv0)
{
    fprintf(stderr,
        "usage: %s [-s sock_path] [-v level]\n"
        "  -s  UNIX domain socket path (default %s)\n"
        "  -v  log level: error|warn|info|debug|trace\n",
        argv0, HH_RC_DEFAULT_SOCK_PATH);
}

int main(int argc, char **argv)
{
    const char *sock_path = HH_RC_DEFAULT_SOCK_PATH;
    hh_mock_backend_t backend;
    hh_radio_t radio;
    hh_radiod_t daemon;
    hh_status_t st;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-s") && i + 1 < argc) sock_path = argv[++i];
        else if (!strcmp(argv[i], "-v") && i + 1 < argc) {
            hh_log_level_t lvl;
            if (!hh_log_level_parse(argv[++i], &lvl)) { usage(argv[0]); return 2; }
            hh_log_set_level(lvl);
        } else { usage(argv[0]); return 2; }
    }

    hh_mock_backend_init(&backend, &radio);

    st = hh_radiod_init(&daemon, &radio, hh_clock_monotonic());
    if (st != HH_OK) { fprintf(stderr, "radiod init failed: %s\n", hh_status_str(st)); return 1; }

    hh_radiod_set_fault_hook(&daemon, fault_hook, &backend);

    st = hh_radiod_listen(&daemon, sock_path);
    if (st != HH_OK) { fprintf(stderr, "radiod listen failed: %s\n", hh_status_str(st)); return 1; }

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    while (!g_stop) {
        struct timespec ts = { 0, 10 * 1000 * 1000 }; /* 10 ms control loop */
        hh_time_ms_t now = hh_now(hh_clock_monotonic());
        hh_radiod_tick(&daemon, now);
        if (hh_radiod_shutdown_requested(&daemon)) break;
        nanosleep(&ts, NULL);
    }

    hh_radiod_release(&daemon);
    return 0;
}
