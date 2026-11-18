#include "hhsdr/dataterm/peers.h"
#include "hhsdr/core/log.h"
#include <arpa/inet.h>
#include <stdlib.h>
#include <string.h>

#define COMP "dataterm_peers"

void hh_dt_peers_init(hh_dt_peers_t *t)
{
    if (t) memset(t, 0, sizeof *t);
}

static hh_dt_peer_addr_t *find(hh_dt_peers_t *t, hh_node_id_t node)
{
    for (int i = 0; i < HH_DT_MAX_PEERS; i++)
        if (t->p[i].used && t->p[i].node == node) return &t->p[i];
    return NULL;
}

hh_status_t hh_dt_peers_add(hh_dt_peers_t *t, hh_node_id_t node, const char *ip,
                            uint16_t port)
{
    if (!t || !ip || node == HH_NODE_ID_INVALID || port == 0) return HH_ERR_INVAL;
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(port) };
    if (inet_pton(AF_INET, ip, &a.sin_addr) != 1) return HH_ERR_INVAL;
    hh_dt_peer_addr_t *p = find(t, node);
    for (int i = 0; !p && i < HH_DT_MAX_PEERS; i++)
        if (!t->p[i].used) p = &t->p[i];
    if (!p) return HH_ERR_NOMEM;
    p->used = true;
    p->node = node;
    p->addr = a;
    HH_LOGI(COMP, "peer", "node=%u ip=%s port=%u", node, ip, port);
    return HH_OK;
}

hh_status_t hh_dt_peers_add_str(hh_dt_peers_t *t, const char *spec, uint16_t default_port)
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
    return hh_dt_peers_add(t, (hh_node_id_t)node, ip, (uint16_t)port);
}

const hh_dt_peer_addr_t *hh_dt_peers_by_node(const hh_dt_peers_t *t, hh_node_id_t node)
{
    if (!t) return NULL;
    for (int i = 0; i < HH_DT_MAX_PEERS; i++)
        if (t->p[i].used && t->p[i].node == node) return &t->p[i];
    return NULL;
}

hh_node_id_t hh_dt_peers_node_of(const hh_dt_peers_t *t, const struct sockaddr_in *a)
{
    if (!t || !a) return HH_NODE_ID_INVALID;
    for (int i = 0; i < HH_DT_MAX_PEERS; i++)
        if (t->p[i].used && t->p[i].addr.sin_addr.s_addr == a->sin_addr.s_addr)
            return t->p[i].node;
    return HH_NODE_ID_INVALID;
}
