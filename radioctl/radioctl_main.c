/*
 * radioctl — the radio-control command-line client.
 *
 * The "radioctl" half of the architecture's librc/radioctl box: the public CLI
 * through which operators and test automation drive radiod. The architecture
 * requires test automation to reach the radio through this public interface
 * rather than through private or internal access, which is precisely what this
 * binary provides.
 *
 * It speaks to radiod only through librc. It does not open the control socket
 * itself, does not link the daemon, and — per the single-owner rule — never
 * touches the PL or OpenCPI. Every radio operation it performs is a request
 * that radiod services.
 *
 * SCOPE: the subcommands below map one-to-one onto the ten command verbs the
 * control protocol already defines (hhsdr/protocol/rc.h). No verb, parameter,
 * or output field is invented here; where the protocol carries no value for
 * something, radioctl reports nothing for it.
 */
#include "hhsdr/librc/rc_client.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define EXIT_USAGE    2   /* bad invocation                                */
#define EXIT_TRANSPORT 3  /* could not reach radiod                        */
#define EXIT_DENIED   4   /* radiod answered, but rejected the request     */

static void usage(const char *argv0)
{
    fprintf(stderr,
        "usage: %s [-s sock_path] <command> [args]\n"
        "\n"
        "Commands (these are the control protocol's verbs; there are no others):\n"
        "  init                      move radiod to INITIALIZED\n"
        "  configure <node_id> [ch]  set node id (and optional channel)\n"
        "  start                     open the radio backend; move to RUNNING\n"
        "  stop                      close the backend; move to STOPPED\n"
        "  status                    report lifecycle state and radio status\n"
        "  stats                     report frame and request counters\n"
        "  set-channel <channel>     request a channel change\n"
        "  inject-fault <kind>       inject a fault for validation\n"
        "  clear-fault               clear an injected fault\n"
        "  shutdown                  release the daemon; it exits afterwards\n"
        "\n"
        "Fault kinds: none tx_failure rx_silence hw_fault backend_io\n"
        "\n"
        "Options:\n"
        "  -s <path>  radiod control socket (default %s)\n"
        "\n"
        "Exit status: 0 success, %d usage error, %d cannot reach radiod,\n"
        "             %d request rejected by radiod\n",
        argv0, HH_RC_DEFAULT_SOCK_PATH,
        EXIT_USAGE, EXIT_TRANSPORT, EXIT_DENIED);
}

/* Map a CLI subcommand to a protocol verb. The CLI spells multi-word verbs
 * with a hyphen (set-channel) while the wire protocol uses an underscore
 * (set_channel); both spellings are accepted so scripts written against either
 * convention work. */
static bool parse_command(const char *s, hh_rc_cmd_t *out)
{
    if (!strcmp(s, "set-channel"))  { *out = HH_RC_CMD_SET_CHANNEL;  return true; }
    if (!strcmp(s, "inject-fault")) { *out = HH_RC_CMD_INJECT_FAULT; return true; }
    if (!strcmp(s, "clear-fault"))  { *out = HH_RC_CMD_CLEAR_FAULT;  return true; }
    return hh_rc_cmd_parse(s, out);
}

/* Print the fields the protocol actually carries for this response, and
 * nothing more. STATUS and STATS have defined payloads; every other verb
 * carries only the resulting lifecycle state. */
static void print_response(const hh_rc_response_t *r)
{
    switch (r->cmd) {
    case HH_RC_CMD_STATUS:
        printf("state=%s\n",        hh_rc_state_str(r->state));
        printf("operational=%d\n",  r->operational ? 1 : 0);
        printf("channel=%u\n",      r->channel);
        printf("frequency_hz=%.1f\n", (double)r->frequency_hz);
        printf("waveform_id=%u\n",  r->waveform_id);
        break;
    case HH_RC_CMD_STATS:
        printf("frames_tx=%llu\n",         (unsigned long long)r->frames_tx);
        printf("frames_rx=%llu\n",         (unsigned long long)r->frames_rx);
        printf("tx_errors=%llu\n",         (unsigned long long)r->tx_errors);
        printf("rx_errors=%llu\n",         (unsigned long long)r->rx_errors);
        printf("requests_total=%llu\n",    (unsigned long long)r->requests_total);
        printf("requests_rejected=%llu\n", (unsigned long long)r->requests_rejected);
        break;
    default:
        printf("state=%s\n", hh_rc_state_str(r->state));
        break;
    }
}

int main(int argc, char **argv)
{
    const char *sock_path = HH_RC_DEFAULT_SOCK_PATH;
    hh_rc_client_t client;
    hh_rc_request_t req;
    hh_rc_response_t resp;
    hh_status_t st;
    int argi = 1;

    memset(&req, 0, sizeof req);

    while (argi < argc && argv[argi][0] == '-') {
        if (!strcmp(argv[argi], "-s") && argi + 1 < argc) {
            sock_path = argv[++argi];
            argi++;
        } else if (!strcmp(argv[argi], "-h") || !strcmp(argv[argi], "--help")) {
            usage(argv[0]);
            return 0;
        } else {
            usage(argv[0]);
            return EXIT_USAGE;
        }
    }

    if (argi >= argc) { usage(argv[0]); return EXIT_USAGE; }

    if (!parse_command(argv[argi], &req.cmd)) {
        fprintf(stderr, "radioctl: unknown command '%s'\n", argv[argi]);
        usage(argv[0]);
        return EXIT_USAGE;
    }
    argi++;

    /* Per-command arguments. Only CONFIGURE, SET_CHANNEL and INJECT_FAULT
     * carry any: those are the only verbs the protocol gives parameters to. */
    switch (req.cmd) {
    case HH_RC_CMD_CONFIGURE:
        if (argi >= argc) {
            fprintf(stderr, "radioctl: configure requires <node_id>\n");
            return EXIT_USAGE;
        }
        req.node_id = (hh_node_id_t)strtoul(argv[argi++], NULL, 10);
        if (argi < argc) req.channel = (uint32_t)strtoul(argv[argi++], NULL, 10);
        break;

    case HH_RC_CMD_SET_CHANNEL:
        if (argi >= argc) {
            fprintf(stderr, "radioctl: set-channel requires <channel>\n");
            return EXIT_USAGE;
        }
        req.channel = (uint32_t)strtoul(argv[argi++], NULL, 10);
        break;

    case HH_RC_CMD_INJECT_FAULT:
        if (argi >= argc) {
            fprintf(stderr, "radioctl: inject-fault requires <kind>\n");
            return EXIT_USAGE;
        }
        if (!hh_rc_fault_parse(argv[argi], &req.fault)) {
            fprintf(stderr, "radioctl: unknown fault kind '%s'\n", argv[argi]);
            return EXIT_USAGE;
        }
        argi++;
        break;

    default:
        break;
    }

    if (argi < argc) {
        fprintf(stderr, "radioctl: unexpected argument '%s'\n", argv[argi]);
        return EXIT_USAGE;
    }

    st = hh_rc_client_connect(&client, sock_path);
    if (st != HH_OK) {
        fprintf(stderr, "radioctl: cannot connect to radiod at %s (%s)\n",
                sock_path, hh_status_str(st));
        return EXIT_TRANSPORT;
    }

    st = hh_rc_client_call(&client, &req, &resp);
    hh_rc_client_close(&client);

    if (st != HH_OK) {
        fprintf(stderr, "radioctl: %s failed in transport (%s)\n",
                hh_rc_cmd_str(req.cmd), hh_status_str(st));
        return EXIT_TRANSPORT;
    }

    /* radiod answered. A rejection is a successful exchange carrying a refusal,
     * so it is reported distinctly from a transport failure. */
    if (!resp.ok) {
        fprintf(stderr, "radioctl: %s rejected (%s)\n",
                hh_rc_cmd_str(resp.cmd), hh_status_str(resp.reason));
        return EXIT_DENIED;
    }

    print_response(&resp);
    return 0;
}
