#include "hhsdr/protocol/rc_cli.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CLI_ERR(...) \
    do { if (err != NULL) (void)snprintf(err, err_len, __VA_ARGS__); return HH_ERR_INVAL; } while (0)

/* Copy an argument into a fixed request field, refusing rather than
 * truncating: a truncated property name addresses a different property. */
static bool arg_copy(char *dst, size_t cap, const char *src)
{
    size_t n = strlen(src);
    if (n == 0u || n >= cap) return false;
    memcpy(dst, src, n + 1u);
    return true;
}

/* A value must survive the space-delimited wire framing unaltered. */
static bool arg_is_token(const char *s)
{
    for (; *s != '\0'; s++) {
        if (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r') return false;
    }
    return true;
}

static bool parse_verb(const char *s, hh_rc_cmd_t *out)
{
    if (!strcmp(s, "set-channel"))  { *out = HH_RC_CMD_SET_CHANNEL;  return true; }
    if (!strcmp(s, "inject-fault")) { *out = HH_RC_CMD_INJECT_FAULT; return true; }
    if (!strcmp(s, "clear-fault"))  { *out = HH_RC_CMD_CLEAR_FAULT;  return true; }
    return hh_rc_cmd_parse(s, out);
}

/*
 * The `opencpi` sub-verbs.
 *
 * `start`/`stop` are sugar for the worker's own `enable` property, which is a
 * real Writable property on both qpsk_tx and qpsk_rx — not an invented
 * operation. Spelling it as a sub-verb is what makes
 *   opencpi start qpsk_tx
 * mean "run that worker" while the bare `start` verb keeps its existing
 * meaning of "bring the whole application up". The two are different actions
 * and are deliberately not merged.
 */
static hh_status_t parse_opencpi(int argc, char *const *argv, hh_rc_request_t *out,
                                 char *err, size_t err_len)
{
    const char *sub;

    if (argc < 2) CLI_ERR("opencpi needs a sub-command: list, get, set, status, start, stop");
    sub = argv[1];

    if (!strcmp(sub, "list")) {
        out->cmd = HH_RC_CMD_OPENCPI_LIST;
        if (argc > 3) CLI_ERR("opencpi list takes at most one worker name");
        if (argc == 3 && !arg_copy(out->worker, sizeof out->worker, argv[2]))
            CLI_ERR("worker name is empty or longer than %d characters", HH_RC_MAX_NAME - 1);
        return HH_OK;
    }
    if (!strcmp(sub, "status")) {
        out->cmd = HH_RC_CMD_OPENCPI_STATUS;
        if (argc != 2) CLI_ERR("opencpi status takes no arguments");
        return HH_OK;
    }
    if (!strcmp(sub, "get")) {
        out->cmd = HH_RC_CMD_OPENCPI_GET;
        if (argc != 4) CLI_ERR("usage: opencpi get <worker> <property>");
        if (!arg_copy(out->worker, sizeof out->worker, argv[2])) CLI_ERR("bad worker name");
        if (!arg_copy(out->prop, sizeof out->prop, argv[3])) CLI_ERR("bad property name");
        return HH_OK;
    }
    if (!strcmp(sub, "set")) {
        out->cmd = HH_RC_CMD_OPENCPI_SET;
        if (argc != 5) CLI_ERR("usage: opencpi set <worker> <property> <value>");
        if (!arg_copy(out->worker, sizeof out->worker, argv[2])) CLI_ERR("bad worker name");
        if (!arg_copy(out->prop, sizeof out->prop, argv[3])) CLI_ERR("bad property name");
        if (!arg_is_token(argv[4]))
            CLI_ERR("a property value may not contain whitespace");
        if (!arg_copy(out->value, sizeof out->value, argv[4]))
            CLI_ERR("value is empty or longer than %d characters", HH_RC_MAX_VALUE - 1);
        return HH_OK;
    }
    if (!strcmp(sub, "start") || !strcmp(sub, "stop")) {
        out->cmd = HH_RC_CMD_OPENCPI_SET;
        if (argc != 3) CLI_ERR("usage: opencpi %s <worker>", sub);
        if (!arg_copy(out->worker, sizeof out->worker, argv[2])) CLI_ERR("bad worker name");
        (void)snprintf(out->prop, sizeof out->prop, "enable");
        (void)snprintf(out->value, sizeof out->value, "%s",
                       (strcmp(sub, "start") == 0) ? "1" : "0");
        return HH_OK;
    }
    CLI_ERR("unknown opencpi sub-command '%s'", sub);
}

hh_status_t hh_rc_cli_parse(int argc, char *const *argv, hh_rc_request_t *out,
                            char *err, size_t err_len)
{
    if (out == NULL || argv == NULL || argc < 1) return HH_ERR_INVAL;
    memset(out, 0, sizeof *out);
    if (err != NULL && err_len > 0u) err[0] = '\0';

    if (!strcmp(argv[0], "opencpi")) return parse_opencpi(argc, argv, out, err, err_len);

    if (!parse_verb(argv[0], &out->cmd)) {
        if (err != NULL) (void)snprintf(err, err_len, "unknown command '%s'", argv[0]);
        return HH_ERR_NOTFOUND;
    }

    switch (out->cmd) {
    case HH_RC_CMD_CONFIGURE:
        if (argc < 2) CLI_ERR("usage: configure <node_id> [channel]");
        if (argc > 3) CLI_ERR("configure takes <node_id> and an optional channel");
        out->node_id = (hh_node_id_t)strtoul(argv[1], NULL, 10);
        if (argc == 3) out->channel = (uint32_t)strtoul(argv[2], NULL, 10);
        return HH_OK;

    case HH_RC_CMD_SET_CHANNEL:
        if (argc != 2) CLI_ERR("usage: set-channel <channel>");
        out->channel = (uint32_t)strtoul(argv[1], NULL, 10);
        return HH_OK;

    case HH_RC_CMD_INJECT_FAULT:
        if (argc != 2) CLI_ERR("usage: inject-fault <none|tx_failure|rx_silence|hw_fault|backend_io>");
        if (!hh_rc_fault_parse(argv[1], &out->fault)) CLI_ERR("unknown fault kind '%s'", argv[1]);
        return HH_OK;

    /* The opencpi_* wire spellings are accepted too, for a script written
     * against the protocol rather than against this CLI; they take their
     * arguments positionally here, exactly as the `opencpi` sub-verbs do. */
    case HH_RC_CMD_OPENCPI_LIST:
    case HH_RC_CMD_OPENCPI_GET:
    case HH_RC_CMD_OPENCPI_SET:
    case HH_RC_CMD_OPENCPI_STATUS: {
        char *sub[HH_RC_CLI_MAX_ARGS];
        int i;
        static char kw_list[]   = "list";
        static char kw_get[]    = "get";
        static char kw_set[]    = "set";
        static char kw_status[] = "status";
        if (argc > HH_RC_CLI_MAX_ARGS - 1) CLI_ERR("too many arguments");
        sub[0] = (char *)"opencpi";
        sub[1] = (out->cmd == HH_RC_CMD_OPENCPI_LIST)   ? kw_list
               : (out->cmd == HH_RC_CMD_OPENCPI_GET)    ? kw_get
               : (out->cmd == HH_RC_CMD_OPENCPI_SET)    ? kw_set
                                                        : kw_status;
        for (i = 1; i < argc; i++) sub[i + 1] = argv[i];
        return parse_opencpi(argc + 1, sub, out, err, err_len);
    }

    default:
        if (argc != 1) CLI_ERR("%s takes no arguments", argv[0]);
        return HH_OK;
    }
}

hh_status_t hh_rc_cli_parse_line(char *line, hh_rc_request_t *out,
                                 char *err, size_t err_len)
{
    char *argv[HH_RC_CLI_MAX_ARGS];
    int argc = 0;
    char *p = line;

    if (line == NULL || out == NULL) return HH_ERR_INVAL;

    while (*p != '\0') {
        while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
        if (*p == '\0') break;
        if (argc == HH_RC_CLI_MAX_ARGS) {
            if (err != NULL) (void)snprintf(err, err_len, "too many words on the line");
            return HH_ERR_INVAL;
        }
        argv[argc++] = p;
        while (*p != '\0' && *p != ' ' && *p != '\t' && *p != '\n' && *p != '\r') p++;
        if (*p != '\0') *p++ = '\0';
    }

    if (argc == 0) return HH_ERR_AGAIN;   /* blank line: nothing to report */
    return hh_rc_cli_parse(argc, argv, out, err, err_len);
}

/* Append to buf without ever overflowing it; *used tracks the length so far. */
static void app(char *buf, size_t cap, size_t *used, const char *fmt, ...)
    __attribute__((format(printf, 4, 5)));

static void app(char *buf, size_t cap, size_t *used, const char *fmt, ...)
{
    va_list ap;
    int n;
    if (*used + 1u >= cap) return;
    va_start(ap, fmt);
    n = vsnprintf(buf + *used, cap - *used, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    *used = ((size_t)n >= cap - *used) ? cap - 1u : *used + (size_t)n;
}

/*
 * Expand a comma-separated item list to one item per line. A 600-character
 * comma list is machine-readable but not human-readable, and this is the human
 * renderer.
 *
 * Three element shapes occur, and each reads best differently:
 *   "name"                 a worker name         -> printed alone
 *   "name:value"           a sampled property    -> printed as name=value
 *   "name:type:access"     a published property  -> printed in columns, so a
 *                                                   list of twenty is scannable
 */
static void render_items(const char *items, char *buf, size_t cap, size_t *used)
{
    const char *p = items;
    while (*p != '\0') {
        const char *comma = strchr(p, ',');
        size_t len = (comma != NULL) ? (size_t)(comma - p) : strlen(p);
        const char *c1 = memchr(p, ':', len);
        const char *c2 = NULL;

        if (c1 != NULL) {
            size_t rest = len - (size_t)(c1 - p) - 1u;
            c2 = memchr(c1 + 1, ':', rest);
        }

        if (c2 != NULL) {
            app(buf, cap, used, "%-22.*s %-10.*s %.*s\n",
                (int)(c1 - p), p,
                (int)(c2 - c1 - 1), c1 + 1,
                (int)(len - (size_t)(c2 - p) - 1u), c2 + 1);
        } else if (c1 != NULL) {
            app(buf, cap, used, "%.*s=%.*s\n", (int)(c1 - p), p,
                (int)(len - (size_t)(c1 - p) - 1u), c1 + 1);
        } else {
            app(buf, cap, used, "%.*s\n", (int)len, p);
        }
        if (comma == NULL) break;
        p = comma + 1;
    }
}

void hh_rc_cli_render(const hh_rc_response_t *r, char *buf, size_t cap)
{
    size_t used = 0;

    if (buf == NULL || cap == 0u) return;
    buf[0] = '\0';
    if (r == NULL) return;

    switch (r->cmd) {
    case HH_RC_CMD_STATUS:
        app(buf, cap, &used, "state=%s\n",        hh_rc_state_str(r->state));
        app(buf, cap, &used, "operational=%d\n",  r->operational ? 1 : 0);
        app(buf, cap, &used, "channel=%u\n",      r->channel);
        app(buf, cap, &used, "frequency_hz=%.1f\n", (double)r->frequency_hz);
        app(buf, cap, &used, "waveform_id=%u\n",  r->waveform_id);
        break;
    case HH_RC_CMD_STATS:
        app(buf, cap, &used, "frames_tx=%llu\n",         (unsigned long long)r->frames_tx);
        app(buf, cap, &used, "frames_rx=%llu\n",         (unsigned long long)r->frames_rx);
        app(buf, cap, &used, "tx_errors=%llu\n",         (unsigned long long)r->tx_errors);
        app(buf, cap, &used, "rx_errors=%llu\n",         (unsigned long long)r->rx_errors);
        app(buf, cap, &used, "requests_total=%llu\n",    (unsigned long long)r->requests_total);
        app(buf, cap, &used, "requests_rejected=%llu\n", (unsigned long long)r->requests_rejected);
        break;
    case HH_RC_CMD_OPENCPI_LIST:
    case HH_RC_CMD_OPENCPI_STATUS:
        if (r->worker[0] != '\0') app(buf, cap, &used, "worker=%s\n", r->worker);
        app(buf, cap, &used, "count=%u\n", r->count);
        render_items(r->items, buf, cap, &used);
        break;
    case HH_RC_CMD_OPENCPI_GET:
        app(buf, cap, &used, "worker=%s\n", r->worker);
        app(buf, cap, &used, "property=%s\n", r->prop);
        app(buf, cap, &used, "value=%s\n", r->value);
        app(buf, cap, &used, "type=%s\n", r->ptype);
        app(buf, cap, &used, "access=%s\n", r->access);
        break;
    case HH_RC_CMD_OPENCPI_SET:
        app(buf, cap, &used, "worker=%s\n", r->worker);
        app(buf, cap, &used, "property=%s\n", r->prop);
        app(buf, cap, &used, "value=%s\n", r->value);
        break;
    default:
        app(buf, cap, &used, "state=%s\n", hh_rc_state_str(r->state));
        break;
    }

    /* Detail the backend supplied, when there is any. Printed last so the
     * machine-readable fields above keep a fixed order. */
    if (r->text[0] != '\0') app(buf, cap, &used, "detail=%s\n", r->text);
}

const char *hh_rc_cli_usage(void)
{
    return
    "Radio lifecycle and status (the control protocol's own verbs):\n"
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
    "Worker properties (serviced by whichever backend radiod is running):\n"
    "  opencpi list              list the workers the backend publishes\n"
    "  opencpi list <worker>     list that worker's properties, with\n"
    "                            type and access (rw | rw-start | ro)\n"
    "  opencpi get <worker> <property>\n"
    "  opencpi set <worker> <property> <value>\n"
    "  opencpi status            sample every readable property at once\n"
    "  opencpi start <worker>    shorthand for: set <worker> enable 1\n"
    "  opencpi stop  <worker>    shorthand for: set <worker> enable 0\n"
    "\n"
    "  `start`/`stop` bring the whole OpenCPI application up and down.\n"
    "  `opencpi start/stop <worker>` runs or halts one worker inside it.\n"
    "  A property marked rw-start can only be written while the application\n"
    "  is NOT running; writing one while it runs is refused, not queued.\n"
    "\n"
    "Fault kinds: none tx_failure rx_silence hw_fault backend_io\n";
}
