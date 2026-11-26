"""Offline tests for version selection, patch relocation and recipe overlays.
Synthetic data only: no Apple files or IPSWs are needed."""
import hashlib
import json
import os
import re
import sys
import tempfile
import unittest
from unittest import mock

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

    def test_boot_cfg_overrides(self):
        """Only older versions override boot.cfg lines; 4.2.1/4.1 have none."""
        self.assertNotIn("boot_cfg", versions.get())
        self.assertNotIn("boot_cfg", versions.get("4.1"))
        # 3.1.3's PICA200 CoreAnimation path is opt-in: default software CA
        # needs the ARM9 copy, like 2.2.1. The opt-in build keeps the
        # generated gpu_mirror=1 and CA_ENABLE_OGL=1 (test below).
        self.assertEqual(versions.boot_cfg("3.1.3")["gpu_mirror"][0], "0")
        self.assertEqual(versions.boot_cfg("2.2.1")["gpu_mirror"][0], "0")
        self.assertFalse(versions.gpu313("3.1.3"))
        self.assertFalse(versions.gpu313("4.2.1"))
        for v in versions.VERSIONS:
            for key, (val, why) in versions.boot_cfg(v).items():
                self.assertRegex(key, r"^[a-z0-9_]+$")
                self.assertTrue(why)

    def test_3_1_3_gpu_path_is_opt_in(self):
        """IOS3DS_GPU313=1 turns on the dyld patch, CA env and the mirror."""
        with tempfile.TemporaryDirectory() as td:
            path = versions.recipe_path("3.1.3", td)
            with open(path) as f:
                plain = json.load(f)
            offsets = [fp["offset"] for fp in plain["file_patches"]]
            self.assertNotIn("0x1bde788", offsets)   # EAGLServer API patch
            sb = "/System/Library/LaunchDaemons/com.apple.SpringBoard.plist"
            self.assertEqual(plain["plist_set"][sb]["EnvironmentVariables"]["CA_ENABLE_OGL"], "0")
            self.assertEqual(plain["plist_set"][sb]["EnvironmentVariables"]["LK_ENABLE_OGL"], "0")
            self.assertNotIn("IOS3DS_CA_SWAP_DIAG", plain["plist_set"][sb]["EnvironmentVariables"])
        with mock.patch.dict(os.environ, {"IOS3DS_GPU313": "1"}):
            self.assertTrue(versions.gpu313("3.1.3"))
            self.assertFalse(versions.gpu313("4.2.1"))
            self.assertEqual(versions.boot_cfg("3.1.3"), {})
            with tempfile.TemporaryDirectory() as td:
                path = versions.recipe_path("3.1.3", td)
                with open(path) as f:
                    recipe = json.load(f)
                self.assertIn("0x1bde788", [fp["offset"] for fp in recipe["file_patches"]])
                env = recipe["plist_set"][sb]["EnvironmentVariables"]
                self.assertEqual(env["CA_ENABLE_OGL"], "1")
                self.assertNotIn("LK_ENABLE_OGL", env)
                self.assertNotIn("IOS3DS_CA_SWAP_DIAG", env)
        with mock.patch.dict(os.environ, {"IOS3DS_GPU313": "1", "IOS3DS_GPU313_DIAG": "1"}):
            with tempfile.TemporaryDirectory() as td:
                with open(versions.recipe_path("3.1.3", td)) as f:
                    recipe = json.load(f)
                env = recipe["plist_set"][sb]["EnvironmentVariables"]
                self.assertEqual(env["IOS3DS_CA_SWAP_DIAG"], "1")

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
            # 26: VirtualAudio's N72/N81 product swap (mic input), the
            # 8B117 MusicLibrary .cbk patch (re-log 131, 4.1 completeness),
            # and the two CoreMotion provider-mask immediates for the gyro
            # (re-log 139).
            self.assertEqual(len(recipe["file_patches"]), 26)
            self.assertIn("0x53bc3c", [fp["offset"] for fp in recipe["file_patches"]])
            self.assertIn("0x53bc4c", [fp["offset"] for fp in recipe["file_patches"]])
            self.assertIn("0x5708c4", [fp["offset"] for fp in recipe["file_patches"]])
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
                    # around DataMigrator (re-log 125).
                    self.assertEqual([fp["offset"] for fp in recipe["file_patches"]],
                                     ["0x176a008", "0x1767600", "0x176a358",
                                      "0x176a3dc", "0x1769eb0", "0x176a1c4",
                                      "0x176a210", "0x176a260",
                                      "0x95760", "0x1bde74c", "0x2f91bb8",
                                      "0xfa800"])  # EAGLServer API patch is opt-in; VirtualAudio N72/N82 swap (mic)
                    # SpringBoard skips DataMigrator only when this matches
                    # the running build (re-log 125).
                    self.assertIn("<key>SBLastSystemVersion</key>\n\t<string>7E18</string>",
                                  recipe["write"]["/private/var/mobile/Library/Preferences/com.apple.springboard.plist"])
                elif v == "2.2.1":
                    # 2.x has no dyld cache: liblockdown is patched in place
                    # and keeps its stock signature blob (re-signing stalls
                    # dyld, re-log 116); the 2.2.1 kernel patch
                    # cs-invalid-page-allow accepts the patched pages.
                    self.assertEqual([fp["offset"] for fp in recipe["file_patches"]],
                                     ["0x2ea8", "0x2c20", "0x544", "0x2be4", "0x30a8",
                                      "0x30f4", "0x3144", "0x2c60"])
                    self.assertTrue(all(fp["path"] == "/usr/lib/liblockdown.dylib"
                                        for fp in recipe["file_patches"]))
                    # 5H11a's VirtualAudio N72->X72 swap (re-log 130) is gone:
                    # it makes GetProductID miss N72 and the routing manager
                    # refuses every category, so no audio stream starts.
                    # lockdown_copy_value's table literal is at +0x28 (re-log 128)
                    self.assertTrue(recipe["file_patches"][1]["new"].startswith("20c09fe5"))
                    self.assertNotIn("resign", recipe)
                    self.assertIn("<key>SBLastSystemVersion</key>\n\t<string>5H11a</string>",
                                  recipe["write"]["/private/var/mobile/Library/Preferences/com.apple.springboard.plist"])
                    names = [p["name"] for p in json.load(open(versions.patch_files(v)[0]))]
                    self.assertIn("cs-invalid-page-allow", names)
                else:
                    self.assertEqual(recipe["file_patches"], [])
                if v == "2.2.1":
                    # the 2.x build of the input daemon (classic lazy binding)
                    self.assertEqual(recipe["add"], {
                        "build/rootfs-add/ios2/ios3ds-input": "/usr/libexec/ios3ds-input",
                        "tools/ios3ds-input/daemon/com.ios3ds.input.plist":
                        "/System/Library/LaunchDaemons/com.ios3ds.input.plist",
                        "build/rootfs-add/ios2/ios3ds-mcud": "/usr/libexec/ios3ds-mcud",
                        "tools/ios3ds-mcu/daemon/com.ios3ds.mcud.plist":
                        "/System/Library/LaunchDaemons/com.ios3ds.mcud.plist",
                        "build/rootfs-add/ios2/ios3ds-tone": "/usr/libexec/ios3ds-tone",
                        # fake GPS: applog hooks, the tool, the gated test job
                        "build/rootfs-add/ios2/ios3ds-applog.dylib": "/usr/lib/ios3ds-applog.dylib",
                        "build/rootfs-add/ios2/ios3ds-diag": "/usr/libexec/ios3ds-diag",
                        "build/rootfs-add/ios2/ios3ds-gps": "/usr/bin/ios3ds-gps",
                        "build/rootfs-add/ios2/ios3ds-gpstest": "/usr/libexec/ios3ds-gpstest",
                        "tools/ios3ds-gps/com.ios3ds.gpstest.plist":
                        "/System/Library/LaunchDaemons/com.ios3ds.gpstest.plist",
                        # Wi-Fi: the shim-ifnet daemon and the in-process Apple80211 bridge
                        "build/rootfs-add/ios2/ios3ds-wifid": "/usr/libexec/ios3ds-wifid",
                        "build/rootfs-add/ios2/ios3ds-nettest": "/usr/libexec/ios3ds-nettest",
                        "build/rootfs-add/ios2/ios3ds-a80211.dylib": "/usr/lib/ios3ds-a80211.dylib",
                        "tools/ios3ds-wifi/daemon/com.ios3ds.wifi.plist":
                        "/System/Library/LaunchDaemons/com.ios3ds.wifi.plist"})
                    self.assertEqual(list(recipe["plist_set"]),
                                     ["/System/Library/LaunchDaemons/com.apple.SpringBoard.plist"])
                    env = recipe["plist_set"]["/System/Library/LaunchDaemons/com.apple.SpringBoard.plist"][
                        "EnvironmentVariables"]
                    self.assertEqual(env["CA_ENABLE_MBX2D"], "0")
                else:
                    # only the 7E18 builds (-DGL_IOS3, -DINPUT_IOS3) of our
                    # GL engine and input daemon
                    self.assertEqual(recipe["add"], {
                        "build/rootfs-add/ios3/MBXGLEngine":
                        "/System/Library/Frameworks/OpenGLES.framework/MBXGLEngine.bundle/MBXGLEngine",
                        "build/rootfs-add/ios3/ios3ds-input": "/usr/libexec/ios3ds-input",
                        "tools/ios3ds-input/daemon/com.ios3ds.input.plist":
                        "/System/Library/LaunchDaemons/com.ios3ds.input.plist",
                        "build/rootfs-add/ios3/ios3ds-mcud": "/usr/libexec/ios3ds-mcud",
                        "tools/ios3ds-mcu/daemon/com.ios3ds.mcud.plist":
                        "/System/Library/LaunchDaemons/com.ios3ds.mcud.plist",
                        "build/rootfs-add/ios3/ios3ds-tone": "/usr/libexec/ios3ds-tone",
                        # Wi-Fi: the 4.2-built daemon and Apple80211 bridge
                        # run on 7E18 (re-log: Wi-Fi on 4.1 and 3.1.3)
                        "build/rootfs-add/usr/libexec/ios3ds-wifid": "/usr/libexec/ios3ds-wifid",
                        "build/rootfs-add/usr/libexec/ios3ds-nettest": "/usr/libexec/ios3ds-nettest",
                        "build/rootfs-add/usr/lib/ios3ds-a80211.dylib": "/usr/lib/ios3ds-a80211.dylib",
                        "tools/ios3ds-wifi/daemon/com.ios3ds.wifi.plist":
                        "/System/Library/LaunchDaemons/com.ios3ds.wifi.plist",
                        "build/rootfs-add/usr/libexec/ios3ds-diag": "/usr/libexec/ios3ds-diag",
                        "tools/ios3ds-diag/com.ios3ds.urltest.plist":
                        "/System/Library/LaunchDaemons/com.ios3ds.urltest.plist",
                        # fake GPS: applog hooks, the tool, the gated test job
                        "build/rootfs-add/usr/lib/ios3ds-applog.dylib": "/usr/lib/ios3ds-applog.dylib",
                        "build/rootfs-add/usr/bin/ios3ds-gps": "/usr/bin/ios3ds-gps",
                        "build/rootfs-add/usr/libexec/ios3ds-gpstest": "/usr/libexec/ios3ds-gpstest",
                        "tools/ios3ds-gps/com.ios3ds.gpstest.plist":
                        "/System/Library/LaunchDaemons/com.ios3ds.gpstest.plist"})
                    self.assertEqual(recipe["plist_merge"], {
                        "/System/Library/LaunchDaemons/com.apple.configd.plist": {
                            "EnvironmentVariables": {"DYLD_INSERT_LIBRARIES": "/usr/lib/ios3ds-a80211.dylib"}}})
                    self.assertEqual(recipe["disable_daemons"], ["com.apple.wifiFirmwareLoader.plist"])
                    self.assertEqual(list(recipe["plist_set"]),
                                     ["/System/Library/LaunchDaemons/com.apple.SpringBoard.plist"])
                    self.assertEqual(recipe["plist_set"][
                        "/System/Library/LaunchDaemons/com.apple.SpringBoard.plist"][
                            "StandardOutPath"], "/dev/console")
                if v == "2.2.1":
                    self.assertEqual(recipe["disable_daemons"], [])
                # Cydia without TLSFix (no Substrate loader on 3.x/2.x); 2.x
                # gets its own era set (Cydia 1.0.3222, rootfs/cydia-2.x.json)
                self.assertEqual(recipe["cydia"], "rootfs/cydia.json" if v == "3.1.3"
                                 else "rootfs/cydia-2.x.json")
                self.assertFalse(recipe["tlsfix"])
                self.assertEqual(recipe["cydia_pseudo"]["firmware"], v)
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

    def test_clcd_swap_dirty_is_version_flagged(self):
        """The mirror's CLCD-swap fix must stay out of the 4.2.1 shim."""
        self.assertFalse(versions.get().get("clcd_swap_dirty", False))
        self.assertFalse(versions.get("4.1").get("clcd_swap_dirty", False))
        self.assertTrue(versions.get("3.1.3")["clcd_swap_dirty"])
        text = open(os.path.join(ROOT, "kernel", "shim", "kpi.h")).read()
        self.assertIn("#define IOS3DS_KPI_CLCD_SWAP_DIRTY 0", text)

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
