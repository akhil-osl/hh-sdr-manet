import json
import os
import tempfile
import unittest

from fake_board import closed_port, config_doc
from hosttest.config import ConfigError
from hosttest.session import build_session


class SessionTest(unittest.TestCase):
    def setUp(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.tmp = tmp.name

    def config(self, doc):
        path = os.path.join(self.tmp, "c.json")
        with open(path, "w", encoding="utf-8") as fh:
            json.dump(doc, fh)
        return path

    def test_building_a_session_creates_no_files(self):
        log_dir = os.path.join(self.tmp, "results")
        session = build_session(self.config(config_doc(closed_port(), log_dir)))
        session.close()
        self.assertFalse(os.path.exists(log_dir))

    def test_log_is_created_by_the_first_record(self):
        log_dir = os.path.join(self.tmp, "results")
        session = build_session(self.config(config_doc(closed_port(), log_dir, 1)))
        self.addCleanup(session.close)
        session.runner.connect()
        self.assertTrue(os.path.exists(session.log_path))

    def test_bad_config_raises(self):
        with self.assertRaises(ConfigError):
            build_session("/nonexistent.json")


if __name__ == "__main__":
    unittest.main()
