# Video over the radio link: trial plan

| Field | Value |
|---|---|
| Version | 0.1 |
| Status | Draft for review. All pass criteria are internal targets |
| Author | Akhil |
| Subject | One-way video with audio: codec, rate and quality trial over the radio link |

## Contents

1. Purpose and scope
2. How to read this plan
3. The link today
4. Prerequisites and measurements needed first
5. Approach
6. Choosing the video rate
7. Loss and recovery experiments
8. Quality measurement
9. Test stages
10. Pass criteria
11. Record for each run
12. Open points
13. Terms

## 1 Purpose and scope

The purpose is to find which video and audio configuration gives the best usable picture and sound at a fixed share of the channel, while control, emergency and voice traffic are not harmed.

The first priority is a one-way live stream of video with its audio, from one host to another. It is not a video call.

The second priority is the transfer of a recorded video file. It runs as data traffic over TCP and has its own stage in section 9, after the live stream results.

This plan fixes the method, the measurements and the pass criteria. It does not fix the video bitrate. The bitrate is calculated from a measured per-node share, after the measurements in section 4. The main risk is a video rate above what the link can carry, so the rate is never picked first.

Not in scope: two-way video calls, encoding or decoding on the radio node, adaptive bitrate, header compression, retransmission of video packets, and compliance with motion imagery interchange standards. Those are later extensions.

The trial needs the radio link running. One direction carries the stream, and section 4 explains why the return direction is still needed. This document is the plan only.

## 2 How to read this plan

| Marker | Meaning |
|---|---|
| Measured | Read from a test run on the hardware |
| From the code | Read from the source, not measured |
| Calculated | Worked out from the figures above it |
| PROPOSED | A choice made for this plan, open to change |
| UNKNOWN / OPEN | Needs a measurement or an answer |

## 3 The link today

Measured, on a coax cable between two nodes, one direction, with test data:
- Payload rate about 9.8 Mbit/s (294.5 Mbit in 30 s).
- About 0.07 percent of radio frames failed the CRC-32 check, and the bit error rate was about 1.5e-6. This is a cable, not the air.
- About 30 ms from the processor through the radio and back, mostly buffering. One frame takes 0.148 ms on air at FEC rate 3/4.

From the code, frame sizes:
- The on-air frame depends on the FEC mode: 280 bytes with no FEC, 208 bytes at rate 3/4, 136 bytes at rate 1/2.
- The last four bytes are a CRC-32 that the PL fills. The MAC PDU is therefore 276, 204 or 132 bytes.
- A PDU holds a header, a control block, up to 16 messages and a CRC-32. The room left for IP packets is smaller than the PDU.
- Fixed cost of a PDU with a 20-slot frame (`src/mac/pdu.c`) is 32 bytes: header 9, control block 6, slot bitmap 3, CRC-32 4, queue report 5, message sub-header 5.
- One IP packet alone in a PDU can therefore be at most the PDU size minus 32: 244 bytes at 276, 172 at 204 and 100 at 132. Block ACK entries (7 bytes each), link reports (4 each), a beacon and a link acknowledgement byte reduce this. Whether the PL's own CRC-32 is counted twice is UNKNOWN / OPEN.
- Several IP packets can share one PDU: up to 16 messages, each with a 5-byte sub-header.
- The MAC refuses a packet larger than one PDU can carry, because fragmentation across PDUs is not built (`src/mac/slot.c:60`).
- One lost PDU removes every packet packed into it. Packet loss on this link is correlated.

From the code, traffic handling:
- The order is control, emergency, voice, video, data.
- DSCP CS3, AF31 to AF33, CS4 and AF41 to AF43 map to the video class. Today the mapping is applied only in the simulator bridge. The real data path has no call to it yet.

Placeholders, not measured: 20 slots per frame, 1 ms slots, voice reserve of 4 slots, cap of 8 slots per node, video to data weight of 2 to 1. A node's real share of the channel is UNKNOWN / OPEN.

Simulation: routing is OLSRv2. A link break was repaired about 11.4 s after the cut, in a 4-node ring with OLSRd2 default timers (HELLO interval 2 s, HELLO validity 20 s, TC interval 5 s). The outage time depends on the timers and the topology. It is measured in this trial and not assumed.

## 4 Prerequisites and measurements needed first

The nodes must carry IP packets over the radio through an interface of their own.

The video and audio go in one direction. A return path is still needed for the RTCP feedback (receiver reports and PLI), probably for address resolution, and for OLSRv2 neighbour discovery, which uses only links that work both ways. Both radio directions therefore have to hold, although the return direction carries little traffic (BUG-01 in `docs/BUGS.md`). A one-way run with static routes and no feedback is the fallback. Whether it works is UNKNOWN / OPEN.

Measurements, in this order:
1. The IP MTU of the radio interface, and the PDU overhead (header, control block, CRC-32).
2. How many IP packets fit in one PDU, and whether one PDU can hold packets from different video frames.
3. The slot allocation of one node, and the sustainable goodput of one node over one, two and three hops.
4. The real voice reservation, and the control and routing traffic rate.
5. Radio-frame loss, consecutive lost frames, and IP packets lost per lost frame.
6. One-way delay, jitter and forwarding delay per node.
7. The OLSRv2 HELLO and TC timers in use, and the link-break detection time.
8. Encoder output on the real source: peak bitrate and average packet size.
9. Which sender and receiver software on the hosts supports the settings in section 5, and sends and acts on PLI.
10. The audio packet size and rate at the radio MTU, for the chosen audio codec, and the audio and video offset the sender and receiver reach.

## 5 Approach

PROPOSED, first trial only:
- Encoding and decoding are done on the host PCs attached to the nodes. The radio node only forwards packets. Software encoders such as x264 and x265 are used, so slice size, intra refresh, GOP and VBV are set by this plan and are not limited by a camera.
- Codec: H.264 (RFC 6184) and H.265 (RFC 7798) are both run in stage 1 at the same rate, and the measured quality at that rate picks the primary. VP8 is the fallback.
- Audio travels with the video as a separate RTP stream in the same session, kept in step with the video by RTCP sender reports. MPEG-TS multiplexing is not used, because a 188-byte TS packet is 216 bytes with IPv4 and UDP and does not fit the small MTU.
- Audio codec: Opus (RFC 7587), mono, 20 ms frames, about 16 kbit/s. To be confirmed. At 16 kbit/s a frame carries 40 bytes of audio, so each packet is 80 bytes with IPv4, UDP and RTP headers. Fifty packets a second use 32 kbit/s at the IP layer, twice the audio rate.
- Audio class: the stream's audio is sent in the video class for the first trial. It is then not a voice call and does not use the voice reserve. To be confirmed.
- Transport: RTP over UDP (RFC 3550). IPv4 first, IPv6 as a separate test. No IP fragmentation.
- Start at 640 by 360 and 15 frames per second.
- Low-latency encoding: no B-frames, no lookahead, GOP of 1 to 2 s, constant or capped bitrate, VBV buffer of 0.5 to 1 s, and small slices sized to the MTU.
- The encoder bitrate is fixed for each run. There is no adaptive bitrate.
- There is no retransmission. It uses airtime, and the packets arrive after their display time.
- Header compression (ROHC) is a later experiment. Do not rely on a saving until it is measured.

## 6 Choosing the video rate

Define:
- `C_node`: measured per-node share in Mbit/s, stating the layer it was measured at.
- `R_other`: airtime kept for control, emergency, voice and data.
- `M`: safety margin. PROPOSED 20 percent.
- `eta`: video bytes per IP packet divided by the IP packet size.
- `R_audio` and `eta_a`: the audio rate, and audio bytes per IP packet divided by the IP packet size. For the proposed audio they are 16 kbit/s and 0.5.
- `f`: FEC parity rate divided by source rate. Zero with no FEC.

    R_video <= ((C_node - R_other) x (1 - M) / (1 + f) - R_audio / eta_a) x eta

With no audio, `R_audio` is zero and this is the video-only formula.

Take `eta` from the measured packet sizes, since the last packet of each unit is smaller than the rest. Add the MAC overhead per frame once it is known. If `C_node` is measured as IP goodput, do not subtract the IP headers again.

Calculated, for H.264 with FU-A packets: `eta` at IPv4 is 58, 108 and 158 bytes at an MTU of 100, 150 and 200 bytes, so 58, 72 and 79 percent. At IPv6 it is 38, 58.7 and 69 percent.

Calculated ceilings for the video, with `R_other` at 25 percent of the share, `M` at 20 percent, no FEC, IPv4, and the proposed audio (32 kbit/s at the IP layer), in Mbit/s. These are ceilings and not operating points:

| Per-node share | MTU 100 B | MTU 150 B | MTU 200 B |
|---:|---:|---:|---:|
| 0.5 | 0.155 | 0.193 | 0.212 |
| 1.5 | 0.503 | 0.625 | 0.686 |
| 3.5 | 1.199 | 1.489 | 1.634 |

Recalculate after the measurements in section 4.

## 7 Loss and recovery experiments

Small packets multiply loss. Calculated: at 400 kbit/s and 15 frames per second a frame averages 3.3 kB, which is about 57 packets of 58 bytes. With independent loss p per packet, the share of video frames that lose at least one packet is 1 - (1 - p)^57. That is about 4 percent at p of 0.07 percent, 25 percent at 0.5 percent and 44 percent at 1 percent. Real loss is correlated, so use the measured burst data.

Loss levels, as average radio-frame loss: 0, 0.1, 0.5, 1, 2 and 5 percent. Each level is run with independent loss and with bursty loss.

The loss emulator has to work on radio frames. It packs IP packets into frame-sized groups, applies a Gilbert-Elliott model to the groups, and drops a whole group when it is lost. `tc netem` with `loss gemodel` works on single IP packets and does not know which packets share a frame. Use it only for simpler tests, and label them as IP-level burst loss. The burst parameters must come from the measured radio, not from invented values.

FEC is compared at a fixed total on-air rate:

    R_video = R_total / (1 + f)

At the same total rate, 10 percent FEC leaves 90.9 percent of the no-FEC video rate and 20 percent FEC leaves 83.3 percent. The total counts media, FEC, IP, UDP, RTP and FEC headers, MAC overhead and padding. Runs F0 (no FEC), F1 (10 percent) and F2 (20 percent) use the same video, loss trace and background traffic. RFC 5109 describes XOR parity FEC. Place the FEC where it can recover the real loss unit. If a lost frame removes several packets, protect across frames.

Recovery configurations: periodic IDR frames, periodic intra refresh, and intra refresh with FEC. The receiver asks for a new reference picture with PLI (RFC 4585). FIR (RFC 5104) is only for a full decoder refresh.

## 8 Quality measurement

Delivery:
- RTP loss from missing sequence numbers. IP loss from transmitted and received counts.
- Radio-frame loss from the PL CRC counters and the MAC receive counters.
- A frame number burned into every source frame, so a missing frame is found by its number. A repeated picture counts as a freeze.
- Freeze count, freeze duration, longest freeze and decoded frames per second.

Latency:
- Burned-in timestamp from the source PC to the receiver PC. The nodes have no real-time clock, so take both timestamps on PCs with synchronised clocks, and record offset and drift.
- Glass to glass, with an LED or running timer in front of the camera and a high-speed camera on both screens.
- RTP timestamps are not wall-clock time. Use them with the RTCP sender reports.

Picture quality:
- VMAF as the main metric. SSIM and PSNR as supporting metrics.
- Match frames by their number and score only matching frames. Report missing frames separately. Never fill gaps with copies of earlier frames.
- VMAF does not measure latency, freezes or loss. Always report it next to frame loss and freeze duration.

Audio and synchronisation:
- Audio packet loss from missing RTP sequence numbers, and the longest audio gap.
- Audio quality against the source with a reference metric such as ViSQOL, or PESQ or POLQA where licensed. To be chosen.
- Audio and video offset, measured with a test clip that has a flash and a beep at the same instant, read from the received recording.

Example for clips that are already aligned, with the distorted clip first and the reference second. The psnr and ssim filters work the same way:

    ffmpeg -i received.mp4 -i reference.mp4 -lavfi "[0:v]setpts=PTS-STARTPTS[d];[1:v]setpts=PTS-STARTPTS[r];[d][r]libvmaf=log_fmt=json:log_path=vmaf.json" -f null -

Source clips, the same for every run: a stationary road scene, a moving vehicle, people moving in an urban scene, trees and vegetation, a fast pan, and a low-light scene.

Operator assessment: an internal evaluation of picture quality, smoothness, freeze severity and whether vehicles, people and objects can be identified, with several operators and the same screen. It is not a test under ITU-T P.910 unless that procedure is followed in full.

## 9 Test stages

| Stage | What | Variables |
|---|---|---|
| 1 Baseline | Two nodes, no added loss, no FEC | H.264 and H.265 at the same rate, 360p and 480p, 10, 15 and 25 fps, 300 to 1000 kbit/s within the ceiling, with the audio stream |
| 2 Loss | Stage 1 with loss | Six loss levels, independent and bursty |
| 3 FEC | Fixed total on-air rate | F0, F1 and F2 at selected loss levels |
| 4 Multi-hop | Real OLSRv2 | One, two and three hops. The per-node share is measured for each and not assumed constant |
| 5 Background | Priority order | Video alone, then adding control, emergency, voice and data. Check that the DSCP mapping reaches the MAC queues, because a DSCP value alone does not prove the priority |
| 6 Outage | Controlled link breaks | HELLO, hold, TC interval and validity time varied |
| 7 File transfer | Second priority. A recorded clip sent over TCP between the hosts | Clip sizes, loss levels from stage 2, one to three hops. Record transfer time, goodput, retransmissions and whether the transfer completes. Check that control, emergency and voice traffic are not starved |

Outage times to record: T0 link failure introduced, T1 failure detected, T2 neighbour state changes, T3 route changes, T4 forwarding resumes, T5 RTP packets resume, T6 first valid decoded frame. Route recovery is T4 minus T0. Video recovery is T6 minus T4. The interruption the user sees is T6 minus T0.

Repeats, PROPOSED: five runs of 10 minutes per condition, 15 minutes for multi-hop and background runs, and 10 outage events per timer setting.

## 10 Pass criteria

All targets below are INTERNAL TARGETS and PROPOSED. Each needs an operator requirement behind it before it is final.

| Parameter | Target | Condition |
|---|---|---|
| Encoder rate | Fixed and within the calculated ceiling | All runs |
| Total on-air rate | Never above the allocated share | All runs |
| RTP loss | 1 percent or less | Nominal |
| Decoded frame loss | 1 percent or less | Nominal |
| Audio packet loss | 1 percent or less | Nominal |
| Longest audio gap | 200 ms or less | Nominal |
| Audio and video offset | 100 ms or less, either way | Nominal |
| Longest freeze | 500 ms or less | Nominal |
| Glass-to-glass latency | 300 ms or less | One hop |
| Glass-to-glass latency | 500 ms or less | Three hops |
| VMAF | 80 or more | Nominal |
| VMAF | 70 or more | Stress (high loss) |
| Higher-priority traffic | No starvation by video | All runs |
| Recovery after route restoration | Measured and reported, no target set | Outage |

The latency targets were set with interactive use in mind. For a one-way stream they may be relaxed, so review them with the other targets.

Use separate criteria for nominal, degraded and outage conditions. Do not apply the nominal VMAF target to 5 percent burst loss.

## 11 Record for each run

- Radio settings (modulation, FEC rate) and MAC settings (frame size, slot plan, allocation).
- IP version and MTU, OLSRv2 configuration, topology and hop count.
- Video settings, encoder version, packet size and rate, FEC setup.
- Loss model and parameters, and background traffic rates.
- All quality and delivery results, and latency and recovery times.
- The evidence: packet captures, encoder and receiver logs, and the source and received clips. Keep the exact encoder commands.

## 12 Open points

Blocked on the PL and the `manet0` binding:
- The IP MTU on the real link. The PDU capacity is a placeholder (128 bytes in `hh_slot_config_default`) until the PL fixes the slot size (U-02). Section 3 gives the room per PDU for the three FEC modes.
- The DSCP to MAC queue mapping on the real data path. The table exists, and today only the simulator bridge calls it (`tools/sim/slot_bridge_frames.c:60`).

To be measured on the boards:
- A node's slot share and its goodput over one to three hops.
- Where FEC runs, and how it recovers a whole lost frame.
- Which host software sends and acts on PLI feedback.
- The MAC link-break detection time.

Routing:
- The return path. OLSRd2 computes routes from symmetric links only (`nhdp_domain.c:1052` in the OONF source), so the return direction is needed for routing even when the video goes one way. The effect of one blocked direction is not tested. The simulator can only cut both directions together (`tests/sim/slot_net.c:404`).
- The OLSRv2 timers behind the 11.4 s repair. The run configs set none, so OONF defaults applied: HELLO interval 2.0 s, HELLO validity 20.0 s, TC interval 5.0 s, TC validity 300 s (`nhdp.c:123-125`, `olsrv2.c:179-180` in the OONF checkout at v0.15.1-187). A 20 s HELLO validity alone does not explain a repair after 11.4 s, so how the break was detected is UNKNOWN / OPEN.

For the team:
- The audio codec and rate. Opus at about 16 kbit/s in the video class is proposed, to be adjusted once the real share is known.
- Motion imagery interoperability standards, to be checked against the customer requirement. They are not part of this trial.
- Pass criteria review.
- RFC 7587 gives no fixed bitrate range for Opus, only a receiver limit (`maxaveragebitrate`). The 16 kbit/s rate is an encoder setting of this plan.

## 13 Terms

| Term | Meaning |
|---|---|
| CRC-32 | A 32-bit check value that shows whether a frame arrived intact |
| FEC | Forward error correction. Extra data that lets the receiver repair some losses |
| PDU | The MAC unit sent in one slot. It carries a header, a control block and messages |
| DSCP | The IP field that marks a packet's traffic class |
| RTP | The packet format that carries video over UDP |
| GOP | The distance between full reference pictures in the video |
| IDR | A full reference picture that a decoder can start from |
| VBV | The encoder's model of its output buffer, which limits bitrate bursts |
| PLI | A receiver request for a new reference picture |
| FIR | A receiver request for a full decoder refresh |
| RTCP | The control stream that runs beside RTP. It carries receiver reports, sender timing and requests such as PLI |
| A/V sync | How closely the sound lines up with the picture |
| VMAF, SSIM, PSNR | Metrics that compare a received picture with its source |
| ROHC | A method that compresses IP, UDP and RTP headers |
| Gilbert-Elliott model | A two-state loss model that produces bursts of lost units |
| Goodput | The useful data rate, after headers and repeats |
