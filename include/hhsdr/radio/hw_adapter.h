/*
 * Hardware radio adapter — PLACEHOLDER / NOT IMPLEMENTED.
 *
 * This is the production adapter that will bind the radio abstraction to the
 * FPGA/PL, PHY, and RF hardware. The PL implementation and its software APIs do
 * not exist yet, so every operation here returns HH_ERR_NOT_IMPLEMENTED.
 *
 * It is a real, linkable adapter that fails loudly and honestly — not a fake
 * that pretends to work. Selecting radio_adapter="hw" in configuration builds a
 * node that reports at open() that the hardware contract is unavailable.
 *
 * WHAT FUTURE FPGA INTEGRATION MUST SUPPLY (and nothing more):
 *   1. hw_open/hw_close   — acquire and release the PL/RF resources.
 *   2. hw_transmit        — hand a framed message to the modulation path.
 *   3. hw_poll            — pump demodulated frames + their measured metrics
 *                           into the registered rx callback.
 *   4. hw_get_status      — report channel, waveform, and error counters.
 *   5. hw_get_link_metrics— report per-neighbor RSSI/SNR/PER/retransmits.
 *   6. hw_set_channel     — retune, or return HH_ERR_UNSUPPORTED.
 *
 * Deliberately absent because the contract is unknown: AXI register maps, DMA
 * descriptors, PHY/modem control, RF tuner control, sample streaming, and any
 * modulation/RCC engine interface. None of these are guessed at here.
 *
 * See docs/HARDWARE-DEPENDENCIES.md for the full tracking list.
 */
#ifndef HHSDR_RADIO_HW_ADAPTER_H
#define HHSDR_RADIO_HW_ADAPTER_H

#include "hhsdr/radio/radio.h"

typedef struct {
    bool     opened;
    uint32_t channel;
} hh_hw_adapter_t;

/* Bind a handle to the hardware adapter. The handle is valid and callable; its
 * operations report HH_ERR_NOT_IMPLEMENTED until the PL contract is available. */
void hh_hw_adapter_init(hh_hw_adapter_t *a, hh_radio_t *out);

/* True while no real hardware backend is compiled in. Lets the node daemon and
 * telemetry surface the gap rather than appearing to run normally. */
bool hh_hw_adapter_is_stub(void);

#endif /* HHSDR_RADIO_HW_ADAPTER_H */
