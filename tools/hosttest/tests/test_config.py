import json
import os
import tempfile
import unittest

from hosttest.config import ConfigError, load_board, load_config

EXAMPLE = os.path.join(os.path.dirname(__file__), "..", "config",
                       "hosttest.example.json")


def write(doc) -> str:
    fd, path = tempfile.mkstemp(suffix=".json")
    with os.fdopen(fd, "w", encoding="utf-8") as fh:
        json.dump(doc, fh)
    return path


def board(**overrides) -> dict:
    base = {"name": "b", "transport": "tcp", "host": "10.0.0.1", "port": 9000,
            "connect_timeout_s": 1.0, "connect_attempts": 2,
            "connect_backoff_s": 0.1}
    base.update(overrides)
    return {"board": base}


class ConfigTest(unittest.TestCase):
    def test_example_file_loads(self):
        cfg = load_board(EXAMPLE)
        self.assertEqual(cfg.transport, "tcp")
        self.assertEqual(cfg.port, 9000)

    def test_missing_field_is_rejected(self):
        doc = board()
        del doc["board"]["host"]
        with self.assertRaisesRegex(ConfigError, "host"):
            load_board(write(doc))

    def test_bad_port_is_rejected(self):
        with self.assertRaises(ConfigError):
            load_board(write(board(port=70000)))

    def test_wrong_type_is_rejected(self):
        with self.assertRaises(ConfigError):
            load_board(write(board(port="9000")))

    def test_unsupported_transport_is_rejected(self):
        with self.assertRaisesRegex(ConfigError, "unsupported"):
            load_board(write(board(transport="serial")))

    def test_missing_file_is_rejected(self):
        with self.assertRaises(ConfigError):
            load_board("/nonexistent/hosttest.json")


def full(**overrides) -> dict:
    doc = board()
    doc["identity"] = {"command": "ID", "expect": {"status": "OK"},
                       "timeout_s": 1.0, "board_id_field": "id",
                       "firmware_field": "fw"}
    doc["tests"] = [{"id": "a", "command": "A", "expect": {"status": "OK"},
                     "timeout_s": 1.0}]
    doc.update(overrides)
    return doc


class TestListConfigTest(unittest.TestCase):
    def test_example_file_loads_with_identity_and_tests(self):
        cfg = load_config(EXAMPLE)
        self.assertEqual(cfg.identity.board_id_field, "id")
        self.assertEqual([t.id for t in cfg.tests],
                         ["example_status", "example_value"])
        self.assertEqual(cfg.tests[1].args, ["channel1"])

    def test_log_dir_has_a_default(self):
        self.assertEqual(load_config(write(full())).log_dir, "hosttest-results")

    def test_duplicate_test_id_is_rejected(self):
        doc = full()
        doc["tests"] = doc["tests"] * 2
        with self.assertRaisesRegex(ConfigError, "duplicate"):
            load_config(write(doc))

    def test_test_without_expect_is_rejected(self):
        doc = full()
        del doc["tests"][0]["expect"]
        with self.assertRaisesRegex(ConfigError, "expect"):
            load_config(write(doc))

    def test_empty_expect_is_rejected(self):
        doc = full()
        doc["tests"][0]["expect"] = {}
        with self.assertRaises(ConfigError):
            load_config(write(doc))

    def test_non_positive_timeout_is_rejected(self):
        doc = full()
        doc["tests"][0]["timeout_s"] = 0
        with self.assertRaises(ConfigError):
            load_config(write(doc))

    def test_missing_identity_is_rejected(self):
        doc = full()
        del doc["identity"]
        with self.assertRaisesRegex(ConfigError, "identity"):
            load_config(write(doc))


if __name__ == "__main__":
    unittest.main()
