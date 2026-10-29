# `tools/hosttest/` - host test application

Python program that runs on a host PC and tests an evaluation board over TCP.
It sends commands, waits for the reply with the matching tag, gives pass or
fail, and logs every command, reply and verdict with a timestamp, the board
ID and the firmware version.

**Status:** development and test tooling. Nothing here is linked into, or
installed by, a production target. Standard library only, Python 3.10 or newer.

**Open contract:** the board's command and reply format is not defined (U-26).
`hosttest/codec.py` is a placeholder, and the commands in the example config
are examples. Only the fake board in `tests/` has been used so far.

## Layers

```
cli.py / gui.py      operator interfaces (gui.py is a thin Tkinter view)
controller.py        GUI state, worker thread and event queue, no Tkinter
session.py           builds the runner and everything under it from a config
runner.py            connect, read identity, run the test list, log each test
command.py           send one command, wait for its tag, return a verdict
codec.py             encode, frame, decode (placeholder)
transport.py         TCP with deadlines and bounded reconnect
results.py           JSON Lines log, CSV export
config.py            JSON config loader
```

The command layer never raises for a bad board. A timeout, a closed link, an
unmatched tag or a wrong value comes back as a failed result with a reason.
Replies with another tag, and lines that cannot be parsed, are discarded and
listed in `metrics.discarded_lines`, so a late reply to an earlier command can
never pass the current one. A command is never sent again after a failure.

## Run

```bash
cd tools/hosttest
cp config/hosttest.example.json config/hosttest.json   # then edit
python3 -m hosttest.cli --config config/hosttest.json [--test ID ...] [--csv]
```

Exit status: 0 all passed, 1 a test or the connection failed, 2 bad usage or
config. The log is written to `<log_dir>/<board name>-<UTC time>.jsonl`.

### GUI

```bash
cd tools/hosttest
python3 -m hosttest.gui [--config config/hosttest.json]
```

One window, one board. Load a config, optionally Connect to see the link, board
ID and firmware, then Run all (Ctrl+R) or select tests and Run selected. Cancel
(Esc) takes effect before the next test. Select a row in Results to see the
command and reply text. All board I/O runs on a worker thread, so the window
stays responsive if the board stops answering. `gui.py` and `controller.py`
never import the transport, codec or command layer (checked by a test).

To try the CLI or GUI without a board, run the fake board and point the config
at `127.0.0.1` and the same port:

```bash
python3 tests/run_fake_board.py --port 9000 [--value 7]
```

## Config

JSON has no comments, so each field is described here.

| Field | Meaning |
|---|---|
| `board.name` | Name used in log file names and records |
| `board.transport` | `tcp` is the only transport |
| `board.host`, `board.port` | Board address |
| `board.connect_timeout_s` | Limit for one connection attempt |
| `board.connect_attempts`, `board.connect_backoff_s` | Bounded retry; the wait doubles after each failure |
| `log_dir` | Folder for logs, default `hosttest-results` |
| `identity` | Command run after each connect to read the board ID and firmware version |
| `identity.board_id_field`, `identity.firmware_field` | Reply fields that hold them |
| `tests[].id` | Unique test name, used by `--test` |
| `tests[].command`, `tests[].args` | What to send |
| `tests[].expect` | Reply fields that must match, as strings. `status` is the reply status. Required and not empty |
| `tests[].timeout_s` | Limit for send plus reply |

A new test is a new entry in `tests`. No code change is needed.

## Log record

One JSON object per line, schema `hh-atp-evidence/2`. It extends
`hh-atp-evidence/1` (see `tools/atp/README.md`) with the board identity and the
command text.

| Field | Meaning |
|---|---|
| `check` | `connect`, `identity` or the test id |
| `result`, `reason` | `pass` or `fail`, and why it failed |
| `started_utc`, `finished_utc` | UTC time with milliseconds |
| `host`, `board_name`, `board_id`, `firmware` | Where it ran and which board. `UNKNOWN` until the identity query succeeds |
| `tx`, `rx` | Command line sent and reply line received, `null` when none |
| `params`, `metrics` | Test parameters; elapsed time and discarded lines |

## Tests

```bash
ctest --test-dir build -R hosttest --output-on-failure
```

`tests/fake_board.py` is a scripted loopback server standing in for a board.
It is test code only. The tests use no hardware.
