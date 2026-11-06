/*
 * Slot scheduler: the MAC logic side of a TDMA frame, as in the slot engine
 * specification (docs/SLOT-ENGINE-SPEC.md). On each own slot it takes queued
 * frames from hh_mac, packs them into one PDU v2 and hands the PDU to a
 * transmit function. Received PDUs are decoded and passed to hh_mac.
 *
 * Time comes only from the injected clock. The transmit function is the seam
 * to the air: a simulated medium today, the logic's PDU buffer on hardware
 * (TODO: how a PDU is named for its slot there, not defined yet).
 *
 * Not modelled: guard time, hop timing, handover one frame ahead, beacon body,
 * Block ACK, link reports, fragmentation, security, entry and pre-emption
 * slots. Single-threaded, no allocation.
 */
#ifndef HHSDR_MAC_SLOT_H
#define HHSDR_MAC_SLOT_H

#include "hhsdr/core/clock.h"
#include "hhsdr/mac/mac.h"
#include "hhsdr/mac/pdu.h"

#define HH_SLOT_FRAME_MAX 64u   /* slots per frame, one bit each in own_slots */

typedef struct {
    uint16_t     slots_per_frame;        /* 1 to 64; default 20 */
    uint16_t     frames_per_superframe;  /* default 50 */
    uint16_t     slot_ms;                /* default 1 */
    uint16_t     capacity;               /* bytes in one PDU, CRC included */
    uint16_t     net_id;
    hh_node_id_t node_id;                /* on-air address, 1 to 0xEFFF */
    uint64_t     own_slots;              /* bit n = own slot n of every frame */
    hh_time_ms_t epoch_ms;               /* start of slot 0 of superframe 0 */
} hh_slot_config_t;

typedef struct {
    uint64_t abs_slot;
    uint32_t superframe;
    uint16_t frame;      /* within the superframe */
    uint16_t slot;       /* within the frame */
} hh_slot_pos_t;

/* Sends one PDU in the slot named by pos. Returns false if it could not. */
typedef bool (*hh_slot_tx_fn)(void *ctx, const hh_slot_pos_t *pos, const uint8_t *pdu, size_t len);

typedef struct {
    uint64_t slots_seen;
    uint64_t own_slots;
    uint64_t own_slots_missed;   /* poll came after the slot ended */
    uint64_t pdus_tx;
    uint64_t pdus_empty;         /* control block only */
    uint64_t tx_failed;
    uint64_t capacity_bytes;     /* summed over PDUs sent */
    uint64_t pdu_bytes;
    uint64_t overhead_bytes;     /* header, control block, CRC */
    uint64_t control_bytes;      /* class 0 messages (routing, beacon, commands), sub-headers included */
    uint64_t routing_bytes;      /* routing messages only, sub-headers included */
    uint64_t slots_with_control; /* PDUs carrying at least one class 0 message */
    uint64_t slots_with_routing; /* PDUs carrying at least one routing message */
    uint64_t user_bytes;         /* class 1 to 4 messages, sub-headers included */
    uint64_t messages_tx;
    uint64_t messages_dropped;   /* destination cannot be addressed on air */
    uint64_t rx_pdus;
    uint64_t rx_bad;             /* CRC or length */
    uint64_t rx_unsupported;     /* security or elements */
    uint64_t rx_wrong_net;
    uint64_t rx_messages_skipped;/* unknown protocol, class or address */
} hh_slot_stats_t;

typedef struct {
    hh_slot_config_t  cfg;
    const hh_clock_t *clock;
    hh_mac_t         *mac;
    hh_slot_tx_fn     tx;
    void             *tx_ctx;
    bool              started;
    uint64_t          next_slot;
    uint8_t           seq;
    uint8_t           buf[HH_PDU_MAX_LEN];
    hh_pdu_t          pdu;
    hh_mac_tx_frame_t frames[HH_PDU_MSG_MAX];
    hh_slot_stats_t   st;
} hh_slot_t;

/* Defaults: 20 slots, 50 frames, 1 ms. capacity is a placeholder (see docs). */
void hh_slot_config_default(hh_slot_config_t *cfg, hh_node_id_t node_id, uint16_t net_id);

/* Checks the configuration and limits the MAC's maximum payload to what one
 * PDU can carry, so larger frames are refused at hh_mac_send. */
hh_status_t hh_slot_init(hh_slot_t *s, const hh_slot_config_t *cfg, const hh_clock_t *clock,
                         hh_mac_t *mac, hh_slot_tx_fn tx, void *tx_ctx);

/* Runs every slot that ended since the last call. Transmits in the current
 * slot only; an own slot that already ended is counted as missed. */
void hh_slot_poll(hh_slot_t *s);

/* A received PDU, as the logic hands it up, with the slot it arrived in. */
void hh_slot_receive(hh_slot_t *s, const hh_slot_pos_t *pos, const uint8_t *pdu, size_t len);

/* Share of own-slot capacity used by class 0 messages, in thousandths. The
 * header, control block and CRC bytes are in overhead_bytes, not attributed. */
uint32_t hh_slot_control_share_permille(const hh_slot_stats_t *st);

#endif /* HHSDR_MAC_SLOT_H */
