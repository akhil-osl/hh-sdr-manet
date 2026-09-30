/*
 * radiod OpenCPI backend.
 *
 * See hhsdr/radiod/opencpi.h for what this implements and which HTI-02 fields
 * are deliberately left at zero, and hhsdr/radiod/opencpi_agent_proto.h for
 * why the OpenCPI work happens in a helper process rather than in-process.
 *
 * No OpenCPI header is included here and no OpenCPI symbol is referenced, so
 * radiod builds and links identically whether or not a CDK is installed.
 */
#define _GNU_SOURCE
#include "hhsdr/radiod/opencpi.h"
#include "hhsdr/core/log.h"
#include <errno.h>
#include <poll.h>
#include <stdarg.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#define COMP "radiod.opencpi"

#define OC_DEFAULT_REPLY_MS  3000u
#define OC_DEFAULT_START_MS 60000u

/* ------------------------------------------------------------------------- */
/* Small helpers                                                             */
/* ------------------------------------------------------------------------- */

static void oc_err(hh_oc_backend_t *b, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

static void oc_err(hh_oc_backend_t *b, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    (void)vsnprintf(b->last_error, sizeof b->last_error, fmt, ap);
    va_end(ap);
}

/* Copy the backend's failure detail into the response, so the reason a
 * specific request was refused reaches the operator rather than being
 * flattened into a status code. */
static void oc_fail(hh_oc_backend_t *b, hh_rc_response_t *resp)
{
    if (resp != NULL) {
        (void)snprintf(resp->text, sizeof resp->text, "%s", b->last_error);
    }
}

/*
 * Pull one value out of an agent reply line.
 *
 * `msg` is taken as the whole remainder of the line because it is the only
 * field allowed to contain spaces; every other value ends at the next space.
 */
static bool reply_kv(const char *line, const char *key, char *out, size_t cap)
{
    char pat[32];
    const char *at;
    size_t n;

    (void)snprintf(pat, sizeof pat, " %s=", key);
    at = strstr(line, pat);
    if (at == NULL) return false;
    at += strlen(pat);

    if (strcmp(key, "msg") == 0) {
        n = strlen(at);
    } else {
        const char *end = strchr(at, ' ');
        n = (end != NULL) ? (size_t)(end - at) : strlen(at);
    }
    if (n >= cap) return false;
    memcpy(out, at, n);
    out[n] = '\0';
    return true;
}

/*
 * Translate an `err` line's stated failure kind into a status code.
 *
 * Absent means io, which is the safe default: a failure of unknown character is
 * more likely to be a transport or hardware problem than a caller mistake, and
 * calling a real fault "your input was invalid" would send an operator looking
 * in the wrong place.
 */
static hh_status_t reply_kind(const char *line)
{
    char kind[24];
    if (!reply_kv(line, HH_RH_AGENT_KIND, kind, sizeof kind)) return HH_ERR_IO;
    if (strcmp(kind, HH_RH_AGENT_KIND_INVAL) == 0)    return HH_ERR_INVAL;
    if (strcmp(kind, HH_RH_AGENT_KIND_STATE) == 0)    return HH_ERR_STATE;
    if (strcmp(kind, HH_RH_AGENT_KIND_NOTFOUND) == 0) return HH_ERR_NOTFOUND;
    if (strcmp(kind, HH_RH_AGENT_KIND_UNSUP) == 0)    return HH_ERR_UNSUPPORTED;
    return HH_ERR_IO;
}

/* ------------------------------------------------------------------------- */
/* Agent process                                                             */
/* ------------------------------------------------------------------------- */

bool hh_oc_backend_agent_alive(const hh_oc_backend_t *b)
{
    return b != NULL && b->pid > 0 && b->to_agent >= 0 && b->from_agent >= 0;
}

const char *hh_oc_backend_last_error(const hh_oc_backend_t *b)
{
    return (b != NULL) ? b->last_error : "";
}

/* Mark the agent gone and release its descriptors. Called both on an orderly
 * quit and when a read reports the pipe closed, so a crashed agent leaves no
 * half-open state that a later call could mistake for a live one. */
static void agent_drop(hh_oc_backend_t *b)
{
    if (b->to_agent >= 0)   { (void)close(b->to_agent);   b->to_agent = -1; }
    if (b->from_agent >= 0) { (void)close(b->from_agent); b->from_agent = -1; }
    if (b->pid > 0) {
        int status;
        (void)kill(b->pid, SIGTERM);
        (void)waitpid(b->pid, &status, 0);
        b->pid = -1;
    }
    b->session_open = false;
    b->rxlen = 0;
}

static hh_status_t agent_spawn(hh_oc_backend_t *b)
{
    int in_pipe[2];    /* radiod writes in_pipe[1], agent reads in_pipe[0]  */
    int out_pipe[2];   /* agent writes out_pipe[1], radiod reads out_pipe[0] */
    pid_t pid;

    if (pipe(in_pipe) != 0) {
        oc_err(b, "pipe() failed: errno=%d", errno);
        return HH_ERR_IO;
    }
    if (pipe(out_pipe) != 0) {
        oc_err(b, "pipe() failed: errno=%d", errno);
        (void)close(in_pipe[0]); (void)close(in_pipe[1]);
        return HH_ERR_IO;
    }

    pid = fork();
    if (pid < 0) {
        oc_err(b, "fork() failed: errno=%d", errno);
        (void)close(in_pipe[0]); (void)close(in_pipe[1]);
        (void)close(out_pipe[0]); (void)close(out_pipe[1]);
        return HH_ERR_IO;
    }

    if (pid == 0) {
        /* Child. stderr is deliberately left attached to radiod's: the agent's
         * OpenCPI diagnostics are the most useful thing in the log when an
         * application fails to deploy, and discarding them would make a
         * deployment failure indistinguishable from a timeout. */
        (void)dup2(in_pipe[0], STDIN_FILENO);
        (void)dup2(out_pipe[1], STDOUT_FILENO);
        (void)close(in_pipe[0]); (void)close(in_pipe[1]);
        (void)close(out_pipe[0]); (void)close(out_pipe[1]);
        (void)execl(b->cfg.agent_path, b->cfg.agent_path, (char *)NULL);
        /* exec failed: report on the inherited stderr and leave. The parent
         * sees the pipe close and turns that into HH_ERR_IO. */
        (void)fprintf(stderr, "rh_agent: cannot exec %s: errno=%d\n",
                      b->cfg.agent_path, errno);
        _exit(127);
    }

    (void)close(in_pipe[0]);
    (void)close(out_pipe[1]);
    b->pid        = pid;
    b->to_agent   = in_pipe[1];
    b->from_agent = out_pipe[0];
    b->rxlen      = 0;
    return HH_OK;
}

/* Write one request line, then block for exactly one reply line.
 *
 * Synchronous on purpose: these are control-plane operations, and radiod's
 * existing backend calls (hh_radio_open, hh_radio_set_channel) are synchronous
 * too. The timeout is what keeps a wedged agent from wedging the control loop.
 */
static hh_status_t agent_call(hh_oc_backend_t *b, uint32_t timeout_ms,
                              const char *request, char *reply, size_t reply_cap)
{
    size_t len, sent = 0;
    char *nl;

    if (!hh_oc_backend_agent_alive(b)) {
        oc_err(b, "no rh_agent is running (OpenCPI backend unavailable)");
        return HH_ERR_IO;
    }

    len = strlen(request);
    while (sent < len) {
        ssize_t w = write(b->to_agent, request + sent, len - sent);
        if (w > 0) { sent += (size_t)w; continue; }
        if (w < 0 && errno == EINTR) continue;
        oc_err(b, "rh_agent write failed: errno=%d", errno);
        agent_drop(b);
        return HH_ERR_IO;
    }

    /* A previous reply may have arrived together with this one. */
    for (;;) {
        b->rxbuf[b->rxlen] = '\0';
        nl = memchr(b->rxbuf, '\n', b->rxlen);
        if (nl != NULL) break;

        if (b->rxlen + 1u >= sizeof b->rxbuf) {
            oc_err(b, "rh_agent reply exceeded %u bytes", (unsigned)sizeof b->rxbuf);
            agent_drop(b);
            return HH_ERR_IO;
        }
        {
            struct pollfd pfd;
            int pr;
            ssize_t n;
            pfd.fd = b->from_agent;
            pfd.events = POLLIN;
            pfd.revents = 0;
            pr = poll(&pfd, 1, (int)timeout_ms);
            if (pr == 0) {
                oc_err(b, "rh_agent did not answer within %u ms", (unsigned)timeout_ms);
                /* A timed-out agent is of unknown state; dropping it is the
                 * only way back to a defined one. */
                agent_drop(b);
                return HH_ERR_IO;
            }
            if (pr < 0) {
                if (errno == EINTR) continue;
                oc_err(b, "poll on rh_agent failed: errno=%d", errno);
                agent_drop(b);
                return HH_ERR_IO;
            }
            n = read(b->from_agent, b->rxbuf + b->rxlen, sizeof b->rxbuf - b->rxlen - 1u);
            if (n == 0) {
                oc_err(b, "rh_agent exited unexpectedly");
                agent_drop(b);
                return HH_ERR_IO;
            }
            if (n < 0) {
                if (errno == EINTR || errno == EAGAIN) continue;
                oc_err(b, "rh_agent read failed: errno=%d", errno);
                agent_drop(b);
                return HH_ERR_IO;
            }
            b->rxlen += (size_t)n;
        }
    }

    {
        size_t linelen = (size_t)(nl - b->rxbuf);
        size_t consumed = linelen + 1u;
        if (linelen >= reply_cap) linelen = reply_cap - 1u;
        memcpy(reply, b->rxbuf, linelen);
        reply[linelen] = '\0';
        memmove(b->rxbuf, b->rxbuf + consumed, b->rxlen - consumed);
        b->rxlen -= consumed;
    }

    if (strncmp(reply, "ok ", 3) == 0) return HH_OK;

    if (strncmp(reply, "err ", 4) == 0) {
        char msg[HH_RC_MAX_TEXT];
        if (reply_kv(reply, "msg", msg, sizeof msg)) oc_err(b, "%s", msg);
        else                                          oc_err(b, "%s", reply + 4);
        /* The agent's numeric rc is a RADHAWK status and is left in the message
         * rather than translated -- collapsing a dozen RADHAWK codes onto eight
         * hh_status_t values would lose exactly the detail that makes an OpenCPI
         * failure diagnosable. What IS translated is the agent's stated `kind`,
         * which exists so radiod never has to infer a status from message text. */
        return reply_kind(reply);
    }

    oc_err(b, "unparseable rh_agent reply: %s", reply);
    agent_drop(b);
    return HH_ERR_IO;
}

/* ------------------------------------------------------------------------- */
/* Registry                                                                  */
/* ------------------------------------------------------------------------- */

/* false = one worker name too many: the registry is then refused at load,
 * the same as a props overflow, rather than its tail silently dropped. */
static bool registry_note_worker(hh_oc_backend_t *b, const char *worker)
{
    size_t i;
    for (i = 0; i < b->nworkers; i++) {
        if (strcmp(b->workers[i], worker) == 0) return true;
    }
    if (b->nworkers >= HH_OC_MAX_WORKERS) return false;
    (void)snprintf(b->workers[b->nworkers], HH_RC_MAX_NAME, "%s", worker);
    b->nworkers++;
    return true;
}

/* Parse one "worker.prop:type:access" element into the registry. */
static bool registry_add(hh_oc_backend_t *b, const char *elem, size_t len)
{
    char buf[128];
    char *dot, *c1, *c2;

    if (len == 0u || len >= sizeof buf) return false;
    memcpy(buf, elem, len);
    buf[len] = '\0';

    dot = strchr(buf, '.');
    if (dot == NULL) return false;
    *dot = '\0';
    c1 = strchr(dot + 1, ':');
    if (c1 == NULL) return false;
    *c1 = '\0';
    c2 = strchr(c1 + 1, ':');
    if (c2 == NULL) return false;
    *c2 = '\0';

    if (b->nprops >= HH_OC_MAX_PROPS) return false;
    if (!registry_note_worker(b, buf)) return false;
    (void)snprintf(b->props[b->nprops].worker, HH_RC_MAX_NAME, "%s", buf);
    (void)snprintf(b->props[b->nprops].prop,   HH_RC_MAX_NAME, "%s", dot + 1);
    (void)snprintf(b->props[b->nprops].type,   16, "%s", c1 + 1);
    (void)snprintf(b->props[b->nprops].access, 16, "%s", c2 + 1);
    b->nprops++;
    return true;
}

static hh_status_t registry_load(hh_oc_backend_t *b)
{
    char reply[HH_RH_AGENT_MAX_LINE];
    /* The whole registry arrives in one reply, so this buffer is sized by the
     * agent link's limit and not by the narrower client-facing one. */
    char items[HH_RH_AGENT_MAX_ITEMS];
    const char *p;
    hh_status_t st;

    b->nprops = 0;
    b->nworkers = 0;

    st = agent_call(b, b->cfg.reply_timeout_ms, HH_RH_AGENT_PROPS "\n",
                    reply, sizeof reply);
    if (st != HH_OK) return st;

    if (!reply_kv(reply, "items", items, sizeof items)) {
        oc_err(b, "rh_agent props reply carried no items list");
        return HH_ERR_IO;
    }

    p = items;
    while (*p != '\0') {
        const char *comma = strchr(p, ',');
        size_t len = (comma != NULL) ? (size_t)(comma - p) : strlen(p);
        if (!registry_add(b, p, len)) {
            oc_err(b, "rh_agent published a property this build cannot hold "
                      "(at element %u, capacity %d)",
                   (unsigned)b->nprops, HH_OC_MAX_PROPS);
            return HH_ERR_NOMEM;
        }
        if (comma == NULL) break;
        p = comma + 1;
    }
    return HH_OK;
}

static const hh_oc_prop_t *registry_find(const hh_oc_backend_t *b,
                                         const char *worker, const char *prop)
{
    size_t i;
    for (i = 0; i < b->nprops; i++) {
        if (strcmp(b->props[i].worker, worker) == 0 &&
            strcmp(b->props[i].prop, prop) == 0) return &b->props[i];
    }
    return NULL;
}

static bool registry_has_worker(const hh_oc_backend_t *b, const char *worker)
{
    size_t i;
    for (i = 0; i < b->nworkers; i++) {
        if (strcmp(b->workers[i], worker) == 0) return true;
    }
    return false;
}

/* ------------------------------------------------------------------------- */
/* Property control surface (props.h)                                        */
/* ------------------------------------------------------------------------- */

/* Append "<text>" to a comma-separated list, refusing rather than truncating:
 * a truncated list would silently hide properties from the operator. */
static bool items_append(char *items, size_t cap, size_t *used, const char *text)
{
    size_t need = strlen(text) + ((*used > 0u) ? 1u : 0u);
    if (*used + need + 1u > cap) return false;
    if (*used > 0u) items[(*used)++] = ',';
    memcpy(items + *used, text, strlen(text));
    *used += strlen(text);
    items[*used] = '\0';
    return true;
}

static hh_status_t oc_list(void *self, const char *worker, hh_rc_response_t *resp)
{
    hh_oc_backend_t *b = self;
    size_t used = 0, i;

    if (b == NULL || resp == NULL) return HH_ERR_INVAL;
    if (b->nprops == 0u) {
        oc_err(b, "no property registry: %s",
               hh_oc_backend_agent_alive(b) ? "rh_agent published none"
                                            : "rh_agent is not running");
        oc_fail(b, resp);
        return HH_ERR_UNSUPPORTED;
    }
    resp->items[0] = '\0';

    if (worker == NULL || worker[0] == '\0') {
        for (i = 0; i < b->nworkers; i++) {
            if (!items_append(resp->items, sizeof resp->items, &used, b->workers[i])) {
                oc_err(b, "worker list does not fit in %u bytes",
                       (unsigned)sizeof resp->items);
                oc_fail(b, resp);
                return HH_ERR_NOMEM;
            }
        }
        resp->count = (uint32_t)b->nworkers;
        return HH_OK;
    }

    if (!registry_has_worker(b, worker)) {
        oc_err(b, "no such worker '%s'", worker);
        oc_fail(b, resp);
        return HH_ERR_NOTFOUND;
    }

    (void)snprintf(resp->worker, sizeof resp->worker, "%s", worker);
    for (i = 0; i < b->nprops; i++) {
        char elem[80];
        if (strcmp(b->props[i].worker, worker) != 0) continue;
        (void)snprintf(elem, sizeof elem, "%s:%s:%s", b->props[i].prop,
                       b->props[i].type, b->props[i].access);
        if (!items_append(resp->items, sizeof resp->items, &used, elem)) {
            oc_err(b, "property list for '%s' does not fit in %u bytes",
                   worker, (unsigned)sizeof resp->items);
            oc_fail(b, resp);
            return HH_ERR_NOMEM;
        }
        resp->count++;
    }
    return HH_OK;
}

static hh_status_t oc_get(void *self, const char *worker, const char *prop,
                          hh_rc_response_t *resp)
{
    hh_oc_backend_t *b = self;
    const hh_oc_prop_t *e;
    char req[HH_RH_AGENT_MAX_LINE];
    char reply[HH_RH_AGENT_MAX_LINE];
    hh_status_t st;

    if (b == NULL || worker == NULL || prop == NULL || resp == NULL) return HH_ERR_INVAL;

    e = registry_find(b, worker, prop);
    if (e == NULL) {
        oc_err(b, "no such property '%s.%s'", worker, prop);
        oc_fail(b, resp);
        return HH_ERR_NOTFOUND;
    }

    /* A read-only property is a live counter inside the PL, so there is
     * nothing to read until the application is running. Reporting a stale or
     * zero counter as if it were current is how a dead link reads healthy. */
    if (strcmp(e->access, HH_PROPS_ACCESS_RO) == 0 && !b->session_open) {
        oc_err(b, "'%s.%s' is a live counter; start the application first",
               worker, prop);
        oc_fail(b, resp);
        return HH_ERR_STATE;
    }

    (void)snprintf(req, sizeof req, "%s %s.%s\n", HH_RH_AGENT_GET, worker, prop);
    st = agent_call(b, b->cfg.reply_timeout_ms, req, reply, sizeof reply);
    if (st != HH_OK) { oc_fail(b, resp); return st; }

    if (!reply_kv(reply, "value", resp->value, sizeof resp->value)) {
        oc_err(b, "rh_agent get reply carried no value");
        oc_fail(b, resp);
        return HH_ERR_IO;
    }
    (void)snprintf(resp->worker, sizeof resp->worker, "%s", worker);
    (void)snprintf(resp->prop,   sizeof resp->prop,   "%s", prop);
    (void)snprintf(resp->ptype,  sizeof resp->ptype,  "%s", e->type);
    (void)snprintf(resp->access, sizeof resp->access, "%s", e->access);
    return HH_OK;
}

static hh_status_t oc_set(void *self, const char *worker, const char *prop,
                          const char *value, bool running, hh_rc_response_t *resp)
{
    hh_oc_backend_t *b = self;
    const hh_oc_prop_t *e;
    char req[HH_RH_AGENT_MAX_LINE];
    char reply[HH_RH_AGENT_MAX_LINE];
    hh_status_t st;
    bool live;

    if (b == NULL || worker == NULL || prop == NULL || value == NULL || resp == NULL)
        return HH_ERR_INVAL;

    e = registry_find(b, worker, prop);
    if (e == NULL) {
        oc_err(b, "no such property '%s.%s'", worker, prop);
        oc_fail(b, resp);
        return HH_ERR_NOTFOUND;
    }

    if (strcmp(e->access, HH_PROPS_ACCESS_RO) == 0) {
        oc_err(b, "'%s.%s' is read-only (%s)", worker, prop, e->type);
        oc_fail(b, resp);
        return HH_ERR_UNSUPPORTED;
    }

    /* The whole point of publishing an access class. A start-time-only
     * property is one the running application cannot absorb -- sym_mode must
     * agree with the AD9361 clock chain, and an AD9361 key is consumed by
     * ad9361_init, which stops the only clock the fabric has. Writing one
     * while the application runs is REFUSED, not queued: silently deferring
     * it would leave the operator believing a change had taken effect. */
    if (strcmp(e->access, HH_PROPS_ACCESS_RW_START) == 0 && running) {
        oc_err(b, "'%s.%s' can only be set while the application is stopped; "
                  "run `stop`, set it, then `start`", worker, prop);
        oc_fail(b, resp);
        return HH_ERR_STATE;
    }

    /* Running: apply now through rh_set_params. Stopped: stage into the
     * configuration the next start will carry. */
    live = running && b->session_open;
    (void)snprintf(req, sizeof req, "%s %s.%s %s\n",
                   live ? HH_RH_AGENT_SET : HH_RH_AGENT_CFG, worker, prop, value);
    st = agent_call(b, b->cfg.reply_timeout_ms, req, reply, sizeof reply);
    if (st != HH_OK) {
        /* `st` already carries the agent's stated kind, so a value the framework
         * or RADHAWK rejected arrives as HH_ERR_INVAL rather than as an I/O
         * error -- a typo in a value must not look like a broken radio. */
        oc_fail(b, resp);
        return st;
    }

    (void)snprintf(resp->worker, sizeof resp->worker, "%s", worker);
    (void)snprintf(resp->prop,   sizeof resp->prop,   "%s", prop);
    (void)snprintf(resp->value,  sizeof resp->value,  "%s", value);
    (void)snprintf(resp->text, sizeof resp->text, "%s",
                   live ? "applied to the running application"
                        : "staged for the next start");
    return HH_OK;
}

static hh_status_t oc_status(void *self, bool running, hh_rc_response_t *resp)
{
    hh_oc_backend_t *b = self;
    char reply[HH_RH_AGENT_MAX_LINE];
    char n[16];
    hh_status_t st;

    (void)running;
    if (b == NULL || resp == NULL) return HH_ERR_INVAL;

    if (!b->session_open) {
        oc_err(b, "no application session is open; run `start` first");
        oc_fail(b, resp);
        return HH_ERR_STATE;
    }

    st = agent_call(b, b->cfg.reply_timeout_ms, HH_RH_AGENT_STATUS "\n",
                    reply, sizeof reply);
    if (st != HH_OK) { oc_fail(b, resp); return st; }

    if (!reply_kv(reply, "items", resp->items, sizeof resp->items)) {
        oc_err(b, "rh_agent status reply carried no items list");
        oc_fail(b, resp);
        return HH_ERR_IO;
    }
    if (reply_kv(reply, "n", n, sizeof n)) resp->count = (uint32_t)strtoul(n, NULL, 10);
    return HH_OK;
}

static const hh_radiod_props_ops_t g_oc_props_ops = {
    .name   = "opencpi",
    .list   = oc_list,
    .get    = oc_get,
    .set    = oc_set,
    .status = oc_status,
};

/* ------------------------------------------------------------------------- */
/* Radio contract (radio.h)                                                  */
/* ------------------------------------------------------------------------- */

static hh_status_t oc_open(void *self)
{
    hh_oc_backend_t *b = self;
    char reply[HH_RH_AGENT_MAX_LINE];
    char dry[8];
    hh_status_t st;

    if (b == NULL) return HH_ERR_INVAL;
    if (!hh_oc_backend_agent_alive(b)) {
        oc_err(b, "rh_agent is not running; cannot deploy the OpenCPI application");
        HH_LOGE(COMP, "open", "detail=%s", b->last_error);
        return HH_ERR_IO;
    }
    if (b->session_open) return HH_OK;   /* idempotent */

    st = agent_call(b, b->cfg.start_timeout_ms, HH_RH_AGENT_START "\n",
                    reply, sizeof reply);
    if (st != HH_OK) {
        HH_LOGE(COMP, "open", "detail=%s", b->last_error);
        return st;
    }
    b->session_open = true;
    b->dry_run = reply_kv(reply, "dry", dry, sizeof dry) && dry[0] == '1';
    HH_LOGI(COMP, "started", "dry_run=%d", b->dry_run ? 1 : 0);
    return HH_OK;
}

static hh_status_t oc_close(void *self)
{
    hh_oc_backend_t *b = self;
    char reply[HH_RH_AGENT_MAX_LINE];
    hh_status_t st;

    if (b == NULL) return HH_ERR_INVAL;
    if (!b->session_open) return HH_OK;

    st = agent_call(b, b->cfg.reply_timeout_ms, HH_RH_AGENT_STOP "\n",
                    reply, sizeof reply);
    /* The session is over either way: if the agent could not be reached it has
     * already been dropped, and RADHAWK releases its run lock when the agent
     * process dies. Leaving session_open set would block every later start. */
    b->session_open = false;
    if (st != HH_OK) HH_LOGW(COMP, "close", "detail=%s", b->last_error);
    return st;
}

static hh_status_t oc_transmit(void *self, const hh_frame_t *frame)
{
    (void)self; (void)frame;
    /* Data plane. rc.h excludes HTI-03 from the control contract, and this
     * application's payload path is DMA, not request/response IPC. */
    return HH_ERR_NOT_IMPLEMENTED;
}

static hh_status_t oc_set_rx_callback(void *self, hh_radio_rx_fn fn, void *ctx)
{
    (void)self; (void)fn; (void)ctx;
    return HH_ERR_NOT_IMPLEMENTED;   /* HTI-04/05: data plane */
}

/* Read one named counter out of the agent's status item list. */
static uint64_t status_u64(const char *items, const char *key)
{
    char pat[64];
    const char *at;
    (void)snprintf(pat, sizeof pat, "%s:", key);
    at = strstr(items, pat);
    if (at == NULL) return 0u;
    /* Must be at the start of an element, not inside a longer name. */
    if (at != items && *(at - 1) != ',') {
        at = strstr(at + 1, pat);
        if (at == NULL || (at != items && *(at - 1) != ',')) return 0u;
    }
    return strtoull(at + strlen(pat), NULL, 10);
}

static hh_status_t oc_get_status(void *self, hh_radio_status_t *out)
{
    hh_oc_backend_t *b = self;
    char reply[HH_RH_AGENT_MAX_LINE];
    char items[HH_RC_MAX_ITEMS];
    char lo[HH_RC_MAX_VALUE];

    if (b == NULL || out == NULL) return HH_ERR_INVAL;
    memset(out, 0, sizeof *out);

    /* operational means "acquired and running", not "the link has locked".
     * See opencpi.h for every field's mapping and for the fields left at 0. */
    out->operational = hh_oc_backend_agent_alive(b) && b->session_open;
    if (!out->operational) return HH_OK;

    if (agent_call(b, b->cfg.reply_timeout_ms, HH_RH_AGENT_STATUS "\n",
                   reply, sizeof reply) != HH_OK) {
        out->operational = false;
        return HH_ERR_IO;
    }
    if (reply_kv(reply, "items", items, sizeof items)) {
        out->tx_errors = status_u64(items, "qpsk_tx.dbg_out_dropped");
        out->rx_errors = status_u64(items, "qpsk_rx.err_count");
    }

    /* The requested LO, not a synthesiser read-back -- see opencpi.h. */
    {
        char req[64];
        (void)snprintf(req, sizeof req, "%s radio.tx_lo_hz\n", HH_RH_AGENT_GET);
        if (agent_call(b, b->cfg.reply_timeout_ms, req, reply, sizeof reply) == HH_OK &&
            reply_kv(reply, "value", lo, sizeof lo) &&
            strcmp(lo, HH_RH_AGENT_KEEP) != 0) {
            out->frequency_hz = (float)strtoull(lo, NULL, 10);
        }
    }
    return HH_OK;
}

static hh_status_t oc_get_link_metrics(void *self, hh_node_id_t neighbor,
                                       hh_link_sample_t *out)
{
    (void)self; (void)neighbor; (void)out;
    return HH_ERR_NOT_IMPLEMENTED;   /* HTI-05: data plane */
}

static hh_status_t oc_set_channel(void *self, uint32_t channel)
{
    (void)self; (void)channel;
    /* No logical-channel-to-RF mapping is defined for this radio
     * (unknown.md U-14). Returning UNSUPPORTED is what radio.h documents for
     * an adapter without channel agility, and the caller falls back to
     * route-based recovery. Retuning is reached explicitly and unambiguously
     * with `opencpi set radio tx_lo_hz <Hz>`. */
    return HH_ERR_UNSUPPORTED;
}

static hh_status_t oc_poll(void *self, hh_time_ms_t now)
{
    hh_oc_backend_t *b = self;
    int status;

    (void)now;
    if (b == NULL) return HH_ERR_INVAL;
    if (b->pid <= 0) return HH_OK;

    /* Reap the agent if it has died, so `operational` stops claiming a running
     * radio the moment the process backing it is gone. */
    if (waitpid(b->pid, &status, WNOHANG) == b->pid) {
        HH_LOGE(COMP, "agent_exited", "pid=%d status=%d", (int)b->pid, status);
        b->pid = -1;
        oc_err(b, "rh_agent exited (wait status %d)", status);
        agent_drop(b);
        return HH_ERR_IO;
    }
    return HH_OK;
}

static const hh_radio_ops_t g_oc_radio_ops = {
    .name             = "opencpi",
    .open             = oc_open,
    .close            = oc_close,
    .transmit         = oc_transmit,
    .set_rx_callback  = oc_set_rx_callback,
    .get_status       = oc_get_status,
    .get_link_metrics = oc_get_link_metrics,
    .set_channel      = oc_set_channel,
    .poll             = oc_poll,
};

/* ------------------------------------------------------------------------- */
/* Construction                                                              */
/* ------------------------------------------------------------------------- */

void hh_oc_config_defaults(hh_oc_config_t *cfg)
{
    if (cfg == NULL) return;
    memset(cfg, 0, sizeof *cfg);
    (void)snprintf(cfg->agent_path, sizeof cfg->agent_path, "%s",
                   HH_RH_AGENT_DEFAULT_PATH);
    cfg->reply_timeout_ms = OC_DEFAULT_REPLY_MS;
    cfg->start_timeout_ms = OC_DEFAULT_START_MS;
}

hh_status_t hh_oc_backend_init(hh_oc_backend_t *b, const hh_oc_config_t *cfg,
                               hh_radio_t *radio_out, hh_radiod_props_t *props_out)
{
    char reply[HH_RH_AGENT_MAX_LINE];
    char api[16];
    hh_status_t st;

    if (b == NULL || radio_out == NULL || props_out == NULL) return HH_ERR_INVAL;

    memset(b, 0, sizeof *b);
    b->pid = -1;
    b->to_agent = -1;
    b->from_agent = -1;
    if (cfg != NULL) b->cfg = *cfg; else hh_oc_config_defaults(&b->cfg);

    /* Bind the handles before anything can fail, so a caller that ignores the
     * return value still gets a callable backend that fails honestly rather
     * than a NULL vtable that crashes. */
    radio_out->ops  = &g_oc_radio_ops;
    radio_out->self = b;
    props_out->ops  = &g_oc_props_ops;
    props_out->self = b;

    st = agent_spawn(b);
    if (st != HH_OK) {
        HH_LOGE(COMP, "agent_spawn", "path=%s detail=%s", b->cfg.agent_path,
                b->last_error);
        return st;
    }

    st = agent_call(b, b->cfg.reply_timeout_ms, HH_RH_AGENT_HELLO "\n",
                    reply, sizeof reply);
    if (st != HH_OK) {
        HH_LOGE(COMP, "agent_hello", "path=%s detail=%s", b->cfg.agent_path,
                b->last_error);
        return st;
    }
    if (!reply_kv(reply, "api", api, sizeof api)) {
        (void)snprintf(api, sizeof api, "?");
    }
    if (strtol(api, NULL, 10) != HH_RH_AGENT_PROTO_VERSION) {
        oc_err(b, "rh_agent speaks protocol %s, radiod speaks %d",
               api, HH_RH_AGENT_PROTO_VERSION);
        HH_LOGE(COMP, "agent_version", "detail=%s", b->last_error);
        agent_drop(b);
        return HH_ERR_UNSUPPORTED;
    }

    st = registry_load(b);
    if (st != HH_OK) {
        HH_LOGE(COMP, "registry", "detail=%s", b->last_error);
        return st;
    }

    HH_LOGI(COMP, "ready", "agent=%s workers=%u properties=%u",
            b->cfg.agent_path, (unsigned)b->nworkers, (unsigned)b->nprops);
    return HH_OK;
}

void hh_oc_backend_release(hh_oc_backend_t *b)
{
    if (b == NULL) return;
    if (b->session_open) (void)oc_close(b);
    if (hh_oc_backend_agent_alive(b)) {
        /* Ask first; agent_drop's SIGTERM is the fallback. An agent that exits
         * on request releases RADHAWK's run lock cleanly. */
        ssize_t w = write(b->to_agent, HH_RH_AGENT_QUIT "\n",
                          sizeof(HH_RH_AGENT_QUIT "\n") - 1u);
        (void)w;
    }
    agent_drop(b);
}
