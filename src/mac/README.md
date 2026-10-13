# hh_mac

MAC service for the network layer. It sits between the network layer above
and the MAC logic below, and holds frames waiting to go out, received frames
waiting to be collected, and link quality reports per neighbour.

It provides the seven operations of the MAC to network layer interface note
(Open, Close, Send, Receive, Status, Link metrics, Channel select), one
bounded priority queue per traffic class (control and data), a bounded
receive buffer, and per-frame and per-neighbour metrics (RSSI, SNR, PER),
each with a valid flag.

Status: skeleton. Not yet joined to the network layer or to the MAC logic.

Details: [docs/MAC-SERVICE.md](../../docs/MAC-SERVICE.md).
