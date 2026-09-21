/*
 * radiod — radio-control daemon entry point.
 *
 * Wires the backend, the control socket, and the radiod state machine, then
 * runs the control loop. Kept thin, matching src/main.c: no MANET or topology
 * logic lives here.
 *
 * The loop blocks in poll() until a client is ready or the tick interval
 * elapses, rather than waking on a fixed timer regardless of traffic.
 *
 * BACKEND: the mock backend is wired in here. In the target architecture
 * radiod is the single PL/OpenCPI owner, and this is where a real backend
 * would be selected instead — but the OpenCPI application, worker names and
 * ACI lifecycle are unspecified (unknown.md U-03, U-04), so no such backend
 * exists to select. The mock implements the same hh_radio_ops_t contract a
 * real one will, so nothing below this file changes when it arrives.
 */
#include "hhsdr/core/clock.h"
#include "hhsdr/core/log.h"
#include "hhsdr/radiod/config.h"
#include "hhsdr/radiod/mock_backend.h"
#include "hhsdr/radiod/radiod.h"
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static volatile sig_atomic_t g_stop = 0;
static void on_signal(int sig) { (void)sig; g_stop = 1; }

/* sigaction rather than signal(): signal()'s semantics vary across platforms,
 * and the default here must be a one-shot flag with no SA_RESTART surprises
 * around poll(). */
static void install_signal_handlers(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;   /* no SA_RESTART: poll() should return EINTR promptly */
    sigaction(SIGINT,  &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    /* A client that disconnects mid-reply must not kill the daemon; the
     * write path already reports the error and drops that client. */
    signal(SIGPIPE, SIG_IGN);
}

static void fault_hook(void *ctx, hh_rc_fault_t fault)
{
    hh_mock_backend_inject_fault((hh_mock_backend_t *)ctx, fault);
}

static void usage(const char *argv0)
{
    fprintf(stderr,
        "usage: %s [-c config] [-s sock_path] [-v level]\n"
        "  -c  configuration file (key = value)\n"
        "  -s  UNIX domain socket path (default %s)\n"
        "  -v  log level: error|warn|info|debug|trace\n"
        "\n"
        "Command-line options override the configuration file.\n",
        argv0, HH_RC_DEFAULT_SOCK_PATH);
}

int main(int argc, char **argv)
{
    hh_radiod_config_t cfg;
    hh_mock_backend_t backend;
    hh_radio_t radio;
    hh_radiod_t daemon;
    hh_status_t st;
    const char *cfg_path = NULL;
    const char *sock_override = NULL;
    bool level_override = false;
    hh_log_level_t level_value = HH_LOG_INFO;
    int err_line = 0;

    hh_radiod_config_defaults(&cfg);

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-c") && i + 1 < argc)      cfg_path = argv[++i];
        else if (!strcmp(argv[i], "-s") && i + 1 < argc) sock_override = argv[++i];
        else if (!strcmp(argv[i], "-v") && i + 1 < argc) {
            if (!hh_log_level_parse(argv[++i], &level_value)) { usage(argv[0]); return 2; }
            level_override = true;
        } else { usage(argv[0]); return 2; }
    }

    if (cfg_path) {
        st = hh_radiod_config_load_file(&cfg, cfg_path, &err_line);
        if (st != HH_OK) {
            fprintf(stderr, "radiod: config error in %s", cfg_path);
            if (err_line) fprintf(stderr, " at line %d", err_line);
            fprintf(stderr, ": %s\n", hh_status_str(st));
            return 1;
        }
    }

    /* Command line wins over the file, so an operator can override a deployed
     * configuration without editing it. */
    if (sock_override) {
        if (hh_radiod_config_set(&cfg, "sock_path", sock_override) != HH_OK) {
            fprintf(stderr, "radiod: invalid socket path\n");
            return 2;
        }
    }
    if (level_override) cfg.log_level = level_value;

    st = hh_radiod_config_validate(&cfg);
    if (st != HH_OK) {
        fprintf(stderr, "radiod: invalid configuration: %s\n", hh_status_str(st));
        return 1;
    }
    hh_log_set_level(cfg.log_level);

    hh_mock_backend_init(&backend, &radio);

    st = hh_radiod_init(&daemon, &radio, hh_clock_monotonic());
    if (st != HH_OK) { fprintf(stderr, "radiod init failed: %s\n", hh_status_str(st)); return 1; }

    st = hh_radiod_configure(&daemon, &cfg);
    if (st != HH_OK) { fprintf(stderr, "radiod configure failed: %s\n", hh_status_str(st)); return 1; }

    hh_radiod_set_fault_hook(&daemon, fault_hook, &backend);

    st = hh_radiod_listen(&daemon, cfg.sock_path);
    if (st != HH_OK) { fprintf(stderr, "radiod listen failed: %s\n", hh_status_str(st)); return 1; }

    install_signal_handlers();

    while (!g_stop) {
        struct pollfd fds[HH_RADIOD_MAX_CLIENTS + 1];
        size_t n = hh_radiod_pollfds(&daemon, fds, sizeof fds / sizeof fds[0]);
        hh_time_ms_t now;

        /* Block until a client needs attention or the tick elapses. The
         * timeout still bounds how long the backend goes unpolled. */
        if (n > 0 && poll(fds, (nfds_t)n, (int)cfg.tick_interval_ms) < 0) {
            if (errno != EINTR) {
                HH_LOGE("radiod", "poll", "errno=%d", errno);
                break;
            }
        }

        now = hh_now(hh_clock_monotonic());
        hh_radiod_tick(&daemon, now);
        if (hh_radiod_shutdown_requested(&daemon)) break;
    }

    hh_radiod_release(&daemon);
    return 0;
}
