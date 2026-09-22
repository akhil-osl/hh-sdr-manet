#define _GNU_SOURCE
#include "hhsdr/radiod/radiod.h"
#include "hhsdr/core/log.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#define COMP "radiod"

hh_status_t hh_radiod_init(hh_radiod_t *d, hh_radio_t *radio, const hh_clock_t *clock)
{
    if (!d || !radio || !clock) return HH_ERR_INVAL;
    memset(d, 0, sizeof *d);
    d->radio      = radio;
    d->clock      = clock;
    d->state      = HH_RC_STATE_CREATED;
    d->node_id    = HH_NODE_ID_INVALID;
    d->listen_fd  = -1;
    for (int i = 0; i < HH_RADIOD_MAX_CLIENTS; i++) d->clients[i].fd = -1;
    hh_radiod_faults_init(&d->faults);
    return HH_OK;
}

hh_status_t hh_radiod_configure(hh_radiod_t *d, const hh_radiod_config_t *cfg)
{
    hh_status_t st;
    if (!d || !cfg) return HH_ERR_INVAL;
    st = hh_radiod_config_validate(cfg);
    if (st != HH_OK) return st;
    d->client_idle_timeout_ms = cfg->client_idle_timeout_ms;
    return HH_OK;
}

void hh_radiod_set_fault_hook(hh_radiod_t *d, hh_radiod_fault_fn fn, void *ctx)
{
    if (!d) return;
    d->fault_fn = fn;
    d->fault_ctx = ctx;
}

/* Backend identity for the log. The vtable carries its own name, so a log
 * line says which backend answered without radiod knowing the concrete type. */
static const char *backend_name(const hh_radiod_t *d)
{
    return (d && d->radio && d->radio->ops && d->radio->ops->name)
         ? d->radio->ops->name : "none";
}

hh_status_t hh_radiod_listen(hh_radiod_t *d, const char *sock_path)
{
    struct sockaddr_un addr;
    int fd;

    if (!d || !sock_path) return HH_ERR_INVAL;

    fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0);
    if (fd < 0) { HH_LOGE(COMP, "listen", "op=socket errno=%d", errno); return HH_ERR_IO; }

    memset(&addr, 0, sizeof addr);
    addr.sun_family = AF_UNIX;
    if (strlen(sock_path) >= sizeof addr.sun_path) { close(fd); return HH_ERR_INVAL; }
    strncpy(addr.sun_path, sock_path, sizeof addr.sun_path - 1);

    unlink(sock_path); /* stale socket from a prior unclean shutdown */

    if (bind(fd, (struct sockaddr *)&addr, sizeof addr) != 0) {
        HH_LOGE(COMP, "listen", "op=bind path=%s errno=%d", sock_path, errno);
        close(fd);
        return HH_ERR_IO;
    }
    if (listen(fd, HH_RADIOD_MAX_CLIENTS) != 0) {
        HH_LOGE(COMP, "listen", "op=listen errno=%d", errno);
        close(fd);
        return HH_ERR_IO;
    }

    d->listen_fd = fd;
    /* Remembered so release() can unlink it, rather than leaving a stale
     * socket file behind for the next start to clean up. */
    snprintf(d->sock_path, sizeof d->sock_path, "%s", sock_path);
    /* Name the backend at startup: with several backends compiled in, which
     * one is loaded is the first thing needed when diagnosing a radio. */
    HH_LOGI(COMP, "listening", "path=%s backend=%s max_clients=%d",
            sock_path, backend_name(d), HH_RADIOD_MAX_CLIENTS);
    return HH_OK;
}

size_t hh_radiod_pollfds(const hh_radiod_t *d, struct pollfd *fds, size_t cap)
{
    size_t n = 0;

    if (!d || !fds || cap < (size_t)HH_RADIOD_MAX_CLIENTS + 1) return 0;

    if (d->listen_fd >= 0) {
        fds[n].fd      = d->listen_fd;
        fds[n].events  = POLLIN;
        fds[n].revents = 0;
        n++;
    }
    for (int i = 0; i < HH_RADIOD_MAX_CLIENTS; i++) {
        if (d->clients[i].fd < 0) continue;
        fds[n].fd     = d->clients[i].fd;
        /* Watch for writability only while a reply is still pending, so an
         * idle connection does not spin the loop on POLLOUT. */
        fds[n].events = POLLIN | (d->clients[i].outlen ? POLLOUT : 0);
        fds[n].revents = 0;
        n++;
    }
    return n;
}

/* ---- state machine (Phase 3) ---- */

static bool transition_allowed(hh_rc_state_t from, hh_rc_cmd_t cmd)
{
    switch (cmd) {
    case HH_RC_CMD_INIT:
        return from == HH_RC_STATE_CREATED;
    case HH_RC_CMD_CONFIGURE:
        return from == HH_RC_STATE_INITIALIZED || from == HH_RC_STATE_CONFIGURED
            || from == HH_RC_STATE_STOPPED;
    case HH_RC_CMD_START:
        return from == HH_RC_STATE_CONFIGURED || from == HH_RC_STATE_STOPPED;
    case HH_RC_CMD_STOP:
        return from == HH_RC_STATE_RUNNING || from == HH_RC_STATE_FAULTED;
    case HH_RC_CMD_SHUTDOWN:
        return from != HH_RC_STATE_RELEASED;
    case HH_RC_CMD_STATUS:
    case HH_RC_CMD_STATS:
        return from != HH_RC_STATE_CREATED && from != HH_RC_STATE_RELEASED;
    case HH_RC_CMD_SET_CHANNEL:
        return from == HH_RC_STATE_RUNNING;
    case HH_RC_CMD_INJECT_FAULT:
    case HH_RC_CMD_CLEAR_FAULT:
        return from == HH_RC_STATE_RUNNING || from == HH_RC_STATE_FAULTED;
    default:
        return false;
    }
}

/* States a command is accepted from, for the reject log. Mirrors
 * transition_allowed() above; keep the two in step when adding a verb. */
static const char *legal_states_for(hh_rc_cmd_t cmd)
{
    switch (cmd) {
    case HH_RC_CMD_INIT:        return "created";
    case HH_RC_CMD_CONFIGURE:   return "initialized,configured,stopped";
    case HH_RC_CMD_START:       return "configured,stopped";
    case HH_RC_CMD_STOP:        return "running,faulted";
    case HH_RC_CMD_SHUTDOWN:    return "any-but-released";
    case HH_RC_CMD_STATUS:
    case HH_RC_CMD_STATS:       return "any-but-created,released";
    case HH_RC_CMD_SET_CHANNEL: return "running";
    case HH_RC_CMD_INJECT_FAULT:
    case HH_RC_CMD_CLEAR_FAULT: return "running,faulted";
    default:                    return "none";
    }
}

static void fill_status(const hh_radiod_t *d, hh_rc_response_t *resp)
{
    hh_radio_status_t st;
    memset(&st, 0, sizeof st);
    hh_radio_get_status(d->radio, &st);
    resp->state         = d->state;
    resp->operational    = st.operational;
    resp->channel        = st.channel;
    resp->frequency_hz   = st.frequency_hz;
    resp->waveform_id    = st.waveform_id;
}

static void fill_stats(const hh_radiod_t *d, hh_rc_response_t *resp)
{
    hh_radio_status_t st;
    memset(&st, 0, sizeof st);
    hh_radio_get_status(d->radio, &st);
    resp->frames_tx         = st.frames_tx;
    resp->frames_rx         = st.frames_rx;
    resp->tx_errors         = st.tx_errors;
    resp->rx_errors         = st.rx_errors;
    resp->requests_total    = d->requests_total;
    resp->requests_rejected = d->requests_rejected;
}

void hh_radiod_handle_request(hh_radiod_t *d, const hh_rc_request_t *req,
                              hh_rc_response_t *resp)
{
    hh_status_t st;
    hh_time_ms_t now;
    hh_rc_state_t prev;

    if (!d || !req || !resp) return;
    now = hh_now(d->clock);
    memset(resp, 0, sizeof *resp);
    resp->cmd = req->cmd;
    d->requests_total++;

    HH_LOGD(COMP, "request", "cmd=%s state=%s", hh_rc_cmd_str(req->cmd),
            hh_rc_state_str(d->state));

    if (!transition_allowed(d->state, req->cmd)) {
        d->requests_rejected++;
        resp->ok = false;
        resp->reason = HH_ERR_STATE;
        resp->state = d->state;
        /* Name the states the command WOULD be legal from, so the reason for
         * the refusal is in the log rather than only in the state table. */
        HH_LOGW(COMP, "reject", "cmd=%s state=%s reason=ESTATE legal_from=%s",
                hh_rc_cmd_str(req->cmd), hh_rc_state_str(d->state),
                legal_states_for(req->cmd));
        return;
    }

    prev = d->state;

    switch (req->cmd) {
    case HH_RC_CMD_INIT:
        d->state = HH_RC_STATE_INITIALIZED;
        resp->ok = true;
        break;

    case HH_RC_CMD_CONFIGURE:
        if (req->node_id == HH_NODE_ID_INVALID) {
            d->requests_rejected++;
            resp->ok = false;
            resp->reason = HH_ERR_INVAL;
            break;
        }
        d->node_id = req->node_id;
        d->state = HH_RC_STATE_CONFIGURED;
        resp->ok = true;
        break;

    case HH_RC_CMD_START:
        st = hh_radio_open(d->radio);
        HH_LOGD(COMP, "backend_call", "op=open backend=%s status=%s",
                backend_name(d), hh_status_str(st));
        if (st != HH_OK) {
            /* A backend that will not open is a hardware fault, and the
             * registry is what records that it happened and when. */
            hh_radiod_faults_assert(&d->faults, HH_RC_FAULT_HW_FAULT, now);
            d->state = HH_RC_STATE_FAULTED;
            resp->ok = false;
            resp->reason = st;
            break;
        }
        d->state = HH_RC_STATE_RUNNING;
        resp->ok = true;
        break;

    case HH_RC_CMD_STOP:
        st = hh_radio_close(d->radio);
        HH_LOGD(COMP, "backend_call", "op=close backend=%s status=%s",
                backend_name(d), hh_status_str(st));
        d->state = HH_RC_STATE_STOPPED;
        resp->ok = true;
        break;

    case HH_RC_CMD_SHUTDOWN:
        if (d->state == HH_RC_STATE_RUNNING) hh_radio_close(d->radio);
        d->state = HH_RC_STATE_RELEASED;
        resp->ok = true;
        break;

    case HH_RC_CMD_STATUS:
        fill_status(d, resp);
        resp->ok = true;
        break;

    case HH_RC_CMD_STATS:
        fill_stats(d, resp);
        resp->ok = true;
        break;

    case HH_RC_CMD_SET_CHANNEL:
        st = hh_radio_set_channel(d->radio, req->channel);
        HH_LOGD(COMP, "backend_call", "op=set_channel backend=%s channel=%u status=%s",
                backend_name(d), req->channel, hh_status_str(st));
        if (st != HH_OK) {
            hh_radiod_faults_assert(&d->faults, HH_RC_FAULT_BACKEND_IO, now);
            resp->ok = false;
            resp->reason = st;
            break;
        }
        resp->ok = true;
        fill_status(d, resp);
        break;

    case HH_RC_CMD_INJECT_FAULT:
        if (d->fault_fn) d->fault_fn(d->fault_ctx, req->fault);
        hh_radiod_faults_assert(&d->faults, req->fault, now);
        if (req->fault == HH_RC_FAULT_HW_FAULT) d->state = HH_RC_STATE_FAULTED;
        resp->ok = true;
        fill_status(d, resp);
        break;

    case HH_RC_CMD_CLEAR_FAULT:
        if (d->fault_fn) d->fault_fn(d->fault_ctx, HH_RC_FAULT_NONE);
        /* The command carries no kind, so it clears everything. */
        hh_radiod_faults_clear_all(&d->faults, now);
        if (d->state == HH_RC_STATE_FAULTED) d->state = HH_RC_STATE_RUNNING;
        resp->ok = true;
        fill_status(d, resp);
        break;

    default:
        d->requests_rejected++;
        resp->ok = false;
        resp->reason = HH_ERR_INVAL;
        break;
    }

    resp->state = d->state; /* always reflect current state, not just STATUS replies */

    /* A state change is the significant event, so it logs at info. A command
     * that leaves the state alone (status, stats) is routine and logs at
     * debug, to keep an idle poller from flooding the log at info. */
    if (d->state != prev) {
        HH_LOGI(COMP, "transition", "cmd=%s from=%s to=%s ok=%d",
                hh_rc_cmd_str(req->cmd), hh_rc_state_str(prev),
                hh_rc_state_str(d->state), resp->ok ? 1 : 0);
    } else if (!resp->ok) {
        HH_LOGW(COMP, "failed", "cmd=%s state=%s reason=%s",
                hh_rc_cmd_str(req->cmd), hh_rc_state_str(d->state),
                hh_status_str(resp->reason));
    } else {
        HH_LOGD(COMP, "ok", "cmd=%s state=%s", hh_rc_cmd_str(req->cmd),
                hh_rc_state_str(d->state));
    }
}

bool hh_radiod_shutdown_requested(const hh_radiod_t *d)
{
    return d && d->state == HH_RC_STATE_RELEASED;
}

const hh_radiod_faults_t *hh_radiod_faults(const hh_radiod_t *d)
{
    return d ? &d->faults : NULL;
}

/* ---- socket servicing ---- */

static void close_client(hh_radiod_client_t *c)
{
    if (c->fd >= 0) close(c->fd);
    c->fd = -1;
    c->inlen = 0;
    c->outlen = 0;
    c->outsent = 0;
}

/* Push whatever is still pending for this client. Returns false if the client
 * was dropped. A partial write is normal and simply leaves the remainder
 * queued for the next pass; previously the write() return was ignored, so a
 * short write silently truncated the reply. */
static bool flush_client(hh_radiod_client_t *c)
{
    while (c->outsent < c->outlen) {
        ssize_t w = write(c->fd, c->outbuf + c->outsent, c->outlen - c->outsent);
        if (w > 0) { c->outsent += (size_t)w; continue; }
        if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return true; /* retry later */
        close_client(c);
        return false;
    }
    c->outlen = 0;
    c->outsent = 0;
    return true;
}

static void accept_clients(hh_radiod_t *d, hh_time_ms_t now)
{
    if (d->listen_fd < 0) return;
    for (;;) {
        int fd = accept4(d->listen_fd, NULL, NULL, SOCK_NONBLOCK);
        if (fd < 0) return; /* EAGAIN or real error; nothing pending either way */

        int slot = -1;
        for (int i = 0; i < HH_RADIOD_MAX_CLIENTS; i++) {
            if (d->clients[i].fd < 0) { slot = i; break; }
        }
        if (slot < 0) {
            /* Capacity refusals are silent to the client -- it just sees a
             * closed connection -- so they must not be silent in the log. */
            HH_LOGW(COMP, "client_refused", "reason=at_capacity max=%d",
                    HH_RADIOD_MAX_CLIENTS);
            close(fd);
            continue;
        }
        HH_LOGD(COMP, "client_connected", "slot=%d fd=%d", slot, fd);
        d->clients[slot].fd = fd;
        d->clients[slot].inlen = 0;
        d->clients[slot].outlen = 0;
        d->clients[slot].outsent = 0;
        d->clients[slot].last_activity = now;
    }
}

static void service_client(hh_radiod_t *d, hh_radiod_client_t *c, hh_time_ms_t now)
{
    ssize_t n;

    /* Finish any reply still in flight before reading more requests, so
     * responses cannot interleave. */
    if (c->outlen && !flush_client(c)) return;
    if (c->outlen) return;  /* still blocked; try again next pass */

    n = read(c->fd, c->inbuf + c->inlen, sizeof c->inbuf - c->inlen - 1);
    if (n == 0) { close_client(c); return; }              /* clean disconnect  */
    if (n < 0) { if (errno != EAGAIN && errno != EWOULDBLOCK) close_client(c); return; }

    c->last_activity = now;
    c->inlen += (size_t)n;
    c->inbuf[c->inlen] = '\0';

    char *nl;
    while ((nl = memchr(c->inbuf, '\n', c->inlen)) != NULL) {
        size_t linelen = (size_t)(nl - c->inbuf);
        char line[HH_RC_MAX_LINE];
        if (linelen >= sizeof line) linelen = sizeof line - 1;
        memcpy(line, c->inbuf, linelen);
        line[linelen] = '\0';

        hh_rc_request_t req;
        hh_rc_response_t resp;

        if (hh_rc_request_parse(line, &req) == HH_OK) {
            hh_radiod_handle_request(d, &req, &resp);
        } else {
            memset(&resp, 0, sizeof resp);
            resp.ok = false;
            resp.reason = HH_ERR_INVAL;
            d->requests_rejected++;
            HH_LOGW(COMP, "parse_error", "reason=EINVAL line=\"%s\"", line);
        }

        c->outlen  = hh_rc_response_format(&resp, c->outbuf, sizeof c->outbuf);
        c->outsent = 0;

        size_t consumed = linelen + 1;
        memmove(c->inbuf, c->inbuf + consumed, c->inlen - consumed);
        c->inlen -= consumed;

        /* One reply at a time: send this one before parsing the next request,
         * so a pipelining client cannot have a queued response overwritten. */
        if (!flush_client(c)) return;
        if (c->outlen) return;  /* socket full; resume on the next pass */
    }

    if (c->inlen >= sizeof c->inbuf - 1) {
        HH_LOGW(COMP, "client_dropped", "reason=line_too_long max=%d",
                HH_RC_MAX_LINE);
        close_client(c);   /* line too long: drop client */
    }
}

/* Drop clients that have been silent longer than the configured timeout.
 * Without this an idle client holds its slot indefinitely, and 16 of them
 * exhaust the table. Disabled when the timeout is zero. */
static void expire_idle_clients(hh_radiod_t *d, hh_time_ms_t now)
{
    if (d->client_idle_timeout_ms == 0) return;

    for (int i = 0; i < HH_RADIOD_MAX_CLIENTS; i++) {
        hh_radiod_client_t *c = &d->clients[i];
        if (c->fd < 0) continue;
        if (c->outlen) continue;  /* mid-reply: not idle */
        if (now < c->last_activity) continue;  /* clock went backwards */
        if ((uint32_t)(now - c->last_activity) < d->client_idle_timeout_ms) continue;
        HH_LOGD(COMP, "client_idle_timeout", "slot=%d idle_ms=%llu", i,
                (unsigned long long)(now - c->last_activity));
        close_client(c);
    }
}

hh_status_t hh_radiod_tick(hh_radiod_t *d, hh_time_ms_t now)
{
    if (!d) return HH_ERR_INVAL;
    if (d->state == HH_RC_STATE_RELEASED) return HH_ERR_STATE;

    accept_clients(d, now);
    for (int i = 0; i < HH_RADIOD_MAX_CLIENTS; i++) {
        if (d->clients[i].fd >= 0) service_client(d, &d->clients[i], now);
    }
    expire_idle_clients(d, now);
    if (d->state == HH_RC_STATE_RUNNING) hh_radio_poll(d->radio, now);
    return HH_OK;
}

void hh_radiod_release(hh_radiod_t *d)
{
    if (!d) return;
    for (int i = 0; i < HH_RADIOD_MAX_CLIENTS; i++) close_client(&d->clients[i]);
    if (d->listen_fd >= 0) { close(d->listen_fd); d->listen_fd = -1; }
    /* Remove our own socket file rather than leaving it for the next start. */
    if (d->sock_path[0] != '\0') { unlink(d->sock_path); d->sock_path[0] = '\0'; }
    d->state = HH_RC_STATE_RELEASED;
}
