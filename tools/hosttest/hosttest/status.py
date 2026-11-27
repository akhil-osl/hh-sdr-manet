"""Link status: what the last board reads said, and how old they are.

No Tkinter and no board I/O here. A read is never shown as live once it is
old or the last attempt failed: view() flags such values as stale.

The board reports counters, not a link flag. The link is "up" when the good
frame count rose since the previous good read, "down" when it did not, and
"unknown" when there is nothing to compare (first read, or the counter went
backwards after a restart). link_status is shown but not used for the verdict:
it stayed 7 with the far transmitter off (docs/BOARD-RUNBOOK.md, section 5).

TODO: the number of RF heads is not defined (U-03), so one head is shown.
TODO: no board source for neighbours exists (U-03), so none are shown.
"""
from __future__ import annotations

from dataclasses import dataclass, field

LINK_UP = "up"
LINK_DOWN = "down"
LINK_UNKNOWN = "unknown"
STALE_AFTER_S = 5.0
RF_HEAD = 0
NEIGHBOURS_NOTE = "Not available: no board source reports neighbours yet (U-03)."

COUNTER_KEYS = ("crc_ok_count", "crc_bad_count", "link_status")


@dataclass(frozen=True)
class Reading:
    """One good read: the three board values and the link verdict from the previous read."""
    crc_ok: int
    crc_bad: int
    lock: int
    link: str


@dataclass(frozen=True)
class Part:
    """The last good reading, when it was read, and the last error."""
    value: Reading | None = None
    read_at: float | None = None
    error: str = ""


@dataclass(frozen=True)
class HeadRow:
    head: int
    link: str
    crc_ok: int
    crc_bad: int
    lock: int
    age_s: float
    stale: bool


@dataclass(frozen=True)
class StatusView:
    heads: list = field(default_factory=list)
    error: str = ""


class StatusModel:
    """What the last reads said. Times are the caller's monotonic seconds."""

    def __init__(self, stale_after_s: float = STALE_AFTER_S) -> None:
        self.stale_after_s = stale_after_s
        self._part = Part()

    def clear(self) -> None:
        self._part = Part()

    def set_reading(self, at: float, counters: dict | None, error: str = "") -> None:
        """Record one read: the counters by name, or the error. A failure keeps the old value."""
        old = self._part
        if counters is not None and not error:
            try:
                ok, bad, lock = (int(counters[k]) for k in COUNTER_KEYS)
            except (KeyError, TypeError, ValueError):
                error = "reply is missing a counter"
            else:
                self._part = Part(Reading(ok, bad, lock, _link(old.value, ok)), at, "")
                return
        self._part = Part(old.value, old.read_at, error or "no reply")

    def view(self, now: float) -> StatusView:
        """Rows for the display. A value is stale after a failed read or when old."""
        part = self._part
        if part.value is None:
            return StatusView([], part.error)
        age = max(0.0, now - part.read_at)
        stale = bool(part.error) or age > self.stale_after_s
        v = part.value
        return StatusView([HeadRow(RF_HEAD, v.link, v.crc_ok, v.crc_bad, v.lock, age, stale)],
                          part.error)


def _link(previous: Reading | None, crc_ok: int) -> str:
    """Link verdict from the good frame count against the previous good read."""
    if previous is None or crc_ok < previous.crc_ok:
        return LINK_UNKNOWN
    return LINK_UP if crc_ok > previous.crc_ok else LINK_DOWN
