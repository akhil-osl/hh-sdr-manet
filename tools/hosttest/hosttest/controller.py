"""GUI controller: state, worker thread and event queue, with no Tkinter.

All board I/O runs on one worker thread. The worker only puts events on a
queue. The GUI thread calls poll() on a timer, which applies the events to
the state and returns them, so widgets are only ever touched by one thread.
"""
from __future__ import annotations

import queue
import threading
import time
from typing import Callable

from .results import UNKNOWN
from .session import Session, build_session
from .status import StatusModel

DISCONNECTED = "Disconnected"
CONNECTING = "Connecting"
CONNECTED = "Connected"
LINK_LOST = "Link lost"


class Busy(Exception):
    """An action was requested while a worker task was running."""


class StatusNotConfigured(Exception):
    """The loaded config has no 'status' section, so there is nowhere to read."""


class Controller:
    def __init__(self, build: Callable[..., Session] = build_session) -> None:
        self._build = build
        self._events: queue.Queue = queue.Queue()
        self._worker: threading.Thread | None = None
        self._was_connected = False
        self.session: Session | None = None
        self.config_path = ""
        self.link = DISCONNECTED
        self.board_id = UNKNOWN
        self.firmware = UNKNOWN
        self.running = False
        self.cancelling = False
        self.records: list[dict] = []
        self.last_by_test: dict[str, dict] = {}
        self.status = StatusModel()
        self.status_running = False
        self._status_stop = threading.Event()

    def load(self, path: str) -> None:
        """Replace the session with one built from `path`. Raises ConfigError."""
        self._require_idle()
        session = self._build(path, on_result=self._on_result)
        if self.session is not None:
            self.session.close()
        self.session = session
        self.config_path = path
        self.link = DISCONNECTED
        self.board_id = self.firmware = UNKNOWN
        self._was_connected = False
        self.records = []
        self.last_by_test = {}
        self.status.clear()

    @property
    def loaded(self) -> bool:
        return self.session is not None

    def connect(self) -> None:
        self._start(lambda runner: runner.connect(), CONNECTING)

    def disconnect(self) -> None:
        self._require_idle()
        if self.session is not None:
            self.session.runner.disconnect()
        self._was_connected = False
        self.link = DISCONNECTED

    def run(self, test_ids: list[str] | None = None) -> None:
        self._start(lambda runner: runner.run(test_ids), CONNECTING
                    if not self._connected() else CONNECTED)

    @property
    def status_configured(self) -> bool:
        return self.session is not None and self.session.status_source is not None

    def start_status(self, period_s: float = 2.0) -> None:
        """Read the board's link counters every period_s until stopped.

        The read goes over ssh, not over the test link. A failed read marks the
        value stale; nothing is shown as live that was not just read.
        """
        self._require_idle()
        if not self.status_configured:
            raise StatusNotConfigured("the config has no status section")
        source = self.session.status_source
        self._status_stop.clear()

        def poll_status(_runner) -> None:
            while not self._status_stop.is_set():
                ok, counters, reason = source.read()
                self._events.put(("status", time.monotonic(), ok, counters, reason))
                self._status_stop.wait(period_s)

        self._start(poll_status, self.link)
        self.status_running = True

    def stop_status(self) -> None:
        self._status_stop.set()

    def cancel(self) -> None:
        self._status_stop.set()
        if self.running and self.session is not None:
            self.cancelling = True
            self.session.runner.cancel()

    def poll(self) -> list[tuple]:
        """Apply queued events to the state. Call from the GUI thread only."""
        applied = []
        while True:
            try:
                event = self._events.get_nowait()
            except queue.Empty:
                return applied
            kind = event[0]
            if kind == "record":
                self._apply_record(event[1])
            elif kind == "status":
                self._apply_status(*event[1:])
            elif kind == "done":
                self._finish()
            applied.append(event)

    def join(self, timeout: float | None = None) -> None:
        if self._worker is not None:
            self._worker.join(timeout)

    def shutdown(self) -> None:
        """Cancel, wait for the worker, close the session."""
        self.cancel()
        self.join(timeout=10)
        if self.session is not None:
            self.session.close()
            self.session = None

    # -- internals -------------------------------------------------------

    def _connected(self) -> bool:
        return self.session is not None and self.session.runner.is_connected

    def _require_idle(self) -> None:
        if self.running:
            raise Busy("a task is running")

    def _start(self, task: Callable, link_while_running: str) -> None:
        self._require_idle()
        if self.session is None:
            raise RuntimeError("no configuration loaded")
        runner = self.session.runner
        self._was_connected = self._connected()
        self.running = True
        self.cancelling = False
        self.link = link_while_running

        def work() -> None:
            try:
                task(runner)
            finally:
                self._events.put(("done",))

        self._worker = threading.Thread(target=work, name="hosttest-worker",
                                        daemon=True)
        self._worker.start()

    def _on_result(self, record: dict) -> None:
        # Called on the worker thread: only hand the record over.
        self._events.put(("record", record))

    def _apply_record(self, record: dict) -> None:
        self.records.append(record)
        if record["check"] == "connect" and record["result"] == "pass":
            self._was_connected = True
        if record["check"] == "identity" and record["result"] == "pass":
            self.board_id, self.firmware = record["board_id"], record["firmware"]
        elif record["check"] not in ("connect", "identity"):
            self.last_by_test[record["check"]] = record

    def _apply_status(self, at: float, ok: bool, counters: dict, reason: str) -> None:
        self.status.set_reading(at, counters if ok else None, "" if ok else reason)

    def _finish(self) -> None:
        self.running = False
        self.status_running = False
        self.cancelling = False
        if self._connected():
            self.link = CONNECTED
        elif self._was_connected:
            self.link = LINK_LOST
        else:
            self.link = DISCONNECTED
