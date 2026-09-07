#include "hhsdr/core/dispatcher.h"
#include "hhsdr/core/log.h"
#include <string.h>

#define COMP "dispatch"

void hh_dispatcher_init(hh_dispatcher_t *d)
{
    if (!d) return;
    memset(d, 0, sizeof *d);
}

int hh_dispatcher_subscribe(hh_dispatcher_t *d, const char *name, uint32_t type_mask,
                            hh_event_handler_fn handler, void *ctx)
{
    hh_subscription_t *s;
    if (!d || !handler || d->sub_count >= HH_MAX_SUBSCRIBERS) return -1;

    s = &d->subs[d->sub_count];
    memset(s, 0, sizeof *s);
    s->handler   = handler;
    s->ctx       = ctx;
    s->name      = name ? name : "anon";
    s->type_mask = type_mask;
    s->in_use    = true;
    return (int)d->sub_count++;
}

hh_status_t hh_dispatcher_publish(hh_dispatcher_t *d, const hh_event_t *ev)
{
    bool any_dropped = false;

    if (!d || !ev || ev->type <= HH_EV_NONE || ev->type >= HH_EV__MAX) return HH_ERR_INVAL;

    d->published++;
    for (size_t i = 0; i < d->sub_count; i++) {
        hh_subscription_t *s = &d->subs[i];
        if (!s->in_use || !(s->type_mask & HH_EV_MASK(ev->type))) continue;

        if (s->count == HH_QUEUE_CAPACITY) {
            /* Bounded queue: drop rather than grow. Counted so the condition is
             * visible in telemetry instead of silently distorting behavior. */
            s->dropped++;
            d->dropped_total++;
            any_dropped = true;
            HH_LOGW(COMP, "queue_full", "subscriber=%s type=%s dropped=%llu",
                    s->name, hh_event_type_str(ev->type),
                    (unsigned long long)s->dropped);
            continue;
        }
        s->ring[s->tail] = *ev;                       /* copy by value */
        s->tail = (s->tail + 1) % HH_QUEUE_CAPACITY;
        s->count++;
    }
    return any_dropped ? HH_ERR_NOMEM : HH_OK;
}

size_t hh_dispatcher_drain(hh_dispatcher_t *d)
{
    size_t delivered = 0;

    if (!d || d->draining) return 0;
    d->draining = true;

    for (size_t i = 0; i < d->sub_count; i++) {
        hh_subscription_t *s = &d->subs[i];
        /* Snapshot the count: events this handler publishes land in the ring but
         * are left for the next drain, bounding one drain's work. */
        size_t n = s->count;
        for (size_t k = 0; k < n; k++) {
            hh_event_t ev = s->ring[s->head];
            s->head = (s->head + 1) % HH_QUEUE_CAPACITY;
            s->count--;
            s->delivered++;
            delivered++;
            s->handler(&ev, s->ctx);
        }
    }
    d->draining = false;
    return delivered;
}

size_t hh_dispatcher_drain_all(hh_dispatcher_t *d, size_t max_rounds)
{
    size_t total = 0;
    for (size_t r = 0; r < max_rounds; r++) {
        size_t n = hh_dispatcher_drain(d);
        total += n;
        if (n == 0) break;
    }
    return total;
}

size_t hh_dispatcher_pending(const hh_dispatcher_t *d)
{
    size_t n = 0;
    if (!d) return 0;
    for (size_t i = 0; i < d->sub_count; i++) n += d->subs[i].count;
    return n;
}

uint64_t hh_dispatcher_dropped(const hh_dispatcher_t *d)
{
    return d ? d->dropped_total : 0;
}
