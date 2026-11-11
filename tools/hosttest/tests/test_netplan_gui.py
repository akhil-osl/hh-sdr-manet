import os
import tempfile
import tkinter as tk
import unittest

from hosttest import netplan
from hosttest.controller import Controller
from hosttest.gui import App
from hosttest.netplan_gui import NetPlanWindow


def display_available() -> bool:
    try:
        root = tk.Tk()
    except tk.TclError:
        return False
    root.destroy()
    return True


def fill(table, **values):
    for name, var in table.vars.items():
        var.set(values.get(name, ""))


@unittest.skipUnless(display_available(), "no display for Tk")
class NetPlanWindowTest(unittest.TestCase):
    def setUp(self):
        self.root = tk.Tk()
        self.root.withdraw()
        self.addCleanup(self.root.destroy)
        self.win = NetPlanWindow(self.root)
        self.nodes = self.win.tables["nodes"]

    def add_node(self, address, role="member", ip=""):
        fill(self.nodes, address=address, role=role, ip=ip)
        self.nodes.add()

    def test_empty_plan_is_valid_and_can_be_saved(self):
        self.assertEqual(self.win.status_var.get(), "Plan is valid")
        self.assertFalse(self.win.save_button.instate(["disabled"]))

    def test_invalid_address_is_flagged_and_blocks_save(self):
        self.add_node("0xF123")
        self.assertEqual([str(i) for i in self.win.issues][0][:17], "nodes[0].address:")
        self.assertIn("bad", self.nodes.tree.item("0", "tags"))
        self.assertTrue(self.win.save_button.instate(["disabled"]))
        self.assertEqual(self.win.command_var.get(), "fix the problems above first")
        self.assertEqual(self.win.problems.size(), 1)

    def test_fixing_the_row_clears_the_problem(self):
        self.add_node("0xF123")
        self.nodes.tree.selection_set("0")
        fill(self.nodes, address="0x0100", role="member")
        self.nodes.update()
        self.assertEqual(self.win.issues, [])
        self.assertFalse(self.win.save_button.instate(["disabled"]))
        self.assertEqual(self.win.command_var.get(), "dataterm -n 0x0100")

    def test_remove_row(self):
        self.add_node("0x0100")
        self.nodes.tree.selection_set("0")
        self.nodes.remove()
        self.assertEqual(self.win.plan["nodes"], [])

    def test_command_line_lists_peers_with_ip(self):
        self.add_node("0x0001", "command", "10.0.0.1")
        self.add_node("0x0002", "member", "10.0.0.2:6000")
        self.win.node_var.set("0x0001")
        self.win._update_command()
        self.assertEqual(self.win.command_var.get(),
                         "dataterm -n 0x0001 -P 0x0002=10.0.0.2:6000")

    def test_command_radio_rule_shows_in_problem_list(self):
        self.add_node("0x0005", "command")
        self.add_node("0x0003", "member")
        self.assertEqual(self.win.status_var.get(), "1 problem(s): fix them to save")
        self.assertIn("lowest addresses", self.win.problems.get(0))

    def test_rf_head_choices_follow_the_rf_head_tab(self):
        heads = self.win.tables["rf_heads"]
        fill(heads, name="head0", eui64="00:11:22:33:44:55:66:77")
        heads.add()
        nets = self.win.tables["nets"]
        self.assertEqual(list(nets.inputs["rf_head"].cget("values")), ["head0"])
        fill(nets, net="alpha", rf_head="head1", net_id="1")
        nets.add()
        self.assertIn("nets[0].rf_head", [i.path for i in self.win.issues])

    def test_save_and_open_round_trip(self):
        self.add_node("0x0100")
        with tempfile.TemporaryDirectory() as tmp:
            path = os.path.join(tmp, "plan.json")
            self.assertTrue(self.win.save_path(path))
            other = NetPlanWindow(self.root)
            self.addCleanup(other.destroy)
            self.assertTrue(other.open_path(path))
            self.assertEqual(other.plan["nodes"], [{"address": "0x0100", "role": "member"}])

    def test_main_window_opens_one_plan_window(self):
        app = App(self.root, Controller())
        first = app.open_netplan()
        self.assertIs(app.open_netplan(), first)
        app.ctl.shutdown()


if __name__ == "__main__":
    unittest.main()
