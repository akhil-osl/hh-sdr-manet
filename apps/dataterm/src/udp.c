#include "hhsdr/dataterm/udp.h"
#include "hhsdr/core/log.h"
#include <arpa/inet.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define COMP "dataterm_udp"

hh_status_t hh_dt_udp_open(hh_dt_udp_t *u, const char *bind_ip, uint16_t port, int dscp)
{
    if (!u || dscp < 0 || dscp > 63) return HH_ERR_INVAL;
    memset(u, 0, sizeof *u);
    u->fd = -1;

    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(port) };
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind_ip && inet_pton(AF_INET, bind_ip, &a.sin_addr) != 1) return HH_ERR_INVAL;

    int fd = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        HH_LOGE(COMP, "socket_failed", "errno=%d (%s)", errno, strerror(errno));
        return HH_ERR_IO;
    }
    int tos = dscp << 2;
    int pmtu = IP_PMTUDISC_DO;
    if (setsockopt(fd, IPPROTO_IP, IP_TOS, &tos, sizeof tos) < 0 ||
        setsockopt(fd, IPPROTO_IP, IP_MTU_DISCOVER, &pmtu, sizeof pmtu) < 0 ||
        bind(fd, (struct sockaddr *)&a, sizeof a) < 0) {
        HH_LOGE(COMP, "socket_setup_failed", "port=%u errno=%d (%s)", port, errno,
                strerror(errno));
        close(fd);
        return HH_ERR_IO;
    }
    socklen_t al = sizeof a;
    getsockname(fd, (struct sockaddr *)&a, &al);
    u->fd = fd;
    u->port = ntohs(a.sin_port);
    HH_LOGI(COMP, "opened", "port=%u dscp=%d df=on", u->port, dscp);
    return HH_OK;
}

void hh_dt_udp_close(hh_dt_udp_t *u)
{
    if (!u || u->fd < 0) return;
    close(u->fd);
    u->fd = -1;
}

static hh_dt_udp_peer_t *find(hh_dt_udp_t *u, hh_node_id_t node)
{
    for (int i = 0; i < HH_DT_MAX_PEERS; i++)
        if (u->peers[i].used && u->peers[i].node == node) return &u->peers[i];
    return NULL;
}

static hh_dt_udp_peer_t *slot(hh_dt_udp_t *u, hh_node_id_t node)
{
    hh_dt_udp_peer_t *p = find(u, node);
    if (p) return p;
    for (int i = 0; i < HH_DT_MAX_PEERS; i++)
        if (!u->peers[i].used) return &u->peers[i];
    return NULL;
}

hh_status_t hh_dt_udp_add_peer(hh_dt_udp_t *u, hh_node_id_t node, const char *ip,
                               uint16_t port)
{
    if (!u || !ip || node == HH_NODE_ID_INVALID || port == 0) return HH_ERR_INVAL;
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(port) };
    if (inet_pton(AF_INET, ip, &a.sin_addr) != 1) return HH_ERR_INVAL;
    hh_dt_udp_peer_t *p = slot(u, node);
    if (!p) return HH_ERR_NOMEM;
    p->used = true;
    p->learned = false;
    p->node = node;
    p->addr = a;
    HH_LOGI(COMP, "peer", "node=%u ip=%s port=%u source=config", node, ip, port);
    return HH_OK;
}

hh_status_t hh_dt_udp_add_peer_str(hh_dt_udp_t *u, const char *spec, uint16_t default_port)
{
    if (!spec) return HH_ERR_INVAL;
    char buf[64];
    if (strlen(spec) >= sizeof buf) return HH_ERR_INVAL;
    strcpy(buf, spec);

    char *eq = strchr(buf, '=');
    if (!eq) return HH_ERR_INVAL;
    *eq = '\0';
    char *end = NULL;
    unsigned long long node = strtoull(buf, &end, 0);
    if (end == buf || *end || node == 0 || node > UINT32_MAX) return HH_ERR_INVAL;

    char *ip = eq + 1;
    unsigned long long port = default_port;
    char *colon = strchr(ip, ':');
    if (colon) {
        *colon = '\0';
        port = strtoull(colon + 1, &end, 10);
        if (end == colon + 1 || *end || port == 0 || port > 65535) return HH_ERR_INVAL;
    }
    return hh_dt_udp_add_peer(u, (hh_node_id_t)node, ip, (uint16_t)port);
}

hh_status_t hh_dt_udp_tx(hh_node_id_t to, const uint8_t *buf, size_t len, void *ctx)
{
    hh_dt_udp_t *u = ctx;
    if (!u || u->fd < 0 || !buf) return HH_ERR_INVAL;
    hh_dt_udp_peer_t *p = find(u, to);
    if (!p) {
        u->tx_no_route++;
        return HH_ERR_NOTFOUND;
    }
    ssize_t n = sendto(u->fd, buf, len, 0, (const struct sockaddr *)&p->addr, sizeof p->addr);
    if (n < 0) {
        /* EMSGSIZE here means the message would need IP fragmentation. */
        HH_LOGW(COMP, "sendto_failed", "to=%u len=%zu errno=%d (%s)", to, len, errno,
                strerror(errno));
        return errno == EAGAIN || errno == EWOULDBLOCK ? HH_ERR_AGAIN : HH_ERR_IO;
    }
    return HH_OK;
}

hh_status_t hh_dt_udp_rx(hh_dt_udp_t *u, uint8_t *buf, size_t cap, size_t *len,
                         struct sockaddr_in *from)
{
    if (!u || u->fd < 0 || !buf || !len) return HH_ERR_INVAL;
    struct sockaddr_in a;
    socklen_t al = sizeof a;
    ssize_t n = recvfrom(u->fd, buf, cap, MSG_TRUNC, (struct sockaddr *)&a, &al);
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) return HH_ERR_AGAIN;
        HH_LOGW(COMP, "recvfrom_failed", "errno=%d (%s)", errno, strerror(errno));
        return HH_ERR_IO;
    }
    if ((size_t)n > cap) {
        HH_LOGW(COMP, "rx_dropped", "reason=too_large len=%zd cap=%zu", n, cap);
        return HH_ERR_NOMEM;
    }
    *len = (size_t)n;
    if (from) *from = a;
    return HH_OK;
}

void hh_dt_udp_learn(hh_dt_udp_t *u, hh_node_id_t node, const struct sockaddr_in *from)
{
    if (!u || !from || node == HH_NODE_ID_INVALID) return;
    hh_dt_udp_peer_t *p = find(u, node);
    if (p && !p->learned) return;
    if (p && p->addr.sin_addr.s_addr == from->sin_addr.s_addr &&
        p->addr.sin_port == from->sin_port) return;
    if (!p) p = slot(u, node);
    if (!p) return;
    p->used = true;
    p->learned = true;
    p->node = node;
    p->addr = *from;
    char ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &from->sin_addr, ip, sizeof ip);
    HH_LOGI(COMP, "peer", "node=%u ip=%s port=%u source=learned", node, ip,
            ntohs(from->sin_port));
}
