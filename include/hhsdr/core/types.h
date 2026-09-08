/*
 * HH-SDR MANET — core shared types.
 *
 * Types marked TBD carry a placeholder representation; the placeholder is
 * recommended, not defined, until a HW/PHY decision fixes it.
 */
#ifndef HHSDR_CORE_TYPES_H
#define HHSDR_CORE_TYPES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* HTI IDL: typedef unsigned long NodeId / SequenceNo. */
typedef uint32_t hh_node_id_t;
typedef uint32_t hh_seq_t;

#define HH_NODE_ID_INVALID ((hh_node_id_t)0u)

/* Monotonic time, milliseconds. Virtualised via hh_clock (core/clock.h) so the
 * whole stack is deterministically testable without wall-clock dependence. */
typedef uint64_t hh_time_ms_t;

/* Uniform status. Kept small and non-errno so it can cross the SCA boundary. */
typedef enum {
    HH_OK = 0,
    HH_ERR_INVAL,        /* caller passed an invalid argument            */
    HH_ERR_NOMEM,        /* bounded table/queue full, or allocation fail */
    HH_ERR_NOTFOUND,     /* lookup miss                                  */
    HH_ERR_AGAIN,        /* would block / retry later                    */
    HH_ERR_STATE,        /* operation invalid in current lifecycle state */
    HH_ERR_UNSUPPORTED,  /* not implemented by this adapter/backend      */
    HH_ERR_NOT_IMPLEMENTED, /* HARDWARE-DEPENDENT / TBD: no real backend */
    HH_ERR_IO            /* underlying transport/hardware failure        */
} hh_status_t;

const char *hh_status_str(hh_status_t s);

/*
 * Link health state machine.
 * Order is significant: worse states compare greater.
 */
typedef enum {
    HH_LINK_HEALTHY = 0,
    HH_LINK_DEGRADED,
    HH_LINK_SUSPECTED_FAILURE,
    HH_LINK_FAILED,
    HH_LINK_RECOVERING
} hh_link_state_t;

const char *hh_link_state_str(hh_link_state_t s);

/*
 * Failure cause classification.
 * The exact enumeration is TBD; this is the closed set derived from the five
 * described failure patterns.
 */
typedef enum {
    HH_CAUSE_UNKNOWN = 0,
    HH_CAUSE_NODE_FAILURE,       /* clean silence, one neighbor only        */
    HH_CAUSE_RF_INTERFERENCE,    /* CRC errors up, RSSI present, many peers */
    HH_CAUSE_MOBILITY,           /* RSSI trending down, PER rising in step  */
    HH_CAUSE_OWN_RADIO_FAILURE,  /* total silence, all neighbors, all chans */
    HH_CAUSE_ASYMMETRIC_LINK     /* beacons land, data ACKs fail            */
} hh_cause_hint_t;

const char *hh_cause_hint_str(hh_cause_hint_t c);

/* Capability bit-field (encoding TBD).
 * These bits are this implementation's encoding, not a hardware contract. */
#define HH_CAP_ROUTING_CAPABLE (1u << 0)
#define HH_CAP_LEAF_ONLY       (1u << 1)
#define HH_CAP_RELAY_HIGH_CAP  (1u << 2)

#endif /* HHSDR_CORE_TYPES_H */
