"""Scripted TCP server standing in for a test board."""
from __future__ import annotations

import socket
import threading
from typing import Callable


class FakeBoard:
    """Listens on a free loopback port and runs `script(conn)` per connection."""

    def __init__(self, script: Callable[[socket.socket], None],
                 port: int = 0, host: str = "127.0.0.1") -> None:
        self._script = script
        self._listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self._listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self._listener.bind((host, port))
        self._listener.listen(4)
        self._listener.settimeout(0.1)
        self.port = self._listener.getsockname()[1]
        self._stop = threading.Event()
        self._thread = threading.Thread(target=self._serve, daemon=True)
        self._thread.start()

    def _serve(self) -> None:
        while not self._stop.is_set():
            try:
                conn, _ = self._listener.accept()
            except socket.timeout:
                continue
            except OSError:
                return
            self._conn = conn
            with conn:
                try:
                    self._script(conn)
                except OSError:
                    pass

    def stop(self) -> None:
        self._stop.set()
        self._listener.close()
        conn = getattr(self, "_conn", None)
        if conn is not None:
            try:
                conn.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass
        self._thread.join(timeout=2)

    def __enter__(self) -> "FakeBoard":
        return self

    def __exit__(self, *exc) -> None:
        self.stop()


def closed_port() -> int:
    """A loopback port with nothing listening on it."""
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


def line_script(handler: Callable[[str], list | None]):
    """Script for a line-oriented board: handler(request_line) -> lines to send.

    The handler returns None or [] to stay silent. Items may be str or bytes,
    and are sent exactly as given, so a test can send bad or split replies.
    """
    def script(conn: socket.socket) -> None:
        pending = b""
        while True:
            data = conn.recv(4096)
            if not data:
                return
            pending += data
            while b"\n" in pending:
                line, pending = pending.split(b"\n", 1)
                for out in handler(line.decode()) or []:
                    conn.sendall(out if isinstance(out, bytes) else out.encode())
    return script


def example_handler(line: str) -> list:
    """A well-behaved board for the placeholder codec."""
    tag, command, *_ = line.split()
    if command == "EXAMPLE_ID":
        return [f"{tag} OK id=B7 fw=1.2\n"]
    if command == "EXAMPLE_STATUS":
        return [f"{tag} OK\n"]
    if command == "EXAMPLE_READ":
        return [f"{tag} OK value=42\n"]
    return [f"{tag} ERR\n"]


def radioctl_text(crc_ok: int, crc_bad: int, link_status: int) -> str:
    """What the board's three `radioctl opencpi get` calls print, as in the runbook."""
    out = []
    for worker, prop, value, kind in (("rx_ber", "crc_ok_count", crc_ok, "ulonglong"),
                                      ("rx_ber", "crc_bad_count", crc_bad, "ulonglong"),
                                      ("rx_fts", "link_status", link_status, "uchar")):
        out.append(f"worker={worker}\nproperty={prop}\nvalue={value}\n"
                   f"type={kind}\naccess=ro\n")
    return "".join(out)


class FakeSsh:
    """Stands in for subprocess.run of ssh. Test code sets what the board "says".

    crc_ok rises by `step` on each call while `frames_flow` is true. Set `fail`
    to a string for an ssh error, `hang` to make the call time out.
    """

    def __init__(self) -> None:
        self.crc_ok, self.crc_bad, self.link_status, self.step = 0, 0, 7, 10
        self.frames_flow = True
        self.fail = ""
        self.hang = False
        self.calls: list[list] = []

    def __call__(self, argv, **kwargs):
        import subprocess
        self.calls.append(argv)
        if self.hang:
            raise subprocess.TimeoutExpired(argv, kwargs.get("timeout", 0))
        if self.fail:
            return subprocess.CompletedProcess(argv, 255, "", self.fail + "\n")
        if self.frames_flow:
            self.crc_ok += self.step
        text = radioctl_text(self.crc_ok, self.crc_bad, self.link_status)
        return subprocess.CompletedProcess(argv, 0, text, "")


def config_doc(port: int, log_dir: str = "results", attempts: int = 2) -> dict:
    """Config for a board on loopback, in the shape load_config expects."""
    return {
        "board": {"name": "fake", "transport": "tcp", "host": "127.0.0.1",
                  "port": port, "connect_timeout_s": 1.0,
                  "connect_attempts": attempts, "connect_backoff_s": 0.0},
        "log_dir": log_dir,
        "status": {"ssh_target": "root@192.0.2.10",
                   "radioctl_dir": "/opt/radhawk_each_worker_status"},
        "identity": {"command": "EXAMPLE_ID", "expect": {"status": "OK"},
                     "timeout_s": 1.0, "board_id_field": "id",
                     "firmware_field": "fw"},
        "tests": [
            {"id": "status", "command": "EXAMPLE_STATUS",
             "expect": {"status": "OK"}, "timeout_s": 1.0},
            {"id": "value", "command": "EXAMPLE_READ", "args": ["ch1"],
             "expect": {"status": "OK", "value": "42"}, "timeout_s": 1.0},
        ],
    }
