/*
 * UDP transport for the data terminal: one IPv4 socket and a table from node
 * id to address. DSCP is set on the socket and IP fragmentation is refused.
 */
#ifndef HHSDR_DATATERM_UDP_H
#define HHSDR_DATATERM_UDP_H

#include "hhsdr/dataterm/dataterm.h"
#include <netinet/in.h>

typedef struct {
    bool               used;
    bool               learned;   /* taken from a received message */
    hh_node_id_t       node;
    struct sockaddr_in addr;
} hh_dt_udp_peer_t;

typedef struct {
    int              fd;
    uint16_t         port;
    uint64_t         tx_no_route;
    hh_dt_udp_peer_t peers[HH_DT_MAX_PEERS];
} hh_dt_udp_t;

/* port 0 picks a free port. dscp is 0 to 63. */
hh_status_t hh_dt_udp_open(hh_dt_udp_t *u, const char *bind_ip, uint16_t port, int dscp);
void hh_dt_udp_close(hh_dt_udp_t *u);

/* Parses "ID=IP" or "ID=IP:PORT". A missing port means default_port. */
hh_status_t hh_dt_udp_add_peer_str(hh_dt_udp_t *u, const char *spec, uint16_t default_port);
hh_status_t hh_dt_udp_add_peer(hh_dt_udp_t *u, hh_node_id_t node, const char *ip,
                               uint16_t port);

/* Matches hh_dt_tx_fn. ctx is the hh_dt_udp_t. */
hh_status_t hh_dt_udp_tx(hh_node_id_t to, const uint8_t *buf, size_t len, void *ctx);

/* Non-blocking read of one datagram. HH_ERR_AGAIN when none is waiting. */
hh_status_t hh_dt_udp_rx(hh_dt_udp_t *u, uint8_t *buf, size_t cap, size_t *len,
                         struct sockaddr_in *from);

/* Remembers where a node's message came from, unless the node is configured. */
void hh_dt_udp_learn(hh_dt_udp_t *u, hh_node_id_t node, const struct sockaddr_in *from);

#endif /* HHSDR_DATATERM_UDP_H */
