/* DSCP (Differentiated Services Code Point) to MAC class table. See include/hhsdr/mac/dscp.h. */
#include "hhsdr/mac/dscp.h"

#define DSCP_COUNT 64u

#define V HH_MAC_CLASS_VOICE
#define W HH_MAC_CLASS_VIDEO
#define D HH_MAC_CLASS_DATA

/*
 * One row per DSCP value 0 to 63. Voice is EF (46). Video is CS3 (24), AF31 to
 * AF33 (26, 28, 30), CS4 (32) and AF41 to AF43 (34, 36, 38). Everything else is
 * data, which includes:
 *  - CS6 (56): routing frames are control by their kind, not by marking.
 *  - CS5 (40), call signalling.
 */
static const uint8_t class_of[DSCP_COUNT] = {
    /*  0 */ D, D, D, D, D, D, D, D,
    /*  8 */ D, D, D, D, D, D, D, D,
    /* 16 */ D, D, D, D, D, D, D, D,
    /* 24 */ W, D, W, D, W, D, W, D,
    /* 32 */ W, D, W, D, W, D, W, D,
    /* 40 */ D, D, D, D, D, D, V, D,
    /* 48 */ D, D, D, D, D, D, D, D,
    /* 56 */ D, D, D, D, D, D, D, D,
};

#undef V
#undef W
#undef D

/* Returns the MAC class for a DSCP value. */
hh_mac_class_t hh_mac_class_from_dscp(uint8_t dscp)
{
    return dscp < DSCP_COUNT ? (hh_mac_class_t)class_of[dscp] : HH_MAC_CLASS_DATA;
}
