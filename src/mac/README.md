# hh_mac

MAC service for the network layer. It sits between the network layer above
and the MAC logic below, and holds frames waiting to go out, received frames
waiting for delivery, and link records per neighbour.

It follows the MAC to network layer interface specification v1.1: Open,
Close, Send, pushed Receive with events, Status, Link metrics and Net preset
select; five traffic classes (control, emergency, voice, video, data) with a
queue, drop policy and lifetime each; transmit inhibit; a control cap and a
minimum share for user traffic; a holding buffer per class; per-frame and
per-neighbour metrics, each with a valid bit.

Status: skeleton. Not yet joined to the network layer or to the MAC logic.

Details: [docs/MAC-SERVICE.md](../../docs/MAC-SERVICE.md).
