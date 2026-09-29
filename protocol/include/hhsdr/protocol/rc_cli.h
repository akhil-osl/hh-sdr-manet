/*
 * rc_cli — the human-facing command grammar, shared by every front end.
 *
 * ==========================================================================
 * WHY THIS IS IN THE PROTOCOL COMPONENT AND NOT IN radioctl
 * ==========================================================================
 * There are now two places a person types a command: radioctl's argv, and
 * radiod's interactive console (radiod -i). Both must accept exactly the same
 * words, or "the radiod command syntax" stops being one thing. Putting the
 * grammar beside the codec it feeds is what guarantees that: a verb added to
 * rc.h is spelled the same way wherever it is typed, because only one
 * translator exists.
 *
 * It is a separate translation unit from rc.c on purpose. rc.c is the wire
 * codec — no opinions about presentation. This file is presentation: the
 * spellings a person types and the layout they read. A machine-to-machine
 * client links rc.c and never touches this.
 *
 * DEPENDENCY DIRECTION is unchanged: this depends on the codec, and the codec
 * depends on nothing. librc, radiod and radioctl all sit above it.
 * ==========================================================================
 */
#ifndef HHSDR_PROTOCOL_RC_CLI_H
#define HHSDR_PROTOCOL_RC_CLI_H

#include "hhsdr/protocol/rc.h"

/* The most tokens any command takes: "opencpi set <worker> <prop> <value>". */
#define HH_RC_CLI_MAX_ARGS 8

/*
 * Translate one already-tokenised command into a request.
 *
 * argv[0] is the verb. Both spellings of every multi-word verb are accepted:
 * the hyphenated form a person types (set-channel) and the underscored wire
 * form a script may have been written against (set_channel). The `opencpi`
 * verbs take a sub-verb, which is how the requested
 *   opencpi list|get|set|status|start|stop
 * shape is reached without adding six more verbs to the wire protocol.
 *
 * Returns HH_OK, or HH_ERR_NOTFOUND when argv[0] is not a command at all
 * (so a caller can distinguish "unknown command" from "bad arguments"), or
 * HH_ERR_INVAL for a known command used wrongly. On either error `err`
 * (when non-NULL, of size err_len) carries a sentence naming the problem.
 */
hh_status_t hh_rc_cli_parse(int argc, char *const *argv, hh_rc_request_t *out,
                            char *err, size_t err_len);

/*
 * Tokenise one whitespace-separated line and parse it. `line` is modified in
 * place (tokens are NUL-terminated within it), so it must be writable. An
 * empty or all-whitespace line returns HH_ERR_AGAIN and writes nothing —
 * that is a blank prompt entry, not an error to report.
 */
hh_status_t hh_rc_cli_parse_line(char *line, hh_rc_request_t *out,
                                 char *err, size_t err_len);

/*
 * Render a response the way a person reads it: one `key=value` per line for
 * the verbs that carry a payload, and the resulting lifecycle state for those
 * that do not. Comma-separated item lists are expanded one item per line,
 * because a 600-character single line is not readable output.
 *
 * Writes at most cap-1 bytes plus a NUL. Never fails.
 */
void hh_rc_cli_render(const hh_rc_response_t *r, char *buf, size_t cap);

/* The usage text for the command set, as one block of lines. Shared so that
 * `radioctl -h` and the console's `help` cannot drift apart. */
const char *hh_rc_cli_usage(void);

#endif /* HHSDR_PROTOCOL_RC_CLI_H */
