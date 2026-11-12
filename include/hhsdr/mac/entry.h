/*
 * Network entry: the MAC commands 0x01 to 0x04 of MAC frame specification v2
 * (4.7), the joining side (listen, request in a shared entry slot with
 * random back-off, grant, join) and the member side (answer, no slot granted
 * twice, leave). Rules 14, 15 and 26 of part 5 and MAC-03 section 6.1.
 *
 * Time is slot numbers from the slot scheduler. No clock, no allocation.
 * Slot sets are bit masks, bit n = slot n of the frame (up to 64 slots).
 *
 * Not modelled: authentication of requests and grants, the channel check,
 * network time acquisition, merging nets, ageing out silent members.
 */
#ifndef HHSDR_MAC_ENTRY_H
#define HHSDR_MAC_ENTRY_H

#include "hhsdr/core/types.h"
#include "hhsdr/mac/mac.h"
#include "hhsdr/mac/slot.h"

#define HH_ENTRY_CMD_REQUEST 0x01u
#define HH_ENTRY_CMD_GRANT   0x02u
#define HH_ENTRY_CMD_REJECT  0x03u
#define HH_ENTRY_CMD_LEAVE   0x04u
#define HH_ENTRY_CMD_MAX     24u        /* grant with a 8-byte bitmap: 12 + 8 */
#define HH_ENTRY_OFFSET_NONE ((int16_t)0x7FFF)

#define HH_ENTRY_PEERS_MAX   32u
#define HH_ENTRY_PROV_MAX    8u

typedef enum {
    HH_ENTRY_REJECT_FULL = 1,
    HH_ENTRY_REJECT_NOT_AUTHORISED = 2,
    HH_ENTRY_REJECT_BAD_REQUEST = 3
} hh_entry_reject_reason_t;

typedef struct {
    uint32_t request_id;
    uint8_t  attempt;
    uint8_t  slots_wanted;
    uint8_t  bitmap_bytes;       /* L */
    uint64_t free_slots;         /* heard free during listen */
} hh_entry_request_t;

typedef struct {
    uint32_t request_id;
    uint32_t apply_at;           /* superframe from which the slots are owned */
    uint8_t  bitmap_bytes;
    uint64_t slots;
    int16_t  arrival_offset;     /* 0.1 us steps, HH_ENTRY_OFFSET_NONE = not measured */
} hh_entry_grant_t;

typedef struct {
    uint32_t request_id;
    uint8_t  reason;             /* hh_entry_reject_reason_t; other values mean bad request */
} hh_entry_reject_t;

typedef struct {
    uint32_t release_at;         /* the slots are free from this superframe */
    uint8_t  bitmap_bytes;
    uint64_t slots;
} hh_entry_leave_t;

typedef struct {
    uint8_t id;                  /* HH_ENTRY_CMD_* */
    union {
        hh_entry_request_t request;
        hh_entry_grant_t   grant;
        hh_entry_reject_t  reject;
        hh_entry_leave_t   leave;
    } u;
} hh_entry_cmd_t;

/* Writes the command body (message data of a protocol 0x20 message). */
hh_status_t hh_entry_cmd_encode(const hh_entry_cmd_t *c, uint8_t *buf, size_t cap, size_t *out_len);

/* HH_ERR_UNSUPPORTED: unknown command ID, to be skipped and counted.
 * HH_ERR_INVAL: known command with a wrong length or a bitmap that is too long. */
hh_status_t hh_entry_cmd_decode(const uint8_t *buf, size_t len, hh_entry_cmd_t *c);

typedef struct {
    uint8_t  n_listen;       /* superframes heard before the first request */
    uint8_t  be_min;         /* first window is 2^be_min opportunities */
    uint8_t  be_max;
    uint8_t  r_max;          /* requests per entry cycle */
    uint8_t  c_max;          /* cycles before giving up */
    uint16_t t_grant;        /* frames to wait for a response */
    uint16_t t_holdoff;      /* superframes to wait after giving up */
    uint8_t  slots_wanted;
    uint8_t  apply_lead;     /* superframes from the grant to the apply-at superframe */
    uint8_t  t_confirm;      /* superframes after apply-at before an unused grant lapses */
    uint8_t  n_leave;        /* superframes in which a leaving radio sends its leave */
} hh_entry_params_t;

/* Values still to be decided are marked TODO in entry.c. */
void hh_entry_params_default(hh_entry_params_t *p);

typedef enum {
    HH_ENTRY_OFF = 0,        /* owns no slot, not joining */
    HH_ENTRY_LISTEN,
    HH_ENTRY_BACKOFF,        /* counting entry opportunities */
    HH_ENTRY_WAIT_GRANT,
    HH_ENTRY_GRANTED,        /* grant accepted, waiting for the apply-at superframe */
    HH_ENTRY_HOLDOFF,        /* gave up, waiting before a new episode */
    HH_ENTRY_REFUSED,        /* rejected for good, until the operator acts */
    HH_ENTRY_MEMBER          /* owns slots, answers requests */
} hh_entry_state_t;

typedef struct {
    /* Joining side. Slots are absolute slot numbers. */
    uint64_t t_start;
    uint64_t t_granted;
    uint64_t t_joined;       /* first burst in a granted slot */
    uint32_t requests_tx;
    uint32_t timeouts;
    uint32_t cycles;
    uint32_t ignored_responses;
    uint32_t bad_grants;
    uint32_t rejects_rx;
    bool     joined;
    /* Member side. */
    uint32_t requests_rx;
    uint32_t grants_tx;
    uint32_t rejects_tx;
    uint32_t duplicates;
    uint32_t no_free_slot;
    uint32_t table_full;
    uint32_t lapsed_grants;
    uint32_t bad_commands;
    uint32_t unknown_commands;
} hh_entry_stats_t;

typedef struct {
    uint16_t src;            /* the joining radio */
    uint32_t request_id;
    bool     is_reject;
    uint8_t  reason;
    uint64_t slots;
    uint32_t apply_at;
    uint32_t expire_sf;
    bool     used;
    bool     mine;           /* this radio answered; otherwise heard from another member */
    bool     pending;        /* to be sent in the next own slot */
    bool     assigned;       /* apply_at is fixed */
} hh_entry_prov_t;

typedef struct {
    uint16_t src;
    uint64_t claims;
    uint32_t release_at;     /* 0 = none */
    uint64_t release_slots;
    bool     used;
} hh_entry_peer_t;

/* What the slot scheduler must do after a call. */
typedef struct {
    bool                own_changed;
    uint64_t            own_slots;
    bool                failed;
    hh_mac_entry_fail_t fail_reason;
} hh_entry_act_t;

struct hh_entry {
    hh_entry_params_t p;
    uint16_t          node_id;
    uint16_t          spf;
    uint16_t          bitmap_bytes;
    uint32_t          sp_super;     /* slots per superframe */
    uint32_t          sp_frame;
    uint64_t          valid;        /* slots of the frame */
    uint64_t          entry_slots;
    uint32_t          rng;
    hh_entry_state_t  state;
    uint64_t          own;
    /* Joining side. */
    uint64_t          busy;         /* slots heard in use */
    uint64_t          listen_end, deadline, holdoff_end;
    uint32_t          backoff;
    uint32_t          req_id;
    bool              req_out;
    uint8_t           be, tries, cycles, attempt;
    uint64_t          granted;
    uint32_t          apply_at;
    bool              first_tx_pending;
    /* Leaving. */
    bool              leaving;
    uint8_t           leave_sent;
    uint32_t          leave_sf;     /* superframe of the last leave sent */
    uint32_t          leave_release;
    hh_entry_peer_t   peers[HH_ENTRY_PEERS_MAX];
    hh_entry_prov_t   prov[HH_ENTRY_PROV_MAX];
    hh_entry_stats_t  st;
};
typedef struct hh_entry hh_entry_t;

/* seed makes the back-off draws of this radio differ from every other radio's
 * (TODO: where a real radio gets its entropy is not decided). A radio that
 * owns slots at init is a member, otherwise it is OFF until hh_entry_join. */
hh_status_t hh_entry_init(hh_entry_t *e, const hh_entry_params_t *p, const hh_slot_config_t *slot_cfg,
                          uint32_t seed);

hh_entry_state_t hh_entry_state(const hh_entry_t *e);

/* Starts a new entry episode. Only from OFF or REFUSED. */
hh_status_t hh_entry_join(hh_entry_t *e, const hh_slot_pos_t *pos);

/* Starts the leave: the command goes out in n_leave consecutive superframes. */
hh_status_t hh_entry_leave(hh_entry_t *e, const hh_slot_pos_t *pos);

/* Once per slot, before the slot is used: timeouts, apply-at, lapsing. */
void hh_entry_tick(hh_entry_t *e, const hh_slot_pos_t *pos, hh_entry_act_t *act);

/* A shared entry slot has come. True: send the request in cmd in this slot. */
bool hh_entry_on_opportunity(hh_entry_t *e, const hh_slot_pos_t *pos, hh_entry_cmd_t *cmd);

/* A CRC-good PDU was heard: who sent it and which slots it claims. */
void hh_entry_on_pdu(hh_entry_t *e, const hh_slot_pos_t *pos, uint16_t src, uint64_t claims);

/* A MAC command message was heard, whatever its destination. */
void hh_entry_on_command(hh_entry_t *e, const hh_slot_pos_t *pos, uint16_t src, uint16_t dst,
                         const uint8_t *body, size_t len, hh_entry_act_t *act);

/* Next command to put in this radio's own-slot PDU: an answer to a request,
 * or the leave. body_max is the room left. False when there is none. */
bool hh_entry_next_command(hh_entry_t *e, const hh_slot_pos_t *pos, uint16_t body_max, uint8_t *buf,
                           size_t *len, uint16_t *dst);

/* An own-slot PDU was sent: the first one after joining is the confirmation. */
void hh_entry_on_own_tx(hh_entry_t *e, const hh_slot_pos_t *pos);

/* Slot bitmap of L bytes, bit n of the mask = bit n of the byte string. */
uint64_t hh_entry_mask_from_bytes(const uint8_t *b, unsigned n);

#endif /* HHSDR_MAC_ENTRY_H */
