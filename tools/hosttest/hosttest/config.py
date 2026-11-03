"""Configuration loader. Board addresses and the test list come only from JSON."""
from __future__ import annotations

import json
from dataclasses import dataclass


class ConfigError(Exception):
    """The configuration file is missing, unreadable or invalid."""


@dataclass(frozen=True)
class BoardConfig:
    name: str
    transport: str
    host: str
    port: int
    connect_timeout_s: float
    connect_attempts: int
    connect_backoff_s: float


@dataclass(frozen=True)
class TestConfig:
    __test__ = False  # not a unittest case

    id: str
    command: str
    args: list
    expect: dict
    timeout_s: float


@dataclass(frozen=True)
class IdentityConfig:
    command: str
    args: list
    expect: dict
    timeout_s: float
    board_id_field: str
    firmware_field: str


@dataclass(frozen=True)
class Config:
    board: BoardConfig
    identity: IdentityConfig
    tests: list
    log_dir: str


def _require(obj: dict, key: str, kind, where: str):
    if key not in obj:
        raise ConfigError(f"{where}: missing '{key}'")
    value = obj[key]
    if isinstance(value, bool) or not isinstance(value, kind):
        raise ConfigError(f"{where}: '{key}' has the wrong type")
    return value


def _positive(value: float, key: str, where: str) -> float:
    if value <= 0:
        raise ConfigError(f"{where}: '{key}' must be positive")
    return float(value)


def _command_fields(obj: dict, where: str):
    command = _require(obj, "command", str, where)
    args = obj.get("args", [])
    if not isinstance(args, list) or not all(isinstance(a, str) for a in args):
        raise ConfigError(f"{where}: 'args' must be a list of strings")
    expect = _require(obj, "expect", dict, where)
    if not expect or not all(isinstance(k, str) and isinstance(v, str)
                             for k, v in expect.items()):
        raise ConfigError(f"{where}: 'expect' must be a non-empty string map")
    timeout_s = _positive(_require(obj, "timeout_s", (int, float), where),
                          "timeout_s", where)
    return command, args, expect, timeout_s


def _read(path: str) -> dict:
    try:
        with open(path, encoding="utf-8") as fh:
            doc = json.load(fh)
    except (OSError, json.JSONDecodeError) as exc:
        raise ConfigError(f"{path}: {exc}") from exc
    if not isinstance(doc, dict):
        raise ConfigError(f"{path}: top level must be an object")
    return doc


def _board(doc: dict, path: str) -> BoardConfig:
    if not isinstance(doc.get("board"), dict):
        raise ConfigError(f"{path}: missing 'board' object")
    board = doc["board"]
    where = f"{path} board"
    transport = _require(board, "transport", str, where)
    if transport != "tcp":
        raise ConfigError(f"{where}: unsupported transport '{transport}'")
    port = _require(board, "port", int, where)
    if not 1 <= port <= 65535:
        raise ConfigError(f"{where}: 'port' out of range")
    timeout_s = _positive(_require(board, "connect_timeout_s", (int, float), where),
                          "connect_timeout_s", where)
    attempts = _require(board, "connect_attempts", int, where)
    backoff_s = float(_require(board, "connect_backoff_s", (int, float), where))
    if attempts < 1 or backoff_s < 0:
        raise ConfigError(f"{where}: attempts or backoff out of range")
    return BoardConfig(
        name=_require(board, "name", str, where),
        transport=transport,
        host=_require(board, "host", str, where),
        port=port,
        connect_timeout_s=timeout_s,
        connect_attempts=attempts,
        connect_backoff_s=backoff_s,
    )


def load_board(path: str) -> BoardConfig:
    return _board(_read(path), path)


def load_config(path: str) -> Config:
    doc = _read(path)
    board = _board(doc, path)

    ident = _require(doc, "identity", dict, path)
    where = f"{path} identity"
    command, args, expect, timeout_s = _command_fields(ident, where)
    identity = IdentityConfig(
        command=command, args=args, expect=expect, timeout_s=timeout_s,
        board_id_field=_require(ident, "board_id_field", str, where),
        firmware_field=_require(ident, "firmware_field", str, where))

    raw_tests = _require(doc, "tests", list, path)
    tests, seen = [], set()
    for index, raw in enumerate(raw_tests):
        where = f"{path} tests[{index}]"
        if not isinstance(raw, dict):
            raise ConfigError(f"{where}: must be an object")
        test_id = _require(raw, "id", str, where)
        if test_id in seen:
            raise ConfigError(f"{where}: duplicate id '{test_id}'")
        seen.add(test_id)
        command, args, expect, timeout_s = _command_fields(raw, where)
        tests.append(TestConfig(test_id, command, args, expect, timeout_s))

    log_dir = doc.get("log_dir", "hosttest-results")
    if not isinstance(log_dir, str) or not log_dir:
        raise ConfigError(f"{path}: 'log_dir' must be a non-empty string")
    return Config(board=board, identity=identity, tests=tests, log_dir=log_dir)
