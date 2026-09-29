/*
 * rh-agent wire contract — the radiod <-> OpenCPI helper protocol.
 *
 * ==========================================================================
 * WHY THERE IS A HELPER PROCESS AT ALL
 * ==========================================================================
 * This is not a design preference; it is what the installed framework allows.
 * The OpenCPI Application Control Interface is C++, and in this CDK its
 * runtime archives are NOT position-independent:
 *
 *   ld: libocpi_application.a(...Application.o): relocation R_X86_64_PC32
 *       against symbol '...' can not be used when making a shared object;
 *       recompile with -fPIC
 *
 * So the ACI links into an EXECUTABLE, statically, and nothing else. It cannot
 * be packaged as a shared library for radiod to dlopen, and linking it
 * directly into radiod would make radiod a C++-linked 16 MB binary that only
 * builds on a host carrying the CDK — breaking the build everywhere else.
 *
 * A separate executable is therefore the only available boundary, and it is
 * the right one anyway. It resolves, rather than works around, three questions
 * this project had recorded as open:
 *   U-11  C versus C++ at the OpenCPI boundary -> a process boundary, so
 *         radiod stays C11 under -Wstrict-prototypes and no ABI is mixed.
 *   U-12  threading model for PL ownership -> the ACI runs in its own
 *         process, so radiod's single-threaded poll loop is untouched.
 *   U-04  ACI error contract -> an ACI exception or abort kills the agent,
 *         not the control plane; radiod sees the pipe close and reports it.
 *
 * SINGLE-OWNER RULE: radiod remains the only owner of the PL. It spawns the
 * agent, is the only process holding its pipes, and kills it on exit. The
 * agent additionally holds RADHAWK's own cross-process run lock
 * (RH_LOCK_PATH) for the whole time a session is open, so exclusivity is
 * enforced by the OpenCPI-side library and not merely by convention.
 *
 * ==========================================================================
 * THE PROTOCOL
 * ==========================================================================
 * Line-oriented ASCII over a pipe pair: radiod writes one request line to the
 * agent's stdin and reads exactly one response line from its stdout. Same
 * shape as the client-facing rc protocol, deliberately, so there is one idea
 * of "a control message" in the system rather than two.
 *
 *   REQUEST                       RESPONSE
 *   hello                         ok hello api=<n> props=<count>
 *   props                         ok props n=<count> items=<csv>
 *   cfg <worker>.<prop> <value>   ok cfg
 *   set <worker>.<prop> <value>   ok set
 *   get <worker>.<prop>           ok get value=<text>
 *   start                         ok start dry=<0|1>
 *   status                        ok status n=<count> items=<csv>
 *   stop                          ok stop
 *   quit                          (none; the agent exits)
 *
 *   failure                       err <verb> rc=<int> [kind=<kind>] msg=<text to EOL>
 *
 * `items` for `props` is "<worker>.<prop>:<type>:<access>" per element; for
 * `status` it is "<worker>.<prop>:<value>". Both are comma-separated with no
 * spaces. `msg` is the only field that may contain spaces and is always last.
 *
 * <access> is one of the HH_PROPS_ACCESS_* strings defined in props.h, which is
 * their single authority: they travel verbatim from the agent's registry,
 * through radiod, to `opencpi list` output.
 *
 * `kind` classifies a failure so that radiod can report the right status code
 * WITHOUT interpreting the message text. An earlier revision inferred the kind
 * by searching `msg` for words like "range", and got it wrong the moment the
 * wording changed -- a rejected value read as an I/O error, which tells an
 * operator to go and look at the hardware instead of at what they typed. The
 * kind is now stated, not guessed. It is optional, and its absence means `io`.
 *
 * WHICH SIDE OWNS WHAT: the agent owns the property registry -- it is the side
 * that knows the RADHAWK ABI and the OpenCPI property names. radiod learns the
 * registry by asking (`props`, once, at startup) and thereafter only routes.
 * That is why adding a property needs a change in the agent alone.
 *
 * `cfg` stages a value into the next rh_start(); `set` applies one to a
 * running session through rh_set_params(). radiod chooses between them from
 * its own lifecycle state, which is why the agent needs no state machine of
 * its own beyond "session open" / "session closed".
 * ==========================================================================
 */
#ifndef HHSDR_RADIOD_OPENCPI_AGENT_PROTO_H
#define HHSDR_RADIOD_OPENCPI_AGENT_PROTO_H

/* Bumped on any incompatible change to the lines above. radiod compares the
 * agent's reported version against this and refuses to use a mismatched agent
 * rather than misinterpreting its replies. */
#define HH_RH_AGENT_PROTO_VERSION 1

/* One request or response line, including its newline.
 *
 * Larger than the client-facing HH_RC_MAX_LINE on purpose: the `props` reply
 * carries the WHOLE registry in one line -- 61 elements of
 * "<worker>.<prop>:<type>:<access>", 1724 bytes, measured 2026-09-29 --
 * whereas the client-facing
 * opencpi_list only ever returns one worker's share of it. This link is
 * internal to radiod and its own helper, so its framing is sized for what it
 * actually carries rather than for what a client needs. */
#define HH_RH_AGENT_MAX_LINE 4096

/* The `items` value within such a line. */
#define HH_RH_AGENT_MAX_ITEMS 3584

/* Request verbs. */
#define HH_RH_AGENT_HELLO  "hello"
#define HH_RH_AGENT_PROPS  "props"
#define HH_RH_AGENT_CFG    "cfg"
#define HH_RH_AGENT_SET    "set"
#define HH_RH_AGENT_GET    "get"
#define HH_RH_AGENT_START  "start"
#define HH_RH_AGENT_STATUS "status"
#define HH_RH_AGENT_STOP   "stop"
#define HH_RH_AGENT_QUIT   "quit"

/* Failure kinds on an `err` line. Each maps to exactly one hh_status_t in
 * radiod, and the vocabulary is radiod's rather than RADHAWK's on purpose: the
 * agent translates its library's codes into these, so radiod never has to know
 * what RH_EINVAL is. */
#define HH_RH_AGENT_KIND         "kind"
#define HH_RH_AGENT_KIND_INVAL   "inval"        /* -> HH_ERR_INVAL       */
#define HH_RH_AGENT_KIND_STATE   "state"        /* -> HH_ERR_STATE       */
#define HH_RH_AGENT_KIND_NOTFOUND "notfound"    /* -> HH_ERR_NOTFOUND    */
#define HH_RH_AGENT_KIND_UNSUP   "unsupported"  /* -> HH_ERR_UNSUPPORTED */
#define HH_RH_AGENT_KIND_IO      "io"           /* -> HH_ERR_IO, the default */

/* The value a writable property reads back as when nothing has set it yet.
 * Mirrors RADHAWK's RH_KEEP sentinel, so "nobody has chosen a value" stays
 * distinguishable from "the value happens to be 0". */
#define HH_RH_AGENT_KEEP   "keep"

/* Default location of the agent binary on the target. Overridable with the
 * radiod `opencpi_agent` configuration key, because a development host runs it
 * straight out of the OpenCPI project's build tree. */
#define HH_RH_AGENT_DEFAULT_PATH "/opt/txrx_worker_rt_ch2_dma/rh_agent"

#endif /* HHSDR_RADIOD_OPENCPI_AGENT_PROTO_H */
