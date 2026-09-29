/*
 * The shared CLI grammar and the new worker-property verbs, at the codec level.
 *
 * Two things are checked here that nothing else can check cheaply:
 *   - a command typed as a person types it produces the request the protocol
 *     expects, including the `opencpi start <worker>` shorthand;
 *   - every new response payload survives format -> parse unchanged, including
 *     the `text` field, which is the only one allowed to contain spaces and is
 *     therefore the one the space-delimited tokeniser could break.
 */
#include "hhsdr/protocol/rc_cli.h"
#include "hh_test.h"
#include <string.h>

static hh_status_t cli(const char *a0, const char *a1, const char *a2,
                       const char *a3, const char *a4, hh_rc_request_t *out)
{
    char *argv[5];
    int argc = 0;
    char b0[64], b1[64], b2[64], b3[64], b4[64];
    char *bufs[5] = { b0, b1, b2, b3, b4 };
    const char *src[5] = { a0, a1, a2, a3, a4 };

    for (int i = 0; i < 5; i++) {
        if (src[i] == NULL) break;
        snprintf(bufs[i], 64, "%s", src[i]);
        argv[argc++] = bufs[i];
    }
    return hh_rc_cli_parse(argc, argv, out, NULL, 0);
}

static void test_existing_commands_unchanged(void)
{
    hh_rc_request_t r;

    HH_ASSERT_OK(cli("status", NULL, NULL, NULL, NULL, &r));
    HH_ASSERT_EQ_INT(r.cmd, HH_RC_CMD_STATUS);

    HH_ASSERT_OK(cli("configure", "7", "3", NULL, NULL, &r));
    HH_ASSERT_EQ_INT(r.cmd, HH_RC_CMD_CONFIGURE);
    HH_ASSERT_EQ_INT(r.node_id, 7);
    HH_ASSERT_EQ_INT(r.channel, 3);

    /* Both spellings of a multi-word verb, as radioctl always accepted. */
    HH_ASSERT_OK(cli("set-channel", "9", NULL, NULL, NULL, &r));
    HH_ASSERT_EQ_INT(r.channel, 9);
    HH_ASSERT_OK(cli("set_channel", "9", NULL, NULL, NULL, &r));
    HH_ASSERT_EQ_INT(r.channel, 9);

    HH_ASSERT_OK(cli("inject-fault", "hw_fault", NULL, NULL, NULL, &r));
    HH_ASSERT_EQ_INT(r.fault, HH_RC_FAULT_HW_FAULT);

    /* An unknown verb is reported distinctly from a misused known one, so a
     * caller can tell "no such command" from "wrong arguments". */
    HH_ASSERT_ERR(cli("frobnicate", NULL, NULL, NULL, NULL, &r), HH_ERR_NOTFOUND);
    HH_ASSERT_ERR(cli("set-channel", NULL, NULL, NULL, NULL, &r), HH_ERR_INVAL);
}

static void test_opencpi_grammar(void)
{
    hh_rc_request_t r;

    HH_ASSERT_OK(cli("opencpi", "list", NULL, NULL, NULL, &r));
    HH_ASSERT_EQ_INT(r.cmd, HH_RC_CMD_OPENCPI_LIST);
    HH_ASSERT_EQ_STR(r.worker, "");

    HH_ASSERT_OK(cli("opencpi", "list", "qpsk_tx", NULL, NULL, &r));
    HH_ASSERT_EQ_STR(r.worker, "qpsk_tx");

    HH_ASSERT_OK(cli("opencpi", "get", "qpsk_rx", "bit_count", NULL, &r));
    HH_ASSERT_EQ_INT(r.cmd, HH_RC_CMD_OPENCPI_GET);
    HH_ASSERT_EQ_STR(r.worker, "qpsk_rx");
    HH_ASSERT_EQ_STR(r.prop, "bit_count");

    HH_ASSERT_OK(cli("opencpi", "set", "qpsk_tx", "rrc_backoff", "16384", &r));
    HH_ASSERT_EQ_INT(r.cmd, HH_RC_CMD_OPENCPI_SET);
    HH_ASSERT_EQ_STR(r.value, "16384");

    HH_ASSERT_OK(cli("opencpi", "status", NULL, NULL, NULL, &r));
    HH_ASSERT_EQ_INT(r.cmd, HH_RC_CMD_OPENCPI_STATUS);

    /* `opencpi start/stop <worker>` is sugar for the worker's own `enable`
     * property. Checking that it lowers to exactly that is what stops it from
     * quietly becoming a separate, invented operation. */
    HH_ASSERT_OK(cli("opencpi", "start", "qpsk_tx", NULL, NULL, &r));
    HH_ASSERT_EQ_INT(r.cmd, HH_RC_CMD_OPENCPI_SET);
    HH_ASSERT_EQ_STR(r.prop, "enable");
    HH_ASSERT_EQ_STR(r.value, "1");

    HH_ASSERT_OK(cli("opencpi", "stop", "qpsk_rx", NULL, NULL, &r));
    HH_ASSERT_EQ_STR(r.prop, "enable");
    HH_ASSERT_EQ_STR(r.value, "0");

    /* Arity and sub-verb errors. */
    HH_ASSERT_ERR(cli("opencpi", NULL, NULL, NULL, NULL, &r), HH_ERR_INVAL);
    HH_ASSERT_ERR(cli("opencpi", "wibble", NULL, NULL, NULL, &r), HH_ERR_INVAL);
    HH_ASSERT_ERR(cli("opencpi", "get", "qpsk_tx", NULL, NULL, &r), HH_ERR_INVAL);
    HH_ASSERT_ERR(cli("opencpi", "set", "qpsk_tx", "rrc_backoff", NULL, &r), HH_ERR_INVAL);
}

static void test_value_with_space_refused(void)
{
    hh_rc_request_t r;
    char *argv[5];
    char a[] = "opencpi", b[] = "set", c[] = "app", d[] = "xml", e[] = "two words";
    char err[128];

    argv[0] = a; argv[1] = b; argv[2] = c; argv[3] = d; argv[4] = e;

    /* The wire framing is space-delimited. A value containing a space must be
     * refused at the CLI rather than truncated into a different value on the
     * wire -- silently setting app.xml to "two" would be far worse than an
     * error message. */
    HH_ASSERT_ERR(hh_rc_cli_parse(5, argv, &r, err, sizeof err), HH_ERR_INVAL);
    HH_ASSERT(strstr(err, "whitespace") != NULL);
}

static void test_parse_line(void)
{
    hh_rc_request_t r;
    char line1[] = "  opencpi   set  qpsk_tx rrc_backoff 16384  \n";
    char line2[] = "   \n";

    HH_ASSERT_OK(hh_rc_cli_parse_line(line1, &r, NULL, 0));
    HH_ASSERT_EQ_INT(r.cmd, HH_RC_CMD_OPENCPI_SET);
    HH_ASSERT_EQ_STR(r.worker, "qpsk_tx");
    HH_ASSERT_EQ_STR(r.value, "16384");

    /* A blank prompt entry is not an error to report back to the operator. */
    HH_ASSERT_ERR(hh_rc_cli_parse_line(line2, &r, NULL, 0), HH_ERR_AGAIN);
}

static void test_request_roundtrip(void)
{
    hh_rc_request_t in, out;
    char buf[HH_RC_MAX_LINE];

    memset(&in, 0, sizeof in);
    in.cmd = HH_RC_CMD_OPENCPI_SET;
    snprintf(in.worker, sizeof in.worker, "qpsk_tx");
    snprintf(in.prop, sizeof in.prop, "rrc_backoff");
    snprintf(in.value, sizeof in.value, "16384");
    HH_ASSERT(hh_rc_request_format(&in, buf, sizeof buf) > 0);
    HH_ASSERT_OK(hh_rc_request_parse(buf, &out));
    HH_ASSERT_EQ_INT(out.cmd, HH_RC_CMD_OPENCPI_SET);
    HH_ASSERT_EQ_STR(out.worker, "qpsk_tx");
    HH_ASSERT_EQ_STR(out.prop, "rrc_backoff");
    HH_ASSERT_EQ_STR(out.value, "16384");

    /* opencpi_list omits the worker key entirely when none was given, so the
     * daemon can tell "list the workers" from "list a worker called ''". */
    memset(&in, 0, sizeof in);
    in.cmd = HH_RC_CMD_OPENCPI_LIST;
    HH_ASSERT(hh_rc_request_format(&in, buf, sizeof buf) > 0);
    HH_ASSERT(strstr(buf, "worker=") == NULL);
    HH_ASSERT_OK(hh_rc_request_parse(buf, &out));
    HH_ASSERT_EQ_STR(out.worker, "");
}

static void test_response_roundtrip(void)
{
    hh_rc_response_t in, out;
    char buf[HH_RC_MAX_LINE];

    /* A list payload, comma-separated with three fields per element. */
    memset(&in, 0, sizeof in);
    in.ok = true;
    in.cmd = HH_RC_CMD_OPENCPI_LIST;
    snprintf(in.worker, sizeof in.worker, "qpsk_tx");
    snprintf(in.items, sizeof in.items, "enable:bool:rw,rrc_backoff:ushort:rw");
    in.count = 2;
    HH_ASSERT(hh_rc_response_format(&in, buf, sizeof buf) > 0);
    HH_ASSERT_OK(hh_rc_response_parse(buf, &out));
    HH_ASSERT(out.ok);
    HH_ASSERT_EQ_INT(out.count, 2);
    HH_ASSERT_EQ_STR(out.items, "enable:bool:rw,rrc_backoff:ushort:rw");
    HH_ASSERT_EQ_STR(out.worker, "qpsk_tx");

    /* A get payload. */
    memset(&in, 0, sizeof in);
    in.ok = true;
    in.cmd = HH_RC_CMD_OPENCPI_GET;
    snprintf(in.worker, sizeof in.worker, "qpsk_rx");
    snprintf(in.prop, sizeof in.prop, "bit_count");
    snprintf(in.value, sizeof in.value, "141184");
    snprintf(in.ptype, sizeof in.ptype, "ulonglong");
    snprintf(in.access, sizeof in.access, "ro");
    HH_ASSERT(hh_rc_response_format(&in, buf, sizeof buf) > 0);
    HH_ASSERT_OK(hh_rc_response_parse(buf, &out));
    HH_ASSERT_EQ_STR(out.value, "141184");
    HH_ASSERT_EQ_STR(out.ptype, "ulonglong");
    HH_ASSERT_EQ_STR(out.access, "ro");
}

static void test_text_field_carries_spaces(void)
{
    hh_rc_response_t in, out;
    char buf[HH_RC_MAX_LINE];
    const char *why = "sym_mode can only be set while the application is stopped";

    /* The whole point of formatting `text` last and parsing it greedily. A
     * refusal is only useful if its reason survives the wire intact. */
    memset(&in, 0, sizeof in);
    in.ok = false;
    in.cmd = HH_RC_CMD_OPENCPI_SET;
    in.reason = HH_ERR_STATE;
    snprintf(in.text, sizeof in.text, "%s", why);
    HH_ASSERT(hh_rc_response_format(&in, buf, sizeof buf) > 0);
    HH_ASSERT_OK(hh_rc_response_parse(buf, &out));
    HH_ASSERT(!out.ok);
    HH_ASSERT_EQ_INT(out.reason, HH_ERR_STATE);
    HH_ASSERT_EQ_STR(out.text, why);

    /* A failure with no detail must stay byte-identical to the pre-existing
     * form, so an older client parsing it is unaffected. */
    memset(&in, 0, sizeof in);
    in.ok = false;
    in.cmd = HH_RC_CMD_START;
    in.reason = HH_ERR_IO;
    HH_ASSERT(hh_rc_response_format(&in, buf, sizeof buf) > 0);
    HH_ASSERT_EQ_STR(buf, "err start reason=8\n");
}

static void test_render_expands_items(void)
{
    hh_rc_response_t r;
    char out[1024];

    memset(&r, 0, sizeof r);
    r.ok = true;
    r.cmd = HH_RC_CMD_OPENCPI_STATUS;
    r.count = 2;
    snprintf(r.items, sizeof r.items, "qpsk_rx.in_sync:1,qpsk_rx.err_count:0");
    hh_rc_cli_render(&r, out, sizeof out);
    HH_ASSERT(strstr(out, "count=2") != NULL);
    HH_ASSERT(strstr(out, "qpsk_rx.in_sync=1") != NULL);
    HH_ASSERT(strstr(out, "qpsk_rx.err_count=0") != NULL);

    /* A three-field element is a published property, and reads in columns. */
    memset(&r, 0, sizeof r);
    r.ok = true;
    r.cmd = HH_RC_CMD_OPENCPI_LIST;
    r.count = 1;
    snprintf(r.items, sizeof r.items, "rrc_backoff:ushort:rw");
    hh_rc_cli_render(&r, out, sizeof out);
    HH_ASSERT(strstr(out, "rrc_backoff") != NULL);
    HH_ASSERT(strstr(out, "ushort") != NULL);
    HH_ASSERT(strstr(out, "rw") != NULL);
}

static void test_every_verb_has_a_name(void)
{
    /* Mirrors the same check in test_rc_protocol.c: a verb added to the enum
     * without a name would format as "unknown" and be unparseable. */
    for (int i = 0; i < HH_RC_CMD__MAX; i++) {
        hh_rc_cmd_t back;
        const char *name = hh_rc_cmd_str((hh_rc_cmd_t)i);
        HH_ASSERT_MSG(strcmp(name, "unknown") != 0, "verb %d has no name", i);
        HH_ASSERT_MSG(hh_rc_cmd_parse(name, &back), "verb '%s' does not parse back", name);
        HH_ASSERT_EQ_INT(back, i);
    }
}

HH_TEST_MAIN_BEGIN("rc_cli")
    HH_RUN(test_existing_commands_unchanged);
    HH_RUN(test_opencpi_grammar);
    HH_RUN(test_value_with_space_refused);
    HH_RUN(test_parse_line);
    HH_RUN(test_request_roundtrip);
    HH_RUN(test_response_roundtrip);
    HH_RUN(test_text_field_carries_spaces);
    HH_RUN(test_render_expands_items);
    HH_RUN(test_every_verb_has_a_name);
HH_TEST_MAIN_END()
