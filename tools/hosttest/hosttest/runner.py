"""Runner: connect, identify the board, run the configured tests, log each one."""
from __future__ import annotations

import threading
from datetime import datetime
from typing import Callable

from .codec import LineCodec
from .command import PASS, FAIL, CommandClient
from .config import Config
from .results import ResultsWriter
from .transport import ConnectFailed, TcpTransport


class TestRunner:
    __test__ = False  # not a unittest case

    def __init__(self, config: Config, transport: TcpTransport,
                 client: CommandClient, writer: ResultsWriter,
                 now: Callable[[], datetime],
                 on_result: Callable[[dict], None] | None = None) -> None:
        self._config = config
        self._transport = transport
        self._client = client
        self._writer = writer
        self._now = now
        self._on_result = on_result or (lambda record: None)
        self._cancel = threading.Event()

    def cancel(self) -> None:
        """Stop before the next test. A command in flight ends at its timeout."""
        self._cancel.set()

    @property
    def is_connected(self) -> bool:
        return self._transport.is_open

    def connect(self) -> list[dict]:
        """Open the link and read the board identity. Returns the records written."""
        records: list[dict] = []

        def emit(record: dict) -> None:
            records.append(record)
            self._on_result(record)

        self._connect(emit)
        return records

    def disconnect(self) -> None:
        self._transport.close()
        self._client.reset()

    def run(self, test_ids: list[str] | None = None) -> list[dict]:
        """Run the selected tests (all when None). Returns the records written."""
        self._cancel.clear()
        records: list[dict] = []
        selected = [t for t in self._config.tests
                    if test_ids is None or t.id in test_ids]

        def emit(record: dict) -> None:
            records.append(record)
            self._on_result(record)

        for test in selected:
            if self._cancel.is_set():
                break
            if not self._transport.is_open and not self._connect(emit):
                break
            started = self._now()
            res = self._client.execute(test.command, test.args, test.expect,
                                       test.timeout_s)
            emit(self._writer.write(
                test.id, res.verdict, res.reason, started,
                tx=res.tx, rx=res.rx,
                params={"command": test.command, "args": test.args,
                        "expect": test.expect, "tag": res.tag,
                        "timeout_s": test.timeout_s},
                metrics={"elapsed_s": round(res.elapsed_s, 6),
                         "discarded_lines": res.discarded}))
        return records

    def _connect(self, emit: Callable[[dict], None]) -> bool:
        """Open the link and read the board identity. False aborts the run."""
        board = self._config.board
        started = self._now()
        try:
            self._transport.connect_with_retry(board.connect_attempts,
                                               board.connect_backoff_s)
        except ConnectFailed as exc:
            emit(self._writer.write("connect", FAIL, str(exc), started,
                                    params={"host": board.host, "port": board.port}))
            return False
        self._client.reset()
        emit(self._writer.write("connect", PASS, "", started,
                                params={"host": board.host, "port": board.port}))
        self._identify(emit)
        return True

    def _identify(self, emit: Callable[[dict], None]) -> None:
        ident = self._config.identity
        started = self._now()
        res = self._client.execute(ident.command, ident.args, ident.expect,
                                   ident.timeout_s)
        verdict, reason = res.verdict, res.reason
        if verdict == PASS:
            board_id = res.fields.get(ident.board_id_field)
            firmware = res.fields.get(ident.firmware_field)
            if board_id is None or firmware is None:
                verdict, reason = FAIL, "identity reply lacks board id or firmware field"
            else:
                self._writer.set_identity(board_id, firmware)
        emit(self._writer.write(
            "identity", verdict, reason, started, tx=res.tx, rx=res.rx,
            params={"command": ident.command, "args": ident.args,
                    "expect": ident.expect, "tag": res.tag,
                    "timeout_s": ident.timeout_s},
            metrics={"elapsed_s": round(res.elapsed_s, 6)}))


def all_passed(records: list[dict]) -> bool:
    return bool(records) and all(r["result"] == PASS for r in records)
