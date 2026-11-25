"""Offline tests for version selection, patch relocation and recipe overlays.
Synthetic data only: no Apple files or IPSWs are needed."""
import hashlib
import json
import os
import re
import sys
import tempfile
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "scripts"))
sys.path.insert(0, os.path.join(ROOT, "kernel"))

import kpi  # noqa: E402
import mkrootfs  # noqa: E402
import patcher  # noqa: E402
import versions  # noqa: E402


class TestVersions(unittest.TestCase):
    def test_default_has_no_overrides(self):
        info = versions.get()
        self.assertEqual(info["build"], "8C148")
        self.assertIsNone(versions.patch_files())
        self.assertIsNone(versions.recipe_path())
        self.assertEqual(versions.ba_version(), 5)
        self.assertEqual(versions.staging_dir(), os.path.join("build", "staging"))

    def test_registry_knows_old_builds(self):
        for v, build in (("2.1.1", "5F138"), ("2.2", "5G77a"), ("2.2.1", "5H11a"),
                         ("3.1.3", "7E18"), ("4.0", "8A293"), ("4.1", "8B117"),
                         ("4.2.1", "8C148")):
            self.assertEqual(versions.get(v)["build"], build)
        with self.assertRaises(SystemExit):
            versions.get("9.9.9")

    def test_other_version_layout(self):
        self.assertEqual(versions.staging_dir("4.1"), os.path.join("build", "fw-4.1", "staging"))
        pf = versions.patch_files("4.1")
        self.assertEqual(len(pf), 1)
        self.assertTrue(os.path.isfile(pf[0]), pf[0])

    def test_old_versions_layout(self):
        """3.1.3/2.2.1 caches are linked at 0xC0000000 (not 0x80000000)."""
        for v, build, epoch, irq in (("3.1.3", "7E18", 5, 0xC0068458),
                                     ("2.2.1", "5H11a", 3, 0xC006540C)):
            info = versions.get(v)
            self.assertEqual(info["build"], build)
            self.assertEqual(versions.ba_version(v), epoch)
            self.assertEqual(versions.virt_base(v), 0xC0000000)
            self.assertEqual(info["irq_cont"], irq)
            self.assertEqual(len(versions.patch_files(v)), 1)
            self.assertTrue(os.path.isfile(versions.patch_files(v)[0]))
            self.assertEqual(versions.staging_dir(v), os.path.join("build", "fw-" + v, "staging"))
        self.assertEqual(versions.virt_base(), 0x80000000)
        self.assertEqual(versions.virt_base("4.1"), 0x80000000)
        self.assertEqual(versions.get("2.2.1")["dt_delete"], ["arm-io/usb-otg"])
        self.assertEqual(versions.get().get("dt_delete", ()), ())

    def test_replace_marker_in_overlay(self):
        base = {"a": {"x": 1, "y": 2}, "b": [1, 2]}
        self.assertEqual(versions._merge(base, {"a": {"_replace": True, "z": 3}}),
                         {"a": {"z": 3}, "b": [1, 2]})
        self.assertEqual(versions._merge(base, {"a": {"y": 5}}),
                         {"a": {"x": 1, "y": 5}, "b": [1, 2]})

    def test_recipe_overlay(self):
        with tempfile.TemporaryDirectory() as td:
            path = versions.recipe_path("4.1", td)
            self.assertEqual(path, os.path.join(td, "recipe.json"))
            with open(path) as f:
                recipe = json.load(f)
            with open(os.path.join(ROOT, "rootfs", "recipe.json")) as f:
                base = json.load(f)
            self.assertEqual(recipe["write"]["/etc/fstab"], base["write"]["/etc/fstab"])
            # the 4.1 overlay carries symbol-relocated file-patch offsets keyed
            # by the base recipe's 8C148 offset: inapplicable patches are
            # dropped, the rest use fixed offsets. Two MBX patches were added
            # to the base recipe after the original positional map and are
            # relocated here too (unique string hits in the 8B117 cache).
            self.assertEqual(len(recipe["file_patches"]), 22)
            self.assertTrue(all("offset" in fp and "search" not in fp
                                for fp in recipe["file_patches"]))
            offsets = [fp["offset"] for fp in recipe["file_patches"]]
            self.assertIn("0x492c7cc", offsets)  # MBX2D failure string
            self.assertIn("0xa899a4", offsets)   # QuartzCore MBX2D init string

    def test_old_recipe_overlays(self):
        """No 4.2.1 cache patches; old versions add only their own verified patches."""
        with tempfile.TemporaryDirectory() as td:
            for v in ("3.1.3", "2.2.1"):
                recipe = json.load(open(versions.recipe_path(v, td)))
                if v == "3.1.3":
                    # Symbol-relocated 7E18 liblockdown activation patches,
                    # plus the three extra lockdown functions SpringBoard
                    # calls with the fake handle (remove/set/disconnect),
                    # the tethered/display-kind gates and the OpenGLES engine
                    # match string. The SpringBoard lock/icon-list/key-window
                    # patches of re-log 121-123 are gone: they only worked
                    # around DataMigrator (re-log NEXT).
                    self.assertEqual([fp["offset"] for fp in recipe["file_patches"]],
                                     ["0x176a008", "0x1767600", "0x176a358",
                                      "0x176a3dc", "0x1769eb0", "0x176a1c4",
                                      "0x176a210", "0x176a260",
                                      "0x95760", "0x1bde74c", "0x2f91bb8"])
                    # SpringBoard skips DataMigrator only when this matches
                    # the running build (re-log NEXT).
                    self.assertIn("<key>SBLastSystemVersion</key>\n\t<string>7E18</string>",
                                  recipe["write"]["/private/var/mobile/Library/Preferences/com.apple.springboard.plist"])
                elif v == "2.2.1":
                    # 2.x has no dyld cache. Patching + re-signing liblockdown
                    # made dyld stop after CoreSurface (re-log 115), so the
                    # stock signed dylib is kept and no file patches apply.
                    self.assertEqual(recipe["file_patches"], [])
                    self.assertNotIn("resign", recipe)
                else:
                    self.assertEqual(recipe["file_patches"], [])
                if v == "2.2.1":
                    self.assertEqual(recipe["add"], {})
                    self.assertEqual(list(recipe["plist_set"]),
                                     ["/System/Library/LaunchDaemons/com.apple.SpringBoard.plist"])
                else:
                    # only the 7E18 builds (-DGL_IOS3, -DINPUT_IOS3) of our
                    # GL engine and input daemon
                    self.assertEqual(recipe["add"], {
                        "build/rootfs-add/ios3/MBXGLEngine":
                        "/System/Library/Frameworks/OpenGLES.framework/MBXGLEngine.bundle/MBXGLEngine",
                        "build/rootfs-add/ios3/ios3ds-input": "/usr/libexec/ios3ds-input",
                        "tools/ios3ds-input/daemon/com.ios3ds.input.plist":
                        "/System/Library/LaunchDaemons/com.ios3ds.input.plist"})
                    self.assertEqual(list(recipe["plist_set"]),
                                     ["/System/Library/LaunchDaemons/com.apple.SpringBoard.plist"])
                    self.assertEqual(recipe["plist_set"][
                        "/System/Library/LaunchDaemons/com.apple.SpringBoard.plist"][
                            "StandardOutPath"], "/dev/console")
                self.assertEqual(recipe["disable_daemons"], [])
                self.assertFalse(recipe["cydia"])
                self.assertFalse(recipe["tlsfix"])
                self.assertEqual(recipe["write"]["/etc/fstab"], "/dev/md0 / hfs rw 0 0\n")
                self.assertNotIn("/System/Library/Caches/com.apple.dyld/enable-dylibs-to-override-cache",
                                 recipe["write"])
                self.assertEqual(len(recipe["remove"]), 1)   # free blocks for hfsplus grow


class TestPatcher(unittest.TestCase):
    def test_fixed_va(self):
        img = bytearray(b"\x00" * 0x100)
        p = {"name": "fixed", "va": "0x80000010", "orig": "11223344", "new": "44332211"}
        self.assertEqual(patcher.locate(img, 0x80000000, p), 0x80000010)
        applied = patcher.apply_all(img, 0x80000000, "/nonexistent", files=[])
        self.assertEqual(applied, [])
        with open(os.path.join(tempfile.gettempdir(), "ios3ds-test-patch.json"), "w") as f:
            json.dump([p], f)
        fn = f.name
        try:
            img[0x10:0x14] = bytes.fromhex(p["orig"])
            applied = patcher.apply_all(img, 0x80000000, fn, files=[fn])
            self.assertEqual(applied[0]["va"], 0x80000010)
            self.assertEqual(bytes(img[0x10:0x14]), bytes.fromhex(p["new"]))
        finally:
            os.remove(fn)

    def test_find_unique_and_bounded(self):
        img = bytearray(b"abcPATTERNdef" + b"PATTERN")
        base = 0x80000000
        p = {"name": "found", "find": {"orig": b"PATTERN".hex(), "from": "0x80000000",
                                       "to": hex(base + 13)},
             "orig": b"PATTERN".hex(), "new": "X" * 14}
        self.assertEqual(patcher.locate(img, base, p), base + 3)
        p2 = dict(p, name="ambiguous")
        p2["find"] = {"orig": b"PATTERN".hex()}
        with self.assertRaises(ValueError):
            patcher.locate(img, base, p2)
        p3 = dict(p, name="with-context")
        p3["find"] = {"orig": b"PATTERN".hex(), "before": b"abc".hex(), "after": b"def".hex()}
        self.assertEqual(patcher.locate(img, base, p3), base + 3)


class TestShimKpi(unittest.TestCase):
    def test_defaults_match_shim_header(self):
        """The generated header must leave the 4.2.1 shim byte-identical."""
        text = open(os.path.join(ROOT, "kernel", "shim", "kpi.h")).read()
        for macro, (sym, fallback) in kpi.SYMBOLS.items():
            m = re.search(r"#define IOS3DS_KPI_%s 0x([0-9a-f]+)" % macro, text)
            self.assertIsNotNone(m, macro)
            self.assertEqual(int(m.group(1), 16), fallback, macro)

    def test_literals_have_defaults(self):
        text = open(os.path.join(ROOT, "kernel", "shim", "kpi.h")).read()
        for macro, (fallback, overrides) in kpi.LITERALS.items():
            m = re.search(r"#define IOS3DS_KPI_%s 0x([0-9a-f]+)" % macro, text)
            self.assertIsNotNone(m, macro)
            self.assertEqual(int(m.group(1), 16), fallback, macro)
        self.assertIn("8B117", kpi.LITERALS["GIO_PLATFORM"][1])
        self.assertIn("7E18", kpi.LITERALS["GIO_PLATFORM"][1])
        self.assertIn("5H11a", kpi.LITERALS["GIO_PLATFORM"][1])
        self.assertIn("#define IOS3DS_KPI_VIRT_BASE 0x80000000", text)
        self.assertIn("#define IOS3DS_KPI_KERNEL_IRQ_CONT 0x80066494", text)


class TestReleasePackaging(unittest.TestCase):
    def test_release_ships_version_files(self):
        """install.py imports versions.py and --ios loads these recipes; the
        release zip must carry them or missing_imports() refuses to package."""
        import glob
        import mkrelease  # noqa: E402
        srcs = set()
        for pat in mkrelease.SOURCES:
            srcs.update(os.path.relpath(h, ROOT) for h in
                        glob.glob(os.path.join(ROOT, pat)) if os.path.isfile(h))
        mkrelease.missing_imports(srcs)             # raises SystemExit if a module is missing
        for rel in ("scripts/versions.py", "kernel/patchsets/4.1.json",
                    "rootfs/overlay-4.1.json", "kernel/patchsets/3.1.3.json",
                    "rootfs/overlay-3.1.3.json", "kernel/patchsets/2.2.1.json",
                    "rootfs/overlay-2.2.1.json"):
            self.assertIn(rel, srcs)


class TestFilePatches(unittest.TestCase):
    def test_offset_and_search(self):
        data = b"aaaORIGINALbbb"
        fp = {"offset": "0x3", "path": "/f", "orig": b"ORIGINAL".hex(), "new": "00" * 8}
        self.assertEqual(mkrootfs.resolve_patch_offset(data, fp), 3)
        fp.pop("offset")
        self.assertEqual(mkrootfs.resolve_patch_offset(data, fp), 3)
        fp["orig_sha256"] = hashlib.sha256(b"ORIGINAL").hexdigest()
        del fp["orig"]
        self.assertEqual(mkrootfs.resolve_patch_offset(data, fp), 3)
        fp["orig_sha256"] = hashlib.sha256(b"nope").hexdigest()
        with self.assertRaises(SystemExit):
            mkrootfs.resolve_patch_offset(data, fp)


if __name__ == "__main__":
    unittest.main()
