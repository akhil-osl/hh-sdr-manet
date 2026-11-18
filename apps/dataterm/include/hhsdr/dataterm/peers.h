/*
 * Node id to IPv4 address table for the data terminal. CoAP carries no node
 * id, so a sender is named by looking its IP address up here.
 */
#ifndef HHSDR_DATATERM_PEERS_H
#define HHSDR_DATATERM_PEERS_H

#include "hhsdr/core/types.h"
#include <netinet/in.h>

#define HH_DT_MAX_PEERS 16

typedef struct {
    bool               used;
    hh_node_id_t       node;
    struct sockaddr_in addr;
} hh_dt_peer_addr_t;

typedef struct {
    hh_dt_peer_addr_t p[HH_DT_MAX_PEERS];
} hh_dt_peers_t;

void hh_dt_peers_init(hh_dt_peers_t *t);

/* Adds or replaces a node. HH_ERR_NOMEM when the table is full. */
hh_status_t hh_dt_peers_add(hh_dt_peers_t *t, hh_node_id_t node, const char *ip,
                            uint16_t port);

/* Parses "ID=IP" or "ID=IP:PORT". A missing port means default_port. */
hh_status_t hh_dt_peers_add_str(hh_dt_peers_t *t, const char *spec, uint16_t default_port);

const hh_dt_peer_addr_t *hh_dt_peers_by_node(const hh_dt_peers_t *t, hh_node_id_t node);

/* Node at this IP address, port ignored. HH_NODE_ID_INVALID if unknown. */
hh_node_id_t hh_dt_peers_node_of(const hh_dt_peers_t *t, const struct sockaddr_in *a);

#endif /* HHSDR_DATATERM_PEERS_H */
