# hh_mac

The MAC software of the HH-SDR. It has four parts: the MAC service for the
network layer, the slot scheduler with its PDU codec, network entry, and the
slot allocator with its traffic classes.

Everything here runs in the host simulation. The binding to the logic, burst
timing and the radio are not defined yet (U-02), so nothing here is
a hardware contract.

## MAC service (`mac.c`)

Sits between the network layer above and the MAC logic below. It holds frames
waiting to go out, received frames waiting for delivery, and link records per
neighbour.

It follows the MAC to network layer interface specification v1.1: Open,
Close, Send, pushed Receive with events, Status, Link metrics and Net preset
select; five traffic classes (control, emergency, voice, video, data) with a
queue, drop policy and lifetime each; transmit inhibit; a control cap and a
minimum share for user traffic; a holding buffer per class; per-frame and
per-neighbour metrics, each with a valid bit.

Details: [docs/MAC-SERVICE.md](../../docs/MAC-SERVICE.md).

## Slot scheduler and PDU (`slot.c`, `pdu.c`)

On each own slot the scheduler takes queued frames from the MAC, packs them into
one PDU v2 (control block plus messages) and hands it to a transmit function.
Received PDUs are decoded and passed up. Time comes only from the injected
clock. A frame is 20 slots of 1 ms by default, 50 frames make a superframe.
Capacity of a slot is a placeholder (U-02).

Details: [docs/SLOT-ENGINE-SPEC.md](../../docs/SLOT-ENGINE-SPEC.md).

## Network entry (`entry.c`)

A radio that owns no slot listens, sends a request in a shared entry slot with
random back-off, and a member answers with a grant. Also the leave. A member can
only grant a slot that nobody owns.

Details: [docs/MAC-03_Contention_Access_and_Network_Entry.md](../../docs/MAC-03_Contention_Access_and_Network_Entry.md).

## Slot allocation and traffic classes (`alloc.c`, `dscp.c`)

Slots follow demand. There is no master: every node computes the same slot map
from the same inputs.

**Demand.** Every own-slot PDU carries the 5-byte queue report of the control
block, one byte per class. The byte is the offered load: the larger of the
messages sent in the last frame and those sent so far in this frame plus those
still queued, capped at 255. Each node keeps the latest report of every node.

**The map.** `hh_alloc_compute` is a pure function of the parameters, the
demand table, the time and the superframe number. In order:

1. Fixed slots (shared entry slots) are left out.
2. Every member gets one keep-alive slot.
3. Voice is served from a reserve, lowest node id first.
4. `join_reserve` slots (one by default) are held idle so a joining radio can be
   granted a free slot.
5. The rest is split between video and data by weight, then one slot at a time
   to each node that still wants more, so a node that asks for little is
   protected.
6. A per-node cap limits any one node. What a capped node cannot take goes to
   the others.
7. Slots nobody asked for are shared round-robin, except the idle reserve.

A report older than the stale limit counts as no demand. A node silent for the
gone limit is no longer a member. More members than slots gives `HH_ERR_NOMEM`
and the map is left unchanged.

**When it applies.** At each superframe boundary a node applies the map it
computed one superframe earlier, taking its own row, then computes the next one.
Demand therefore takes about two superframes to show in the map. If two nodes
claim the same slot, the lower node id keeps it. A node left with no slot keeps
the slots it has.

**Classes.** Three allocation classes: voice, video, data. Control and emergency
stay outside the sharing. `hh_mac_class_from_dscp` maps an IP DSCP to a class:
EF (46) is voice; CS3, AF31 to AF33, CS4 and AF41 to AF43 (24, 26, 28, 30, 32, 34,
36, 38) are video; every other value, call signalling CS5 included, is data. It
never returns control or emergency.

**Voice admission.** `hh_slot_voice_admit` asks, read-only, whether a new voice
flow fits the reserve, using the same table and rule as the allocator. The call
module of the data terminal uses it to refuse a call as busy
(`hh_call_set_admit`, see `apps/dataterm/README.md`). Two nodes asking at the
same moment can both be told yes, because a call shows in the table only once its
traffic is reported.

**With network entry.** Entry grants stay. A radio that is joining or leaving
stays out of the allocation, and the slots of a grant not yet confirmed by the
joiner's first burst are kept out of the map. The entry logic is told whenever
the allocator or a clash changes a member's slots.

### Enabling it

```c
hh_alloc_params_t p;
hh_alloc_params_default(&p, slots_per_frame);   /* placeholder values */
hh_slot_enable_alloc(&slot, &p);
```

Every value in `hh_alloc_params_t` marked TODO in `alloc.h` is a placeholder
until the network owner decides it: voice reserve 4, weights 2 to 1, cap 8 of 20,
idle reserve 1, stale and gone limits, messages per slot.

### Tests

| Test | What it checks |
|---|---|
| `test_alloc` | the map rules, the cap, the idle reserve, voice admission |
| `test_dscp` | all 64 DSCP values, control and emergency unreachable |
| `test_slot` | queue report, offered load, clash rule, enabling allocation |
| `test_slot_net` | a greedy node held at the cap (17/1/1/1 without, 8/4/4/4 with), loss up to 500 per mille, a cut-off node, entry with allocation, joiners under loss, leave and rejoin |
| `test_call_admit_net` | a voice call refused when the reserve is used, a smaller one admitted |

Run them with `ctest --test-dir build --output-on-failure`.

### Known limits

- No smoothing of demand and no rule keeping slot positions stable between maps.
- Demand counts messages, not calls, so two calls started in the same frame can both be admitted.
- A member that leaves stays in the tables until its report goes stale.
- Open values and decisions: U-05 and U-06 in `unknown.md`.
