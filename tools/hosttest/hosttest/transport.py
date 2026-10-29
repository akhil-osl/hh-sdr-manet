"""Transport layer: a byte link to the test board with deadlines and reconnect.

The transport knows nothing about commands, tags or framing. Every blocking
call has a timeout and reports failure with a typed error.
"""
from __future__ import annotations

import socket
import time
from typing import Callable


class TransportError(Exception):
    """Base class for all transport failures."""


class ConnectFailed(TransportError):
    """The link could not be opened."""


class TransportTimeout(TransportError):
    """No data or no send progress before the deadline."""


class TransportClosed(TransportError):
    """The link is closed, or the peer closed it."""


class TcpTransport:
    def __init__(self, host: str, port: int, connect_timeout_s: float,
                 clock: Callable[[], float] = time.monotonic,
                 sleep: Callable[[float], None] = time.sleep) -> None:
        self._host = host
        self._port = port
        self._connect_timeout_s = connect_timeout_s
        self._clock = clock
        self._sleep = sleep
        self._sock: socket.socket | None = None

    @property
    def is_open(self) -> bool:
        return self._sock is not None

    def connect(self) -> None:
        self.close()
        try:
            sock = socket.create_connection((self._host, self._port),
                                            self._connect_timeout_s)
        except OSError as exc:
            raise ConnectFailed(f"{self._host}:{self._port}: {exc}") from exc
        sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        self._sock = sock

    def connect_with_retry(self, attempts: int, backoff_s: float) -> None:
        """Try up to `attempts` times, doubling the wait after each failure."""
        if attempts < 1:
            raise ValueError("attempts must be at least 1")
        wait_s = backoff_s
        for attempt in range(attempts):
            try:
                self.connect()
                return
            except ConnectFailed:
                if attempt == attempts - 1:
                    raise
                self._sleep(wait_s)
                wait_s *= 2

    def send(self, payload: bytes, timeout_s: float) -> None:
        sock = self._require_open()
        deadline = self._clock() + timeout_s
        view = memoryview(payload)
        while view:
            remaining = deadline - self._clock()
            if remaining <= 0:
                raise TransportTimeout("send deadline reached")
            sock.settimeout(remaining)
            try:
                sent = sock.send(view)
            except socket.timeout as exc:
                raise TransportTimeout("send deadline reached") from exc
            except OSError as exc:
                self.close()
                raise TransportClosed(f"send failed: {exc}") from exc
            view = view[sent:]

    def receive(self, timeout_s: float, max_bytes: int = 4096) -> bytes:
        """Return whatever bytes arrive first, at most `max_bytes`."""
        sock = self._require_open()
        sock.settimeout(timeout_s)
        try:
            data = sock.recv(max_bytes)
        except socket.timeout as exc:
            raise TransportTimeout("receive deadline reached") from exc
        except OSError as exc:
            self.close()
            raise TransportClosed(f"receive failed: {exc}") from exc
        if not data:
            self.close()
            raise TransportClosed("peer closed the connection")
        return data

    def close(self) -> None:
        if self._sock is not None:
            try:
                self._sock.close()
            finally:
                self._sock = None

    def _require_open(self) -> socket.socket:
        if self._sock is None:
            raise TransportClosed("transport is not open")
        return self._sock
