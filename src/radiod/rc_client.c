#include "hhsdr/radiod/rc_client.h"
#include <errno.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

hh_status_t hh_rc_client_connect(hh_rc_client_t *c, const char *sock_path)
{
    struct sockaddr_un addr;
    int fd;

    if (!c || !sock_path) return HH_ERR_INVAL;

    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return HH_ERR_IO;

    memset(&addr, 0, sizeof addr);
    addr.sun_family = AF_UNIX;
    if (strlen(sock_path) >= sizeof addr.sun_path) { close(fd); return HH_ERR_INVAL; }
    strncpy(addr.sun_path, sock_path, sizeof addr.sun_path - 1);

    if (connect(fd, (struct sockaddr *)&addr, sizeof addr) != 0) {
        close(fd);
        return HH_ERR_IO;
    }
    c->fd = fd;
    return HH_OK;
}

hh_status_t hh_rc_client_call(hh_rc_client_t *c, const hh_rc_request_t *req,
                              hh_rc_response_t *resp)
{
    char line[HH_RC_MAX_LINE];
    char in[HH_RC_MAX_LINE];
    size_t len, inlen = 0;
    ssize_t n;

    if (!c || c->fd < 0 || !req || !resp) return HH_ERR_INVAL;

    len = hh_rc_request_format(req, line, sizeof line);
    if (len == 0) return HH_ERR_INVAL;
    if (write(c->fd, line, len) != (ssize_t)len) return HH_ERR_IO;

    /* Read until a full line arrives; radiod writes one response line
     * per request, so a single line is always sufficient here. */
    for (;;) {
        char *nl;
        n = read(c->fd, in + inlen, sizeof in - inlen - 1);
        if (n <= 0) return HH_ERR_IO;
        inlen += (size_t)n;
        in[inlen] = '\0';
        nl = memchr(in, '\n', inlen);
        if (nl) break;
        if (inlen >= sizeof in - 1) return HH_ERR_IO;
    }

    return hh_rc_response_parse(in, resp);
}

void hh_rc_client_close(hh_rc_client_t *c)
{
    if (!c) return;
    if (c->fd >= 0) close(c->fd);
    c->fd = -1;
}
