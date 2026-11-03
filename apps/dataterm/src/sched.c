#include "hhsdr/dataterm/sched.h"

int hh_dt_class_dscp(hh_dt_class_t cls)
{
    switch (cls) {
    case HH_DT_CLASS_VOICE:  return 46;  /* EF   */
    case HH_DT_CLASS_SIGNAL: return 40;  /* CS5  */
    case HH_DT_CLASS_VIDEO:  return 34;  /* AF41 */
    default:                 return 0;
    }
}

const char *hh_dt_class_str(hh_dt_class_t cls)
{
    switch (cls) {
    case HH_DT_CLASS_VOICE:  return "voice";
    case HH_DT_CLASS_SIGNAL: return "signal";
    case HH_DT_CLASS_VIDEO:  return "video";
    case HH_DT_CLASS_DATA:   return "data";
    default:                 return "?";
    }
}

void hh_dt_sched_config_default(hh_dt_sched_config_t *cfg)
{
    if (!cfg) return;
    /* TODO: confirm all three; no document gives them. */
    cfg->data_every = 8;
    cfg->voice_burst = 8;
    cfg->voice_refill_ms = 20;
}

hh_status_t hh_dt_sched_init(hh_dt_sched_t *s, const hh_dt_sched_config_t *cfg, hh_time_ms_t now)
{
    if (!s || !cfg || !cfg->data_every || !cfg->voice_burst || !cfg->voice_refill_ms)
        return HH_ERR_INVAL;
    s->cfg = *cfg;
    s->since_data = 0;
    s->voice_tokens = cfg->voice_burst;
    s->voice_last = now;
    return HH_OK;
}

static void voice_refill(hh_dt_sched_t *s, hh_time_ms_t now)
{
    if (now <= s->voice_last) return;
    hh_time_ms_t add = (now - s->voice_last) / s->cfg.voice_refill_ms;
    if (!add) return;
    s->voice_tokens = add >= s->cfg.voice_burst - s->voice_tokens
                          ? s->cfg.voice_burst
                          : s->voice_tokens + (uint32_t)add;
    s->voice_last += add * s->cfg.voice_refill_ms;
}

int hh_dt_sched_pick(hh_dt_sched_t *s, const uint16_t waiting[HH_DT_CLASS_COUNT],
                     hh_time_ms_t now)
{
    if (!s || !waiting) return -1;
    voice_refill(s, now);
    int pick = -1;
    if (waiting[HH_DT_CLASS_DATA] && s->since_data >= s->cfg.data_every - 1u) {
        pick = HH_DT_CLASS_DATA;
    } else {
        for (int c = 0; c < HH_DT_CLASS_COUNT && pick < 0; c++) {
            if (!waiting[c]) continue;
            if (c == HH_DT_CLASS_VOICE && !s->voice_tokens) continue;
            pick = c;
        }
    }
    if (pick < 0) return -1;
    if (pick == HH_DT_CLASS_VOICE) s->voice_tokens--;
    /* Data waiting counts toward its share, data not waiting does not. */
    if (pick == HH_DT_CLASS_DATA) s->since_data = 0;
    else if (waiting[HH_DT_CLASS_DATA]) s->since_data++;
    return pick;
}
