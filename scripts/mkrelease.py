#!/usr/bin/env python3
"""Package an ios3ds release zip: our own builds + the scripts the installer needs.

  scripts/mkrelease.py [--version v0.1] [--out build/release]

Takes the build outputs from build/ (make firm shim launcher userland, and
scripts/build-hfsplus.sh --static) and writes build/release/ios3ds-<version>.zip
with one top folder ios3ds-<version>/:

  install.py                      the installer (runs scripts/install.py): IPSW -> SD card tree
  INSTALL.md README.md LICENSE
  prebuilt/ios3ds.firm            3DS payload (ARM9 monitor + ARM11 stub)
  prebuilt/ios3ds.3dsx            Homebrew Launcher app
  prebuilt/shim.bin, gpu.bin      kernel shim + its GPU part (unconfigured: mkboot patches them)
  prebuilt/rootfs-add/...         our iOS binaries (input daemon, GL engine, ...)
  prebuilt/hfsplus-linux-x86_64   xpwn's HFS+ editor, static (source: src/xpwn-hfsplus/)
  prebuilt/MANIFEST.json          sha256, size and provenance of every prebuilt file
  scripts/ kernel/ rootfs/ tools/ the Python build scripts, recipes and plists the
                                  installer runs (same files as in the repository)
  src/xpwn-hfsplus/               the hfsplus sources (GPL-3.0) + how to rebuild them

Nothing derived from Apple's firmware may go in (docs/release.md). Everything
Apple-derived (kernel.bin, devtree.bin, rootfs.hfs, keys) is generated on the
user's PC from their own IPSW by install.py. check_clean() enforces that.
"""
import argparse
import glob
import hashlib
import json
import os
import subprocess
import sys
import time
import zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
B = os.path.join(ROOT, "build")

# (zip path under prebuilt/, build path, provenance)
PREBUILT = [
    ("ios3ds.firm", "boot/ios3ds.firm", "ours: boot/ (ARM9 monitor + ARM11 stub), arm-none-eabi-gcc"),
    ("VERSION", "boot/VERSION", "ours: payload version string"),
    ("ios3ds.3dsx", "launcher/ios3ds.3dsx", "ours: tools/ios3ds-launcher, devkitARM + libctru (libctru: zlib licence)"),
    ("shim.bin", "shim/shim.bin", "ours: kernel/shim + tools/*/shim, arm-none-eabi-gcc (mkboot.py --shim-only)"),
    ("gpu.bin", "shim/gpu.bin", "ours: the shim's PICA200 part + tools/ios3ds-pica mirror command lists"),
    ("hfsplus-linux-x86_64", "tools/hfsplus-linux-x86_64", "xpwn hfsplus (GPL-3.0, planetbeing/xpwn), static; source in src/xpwn-hfsplus"),
]
ROOTFS_ADD = "rootfs-add"       # build/rootfs-add/** -> prebuilt/rootfs-add/**, ours (make userland)

# repository files the installer imports or reads (paths kept as in the repo)
SOURCES = [
    "scripts/fwprep.py", "scripts/mkboot.py", "scripts/mkrootfs.py", "scripts/mksd.py",
    "scripts/inject_dylib.py",
    "scripts/install.py", "scripts/versions.py", "scripts/kpi.py", "scripts/fwlib/*.py",
    "kernel/dtgen.py", "kernel/patcher.py", "kernel/patches*.json",
    # per-version patch sets and rootfs overlays (experimental IOS=4.1 build)
    "kernel/patchsets/*.json", "rootfs/overlay-*.json",
    "tools/ios3ds-audio/dt_audio.py", "tools/ios3ds-mcu/dt_mcu.py",
    "tools/ios3ds-wifi/ios3ds-wifi-fw.gm9", "tools/ios3ds-wifi/nwmfw.py",
    "scripts/cydia.py", "scripts/certs.py", "scripts/music.py", "scripts/carryover.py", "scripts/crashlogs.py", "rootfs/recipe.json", "rootfs/cydia.json",
    "rootfs/files/*", "rootfs/certs/roots.pem", "rootfs/certs/roots-ec.pem",
    "README.md", "LICENSE", "docs/INSTALL.md",
]
XPWN_FILES = ["hfs/*.c", "common/abstractfile.c", "common/base64.c", "includes/**/*.h", "LICENSE",
              "README.markdown"]
XPWN_REV = "20c32e5c12d1b22a9d55a59a0ff6267f539b77f4"

INSTALL_WRAPPER = """#!/usr/bin/env python3
# ios3ds installer: see README.md (Install).  python3 install.py IPSW [SD]
import os, runpy, sys
sys.argv[0] = os.path.abspath(__file__)
runpy.run_path(os.path.join(os.path.dirname(os.path.abspath(__file__)), "scripts", "install.py"),
               run_name="__main__")
"""

# Apple-derived outputs of fwprep/mkboot/mkrootfs: never in a release
FORBIDDEN_NAMES = ("kernel.bin", "devtree.bin", "rootfs.hfs", "kernelcache", "devicetree",
                   "dyld_shared_cache", "fakeio.bin", "boot.cfg", "manifest.json")
FORBIDDEN_EXT = (".ipsw", ".img3", ".dmg", ".hfs", ".macho")
# content signatures of Apple images: IMG3, decompressed/compressed kernelcache,
# dyld cache, Apple flat device tree marker
MAGICS = (b"3gmI", b"complzss", b"dyld_v1 ", b"Darwin Kernel Version")


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for b in iter(lambda: f.read(1 << 20), b""):
            h.update(b)
    return h.hexdigest()


def check_clean(name, data):
    """Refuse anything that looks Apple-derived (by name or content)."""
    base = os.path.basename(name).lower()
    if base.startswith("keys-") or base.endswith(FORBIDDEN_EXT) or \
            any(base.startswith(n) for n in FORBIDDEN_NAMES) or "staging/" in name:
        raise SystemExit("[mkrelease] refusing Apple-derived file name: %s" % name)
    if len(data) >= 1026 and data[1024:1026] in (b"H+", b"HX"):
        raise SystemExit("[mkrelease] refusing HFS+ image: %s" % name)
    for m in MAGICS:
        if m in data:
            raise SystemExit("[mkrelease] refusing %s: contains %r (Apple firmware content?)" % (name, m))


def missing_imports(srcs):
    """Fail if a packaged script imports a module that is a repo .py file but
    not in the zip (issue #4: 0.3.0 shipped kernel/dtgen.py without
    tools/ios3ds-mcu/dt_mcu.py, so install.py died at 'import dt_mcu')."""
    import ast
    import subprocess
    tracked = subprocess.run(["git", "-C", ROOT, "ls-files", "*.py"], capture_output=True, text=True).stdout.split()
    repo_mods = {}
    for t in tracked:
        repo_mods.setdefault(os.path.splitext(os.path.basename(t))[0], []).append(t)
    shipped = {os.path.splitext(os.path.basename(r))[0] for r in srcs if r.endswith(".py")}
    bad = []
    for rel in sorted(r for r in srcs if r.endswith(".py")):
        tree = ast.parse(open(os.path.join(ROOT, rel), encoding="utf-8").read(), rel)
        for n in ast.walk(tree):
            names = [a.name for a in n.names] if isinstance(n, ast.Import) else \
                    [n.module] if isinstance(n, ast.ImportFrom) and n.module and not n.level else []
            for name in names:
                top = name.split(".")[0]
                if top in repo_mods and top not in shipped and not any(
                        r.startswith(top + "/") or "/%s/" % top in "/" + r for r in srcs):
                    bad.append("%s imports %s (%s)" % (rel, top, ", ".join(repo_mods[top])))
    if bad:
        raise SystemExit("[mkrelease] the installer would fail, modules missing from SOURCES:\n  " + "\n  ".join(bad))


def userland_list():
    root = os.path.join(B, ROOTFS_ADD)
    recipe = json.load(open(os.path.join(ROOT, "rootfs", "recipe.json")))
    need = sorted(l[len("build/rootfs-add/"):] for l in recipe["add"] if l.startswith("build/rootfs-add/"))
    have = sorted(os.path.relpath(os.path.join(d, f), root)
                  for d, _, fs in os.walk(root) for f in fs) if os.path.isdir(root) else []
    missing = [n for n in need if n not in have]
    if missing:
        raise SystemExit("[mkrelease] build/rootfs-add is missing %s (make userland)" % ", ".join(missing))
    return need


def recipe_sources():
    """Repo files (plists) the rootfs recipe adds, besides build/rootfs-add/."""
    recipe = json.load(open(os.path.join(ROOT, "rootfs", "recipe.json")))
    return sorted(l for l in recipe["add"] if not l.startswith("build/"))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--version", help="release name (default: git describe)")
    ap.add_argument("--out", default=os.path.join(B, "release"))
    a = ap.parse_args()
    version = a.version or subprocess.run(["git", "-C", ROOT, "describe", "--always", "--tags", "--dirty"],
                                          capture_output=True, text=True).stdout.strip() or "dev"
    top = "ios3ds-%s" % version
    os.makedirs(a.out, exist_ok=True)
    zpath = os.path.join(a.out, top + ".zip")

    files = []          # (zip path, source path, mode)
    manifest = {"version": version, "files": {}}
    for zp, bp, why in PREBUILT:
        src = os.path.join(B, bp)
        if not os.path.exists(src):
            raise SystemExit("[mkrelease] missing build/%s (see the Makefile: firm, shim, launcher; "
                             "scripts/build-hfsplus.sh --static)" % bp)
        files.append(("prebuilt/" + zp, src, 0o755 if zp.startswith("hfsplus") else 0o644))
        manifest["files"]["prebuilt/" + zp] = {"from": why}
    for rel in userland_list():
        src = os.path.join(B, ROOTFS_ADD, rel)
        zp = "prebuilt/rootfs-add/" + rel
        files.append((zp, src, 0o755))
        manifest["files"][zp] = {"from": "ours: armv6 iOS Mach-O built with scripts/ios-cc (make userland)"}
    srcs = set()
    for pat in SOURCES:
        hits = glob.glob(os.path.join(ROOT, pat))
        if not hits:
            raise SystemExit("[mkrelease] no file matches %s" % pat)
        srcs.update(os.path.relpath(h, ROOT) for h in hits if os.path.isfile(h))
    srcs.update(recipe_sources())
    missing_imports(srcs)
    for rel in sorted(srcs):
        files.append((rel, os.path.join(ROOT, rel), 0o755 if rel.endswith(".py") else 0o644))
    xpwn = os.path.join(B, "xpwn")
    for pat in XPWN_FILES:
        hits = [h for h in glob.glob(os.path.join(xpwn, pat), recursive=True) if os.path.isfile(h)]
        if not hits:
            raise SystemExit("[mkrelease] build/xpwn has no %s (scripts/build-hfsplus.sh)" % pat)
        for h in sorted(hits):
            files.append(("src/xpwn-hfsplus/" + os.path.relpath(h, xpwn), h, 0o644))

    for zp, src, _ in files:
        with open(src, "rb") as f:
            data = f.read()
        # names: everything; content signatures: the binaries (the scripts name them on purpose)
        check_clean(zp, data if zp.startswith("prebuilt/") else b"")
        if zp.startswith("prebuilt/"):
            manifest["files"][zp].update(size=len(data), sha256=hashlib.sha256(data).hexdigest())

    readme = ("ios3ds %s\n\nInstall: see README.md (section Install), or run\n\n"
              "  python3 install.py /path/to/iPod2,1_4.2.1_8C148_Restore.ipsw /path/to/sdcard\n\n"
              "src/xpwn-hfsplus: source of prebuilt/hfsplus-linux-x86_64 (GPL-3.0), "
              "https://github.com/planetbeing/xpwn at %s, built with:\n"
              "  cc -O2 -w -Iincludes hfs/*.c common/abstractfile.c common/base64.c -lz -o hfsplus\n"
              % (version, XPWN_REV))
    with zipfile.ZipFile(zpath + ".tmp", "w", zipfile.ZIP_DEFLATED) as z:
        for zp, src, mode in files:
            zi = zipfile.ZipInfo.from_file(src, top + "/" + zp)
            zi.external_attr = (0o100000 | mode) << 16
            zi.compress_type = zipfile.ZIP_DEFLATED
            with open(src, "rb") as f:
                z.writestr(zi, f.read())
        zi = zipfile.ZipInfo(top + "/install.py", date_time=time.localtime()[:6])
        zi.external_attr = (0o100755) << 16
        z.writestr(zi, INSTALL_WRAPPER)
        z.writestr(top + "/prebuilt/MANIFEST.json", json.dumps(manifest, indent=1, sort_keys=True) + "\n")
        z.writestr(top + "/src/xpwn-hfsplus/BUILD.txt", readme)
    os.replace(zpath + ".tmp", zpath)
    print("[mkrelease] %s: %d files, %d KiB" % (zpath, len(files) + 3, os.path.getsize(zpath) >> 10))
    for zp in sorted(manifest["files"]):
        e = manifest["files"][zp]
        print("[mkrelease]   %-80s %8d  %s" % (zp, e["size"], e["from"]))


if __name__ == "__main__":
    main()
