# `librc/` — the radio-control client library

The C API every out-of-process client uses to reach `radiod`. `radioctl` uses
it, the end-to-end tests use it, and in the target architecture the routing
daemon and applications use it too.

**Target:** `librc` → `librc.a`
**Depends on:** `hhsdr_protocol` **only**
**Sources:** [`src/rc_client.c`](src/rc_client.c) · [`include/hhsdr/librc/rc_client.h`](include/hhsdr/librc/rc_client.h)

```
your application ──► librc ──► protocol ──► AF_UNIX socket ──► radiod
```

---

## The dependency rule

**`librc` must never link `hhsdr_radiod`.**

This is the reason the component exists separately. The client code originally
lived *inside* the daemon library, which meant anything wanting to talk to
`radiod` had to link the entire daemon — pulling the daemon's internals into
every caller's build.

Now the dependency runs one way: client → protocol. Both sides share the wire
format and nothing else.

That rule is enforced by the build, not just documented. `test_radiod_daemon`
links `librc` and forks the real `radiod` binary; if `librc` ever grew a
dependency on the daemon library, the layering violation would show up at link
time.

---

## API — three functions

```c
typedef struct { int fd; } hh_rc_client_t;

hh_status_t hh_rc_client_connect(hh_rc_client_t *c, const char *sock_path);
hh_status_t hh_rc_client_call   (hh_rc_client_t *c,
                                 const hh_rc_request_t *req,
                                 hh_rc_response_t *resp);
void        hh_rc_client_close  (hh_rc_client_t *c);
```

`hh_rc_client_t` is caller-allocated — put it on the stack. No `malloc`, no
hidden state, nothing to free beyond `close`.

### Minimal usage

```c
#include "hhsdr/librc/rc_client.h"

hh_rc_client_t c;
hh_rc_request_t  req = {0};
hh_rc_response_t resp;

if (hh_rc_client_connect(&c, HH_RC_DEFAULT_SOCK_PATH) != HH_OK)
    return 1;                                   /* daemon not reachable */

req.cmd = HH_RC_CMD_STATUS;
if (hh_rc_client_call(&c, &req, &resp) != HH_OK) {
    hh_rc_client_close(&c);
    return 1;                                   /* transport failure    */
}

if (!resp.ok) {
    fprintf(stderr, "rejected: %s\n", hh_status_str(resp.reason));
} else {
    printf("state=%s operational=%d channel=%u\n",
           hh_rc_state_str(resp.state), resp.operational, resp.channel);
}

hh_rc_client_close(&c);
```

**Always zero the request** (`= {0}`): unused fields must not carry stack
garbage. `configure` in particular rejects `node_id == 0`
(`HH_NODE_ID_INVALID`).

### Two failure modes — do not conflate them

```c
st = hh_rc_client_call(&c, &req, &resp);
```

| | Meaning |
|---|---|
| `st != HH_OK` | **Transport failure.** Could not reach the daemon, or the exchange broke. `resp` is meaningless. |
| `st == HH_OK && !resp.ok` | **The daemon answered and refused.** `resp.reason` says why (usually `HH_ERR_STATE`). This is a *successful* exchange. |
| `st == HH_OK && resp.ok` | Success; payload fields are populated per `resp.cmd`. |

Automation depends on this distinction — "the radio said no" is not "the radio
is gone". `radioctl` maps them to exit codes 4 and 3 respectively.

### Building a request

Only three verbs take parameters:

```c
req.cmd = HH_RC_CMD_CONFIGURE;   req.node_id = 42;  req.channel = 7;
req.cmd = HH_RC_CMD_SET_CHANNEL; req.channel = 11;
req.cmd = HH_RC_CMD_INJECT_FAULT; req.fault  = HH_RC_FAULT_HW_FAULT;
```

Everything else needs `req.cmd` alone. The full verb list, response payload
fields and error codes are in [`../protocol/README.md`](../protocol/README.md).

---

## Semantics and limits

**Synchronous and blocking.** `hh_rc_client_call` writes one request and blocks
until a full response line arrives. One request in flight at a time; the
protocol has no transaction IDs, so replies are matched by ordering alone.

**No timeout.** ⚠️ The socket is created without `SO_RCVTIMEO`, so a daemon that
accepts a connection and then never replies will block the caller
**indefinitely**. Worth knowing before putting this on a critical path. If you
need one today, set it yourself on `c.fd`:

```c
struct timeval tv = { .tv_sec = 5 };
setsockopt(c.fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
```

**No reconnect.** A dropped connection is not re-established. Detect it via the
`HH_ERR_IO` return and reconnect explicitly.

**No async events.** `radiod` only ever replies to requests — it never pushes.
State changes and faults must be polled with `status`. Asynchronous delivery
needs a wire framing for unsolicited messages, which the protocol does not have
(see [`../unknown.md`](../unknown.md), U-01).

**Not thread-safe.** One `hh_rc_client_t` per thread; do not share a connection
across threads.

**Connection lifetime is your choice.** The daemon holds up to 16 concurrent
clients and (by default) never times them out, so a long-lived connection is
fine. `radioctl` deliberately connects per invocation instead, because a CLI
process is short-lived anyway.

---

## About the name `rc_*`

The architecture drawing shows applications calling an **`rc_* API`**. Those
exact signatures are **not specified anywhere** (U-02), so this library keeps
its existing `hh_rc_client_*` names rather than inventing `rc_*` ones to match a
picture.

If a specified `rc_*` API does arrive, it can be layered over these three
functions without `radiod` changing at all.

---

## Linking

```cmake
target_link_libraries(your_target PRIVATE librc)
```

That transitively provides `hhsdr_protocol` and its include directories. The
artifact is `librc.a` (via `OUTPUT_NAME rc`), not `liblibrc.a`.

---

## Testing

`librc` has no standalone test binary. It is covered end-to-end by
`test_radiod_daemon` and `test_radioctl`, both of which drive the **real**
`radiod` binary over a **real** socket through this library:

```bash
ctest --test-dir build -R 'test_radiod_daemon|test_radioctl' --output-on-failure
```

Exercising it against the actual daemon is a stronger check than mocking the
socket would be — it covers the framing, the transport and the daemon's replies
together.
