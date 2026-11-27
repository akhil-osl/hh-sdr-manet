/*
 * Demand table for slot allocation: the latest queue report heard from each
 * node, and the slot it was sent in. The report is the optional queue report
 * of the PDU v2 control block: one byte per MAC class, the messages the node
 * offered in a frame (the larger of those sent in the last frame and those
 * sent so far in this frame plus those still queued).
 *
 * Time is absolute slot numbers from the slot scheduler. No clock, no
 * allocation, single-threaded.
 */
#ifndef HHSDR_MAC_ALLOC_H
#define HHSDR_MAC_ALLOC_H

#include "hhsdr/core/types.h"
#include <stdbool.h>
#include <stdint.h>

#define HH_ALLOC_NODES_MAX   32u   /* nodes in the largest field net */
#define HH_ALLOC_CLASSES     5u    /* MAC classes 0 to 4, one report byte each */

typedef struct {
    bool     used;
    uint16_t node;                       /* on-air address of the sender */
    uint8_t  depth[HH_ALLOC_CLASSES];    /* messages offered per frame and class, 255 = 255 or more */
    uint64_t seen_slot;                  /* absolute slot the report was sent in */
    uint8_t  peak[HH_ALLOC_CLASSES];     /* highest depth held for smoothing, see hold_slots */
    uint64_t peak_slot[HH_ALLOC_CLASSES];/* absolute slot the held peak was reported in */
} hh_alloc_demand_t;

typedef struct {
    hh_alloc_demand_t entry[HH_ALLOC_NODES_MAX];
} hh_alloc_table_t;

/* Empties the table. */
void hh_alloc_table_init(hh_alloc_table_t *t);

/* Stores a report from a node, keeping only the latest by slot number.
 * HH_ERR_INVAL for a null argument or node 0, HH_ERR_NOMEM when a new node
 * does not fit (existing entries are left unchanged). */
hh_status_t hh_alloc_note(hh_alloc_table_t *t, uint16_t node, const uint8_t depth[HH_ALLOC_CLASSES],
                          uint64_t slot);

/* Forgets a node, as when it has left. A node that is not in the table is not an error. */
void hh_alloc_remove(hh_alloc_table_t *t, uint16_t node);

/* Forgets every node silent for more than gone_slots, which the allocation no longer counts as members. */
void hh_alloc_evict_gone(hh_alloc_table_t *t, uint64_t now_slot, uint32_t gone_slots);

/* The latest report of a node, or NULL if none was heard. */
const hh_alloc_demand_t *hh_alloc_find(const hh_alloc_table_t *t, uint16_t node);

/* Slots since the report was sent. 0 if now is not after it. */
uint64_t hh_alloc_age(const hh_alloc_demand_t *d, uint64_t now_slot);

#define HH_ALLOC_SLOTS_MAX   64u   /* slots per frame, as in the slot scheduler */

/*
 * Slot allocation. Every node runs hh_alloc_compute() on the same demand
 * table and gets the same map, so no node owns the map. The result depends
 * only on the parameters, the table, now_slot and the superframe number: no
 * clock, no random numbers, no floating point, and not on the order in which
 * reports arrived.
 *
 * Order of allocation:
 *  1. Slots in `fixed_slots` (shared entry, pre-emption) are never allocated.
 *  2. Every member gets one slot, the keep-alive slot of its PDU.
 *  3. Voice demand comes next, from a reserve of `voice_reserve` slots, lowest
 *     node id first.
 *  4. The remaining slots are split between video and data by weight, and
 *     inside a class one slot at a time to each node that still wants more.
 *  5. Slots nobody asked for are dealt round-robin to all members, except
 *     join_reserve of them, which stay idle for network entry.
 *     The reserve is held back from the demand dealing too.
 * Demand is smoothed (hold_slots): after a drop, the highest recent report keeps
 * counting until it is hold_slots old, then the latest report counts. The
 * layout depends on the quotas and on superframe / rot_period only, so with
 * unchanged demand the map does not change inside a rot_period.
 * No node gets more than node_cap slots, whatever the class. What a capped
 * node cannot take goes to the others, and a slot nobody may take stays idle.
 * Control and emergency traffic are outside the sharing.
 *
 * Every value in hh_alloc_params_t marked TODO is a placeholder until the
 * network owner decides it.
 */
typedef struct {
    uint16_t slots_per_frame;                   /* 1 to HH_ALLOC_SLOTS_MAX */
    uint64_t fixed_slots;                       /* bit n = slot n is never allocated */
    uint8_t  voice_reserve;                     /* slots kept for voice. TODO */
    uint8_t  msgs_per_slot[HH_ALLOC_CLASSES];   /* messages one slot carries per frame, 1 or more; classes 2 to 4 used. TODO */
    uint8_t  weight_video;                      /* share of the slots left after voice. TODO */
    uint8_t  weight_data;                       /* same, for data. TODO */
    uint8_t  node_cap;                          /* most slots one node may hold per frame, keep-alive slot included; 0 = no cap. TODO */
    uint8_t  join_reserve;                      /* slots kept idle, even against demand, so a joining radio finds a free one; 0 = none. TODO */
    uint32_t stale_slots;                       /* a report older than this counts as no demand. TODO */
    uint32_t gone_slots;                        /* a node silent for this long is no longer a member. TODO */
    uint32_t hold_slots;                        /* a higher report counts at once; a lower one only after the highest has been held this long; 0 = no smoothing. TODO */
    uint16_t rot_period;                        /* superframes between changes of who is dealt the spare slots first; 0 or 1 = every superframe. TODO */
} hh_alloc_params_t;

typedef struct {
    uint16_t owner[HH_ALLOC_SLOTS_MAX];         /* node of each slot, 0 = nobody */
} hh_alloc_map_t;

/* Placeholder values for a frame of slots_per_frame slots (TODO in alloc.c). */
void hh_alloc_params_default(hh_alloc_params_t *p, uint16_t slots_per_frame);

/* HH_ERR_INVAL if the parameters cannot be used. */
hh_status_t hh_alloc_params_check(const hh_alloc_params_t *p);

/* Computes the map. HH_ERR_NOMEM when there are more members than slots to
 * allocate; out is then unchanged. */
hh_status_t hh_alloc_compute(const hh_alloc_params_t *p, const hh_alloc_table_t *t, uint64_t now_slot,
                             uint32_t superframe, hh_alloc_map_t *out);

/*
 * Voice admission. Both calls only read the table and use the rule of the
 * allocation: the keep-alive slot of a node carries its first voice message of
 * a frame, every further message needs a slot from the reserve, and the cap
 * limits what one node may hold. Reports older than the stale limit count as
 * no voice.
 *
 * Limit: the answer follows the reports of the last frame. Two nodes that ask
 * at the same time can both be told yes. The allocator then serves voice from
 * the lowest node id up, so the node with the higher id can lose slots. Only a
 * flow age in the report would let the older call win (TODO: not defined).
 */

/* Voice slots of the reserve that the voice demand in the table does not take. */
unsigned hh_alloc_voice_free(const hh_alloc_params_t *p, const hh_alloc_table_t *t, uint64_t now_slot);

/* True if node can offer msgs more voice messages per frame and still be given every voice slot it needs. */
bool hh_alloc_voice_admit(const hh_alloc_params_t *p, const hh_alloc_table_t *t, uint64_t now_slot, uint16_t node,
                          unsigned msgs);

#endif /* HHSDR_MAC_ALLOC_H */
