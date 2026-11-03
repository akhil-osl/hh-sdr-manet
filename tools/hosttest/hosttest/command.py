"""Command layer: send one command, wait for its tagged reply, give a verdict.

This is the only layer the GUI and the CLI call. It never raises for a bad
board: every failure comes back as a failed CommandResult with a reason.
"""
from __future__ import annotations

import itertools
import time
from dataclasses import dataclass, field
from typing import Callable

from .codec import LineCodec, Reply
from .transport import TcpTransport, TransportClosed, TransportTimeout

PASS = "pass"
FAIL = "fail"


@dataclass
class CommandResult:
    verdict: str
    reason: str
    tag: str
    tx: str
    rx: str | None
    fields: dict
    elapsed_s: float
    discarded: list[str] = field(default_factory=list)


class CommandClient:
    def __init__(self, transport: TcpTransport, codec: LineCodec,
                 clock: Callable[[], float] = time.monotonic) -> None:
        self._transport = transport
        self._codec = codec
        self._clock = clock
        self._counter = itertools.count(1)

    def reset(self) -> None:
        """Forget partly received data. Call after the link is reopened."""
        self._codec.reset()

    def execute(self, command: str, args: list[str], expect: dict,
                timeout_s: float) -> CommandResult:
        tag = f"t{next(self._counter)}"
        start = self._clock()
        deadline = start + timeout_s
        discarded: list[str] = []

        def result(verdict, reason, tx="", reply: Reply | None = None):
            return CommandResult(
                verdict=verdict, reason=reason, tag=tag, tx=tx,
                rx=reply.raw if reply else None,
                fields=dict(reply.fields) if reply else {},
                elapsed_s=self._clock() - start, discarded=discarded)

        try:
            payload = self._codec.encode(tag, command, args)
        except ValueError as exc:
            return result(FAIL, str(exc))
        tx = payload.decode("ascii").rstrip("\n")

        try:
            self._transport.send(payload, timeout_s)
            while True:
                remaining = deadline - self._clock()
                if remaining <= 0:
                    raise TransportTimeout("deadline reached")
                data = self._transport.receive(remaining)
                for reply in self._codec.feed(data):
                    if reply.tag == tag:
                        return self._judge(result, tx, reply, expect)
                    discarded.append(reply.raw)
        except TransportTimeout:
            note = f", {len(discarded)} unmatched line(s) discarded" if discarded else ""
            return result(FAIL, f"no reply within {timeout_s}s{note}", tx)
        except TransportClosed as exc:
            return result(FAIL, f"link closed: {exc}", tx)

    @staticmethod
    def _judge(result, tx: str, reply: Reply, expect: dict) -> CommandResult:
        actual = dict(reply.fields)
        actual["status"] = reply.status
        for key, want in expect.items():
            if actual.get(key) != want:
                return result(FAIL, f"expected {key}={want}, got {actual.get(key)!r}",
                              tx, reply)
        return result(PASS, "", tx, reply)
