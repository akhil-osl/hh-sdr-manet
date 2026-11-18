import json
import os
import tempfile
import time
import tkinter as tk
import unittest

from fake_board import FakeBoard, config_doc, example_handler, line_script
from hosttest.controller import Controller
from hosttest.gui import App


def display_available() -> bool:
    try:
        root = tk.Tk()
    except tk.TclError:
        return False
    root.destroy()
    return True


@unittest.skipUnless(display_available(), "no display for Tk")
class GuiTest(unittest.TestCase):
    def setUp(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        board = FakeBoard(line_script(example_handler))
        self.addCleanup(board.stop)
        doc = config_doc(board.port, os.path.join(tmp.name, "results"))
        self.path = os.path.join(tmp.name, "c.json")
        with open(self.path, "w", encoding="utf-8") as fh:
            json.dump(doc, fh)
        self.root = tk.Tk()
        self.root.withdraw()
        self.app = App(self.root, Controller())
        self.addCleanup(self.cleanup)

    def cleanup(self):
        self.app.ctl.shutdown()
        try:
            self.root.destroy()
        except tk.TclError:
            pass

    def pump_until_idle(self, limit_s=5.0):
        end = time.monotonic() + limit_s
        while time.monotonic() < end:
            self.root.update()
            self.app.ctl.join(0.01)
            self.root.update()
            if not self.app.ctl.running:
                return
        self.fail("run did not finish")

    def test_nothing_runs_before_a_config_is_loaded(self):
        self.assertEqual(self.app.run_button.instate(["disabled"]), True)
        self.assertEqual(self.app._vars["link"].get(), "Disconnected")

    def test_load_fills_header_and_test_list(self):
        self.assertTrue(self.app.load_config(self.path))
        self.assertEqual(self.app._vars["board"].get(), "fake")
        self.assertEqual(list(self.app.tests.get_children()), ["status", "value"])
        self.assertEqual(len(self.app.tests.selection()), 2)

    def test_run_all_fills_results_and_test_rows(self):
        self.app.load_config(self.path)
        self.app.run_all()
        self.assertTrue(self.app.run_button.instate(["disabled"]))
        self.pump_until_idle()
        self.app.root.update()
        self.assertEqual(len(self.app.results.get_children()), 4)
        self.assertEqual(self.app.tests.set("value", "last"), "PASS")
        self.assertEqual(self.app._vars["board_id"].get(), "B7")
        self.assertEqual(self.app._vars["link"].get(), "Connected")
        self.assertEqual(self.app._vars["counts"].get(), "Pass 4   Fail 0")
        self.assertFalse(self.app.run_button.instate(["disabled"]))

    def test_selecting_a_result_shows_tx_and_rx(self):
        self.app.load_config(self.path)
        self.app.run_all()
        self.pump_until_idle()
        self.app.root.update()
        self.app.results.selection_set("3")
        self.app._show_detail()
        text = self.app.detail.get("1.0", "end")
        self.assertIn("EXAMPLE_READ ch1", text)
        self.assertIn("value=42", text)

    def test_close_while_idle_destroys_the_window(self):
        self.app.on_close()
        with self.assertRaises(tk.TclError):
            self.root.winfo_exists()
        self.assertTrue(self.app.ctl.session is None)


if __name__ == "__main__":
    unittest.main()
