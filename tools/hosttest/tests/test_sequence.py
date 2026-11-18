import json
import os
import tempfile
import unittest
from datetime import datetime, timedelta, timezone

from fake_board import FakeBoard, config_doc, example_handler, line_script
from hosttest.cli import main as cli_main
from hosttest.codec import LineCodec
from hosttest.command import CommandClient
from hosttest.config import ConfigError, load_config
from hosttest.results import ResultsWriter
from hosttest.sequence import SequenceRunner, judge_limit, load_sequence
from hosttest.transport import TcpTransport


def step(id="s1", value_min=40, value_max=45, **extra):
    doc = {"id": id, "command": "EXAMPLE_READ", "args": ["ch1"],
           "expect": {"status": "OK"}, "timeout_s": 1.0, "field": "value",
           "unit": "dBm", "limit": {"min": value_min, "max": value_max},
           "limit_source": "clause X", "uncertainty": 0.5}
    doc.update(extra)
    return doc


def seq(*steps):
    return {"name": "t", "version": "1", "steps": list(steps)}


class SequenceTest(unittest.TestCase):
    def setUp(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.tmp = tmp.name

    def write(self, name, doc):
        path = os.path.join(self.tmp, name)
        with open(path, "w", encoding="utf-8") as fh:
            json.dump(doc, fh)
        return path

    def load(self, doc):
        return load_sequence(self.write("s.json", doc))

    def run_seq(self, doc, handler=example_handler, serial="SN-1"):
        board = FakeBoard(line_script(handler))
        self.addCleanup(board.stop)
        config = load_config(self.write("c.json", config_doc(board.port)))
        ticks = iter(range(10000))
        now = lambda: datetime(2026, 11, 12, tzinfo=timezone.utc) + timedelta(
            milliseconds=next(ticks))
        link = TcpTransport("127.0.0.1", board.port, 1.0, sleep=lambda s: None)
        self.addCleanup(link.close)
        writer = ResultsWriter(os.path.join(self.tmp, "run.jsonl"), "pc", "fake", now)
        self.addCleanup(writer.close)
        if serial:
            writer.set_unit_serial(serial)
        runner = SequenceRunner(config, link, CommandClient(link, LineCodec()),
                                writer, now)
        return runner.run_sequence(self.load(doc))

    def test_pass_logs_measured_limit_source_uncertainty_and_serial(self):
        records = self.run_seq(seq(step()))
        self.assertEqual([r["check"] for r in records],
                         ["connect", "identity", "s1"])
        rec = records[2]
        self.assertEqual(rec["result"], "pass")
        self.assertEqual(rec["metrics"]["measured"], 42.0)
        self.assertEqual(rec["metrics"]["unit"], "dBm")
        self.assertEqual(rec["params"]["limit"], {"min": 40.0, "max": 45.0})
        self.assertEqual(rec["params"]["limit_source"], "clause X")
        self.assertEqual(rec["params"]["uncertainty"], 0.5)
        self.assertTrue(all(r["unit_serial"] == "SN-1" for r in records))
        self.assertTrue(rec["started_utc"].endswith("Z"))

    def test_value_outside_limit_fails_with_reason(self):
        rec = self.run_seq(seq(step(value_min=43, value_max=50)))[2]
        self.assertEqual(rec["result"], "fail")
        self.assertIn("below min", rec["reason"])
        self.assertEqual(rec["metrics"]["measured"], 42.0)

    def test_limit_boundary_is_inside(self):
        self.assertEqual(self.run_seq(seq(step(value_min=42, value_max=42)))[2]["result"],
                         "pass")

    def test_stop_on_fail_skips_later_steps(self):
        records = self.run_seq(seq(step("a", 50, 60), step("b")))
        self.assertEqual([r["check"] for r in records][2:], ["a"])

    def test_continue_on_fail_runs_later_steps(self):
        records = self.run_seq(seq(step("a", 50, 60, on_fail="continue"), step("b")))
        self.assertEqual([(r["check"], r["result"]) for r in records][2:],
                         [("a", "fail"), ("b", "pass")])

    def test_non_numeric_and_nan_replies_fail(self):
        for text in ("abc", "nan"):
            handler = lambda line, t=text: [f"{line.split()[0]} OK value={t}\n"]
            rec = self.run_seq(seq(step()), handler)[-1]
            self.assertEqual(rec["result"], "fail", text)
            self.assertIn("not a number", rec["reason"])

    def test_command_mismatch_fails_without_measurement(self):
        rec = self.run_seq(seq(step(expect={"status": "NO"})))[2]
        self.assertEqual(rec["result"], "fail")
        self.assertIsNone(rec["metrics"]["measured"])

    def test_serial_defaults_to_unknown(self):
        records = self.run_seq(seq(step()), serial=None)
        self.assertEqual({r["unit_serial"] for r in records}, {"UNKNOWN"})

    def test_loader_rejects_traceability_gaps(self):
        bad = [
            seq({k: v for k, v in step().items() if k != "limit_source"}),
            seq(step(limit_source="  ")),
            seq({k: v for k, v in step().items() if k != "uncertainty"}),
            seq(step(uncertainty=-1)),
            seq({k: v for k, v in step().items() if k != "limit"}),
            seq(step(limit={})),
            seq(step(value_min=5, value_max=1)),
            seq(step(on_fail="ignore")),
            seq(step("a"), step("a")),
            seq(),
        ]
        for doc in bad:
            with self.assertRaises(ConfigError, msg=str(doc)[:80]):
                self.load(doc)

    def test_judge_limit_one_sided(self):
        s = self.load(seq(step(limit={"max": 10}))).steps[0]
        self.assertEqual(judge_limit(s, -100), "")
        self.assertIn("above max", judge_limit(s, 11))

    def test_cli_requires_serial_and_runs_sequence(self):
        board = FakeBoard(line_script(example_handler))
        self.addCleanup(board.stop)
        cfg = config_doc(board.port, log_dir=os.path.join(self.tmp, "logs"))
        cfg_path = self.write("c.json", cfg)
        seq_path = self.write("s.json", seq(step()))
        self.assertEqual(cli_main(["--config", cfg_path, "--sequence", seq_path]), 2)
        self.assertEqual(cli_main(["--config", cfg_path, "--sequence", seq_path,
                                   "--serial", "SN-9"]), 0)
        logs = os.listdir(os.path.join(self.tmp, "logs"))
        with open(os.path.join(self.tmp, "logs", logs[0]), encoding="utf-8") as fh:
            lines = [json.loads(x) for x in fh]
        self.assertEqual({r["unit_serial"] for r in lines}, {"SN-9"})


if __name__ == "__main__":
    unittest.main()
