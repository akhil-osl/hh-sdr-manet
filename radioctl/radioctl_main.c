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
 * that radiod services, including every worker-property operation: the ACI
 * lives in radiod's helper process, never here.
 *
 * SCOPE: the subcommands map onto the command verbs the control protocol
 * defines (hhsdr/protocol/rc.h). No verb, parameter, or output field is
 * invented here. The grammar and the output layout both live in rc_cli.c,
 * shared with radiod's interactive console so that one command means the same
 * thing wherever it is typed.
 */
#include "hhsdr/librc/rc_client.h"
#include "hhsdr/protocol/rc_cli.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define EXIT_USAGE    2   /* bad invocation                                */
#define EXIT_TRANSPORT 3  /* could not reach radiod                        */
#define EXIT_DENIED   4   /* radiod answered, but rejected the request     */

/* One line of rendered output can hold an expanded item list, which is the
 * longest thing any response produces. */
#define RENDER_CAP (HH_RC_MAX_LINE * 2)

static void usage(const char *argv0)
{
    fprintf(stderr,
        "usage: %s [-s sock_path] <command> [args]\n"
        "       %s [-s sock_path] shell\n"
        "\n"
        "%s"
        "\n"
        "Options:\n"
        "  -s <path>  radiod control socket (default %s)\n"
        "\n"
        "`shell` opens an interactive prompt on one connection, which is the\n"
        "same prompt `radiod -i` offers and takes the same commands.\n"
        "\n"
        "Exit status: 0 success, %d usage error, %d cannot reach radiod,\n"
        "             %d request rejected by radiod\n",
        argv0, argv0, hh_rc_cli_usage(), HH_RC_DEFAULT_SOCK_PATH,
        EXIT_USAGE, EXIT_TRANSPORT, EXIT_DENIED);
}

/* Report a rejection the same way everywhere: radiod answered, so this is a
 * refusal carrying a reason, reported distinctly from a transport failure.
 * The backend's own sentence is printed when there is one — for a property
 * write that is the difference between "ESTATE" and "sym_mode can only be set
 * while the application is stopped". */
static void print_rejection(const hh_rc_response_t *r)
{
    fprintf(stderr, "radioctl: %s rejected (%s)", hh_rc_cmd_str(r->cmd),
            hh_status_str(r->reason));
    if (r->text[0] != '\0') fprintf(stderr, ": %s", r->text);
    fprintf(stderr, "\n");
}

/* Interactive mode: one connection, many commands. Holding the connection
 * open matters for more than speed — radiod's lifecycle state is per-daemon,
 * not per-connection, so a session reads naturally as a sequence of steps. */
static int run_shell(hh_rc_client_t *client)
{
    char line[HH_RC_MAX_LINE];

    printf("radioctl shell. `help` lists commands, `quit` exits.\n");
    for (;;) {
        hh_rc_request_t req;
        hh_rc_response_t resp;
        char err[160];
        char out[RENDER_CAP];
        hh_status_t st;

        printf("radiod> ");
        (void)fflush(stdout);
        if (!fgets(line, sizeof line, stdin)) { printf("\n"); return 0; }

        if (!strcmp(line, "quit\n") || !strcmp(line, "exit\n")) return 0;
        if (!strcmp(line, "help\n") || !strcmp(line, "?\n")) {
            fputs(hh_rc_cli_usage(), stdout);
            continue;
        }

        st = hh_rc_cli_parse_line(line, &req, err, sizeof err);
        if (st == HH_ERR_AGAIN) continue;             /* blank line */
        if (st != HH_OK) { fprintf(stderr, "error: %s\n", err); continue; }

        if (hh_rc_client_call(client, &req, &resp) != HH_OK) {
            fprintf(stderr, "radioctl: transport error; radiod is gone\n");
            return EXIT_TRANSPORT;
        }
        if (!resp.ok) { print_rejection(&resp); continue; }

        hh_rc_cli_render(&resp, out, sizeof out);
        fputs(out, stdout);

        /* radiod exits after answering shutdown, so the connection is over. */
        if (req.cmd == HH_RC_CMD_SHUTDOWN) return 0;
    }
}

int main(int argc, char **argv)
{
    const char *sock_path = HH_RC_DEFAULT_SOCK_PATH;
    hh_rc_client_t client;
    hh_rc_request_t req;
    hh_rc_response_t resp;
    char err[160];
    char out[RENDER_CAP];
    hh_status_t st;
    bool shell = false;
    int argi = 1;

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

    if (!strcmp(argv[argi], "shell")) {
        if (argi + 1 != argc) {
            fprintf(stderr, "radioctl: shell takes no arguments\n");
            return EXIT_USAGE;
        }
        shell = true;
    } else {
        st = hh_rc_cli_parse(argc - argi, &argv[argi], &req, err, sizeof err);
        if (st == HH_ERR_NOTFOUND) {
            fprintf(stderr, "radioctl: %s\n", err);
            usage(argv[0]);
            return EXIT_USAGE;
        }
        if (st != HH_OK) {
            fprintf(stderr, "radioctl: %s\n", err);
            return EXIT_USAGE;
        }
    }

    st = hh_rc_client_connect(&client, sock_path);
    if (st != HH_OK) {
        fprintf(stderr, "radioctl: cannot connect to radiod at %s (%s)\n",
                sock_path, hh_status_str(st));
        return EXIT_TRANSPORT;
    }

    if (shell) {
        int rc = run_shell(&client);
        hh_rc_client_close(&client);
        return rc;
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
        print_rejection(&resp);
        return EXIT_DENIED;
    }

    hh_rc_cli_render(&resp, out, sizeof out);
    fputs(out, stdout);
    return 0;
}
