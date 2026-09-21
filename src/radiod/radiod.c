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
    return HH_OK;
}

void hh_radiod_set_fault_hook(hh_radiod_t *d, hh_radiod_fault_fn fn, void *ctx)
{
    if (!d) return;
    d->fault_fn = fn;
    d->fault_ctx = ctx;
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
    HH_LOGI(COMP, "listening", "path=%s", sock_path);
    return HH_OK;
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

    if (!d || !req || !resp) return;
    memset(resp, 0, sizeof *resp);
    resp->cmd = req->cmd;
    d->requests_total++;

    if (!transition_allowed(d->state, req->cmd)) {
        d->requests_rejected++;
        resp->ok = false;
        resp->reason = HH_ERR_STATE;
        resp->state = d->state;
        HH_LOGW(COMP, "reject", "cmd=%s state=%s", hh_rc_cmd_str(req->cmd),
                hh_rc_state_str(d->state));
        return;
    }

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
        if (st != HH_OK) {
            d->state = HH_RC_STATE_FAULTED;
            resp->ok = false;
            resp->reason = st;
            break;
        }
        d->state = HH_RC_STATE_RUNNING;
        resp->ok = true;
        break;

    case HH_RC_CMD_STOP:
        hh_radio_close(d->radio);
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
        if (st != HH_OK) {
            resp->ok = false;
            resp->reason = st;
            break;
        }
        resp->ok = true;
        fill_status(d, resp);
        break;

    case HH_RC_CMD_INJECT_FAULT:
        if (d->fault_fn) d->fault_fn(d->fault_ctx, req->fault);
        if (req->fault == HH_RC_FAULT_HW_FAULT) d->state = HH_RC_STATE_FAULTED;
        resp->ok = true;
        fill_status(d, resp);
        break;

    case HH_RC_CMD_CLEAR_FAULT:
        if (d->fault_fn) d->fault_fn(d->fault_ctx, HH_RC_FAULT_NONE);
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
}

bool hh_radiod_shutdown_requested(const hh_radiod_t *d)
{
    return d && d->state == HH_RC_STATE_RELEASED;
}

/* ---- socket servicing ---- */

static void close_client(hh_radiod_client_t *c)
{
    if (c->fd >= 0) close(c->fd);
    c->fd = -1;
    c->inlen = 0;
}

static void accept_clients(hh_radiod_t *d)
{
    if (d->listen_fd < 0) return;
    for (;;) {
        int fd = accept4(d->listen_fd, NULL, NULL, SOCK_NONBLOCK);
        if (fd < 0) return; /* EAGAIN or real error; nothing pending either way */

        int slot = -1;
        for (int i = 0; i < HH_RADIOD_MAX_CLIENTS; i++) {
            if (d->clients[i].fd < 0) { slot = i; break; }
        }
        if (slot < 0) { close(fd); continue; } /* at capacity */
        d->clients[slot].fd = fd;
        d->clients[slot].inlen = 0;
    }
}

static void service_client(hh_radiod_t *d, hh_radiod_client_t *c)
{
    ssize_t n = read(c->fd, c->inbuf + c->inlen, sizeof c->inbuf - c->inlen - 1);
    if (n == 0) { close_client(c); return; }              /* clean disconnect  */
    if (n < 0) { if (errno != EAGAIN && errno != EWOULDBLOCK) close_client(c); return; }

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
        char out[HH_RC_MAX_LINE];
        size_t outlen;

        if (hh_rc_request_parse(line, &req) == HH_OK) {
            hh_radiod_handle_request(d, &req, &resp);
        } else {
            memset(&resp, 0, sizeof resp);
            resp.ok = false;
            resp.reason = HH_ERR_INVAL;
            d->requests_rejected++;
        }
        outlen = hh_rc_response_format(&resp, out, sizeof out);
        if (outlen > 0) {
            ssize_t w = write(c->fd, out, outlen);
            (void)w; /* a slow/blocked client is dropped, never allowed to stall radiod */
        }

        size_t consumed = linelen + 1;
        memmove(c->inbuf, c->inbuf + consumed, c->inlen - consumed);
        c->inlen -= consumed;
    }

    if (c->inlen >= sizeof c->inbuf - 1) close_client(c); /* line too long: drop client */
}

hh_status_t hh_radiod_tick(hh_radiod_t *d, hh_time_ms_t now)
{
    if (!d) return HH_ERR_INVAL;
    if (d->state == HH_RC_STATE_RELEASED) return HH_ERR_STATE;

    accept_clients(d);
    for (int i = 0; i < HH_RADIOD_MAX_CLIENTS; i++) {
        if (d->clients[i].fd >= 0) service_client(d, &d->clients[i]);
    }
    if (d->state == HH_RC_STATE_RUNNING) hh_radio_poll(d->radio, now);
    return HH_OK;
}

void hh_radiod_release(hh_radiod_t *d)
{
    if (!d) return;
    for (int i = 0; i < HH_RADIOD_MAX_CLIENTS; i++) close_client(&d->clients[i]);
    if (d->listen_fd >= 0) { close(d->listen_fd); d->listen_fd = -1; }
    d->state = HH_RC_STATE_RELEASED;
}
