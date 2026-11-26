#!/usr/bin/env python3
"""Check that every symbol our iOS binaries import is in the CI stub SDK.

CI links make userland against a stub SDK generated from
scripts/ios-sdk-symbols.json, while a local build may link against a fuller
SDK. A new libc/CoreFoundation call then builds locally but fails CI with
"Undefined symbols". This compares `nm -u` of every Mach-O in
build/rootfs-add with that list. Skipped (exit 0) without a toolchain or
without built binaries.
"""
import json
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
NM = os.path.join(ROOT, "build", "toolchain", "bin", "arm-apple-darwin10-nm")
ADD = os.path.join(ROOT, "build", "rootfs-add")
MAGIC = (b"\xce\xfa\xed\xfe", b"\xca\xfe\xba\xbe")


def main():
    if not os.path.exists(NM) or not os.path.isdir(ADD):
        print("check-stub-symbols: skipped (no toolchain or no make userland output)")
        return 0
    libs = json.load(open(os.path.join(ROOT, "scripts", "ios-sdk-symbols.json")))["dylibs"]
    have = set()
    for lib in libs.values():
        for k in ("text", "data", "weak", "reexports"):
            have |= set(lib.get(k, []))
    # link-time stand-ins built from source next to the 2.x GL dylib
    # (tools/ios3ds-gl/ios2/stub_*.c: 5H11a OpenGLES, MBX2D, IOMFB names)
    stubs = os.path.join(ROOT, "build", "gl-ios2")
    if os.path.isdir(stubs):
        for f in os.listdir(stubs):
            out = subprocess.run([NM, "-g", os.path.join(stubs, f)], capture_output=True, text=True).stdout
            have |= {l.split()[-1] for l in out.splitlines() if l.strip()}
    missing = {}
    for d, _, files in os.walk(ADD):
        for f in files:
            p = os.path.join(d, f)
            if os.path.islink(p):
                continue
            with open(p, "rb") as fh:
                if fh.read(4) not in MAGIC:
                    continue
            out = subprocess.run([NM, "-u", p], capture_output=True, text=True).stdout.split()
            for s in out:
                if s not in have:
                    missing.setdefault(s, []).append(os.path.relpath(p, ADD))
    if missing:
        for s, ps in sorted(missing.items()):
            print("check-stub-symbols: %s (used by %s) is not in scripts/ios-sdk-symbols.json" % (s, ", ".join(ps[:3])))
        return 1
    print("check-stub-symbols: ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
