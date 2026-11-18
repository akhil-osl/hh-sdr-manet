/*
 * hh-manet — node daemon entry point.
 *
 * Wires configuration, the selected radio adapter, and the assembled node, then
 * runs the control loop. Kept deliberately thin: it contains no MANET logic.
 *
 * With radio_adapter="hw" the node will fail to start, because the FPGA/PL
 * contract does not exist yet and the hardware adapter reports that honestly
 * rather than pretending to run. That is the intended behavior.
 *
 * Legacy: runs the distance-vector node. Routing is OLSRd2 (unknown.md U-09).
 */
#include "hhsdr/manet/node.h"
#include "hhsdr/manet/telemetry.h"
#include "hhsdr/radio/hw_adapter.h"
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static volatile sig_atomic_t g_stop = 0;

static void on_signal(int sig) { (void)sig; g_stop = 1; }

static void usage(const char *argv0)
{
    fprintf(stderr,
        "usage: %s [-c config] [-n node_id] [-t seconds] [-v level]\n"
        "  -c  configuration file (key = value)\n"
        "  -n  node id, overriding the configuration\n"
        "  -t  run for N seconds then exit (0 = run until signalled)\n"
        "  -v  log level: error|warn|info|debug|trace\n", argv0);
}

int main(int argc, char **argv)
{
    hh_config_t cfg;
    hh_hw_adapter_t hw;
    hh_radio_t radio;
    hh_node_t node;
    hh_status_t st;
    const char *cfg_path = NULL;
    unsigned run_seconds = 0;
    int err_line = 0;

    hh_config_defaults(&cfg);

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-c") && i + 1 < argc)      cfg_path = argv[++i];
        else if (!strcmp(argv[i], "-n") && i + 1 < argc) cfg.node_id = (hh_node_id_t)strtoul(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "-t") && i + 1 < argc) run_seconds = (unsigned)strtoul(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "-v") && i + 1 < argc) {
            if (!hh_log_level_parse(argv[++i], &cfg.log_level)) { usage(argv[0]); return 2; }
        } else { usage(argv[0]); return 2; }
    }

    if (cfg_path) {
        hh_node_id_t cli_id = cfg.node_id;
        st = hh_config_load_file(&cfg, cfg_path, &err_line);
        if (st != HH_OK) {
            fprintf(stderr, "config error in %s", cfg_path);
            if (err_line) fprintf(stderr, " at line %d", err_line);
            fprintf(stderr, ": %s\n", hh_status_str(st));
            return 1;
        }
        /* An explicit -n overrides the file, which is the usual precedence. */
        if (cli_id != HH_NODE_ID_INVALID) cfg.node_id = cli_id;
    }

    hh_log_set_level(cfg.log_level);

    st = hh_config_validate(&cfg);
    if (st != HH_OK) {
        fprintf(stderr, "invalid configuration: %s%s\n", hh_status_str(st),
                cfg.node_id == HH_NODE_ID_INVALID ? " (node_id not set; use -n)" : "");
        return 1;
    }

    /* Only the hardware adapter is available to production. The mock lives in
     * tests/ and is deliberately not selectable from here. */
    if (strcmp(cfg.radio_adapter, "hw") != 0) {
        fprintf(stderr, "unknown radio adapter '%s' (only \"hw\" is built into "
                        "the production binary)\n", cfg.radio_adapter);
        return 1;
    }
    hh_hw_adapter_init(&hw, &radio);

    st = hh_node_init(&node, &cfg, hh_clock_monotonic(), &radio);
    if (st != HH_OK) { fprintf(stderr, "node init failed: %s\n", hh_status_str(st)); return 1; }

    hh_node_configure(&node, NULL);

    st = hh_node_start(&node);
    if (st != HH_OK) {
        fprintf(stderr,
            "node start failed: %s\n"
            "\n"
            "The radio adapter has no hardware backend: the FPGA/PL software API\n"
            "is not available, so no radio contract can be honored yet. See\n"
            "docs/HARDWARE-DEPENDENCIES.md for what FPGA integration must supply.\n"
            "The MANET stack itself is exercised by the test suite (ctest), which\n"
            "drives this same code through a test radio adapter.\n",
            hh_status_str(st));
        return 1;
    }

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    while (!g_stop) {
        struct timespec ts = { 0, 10 * 1000 * 1000 };   /* 10 ms control loop */
        hh_time_ms_t now = hh_now(hh_clock_monotonic());
        hh_node_tick(&node, now);
        if (run_seconds && now / 1000 >= run_seconds) break;
        nanosleep(&ts, NULL);
    }

    hh_telemetry_dump(&node, stdout);
    hh_node_release(&node);
    return 0;
}
