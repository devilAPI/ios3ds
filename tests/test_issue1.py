#!/usr/bin/env python3
"""Offline checks for issue 1: SpringBoard crash reports and the status-bar PNG.

The console log had no reason for the SpringBoard exit because the stock 8C148
rootfs has no CrashReporter directory. The build must create all three paths
scripts/crashlogs.py reads, and must ship our own transparent placeholder for
the status-bar image SpringBoard asks for when there is no carrier bundle.
"""
import json
import os
import struct
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


class Issue1Recipe(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        with open(os.path.join(ROOT, "rootfs", "recipe.json")) as f:
            cls.recipe = json.load(f)

    def test_crash_reporter_dirs(self):
        mkdirs = self.recipe.get("mkdirs", [])
        # /Library/Logs is a stock symlink to /private/var/log; the hfsplus
        # editor cannot descend into it, so only the real directories are
        # created here (crashlogs.py still finds reports under either path).
        for path in (
            "/private/var/mobile/Library/Logs",
            "/private/var/mobile/Library/Logs/CrashReporter",
            "/private/var/log/CrashReporter",
        ):
            self.assertIn(path, mkdirs, path)

    def test_carrier_png_is_ours_and_valid(self):
        source = "rootfs/files/statusbar/Black_CARRIER_.png"
        target = "/System/Library/CoreServices/SpringBoard.app/Black_CARRIER_.png"
        self.assertEqual(self.recipe.get("add", {}).get(source), target)
        self.assertEqual(self.recipe.get("chmod", {}).get(target), "0644")
        with open(os.path.join(ROOT, source), "rb") as f:
            png = f.read()
        self.assertTrue(png.startswith(b"\x89PNG\r\n\x1a\n"))
        # IHDR: 1x1 RGBA, no interlace; no Apple image bytes are copied.
        width, height, depth, color, comp, filt, interlace = struct.unpack_from(">IIBBBBB", png, 16)
        self.assertEqual((width, height, depth, color, comp, filt, interlace), (1, 1, 8, 6, 0, 0, 0))
        self.assertLess(len(png), 128)

    def test_springboard_crash_logging_is_built_in(self):
        with open(os.path.join(ROOT, "tools", "ios3ds-diag", "applog.c")) as f:
            src = f.read()
        self.assertIn("crash_start();", src)
        self.assertIn("uncaught exception", src)
        self.assertIn("fatal signal", src)
        # Only icons inside a folder need a ghostly image; warming all of them
        # cost 9-15 s of main-thread time on the tester's New 3DS.
        self.assertIn('sb_call(root, "folderIcons")', src)
        self.assertIn('sb_call(sb_call(folder_icon, "folder"), "leafIcons")', src)


if __name__ == "__main__":
    unittest.main()
