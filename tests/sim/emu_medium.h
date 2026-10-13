/*
 * Emulated radio medium, test only. Connects mock radios over up/down links
 * and applies the MAC address filter (this node or broadcast). Unlike netsim
 * it does not need hh_node_t.
 */
#ifndef HH_EMU_MEDIUM_H
#define HH_EMU_MEDIUM_H

#include "mock_radio.h"

#define EMU_MAX_PORTS 8

typedef struct {
    hh_node_id_t  id;
    mock_radio_t *radio;
    bool          used;
} emu_port_t;

typedef struct {
    emu_port_t ports[EMU_MAX_PORTS];
    size_t     port_count;
    bool       link_up[EMU_MAX_PORTS][EMU_MAX_PORTS];   /* [from][to] */
    float      rssi[EMU_MAX_PORTS][EMU_MAX_PORTS];
    uint64_t   delivered;
    uint64_t   not_addressed;
    uint64_t   out_of_range;
} emu_medium_t;

void emu_medium_init(emu_medium_t *m);

/* Returns the port index, or -1. */
int  emu_medium_attach(emu_medium_t *m, hh_node_id_t id, mock_radio_t *radio);

/* Links are symmetric and start down. */
void emu_medium_link_up(emu_medium_t *m, hh_node_id_t a, hh_node_id_t b, float rssi);
void emu_medium_link_down(emu_medium_t *m, hh_node_id_t a, hh_node_id_t b);

/* Receivers see the frames on their next hh_radio_poll(). */
void emu_medium_step(emu_medium_t *m, hh_time_ms_t now);

#endif /* HH_EMU_MEDIUM_H */
