"""Offline tests of the Cydia pre-install (scripts/cydia.py, docs/cydia.md): the
pinned manifest, .deb parsing, the dpkg database it writes and the
download-failure fallback. A synthetic deb and a fake hfsplus stand in for the
real ones, so no network and no Apple files are needed."""
import gzip
import hashlib
import io
import json
import lzma
import os
import plistlib
import re
import sys
import tarfile
import tempfile
import unittest
from unittest import mock

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "scripts"))
import cydia  # noqa: E402


def tar_blob(entries):
    """entries: (name, kind, data/target, mode) -> uncompressed tar bytes"""
    bio = io.BytesIO()
    with tarfile.open(fileobj=bio, mode="w") as t:
        for name, kind, data, mode in entries:
            ti = tarfile.TarInfo(name)
            ti.mode = mode
            if kind == "dir":
                ti.type = tarfile.DIRTYPE
                t.addfile(ti)
            elif kind == "sym":
                ti.type = tarfile.SYMTYPE
                ti.linkname = data
                t.addfile(ti)
            else:
                ti.size = len(data)
                t.addfile(ti, io.BytesIO(data))
    return bio.getvalue()


def ar(members):
    out = b"!<arch>\n"
    for name, data in members:
        hdr = "%-16s%-12s%-6s%-6s%-8s%-10d`\n" % (name, 0, 0, 0, "100644", len(data))
        out += hdr.encode() + data + (b"\n" if len(data) & 1 else b"")
    return out


def make_deb(path, control, data_entries, scripts=None, lzma_data=True):
    ctl = [("./", "dir", None, 0o755), ("./control", "file", control.encode(), 0o644)]
    for n, d in (scripts or {}).items():
        ctl.append(("./" + n, "file", d, 0o755))
    data = tar_blob(data_entries)
    if lzma_data:
        dname, dblob = "data.tar.lzma", lzma.compress(data, format=lzma.FORMAT_ALONE)
    else:
        dname, dblob = "data.tar.gz", gzip.compress(data)
    with open(path, "wb") as f:
        f.write(ar([("debian-binary", b"2.0\n"), ("control.tar.gz", gzip.compress(tar_blob(ctl))),
                    (dname, dblob)]))


class FakeHfs:
    """Implements the hfsplus subcommands cydia.Image uses, on a dict."""

    def __init__(self):
        self.fs = {"/": ("d", 0o755, None), "/private": ("d", 0o755, None),
                   "/private/var": ("d", 0o755, None), "/private/etc": ("d", 0o755, None),
                   "/var": ("l", 0o755, "/private/var"), "/etc": ("l", 0o755, "/private/etc"),
                   "/sbin": ("d", 0o755, None), "/sbin/fsck": ("f", 0o555, b"stock")}

    def resolve(self, p):
        for a in ("/var", "/etc"):
            if p == a or p.startswith(a + "/"):
                return self.fs[a][2] + p[len(a):]
        return p

    def __call__(self, img, cmd, *args, check=True):
        if cmd == "ls":
            d = self.resolve(args[0])
            if d not in self.fs:
                return "No such file directory\n"
            lines = ["%s:" % d]
            for p, (k, mode, _) in sorted(self.fs.items()):
                if p != "/" and p.rpartition("/")[0] == (d if d != "/" else ""):
                    t = {"d": 0o040000, "f": 0o100000, "l": 0o120000}[k]
                    lines.append("%06o   0   0   0 1/1/2026 00:00 %s" % (t | mode, p.rpartition("/")[2]))
            return "\n".join(lines) + "\nTotal filesystem size: 1, free: 1\n"
        p = self.resolve(args[-1] if cmd in ("add", "chmod") else args[0])
        parent = p.rpartition("/")[0] or "/"
        assert self.fs.get(parent, ("x",))[0] == "d", "no parent dir for " + p
        if cmd == "mkdir":
            assert p not in self.fs
            self.fs[p] = ("d", 0o755, None)
        elif cmd == "add":
            assert p not in self.fs, "add over existing " + p
            with open(args[0], "rb") as f:
                self.fs[p] = ("f", 0o644, f.read())
        elif cmd == "rm":
            assert self.fs[p][0] != "d"
            del self.fs[p]
        elif cmd == "chmod":
            k, _, d = self.fs[p]
            self.fs[p] = (k, int(args[0], 8), d)
        elif cmd == "symlink":
            self.fs[p] = ("l", 0o755, args[1])
        else:
            raise AssertionError(cmd)
        return ""


class Manifest(unittest.TestCase):
    def test_pins(self):
        m = cydia.load_manifest()
        names = [p["package"] for p in m["packages"]]
        self.assertEqual(len(names), len(set(names)))
        for want in ("cydia", "apt7-lib", "dpkg", "base", "bash", "coreutils", "cydia-lproj"):
            self.assertIn(want, names)
        for p in m["packages"]:
            self.assertRegex(p["sha256"], r"^[0-9a-f]{64}$", p["package"])
            self.assertGreater(p["size"], 0)
            self.assertTrue(p["path"].endswith(".deb"))
            if p.get("url"):                                  # not from Telesphoreo: optional
                self.assertTrue(p["url"].startswith("https://") and p.get("group"), p["package"])
            else:
                self.assertTrue(p["path"].startswith("debs/"), p["package"])
        self.assertTrue(all(u.startswith("http") for u in m["mirrors"]))
        self.assertIn("/.cydia_no_stash", m["write"])        # one volume: no stashing
        # Substrate + its safe mode in the base set, inserted by launchd's environment
        for want in ("mobilesubstrate", "com.saurik.substrate.safemode"):
            self.assertIn(want, names)
        self.assertEqual(m["substrate"]["insert"], "/Library/MobileSubstrate/MobileSubstrate.dylib")
        self.assertNotIn("/etc/launchd.conf", m["write"])      # Substrate's launchd hook hangs 4.2.1
        tls = [p for p in m["packages"] if p["package"] == "com.skyglow.tlsfix"]
        self.assertEqual([p.get("group") for p in tls], ["tlsfix"])

    def test_ios2_set(self):
        """iPhone OS 2.x set (rootfs/cydia-2.x.json): Cydia 1.0.x, no Substrate/TLSFix,
        its own cache, Cydia's stash check patched out, the role preset"""
        m = cydia.load_manifest(os.path.join(ROOT, "rootfs", "cydia-2.x.json"))
        names = {p["package"]: p["version"] for p in m["packages"]}
        self.assertTrue(names["cydia"].startswith("1.0."))
        for want in ("apt7-lib", "apt7-key", "dpkg", "apr-lib", "pcre", "uikittools", "essential"):
            self.assertIn(want, names)
        for no in ("mobilesubstrate", "com.skyglow.tlsfix", "cydia-lproj"):
            self.assertNotIn(no, names)
        self.assertNotIn("substrate", m)
        self.assertEqual(m["cache"], "cydia-2.x")
        for p in m["packages"]:
            self.assertRegex(p["sha256"], r"^[0-9a-f]{64}$", p["package"])
            self.assertTrue(p["path"].startswith("debs/"))
        self.assertEqual(len(m["patch"]), 7)
        for p in m["patch"]:
            self.assertEqual((p["path"], p["orig"], p["new"]), ("/Applications/Cydia.app/Cydia_", "00d1", "00e0"))
        role = plistlib.loads(m["write"]["/var/lib/cydia/metadata.plist"].encode())
        self.assertEqual(role, {"Settings": {"Role": "User"}})
        self.assertNotIn("patch", cydia.load_manifest())      # 4.x set unchanged

    def test_patch_file(self):
        m = {"patch": [{"path": "/a", "offset": "0x2", "orig": "00d1", "new": "00e0"}]}
        self.assertEqual(cydia.patch_file(m, "/a", b"xx\x00\xd1yy", False), b"xx\x00\xe0yy")
        self.assertEqual(cydia.patch_file(m, "/b", b"xx\x00\xd1yy", False), b"xx\x00\xd1yy")
        with self.assertRaises(SystemExit):
            cydia.patch_file(m, "/a", b"xxzzyy", False)

    def test_recipe_and_release(self):
        with open(os.path.join(ROOT, "rootfs", "recipe.json")) as f:
            recipe = json.load(f)
        self.assertEqual(recipe["cydia"], "rootfs/cydia.json")
        with open(os.path.join(ROOT, "scripts", "mkrelease.py")) as f:
            rel = f.read()
        self.assertIn('"rootfs/cydia.json"', rel)
        self.assertNotIn(".deb", rel)                         # never shipped

    def test_icon_state(self):
        """Cydia on the first home screen page, Apple's layout otherwise unchanged (re-log 77)"""
        with open(os.path.join(ROOT, "rootfs", "recipe.json")) as f:
            recipe = json.load(f)
        st = recipe["cydia_plist_write"]["/private/var/mobile/Library/SpringBoard/IconState.plist"]
        plistlib.dumps(st)                                    # serialisable as written
        pages = st["iconLists"]
        self.assertIn("com.saurik.Cydia", pages[0])
        self.assertTrue(all(len(p) <= 16 for p in pages))     # 4x4 grid on the iPod's screen
        self.assertLessEqual(len(st["buttonBar"]), 4)
        ids = list(st["buttonBar"])
        folders = []
        for p in pages:
            for e in p:
                if isinstance(e, dict):
                    folders.append(e)
                    ids += [i for fp in e["iconLists"] for i in fp]
                else:
                    ids.append(e)
        self.assertEqual(len(ids), len(set(ids)))
        self.assertEqual([(f["displayName"], f["iconLists"]) for f in folders],
                         [("Utilities", [["com.apple.mobiletimer", "com.apple.calculator", "com.apple.VoiceMemos"]])])
        self.assertEqual(len(ids), 21)          # the 20 stock icons of 4.2.1 on the iPod2,1 + Cydia

    def test_switch(self):
        with mock.patch.dict(os.environ, {"IOS3DS_CYDIA": "0"}):
            self.assertFalse(cydia.enabled({"cydia": "rootfs/cydia.json"}))
        with mock.patch.dict(os.environ, {"IOS3DS_CYDIA": "1"}):
            self.assertTrue(cydia.enabled({"cydia": "rootfs/cydia.json"}))
            self.assertFalse(cydia.enabled({"cydia": False}))
        with mock.patch.dict(os.environ, {"IOS3DS_TLSFIX": "0"}):
            self.assertFalse(cydia.tlsfix_enabled({"tlsfix": True}))
        with mock.patch.dict(os.environ, {"IOS3DS_TLSFIX": "1"}):
            self.assertTrue(cydia.tlsfix_enabled({"tlsfix": True}))
            self.assertTrue(cydia.tlsfix_enabled({}))
            self.assertFalse(cydia.tlsfix_enabled({"tlsfix": False}))


class Deb(unittest.TestCase):
    def test_norm(self):
        self.assertIsNone(cydia.norm("./"))
        self.assertEqual(cydia.norm("./usr/bin/"), "/usr/bin")
        self.assertEqual(cydia.norm("bin/bash"), "/bin/bash")
        with self.assertRaises(ValueError):
            cydia.norm("./usr/../../etc/passwd")

    def test_install(self):
        with tempfile.TemporaryDirectory() as td:
            a, b = os.path.join(td, "a.deb"), os.path.join(td, "b.deb")
            make_deb(a, "Package: foo\nVersion: 1.0-1\nArchitecture: iphoneos-arm\n", [
                ("./", "dir", None, 0o755), ("./usr/", "dir", None, 0o755),
                ("./usr/bin/", "dir", None, 0o755),
                ("./usr/bin/foo", "file", b"\xcf\xfa", 0o755),
                ("./usr/bin/sufoo", "file", b"x", 0o4755),
                ("./usr/bin/bar", "sym", "foo", 0o777),
                ("./etc/", "dir", None, 0o755), ("./etc/foo.conf", "file", b"conf\n", 0o644),
                ("./var/", "dir", None, 0o755), ("./var/lib/", "dir", None, 0o755),
                ("./var/lib/foo/", "dir", None, 0o700),
            ], scripts={"postinst": b"#!/bin/sh\n", "conffiles": b"/etc/foo.conf\n"})
            make_deb(b, "Package: firmware-sbin\nVersion: 0-1\n", [
                ("./", "dir", None, 0o755), ("./sbin/", "dir", None, 0o755),
                ("./sbin/fsck", "file", b"", 0o644)], lzma_data=False)
            fake = FakeHfs()
            m = {"packages": [], "keep_stock": ["firmware-sbin"],
                 "pseudo": [{"package": "firmware", "version": "4.2.1", "description": "x"}],
                 "mkdirs": ["/var/lib/apt/lists/partial"], "write": {"/.cydia_no_stash": ""}}
            with mock.patch("sys.stdout", new=io.StringIO()):
                cydia.install("img", fake, [({}, a), ({}, b)], m)
            fs = fake.fs
            self.assertEqual(fs["/usr/bin/foo"][:2], ("f", 0o755))
            self.assertEqual(fs["/usr/bin/sufoo"][1], 0o4755)
            self.assertEqual(fs["/usr/bin/bar"], ("l", 0o755, "foo"))
            self.assertEqual(fs["/private/var/lib/foo"][1], 0o700)
            self.assertEqual(fs["/private/etc/foo.conf"][2], b"conf\n")
            self.assertEqual(fs["/sbin/fsck"][2], b"stock")    # firmware-sbin keeps Apple's tool
            self.assertIn("/private/var/lib/apt/lists/partial", fs)
            self.assertIn("/.cydia_no_stash", fs)
            status = fs["/private/var/lib/dpkg/status"][2].decode()
            self.assertEqual(re.findall(r"^Package: (.*)$", status, re.M),
                             ["firmware", "firmware-sbin", "foo"])
            self.assertEqual(status.count("Status: install ok installed"), 3)
            self.assertIn("Conffiles:\n /etc/foo.conf %s" % hashlib.md5(b"conf\n").hexdigest(), status)
            lst = fs["/private/var/lib/dpkg/info/foo.list"][2].decode().split("\n")
            self.assertEqual(lst[0], "/.")
            self.assertIn("/usr/bin/bar", lst)
            self.assertIn("/var/lib/foo", lst)
            self.assertEqual(fs["/private/var/lib/dpkg/info/foo.postinst"][1], 0o755)
            self.assertIn("/sbin/fsck", fs["/private/var/lib/dpkg/info/firmware-sbin.list"][2].decode())

    def test_substrate_setup(self):
        """Substrate goes after our own interposers in the jobs that insert one."""
        fake = FakeHfs()
        for d in ("/System", "/System/Library", "/System/Library/LaunchDaemons"):
            fake.fs[d] = ("d", 0o755, None)
        sb = "/System/Library/LaunchDaemons/com.apple.SpringBoard.plist"
        other = "/System/Library/LaunchDaemons/com.apple.other.plist"
        fake.fs[sb] = ("f", 0o644, plistlib.dumps({"Label": "sb", "EnvironmentVariables": {
            "DYLD_INSERT_LIBRARIES": "/usr/lib/ios3ds-applog.dylib", "X": "1"}}))
        fake.fs[other] = ("f", 0o644, plistlib.dumps({"Label": "o"}))
        recipe = {"plist_set": {sb: {"EnvironmentVariables": {"DYLD_INSERT_LIBRARIES": "x"}},
                                other: {"StandardErrorPath": "/dev/console"}}}
        m = {"substrate": {"insert": "/Library/MobileSubstrate/MobileSubstrate.dylib"}}
        cat = lambda img, path: fake.fs[path][2]                    # noqa: E731
        with mock.patch("sys.stdout", new=io.StringIO()):
            cydia.substrate_setup("img", fake, cat, recipe, m)
            cydia.substrate_setup("img", fake, cat, recipe, m)   # idempotent
        env = plistlib.loads(fake.fs[sb][2])["EnvironmentVariables"]
        self.assertEqual(env["DYLD_INSERT_LIBRARIES"],
                         "/usr/lib/ios3ds-applog.dylib:/Library/MobileSubstrate/MobileSubstrate.dylib")
        self.assertEqual(env["X"], "1")
        self.assertEqual(plistlib.loads(fake.fs[other][2]), {"Label": "o"})

    def test_substrate_guard(self):
        """cynject is diverted behind the pid-1 guard, dpkg knows the diversion, a
        cynject line in launchd.conf is dropped; the manifest holds Substrate."""
        fake = FakeHfs()
        cy = "/Library/Frameworks/CydiaSubstrate.framework/Commands/cynject"
        for d in ("/Library", "/Library/Frameworks", "/Library/Frameworks/CydiaSubstrate.framework",
                  "/Library/Frameworks/CydiaSubstrate.framework/Commands", "/usr", "/usr/libexec",
                  "/private/var/lib", "/private/var/lib/dpkg"):
            fake.fs[d] = ("d", 0o755, None)
        fake.fs[cy] = ("f", 0o2755, b"\xce\xfa real cynject")
        fake.fs["/private/etc/launchd.conf"] = ("f", 0o644, b"setenv X 1\nbsexec .. /usr/bin/cynject 1 /x.dylib\n")
        m = {"substrate": {"insert": "/Library/MobileSubstrate/MobileSubstrate.dylib",
                           "write": {"/usr/libexec/ios3ds-cynject": "#!/bin/bash\n# ios3ds cynject guard\n"},
                           "mode": {"/usr/libexec/ios3ds-cynject": "755"},
                           "divert": {cy: "/usr/libexec/ios3ds-cynject"}}}
        cat = lambda img, path: fake.fs[fake.resolve(path)][2]    # noqa: E731
        with mock.patch("sys.stdout", new=io.StringIO()):
            cydia.substrate_setup("img", fake, cat, {}, m)
        self.assertEqual(fake.fs[cy + ".real"][2], b"\xce\xfa real cynject")
        self.assertIn(b"ios3ds cynject guard", fake.fs[cy][2])
        self.assertEqual(fake.fs[cy][1], 0o755)
        self.assertEqual(fake.fs["/private/var/lib/dpkg/diversions"][2].decode(), "%s\n%s.real\n:\n" % (cy, cy))
        self.assertEqual(fake.fs["/private/etc/launchd.conf"][2], b"setenv X 1\n")
        real = cydia.load_manifest()
        self.assertEqual(set(real["hold"]), {"mobilesubstrate", "com.saurik.substrate.safemode"})
        self.assertIn(cy, real["substrate"]["divert"])
        self.assertIn("Pin: version 0.9.6301", real["substrate"]["write"]["/etc/apt/preferences"])
        self.assertEqual(cydia.status_entry("Package: a\nVersion: 1\n", hold=True).split("\n")[1],
                         "Status: hold ok installed")

    def test_fetch_offline(self):
        m = cydia.load_manifest()
        with tempfile.TemporaryDirectory() as td, mock.patch("sys.stdout", new=io.StringIO()), \
                mock.patch.object(cydia, "_get", side_effect=OSError("offline")), \
                mock.patch.object(cydia.time, "sleep"):
            self.assertIsNone(cydia.fetch(m, td))

    def test_fetch_bad_hash(self):
        m = {"mirrors": ["http://x/"], "packages": [
            {"package": "p", "version": "1", "path": "debs/p.deb", "size": 3, "sha256": "0" * 64}]}
        with tempfile.TemporaryDirectory() as td, mock.patch("sys.stdout", new=io.StringIO()), \
                mock.patch.object(cydia, "_get", return_value=b"abc"):
            self.assertIsNone(cydia.fetch(m, td))
            self.assertEqual(os.listdir(td), [])

    def test_fetch_optional_group(self):
        good = b"deb"
        m = {"mirrors": ["http://x/"], "packages": [
            {"package": "p", "version": "1", "path": "debs/p.deb", "size": 3,
             "sha256": hashlib.sha256(good).hexdigest()},
            {"package": "t", "version": "1", "group": "tlsfix", "url": "https://y/t.deb",
             "path": "t.deb", "size": 3, "sha256": "0" * 64}]}
        with tempfile.TemporaryDirectory() as td, mock.patch("sys.stdout", new=io.StringIO()), \
                mock.patch.object(cydia, "_get", return_value=good) as get:
            got = cydia.fetch(m, td)                          # bad optional deb: only it is dropped
            self.assertEqual([p["package"] for p, _ in got], ["p"])
            self.assertIn(mock.call("https://y/t.deb"), get.call_args_list)
            get.reset_mock()
            got = cydia.fetch(m, td, groups=())               # switched off: not even fetched
            self.assertEqual([p["package"] for p, _ in got], ["p"])
            get.assert_not_called()


if __name__ == "__main__":
    unittest.main()
