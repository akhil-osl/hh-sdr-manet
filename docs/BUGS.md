# Bugs

Open defects found on hardware or in test. One heading per bug, numbered BUG-nn. Status values: OPEN, FIXED, WORKAROUND.

## BUG-01 Receiver may not find the signal again after losing it

Status: OPEN. Cause not confirmed. Did not reproduce on the later builds.

What was seen:
- First build, two boards: after a short loss of the other board's signal, one receiver stayed at `link_status` 0 until its radio was restarted. A restart also switches off that board's transmitter, so the other board lost its lock the same way.
- Later build, two boards: six runs with the far transmitter switched off for 6 to 13 s. The receiver found the signal again every time. Both directions held for 15 min.
- Later build, one board in loopback: the cable was out for up to 30 s with no command given. The receiver locked again by itself within one 5 s reading.

Effect: routing, network entry and ping need both directions at once. A receiver that does not recover cannot hold a link in service.

What is not known:
- Why it failed on the first build and not later. The build changed between the runs, and the cause was never confirmed.
- Whether a restart of one board, or a longer gap, still breaks it.
- Evidence of the one-board test is in `evidence/t2074-board/cable-pull`.

What is needed:
- A two-board run with a long gap and a restart of one board.
- From the PL owner: confirm the cause, and make the receiver search again by itself after a loss.

## BUG-02 Heavy traffic is lost when packets queue up

Status: OPEN. Cause found. Fix made on a branch, not merged.

What was seen: on one board in loopback, a sender with a queue backed up got only 16 to 18 of 750 frames back. One payload every 20 ms worked, 1440 of 1440.

Cause: when more packets were waiting, the MAC set a "more data" bit in the frame header. The PL hardware that adds and checks the CRC accepts only the header without that bit, so such frames get no CRC and are lost. Nothing reads the bit. The bit was already on main.

Proof: with the bit forced off in a test build, 749 of 749 frames and 1054 of 1054 payloads came back.

Fix: the MAC never sets the bit. Branch `fix/bug-02-more-data-bit`, one line in `src/mac/slot.c` and its test. The standard and sanitizer builds pass, 57 of 57. It is not committed or merged yet, and it has not been run on a board.

If the bit is needed later, the PL owner would change the CRC hardware to accept it.
