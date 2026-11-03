"""Headless entry point: run the configured tests without the GUI.

Usage: python3 -m hosttest.cli --config config/hosttest.json [--test ID ...] [--csv]
Exit status: 0 all passed, 1 a test or the connection failed, 2 bad usage or config.
"""
from __future__ import annotations

import argparse
import os
import sys

from .config import ConfigError
from .results import export_csv
from .runner import all_passed
from .session import build_session


def print_record(record: dict) -> None:
    reason = f"  ({record['reason']})" if record["reason"] else ""
    print(f"{record['result'].upper():4} {record['check']}{reason}")


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(prog="hosttest")
    parser.add_argument("--config", required=True)
    parser.add_argument("--test", action="append", dest="tests",
                        help="run only this test id (repeatable)")
    parser.add_argument("--csv", action="store_true",
                        help="also write the run as CSV next to the JSONL log")
    args = parser.parse_args(argv)

    try:
        session = build_session(args.config, on_result=print_record)
    except ConfigError as exc:
        print(f"config error: {exc}", file=sys.stderr)
        return 2
    known = {t.id for t in session.config.tests}
    unknown = [t for t in (args.tests or []) if t not in known]
    if unknown:
        print(f"unknown test id(s): {', '.join(unknown)}", file=sys.stderr)
        return 2

    try:
        records = session.runner.run(args.tests)
    finally:
        session.close()
    if args.csv and os.path.exists(session.log_path):
        export_csv(session.log_path, session.log_path[:-len(".jsonl")] + ".csv")
    print(f"log: {session.log_path}")
    return 0 if all_passed(records) else 1


if __name__ == "__main__":
    sys.exit(main())
