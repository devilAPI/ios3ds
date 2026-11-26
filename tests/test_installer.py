"""Offline tests for the release installer's --ios version choice.

Synthetic files and mocked build steps only: no IPSW, Apple file or SD card.
"""
import argparse
import hashlib
import io
import json
import os
import sys
import tempfile
import types
import unittest
from contextlib import redirect_stderr
from unittest import mock

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "scripts"))

import install as installer  # noqa: E402
import mkrelease  # noqa: E402


def capture_say(testcase):
    """Patch installer.say() and return a list of the lines it printed."""
    lines = []
    patcher = mock.patch.object(installer, "say", side_effect=lines.append)
    patcher.start()
    testcase.addCleanup(patcher.stop)
    return lines


class TestVersionChoice(unittest.TestCase):
    def test_installable_versions_and_ipsw_names(self):
        self.assertEqual(installer.INSTALL_VERSIONS, ("4.2.1", "4.1", "3.1.3", "2.2.1"))
        want = {
            "4.2.1": "iPod2,1_4.2.1_8C148_Restore.ipsw",
            "4.1": "iPod2,1_4.1_8B117_Restore.ipsw",
            "3.1.3": "iPod2,1_3.1.3_7E18_Restore.ipsw",
            "2.2.1": "iPod2,1_2.2.1_5H11a_Restore.ipsw",
        }
        for v, name in want.items():
            self.assertEqual(installer.ipsw_name(v), name)

    def test_parser_default_and_supported_choices(self):
        args = installer.parser().parse_args(["fake.ipsw", "/mnt/sd"])
        self.assertEqual(args.ios, "4.2.1")
        for v in installer.INSTALL_VERSIONS:
            args = installer.parser().parse_args(["--ios", v, "fake.ipsw", "/mnt/sd"])
            self.assertEqual(args.ios, v)
        # inventory-only entries in versions.py are not installer choices
        for bad in ("3.0", "4.0", "9.9.9", "4.2.1.1"):
            err = io.StringIO()
            with redirect_stderr(err), self.assertRaises(SystemExit) as cm:
                installer.parser().parse_args(["--ios", bad, "fake.ipsw"])
            self.assertEqual(cm.exception.code, 2)
            self.assertIn("unknown iOS version", err.getvalue())

    def test_incomplete_recipe_is_refused(self):
        broken = {"patch_set": None, "overlay": None, "build": "0A0"}
        with mock.patch.object(installer.versions, "get", return_value=broken):
            with self.assertRaises(argparse.ArgumentTypeError) as cm:
                installer.check_version("4.1")
            self.assertIn("no installer build recipe", str(cm.exception))

    def test_warning_only_for_nondefault_versions(self):
        lines = []
        installer.warn_experimental("4.2.1", lines.append)
        self.assertEqual(lines, [])
        for version in ("4.1", "3.1.3", "2.2.1"):
            lines = []
            installer.warn_experimental(version, lines.append)
            text = "\n".join(lines)
            self.assertIn(version, text)
            self.assertIn("HIGHLY EXPERIMENTAL", text)
            self.assertIn("QEMU emulator", text)
            self.assertIn("NEVER on a real 3DS", text)
            self.assertIn("MAY NOT BOOT", text)
            self.assertIn("docs/other-versions.md", text)

    def test_ipsw_sha1_is_checked(self):
        data = b"synthetic firmware bytes, not an IPSW" * 32
        with tempfile.TemporaryDirectory() as td:
            path = os.path.join(td, "fake.ipsw")
            with open(path, "wb") as f:
                f.write(data)
            good = hashlib.sha1(data).hexdigest()
            with mock.patch.object(installer.versions, "get", return_value={
                    "sha1": good, "build": "TEST", "ipsw": "fake.ipsw"}):
                lines = capture_say(self)
                installer.verify_ipsw(path, "2.2.1")
                self.assertTrue(any(good in line for line in lines))
            bad = "0" * 40
            with mock.patch.object(installer.versions, "get", return_value={
                    "sha1": bad, "build": "TEST", "ipsw": "fake.ipsw"}):
                lines = capture_say(self)
                with self.assertRaises(SystemExit) as cm:
                    installer.verify_ipsw(path, "2.2.1")
                self.assertEqual(cm.exception.code, 1)
                text = "\n".join(lines)
                self.assertIn(bad, text)
                self.assertIn("not the iOS 2.2.1 IPSW", text)
                # --force is a separate option and must not weaken the check
                self.assertIn("cannot be bypassed", text)

    def test_staging_folder_must_match_selected_version(self):
        with tempfile.TemporaryDirectory() as td:
            man = os.path.join(td, "manifest.json")
            with open(man, "w") as f:
                json.dump({"source": {"product": "iPod2,1", "build": "8B117"}}, f)
            capture_say(self)
            installer.check_staging(td, "4.1")     # matching build: no exit
            err = capture_say(self)
            with self.assertRaises(SystemExit):
                installer.check_staging(td, "3.1.3")
            self.assertTrue(any("not iOS 3.1.3" in line for line in err))

    def test_build_tree_passes_ios_like_make_sd(self):
        fake_mksd = types.SimpleNamespace(
            parser=lambda: argparse.ArgumentParser(),
            build_tree=mock.Mock())
        with mock.patch.dict(sys.modules, {"mksd": fake_mksd}):
            installer.build_tree("stage", "tree", "prebuilt", "hfs", ios="3.1.3")
        args = fake_mksd.build_tree.call_args[0][0]
        self.assertEqual(args.ios, "3.1.3")
        self.assertEqual(args.staging, "stage")
        self.assertEqual(args.out, "tree")
        # the installer never picks up the developer CARRY_FROM env
        self.assertIsNone(args.carry_from)


class TestReleasePackaging(unittest.TestCase):
    def test_zip_installer_and_other_version_docs(self):
        self.assertIn("docs/other-versions.md", mkrelease.SOURCES)
        self.assertIn("scripts/install.py", mkrelease.SOURCES)
        self.assertTrue(os.path.isfile(os.path.join(ROOT, "docs", "other-versions.md")))
        self.assertIn("runpy.run_path", mkrelease.INSTALL_WRAPPER)  # forwards --ios

    def test_docs_document_the_choice_and_warning(self):
        for rel in ("README.md", "docs/release.md", "docs/other-versions.md"):
            with open(os.path.join(ROOT, rel), encoding="utf-8") as f:
                text = f.read()
            self.assertIn("--ios", text, rel)
            self.assertIn("4.2.1", text, rel)
        with open(os.path.join(ROOT, "docs", "other-versions.md"), encoding="utf-8") as f:
            page = f.read()
        for marker in ("HIGHLY EXPERIMENTAL", "QEMU", "console", "may not boot",
                       "IOS3DS_GPU313", "IOS3DS_GPU221"):
            self.assertIn(marker, page, marker)


if __name__ == "__main__":
    unittest.main()
