"""Session: builds the runner and everything under it from a config file.

The CLI and the GUI both start here, so the layers are wired in one place.
"""
from __future__ import annotations

import os
import socket
from dataclasses import dataclass
from datetime import datetime, timezone
from typing import Callable

from .codec import LineCodec
from .command import CommandClient
from .config import Config, load_config
from .results import ResultsWriter
from .runner import TestRunner
from .transport import TcpTransport


def utc_now() -> datetime:
    return datetime.now(timezone.utc)


@dataclass
class Session:
    config: Config
    runner: TestRunner
    log_path: str
    _transport: TcpTransport
    _writer: ResultsWriter

    def close(self) -> None:
        self._transport.close()
        self._writer.close()


def build_session(config_path: str,
                  on_result: Callable[[dict], None] | None = None,
                  now: Callable[[], datetime] = utc_now) -> Session:
    """Raises ConfigError. No file is created until the first record."""
    config = load_config(config_path)
    board = config.board
    stamp = now().strftime("%Y%m%dT%H%M%SZ")
    log_path = os.path.join(config.log_dir, f"{board.name}-{stamp}.jsonl")
    transport = TcpTransport(board.host, board.port, board.connect_timeout_s)
    writer = ResultsWriter(log_path, socket.gethostname(), board.name, now)
    runner = TestRunner(config, transport, CommandClient(transport, LineCodec()),
                        writer, now, on_result)
    return Session(config, runner, log_path, transport, writer)
