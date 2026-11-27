/*
 * IP DSCP to MAC traffic class, for a data frame handed to hh_mac_send().
 *
 * The MAC never reads packet contents, so the caller (the network binding)
 * reads the DSCP and passes the class in. Routing and beacon frames get the
 * control class from their kind and are not mapped here. The emergency class
 * is set only through the break-in function and is not reachable by DSCP. The
 * MAC refuses the control class on a data frame, so this never returns it.
 *
 * EF is voice. CS3, AF31 to AF33, CS4 and AF41 to AF43 are video. Everything
 * else is data, call signalling (CS5) included. The MAC to network interface
 * specification section 7.1 lists only EF and AF41 to AF43.
 */
#ifndef HHSDR_MAC_DSCP_H
#define HHSDR_MAC_DSCP_H

#include "hhsdr/mac/mac.h"
#include <stdint.h>

/* Returns voice, video or data. A value above 63 is not a DSCP and gives data. */
hh_mac_class_t hh_mac_class_from_dscp(uint8_t dscp);

#endif /* HHSDR_MAC_DSCP_H */
