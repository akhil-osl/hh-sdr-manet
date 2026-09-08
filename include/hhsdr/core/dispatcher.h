/*
 * Event dispatcher — typed, bounded-queue publish/subscribe.
 *
 * Design constraints taken directly from the architecture:
 *  - "Every subscriber reads from its own queue off the same dispatcher — a slow
 *    consumer never backs up another's queue." Each subscription owns a private
 *    ring; publish copies the event into every interested ring.
 *  - No global lock. Publishing never runs subscriber callbacks inline, so a slow
 *    subsystem's latency is never coupled into a fast one's path.
 *  - Bounded: a full ring drops the event and counts the drop rather than
 *    growing without limit, which on an embedded target is the safer failure.
 *
 * Delivery is explicit: hh_dispatcher_drain() runs pending callbacks at a point
 * the caller chooses. This keeps publish O(subscribers) and makes event ordering
 * deterministic under test.
 */
#ifndef HHSDR_CORE_DISPATCHER_H
#define HHSDR_CORE_DISPATCHER_H

#include "hhsdr/core/events.h"

#define HH_MAX_SUBSCRIBERS  24
#define HH_QUEUE_CAPACITY   64

typedef void (*hh_event_handler_fn)(const hh_event_t *ev, void *ctx);

typedef struct {
    hh_event_handler_fn handler;
    void               *ctx;
    const char         *name;          /* subscriber name, for diagnostics */
    uint32_t            type_mask;     /* bit per hh_event_type_t          */
    hh_event_t          ring[HH_QUEUE_CAPACITY];
    size_t              head, tail, count;
    uint64_t            delivered;
    uint64_t            dropped;       /* events lost to a full ring       */
    bool                in_use;
} hh_subscription_t;

typedef struct {
    hh_subscription_t subs[HH_MAX_SUBSCRIBERS];
    size_t            sub_count;
    uint64_t          published;
    uint64_t          dropped_total;
    bool              draining;        /* guards against reentrant drain   */
} hh_dispatcher_t;

void        hh_dispatcher_init(hh_dispatcher_t *d);

/* Subscribe to the event types set in type_mask (see HH_EV_MASK).
 * Returns a subscription id, or negative on failure. */
int         hh_dispatcher_subscribe(hh_dispatcher_t *d, const char *name,
                                    uint32_t type_mask,
                                    hh_event_handler_fn handler, void *ctx);

/* Copy ev into each matching subscriber's ring. Never invokes handlers. */
hh_status_t hh_dispatcher_publish(hh_dispatcher_t *d, const hh_event_t *ev);

/* Invoke handlers for queued events. Returns how many were delivered.
 * Events published during a drain are picked up by the next drain, which keeps
 * a cascade (neighbor -> route -> forwarder) from recursing unboundedly. */
size_t      hh_dispatcher_drain(hh_dispatcher_t *d);

/* Drain repeatedly until no events remain or max_rounds is reached, so a test
 * can settle a full event cascade. Returns total events delivered. */
size_t      hh_dispatcher_drain_all(hh_dispatcher_t *d, size_t max_rounds);

size_t      hh_dispatcher_pending(const hh_dispatcher_t *d);
uint64_t    hh_dispatcher_dropped(const hh_dispatcher_t *d);

#define HH_EV_MASK(t) (1u << (t))
#define HH_EV_MASK_ALL 0xFFFFFFFFu

#endif /* HHSDR_CORE_DISPATCHER_H */
