"""Wire codec: encodes commands, frames the byte stream, decodes replies.

TODO: PLACEHOLDER. The board's command and reply format is not defined. This
line format exists only so the rest of the tool can be built and tested; it
must be replaced by the agreed contract. See the host test entry in unknown.md.

Placeholder format, one ASCII line each way:
    request:  <tag> <command> [arg ...]\\n
    reply:    <tag> <status> [key=value ...]\\n
"""
from __future__ import annotations

from dataclasses import dataclass, field

MAX_PENDING_BYTES = 65536


@dataclass(frozen=True)
class Reply:
    raw: str
    tag: str | None = None      # None when the line could not be parsed
    status: str | None = None
    fields: dict = field(default_factory=dict)


class LineCodec:
    def __init__(self) -> None:
        self._buf = b""

    def reset(self) -> None:
        self._buf = b""

    def encode(self, tag: str, command: str, args: list[str]) -> bytes:
        tokens = [tag, command, *args]
        for token in tokens:
            if not token or any(c.isspace() or ord(c) < 32 for c in token):
                raise ValueError(f"token not encodable: {token!r}")
        return (" ".join(tokens) + "\n").encode("ascii")

    def feed(self, data: bytes) -> list[Reply]:
        """Add received bytes; return every complete reply line now available."""
        self._buf += data
        replies = []
        while b"\n" in self._buf:
            line, self._buf = self._buf.split(b"\n", 1)
            text = line.decode("utf-8", errors="replace").strip()
            if text:
                replies.append(self._parse(text))
        if len(self._buf) > MAX_PENDING_BYTES:
            self._buf = b""
            replies.append(Reply(raw="<line too long, discarded>"))
        return replies

    @staticmethod
    def _parse(text: str) -> Reply:
        parts = text.split()
        if len(parts) < 2:
            return Reply(raw=text)
        fields = {}
        for token in parts[2:]:
            key, sep, value = token.partition("=")
            if not sep or not key:
                return Reply(raw=text)
            fields[key] = value
        return Reply(raw=text, tag=parts[0], status=parts[1], fields=fields)
