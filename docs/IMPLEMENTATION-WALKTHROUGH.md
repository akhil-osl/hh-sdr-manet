# Implementation walkthrough: node bring-up, discovery, and the data plane

This traces `hh_node_t` from construction through the first beacon, the first
installed route, and the first forwarded packet, function by function, with
file paths and line numbers. Everything here is production code — nothing in
this document is simulator-only.

Reading order matches `README.md` §4: `radio.h` → `node.h` → `route_table.h`
→ `hh_manet_sim.c`. This document fills in the function bodies between those
files.

---

## 1. Bring-up: how a node gets introduced to the network

A node is never "added to a network" as an explicit operation — there is no
join request or registration. A node comes up in isolation and becomes
reachable purely by *speaking*: it broadcasts beacons, and every other node
that hears one decides on its own that a neighbor now exists.

### Lifecycle: `hh_node_init` → `configure` → `start`

Every node is one `hh_node_t` struct (`include/hhsdr/manet/node.h`) owning
ten sub-components. `hh_node_init()` constructs all of them and wires an
event bus between them — nothing is running yet.

**`src/manet/node.c:133` — `hh_node_init(n, cfg, clock, radio)`**

1. `hh_config_validate(cfg)` — rejects a bad config before anything is
   touched (bounds-checks intervals, node_id, queue depths).
2. `hh_dispatcher_init(&n->bus)` — the typed pub/sub event bus every
   component below talks through. Components never call each other
   directly.
3. Seven component inits — `hh_discovery_init`, `hh_neighbor_init`,
   `hh_link_health_init`, `hh_routing_init`, `hh_fd_init`,
   `hh_topology_init`, `hh_sh_init`. Each takes `&n->cfg`, `clock`,
   `&n->bus` — the same three handles, so every component reads one shared
   config and one shared clock.
4. `hh_forwarder_init(&n->forwarder, &n->cfg, radio, &n->routing.publisher)`
   — the data plane gets a pointer to the routing engine's snapshot
   publisher. That pointer is its *only* tie to the control plane.
5. Ten `hh_dispatcher_subscribe(&n->bus, name, EV_MASK, handler_fn, n)`
   calls — wires every consumer to the event types it cares about. E.g.
   `on_beacon_rx` subscribes to `HH_EV_BEACON_RX`; `on_neighbor_up`
   subscribes to `HH_EV_NEIGHBOR_UP`.
6. `hh_radio_set_rx_callback(radio, radio_rx, n)` — registers `radio_rx()`
   as the single ingress point for anything arriving off the air. It just
   forwards to `hh_node_on_frame()`.
7. `n->state = HH_NODE_INITIALIZED` (SCA lifecycle marker). Nothing has
   transmitted or received anything yet.

Two more calls finish bring-up. `hh_node_configure()` applies the log level
and moves state to `CONFIGURED`. `hh_node_start()` (`node.c:206`) is the one
that actually goes live:

```c
hh_status_t hh_node_start(hh_node_t *n)
{
    /* ... state check ... */
    hh_radio_open(n->radio);            /* acquire radio; ENOTIMPL on real HW today */
    hh_discovery_start(&n->discovery);  /* phase = ACQUISITION, beacon due immediately */
    n->state = HH_NODE_RUNNING;
}
```

After this call the node is `HH_NODE_RUNNING` and its discovery component
is armed with `phase = HH_DISC_ACQUISITION`, `last_beacon_at = 0` — which
means the very next `hh_node_tick()` will transmit a beacon, because
"beacon due" is judged against 0.

**Nothing happens until the caller ticks the node.** `hh_node_start()` only
arms state. The driver — production daemon or simulator — must call
`hh_node_tick(n, now)` repeatedly for anything to actually transmit,
receive, or converge.

---

## 2. Discovery → neighbor → route: the introduction sequence

This is the part that actually "introduces" a node to the network. It is
entirely reactive: one node's `hh_discovery_tick()` transmits, the radio
layer delivers the frame to whoever is in range, and the receiving node's
`hh_node_on_frame()` reacts by cascading through three components in
sequence.

### The pipeline

```
radio_rx()  →  hh_node_on_frame()  →  hh_discovery_on_frame()  →  HH_EV_BEACON_RX
   →  on_beacon_rx()  →  hh_neighbor_on_beacon()  →  HH_EV_NEIGHBOR_UP
   →  on_neighbor_up()  →  hh_routing_on_neighbor_up()
```

### Ingress: `hh_node_on_frame()`

**`src/manet/node.c:278`** — the single dispatch point for anything received:

```c
void hh_node_on_frame(hh_node_t *n, const hh_frame_t *f, const hh_link_sample_t *m)
{
    now = hh_now(n->clock);
    switch (f->kind) {
    case HH_FRAME_BEACON:  hh_discovery_on_frame(&n->discovery, f, m); break;
    case HH_FRAME_ROUTING: handle_route_update(n, f, now);             break;
    case HH_FRAME_DATA:    hh_forwarder_forward(&n->forwarder, f, now); break;
    }
}
```

Every inbound frame — from the mock radio in simulation, or from a real
adapter later — passes through exactly this switch. This is the function to
set a breakpoint on if you want to watch a specific node's ingress.

### Step A — `hh_discovery_on_frame()` validates and publishes

**`src/manet/discovery.c:142`**

- Params: `(hh_discovery_t *d, const hh_frame_t *f, const hh_link_sample_t *metrics)`
- Decodes wire bytes → `hh_beacon_t b` via `hh_beacon_decode()`.
- Rejects our own reflected beacon (`b.node_id == cfg->node_id`), and any
  beacon not newer than the last accepted sequence for that source
  (replay/duplicate protection).
- Side effect: on the first beacon from *any* source, calls
  `hh_discovery_notify_neighbor_heard(d)` → flips `d->phase` from
  `HH_DISC_ACQUISITION` to `HH_DISC_STEADY`, slowing the beacon cadence.
- Publishes `HH_EV_BEACON_RX` onto the bus, carrying the decoded beacon plus
  the RF sample (RSSI/SNR) the radio measured for it.

### Step B — the bus fans the event to `on_beacon_rx()`

**`src/manet/node.c:13`** — a bus subscriber, not a direct call from discovery:

```c
static void on_beacon_rx(const hh_event_t *ev, void *ctx)
{
    hh_node_t *n = ctx;
    hh_neighbor_on_beacon(&n->neighbors, &ev->u.beacon_rx.beacon,
                          &ev->u.beacon_rx.sample, ev->timestamp);
    /* the beacon doubles as the liveness heartbeat, so link health is fed
     * from the same event — not a second message stream */
    hh_link_health_on_beacon(&n->link_health, ev->u.beacon_rx.beacon.node_id, ev->timestamp);
    hh_link_health_on_sample(&n->link_health, &ev->u.beacon_rx.sample, ev->timestamp);
}
```

Discovery never calls the neighbor manager directly — it publishes an event
and `node.c` owns the wiring that decides who reacts. This indirection is
deliberate: it's what lets Topology, Routing, and Link Health independently
react to the same neighbor event without any of them knowing about each
other.

### Step C — `hh_neighbor_on_beacon()` creates the neighbor entry

**`src/manet/neighbor.c:73`**

Looks up `b->node_id` in the node's `hh_neighbor_mgr_t` table. Not found →
`alloc_slot()` (LRU-evicting if the table is full), fills in `id`,
`first_heard`, `last_heard`, capabilities, and the RF sample, then publishes
`HH_EV_NEIGHBOR_UP`.

### Step D — the bus fans `NEIGHBOR_UP` to three independent consumers

```c
static void on_neighbor_up(const hh_event_t *ev, void *ctx)
{
    hh_node_t *n = ctx;
    hh_node_id_t id = ev->u.neighbor.neighbor_id;
    hh_link_health_add(&n->link_health, id, ev->timestamp);
    hh_topology_on_neighbor_up(&n->topology, id, ev->timestamp);
    hh_routing_on_neighbor_up(&n->routing, id, ev->timestamp);  /* ← installs the route */
}
```

`hh_routing_on_neighbor_up(id)` is what actually installs the 1-hop route:
`destination=id, next_hop=id, hop_count=1`, published through
`hh_route_publisher_begin/commit` (§4 covers that publisher in detail).
This is the exact call that produces a `ROUTE INSTALLED to B via B` line in
the simulator.

**Multi-hop routes take a second path entirely.** A 2-hop route like
`A → C via B` is never produced by `on_neighbor_up` — A and C aren't
neighbors. It comes from `HH_FRAME_ROUTING` frames: each node's tick
periodically calls `send_route_update()`, which broadcasts its own route
table (distance-vector style) to each neighbor individually. The receiving
node's `handle_route_update()` (`node.c:251`) increments every advertised
hop count by one and calls
`hh_routing_offer(originator, sender, seq, hops, metric, now)` — that's the
function that decides whether the new path beats what's already installed.

---

## 3. The steady-state tick

Everything above is triggered by frame arrival. But someone has to keep
pumping the radio and the timers. That's `hh_node_tick()` — called once per
node per simulated (or real) time step, and it is the actual heartbeat of
the whole stack.

**`src/manet/node.c:333` — `hh_node_tick(n, now)`**, called every 10ms in
the simulator:

1. `hh_radio_poll(n->radio, now)` → `hh_dispatcher_drain_all(&n->bus, 8)` —
   pumps the radio (delivers any queued rx frames via the callback
   registered in §1), then drains up to 8 events those frames produced.
2. `hh_discovery_tick(&n->discovery, now)` — sends a beacon if the interval
   has elapsed. This is what makes the node itself discoverable.
3. `send_route_update(n, now)` (gated on `route_update_interval_ms`) —
   per-neighbor distance-vector broadcast, the mechanism §2's callout
   described.
4. `hh_link_health_tick` → `hh_neighbor_tick_ex` → `hh_routing_tick` →
   `hh_sh_tick` → `hh_topology_evaluate` — order matters and is commented
   in source: link health runs *before* neighbor expiry, so a silent
   neighbor transitions through Degraded → Suspected → Failed rather than
   being silently deleted before the Failure Detector can confirm it.
5. `hh_forwarder_flush(&n->forwarder, now, route_active_timeout_ms)` —
   retries any data packets that were buffered earlier because their route
   wasn't known yet.

This function is what `netsim_step()` calls for every node, every tick, in
the simulator. In the production daemon, the same function is driven by
`main.c`'s run loop instead of the simulator's virtual clock.

---

## 4. Data plane: how a packet actually crosses the mesh

This is the part the architecture is built around protecting: forwarding a
packet must never wait on routing computation. The entire data plane is one
struct (`hh_forwarder_t`) that reads a single atomically-published pointer.

### The lock-free handoff

```
Routing Engine builds draft  →  hh_route_publisher_commit() [one atomic store]
   →  Forwarder reads hh_route_publisher_current()
```

The forwarder never takes a lock the routing engine can hold, never blocks,
and never walks a graph — it does exactly one flat array scan
(`hh_route_lookup`) per packet. This split is called out in `README.md` §2
as "the single most important property of the design."

### Application send: `hh_node_send()`

**`src/manet/node.c:378` → `src/dataplane/forwarder.c:92`**

```
hh_node_send(n, dst, payload, len, now)
  └─ hh_forwarder_send(&n->forwarder, dst, payload, len, cfg->max_hop_count, now)
        // build FWD_HDR_LEN=10-byte header: original src, final dst, ttl, hop count
        f.kind = HH_FRAME_DATA;
        put32(f.data+0, node_id);   // original source
        put32(f.data+4, dst);       // final destination — survives every hop
        f.data[8] = ttl; f.data[9] = 0;
        └─ transmit_via_route(fw, &f, dst)
              snap  = hh_route_publisher_current(fw->routes);   // one atomic read
              route = hh_route_lookup(snap, dst);               // flat scan, no lock
              f.dst = route->next_hop;
              hh_radio_transmit(fw->radio, &f);
```

If `hh_route_lookup` misses (`HH_ERR_AGAIN`), the packet is *not* dropped
immediately — it goes into `fw->pending[]`, a bounded ring the control
plane resolves asynchronously on the next matching `hh_forwarder_flush()`
call in the tick loop (§3, step 5).

### Relay: `hh_forwarder_forward()`

**`src/dataplane/forwarder.c:123`** — reached via `hh_node_on_frame`'s
`HH_FRAME_DATA` case.

A relay node runs the same `transmit_via_route()` fast path, but first:
decodes `final_dst` from the header, delivers locally if it matches this
node's own ID, decrements TTL (dropping on exhaustion — the loop guard),
and increments the hop counter at `f.data[9]` before re-transmitting.

**The forwarder is deliberately outside the SCA resource graph** (see the
header comment in `forwarder.h`) — sub-microsecond forwarding-path timing
is not something the descriptor-driven deployment lifecycle should govern.
It also emits no telemetry synchronously; counters like `forwarded`,
`dropped_ttl`, `queued` are plain increments, read out-of-band by the
management plane.

---

## 5. Struct reference

The structs worth keeping in your head. Everything else in the codebase is
built by composing these.

### `hh_node_t` — `include/hhsdr/manet/node.h`

| Field | Type | Notes |
|---|---|---|
| `cfg` | `hh_config_t` | Full tunable set — node identity, beacon intervals, timeouts, queue depths |
| `bus` | `hh_dispatcher_t` | The event bus every component below is wired through — the only path components use to reach each other |
| `discovery` / `neighbors` / `link_health` / `routing` / `failure_detector` / `topology` / `self_healing` | 7 structs | The control-plane components, one instance each per node |
| `forwarder` | `hh_forwarder_t` | The entire data plane — holds only a read pointer into `routing.publisher` |
| `state` | `hh_node_state_t` | SCA lifecycle: `CREATED` → `INITIALIZED` → `CONFIGURED` → `RUNNING` → `STOPPED` → `RELEASED` |

### `hh_frame_t` — `include/hhsdr/radio/radio.h`

| Field | Type | Notes |
|---|---|---|
| `kind` | `hh_frame_kind_t` | `HH_FRAME_BEACON` · `HH_FRAME_DATA` · `HH_FRAME_ROUTING` — the field `hh_node_on_frame`'s switch dispatches on |
| `src` / `dst` | `hh_node_id_t` | `dst == HH_NODE_ID_INVALID` means broadcast (used for beacons) |
| `len` | `uint16_t` | Bytes valid in `data` |
| `data[512]` | `uint8_t[]` | Opaque wire bytes — never a struct pointer. This is what actually crosses the radio boundary |

### `hh_route_entry_t` / `hh_route_snapshot_t` — `include/hhsdr/manet/route_table.h`

| Field | Type | Notes |
|---|---|---|
| `destination` / `next_hop` | `hh_node_id_t` | The whole point: where a packet needs to go, and who to hand it to now |
| `metric` | `float` | Composite cost, lower is better |
| `hop_count` | `uint8_t` | Distance-vector hop count |
| `alt_next_hop` / `has_alt` | `hh_node_id_t` / `bool` | Warm standby next-hop, so Self-Healing can switch without recomputing |
| `current` (on the publisher) | `const hh_route_snapshot_t *_Atomic` | The single pointer the forwarder reads — swapped by one atomic store in `hh_route_publisher_commit` |

### `hh_forwarder_t` — `include/hhsdr/dataplane/forwarder.h`

| Field | Type | Notes |
|---|---|---|
| `routes` | `const hh_route_publisher_t *` | Read-only. This is the forwarder's entire connection to the control plane |
| `pending[64]` | `hh_pending_packet_t[]` | Bounded buffer for route misses — dropped, never grown, on overflow |
| `forwarded` / `dropped_ttl` / `dropped_no_route` / `queued` | `uint64_t` | Plain counters, incremented synchronously, read out-of-band |

### `hh_radio_ops_t` — `include/hhsdr/radio/radio.h` (the hardware seam)

| Field | Type | Notes |
|---|---|---|
| `open` / `close` | `fn(void*)` | Acquire / release hardware. Today: the mock radio in simulation, or `ENOTIMPL` on real hardware |
| `transmit` | `fn(self, frame)` | Fire-and-forget send — HTI-03 |
| `set_rx_callback` | `fn(self, hh_radio_rx_fn, ctx)` | Registers `radio_rx()` — see §1 step 6. HTI-04/05 |
| `poll` | `fn(self, now)` | Called every tick to pump pending receptions into the callback — the stack needs no thread of its own |

---

Traced from `src/manet/node.c`, `src/dataplane/forwarder.c`,
`src/manet/discovery.c`, `src/manet/neighbor.c`, and the corresponding
headers under `include/hhsdr/`. Every call chain above is production code —
none of it is simulator-only.
