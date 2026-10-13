#include "hhsdr/protocol/rc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const char *hh_rc_state_str(hh_rc_state_t s)
{
    switch (s) {
    case HH_RC_STATE_CREATED:      return "created";
    case HH_RC_STATE_INITIALIZED:  return "initialized";
    case HH_RC_STATE_CONFIGURED:   return "configured";
    case HH_RC_STATE_RUNNING:      return "running";
    case HH_RC_STATE_STOPPED:      return "stopped";
    case HH_RC_STATE_FAULTED:      return "faulted";
    case HH_RC_STATE_RELEASED:     return "released";
    default:                       return "unknown";
    }
}

static const char *g_cmd_names[HH_RC_CMD__MAX] = {
    [HH_RC_CMD_INIT]          = "init",
    [HH_RC_CMD_CONFIGURE]     = "configure",
    [HH_RC_CMD_START]         = "start",
    [HH_RC_CMD_STOP]          = "stop",
    [HH_RC_CMD_SHUTDOWN]      = "shutdown",
    [HH_RC_CMD_STATUS]        = "status",
    [HH_RC_CMD_STATS]         = "stats",
    [HH_RC_CMD_SET_CHANNEL]   = "set_channel",
    [HH_RC_CMD_INJECT_FAULT]  = "inject_fault",
    [HH_RC_CMD_CLEAR_FAULT]   = "clear_fault",
};

const char *hh_rc_cmd_str(hh_rc_cmd_t c)
{
    return (c >= 0 && c < HH_RC_CMD__MAX) ? g_cmd_names[c] : "unknown";
}

bool hh_rc_cmd_parse(const char *s, hh_rc_cmd_t *out)
{
    if (!s || !out) return false;
    for (int i = 0; i < HH_RC_CMD__MAX; i++) {
        if (strcmp(s, g_cmd_names[i]) == 0) { *out = (hh_rc_cmd_t)i; return true; }
    }
    return false;
}

const char *hh_rc_fault_str(hh_rc_fault_t f)
{
    switch (f) {
    case HH_RC_FAULT_NONE:        return "none";
    case HH_RC_FAULT_TX_FAILURE:  return "tx_failure";
    case HH_RC_FAULT_RX_SILENCE:  return "rx_silence";
    case HH_RC_FAULT_HW_FAULT:    return "hw_fault";
    case HH_RC_FAULT_BACKEND_IO:  return "backend_io";
    default:                      return "unknown";
    }
}

bool hh_rc_fault_parse(const char *s, hh_rc_fault_t *out)
{
    if (!s || !out) return false;
    if (!strcmp(s, "none"))        { *out = HH_RC_FAULT_NONE;       return true; }
    if (!strcmp(s, "tx_failure"))  { *out = HH_RC_FAULT_TX_FAILURE; return true; }
    if (!strcmp(s, "rx_silence"))  { *out = HH_RC_FAULT_RX_SILENCE; return true; }
    if (!strcmp(s, "hw_fault"))    { *out = HH_RC_FAULT_HW_FAULT;   return true; }
    if (!strcmp(s, "backend_io"))  { *out = HH_RC_FAULT_BACKEND_IO; return true; }
    return false;
}

/* Split "key=value" tokens from a mutable copy of the line, one per call. */
static char *next_token(char **cursor)
{
    char *tok;
    if (!*cursor) return NULL;
    while (**cursor == ' ') (*cursor)++;
    if (**cursor == '\0') return NULL;
    tok = *cursor;
    char *sp = strchr(tok, ' ');
    if (sp) { *sp = '\0'; *cursor = sp + 1; } else { *cursor = NULL; }
    return tok;
}

static bool kv_split(char *tok, const char **k, const char **v)
{
    char *eq = strchr(tok, '=');
    if (!eq) return false;
    *eq = '\0';
    *k = tok;
    *v = eq + 1;
    return true;
}

hh_status_t hh_rc_request_parse(const char *line, hh_rc_request_t *out)
{
    char buf[HH_RC_MAX_LINE];
    char *cursor, *tok;
    size_t len;

    if (!line || !out) return HH_ERR_INVAL;
    len = strlen(line);
    while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) len--;
    if (len == 0 || len >= sizeof buf) return HH_ERR_INVAL;
    memcpy(buf, line, len);
    buf[len] = '\0';

    memset(out, 0, sizeof *out);

    cursor = buf;
    tok = next_token(&cursor);
    if (!tok || !hh_rc_cmd_parse(tok, &out->cmd)) return HH_ERR_INVAL;

    while ((tok = next_token(&cursor)) != NULL) {
        const char *k, *v;
        if (!kv_split(tok, &k, &v)) return HH_ERR_INVAL;
        if (!strcmp(k, "node_id"))      out->node_id = (hh_node_id_t)strtoul(v, NULL, 10);
        else if (!strcmp(k, "channel")) out->channel = (uint32_t)strtoul(v, NULL, 10);
        else if (!strcmp(k, "kind")) {
            if (!hh_rc_fault_parse(v, &out->fault)) return HH_ERR_INVAL;
        } else return HH_ERR_INVAL;
    }
    return HH_OK;
}

size_t hh_rc_request_format(const hh_rc_request_t *r, char *buf, size_t cap)
{
    int n;
    if (!r || !buf || cap == 0) return 0;

    switch (r->cmd) {
    case HH_RC_CMD_CONFIGURE:
        n = snprintf(buf, cap, "%s node_id=%u channel=%u\n",
                     hh_rc_cmd_str(r->cmd), r->node_id, r->channel);
        break;
    case HH_RC_CMD_SET_CHANNEL:
        n = snprintf(buf, cap, "%s channel=%u\n", hh_rc_cmd_str(r->cmd), r->channel);
        break;
    case HH_RC_CMD_INJECT_FAULT:
        n = snprintf(buf, cap, "%s kind=%s\n", hh_rc_cmd_str(r->cmd), hh_rc_fault_str(r->fault));
        break;
    default:
        n = snprintf(buf, cap, "%s\n", hh_rc_cmd_str(r->cmd));
        break;
    }
    return (n < 0 || (size_t)n >= cap) ? 0 : (size_t)n;
}

hh_status_t hh_rc_response_parse(const char *line, hh_rc_response_t *out)
{
    char buf[HH_RC_MAX_LINE];
    char *cursor, *tok;
    size_t len;
    bool ok;

    if (!line || !out) return HH_ERR_INVAL;
    len = strlen(line);
    while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) len--;
    if (len == 0 || len >= sizeof buf) return HH_ERR_INVAL;
    memcpy(buf, line, len);
    buf[len] = '\0';

    memset(out, 0, sizeof *out);

    cursor = buf;
    tok = next_token(&cursor);
    if (!tok) return HH_ERR_INVAL;
    if (!strcmp(tok, "ok")) ok = true;
    else if (!strcmp(tok, "err")) ok = false;
    else return HH_ERR_INVAL;
    out->ok = ok;

    tok = next_token(&cursor);
    if (!tok || !hh_rc_cmd_parse(tok, &out->cmd)) return HH_ERR_INVAL;

    while ((tok = next_token(&cursor)) != NULL) {
        const char *k, *v;
        if (!kv_split(tok, &k, &v)) return HH_ERR_INVAL;

        if (!strcmp(k, "reason"))          out->reason = (hh_status_t)strtol(v, NULL, 10);
        else if (!strcmp(k, "state"))      { for (int i = 0; i <= HH_RC_STATE_RELEASED; i++)
                                                  if (!strcmp(v, hh_rc_state_str((hh_rc_state_t)i))) out->state = (hh_rc_state_t)i; }
        else if (!strcmp(k, "operational")) out->operational = (strcmp(v, "1") == 0);
        else if (!strcmp(k, "channel"))     out->channel = (uint32_t)strtoul(v, NULL, 10);
        else if (!strcmp(k, "frequency_hz"))out->frequency_hz = strtof(v, NULL);
        else if (!strcmp(k, "waveform_id")) out->waveform_id = (uint32_t)strtoul(v, NULL, 10);
        else if (!strcmp(k, "frames_tx"))   out->frames_tx = strtoull(v, NULL, 10);
        else if (!strcmp(k, "frames_rx"))   out->frames_rx = strtoull(v, NULL, 10);
        else if (!strcmp(k, "tx_errors"))   out->tx_errors = strtoull(v, NULL, 10);
        else if (!strcmp(k, "rx_errors"))   out->rx_errors = strtoull(v, NULL, 10);
        else if (!strcmp(k, "requests_total"))    out->requests_total = strtoull(v, NULL, 10);
        else if (!strcmp(k, "requests_rejected")) out->requests_rejected = strtoull(v, NULL, 10);
        else return HH_ERR_INVAL;
    }
    return HH_OK;
}

size_t hh_rc_response_format(const hh_rc_response_t *r, char *buf, size_t cap)
{
    int n;
    const char *tag;
    if (!r || !buf || cap == 0) return 0;
    tag = r->ok ? "ok" : "err";

    if (!r->ok) {
        n = snprintf(buf, cap, "%s %s reason=%d\n", tag, hh_rc_cmd_str(r->cmd), (int)r->reason);
        return (n < 0 || (size_t)n >= cap) ? 0 : (size_t)n;
    }

    switch (r->cmd) {
    case HH_RC_CMD_STATUS:
        n = snprintf(buf, cap,
            "%s %s state=%s operational=%d channel=%u frequency_hz=%.1f waveform_id=%u\n",
            tag, hh_rc_cmd_str(r->cmd), hh_rc_state_str(r->state),
            r->operational ? 1 : 0, r->channel, (double)r->frequency_hz, r->waveform_id);
        break;
    case HH_RC_CMD_STATS:
        n = snprintf(buf, cap,
            "%s %s frames_tx=%llu frames_rx=%llu tx_errors=%llu rx_errors=%llu "
            "requests_total=%llu requests_rejected=%llu\n",
            tag, hh_rc_cmd_str(r->cmd),
            (unsigned long long)r->frames_tx, (unsigned long long)r->frames_rx,
            (unsigned long long)r->tx_errors, (unsigned long long)r->rx_errors,
            (unsigned long long)r->requests_total, (unsigned long long)r->requests_rejected);
        break;
    default:
        n = snprintf(buf, cap, "%s %s state=%s\n", tag, hh_rc_cmd_str(r->cmd),
                     hh_rc_state_str(r->state));
        break;
    }
    return (n < 0 || (size_t)n >= cap) ? 0 : (size_t)n;
}
