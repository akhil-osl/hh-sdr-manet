/*
 * Packet Forwarder tests (Doc 1 §3, §10).
 *
 * The architectural claims under test: forwarding reads the published snapshot
 * and makes no routing decision of its own; a route miss buffers or drops
 * rather than blocking on the control plane; TTL bounds loops; and the pending
 * queue is bounded.
 */
#include "hhsdr/dataplane/forwarder.h"
#include "hhsdr/manet/route_table.h"
#include "mock_radio.h"
#include "hh_test.h"
#include "vclock.h"
#include <string.h>

typedef struct {
    hh_config_t          cfg;
    vclock_t             vc;
    mock_radio_t         mock;
    hh_radio_t           radio;
    hh_route_publisher_t pub;
    hh_forwarder_t       fw;
} fix_t;

static void fix_init(fix_t *f)
{
    memset(f, 0, sizeof *f);
    hh_config_defaults(&f->cfg);
    f->cfg.node_id = 1;
    vclock_init(&f->vc, 1000);
    mock_radio_init(&f->mock, "fw", &f->radio);
    hh_radio_open(&f->radio);
    hh_route_publisher_init(&f->pub);
    hh_forwarder_init(&f->fw, &f->cfg, &f->radio, &f->pub);
}

/* Publish a route as the Routing Engine would. */
static void publish_route(fix_t *f, hh_node_id_t dst, hh_node_id_t next_hop)
{
    hh_route_snapshot_t *d = hh_route_publisher_begin(&f->pub);
    bool found = false;
    for (size_t i = 0; i < d->count; i++) {
        if (d->entries[i].destination == dst) {
            d->entries[i].next_hop = next_hop;
            d->entries[i].valid = true;
            found = true;
        }
    }
    if (!found) {
        hh_route_entry_t *e = &d->entries[d->count++];
        memset(e, 0, sizeof *e);
        e->destination = dst;
        e->next_hop = next_hop;
        e->valid = true;
        e->hop_count = 2;
    }
    hh_route_publisher_commit(&f->pub, d);
}

static void invalidate_route(fix_t *f, hh_node_id_t dst)
{
    hh_route_snapshot_t *d = hh_route_publisher_begin(&f->pub);
    for (size_t i = 0; i < d->count; i++)
        if (d->entries[i].destination == dst) d->entries[i].valid = false;
    hh_route_publisher_commit(&f->pub, d);
}

static void test_forwards_via_published_next_hop(void)
{
    fix_t f; fix_init(&f);
    uint8_t payload[] = { 1, 2, 3, 4 };

    publish_route(&f, 9, 2);
    HH_ASSERT_OK(hh_forwarder_send(&f.fw, 9, payload, sizeof payload, 8, f.vc.now));

    /* The frame goes to the NEXT HOP the snapshot named, not the destination. */
    const hh_frame_t *tx = mock_radio_last_tx(&f.mock, HH_FRAME_DATA);
    HH_ASSERT(tx != NULL);
    HH_ASSERT_EQ_INT(tx->dst, 2);
    HH_ASSERT_EQ_INT(f.fw.forwarded, 1);
}

static void test_local_delivery_never_touches_radio(void)
{
    fix_t f; fix_init(&f);
    uint8_t payload[] = { 7 };

    HH_ASSERT_OK(hh_forwarder_send(&f.fw, 1, payload, sizeof payload, 8, f.vc.now));
    HH_ASSERT_EQ_INT(f.fw.delivered_local, 1);
    HH_ASSERT_EQ_INT(f.mock.tx_total, 0);
}

static void test_route_miss_buffers_without_blocking(void)
{
    fix_t f; fix_init(&f);
    uint8_t payload[] = { 1 };

    /* Doc 1 §10: a miss is buffered while the control plane resolves it
     * asynchronously — the forwarder must not compute a route itself. */
    HH_ASSERT_ERR(hh_forwarder_send(&f.fw, 9, payload, sizeof payload, 8, f.vc.now),
                  HH_ERR_AGAIN);
    HH_ASSERT_EQ_INT(hh_forwarder_pending(&f.fw), 1);
    HH_ASSERT_EQ_INT(f.mock.tx_total, 0);
    HH_ASSERT_EQ_INT(f.fw.dropped_no_route, 1);

    /* Once the control plane publishes a route, the buffered packet goes out. */
    publish_route(&f, 9, 2);
    HH_ASSERT_EQ_INT(hh_forwarder_flush(&f.fw, f.vc.now, 5000), 1);
    HH_ASSERT_EQ_INT(hh_forwarder_pending(&f.fw), 0);
    HH_ASSERT_EQ_INT(f.mock.tx_total, 1);
    HH_ASSERT_EQ_INT(f.fw.requeued_ok, 1);
}

static void test_pending_queue_is_bounded(void)
{
    fix_t f; fix_init(&f);
    uint8_t payload[] = { 1 };
    f.cfg.forward_queue_depth = 4;

    for (int i = 0; i < 4; i++)
        HH_ASSERT_ERR(hh_forwarder_send(&f.fw, 9, payload, 1, 8, f.vc.now), HH_ERR_AGAIN);
    HH_ASSERT_EQ_INT(hh_forwarder_pending(&f.fw), 4);

    /* Beyond the bound, drop rather than grow: a routing outage must not become
     * memory exhaustion. */
    HH_ASSERT_ERR(hh_forwarder_send(&f.fw, 9, payload, 1, 8, f.vc.now), HH_ERR_NOMEM);
    HH_ASSERT_EQ_INT(f.fw.dropped_queue_full, 1);
    HH_ASSERT_EQ_INT(hh_forwarder_pending(&f.fw), 4);
}

static void test_buffered_packets_expire(void)
{
    fix_t f; fix_init(&f);
    uint8_t payload[] = { 1 };

    hh_forwarder_send(&f.fw, 9, payload, 1, 8, f.vc.now);
    HH_ASSERT_EQ_INT(hh_forwarder_pending(&f.fw), 1);

    /* Buffered "briefly", not indefinitely. */
    vclock_advance(&f.vc, 5001);
    hh_forwarder_flush(&f.fw, f.vc.now, 5000);
    HH_ASSERT_EQ_INT(hh_forwarder_pending(&f.fw), 0);
    HH_ASSERT_EQ_INT(f.mock.tx_total, 0);
}

static void test_invalidated_route_stops_forwarding_immediately(void)
{
    fix_t f; fix_init(&f);
    uint8_t payload[] = { 1 };

    publish_route(&f, 9, 2);
    HH_ASSERT_OK(hh_forwarder_send(&f.fw, 9, payload, 1, 8, f.vc.now));

    /* Phase 1 of two-phase invalidation must stop traffic at once, before the
     * entry is deleted. */
    invalidate_route(&f, 9);
    HH_ASSERT_ERR(hh_forwarder_send(&f.fw, 9, payload, 1, 8, f.vc.now), HH_ERR_AGAIN);
    HH_ASSERT_EQ_INT(f.fw.forwarded, 1);
}

static void test_relay_decrements_ttl_and_rewrites_next_hop(void)
{
    fix_t f; fix_init(&f);
    hh_frame_t in;
    const hh_frame_t *tx;

    /* A frame from node 5 destined for node 9, arriving at us as a relay. */
    memset(&in, 0, sizeof in);
    in.kind = HH_FRAME_DATA;
    in.src = 5;
    memcpy(in.data + 0, (uint8_t[]){5,0,0,0}, 4);
    memcpy(in.data + 4, (uint8_t[]){9,0,0,0}, 4);
    in.data[8] = 5;    /* ttl */
    in.data[9] = 1;    /* hops so far */
    in.len = 12;

    publish_route(&f, 9, 3);
    HH_ASSERT_OK(hh_forwarder_forward(&f.fw, &in, f.vc.now));

    tx = mock_radio_last_tx(&f.mock, HH_FRAME_DATA);
    HH_ASSERT(tx != NULL);
    HH_ASSERT_EQ_INT(tx->dst, 3);        /* our next hop */
    HH_ASSERT_EQ_INT(tx->data[8], 4);    /* ttl decremented */
    HH_ASSERT_EQ_INT(tx->data[9], 2);    /* hop count incremented */
    /* The true source and destination survive the relay. */
    HH_ASSERT_EQ_INT(tx->data[0], 5);
    HH_ASSERT_EQ_INT(tx->data[4], 9);
}

static void test_ttl_exhaustion_drops(void)
{
    fix_t f; fix_init(&f);
    hh_frame_t in;

    memset(&in, 0, sizeof in);
    in.kind = HH_FRAME_DATA;
    in.src = 5;
    memcpy(in.data + 4, (uint8_t[]){9,0,0,0}, 4);
    in.data[8] = 1;     /* last hop */
    in.len = 12;

    publish_route(&f, 9, 3);
    /* TTL bounds a loop surviving a transient routing inconsistency. */
    HH_ASSERT_ERR(hh_forwarder_forward(&f.fw, &in, f.vc.now), HH_ERR_INVAL);
    HH_ASSERT_EQ_INT(f.fw.dropped_ttl, 1);
    HH_ASSERT_EQ_INT(f.mock.tx_total, 0);
}

static void test_relay_delivers_locally_when_addressed_to_us(void)
{
    fix_t f; fix_init(&f);
    hh_frame_t in;

    memset(&in, 0, sizeof in);
    in.kind = HH_FRAME_DATA;
    in.src = 5;
    memcpy(in.data + 4, (uint8_t[]){1,0,0,0}, 4);   /* dst == our node id */
    in.data[8] = 5;
    in.len = 12;

    HH_ASSERT_OK(hh_forwarder_forward(&f.fw, &in, f.vc.now));
    HH_ASSERT_EQ_INT(f.fw.delivered_local, 1);
    HH_ASSERT_EQ_INT(f.mock.tx_total, 0);   /* not retransmitted */
}

static void test_tx_error_reported_not_silently_dropped(void)
{
    fix_t f; fix_init(&f);
    uint8_t payload[] = { 1 };

    publish_route(&f, 9, 2);
    f.mock.tx_fails = true;
    HH_ASSERT_ERR(hh_forwarder_send(&f.fw, 9, payload, 1, 8, f.vc.now), HH_ERR_IO);
    HH_ASSERT_EQ_INT(f.fw.dropped_tx_error, 1);
}

static void test_rejects_malformed_input(void)
{
    fix_t f; fix_init(&f);
    uint8_t payload[] = { 1 };
    hh_frame_t runt;

    HH_ASSERT_ERR(hh_forwarder_send(&f.fw, 9, payload, 1, 0, f.vc.now), HH_ERR_INVAL);
    HH_ASSERT_ERR(hh_forwarder_send(&f.fw, 9, payload, HH_RADIO_MAX_FRAME, 8, f.vc.now),
                  HH_ERR_INVAL);
    memset(&runt, 0, sizeof runt);
    runt.kind = HH_FRAME_DATA;
    runt.len = 3;   /* shorter than the header */
    HH_ASSERT_ERR(hh_forwarder_forward(&f.fw, &runt, f.vc.now), HH_ERR_INVAL);
}

static void test_forwarder_tracks_snapshot_version(void)
{
    fix_t f; fix_init(&f);
    uint8_t payload[] = { 1 };
    uint32_t v1;

    publish_route(&f, 9, 2);
    hh_forwarder_send(&f.fw, 9, payload, 1, 8, f.vc.now);
    v1 = f.fw.last_snapshot_version;

    /* After a republish the forwarder observes the new version on its next
     * lookup — it always reads the current pointer, never a cached table. */
    publish_route(&f, 9, 3);
    hh_forwarder_send(&f.fw, 9, payload, 1, 8, f.vc.now);
    HH_ASSERT(f.fw.last_snapshot_version > v1);
    HH_ASSERT_EQ_INT(mock_radio_last_tx(&f.mock, HH_FRAME_DATA)->dst, 3);
}

HH_TEST_MAIN_BEGIN("forwarder")
    HH_RUN(test_forwards_via_published_next_hop);
    HH_RUN(test_local_delivery_never_touches_radio);
    HH_RUN(test_route_miss_buffers_without_blocking);
    HH_RUN(test_pending_queue_is_bounded);
    HH_RUN(test_buffered_packets_expire);
    HH_RUN(test_invalidated_route_stops_forwarding_immediately);
    HH_RUN(test_relay_decrements_ttl_and_rewrites_next_hop);
    HH_RUN(test_ttl_exhaustion_drops);
    HH_RUN(test_relay_delivers_locally_when_addressed_to_us);
    HH_RUN(test_tx_error_reported_not_silently_dropped);
    HH_RUN(test_rejects_malformed_input);
    HH_RUN(test_forwarder_tracks_snapshot_version);
HH_TEST_MAIN_END()
