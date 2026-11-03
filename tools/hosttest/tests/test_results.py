import csv
import json
import os
import tempfile
import unittest
from datetime import datetime, timedelta, timezone

from hosttest.results import SCHEMA, UNKNOWN, ResultsWriter, export_csv

T0 = datetime(2026, 10, 29, 6, 30, 0, tzinfo=timezone.utc)


class ResultsTest(unittest.TestCase):
    def setUp(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.path = os.path.join(tmp.name, "run.jsonl")
        self.writer = ResultsWriter(self.path, "pc-1", "bench-1", lambda: T0)
        self.addCleanup(self.writer.close)

    def lines(self):
        with open(self.path, encoding="utf-8") as fh:
            return [json.loads(l) for l in fh if l.strip()]

    def test_record_carries_identity_time_and_command_text(self):
        self.writer.set_identity("B7", "1.2")
        self.writer.write("status", "pass", "", T0, T0 + timedelta(milliseconds=5),
                          tx="t1 STATUS", rx="t1 OK", params={"tag": "t1"})
        [rec] = self.lines()
        self.assertEqual(rec["schema"], SCHEMA)
        self.assertEqual((rec["board_id"], rec["firmware"]), ("B7", "1.2"))
        self.assertEqual((rec["host"], rec["board_name"]), ("pc-1", "bench-1"))
        self.assertEqual(rec["started_utc"], "2026-10-29T06:30:00.000Z")
        self.assertEqual(rec["finished_utc"], "2026-10-29T06:30:00.005Z")
        self.assertEqual((rec["tx"], rec["rx"]), ("t1 STATUS", "t1 OK"))

    def test_identity_is_unknown_until_read(self):
        self.writer.write("connect", "pass", "", T0)
        [rec] = self.lines()
        self.assertEqual((rec["board_id"], rec["firmware"]), (UNKNOWN, UNKNOWN))

    def test_each_record_is_on_disk_before_the_writer_closes(self):
        self.writer.write("a", "pass", "", T0)
        self.writer.write("b", "fail", "why", T0)
        self.assertEqual([r["check"] for r in self.lines()], ["a", "b"])

    def test_csv_export_has_one_row_per_record(self):
        self.writer.set_identity("B7", "1.2")
        self.writer.write("a", "pass", "", T0, tx="t1 A", rx="t1 OK")
        self.writer.write("b", "fail", "why", T0)
        out = self.path[:-6] + ".csv"
        self.assertEqual(export_csv(self.path, out), 2)
        with open(out, newline="", encoding="utf-8") as fh:
            rows = list(csv.DictReader(fh))
        self.assertEqual([r["check"] for r in rows], ["a", "b"])
        self.assertEqual(rows[1]["reason"], "why")
        self.assertEqual(rows[0]["board_id"], "B7")


if __name__ == "__main__":
    unittest.main()
