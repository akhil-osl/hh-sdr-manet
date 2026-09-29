/*
 * stub_agent — a test-only implementation of the rh_agent wire contract.
 *
 * TEST INFRASTRUCTURE, never linked into or shipped with anything. It exists
 * so the OpenCPI backend, radiod's routing, the control protocol and radioctl
 * can all be exercised end to end on a machine with no OpenCPI installation
 * and no FPGA — which is every developer machine and every CI runner.
 *
 * It is the same arrangement tests/sim/mock_radio.c already uses for the MANET
 * stack: a deterministic stand-in at a published boundary. The boundary here is
 * hhsdr/radiod/opencpi_agent_proto.h, and this program implements that contract
 * and nothing else. It contains no OpenCPI code, no RADHAWK code and no
 * pretence of either — the real agent (applications/rh_agent/rh_agent.cc in the
 * OpenCPI project) is what talks to hardware.
 *
 * Its registry is deliberately NOT the real one. Using invented worker names
 * (w1, w2) is what proves radiod carries no knowledge of qpsk_tx or qpsk_rx: if
 * anything in radiod depended on a real property name, these tests would fail.
 *
 * Failure modes, selected by HH_STUB_AGENT_MODE in the environment, because
 * radiod execs the agent with no arguments:
 *   (unset)     behave correctly
 *   badversion  report a protocol version radiod cannot speak
 *   die         answer `hello`, then exit — the crashed-agent path
 *   silent      never answer anything — the timeout path
 */
#include "hhsdr/radiod/opencpi_agent_proto.h"
#include "hhsdr/radiod/props.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define KEEP_SENTINEL (-1)

typedef struct {
    const char *worker;
    const char *prop;
    const char *type;
    const char *access;
    long        value;      /* KEEP_SENTINEL until written */
} stub_prop_t;

static stub_prop_t g_props[] = {
    { "w1", "live",      "ulong",     HH_PROPS_ACCESS_RW,       KEEP_SENTINEL },
    { "w1", "startonly", "ulong",     HH_PROPS_ACCESS_RW_START, KEEP_SENTINEL },
    { "w1", "counter",   "ulonglong", HH_PROPS_ACCESS_RO,       0 },
    { "w2", "live",      "bool",      HH_PROPS_ACCESS_RW,       KEEP_SENTINEL }
};
static const size_t g_nprops = sizeof g_props / sizeof g_props[0];

static int g_running;

static void ok(const char *verb, const char *extra)
{
    if (extra != NULL && extra[0] != '\0') printf("ok %s %s\n", verb, extra);
    else                                   printf("ok %s\n", verb);
    fflush(stdout);
}

/* The stub states a failure kind exactly as the real agent does, so radiod's
 * kind -> hh_status_t mapping is what the tests exercise. */
static void fail(const char *verb, const char *kind, const char *msg)
{
    printf("err %s rc=-1 %s=%s msg=%s\n", verb, HH_RH_AGENT_KIND, kind, msg);
    fflush(stdout);
}

static stub_prop_t *find(const char *key)
{
    const char *dot = strchr(key, '.');
    size_t wlen;
    if (dot == NULL) return NULL;
    wlen = (size_t)(dot - key);
    for (size_t i = 0; i < g_nprops; i++) {
        if (strlen(g_props[i].worker) == wlen &&
            strncmp(g_props[i].worker, key, wlen) == 0 &&
            strcmp(g_props[i].prop, dot + 1) == 0) return &g_props[i];
    }
    return NULL;
}

static void do_props(void)
{
    char line[HH_RH_AGENT_MAX_ITEMS];
    size_t used = 0;
    line[0] = '\0';
    for (size_t i = 0; i < g_nprops; i++) {
        int n = snprintf(line + used, sizeof line - used, "%s%s.%s:%s:%s",
                         (i > 0u) ? "," : "", g_props[i].worker, g_props[i].prop,
                         g_props[i].type, g_props[i].access);
        if (n < 0) return;
        used += (size_t)n;
    }
    printf("ok %s n=%u items=%s\n", HH_RH_AGENT_PROPS, (unsigned)g_nprops, line);
    fflush(stdout);
}

static void do_assign(const char *verb, const char *key, const char *value)
{
    stub_prop_t *p = find(key);
    char *end = NULL;
    long v;

    if (p == NULL) { fail(verb, HH_RH_AGENT_KIND_NOTFOUND, "no such property"); return; }
    if (strcmp(p->access, HH_PROPS_ACCESS_RO) == 0) { fail(verb, HH_RH_AGENT_KIND_UNSUP, "property is read-only"); return; }

    v = strtol(value, &end, 0);
    if (end == value || *end != '\0') { fail(verb, HH_RH_AGENT_KIND_INVAL, "value is not an integer"); return; }

    /* The agent-side half of the access rule. radiod refuses this too, from the
     * access class published above; both sides check so that neither is the
     * only thing standing between a bad write and the hardware. */
    if (strcmp(verb, HH_RH_AGENT_SET) == 0) {
        if (strcmp(p->access, HH_PROPS_ACCESS_RW_START) == 0) {
            fail(verb, HH_RH_AGENT_KIND_STATE, "property can only be set while stopped");
            return;
        }
        if (!g_running) { fail(verb, HH_RH_AGENT_KIND_STATE, "no session is running"); return; }
    }
    p->value = v;
    ok(verb, "");
}

static void do_get(const char *key)
{
    stub_prop_t *p = find(key);
    char extra[64];

    if (p == NULL) { fail(HH_RH_AGENT_GET, HH_RH_AGENT_KIND_NOTFOUND, "no such property"); return; }
    if (strcmp(p->access, HH_PROPS_ACCESS_RO) == 0 && !g_running) {
        fail(HH_RH_AGENT_GET, HH_RH_AGENT_KIND_STATE, "no session is running");
        return;
    }
    if (p->value == KEEP_SENTINEL) snprintf(extra, sizeof extra, "value=%s", HH_RH_AGENT_KEEP);
    else                          snprintf(extra, sizeof extra, "value=%ld", p->value);
    ok(HH_RH_AGENT_GET, extra);
}

static void do_status(void)
{
    char line[HH_RH_AGENT_MAX_ITEMS];
    size_t used = 0;
    unsigned n = 0;
    line[0] = '\0';
    for (size_t i = 0; i < g_nprops; i++) {
        int w;
        if (strcmp(g_props[i].access, HH_PROPS_ACCESS_RO) != 0) continue;
        w = snprintf(line + used, sizeof line - used, "%s%s.%s:%ld",
                     (n > 0u) ? "," : "", g_props[i].worker, g_props[i].prop,
                     g_props[i].value);
        if (w < 0) return;
        used += (size_t)w;
        n++;
    }
    if (!g_running) { fail(HH_RH_AGENT_STATUS, HH_RH_AGENT_KIND_STATE, "no session is running"); return; }
    printf("ok %s n=%u items=%s\n", HH_RH_AGENT_STATUS, n, line);
    fflush(stdout);
}

int main(void)
{
    const char *mode = getenv("HH_STUB_AGENT_MODE");
    char line[HH_RH_AGENT_MAX_LINE];
    int die_after_hello = (mode != NULL && strcmp(mode, "die") == 0);
    int silent          = (mode != NULL && strcmp(mode, "silent") == 0);
    int bad_version     = (mode != NULL && strcmp(mode, "badversion") == 0);

    while (fgets(line, (int)sizeof line, stdin) != NULL) {
        char *w[3] = { NULL, NULL, NULL };
        int n = 0;
        char *p = line;
        size_t len = strlen(line);

        while (len > 0u && (line[len - 1u] == '\n' || line[len - 1u] == '\r')) line[--len] = '\0';
        while (*p != '\0' && n < 3) {
            while (*p == ' ') p++;
            if (*p == '\0') break;
            w[n++] = p;
            while (*p != '\0' && *p != ' ') p++;
            if (*p != '\0') *p++ = '\0';
        }
        if (n == 0) continue;
        if (silent) continue;                       /* answer nothing, ever */

        if (!strcmp(w[0], HH_RH_AGENT_QUIT)) break;

        if (!strcmp(w[0], HH_RH_AGENT_HELLO)) {
            char extra[64];
            snprintf(extra, sizeof extra, "api=%d props=%u",
                     bad_version ? HH_RH_AGENT_PROTO_VERSION + 99
                                 : HH_RH_AGENT_PROTO_VERSION,
                     (unsigned)g_nprops);
            ok(HH_RH_AGENT_HELLO, extra);
            if (die_after_hello) return 3;
            continue;
        }
        if (!strcmp(w[0], HH_RH_AGENT_PROPS))  { do_props(); continue; }
        if (!strcmp(w[0], HH_RH_AGENT_START))  { g_running = 1; g_props[2].value = 4242;
                                                 ok(HH_RH_AGENT_START, "dry=1"); continue; }
        if (!strcmp(w[0], HH_RH_AGENT_STOP))   { g_running = 0; ok(HH_RH_AGENT_STOP, ""); continue; }
        if (!strcmp(w[0], HH_RH_AGENT_STATUS)) { do_status(); continue; }
        if (!strcmp(w[0], HH_RH_AGENT_GET)) {
            if (n < 2) { fail(HH_RH_AGENT_GET, HH_RH_AGENT_KIND_INVAL, "usage: get <worker>.<property>"); continue; }
            do_get(w[1]);
            continue;
        }
        if (!strcmp(w[0], HH_RH_AGENT_CFG) || !strcmp(w[0], HH_RH_AGENT_SET)) {
            if (n < 3) { fail(w[0], HH_RH_AGENT_KIND_INVAL, "usage: cfg|set <worker>.<property> <value>"); continue; }
            do_assign(w[0], w[1], w[2]);
            continue;
        }
        fail(w[0], HH_RH_AGENT_KIND_INVAL, "unknown verb");
    }
    return 0;
}
