"""Serve the placeholder protocol on a port, to try the CLI or GUI without a board.

Usage: python3 tests/run_fake_board.py [--port 9000] [--value 42]
Point config/hosttest.json at 127.0.0.1 and the same port. Stop with Ctrl+C.
Test code only: the commands are the placeholder examples, not a real board's.
"""
import argparse
import os
import sys
import threading

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from fake_board import FakeBoard, example_handler, line_script  # noqa: E402


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=9000)
    parser.add_argument("--value", default="42",
                        help="value returned by EXAMPLE_READ (a test expects 42)")
    args = parser.parse_args()

    def handler(line: str) -> list:
        tag, command, *_ = line.split()
        if command == "EXAMPLE_READ":
            return [f"{tag} OK value={args.value}\n"]
        return example_handler(line)

    board = FakeBoard(line_script(handler), port=args.port, host=args.host)
    print(f"fake board on {args.host}:{board.port}, Ctrl+C to stop")
    try:
        threading.Event().wait()
    except KeyboardInterrupt:
        pass
    finally:
        board.stop()
    return 0


if __name__ == "__main__":
    sys.exit(main())
