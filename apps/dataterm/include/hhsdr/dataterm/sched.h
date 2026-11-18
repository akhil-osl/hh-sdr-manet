/*
 * Send order for the data terminal. Four classes, each with its own DSCP:
 * voice (EF), signalling (CS5), video (AF41) and data (default). The next
 * message to send is chosen by class, highest first, with two guards:
 *
 *  - Voice has a token bucket, so a voice flood cannot take every send.
 *  - Data is picked at least once in every data_every picks while it waits,
 *    so higher classes cannot starve it.
 *
 * The module holds counts only. The caller owns the queues and the clock.
 */
#ifndef HHSDR_DATATERM_SCHED_H
#define HHSDR_DATATERM_SCHED_H

#include "hhsdr/core/clock.h"

/* Order is the send order, highest first. */
typedef enum {
    HH_DT_CLASS_VOICE = 0,   /* DSCP EF   */
    HH_DT_CLASS_SIGNAL,      /* DSCP CS5  */
    HH_DT_CLASS_VIDEO,       /* DSCP AF41 */
    HH_DT_CLASS_DATA,        /* DSCP 0    */
    HH_DT_CLASS_COUNT
} hh_dt_class_t;

typedef struct {
    uint32_t data_every;       /* data is picked at least once in this many picks */
    uint32_t voice_burst;      /* voice tokens at most                            */
    uint32_t voice_refill_ms;  /* one voice token per this many ms                */
} hh_dt_sched_config_t;

typedef struct {
    hh_dt_sched_config_t cfg;
    uint32_t             since_data;  /* picks since data was last picked */
    uint32_t             voice_tokens;
    hh_time_ms_t         voice_last;
} hh_dt_sched_t;

/* DSCP for a class: 46, 40, 34 or 0. */
int hh_dt_class_dscp(hh_dt_class_t cls);
const char *hh_dt_class_str(hh_dt_class_t cls);

void hh_dt_sched_config_default(hh_dt_sched_config_t *cfg);
hh_status_t hh_dt_sched_init(hh_dt_sched_t *s, const hh_dt_sched_config_t *cfg, hh_time_ms_t now);

/* waiting[c] is how many messages wait in class c. Returns the class to send
 * from next, or -1 when nothing may be sent now. Counts the pick. */
int hh_dt_sched_pick(hh_dt_sched_t *s, const uint16_t waiting[HH_DT_CLASS_COUNT],
                     hh_time_ms_t now);

#endif /* HHSDR_DATATERM_SCHED_H */
