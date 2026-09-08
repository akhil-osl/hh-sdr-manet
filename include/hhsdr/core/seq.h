/*
 * 32-bit wraparound-safe sequence-number comparison (RFC 1982 serial arithmetic).
 *
 * "every beacon carries a per-node monotonic sequence number (32-bit,
 * wraparound-safe — reusing mesh_router.py's seq_gt/seq_ge logic verbatim)".
 * The same discipline is applied to route sequence numbers (
 * records the route bit-width as TBD-by-analogy; 32-bit is used consistently).
 */
#ifndef HHSDR_CORE_SEQ_H
#define HHSDR_CORE_SEQ_H

#include "hhsdr/core/types.h"

/* True when a is strictly newer than b, tolerating one wrap of the 32-bit space.
 * Exactly-half-space apart is ambiguous and reported as "not newer". */
static inline bool hh_seq_gt(hh_seq_t a, hh_seq_t b)
{
    return a != b && (hh_seq_t)(a - b) < (hh_seq_t)0x80000000u;
}

static inline bool hh_seq_ge(hh_seq_t a, hh_seq_t b) { return a == b || hh_seq_gt(a, b); }
static inline bool hh_seq_lt(hh_seq_t a, hh_seq_t b) { return hh_seq_gt(b, a); }
static inline bool hh_seq_le(hh_seq_t a, hh_seq_t b) { return a == b || hh_seq_gt(b, a); }

#endif /* HHSDR_CORE_SEQ_H */
