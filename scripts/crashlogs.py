#!/usr/bin/env python3
"""Copy CrashReporter reports out of an ios3ds SD image or rootfs.hfs.

Usage:
    python3 scripts/crashlogs.py IMAGE OUTDIR [options]

IMAGE is either a raw SD card image (the FAT partition at offset 1 MiB is
searched for /ios3ds/rootfs.hfs, as in emu/session.py) or a rootfs.hfs itself.
OUTDIR is created if needed; every *.plist, *.crash and *.panic under
CrashReporter is written there, with the HFS path flattened into the file name
so reports from different directories cannot overwrite each other.

The iOS disk is only read.  For a real console, put the SD card in a PC and
pass its raw device/image (e.g. /dev/sdX on Linux, /dev/disk2 on macOS) or the
ios3ds/rootfs.hfs file copied from it.  A raw SD image needs a temporary 1 GiB
copy of the HFS file because xpwn's hfsplus tool cannot read a partition offset;
the copy is deleted afterwards.  --tmpdir chooses where it is made (default:
this checkout's build/ directory, which is expected to have room).

Options:
    --hfsplus TOOL   xpwn hfsplus binary (default: build/tools/hfsplus)
    --tmpdir DIR     parent directory for the temporary rootfs.hfs copy
    -v               print each HFS path that is copied
"""
import argparse
import os
import re
import shutil
import stat
import subprocess
import sys
import tempfile

ROOT = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
HFS_PATHS = (
    "/private/var/mobile/Library/Logs/CrashReporter",
    "/Library/Logs/CrashReporter",
    "/private/var/log/CrashReporter",
)
SUFFIXES = (".plist", ".crash", ".panic")
# xpwn hfsplus ls: mode uid gid size month day time/year name
LS_RE = re.compile(r"^(\d{6})\s+(\d+)\s+(\d+)\s+(\d+)\s+\S+\s+\S+\s+(?:\d+:\d\d\s+)?(.+?)\s*$")


def is_hfs_image(path):
    """True if path starts with an HFS+/HFSX volume header ('H+'/'HX' at 0x400).

    ios3ds images are HFSX (case sensitive), as made by mkfs.hfsplus -s.
    """
    try:
        with open(path, "rb") as f:
            f.seek(0x400)
            return f.read(2) in (b"H+", b"HX")
    except OSError:
        return False


def wanted(name):
    return name.lower().endswith(SUFFIXES)


def parse_ls(text):
    """Return [(mode, size, name)] for hfsplus ls output; skip its footer."""
    entries = []
    for line in (text or "").splitlines():
        m = LS_RE.match(line)
        if not m:
            continue
        name = m.group(5).strip()
        if not name or name.endswith(":") or name.startswith("Total filesystem"):
            continue
        entries.append((m.group(1), int(m.group(4)), name))
    return entries


def run(cmd, **kw):
    return subprocess.run(cmd, capture_output=True, text=True, errors="replace", **kw)


def list_dir(hfs, image, path):
    r = run([hfs, image, "ls", path])
    out = r.stdout + r.stderr
    if "No such file or directory" in out:
        return []
    return parse_ls(out)


def find_reports(hfs, image, verbose=False):
    """Yield (hfs_path, size) for every crash report in the known directories."""
    seen = set()
    for base in HFS_PATHS:
        pending = [base]
        while pending:
            path = pending.pop(0)
            if path in seen:
                continue
            seen.add(path)
            entries = list_dir(hfs, image, path)
            if not entries and path == base and verbose:
                print("[crashlogs] %s: absent" % path, file=sys.stderr)
            for mode, size, name in entries:
                child = path.rstrip("/") + "/" + name
                try:
                    is_dir = stat.S_ISDIR(int(mode, 8))
                except ValueError:
                    continue
                if is_dir:
                    pending.append(child)
                elif wanted(name):
                    yield child, size


def sd_to_temp(sd, path):
    """Stream /ios3ds/rootfs.hfs out of the FAT partition of an SD image."""
    part = "%s@@%d" % (sd, 2048 * 512)
    r = subprocess.run(["mcopy", "-i", part, "::/ios3ds/rootfs.hfs", path],
                       capture_output=True, text=True)
    if r.returncode:
        raise SystemExit("crashlogs: cannot read /ios3ds/rootfs.hfs from %s:\n%s%s" %
                         (sd, r.stdout, r.stderr))
    return path


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("image", help="rootfs.hfs or raw SD image")
    ap.add_argument("outdir", help="directory to write reports into")
    ap.add_argument("--hfsplus", default=os.path.join(ROOT, "build", "tools", "hfsplus"))
    ap.add_argument("--tmpdir", default=os.path.join(ROOT, "build"))
    ap.add_argument("-v", "--verbose", action="store_true")
    a = ap.parse_args()
    if os.path.isdir(a.image):
        # mounted SD card: its ios3ds/rootfs.hfs is already a plain file
        a.image = os.path.join(a.image, "ios3ds", "rootfs.hfs")
    if not os.path.isfile(a.image):
        raise SystemExit("crashlogs: no such image: %s" % a.image)
    if not os.path.isfile(a.hfsplus):
        raise SystemExit("crashlogs: hfsplus tool not found: %s (build it with 'make hfsplus')" %
                         a.hfsplus)

    os.makedirs(a.outdir, exist_ok=True)
    os.makedirs(a.tmpdir, exist_ok=True)
    tmp = tempfile.mkdtemp(prefix="crashlogs-", dir=a.tmpdir)
    try:
        image = a.image
        if not is_hfs_image(image):
            image = os.path.join(tmp, "rootfs.hfs")
            sd_to_temp(a.image, image)
            if not is_hfs_image(image):
                raise SystemExit("crashlogs: %s is neither rootfs.hfs nor an ios3ds SD image" %
                                 a.image)
        found = list(find_reports(a.hfsplus, image, a.verbose))
        for path, size in found:
            flat = path.strip("/").replace("/", "__")
            out = os.path.join(a.outdir, flat)
            r = run([a.hfsplus, image, "extract", path, out])
            if r.returncode or not os.path.isfile(out):
                print("crashlogs: failed to extract %s: %s%s" % (path, r.stdout, r.stderr),
                      file=sys.stderr)
                continue
            if a.verbose:
                print("[crashlogs] %s (%d bytes)" % (path, size))
            print(out)
        if not found:
            print("crashlogs: no CrashReporter reports found in %s" % a.image)
        return 0
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
