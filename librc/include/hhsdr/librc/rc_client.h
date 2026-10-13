/*
 * librc — the radio-control client library.
 *
 * This is the "librc" box of the three-plane architecture: the C API through
 * which every out-of-process client reaches radiod. radioctl, test automation,
 * and (in the target architecture) the routing daemon and applications all go
 * through here rather than opening the control socket themselves.
 *
 * Connects to a running radiod over its UNIX domain socket and exchanges one
 * request/response message per call.
 *
 * DEPENDENCY DIRECTION — the reason this library exists separately:
 * librc depends on the protocol codec ONLY. It must never depend on
 * hhsdr_radiod. A client that had to link the daemon in order to talk to the
 * daemon would defeat the point of a client library, and would make the
 * daemon's internals part of every caller's build.
 *
 * NAMING: the architecture drawing shows applications calling an "rc_* API".
 * Those exact signatures are not specified anywhere (see unknown.md, U-02), so
 * the existing hh_rc_client_* API is kept rather than inventing rc_* names. If
 * the specified API differs, it can be layered over this one without changing
 * radiod.
 */
#ifndef HHSDR_LIBRC_RC_CLIENT_H
#define HHSDR_LIBRC_RC_CLIENT_H

#include "hhsdr/protocol/rc.h"

typedef struct {
    int fd;
} hh_rc_client_t;

/* Connect to a radiod listening on `sock_path`. */
hh_status_t hh_rc_client_connect(hh_rc_client_t *c, const char *sock_path);

/* Send one request and block for its response line. */
hh_status_t hh_rc_client_call(hh_rc_client_t *c, const hh_rc_request_t *req,
                              hh_rc_response_t *resp);

void hh_rc_client_close(hh_rc_client_t *c);

#endif /* HHSDR_LIBRC_RC_CLIENT_H */
