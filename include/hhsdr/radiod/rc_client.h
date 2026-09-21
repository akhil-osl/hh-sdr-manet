/*
 * RC client — thin synchronous client over the rc.h protocol.
 *
 * Connects to a running radiod over its UNIX domain socket and exchanges
 * one request/response line per call. Used by the radiod test suite to
 * exercise the daemon as a real separate process, and available for a
 * future MANET-side client without duplicating the wire encoding.
 */
#ifndef HHSDR_RADIOD_RC_CLIENT_H
#define HHSDR_RADIOD_RC_CLIENT_H

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

#endif /* HHSDR_RADIOD_RC_CLIENT_H */
