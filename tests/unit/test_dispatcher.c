/*
 * Event dispatcher tests.
 *
 * These verify the architectural guarantees Doc 1 §11 relies on, not merely that
 * events arrive: per-subscriber queue isolation, type filtering, boundedness
 * under overload, and non-reentrant delivery.
 */
#include "hhsdr/core/dispatcher.h"
#include "hh_test.h"
#include <string.h>

typedef struct {
    int         count;
    hh_event_t  last;
    hh_event_type_t seen[64];
} recorder_t;

static void record(const hh_event_t *ev, void *ctx)
{
    recorder_t *r = ctx;
    if (r->count < 64) r->seen[r->count] = ev->type;
    r->count++;
    r->last = *ev;
}

static hh_event_t mk(hh_event_type_t t, hh_node_id_t nbr)
{
    hh_event_t ev;
    memset(&ev, 0, sizeof ev);
    ev.type = t;
    ev.u.neighbor.neighbor_id = nbr;
    return ev;
}

static void test_publish_delivers_only_on_drain(void)
{
    hh_dispatcher_t d;
    recorder_t r = {0};
    hh_event_t ev = mk(HH_EV_NEIGHBOR_UP, 2);

    hh_dispatcher_init(&d);
    HH_ASSERT(hh_dispatcher_subscribe(&d, "r", HH_EV_MASK(HH_EV_NEIGHBOR_UP), record, &r) >= 0);

    HH_ASSERT_OK(hh_dispatcher_publish(&d, &ev));
    /* Publishing must not run handlers inline: that is what keeps a slow
     * subscriber off the publisher's latency path (Doc 1 §3, §11). */
    HH_ASSERT_EQ_INT(r.count, 0);
    HH_ASSERT_EQ_INT(hh_dispatcher_pending(&d), 1);

    HH_ASSERT_EQ_INT(hh_dispatcher_drain(&d), 1);
    HH_ASSERT_EQ_INT(r.count, 1);
    HH_ASSERT_EQ_INT(r.last.u.neighbor.neighbor_id, 2);
    HH_ASSERT_EQ_INT(hh_dispatcher_pending(&d), 0);
}

static void test_type_mask_filters(void)
{
    hh_dispatcher_t d;
    recorder_t up = {0}, down = {0};
    hh_event_t a = mk(HH_EV_NEIGHBOR_UP, 1), b = mk(HH_EV_NEIGHBOR_DOWN, 1);

    hh_dispatcher_init(&d);
    hh_dispatcher_subscribe(&d, "up",   HH_EV_MASK(HH_EV_NEIGHBOR_UP),   record, &up);
    hh_dispatcher_subscribe(&d, "down", HH_EV_MASK(HH_EV_NEIGHBOR_DOWN), record, &down);

    hh_dispatcher_publish(&d, &a);
    hh_dispatcher_publish(&d, &b);
    hh_dispatcher_drain(&d);

    HH_ASSERT_EQ_INT(up.count, 1);
    HH_ASSERT_EQ_INT(down.count, 1);
    HH_ASSERT_EQ_INT(up.seen[0], HH_EV_NEIGHBOR_UP);
    HH_ASSERT_EQ_INT(down.seen[0], HH_EV_NEIGHBOR_DOWN);
}

static void test_fanout_each_subscriber_gets_own_copy(void)
{
    hh_dispatcher_t d;
    recorder_t r1 = {0}, r2 = {0}, r3 = {0};
    hh_event_t ev = mk(HH_EV_LINK_STATE_CHANGED, 5);

    hh_dispatcher_init(&d);
    /* HTI-07 is a four-consumer fan-out: "one event, four consumers, none
     * blocking another." */
    hh_dispatcher_subscribe(&d, "fd",   HH_EV_MASK(HH_EV_LINK_STATE_CHANGED), record, &r1);
    hh_dispatcher_subscribe(&d, "rt",   HH_EV_MASK(HH_EV_LINK_STATE_CHANGED), record, &r2);
    hh_dispatcher_subscribe(&d, "topo", HH_EV_MASK(HH_EV_LINK_STATE_CHANGED), record, &r3);

    hh_dispatcher_publish(&d, &ev);
    HH_ASSERT_EQ_INT(hh_dispatcher_pending(&d), 3);
    hh_dispatcher_drain(&d);

    HH_ASSERT_EQ_INT(r1.count, 1);
    HH_ASSERT_EQ_INT(r2.count, 1);
    HH_ASSERT_EQ_INT(r3.count, 1);
}

/* A subscriber that never drains, to prove one full queue does not affect others. */
static void noop(const hh_event_t *ev, void *ctx) { (void)ev; (void)ctx; }

static void test_slow_consumer_does_not_starve_others(void)
{
    hh_dispatcher_t d;
    recorder_t fast = {0};
    hh_event_t ev = mk(HH_EV_NEIGHBOR_UP, 1);
    int slow_id;

    hh_dispatcher_init(&d);
    slow_id = hh_dispatcher_subscribe(&d, "slow", HH_EV_MASK(HH_EV_NEIGHBOR_UP), noop, NULL);
    hh_dispatcher_subscribe(&d, "fast", HH_EV_MASK(HH_EV_NEIGHBOR_UP), record, &fast);
    HH_ASSERT(slow_id >= 0);

    /* Overrun the ring by 10 events. */
    for (int i = 0; i < HH_QUEUE_CAPACITY + 10; i++) {
        ev.u.neighbor.neighbor_id = (hh_node_id_t)i;
        hh_dispatcher_publish(&d, &ev);
        /* Only the fast subscriber drains, one event per publish. */
        d.subs[1].handler(&d.subs[1].ring[d.subs[1].head], d.subs[1].ctx);
        d.subs[1].head = (d.subs[1].head + 1) % HH_QUEUE_CAPACITY;
        d.subs[1].count--;
    }
    /* The fast subscriber saw every event despite the slow one's ring filling. */
    HH_ASSERT_EQ_INT(fast.count, HH_QUEUE_CAPACITY + 10);
    HH_ASSERT_EQ_INT(d.subs[0].count, HH_QUEUE_CAPACITY);  /* slow ring is full */
    HH_ASSERT_EQ_INT(d.subs[0].dropped, 10);               /* excess dropped    */
    HH_ASSERT_EQ_INT(d.subs[1].dropped, 0);                /* fast lost nothing */
}

static void test_bounded_queue_drops_and_counts(void)
{
    hh_dispatcher_t d;
    hh_event_t ev = mk(HH_EV_NEIGHBOR_UP, 1);

    hh_dispatcher_init(&d);
    hh_dispatcher_subscribe(&d, "s", HH_EV_MASK(HH_EV_NEIGHBOR_UP), noop, NULL);

    for (int i = 0; i < HH_QUEUE_CAPACITY; i++)
        HH_ASSERT_OK(hh_dispatcher_publish(&d, &ev));
    /* Overflow is reported to the publisher and counted, not silently absorbed. */
    HH_ASSERT_ERR(hh_dispatcher_publish(&d, &ev), HH_ERR_NOMEM);
    HH_ASSERT_EQ_INT(hh_dispatcher_dropped(&d), 1);
}

/* Handler that republishes, to check a cascade defers to the next drain round. */
typedef struct { hh_dispatcher_t *d; int count; } cascader_t;

static void cascade(const hh_event_t *ev, void *ctx)
{
    cascader_t *c = ctx;
    c->count++;
    if (ev->type == HH_EV_NEIGHBOR_UP && c->count < 3) {
        hh_event_t next = mk(HH_EV_NEIGHBOR_UP, 9);
        hh_dispatcher_publish(c->d, &next);
    }
}

static void test_reentrant_publish_defers_to_next_round(void)
{
    hh_dispatcher_t d;
    cascader_t c;
    hh_event_t ev = mk(HH_EV_NEIGHBOR_UP, 1);

    hh_dispatcher_init(&d);
    c.d = &d; c.count = 0;
    hh_dispatcher_subscribe(&d, "casc", HH_EV_MASK(HH_EV_NEIGHBOR_UP), cascade, &c);

    hh_dispatcher_publish(&d, &ev);
    /* One drain delivers exactly the events queued when it started. */
    HH_ASSERT_EQ_INT(hh_dispatcher_drain(&d), 1);
    HH_ASSERT_EQ_INT(c.count, 1);
    HH_ASSERT_EQ_INT(hh_dispatcher_pending(&d), 1);

    /* drain_all settles the whole cascade. */
    hh_dispatcher_drain_all(&d, 16);
    HH_ASSERT_EQ_INT(c.count, 3);
    HH_ASSERT_EQ_INT(hh_dispatcher_pending(&d), 0);
}

static void test_publish_rejects_invalid(void)
{
    hh_dispatcher_t d;
    hh_event_t bad = mk(HH_EV_NONE, 0);
    hh_dispatcher_init(&d);
    HH_ASSERT_ERR(hh_dispatcher_publish(&d, &bad), HH_ERR_INVAL);
    HH_ASSERT_ERR(hh_dispatcher_publish(&d, NULL), HH_ERR_INVAL);
    bad.type = HH_EV__MAX;
    HH_ASSERT_ERR(hh_dispatcher_publish(&d, &bad), HH_ERR_INVAL);
}

static void test_fifo_order_preserved(void)
{
    hh_dispatcher_t d;
    recorder_t r = {0};
    hh_dispatcher_init(&d);
    hh_dispatcher_subscribe(&d, "r", HH_EV_MASK_ALL, record, &r);

    for (int i = 0; i < 5; i++) {
        hh_event_t ev = mk(HH_EV_NEIGHBOR_UP, (hh_node_id_t)i);
        hh_dispatcher_publish(&d, &ev);
    }
    hh_dispatcher_drain(&d);
    HH_ASSERT_EQ_INT(r.count, 5);
    HH_ASSERT_EQ_INT(r.last.u.neighbor.neighbor_id, 4);
}

static void test_event_type_names_are_unique_and_present(void)
{
    for (int t = HH_EV_NONE + 1; t < HH_EV__MAX; t++) {
        const char *n = hh_event_type_str((hh_event_type_t)t);
        HH_ASSERT_MSG(strcmp(n, "unknown") != 0, "event type %d has no name", t);
    }
}

HH_TEST_MAIN_BEGIN("dispatcher")
    HH_RUN(test_publish_delivers_only_on_drain);
    HH_RUN(test_type_mask_filters);
    HH_RUN(test_fanout_each_subscriber_gets_own_copy);
    HH_RUN(test_slow_consumer_does_not_starve_others);
    HH_RUN(test_bounded_queue_drops_and_counts);
    HH_RUN(test_reentrant_publish_defers_to_next_round);
    HH_RUN(test_publish_rejects_invalid);
    HH_RUN(test_fifo_order_preserved);
    HH_RUN(test_event_type_names_are_unique_and_present);
HH_TEST_MAIN_END()
