# `protocol/` — the radio-control wire format

The encode/decode layer shared by `radiod` (server) and `librc` (client).
Neither owns it, because both need it and neither should depend on the other.

```
radioctl ──► librc ──┐
                     ├──► protocol ──► hhsdr_core_base
radiod   ────────────┘
```

**Target:** `hhsdr_protocol` (`libhhsdr_protocol.a`)
**Sources:** [`src/rc.c`](src/rc.c) · [`include/hhsdr/protocol/rc.h`](include/hhsdr/protocol/rc.h)
**Tests:** [`tests/test_rc_protocol.c`](tests/test_rc_protocol.c) → `ctest -R test_rc_protocol`

---

## ⚠️ This is NOT ICD-2

The target architecture specifies **"versioned TLV over UNIX socket" (ICD-2)**.
What is implemented here is a **line-oriented ASCII `key=value` protocol**, and
it differs from ICD-2 in every structural respect:

| ICD-2 calls for | This implements |
|---|---|
| TLV (type/length/value) framing | newline-delimited ASCII text |
| a version field + negotiation | **no version field at all** |
| numeric command IDs | ASCII verb names (`status`, `set_channel`, …) |
| transaction/request IDs | none — strictly one request in flight |
| asynchronous events | none — exactly one response per request |

**No ICD-2 specification exists** (see [`../unknown.md`](../unknown.md), U-01), so
inventing a TLV layout would mean inventing the contract. The existing ASCII
protocol was a deliberate choice: it reuses the project's `key = value`
configuration and telemetry conventions instead of adding a serialization
layer.

When ICD-2 is specified, its codec is added **in this directory, alongside**
`rc.c`. Nothing in `radiod` or `librc` changes: they call
`hh_rc_request_parse`/`hh_rc_response_format`, not a socket.

---

## Wire format

One request per line, one response per line, `\n`-terminated. Max **512 bytes**
per line including the newline (`HH_RC_MAX_LINE`).

```
REQUEST     <verb>[ key=value]*\n
RESPONSE    ok <verb>[ key=value]*\n
            err <verb> reason=<int>\n
```

Live example (captured from a running daemon):

```
→ init
← ok init state=initialized

→ configure node_id=42 channel=7
← ok configure state=configured

→ start
← ok start state=running

→ status
← ok status state=running operational=1 channel=0 frequency_hz=0.0 waveform_id=0

→ set_channel channel=11
← ok set_channel state=running operational=1 channel=11 frequency_hz=0.0 waveform_id=0

→ start
← err start reason=5            (5 = HH_ERR_STATE: already running)
```

### Verbs (10) and their parameters

| Verb | Parameters | Response payload |
|---|---|---|
| `init` | — | `state` |
| `configure` | `node_id` (required), `channel` | `state` |
| `start` | — | `state` |
| `stop` | — | `state` |
| `shutdown` | — | `state` |
| `status` | — | full status block |
| `stats` | — | counter block |
| `set_channel` | `channel` (required) | full status block |
| `inject_fault` | `kind` | full status block |
| `clear_fault` | — | full status block |

Only three verbs take parameters. `kind` ∈ `none`, `tx_failure`, `rx_silence`,
`hw_fault`, `backend_io`.

### Response payloads

`status` and `set_channel`/`inject_fault`/`clear_fault` return:
`state`, `operational` (0/1), `channel`, `frequency_hz`, `waveform_id`.

`stats` returns: `frames_tx`, `frames_rx`, `tx_errors`, `rx_errors`,
`requests_total`, `requests_rejected`.

Every other successful response returns `state` alone.

### Error codes

`reason=` carries `hh_status_t` as a **signed decimal**
([`core/types.h`](../include/hhsdr/core/types.h)):

| Value | Name | Typical cause |
|---|---|---|
| 0 | `HH_OK` | — |
| 1 | `HH_ERR_INVAL` | malformed line, unknown key, bad `node_id` |
| 5 | `HH_ERR_STATE` | verb not legal in the current state |
| 6 | `HH_ERR_UNSUPPORTED` | backend lacks the operation |
| 7 | `HH_ERR_NOT_IMPLEMENTED` | no hardware backend |
| 8 | `HH_ERR_IO` | backend I/O failure |

---

## API

```c
hh_status_t hh_rc_request_parse (const char *line, hh_rc_request_t  *out);
size_t      hh_rc_request_format(const hh_rc_request_t  *r, char *buf, size_t cap);
hh_status_t hh_rc_response_parse (const char *line, hh_rc_response_t *out);
size_t      hh_rc_response_format(const hh_rc_response_t *r, char *buf, size_t cap);
```

The `format` functions return **bytes written, or 0 if the output did not fit** —
0 is the failure signal, not a status code. Both `parse` functions tolerate a
missing trailing newline and strip `\r\n`.

Enum helpers: `hh_rc_cmd_str`/`hh_rc_cmd_parse`, `hh_rc_state_str`,
`hh_rc_fault_str`/`hh_rc_fault_parse`.

### Key types

```c
typedef struct {                    typedef struct {
    hh_rc_cmd_t   cmd;                  bool          ok;
    hh_node_id_t  node_id;              hh_rc_cmd_t   cmd;
    uint32_t      channel;              hh_status_t   reason;   /* when !ok */
    hh_rc_fault_t fault;                hh_rc_state_t state;
} hh_rc_request_t;                      bool          operational;
                                        uint32_t      channel;
                                        float         frequency_hz;
                                        uint32_t      waveform_id;
                                        uint64_t      frames_tx, frames_rx;
                                        uint64_t      tx_errors, rx_errors;
                                        uint64_t      requests_total;
                                        uint64_t      requests_rejected;
                                    } hh_rc_response_t;
```

`hh_rc_response_t` is a **flat union-by-convention**: which fields are populated
depends on `cmd`. `status` fills the radio block, `stats` fills the counters,
everything else fills only `state`. `resp.state` is always set, even on
rejection — so a client always learns the current state.

---

## Compatibility behaviour — read before extending

The protocol is **strict**, with one asymmetry worth knowing:

- **Unknown request key → whole request rejected** (`HH_ERR_INVAL`).
- **Unknown response key → whole response rejected** (`HH_ERR_INVAL`).
- **Unknown *state string* → silently becomes `created`** (value 0).

That last one is a genuine forward-compatibility hazard: a client built against
an older build, talking to a daemon that adds a state, will silently misreport
it as `created` rather than erroring. It is called out here because a versioned
protocol must fix it, and because it is not obvious from reading the parser.

Adding a verb or key **breaks older peers in both directions**. There is no
version negotiation to soften that — which is precisely why ICD-2 asks for one.

---

## Constants

| Constant | Value | Meaning |
|---|---|---|
| `HH_RC_MAX_LINE` | 512 | max bytes per line, including `\n` |
| `HH_RC_DEFAULT_SOCK_PATH` | `/tmp/hh-radiod.sock` | default socket (see U-10) |

---

## Testing

```bash
ctest --test-dir build -R test_rc_protocol --output-on-failure
```

Covers round-tripping every verb, malformed input, unknown keys, boundary
lengths, and error formatting.
