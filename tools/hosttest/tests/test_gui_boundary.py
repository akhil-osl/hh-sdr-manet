import ast
import os
import unittest

PKG = os.path.join(os.path.dirname(__file__), "..", "hosttest")
# The GUI side uses the session, runner and config. It never reaches the link,
# the wire format or the command layer. (results is imported only for the
# UNKNOWN constant.)
FORBIDDEN = {"socket", "transport", "codec", "command"}


def imported(module_file):
    with open(os.path.join(PKG, module_file), encoding="utf-8") as fh:
        tree = ast.parse(fh.read())
    names = set()
    for node in ast.walk(tree):
        if isinstance(node, ast.Import):
            names.update(a.name.split(".")[0] for a in node.names)
        elif isinstance(node, ast.ImportFrom):
            if node.level:
                names.add((node.module or "").split(".")[0])
                names.update(a.name for a in node.names if not node.module)
            else:
                names.add((node.module or "").split(".")[0])
    return names


class BoundaryTest(unittest.TestCase):
    def test_gui_side_does_not_reach_below_the_runner(self):
        for module in ("gui.py", "controller.py"):
            self.assertEqual(imported(module) & FORBIDDEN, set(), module)

    def test_controller_has_no_tkinter(self):
        self.assertNotIn("tkinter", imported("controller.py"))


if __name__ == "__main__":
    unittest.main()
