/*
 * Route table snapshot (Doc 1 §6, §10, §12; HTI-08).
 *
 * THE fast-path contract: "the only thing the data plane ever reads from the
 * control plane is a pointer to the current route-table snapshot, swapped
 * atomically. The data plane never takes a lock the control plane can also
 * hold, never waits on a control-plane computation, and never blocks on an IPC
 * round-trip." (Doc 1 §10)
 *
 * A snapshot is IMMUTABLE once published. The Routing Engine is the sole writer:
 * it builds a new snapshot off to the side and swaps the pointer in one atomic
 * store. Readers take no lock at all — a reader holding an older snapshot keeps
 * a consistent view rather than seeing a half-updated table.
 *
 * Forwarding is a flat next-hop lookup. It never walks a graph (Doc 1 §3).
 */
#ifndef HHSDR_MANET_ROUTE_TABLE_H
#define HHSDR_MANET_ROUTE_TABLE_H

#include "hhsdr/core/types.h"
#include <stdatomic.h>

#define HH_MAX_ROUTES 256

typedef struct {
    hh_node_id_t destination;
    hh_node_id_t next_hop;
    float        metric;        /* composite; lower is better */
    hh_seq_t     sequence_no;
    uint8_t      hop_count;
    bool         valid;         /* false == marked invalid, awaiting delete   */
    /* Warm standby (Doc 1 §6): the second-best next hop, so Self-Healing can
     * switch without a full recompute. */
    hh_node_id_t alt_next_hop;
    float        alt_metric;
    uint8_t      alt_hop_count;
    bool         has_alt;
} hh_route_entry_t;

/* Immutable once published. */
typedef struct {
    uint32_t         version;
    size_t           count;
    hh_route_entry_t entries[HH_MAX_ROUTES];
} hh_route_snapshot_t;

/*
 * Double-buffered publisher. The writer builds into the inactive buffer and
 * publishes by swapping an atomic pointer; readers dereference that pointer.
 * Two buffers suffice because the writer is single and readers never retain a
 * snapshot beyond one lookup.
 */
typedef struct {
    hh_route_snapshot_t buffers[2];
    /* Points to const: readers get a read-only view, and the accessor needs
     * no const-discarding cast. */
    const hh_route_snapshot_t *_Atomic current;
    unsigned            next_buffer;
} hh_route_publisher_t;

void hh_route_publisher_init(hh_route_publisher_t *p);

/* Writer side: get the scratch buffer to build into (a copy of current). */
hh_route_snapshot_t *hh_route_publisher_begin(hh_route_publisher_t *p);

/* Writer side: publish the scratch buffer with a single atomic store. */
void hh_route_publisher_commit(hh_route_publisher_t *p, hh_route_snapshot_t *draft);

/* Reader side: lock-free. Never blocks, never takes a lock. */
const hh_route_snapshot_t *hh_route_publisher_current(const hh_route_publisher_t *p);

/* Flat next-hop lookup in a snapshot. Returns NULL when no valid route exists. */
const hh_route_entry_t *hh_route_lookup(const hh_route_snapshot_t *s, hh_node_id_t dst);

#endif /* HHSDR_MANET_ROUTE_TABLE_H */
