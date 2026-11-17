/*
 * DSCP to MAC class: the table row by row, what is not reachable by DSCP, and
 * the marks the data terminal uses.
 */
#include "hhsdr/mac/dscp.h"
#include "hh_test.h"
#include <stdio.h>

#define DSCP_EF   46
#define DSCP_AF41 34
#define DSCP_AF42 36
#define DSCP_AF43 38
#define DSCP_CS5  40
#define DSCP_CS6  56

/* Every one of the 64 DSCP values: only EF is voice, only AF41 to AF43 are video, the rest data. */
static void test_every_dscp_value(void)
{
    for (unsigned d = 0; d < 64; d++) {
        hh_mac_class_t c = hh_mac_class_from_dscp((uint8_t)d);
        if (d == DSCP_EF) HH_ASSERT_EQ_INT(c, HH_MAC_CLASS_VOICE);
        else if (d == DSCP_AF41 || d == DSCP_AF42 || d == DSCP_AF43) HH_ASSERT_EQ_INT(c, HH_MAC_CLASS_VIDEO);
        else HH_ASSERT_EQ_INT(c, HH_MAC_CLASS_DATA);
    }
}

/* No input gives control or emergency: the MAC refuses control on a data frame, and emergency is set by break-in only. */
static void test_control_and_emergency_not_reachable(void)
{
    for (unsigned d = 0; d < 256; d++) {
        hh_mac_class_t c = hh_mac_class_from_dscp((uint8_t)d);
        HH_ASSERT(c == HH_MAC_CLASS_VOICE || c == HH_MAC_CLASS_VIDEO || c == HH_MAC_CLASS_DATA);
    }
    HH_ASSERT_EQ_INT(hh_mac_class_from_dscp(DSCP_CS6), HH_MAC_CLASS_DATA);
}

/* A value above 63 is not a DSCP, so it is data. */
static void test_out_of_range_is_data(void)
{
    HH_ASSERT_EQ_INT(hh_mac_class_from_dscp(64), HH_MAC_CLASS_DATA);
    HH_ASSERT_EQ_INT(hh_mac_class_from_dscp(255), HH_MAC_CLASS_DATA);
}

/* The data terminal marks voice EF, call signalling CS5, video AF41 and data 0. Signalling lands in data. */
static void test_data_terminal_marks(void)
{
    HH_ASSERT_EQ_INT(hh_mac_class_from_dscp(DSCP_EF), HH_MAC_CLASS_VOICE);
    HH_ASSERT_EQ_INT(hh_mac_class_from_dscp(DSCP_CS5), HH_MAC_CLASS_DATA);
    HH_ASSERT_EQ_INT(hh_mac_class_from_dscp(DSCP_AF41), HH_MAC_CLASS_VIDEO);
    HH_ASSERT_EQ_INT(hh_mac_class_from_dscp(0), HH_MAC_CLASS_DATA);
}

/* Prints the table for the record: the values of voice and video, and how many values are data. */
static void test_print_table(void)
{
    unsigned data = 0;

    printf("dscp_map class=voice dscp=");
    for (unsigned d = 0; d < 64; d++)
        if (hh_mac_class_from_dscp((uint8_t)d) == HH_MAC_CLASS_VOICE) printf("%u ", d);
    printf("\ndscp_map class=video dscp=");
    for (unsigned d = 0; d < 64; d++)
        if (hh_mac_class_from_dscp((uint8_t)d) == HH_MAC_CLASS_VIDEO) printf("%u ", d);
    for (unsigned d = 0; d < 64; d++)
        if (hh_mac_class_from_dscp((uint8_t)d) == HH_MAC_CLASS_DATA) data++;
    printf("\ndscp_map class=data dscp=all other values count=%u\n", data);
    HH_ASSERT_EQ_INT(data, 60);
}

HH_TEST_MAIN_BEGIN("dscp")
    HH_RUN(test_every_dscp_value);
    HH_RUN(test_control_and_emergency_not_reachable);
    HH_RUN(test_out_of_range_is_data);
    HH_RUN(test_data_terminal_marks);
    HH_RUN(test_print_table);
HH_TEST_MAIN_END()
