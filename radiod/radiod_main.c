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
 * BACKEND: selected by the `radio_backend` configuration key, and this file is
 * the only place that knows which concrete backends exist.
 *
 *   mock     (default)  the deterministic control-plane backend
 *   opencpi             the real PL, reached through the rh_agent helper and
 *                       the RADHAWK API (see hhsdr/radiod/opencpi.h)
 *
 * Both implement hh_radio_ops_t, so nothing in radiod.c distinguishes them.
 * The OpenCPI backend additionally publishes a worker-property control surface
 * (props.h), which is what the opencpi_* verbs are serviced from; the mock
 * publishes none, and those verbs then answer HH_ERR_UNSUPPORTED.
 *
 * The default stays "mock" deliberately: a radio is something an operator opts
 * into, and every existing deployment keeps its previous behaviour.
 *
 * Upstream hh-sdr-manet adds a third choice, `backend = ocpi` (separate key):
 * `mock` (the default) is
 * the control-plane validation backend. `ocpi` makes radiod the owner of an
 * OpenCPI application — the single-PL-owner role of the target architecture —
 * and exists only in builds configured with -DHH_WITH_OPENCPI=ON. Both
 * implement hh_radio_ops_t, so nothing below this file depends on the choice.
 */
#include "hhsdr/core/clock.h"
#include "hhsdr/core/log.h"
#include "hhsdr/radiod/config.h"
#include "hhsdr/radiod/mock_backend.h"
#include "hhsdr/radiod/opencpi.h"
#include "hhsdr/radiod/ocpi_backend.h"
#include "hhsdr/radiod/radiod.h"
#include "hhsdr/radiod/shm_ring.h"
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

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
        "usage: %s [-c config] [-s sock_path] [-b backend] [-a agent]\n"
        "            [-v level] [-i]\n"
        "  -c  configuration file (key = value)\n"
        "  -s  UNIX domain socket path (default %s)\n"
        "  -b  radio backend: mock (default) | opencpi\n"
        "  -a  rh_agent binary, for the opencpi backend\n"
        "  -v  log level: error|warn|info|debug|trace\n"
        "  -i  also serve an interactive console on this terminal\n"
        "\n"
        "Command-line options override the configuration file.\n"
        "\n"
        "The control socket is always served. -i adds a prompt on stdin for an\n"
        "operator sitting at the terminal; it accepts exactly the commands\n"
        "radioctl takes, and `help` lists them.\n",
        argv0, HH_RC_DEFAULT_SOCK_PATH);
}

/* Construct the configured backend into `radio`. The OpenCPI backend is
 * compiled only when OpenCPI is available; asking for it otherwise is a
 * configuration the daemon cannot honour, so it says so and refuses to start
 * rather than quietly falling back to the mock. */
static hh_status_t make_backend(const hh_radiod_config_t *cfg, hh_mock_backend_t *mock,
                                hh_ocpi_backend_t *ocpi, hh_radio_t *radio)
{
    switch (cfg->backend) {
    case HH_RADIOD_BACKEND_MOCK:
        hh_mock_backend_init(mock, radio);
        return HH_OK;
    case HH_RADIOD_BACKEND_OCPI:
#ifdef HH_HAVE_OPENCPI
        return hh_ocpi_backend_init(ocpi, &cfg->ocpi, radio);
#else
        (void)ocpi;
        HH_LOGE("radiod", "backend_unavailable",
                "backend=ocpi reason=\"radiod was built without OpenCPI; configure with -DHH_WITH_OPENCPI=ON\"");
        return HH_ERR_UNSUPPORTED;
#endif
    }
    return HH_ERR_INVAL;
}

int main(int argc, char **argv)
{
    hh_radiod_config_t cfg;
    hh_mock_backend_t backend;
    hh_oc_backend_t   oc_backend;
    hh_ocpi_backend_t ocpi_backend;
    hh_radio_t radio;
    hh_radiod_props_t props;
    hh_shm_ring_t rx_frames;   /* the third ring: received frames, owned here */
    hh_radiod_t daemon;
    hh_status_t st;
    const char *cfg_path = NULL;
    const char *sock_override = NULL;
    const char *backend_override = NULL;
    const char *agent_override = NULL;
    bool level_override = false;
    bool interactive = false;
    bool use_opencpi = false;
    hh_log_level_t level_value = HH_LOG_INFO;
    int err_line = 0;

    memset(&props, 0, sizeof props);

    hh_radiod_config_defaults(&cfg);

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-c") && i + 1 < argc)      cfg_path = argv[++i];
        else if (!strcmp(argv[i], "-s") && i + 1 < argc) sock_override = argv[++i];
        else if (!strcmp(argv[i], "-b") && i + 1 < argc) backend_override = argv[++i];
        else if (!strcmp(argv[i], "-a") && i + 1 < argc) agent_override = argv[++i];
        else if (!strcmp(argv[i], "-i")) interactive = true;
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
    if (backend_override) {
        if (hh_radiod_config_set(&cfg, "radio_backend", backend_override) != HH_OK) {
            fprintf(stderr, "radiod: unknown backend '%s' (want mock or opencpi)\n",
                    backend_override);
            return 2;
        }
    }
    if (agent_override) {
        if (hh_radiod_config_set(&cfg, "opencpi_agent", agent_override) != HH_OK) {
            fprintf(stderr, "radiod: invalid agent path\n");
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

    use_opencpi = (strcmp(cfg.radio_backend, "opencpi") == 0);
    if (use_opencpi && cfg.backend != HH_RADIOD_BACKEND_MOCK) {
        fprintf(stderr, "radiod: radio_backend = opencpi and backend = %s both select a "
                        "real backend; choose one\n", hh_radiod_backend_str(cfg.backend));
        return 1;
    }
    memset(&rx_frames, 0, sizeof rx_frames);
    if (use_opencpi) {
        /* THE THIRD RING. radiod owns the RX frame ring: rh_agent (another
         * process) attaches to it and pushes every complete received frame,
         * before its own extract/demux. Created before the agent can start.
         * Not fatal: without it the agent only skips the push. The consumer
         * is the MAC, not connected yet, so a full ring counts drops. */
        st = hh_shm_ring_create(&rx_frames, HH_RX_FRAME_RING_PATH,
                                HH_RX_FRAME_RING_SLOT, HH_RX_FRAME_RING_COUNT);
        if (st == HH_OK)
            HH_LOGI("radiod", "rx_frame_ring", "path=%s slots=%u slot_bytes=%u",
                    HH_RX_FRAME_RING_PATH, HH_RX_FRAME_RING_COUNT, HH_RX_FRAME_RING_SLOT);
        else
            fprintf(stderr, "radiod: RX frame ring %s not created: %s\n",
                    HH_RX_FRAME_RING_PATH, hh_status_str(st));
    }
    if (use_opencpi) {
        hh_oc_config_t occ;
        hh_oc_config_defaults(&occ);
        if (cfg.opencpi_agent[0] != '\0')
            snprintf(occ.agent_path, sizeof occ.agent_path, "%s", cfg.opencpi_agent);
        occ.reply_timeout_ms = cfg.opencpi_reply_timeout_ms;
        occ.start_timeout_ms = cfg.opencpi_start_timeout_ms;

        st = hh_oc_backend_init(&oc_backend, &occ, &radio, &props);
        if (st != HH_OK) {
            /* Not fatal, and deliberately so. The backend is bound and every
             * operation on it now fails with this reason attached, so an
             * operator who connects is told what is wrong. Exiting instead
             * would leave them with a daemon that is simply not there and no
             * explanation of why. */
            fprintf(stderr, "radiod: OpenCPI backend unavailable: %s\n",
                    hh_oc_backend_last_error(&oc_backend));
            fprintf(stderr, "radiod: serving anyway; radio operations will "
                            "report this failure\n");
        }
    } else {
        st = make_backend(&cfg, &backend, &ocpi_backend, &radio);
        if (st != HH_OK) {
            fprintf(stderr, "radiod: backend %s unavailable: %s\n",
                    hh_radiod_backend_str(cfg.backend), hh_status_str(st));
            return 1;
        }
        HH_LOGI("radiod", "backend", "kind=%s", hh_radiod_backend_str(cfg.backend));
    }

    st = hh_radiod_init(&daemon, &radio, hh_clock_monotonic());
    if (st != HH_OK) { fprintf(stderr, "radiod init failed: %s\n", hh_status_str(st)); return 1; }

    st = hh_radiod_configure(&daemon, &cfg);
    if (st != HH_OK) { fprintf(stderr, "radiod configure failed: %s\n", hh_status_str(st)); return 1; }

    if (use_opencpi) {
        /* Only the OpenCPI backend publishes worker properties. Fault
         * injection is the mock's, because injecting a fault into real
         * hardware is not something a validation hook can do. */
        hh_radiod_set_props(&daemon, &props);
    } else if (cfg.backend == HH_RADIOD_BACKEND_MOCK) {
        /* With upstream's `ocpi` backend there is no injection point either. */
        hh_radiod_set_fault_hook(&daemon, fault_hook, &backend);
    }

    st = hh_radiod_listen(&daemon, cfg.sock_path);
    if (st != HH_OK) { fprintf(stderr, "radiod listen failed: %s\n", hh_status_str(st)); return 1; }

    st = hh_radiod_open_rings(&daemon, &cfg);
    if (st != HH_OK) { fprintf(stderr, "radiod rings failed: %s\n", hh_status_str(st)); return 1; }

    install_signal_handlers();

    if (interactive) {
        st = hh_radiod_console_enable(&daemon, STDIN_FILENO, STDOUT_FILENO);
        if (st != HH_OK) {
            fprintf(stderr, "radiod: cannot attach the console: %s\n", hh_status_str(st));
            hh_radiod_release(&daemon);
            return 1;
        }
    }

    while (!g_stop) {
        struct pollfd fds[HH_RADIOD_MAX_POLLFDS];
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
    if (use_opencpi) hh_oc_backend_release(&oc_backend);
    hh_shm_ring_close(&rx_frames);
    return 0;
}
