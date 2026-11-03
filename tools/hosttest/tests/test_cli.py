import contextlib
import io
import json
import os
import tempfile
import unittest

from fake_board import (FakeBoard, closed_port, config_doc, example_handler,
                        line_script)
from hosttest import cli


class CliTest(unittest.TestCase):
    def setUp(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.tmp = tmp.name

    def run_cli(self, doc, *extra):
        doc["log_dir"] = os.path.join(self.tmp, "results")
        path = os.path.join(self.tmp, "c.json")
        with open(path, "w", encoding="utf-8") as fh:
            json.dump(doc, fh)
        out, err = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            code = cli.main(["--config", path, *extra])
        return code, out.getvalue(), err.getvalue()

    def logs(self, suffix):
        folder = os.path.join(self.tmp, "results")
        return [os.path.join(folder, f) for f in sorted(os.listdir(folder))
                if f.endswith(suffix)]

    def board(self, handler=example_handler):
        board = FakeBoard(line_script(handler))
        self.addCleanup(board.stop)
        return board

    def test_passing_run_exits_zero_and_writes_the_log(self):
        code, out, _ = self.run_cli(config_doc(self.board().port), "--csv")
        self.assertEqual(code, 0)
        self.assertIn("PASS status", out)
        [jsonl] = self.logs(".jsonl")
        with open(jsonl, encoding="utf-8") as fh:
            checks = [json.loads(l)["check"] for l in fh]
        self.assertEqual(checks, ["connect", "identity", "status", "value"])
        self.assertEqual(len(self.logs(".csv")), 1)

    def test_failing_test_exits_one(self):
        def handler(line):
            tag, command, *_ = line.split()
            return [f"{tag} ERR\n"] if command == "EXAMPLE_STATUS" else example_handler(line)
        code, out, _ = self.run_cli(config_doc(self.board(handler).port))
        self.assertEqual(code, 1)
        self.assertIn("FAIL status", out)

    def test_unreachable_board_exits_one(self):
        code, out, _ = self.run_cli(config_doc(closed_port()))
        self.assertEqual(code, 1)
        self.assertIn("FAIL connect", out)

    def test_test_option_limits_the_run(self):
        code, out, _ = self.run_cli(config_doc(self.board().port), "--test", "value")
        self.assertEqual(code, 0)
        self.assertNotIn("status", out)

    def test_unknown_test_id_exits_two(self):
        code, _, err = self.run_cli(config_doc(self.board().port), "--test", "nope")
        self.assertEqual(code, 2)
        self.assertIn("nope", err)

    def test_bad_config_exits_two(self):
        doc = config_doc(9000)
        del doc["identity"]
        code, _, err = self.run_cli(doc)
        self.assertEqual(code, 2)
        self.assertIn("identity", err)


if __name__ == "__main__":
    unittest.main()
