import json
import os
import tempfile
import unittest
from datetime import datetime, timedelta, timezone

from fake_board import (FakeBoard, closed_port, config_doc, example_handler,
                        line_script)
from hosttest.codec import LineCodec
from hosttest.command import CommandClient
from hosttest.config import load_config
from hosttest.results import UNKNOWN, ResultsWriter
from hosttest.runner import TestRunner, all_passed
from hosttest.transport import TcpTransport


class RunnerTest(unittest.TestCase):
    def build(self, port, on_result=None, mutate=None):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        doc = config_doc(port)
        if mutate:
            mutate(doc)
        path = os.path.join(tmp.name, "c.json")
        with open(path, "w", encoding="utf-8") as fh:
            json.dump(doc, fh)
        config = load_config(path)
        ticks = iter(range(10000))
        now = lambda: datetime(2026, 10, 29, tzinfo=timezone.utc) + timedelta(
            milliseconds=next(ticks))
        link = TcpTransport("127.0.0.1", port, 1.0, sleep=lambda s: None)
        self.addCleanup(link.close)
        log = os.path.join(tmp.name, "run.jsonl")
        writer = ResultsWriter(log, "pc-1", "fake", now)
        self.addCleanup(writer.close)
        runner = TestRunner(config, link, CommandClient(link, LineCodec()),
                            writer, now, on_result)
        return runner

    def board(self, handler=example_handler):
        board = FakeBoard(line_script(handler))
        self.addCleanup(board.stop)
        return board

    def test_full_run_logs_connect_identity_and_every_test(self):
        runner = self.build(self.board().port)
        records = runner.run()
        self.assertEqual([r["check"] for r in records],
                         ["connect", "identity", "status", "value"])
        self.assertTrue(all_passed(records))
        self.assertEqual({r["board_id"] for r in records[1:]}, {"B7"})
        self.assertEqual({r["firmware"] for r in records[1:]}, {"1.2"})
        self.assertEqual(records[0]["board_id"], UNKNOWN)

    def test_failed_test_is_recorded_and_run_continues(self):
        def handler(line):
            tag, command, *_ = line.split()
            if command == "EXAMPLE_READ":
                return [f"{tag} OK value=7\n"]
            return example_handler(line)
        records = self.build(self.board(handler).port).run()
        self.assertFalse(all_passed(records))
        self.assertEqual([r["result"] for r in records],
                         ["pass", "pass", "pass", "fail"])
        self.assertIn("value=42", records[3]["reason"])

    def test_identity_failure_leaves_unknown_but_tests_still_run(self):
        def handler(line):
            tag, command, *_ = line.split()
            if command == "EXAMPLE_ID":
                return [f"{tag} ERR\n"]
            return example_handler(line)
        records = self.build(self.board(handler).port).run()
        by_check = {r["check"]: r for r in records}
        self.assertEqual(by_check["identity"]["result"], "fail")
        self.assertEqual(by_check["status"]["board_id"], UNKNOWN)
        self.assertEqual(by_check["status"]["result"], "pass")

    def test_identity_reply_without_firmware_field_fails(self):
        def handler(line):
            tag, command, *_ = line.split()
            if command == "EXAMPLE_ID":
                return [f"{tag} OK id=B7\n"]
            return example_handler(line)
        records = self.build(self.board(handler).port).run()
        self.assertIn("lacks", records[1]["reason"])

    def test_unreachable_board_logs_connect_failure_and_stops(self):
        records = self.build(closed_port()).run()
        self.assertEqual([(r["check"], r["result"]) for r in records],
                         [("connect", "fail")])
        self.assertFalse(all_passed(records))

    def test_link_drop_fails_that_test_then_reconnects_for_the_next(self):
        state = {"n": 0}

        def script(conn):
            state["n"] += 1
            if state["n"] == 1:
                conn.recv(4096)
                conn.sendall(b"t1 OK id=B7 fw=1.2\n")
                conn.recv(4096)
                return
            line_script(example_handler)(conn)

        board = FakeBoard(script)
        self.addCleanup(board.stop)
        records = self.build(board.port).run()
        self.assertEqual([(r["check"], r["result"]) for r in records],
                         [("connect", "pass"), ("identity", "pass"),
                          ("status", "fail"), ("connect", "pass"),
                          ("identity", "pass"), ("value", "pass")])
        self.assertIn("link closed", records[2]["reason"])

    def test_connect_reads_identity_and_disconnect_closes_the_link(self):
        runner = self.build(self.board().port)
        records = runner.connect()
        self.assertEqual([(r["check"], r["result"]) for r in records],
                         [("connect", "pass"), ("identity", "pass")])
        self.assertTrue(runner.is_connected)
        runner.disconnect()
        self.assertFalse(runner.is_connected)

    def test_run_after_connect_does_not_connect_again(self):
        runner = self.build(self.board().port)
        runner.connect()
        records = runner.run()
        self.assertEqual([r["check"] for r in records], ["status", "value"])

    def test_selected_ids_only(self):
        records = self.build(self.board().port).run(["value"])
        self.assertEqual([r["check"] for r in records],
                         ["connect", "identity", "value"])

    def test_cancel_stops_before_the_next_test(self):
        holder = {}

        def on_result(record):
            if record["check"] == "status":
                holder["runner"].cancel()

        runner = self.build(self.board().port, on_result)
        holder["runner"] = runner
        records = runner.run()
        self.assertEqual([r["check"] for r in records],
                         ["connect", "identity", "status"])

    def test_result_callback_sees_every_record(self):
        seen = []
        records = self.build(self.board().port, seen.append).run()
        self.assertEqual(seen, records)


if __name__ == "__main__":
    unittest.main()
