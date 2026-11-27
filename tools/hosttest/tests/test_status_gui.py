import time
import tkinter as tk
import unittest

from hosttest.controller import Controller
from hosttest.gui import App
from hosttest.status import HeadRow
from hosttest.status_gui import StatusWindow, head_values


def display_available() -> bool:
    try:
        root = tk.Tk()
    except tk.TclError:
        return False
    root.destroy()
    return True


class ValuesTest(unittest.TestCase):
    def test_head_cells(self):
        self.assertEqual(head_values(HeadRow(0, "down", 130, 40, 7, 2.04, False)),
                         ("head 0", "down", "130", "40", "7", "2.0 s", "live"))
        self.assertEqual(head_values(HeadRow(0, "up", 1, 0, 7, 9.0, True))[6], "STALE")


@unittest.skipUnless(display_available(), "no display for Tk")
class StatusWindowTest(unittest.TestCase):
    def setUp(self):
        self.root = tk.Tk()
        self.root.withdraw()
        self.addCleanup(self.root.destroy)
        self.ctl = Controller()
        self.win = StatusWindow(self.root, self.ctl)

    def rows(self, tree):
        return [(tree.item(i, "values"), tree.item(i, "tags")[0])
                for i in tree.get_children()]

    def counters(self, ok):
        return {"crc_ok_count": ok, "crc_bad_count": 0, "link_status": 7}

    def test_a_down_link_is_marked_down(self):
        now = time.monotonic()
        self.ctl.status.set_reading(now, self.counters(5))
        self.ctl.status.set_reading(now, self.counters(5))
        self.win.refresh()
        cells = self.rows(self.win.heads)
        self.assertEqual([(v[0], v[1], t) for v, t in cells], [("head 0", "down", "down")])

    def test_a_failed_read_greys_the_row_and_shows_the_reason(self):
        now = time.monotonic()
        self.ctl.status.set_reading(now, self.counters(5))
        self.ctl.status.set_reading(now, None, "no reply within 4s")
        self.win.refresh()
        self.assertEqual([t for _, t in self.rows(self.win.heads)], ["stale"])
        self.assertIn("no reply", self.win.note.get())

    def test_table_is_redrawn_not_appended(self):
        self.ctl.status.set_reading(time.monotonic(), self.counters(5))
        self.win.refresh()
        self.win.refresh()
        self.assertEqual(len(self.rows(self.win.heads)), 1)

    def test_start_is_disabled_until_a_config_with_status_is_loaded(self):
        self.win.refresh()
        self.assertTrue(self.win.start_button.instate(["disabled"]))
        self.assertTrue(self.win.stop_button.instate(["disabled"]))

    def test_app_menu_opens_one_window(self):
        app = App(self.root, Controller())
        first = app.open_status()
        self.assertIs(app.open_status(), first)
        app.ctl.shutdown()


if __name__ == "__main__":
    unittest.main()
