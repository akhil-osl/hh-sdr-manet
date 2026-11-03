"""GUI controller: state, worker thread and event queue, with no Tkinter.

All board I/O runs on one worker thread. The worker only puts events on a
queue. The GUI thread calls poll() on a timer, which applies the events to
the state and returns them, so widgets are only ever touched by one thread.
"""
from __future__ import annotations

import queue
import threading
from typing import Callable

from .results import UNKNOWN
from .session import Session, build_session

DISCONNECTED = "Disconnected"
CONNECTING = "Connecting"
CONNECTED = "Connected"
LINK_LOST = "Link lost"


class Busy(Exception):
    """An action was requested while a worker task was running."""


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

    def cancel(self) -> None:
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

    def _finish(self) -> None:
        self.running = False
        self.cancelling = False
        if self._connected():
            self.link = CONNECTED
        elif self._was_connected:
            self.link = LINK_LOST
        else:
            self.link = DISCONNECTED
