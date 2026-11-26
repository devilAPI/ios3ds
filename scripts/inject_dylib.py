#!/usr/bin/env python3
"""Add a dylib dependency to a 32-bit Mach-O without moving any section data.

Old iOS versions have no supported way for SpringBoard to put
DYLD_INSERT_LIBRARIES into the environment of *every* app it launches
(`SBGuardMallocAlways` only exists on 4.x, LSEnvironment is macOS-only), and
setting it in launchd's environment injects into every daemon and hangs the
3.1.3 boot.  A plain LC_LOAD_DYLIB in the app executable avoids both problems:
dyld loads our library before main(), and no environment variable is involved.

Apple's app binaries leave slack between the end of LC_SEGMENT/LC_LOAD_DYLIB
commands and the first section's file offset (600 bytes in 7E18 Maps, 1176 in
5H11a Safari).  A new 56-byte LC_LOAD_DYLIB fits there: we only grow
`ncmds`/`sizeofcmds` and write into the slack, so every file offset and the
code signature blob's *offsets* stay where they were.  Code signing is disabled
in our boot-args / kernel patches, so the changed header is accepted.

Only used by the 3.1.3/2.2.1 rootfs overlays; the default 4.2.1 recipe never
calls this.  SPDX-License-Identifier: GPL-3.0-or-later
"""
import argparse
import struct

MH_MAGIC = 0xFEEDFACE
LC_SEGMENT = 0x1
LC_LOAD_DYLIB = 0xC
DYLIB_COMMAND_HEADER = 24


def patch(blob, dylib):
    """Return `blob` with an LC_LOAD_DYLIB for `dylib` appended (idempotent)."""
    if blob[:4] != struct.pack("<I", MH_MAGIC):
        raise ValueError("not a 32-bit little-endian Mach-O")
    ncmds, sizeofcmds = struct.unpack_from("<II", blob, 16)
    end = 28 + sizeofcmds
    off = 28
    lowest = None
    for _ in range(ncmds):
        cmd, size = struct.unpack_from("<II", blob, off)
        if size < 8 or off + size > end:
            raise ValueError("bad load command size %d at %#x" % (size, off))
        if cmd == LC_SEGMENT:
            nsects = struct.unpack_from("<I", blob, off + 48)[0]
            so = off + 56
            for _ in range(nsects):
                fo = struct.unpack_from("<I", blob, so + 40)[0]
                if fo:
                    lowest = fo if lowest is None else min(lowest, fo)
                so += 68
        elif cmd == LC_LOAD_DYLIB:
            nameoff = struct.unpack_from("<I", blob, off + 8)[0]
            name = blob[off + nameoff:off + size].split(b"\0", 1)[0].decode(errors="replace")
            if name == dylib:
                return blob
        off += size
    if off != end:
        raise ValueError("load commands do not end at %#x" % end)
    if lowest is None:
        raise ValueError("no file-backed segments (nowhere to put a new command)")
    name = dylib.encode() + b"\0"
    size = DYLIB_COMMAND_HEADER + (len(name) + 3) // 4 * 4
    if end + size > lowest:
        raise ValueError("only %d bytes of slack before the first section; need %d"
                         % (lowest - end, size))
    cmd = struct.pack("<IIIIII", LC_LOAD_DYLIB, size, DYLIB_COMMAND_HEADER, 0, 0, 0) + name
    cmd += b"\0" * (size - len(cmd))
    out = bytearray(blob)
    out[end:end + size] = cmd
    struct.pack_into("<II", out, 16, ncmds + 1, sizeofcmds + size)
    return bytes(out)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("input")
    ap.add_argument("output")
    ap.add_argument("--dylib", default="/usr/lib/ios3ds-applog.dylib")
    a = ap.parse_args()
    out = patch(open(a.input, "rb").read(), a.dylib)
    with open(a.output, "wb") as f:
        f.write(out)
    print("[inject_dylib] %s -> %s (%s)" % (a.input, a.output, a.dylib))


if __name__ == "__main__":
    main()
