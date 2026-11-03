"""Results layer: one JSON line per command, with board identity and time.

Schema hh-atp-evidence/2 extends hh-atp-evidence/1 (tools/atp) with the board
identity at the top level and the command text and reply text.
"""
from __future__ import annotations

import csv
import json
import os
from datetime import datetime
from typing import Callable

SCHEMA = "hh-atp-evidence/2"
UNKNOWN = "UNKNOWN"
CSV_COLUMNS = ["started_utc", "finished_utc", "board_name", "board_id",
               "firmware", "host", "check", "result", "reason", "tx", "rx"]


def utc_text(moment: datetime) -> str:
    return moment.isoformat(timespec="milliseconds").replace("+00:00", "Z")


class ResultsWriter:
    """Appends records to `path`. The file is created on the first record."""

    def __init__(self, path: str, host: str, board_name: str,
                 now: Callable[[], datetime]) -> None:
        self.path = path
        self._fh = None
        self._host = host
        self._board_name = board_name
        self._now = now
        self.board_id = UNKNOWN
        self.firmware = UNKNOWN

    def set_identity(self, board_id: str, firmware: str) -> None:
        self.board_id = board_id
        self.firmware = firmware

    def write(self, check: str, result: str, reason: str,
              started: datetime, finished: datetime | None = None,
              tx: str | None = None, rx: str | None = None,
              params: dict | None = None, metrics: dict | None = None) -> dict:
        record = {
            "schema": SCHEMA,
            "check": check,
            "result": result,
            "reason": reason,
            "started_utc": utc_text(started),
            "finished_utc": utc_text(finished or self._now()),
            "time_source": "system",
            "host": self._host,
            "board_name": self._board_name,
            "board_id": self.board_id,
            "firmware": self.firmware,
            "tx": tx,
            "rx": rx,
            "params": params or {},
            "metrics": metrics or {},
        }
        if self._fh is None:
            folder = os.path.dirname(self.path)
            if folder:
                os.makedirs(folder, exist_ok=True)
            self._fh = open(self.path, "a", encoding="utf-8")
        self._fh.write(json.dumps(record, sort_keys=False) + "\n")
        self._fh.flush()
        return record

    def close(self) -> None:
        if self._fh is not None:
            self._fh.close()
            self._fh = None


def export_csv(jsonl_path: str, csv_path: str) -> int:
    """Write the flat columns of a JSONL run to CSV; return the row count."""
    rows = 0
    with open(jsonl_path, encoding="utf-8") as src, \
            open(csv_path, "w", newline="", encoding="utf-8") as dst:
        out = csv.DictWriter(dst, fieldnames=CSV_COLUMNS, extrasaction="ignore")
        out.writeheader()
        for line in src:
            if line.strip():
                out.writerow(json.loads(line))
                rows += 1
    return rows
