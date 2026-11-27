import json
import os
import tempfile
import time
import unittest

from fake_board import FakeSsh, config_doc
from hosttest.board_ssh import SshStatusSource
from hosttest.controller import Busy, Controller, StatusNotConfigured
from hosttest.status import StatusModel


def counters(ok, bad=0, lock=7):
    return {"crc_ok_count": ok, "crc_bad_count": bad, "link_status": lock}


class ModelTest(unittest.TestCase):
    def view(self, m, now):
        return m.view(now).heads[0]

    def test_first_read_has_no_verdict(self):
        m = StatusModel()
        m.set_reading(1.0, counters(100))
        self.assertEqual(self.view(m, 1.0).link, "unknown")

    def test_rising_good_frames_mean_up_and_a_flat_count_means_down(self):
        m = StatusModel()
        m.set_reading(1.0, counters(100))
        m.set_reading(2.0, counters(130))
        self.assertEqual(self.view(m, 2.0).link, "up")
        m.set_reading(3.0, counters(130, bad=40))
        row = self.view(m, 3.0)
        self.assertEqual((row.link, row.crc_ok, row.crc_bad), ("down", 130, 40))

    def test_link_status_7_does_not_make_a_flat_count_up(self):
        m = StatusModel()
        m.set_reading(1.0, counters(5, lock=7))
        m.set_reading(2.0, counters(5, lock=7))
        self.assertEqual(self.view(m, 2.0).link, "down")

    def test_a_counter_that_goes_backwards_is_unknown_not_down(self):
        m = StatusModel()
        m.set_reading(1.0, counters(900))
        m.set_reading(2.0, counters(3))
        self.assertEqual(self.view(m, 2.0).link, "unknown")

    def test_a_fresh_good_read_is_live(self):
        m = StatusModel()
        m.set_reading(1.0, counters(1))
        row = self.view(m, 3.0)
        self.assertEqual((row.stale, row.age_s), (False, 2.0))

    def test_an_old_read_is_stale(self):
        m = StatusModel(stale_after_s=5.0)
        m.set_reading(1.0, counters(1))
        self.assertTrue(self.view(m, 6.5).stale)

    def test_a_failed_read_makes_the_value_stale_and_keeps_the_reason(self):
        m = StatusModel()
        m.set_reading(1.0, counters(1))
        m.set_reading(2.0, None, "no reply within 4s")
        view = m.view(2.1)
        self.assertTrue(view.heads[0].stale)
        self.assertEqual(view.error, "no reply within 4s")

    def test_a_good_read_after_a_failure_clears_the_error_and_compares_to_the_last_good(self):
        m = StatusModel()
        m.set_reading(1.0, counters(10))
        m.set_reading(2.0, None, "no reply")
        m.set_reading(3.0, counters(20))
        view = m.view(3.0)
        self.assertEqual((view.heads[0].link, view.heads[0].stale, view.error),
                         ("up", False, ""))

    def test_a_failure_before_any_read_shows_no_rows_but_the_reason(self):
        m = StatusModel()
        m.set_reading(1.0, None, "ssh failed")
        view = m.view(1.0)
        self.assertEqual((view.heads, view.error), ([], "ssh failed"))

    def test_a_reply_missing_a_counter_is_a_failure(self):
        m = StatusModel()
        m.set_reading(1.0, {"crc_ok_count": 1})
        self.assertIn("missing", m.view(1.0).error)
        self.assertEqual(m.view(1.0).heads, [])

    def test_clear_forgets_everything(self):
        m = StatusModel()
        m.set_reading(1.0, counters(1))
        m.clear()
        self.assertEqual(m.view(1.0).heads, [])


def wait_for(ctl, condition, timeout=5.0):
    """Apply worker events until condition() holds."""
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        ctl.poll()
        if condition():
            return True
        time.sleep(0.01)
    return False


class ControllerStatusTest(unittest.TestCase):
    def setUp(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        path = os.path.join(tmp.name, "c.json")
        with open(path, "w", encoding="utf-8") as fh:
            json.dump(config_doc(1, os.path.join(tmp.name, "r")), fh)
        self.fake = FakeSsh()
        self.ctl = Controller()
        self.addCleanup(self.ctl.shutdown)
        self.ctl.load(path)
        self.ctl.session.status_source = SshStatusSource(
            self.ctl.session.config.status, run=self.fake)

    def view(self):
        return self.ctl.status.view(time.monotonic())

    def start(self):
        self.ctl.start_status(period_s=0.02)
        self.assertTrue(wait_for(self.ctl, lambda: self.view().heads))

    def test_frames_flowing_shows_live_up(self):
        self.start()
        self.assertTrue(wait_for(self.ctl, lambda: self.view().heads[0].link == "up"))
        self.assertFalse(self.view().heads[0].stale)

    def test_frames_stopping_shows_down_within_a_few_reads(self):
        self.start()
        self.assertTrue(wait_for(self.ctl, lambda: self.view().heads[0].link == "up"))
        self.fake.frames_flow = False
        self.assertTrue(wait_for(self.ctl, lambda: self.view().heads[0].link == "down"))
        self.assertFalse(self.view().heads[0].stale)

    def test_an_unreachable_board_makes_the_row_stale(self):
        self.start()
        self.fake.fail = "Connection timed out"
        self.assertTrue(wait_for(self.ctl, lambda: self.view().heads[0].stale))
        self.assertIn("Connection timed out", self.view().error)
        self.fake.fail = ""
        self.assertTrue(wait_for(self.ctl, lambda: not self.view().heads[0].stale))

    def test_a_hung_ssh_makes_the_row_stale(self):
        self.start()
        self.fake.hang = True
        self.assertTrue(wait_for(self.ctl, lambda: self.view().heads[0].stale))

    def test_stop_ends_the_task_and_tests_can_run_again(self):
        self.start()
        with self.assertRaises(Busy):
            self.ctl.start_status()
        self.ctl.stop_status()
        self.assertTrue(wait_for(self.ctl, lambda: not self.ctl.running))
        self.assertFalse(self.ctl.status_running)

    def test_no_status_section_means_nothing_to_start(self):
        self.ctl.session.status_source = None
        self.assertFalse(self.ctl.status_configured)
        with self.assertRaises(StatusNotConfigured):
            self.ctl.start_status()
        self.assertFalse(self.ctl.running)


if __name__ == "__main__":
    unittest.main()
