"""Offline tests for the old-version app dylib injector (synthetic Mach-O)."""
import os
import struct
import sys
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "scripts"))

import inject_dylib  # noqa: E402


def synthetic(first_section=0x200):
    """A minimal 32-bit Mach-O: one __TEXT segment with one file-backed section."""
    cmdsize = 56 + 68
    header = struct.pack("<7I", 0xFEEDFACE, 12, 6, 2, 1, cmdsize, 0)
    seg = struct.pack("<II", 1, cmdsize) + b"__TEXT".ljust(16, b"\0")
    seg += struct.pack("<8I", 0x1000, 0x1000, 0, 0x100, 7, 5, 1, 0)
    sect = b"__text".ljust(16, b"\0") + b"__TEXT".ljust(16, b"\0")
    sect += struct.pack("<10I", 0x1000, 0x100, first_section, 2, 0, 0, 0, 0, 0, 0)
    blob = bytearray(header + seg + sect)
    blob.extend(b"\0" * (first_section - len(blob)))
    blob.extend(b"MAGIC" * 32)
    return bytes(blob)


class TestInjectDylib(unittest.TestCase):
    def test_append_command(self):
        src = synthetic()
        out = inject_dylib.patch(src, "/usr/lib/ios3ds-applog.dylib")
        self.assertNotEqual(src, out)
        self.assertEqual(struct.unpack_from("<II", out, 16), (2, 124 + 56))
        cmd, size, nameoff, stamp, cur, compat = struct.unpack_from("<IIIIII", out, 28 + 124)
        self.assertEqual((cmd, size, nameoff), (0xC, 56, 24))
        self.assertEqual(out[28 + 124 + 24:28 + 124 + size].split(b"\0")[0],
                         b"/usr/lib/ios3ds-applog.dylib")
        self.assertEqual(out[0x200:0x200 + 160], src[0x200:0x200 + 160])

    def test_idempotent(self):
        src = inject_dylib.patch(synthetic(), "/usr/lib/ios3ds-applog.dylib")
        self.assertEqual(inject_dylib.patch(src, "/usr/lib/ios3ds-applog.dylib"), src)

    def test_rejects_small_slack(self):
        with self.assertRaisesRegex(ValueError, "slack"):
            inject_dylib.patch(synthetic(0xC0), "/usr/lib/ios3ds-applog.dylib")

    def test_rejects_wrong_magic(self):
        with self.assertRaises(ValueError):
            inject_dylib.patch(b"not a mach-o at all", "/usr/lib/ios3ds-applog.dylib")


if __name__ == "__main__":
    unittest.main()
