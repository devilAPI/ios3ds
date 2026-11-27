#!/usr/bin/env python3
"""Host test for the ARM9's dirty-volume repair (boot/arm9/hfsfix.c, re-log launchdhang).

Builds a small HFS+ image (mkfs.hfsplus + xpwn's build/tools/hfsplus), damages
the allocation bitmap and the directory valences the way an interrupted run
does, runs the same C code the ARM9 uses on the file and checks that the bitmap
and the valences are back (and that fsck.hfsplus agrees, when present). The
tests are skipped without mkfs.hfsplus or the hfsplus tool.
"""
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MKFS = shutil.which("mkfs.hfsplus")
FSCK = shutil.which("fsck.hfsplus")
HFSPLUS = os.path.join(ROOT, "build", "tools", "hfsplus")
CC = shutil.which("cc") or shutil.which("gcc")


def vh(img):
    with open(img, "rb") as f:
        f.seek(1024)
        return f.read(512)


def be32(b, o):
    return struct.unpack_from(">I", b, o)[0]


class HfsFixTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not (MKFS and CC and os.path.exists(HFSPLUS)):
            raise unittest.SkipTest("mkfs.hfsplus, a C compiler or build/tools/hfsplus is missing")
        cls.tmp = tempfile.mkdtemp(prefix="hfsfix")
        cls.tool = os.path.join(cls.tmp, "hfsfix_tool")
        subprocess.check_call([CC, "-O1", "-Wall", "-I" + os.path.join(ROOT, "boot", "common"),
                               "-I" + os.path.join(ROOT, "boot", "arm9"), "-o", cls.tool,
                               os.path.join(ROOT, "tests", "hfsfix_tool.c"),
                               os.path.join(ROOT, "boot", "arm9", "hfsfix.c"),
                               os.path.join(ROOT, "boot", "common", "util.c")],
                              stderr=subprocess.DEVNULL)

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.tmp, ignore_errors=True)

    def image(self):
        img = os.path.join(self.tmp, "t%d.img" % id(self))
        with open(img, "wb") as f:
            f.truncate(24 * 1024 * 1024)
        subprocess.check_call([MKFS, "-s", "-v", "T", img], stdout=subprocess.DEVNULL)
        src = os.path.join(self.tmp, "src")
        os.makedirs(src, exist_ok=True)
        for i, size in enumerate((10, 5000, 70000, 300000, 1500000)):
            with open(os.path.join(src, "f%d" % i), "wb") as f:
                f.write(os.urandom(size))
        subprocess.check_call([HFSPLUS, img, "mkdir", "/a"], stdout=subprocess.DEVNULL)
        subprocess.check_call([HFSPLUS, img, "mkdir", "/a/b"], stdout=subprocess.DEVNULL)
        for i in range(5):
            for d in ("/", "/a/", "/a/b/"):
                subprocess.check_call([HFSPLUS, img, "add", os.path.join(src, "f%d" % i), d + "f%d" % i],
                                      stdout=subprocess.DEVNULL)
        return img

    def bitmap(self, img):
        h = vh(img)
        bs = be32(h, 40)
        start, count = struct.unpack_from(">II", h, 112 + 16)
        self.assertEqual(count * bs >= be32(h, 44) // 8, True)
        return start * bs, (be32(h, 44) + 7) // 8

    def run_tool(self, img):
        out = subprocess.run([self.tool, img], capture_output=True, text=True)
        return out.returncode, out.stdout

    def test_bitmap_rebuilt(self):
        img = self.image()
        off, n = self.bitmap(img)
        with open(img, "rb") as f:
            f.seek(off)
            good = f.read(n)
        free = be32(vh(img), 48)
        with open(img, "r+b") as f:          # all blocks read as free, count as before the writes
            f.seek(off)
            f.write(b"\0" * n)
        rc, out = self.run_tool(img)
        self.assertEqual(rc, 0, out)
        with open(img, "rb") as f:
            f.seek(off)
            self.assertEqual(f.read(n), good)
        self.assertEqual(be32(vh(img), 48), free)
        rc, out = self.run_tool(img)         # second run: nothing left to do
        self.assertEqual(rc, 0, out)
        self.assertIn("fixed=0 ", out)
        if FSCK:
            self.assertEqual(subprocess.run([FSCK, "-fn", img], capture_output=True).returncode, 0)

    def test_only_missing_bits_are_set(self):
        img = self.image()
        off, n = self.bitmap(img)
        with open(img, "r+b") as f:          # a leaked block (marked used, owned by nothing) stays
            f.seek(off + n - 8)
            leak = f.read(1)
            f.seek(off + n - 8)
            f.write(bytes([leak[0] | 0x01]))
            f.seek(off + n - 8)
            expect = f.read(1)
        rc, out = self.run_tool(img)
        self.assertEqual(rc, 0, out)
        with open(img, "rb") as f:
            f.seek(off + n - 8)
            self.assertEqual(f.read(1), expect)

    def test_valence_fixed(self):
        img = self.image()
        sys.path.insert(0, os.path.join(ROOT, "emu"))
        import hfsmap
        h = hfsmap.HFS(img)
        size, blocks, ext = h.special["[catalog B-tree]"]
        data = h.fork_bytes(ext)
        nsz = struct.unpack_from(">H", data, 32)[0]
        folders = {}
        for node, o in h.leaves(ext):
            klen, parent, nlen = struct.unpack_from(">HIH", node, o)
            name = node[o + 8:o + 8 + 2 * nlen].decode("utf-16-be")
            d = o + 2 + klen
            d += d & 1
            if struct.unpack_from(">h", node, d)[0] == 1 and name in ("a", "b"):
                # byte position of the valence field of this record in the image
                folders[name] = (bytes(node), d + 4)
        self.assertEqual(set(folders), {"a", "b"})
        # find the on-disk positions by scanning the catalog fork for the record bytes
        pos = {}
        for s, c in ext:
            blob = h.read(s * h.bs, c * h.bs)
            for name, (node, voff) in folders.items():
                rec = node[voff - 4:voff + 12]      # type, flags, valence, id, ...
                k = blob.find(rec)
                if k >= 0:
                    pos[name] = s * h.bs + k + 4
        h.f.close()
        self.assertEqual(set(pos), {"a", "b"})
        with open(img, "r+b") as f:
            for k in pos:
                f.seek(pos[k])
                f.write(struct.pack(">I", 99))
        rc, out = self.run_tool(img)
        self.assertEqual(rc, 0, out)
        self.assertIn("valences_fixed=2", out)
        with open(img, "rb") as f:
            for k in pos:
                f.seek(pos[k])
                self.assertEqual(struct.unpack(">I", f.read(4))[0], 6 if k == "a" else 5)  # b: 5 files; a: 5 files + b
        if FSCK:
            self.assertEqual(subprocess.run([FSCK, "-fn", img], capture_output=True).returncode, 0)

    def test_journaled_is_left_alone(self):
        img = self.image()
        with open(img, "r+b") as f:
            f.seek(1024 + 4)
            f.write(struct.pack(">I", be32(vh(img), 4) | 0x2000))
        rc, out = self.run_tool(img)
        self.assertEqual(rc, 1)
        self.assertIn("rc=-2", out)


if __name__ == "__main__":
    unittest.main()
