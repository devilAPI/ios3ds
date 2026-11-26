#!/usr/bin/env python3
"""Offline check for emu/wifi_popup_test.py's --out handling.

The QEMU harness used to call home_screen(out) / shot(out, ...) inside main()
without ever binding ``out``: it died with NameError after boot, before any
join check ran.  This test needs no QEMU or Apple files; it only checks that
main() binds the output directory before the first use.
"""
import ast
import os
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SOURCE = os.path.join(ROOT, "emu", "wifi_popup_test.py")


class OutputBinding(ast.NodeVisitor):
    def __init__(self):
        self.events = []

    def visit_Assign(self, node):
        if (isinstance(node.value, ast.Attribute) and node.value.attr == "out" and
                isinstance(node.value.value, ast.Name) and node.value.value.id == "a" and
                any(isinstance(t, ast.Name) and t.id == "out" for t in node.targets)):
            self.events.append(("bind", node.lineno))
        self.generic_visit(node)

    def visit_Call(self, node):
        func = node.func
        if (isinstance(func, ast.Name) and func.id in ("shot", "home_screen") and
                node.args and isinstance(node.args[0], ast.Name) and
                node.args[0].id == "out"):
            self.events.append(("use", node.lineno))
        self.generic_visit(node)


class WifiPopupHarnessTest(unittest.TestCase):
    def test_main_binds_out_before_use(self):
        with open(SOURCE, encoding="utf-8") as f:
            tree = ast.parse(f.read(), SOURCE)
        main = next(n for n in tree.body
                    if isinstance(n, ast.FunctionDef) and n.name == "main")
        visitor = OutputBinding()
        visitor.visit(main)
        events = visitor.events
        self.assertTrue(events, "main() no longer references the output directory")
        self.assertEqual(events[0][0], "bind",
                         "first out event in main() is %s at line %d, not the binding"
                         % (events[0][0], events[0][1]))


if __name__ == "__main__":
    unittest.main()
