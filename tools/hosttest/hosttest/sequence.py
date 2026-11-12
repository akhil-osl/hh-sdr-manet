"""Test sequence: file loader and runner.

A sequence file lists measurement steps. Each step sends one command to the
board, reads one numeric reply field and checks it against a limit. The limit,
its source clause and its uncertainty come only from the file, never from code,
so a pass can be traced back to the calibration and uncertainty budget.

TODO: only radio (board) steps exist. Bench instrument steps wait for an
instrument control contract; see the host test entry in unknown.md.
"""
from __future__ import annotations

import json
import math
from dataclasses import dataclass
from datetime import datetime

from .command import FAIL, PASS
from .config import ConfigError, _command_fields, _require
from .runner import TestRunner

STOP = "stop"
CONTINUE = "continue"


@dataclass(frozen=True)
class Step:
    id: str
    command: str
    args: list
    expect: dict
    timeout_s: float
    field: str
    unit: str
    min: float | None
    max: float | None
    limit_source: str
    uncertainty: float
    on_fail: str


@dataclass(frozen=True)
class Sequence:
    name: str
    version: str
    steps: list


def _number(obj: dict, key: str, where: str) -> float:
    return float(_require(obj, key, (int, float), where))


def _step(raw: object, index: int, path: str) -> Step:
    where = f"{path} steps[{index}]"
    if not isinstance(raw, dict):
        raise ConfigError(f"{where}: must be an object")
    command, args, expect, timeout_s = _command_fields(raw, where)
    limit = _require(raw, "limit", dict, where)
    bounds = {k: _number(limit, k, f"{where} limit") for k in ("min", "max")
              if k in limit}
    if not bounds or set(limit) - {"min", "max"}:
        raise ConfigError(f"{where}: 'limit' needs 'min' and/or 'max' only")
    if len(bounds) == 2 and bounds["min"] > bounds["max"]:
        raise ConfigError(f"{where}: limit min is above max")
    source = _require(raw, "limit_source", str, where)
    if not source.strip():
        raise ConfigError(f"{where}: 'limit_source' must not be empty")
    uncertainty = _number(raw, "uncertainty", where)
    if uncertainty < 0:
        raise ConfigError(f"{where}: 'uncertainty' must not be negative")
    on_fail = raw.get("on_fail", STOP)
    if on_fail not in (STOP, CONTINUE):
        raise ConfigError(f"{where}: 'on_fail' must be '{STOP}' or '{CONTINUE}'")
    return Step(
        id=_require(raw, "id", str, where), command=command, args=args,
        expect=expect, timeout_s=timeout_s,
        field=_require(raw, "field", str, where),
        unit=_require(raw, "unit", str, where),
        min=bounds.get("min"), max=bounds.get("max"),
        limit_source=source, uncertainty=uncertainty, on_fail=on_fail)


def load_sequence(path: str) -> Sequence:
    try:
        with open(path, encoding="utf-8") as fh:
            doc = json.load(fh)
    except (OSError, json.JSONDecodeError) as exc:
        raise ConfigError(f"{path}: {exc}") from exc
    if not isinstance(doc, dict):
        raise ConfigError(f"{path}: top level must be an object")
    raw_steps = _require(doc, "steps", list, path)
    steps, seen = [], set()
    for index, raw in enumerate(raw_steps):
        step = _step(raw, index, path)
        if step.id in seen:
            raise ConfigError(f"{path} steps[{index}]: duplicate id '{step.id}'")
        seen.add(step.id)
        steps.append(step)
    if not steps:
        raise ConfigError(f"{path}: 'steps' must not be empty")
    return Sequence(name=_require(doc, "name", str, path),
                    version=_require(doc, "version", str, path), steps=steps)


def judge_limit(step: Step, value: float) -> str:
    """Empty string when `value` is inside the limit, else the reason."""
    if step.min is not None and value < step.min:
        return f"{value} {step.unit} is below min {step.min}"
    if step.max is not None and value > step.max:
        return f"{value} {step.unit} is above max {step.max}"
    return ""


class SequenceRunner(TestRunner):
    """Runs a sequence on the same link, identity and log as TestRunner."""

    def run_sequence(self, sequence: Sequence) -> list[dict]:
        """Run the steps in order. Returns the records written.

        A failed step ends the run when its on_fail is 'stop'. The steps after
        it are not run and not logged.
        """
        self._cancel.clear()
        records: list[dict] = []

        def emit(record: dict) -> None:
            records.append(record)
            self._on_result(record)

        for step in sequence.steps:
            if self._cancel.is_set():
                break
            if not self._transport.is_open and not self._connect(emit):
                break
            started = self._now()
            verdict, reason, measured, res = self._measure(step)
            emit(self._writer.write(
                step.id, verdict, reason, started, tx=res.tx, rx=res.rx,
                params={"sequence": sequence.name,
                        "sequence_version": sequence.version,
                        "command": step.command, "args": step.args,
                        "expect": step.expect, "tag": res.tag,
                        "timeout_s": step.timeout_s, "field": step.field,
                        "limit": {"min": step.min, "max": step.max},
                        "limit_source": step.limit_source,
                        "uncertainty": step.uncertainty,
                        "on_fail": step.on_fail},
                metrics={"measured": measured, "unit": step.unit,
                         "elapsed_s": round(res.elapsed_s, 6),
                         "discarded_lines": res.discarded}))
            if verdict == FAIL and step.on_fail == STOP:
                break
        return records

    def _measure(self, step: Step):
        """Send the step's command and judge the reply and the measured value."""
        res = self._client.execute(step.command, step.args, step.expect,
                                   step.timeout_s)
        if res.verdict != PASS:
            return FAIL, res.reason, None, res
        raw = res.fields.get(step.field)
        try:
            measured = float(raw)
        except (TypeError, ValueError):
            measured = math.nan
        if not math.isfinite(measured):
            return FAIL, f"field '{step.field}' is not a number: {raw!r}", None, res
        reason = judge_limit(step, measured)
        return (FAIL if reason else PASS), reason, measured, res
