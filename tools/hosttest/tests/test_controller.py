import json
import os
import tempfile
import unittest

from fake_board import (FakeBoard, closed_port, config_doc, example_handler,
                        line_script)
from hosttest.config import ConfigError
from hosttest.controller import (CONNECTED, DISCONNECTED, LINK_LOST, Busy,
                                 Controller)


class ControllerTest(unittest.TestCase):
    def setUp(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.tmp = tmp.name
        self.ctl = Controller()
        self.addCleanup(self.ctl.shutdown)

    def config(self, port, attempts=1):
        doc = config_doc(port, os.path.join(self.tmp, "results"), attempts)
        path = os.path.join(self.tmp, "c.json")
        with open(path, "w", encoding="utf-8") as fh:
            json.dump(doc, fh)
        return path

    def board(self, handler=example_handler):
        board = FakeBoard(line_script(handler))
        self.addCleanup(board.stop)
        return board

    def finish(self):
        self.ctl.join(5)
        return self.ctl.poll()

    def test_load_reads_config_without_touching_the_board(self):
        self.ctl.load(self.config(closed_port()))
        self.assertTrue(self.ctl.loaded)
        self.assertEqual(self.ctl.link, DISCONNECTED)
        self.assertFalse(os.path.exists(self.ctl.session.log_path))

    def test_bad_config_raises_and_keeps_the_previous_session(self):
        self.ctl.load(self.config(closed_port()))
        before = self.ctl.session
        with self.assertRaises(ConfigError):
            self.ctl.load("/nonexistent.json")
        self.assertIs(self.ctl.session, before)

    def test_connect_shows_link_and_identity(self):
        self.ctl.load(self.config(self.board().port))
        self.ctl.connect()
        self.assertTrue(self.ctl.running)
        self.finish()
        self.assertFalse(self.ctl.running)
        self.assertEqual(self.ctl.link, CONNECTED)
        self.assertEqual((self.ctl.board_id, self.ctl.firmware), ("B7", "1.2"))

    def test_run_collects_records_and_the_last_result_per_test(self):
        self.ctl.load(self.config(self.board().port))
        self.ctl.run()
        events = self.finish()
        self.assertEqual([e[0] for e in events],
                         ["record"] * 4 + ["done"])
        self.assertEqual(set(self.ctl.last_by_test), {"status", "value"})
        self.assertEqual(self.ctl.last_by_test["value"]["result"], "pass")
        self.assertEqual(self.ctl.link, CONNECTED)

    def test_run_selected_ids_only(self):
        self.ctl.load(self.config(self.board().port))
        self.ctl.run(["value"])
        self.finish()
        self.assertEqual(set(self.ctl.last_by_test), {"value"})

    def test_unreachable_board_leaves_link_disconnected(self):
        self.ctl.load(self.config(closed_port()))
        self.ctl.connect()
        self.finish()
        self.assertEqual(self.ctl.link, DISCONNECTED)
        self.assertEqual(self.ctl.records[0]["result"], "fail")

    def test_link_dropped_during_a_run_is_reported_as_lost(self):
        def script(conn):
            conn.recv(4096)
            conn.sendall(b"t1 OK id=B7 fw=1.2\n")
            conn.recv(4096)

        board = FakeBoard(script)
        self.addCleanup(board.stop)
        self.ctl.load(self.config(board.port))
        self.ctl.run()
        self.finish()
        self.assertEqual(self.ctl.link, LINK_LOST)

    def test_actions_are_refused_while_a_task_runs(self):
        self.ctl.load(self.config(self.board().port))
        self.ctl.run()
        with self.assertRaises(Busy):
            self.ctl.run()
        with self.assertRaises(Busy):
            self.ctl.load(self.config(closed_port()))
        with self.assertRaises(Busy):
            self.ctl.disconnect()
        self.finish()

    def test_disconnect_after_connect(self):
        self.ctl.load(self.config(self.board().port))
        self.ctl.connect()
        self.finish()
        self.ctl.disconnect()
        self.assertEqual(self.ctl.link, DISCONNECTED)
        self.assertFalse(self.ctl.session.runner.is_connected)

    def test_cancel_stops_the_run_between_tests(self):
        gate_open = []

        def handler(line):
            if "EXAMPLE_STATUS" in line:
                self.ctl.cancel()
                gate_open.append(1)
            return example_handler(line)

        self.ctl.load(self.config(self.board(handler).port))
        self.ctl.run()
        self.finish()
        self.assertEqual(set(self.ctl.last_by_test), {"status"})
        self.assertFalse(self.ctl.cancelling)

    def test_run_without_a_loaded_config_is_an_error(self):
        with self.assertRaises(RuntimeError):
            self.ctl.run()


if __name__ == "__main__":
    unittest.main()
