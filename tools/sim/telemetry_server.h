/*
 * Telemetry export server — DEVELOPMENT/TEST TOOLING ONLY.
 *
 * Serves the same read-only telemetry hh_telemetry_* already produces
 * (see include/hhsdr/manet/telemetry.h) to TCP clients as newline-delimited
 * JSON, one object per simulated node per tick. Built for the MA-OI GUI
 * integration (docs/GUI-INTEGRATION-PLAN.md) so an external process can
 * visualize live netsim state without hardware.
 *
 * Never linked into hh-manet. Plaintext only: see
 * docs/GUI-INTEGRATION-PLAN.md §5a-1 — transport encryption for a fielded
 * deployment is an explicitly open decision, not implemented here.
 */
#ifndef HH_TELEMETRY_SERVER_H
#define HH_TELEMETRY_SERVER_H

#include "netsim.h"
#include <stddef.h>

typedef struct hh_telemetry_server hh_telemetry_server_t;

#define TS_MSG_ID_LEN   64
#define TS_PAYLOAD_LEN  256

/* A parsed inbound command. Only one command shape exists today ("send");
 * more can be added the same way (a command "kind" plus a payload union) if
 * this ever needs to do more than inject application data. */
typedef struct {
    bool         valid;
    char         msg_id[TS_MSG_ID_LEN];
    hh_node_id_t from;
    hh_node_id_t to;
    char         payload[TS_PAYLOAD_LEN];
    uint16_t     payload_len;
} ts_send_cmd_t;

/* Bind a listening socket on 127.0.0.1:port. Returns NULL on failure
 * (port in use, socket() failure, ...); prints the reason to stderr. */
hh_telemetry_server_t *telemetry_server_start(uint16_t port);

/* Accept any pending new connections (non-blocking) and drop any that have
 * closed. Call once per netsim tick, before or after telemetry_server_publish. */
void telemetry_server_accept(hh_telemetry_server_t *ts);

/* Serialize every node currently in `sim` to one JSON line each, and write
 * each line to every connected client. A slow/blocked client is dropped
 * rather than allowed to stall the simulation (non-blocking sends). */
void telemetry_server_publish(hh_telemetry_server_t *ts, netsim_t *sim);

/* Read any pending command lines from any client (non-blocking) and parse
 * them into `out`, up to `max_cmds` per call. Returns the count parsed.
 * Malformed lines are logged and skipped, never returned. Call once per
 * netsim tick alongside telemetry_server_accept. */
size_t telemetry_server_poll_commands(hh_telemetry_server_t *ts,
                                       ts_send_cmd_t *out, size_t max_cmds);

/* Write one JSON line to every connected client — used for send_ack /
 * send_result notifications, same broadcast path telemetry uses. There is
 * no per-client addressing: a GUI client filters by msg_id, same as it
 * already filters node telemetry by node_id. */
void telemetry_server_broadcast(hh_telemetry_server_t *ts, const char *line, size_t len);

size_t telemetry_server_client_count(const hh_telemetry_server_t *ts);

void telemetry_server_stop(hh_telemetry_server_t *ts);

#endif /* HH_TELEMETRY_SERVER_H */
