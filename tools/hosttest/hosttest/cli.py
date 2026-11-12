"""Headless entry point: run the configured tests without the GUI.

Usage: python3 -m hosttest.cli --config config/hosttest.json [--test ID ...] [--csv]
       python3 -m hosttest.cli --config C --sequence S --serial SN [--csv]
Exit status: 0 all passed, 1 a test or the connection failed, 2 bad usage or config.
"""
from __future__ import annotations

import argparse
import os
import sys

from .config import ConfigError
from .results import export_csv
from .runner import all_passed
from .sequence import load_sequence
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
    parser.add_argument("--sequence",
                        help="run this test sequence file instead of the config tests")
    parser.add_argument("--serial",
                        help="serial number of the unit under test (needed with --sequence)")
    args = parser.parse_args(argv)
    if args.sequence and (not args.serial or not args.serial.strip()):
        print("--serial is required with --sequence", file=sys.stderr)
        return 2
    if args.sequence and args.tests:
        print("--test cannot be used with --sequence", file=sys.stderr)
        return 2

    try:
        sequence = load_sequence(args.sequence) if args.sequence else None
        session = build_session(args.config, on_result=print_record,
                                unit_serial=args.serial)
    except ConfigError as exc:
        print(f"config error: {exc}", file=sys.stderr)
        return 2
    known = {t.id for t in session.config.tests}
    unknown = [t for t in (args.tests or []) if t not in known]
    if unknown:
        print(f"unknown test id(s): {', '.join(unknown)}", file=sys.stderr)
        return 2

    try:
        records = (session.runner.run_sequence(sequence) if sequence
                   else session.runner.run(args.tests))
    finally:
        session.close()
    if args.csv and os.path.exists(session.log_path):
        export_csv(session.log_path, session.log_path[:-len(".jsonl")] + ".csv")
    print(f"log: {session.log_path}")
    return 0 if all_passed(records) else 1


if __name__ == "__main__":
    sys.exit(main())
