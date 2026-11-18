#include "emu_medium.h"
#include <string.h>

void emu_medium_init(emu_medium_t *m)
{
    if (m) memset(m, 0, sizeof *m);
}

static int port_of(const emu_medium_t *m, hh_node_id_t id)
{
    for (size_t i = 0; i < EMU_MAX_PORTS; i++)
        if (m->ports[i].used && m->ports[i].id == id) return (int)i;
    return -1;
}

int emu_medium_attach(emu_medium_t *m, hh_node_id_t id, mock_radio_t *radio)
{
    if (!m || !radio || id == HH_NODE_ID_INVALID || port_of(m, id) >= 0) return -1;
    for (size_t i = 0; i < EMU_MAX_PORTS; i++) {
        if (m->ports[i].used) continue;
        m->ports[i].used  = true;
        m->ports[i].id    = id;
        m->ports[i].radio = radio;
        m->port_count++;
        return (int)i;
    }
    return -1;
}

static void set_link(emu_medium_t *m, hh_node_id_t a, hh_node_id_t b, bool up, float rssi)
{
    int ia, ib;
    if (!m) return;
    ia = port_of(m, a);
    ib = port_of(m, b);
    if (ia < 0 || ib < 0 || ia == ib) return;
    m->link_up[ia][ib] = m->link_up[ib][ia] = up;
    m->rssi[ia][ib] = m->rssi[ib][ia] = rssi;
}

void emu_medium_link_up(emu_medium_t *m, hh_node_id_t a, hh_node_id_t b, float rssi)
{
    set_link(m, a, b, true, rssi);
}

void emu_medium_link_down(emu_medium_t *m, hh_node_id_t a, hh_node_id_t b)
{
    set_link(m, a, b, false, 0.0f);
}

void emu_medium_step(emu_medium_t *m, hh_time_ms_t now)
{
    if (!m) return;
    for (size_t i = 0; i < EMU_MAX_PORTS; i++) {
        emu_port_t *tx = &m->ports[i];
        size_t pending;
        if (!tx->used) continue;

        pending = tx->radio->tx_count < MOCK_TX_LOG ? tx->radio->tx_count : MOCK_TX_LOG;
        for (size_t k = 0; k < pending; k++) {
            const hh_frame_t *f = &tx->radio->tx_log[k];

            for (size_t j = 0; j < EMU_MAX_PORTS; j++) {
                emu_port_t *rx = &m->ports[j];
                hh_link_sample_t s;
                if (j == i || !rx->used) continue;
                if (!m->link_up[i][j]) { m->out_of_range++; continue; }
                /* MAC address filter. */
                if (f->dst != HH_NODE_ID_INVALID && f->dst != rx->id) {
                    m->not_addressed++;
                    continue;
                }
                memset(&s, 0, sizeof s);
                s.neighbor_id = tx->id;
                s.rssi = m->rssi[i][j];
                mock_radio_enqueue_rx(rx->radio, f, &s, now);
                m->delivered++;
            }
        }
        mock_radio_clear_tx_log(tx->radio);
    }
}
