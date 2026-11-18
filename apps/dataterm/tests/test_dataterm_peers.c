/* Peer table: parsing, lookup by node and by IP address. */
#include "hh_test.h"
#include "hhsdr/core/log.h"
#include "hhsdr/dataterm/peers.h"
#include <arpa/inet.h>

static void quiet(hh_log_level_t lvl, const char *line, void *ctx) { (void)lvl; (void)line; (void)ctx; }

static void test_parse(void)
{
    hh_dt_peers_t t;
    hh_dt_peers_init(&t);
    HH_ASSERT_OK(hh_dt_peers_add_str(&t, "2=10.0.2.2", 5683));
    HH_ASSERT_OK(hh_dt_peers_add_str(&t, "3=10.0.1.3:6000", 5683));
    HH_ASSERT_EQ_INT(ntohs(hh_dt_peers_by_node(&t, 2)->addr.sin_port), 5683);
    HH_ASSERT_EQ_INT(ntohs(hh_dt_peers_by_node(&t, 3)->addr.sin_port), 6000);
    HH_ASSERT_ERR(hh_dt_peers_add_str(&t, "0=10.0.0.1", 5683), HH_ERR_INVAL);
    HH_ASSERT_ERR(hh_dt_peers_add_str(&t, "4=nonsense", 5683), HH_ERR_INVAL);
    HH_ASSERT_ERR(hh_dt_peers_add_str(&t, "5", 5683), HH_ERR_INVAL);
    HH_ASSERT_ERR(hh_dt_peers_add_str(&t, "6=10.0.0.6:70000", 5683), HH_ERR_INVAL);
    HH_ASSERT(hh_dt_peers_by_node(&t, 9) == NULL);
}

static void test_lookup_by_ip_and_replace(void)
{
    hh_dt_peers_t t;
    hh_dt_peers_init(&t);
    HH_ASSERT_OK(hh_dt_peers_add(&t, 2, "10.0.2.2", 5683));
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(40000) };
    inet_pton(AF_INET, "10.0.2.2", &a.sin_addr);
    HH_ASSERT_EQ_INT(hh_dt_peers_node_of(&t, &a), 2);   /* port is ignored */
    inet_pton(AF_INET, "10.0.2.9", &a.sin_addr);
    HH_ASSERT_EQ_INT(hh_dt_peers_node_of(&t, &a), HH_NODE_ID_INVALID);

    HH_ASSERT_OK(hh_dt_peers_add(&t, 2, "10.0.2.9", 5683));  /* replaces */
    HH_ASSERT_EQ_INT(hh_dt_peers_node_of(&t, &a), 2);
}

static void test_table_full(void)
{
    hh_dt_peers_t t;
    hh_dt_peers_init(&t);
    for (unsigned i = 1; i <= HH_DT_MAX_PEERS; i++)
        HH_ASSERT_OK(hh_dt_peers_add(&t, i, "10.0.0.1", 5683));
    HH_ASSERT_ERR(hh_dt_peers_add(&t, 100, "10.0.0.1", 5683), HH_ERR_NOMEM);
}

HH_TEST_MAIN_BEGIN("dataterm_peers")
    hh_log_set_sink(quiet, NULL);
    HH_RUN(test_parse);
    HH_RUN(test_lookup_by_ip_and_replace);
    HH_RUN(test_table_full);
    hh_log_reset();
HH_TEST_MAIN_END()
