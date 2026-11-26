#!/usr/bin/env python3
"""Prepare the root filesystem image for ios3ds: copy the staged 4.2.1 rootfs.hfs
and apply rootfs/recipe.json with xpwn's hfsplus tool (build/tools/hfsplus,
built by scripts/build-hfsplus.sh). Output stays in build/ (never committed).

  scripts/mkrootfs.py [--staging build/staging] [--out build/sd/ios3ds/rootfs.hfs]
"""
import argparse
import hashlib
import json
import os
import shutil
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
HFS = os.path.join(ROOT, "build", "tools", "hfsplus")
ADD_PREFIX = "build/rootfs-add/"      # recipe 'add' sources built by our userland Makefiles
LD = "/System/Library/LaunchDaemons"
LD_OFF = "/System/Library/LaunchDaemons-ios3ds-disabled"


def hfs(img, *args, check=True):
    r = subprocess.run([HFS, img] + list(args), capture_output=True, text=True)
    if check and r.returncode:
        raise SystemExit("hfsplus %s failed: %s" % (" ".join(args), r.stderr.strip() or r.stdout.strip()))
    return r.stdout


def hfs_cat(img, path):
    r = subprocess.run([HFS, img, "cat", path], capture_output=True)
    if r.returncode:
        raise SystemExit("hfsplus cat %s failed" % path)
    return r.stdout


def exists(img, path):
    d, _, name = path.rstrip("/").rpartition("/")
    out = hfs(img, "ls", d or "/", check=False)
    return any(l.split()[-1:] == [name] for l in out.splitlines() if l.strip())


def merge_dict(dst, src):
    """recipe "plist_merge": nested dictionaries are merged key by key (e.g. one
    capability added to N72AP.plist's capabilities without restating the rest)."""
    for k, v in src.items():
        if isinstance(v, dict) and isinstance(dst.get(k), dict):
            merge_dict(dst[k], v)
        else:
            dst[k] = v
    return dst


def resolve_patch_offset(data, fp):
    """Where a file patch applies: `offset` (original behaviour) or a unique
    search for `orig` / `orig_sha256` in `data` (other iOS builds)."""
    if "offset" in fp:
        return int(fp["offset"], 0)
    orig = bytes.fromhex(fp["orig"]) if "orig" in fp else None
    if orig is not None:
        hits = []
        pos = 0
        while True:
            i = data.find(orig, pos)
            if i < 0:
                break
            hits.append(i)
            pos = i + 1
    else:
        n = len(bytes.fromhex(fp["new"]))
        hits = [i for i in range(len(data) - n + 1)
                if hashlib.sha256(data[i:i + n]).hexdigest() == fp["orig_sha256"]]
    if len(hits) != 1:
        raise SystemExit("file patch %s: %d matches for original bytes" % (fp.get("path", "?"), len(hits)))
    return hits[0]


def local_path(local, rootfs_add=None):
    """Recipe source path -> file: repo-relative, except build/rootfs-add/ -> rootfs_add."""
    if rootfs_add and local.startswith(ADD_PREFIX):
        return os.path.join(rootfs_add, local[len(ADD_PREFIX):])
    return os.path.join(ROOT, local)


def build(staging, out, recipe_path=None, verbose=True, hfsplus=None, rootfs_add=None,
          cydia=True, cydia_cache=None, tlsfix=True, music=None, test_song=True):
    """hfsplus: path of xpwn's hfsplus tool (default build/tools/hfsplus, built on
    demand); rootfs_add: directory standing in for build/rootfs-add/ (release zip);
    cydia: False leaves Cydia out even if the recipe has it (docs/cydia.md);
    cydia_cache: where the downloaded debs are kept (default build/cydia);
    tlsfix: False leaves TLSFix (and only it) out of the Cydia set;
    music: folder of the user's audio files for the Music app, test_song: False
    leaves out the generated test song (scripts/music.py, docs/audio.md section 10)."""
    global HFS
    if hfsplus:
        HFS = hfsplus
    elif not os.path.exists(HFS):
        subprocess.run([os.path.join(ROOT, "scripts", "build-hfsplus.sh")], check=True)
    recipe = json.load(open(recipe_path or os.path.join(ROOT, "rootfs", "recipe.json")))
    # A version overlay can swap one source file (e.g. a 4.1-only input daemon)
    # without dropping the rest of the base recipe's "add" set.
    add_files = dict(recipe.get("add", {}))
    for old, new in recipe.get("replace_add", {}).items():
        if old in add_files:
            add_files[new] = add_files.pop(old)
    missing = [l for l in add_files if not os.path.exists(local_path(l, rootfs_add))]
    if missing:
        raise SystemExit("[mkrootfs] missing files for the rootfs (build the userland tools: "
                         "make userland):\n  " + "\n  ".join(missing))
    src = os.path.join(staging, "rootfs.hfs")
    if os.path.exists(out):
        os.remove(out)
    shutil.copyfile(src, out)
    for path in recipe.get("remove", []):
        # frees blocks, which 'grow' needs to enlarge the allocation bitmap
        hfs(out, "rm", path)
        if verbose:
            print("[mkrootfs] removed %s" % path)
    if recipe.get("grow_bytes"):
        # Apple's image has 0 bytes free: launchd, syslogd & co need to write
        res = hfs(out, "grow", str(recipe["grow_bytes"]))
        if "grew volume" not in res:
            raise SystemExit("hfsplus grow failed: " + res.strip())
        if verbose:
            print("[mkrootfs] grew volume to %d MiB" % (os.path.getsize(out) >> 20))
    # Older stock rootfs images have no Logs/CrashReporter directory. Without
    # it ReportCrash silently drops the reports we need for older-iOS bring-up.
    # No 4.2.1 recipe sets this key, so its rootfs is unchanged.
    for path in recipe.get("mkdirs", []):
        hfs(out, "mkdir", path, check=False)
        if verbose:
            print("[mkrootfs] ensured directory %s" % path)
    cydia_mod = cydia_set = None
    if recipe.get("cydia") and cydia:
        # before our own files: whatever the recipe writes below wins over a deb
        import cydia as cydia_mod
        cydia_set = cydia_mod.apply(out, hfs, recipe, cydia_cache, verbose, tlsfix)
    elif verbose and recipe.get("cydia"):
        print("[mkrootfs] Cydia left out (--no-cydia)")
    with tempfile.TemporaryDirectory() as td:
        ts = recipe.get("trust_store")
        if ts:
            # current root certificates as user anchors (scripts/certs.py, docs/certificates.md)
            import certs
            tmp = os.path.join(td, "TrustStore.sqlite3")
            n = certs.write_db(tmp, certs.read_pem(os.path.join(ROOT, ts["pem"])))
            if exists(out, ts["path"]):
                hfs(out, "rm", ts["path"])
            hfs(out, "add", tmp, ts["path"])        # owner: _securityd, from /private/var/Keychains
            hfs(out, "chmod", "0644", ts["path"])
            if verbose:
                print("[mkrootfs] trust store %s: %d root certificates" % (ts["path"], n))
            if ts.get("profile"):
                # RSA + EC roots as profile CA payloads: TLSFix's OpenSSL check of EC chains
                ders = certs.read_pem(os.path.join(ROOT, ts["pem"]))
                ders += certs.read_pem(os.path.join(ROOT, ts["ec_pem"])) if ts.get("ec_pem") else []
                tmp = os.path.join(td, "roots.plist")
                n = certs.write_profile(tmp, ders)
                hfs(out, "mkdir", ts["profile"].rsplit("/", 1)[0], check=False)
                if exists(out, ts["profile"]):
                    hfs(out, "rm", ts["profile"])
                hfs(out, "add", tmp, ts["profile"])
                hfs(out, "chmod", "0644", ts["profile"])
                if verbose:
                    print("[mkrootfs] %s: %d root certificates for TLSFix's EC check" % (ts["profile"], n))
        for path, text in recipe.get("write", {}).items():
            tmp = os.path.join(td, "f")
            with open(tmp, "w") as f:
                f.write(text)
            if exists(out, path):
                hfs(out, "rm", path)
            hfs(out, "add", tmp, path)
            if verbose:
                print("[mkrootfs] wrote %s" % path)
        for local, path in add_files.items():
            if exists(out, path):
                hfs(out, "rm", path)
            hfs(out, "add", local_path(local, rootfs_add), path)
            if verbose:
                print("[mkrootfs] added %s -> %s" % (local, path))
        for path, target in recipe.get("symlink", {}).items():
            if exists(out, path):
                hfs(out, "rm", path)
            res = hfs(out, "symlink", path, target)
            if not exists(out, path):
                raise SystemExit("hfsplus symlink %s failed: %s" % (path, res.strip()))
            if verbose:
                print("[mkrootfs] symlink %s -> %s" % (path, target))
        edits = [(p, k, False) for p, k in recipe.get("plist_set", {}).items()]
        edits += [(p, k, True) for p, k in recipe.get("plist_merge", {}).items()]
        for path, keys, deep in edits:
            import plistlib
            tmp = os.path.join(td, "p.plist")
            with open(tmp, "wb") as f:
                f.write(hfs_cat(out, path))
            with open(tmp, "rb") as f:
                pl = plistlib.load(f)
            if deep:
                merge_dict(pl, keys)
            else:
                pl.update(keys)
            with open(tmp, "wb") as f:
                plistlib.dump(pl, f, fmt=plistlib.FMT_XML)
            hfs(out, "rm", path)
            hfs(out, "add", tmp, path)
            if verbose:
                print("[mkrootfs] %s: set %s" % (path, ", ".join(keys)))
        for path, obj in (recipe.get("cydia_plist_write", {}) if cydia_set else {}).items():
            # plists that only make sense with Cydia in the image (SpringBoard's
            # icon layout with Cydia on page 1, re-log 77); new folders and the
            # file inherit their parent's owner (501 under /var/mobile)
            import plistlib
            parent = ""
            for part in path.strip("/").split("/")[:-1]:
                parent += "/" + part
                if not exists(out, parent):
                    hfs(out, "mkdir", parent)
            tmp = os.path.join(td, "w.plist")
            with open(tmp, "wb") as f:
                plistlib.dump(obj, f, fmt=plistlib.FMT_XML)
            if exists(out, path):
                hfs(out, "rm", path)
            hfs(out, "add", tmp, path)
            hfs(out, "chmod", "0644", path)
            if verbose:
                print("[mkrootfs] wrote %s (Cydia in the image)" % path)
        if cydia_set:
            # Cydia Substrate next to our interposers (after the plists above exist)
            cydia_mod.substrate_setup(out, hfs, hfs_cat, recipe, cydia_set, verbose)
        by_path = {}
        for fp in recipe.get("file_patches", []):
            by_path.setdefault(fp["path"], []).append(fp)
        for path, fps in by_path.items():
            tmp = os.path.join(td, "patched")
            with open(tmp, "wb") as f:
                f.write(hfs_cat(out, path))
            with open(tmp, "r+b") as f:
                for fp in fps:
                    new = bytes.fromhex(fp["new"])
                    # "orig": the bytes replaced; "orig_sha256": their hash instead, for
                    # longer patches (keeps Apple's code out of the recipe)
                    orig = bytes.fromhex(fp["orig"]) if "orig" in fp else None
                    if orig is not None and len(orig) != len(new):
                        raise SystemExit("file patch %s: orig/new length differ" % path)
                    if "offset" in fp:
                        off = int(fp["offset"], 0)
                    else:
                        # another iOS build: find the verified original bytes once
                        f.seek(0)
                        off = resolve_patch_offset(f.read(), fp)
                    f.seek(off)
                    cur = f.read(len(new))
                    if (hashlib.sha256(cur).hexdigest() != fp["orig_sha256"]
                            if "orig_sha256" in fp else cur != orig):
                        raise SystemExit("file patch %s @%#x: original bytes differ" % (path, off))
                    f.seek(off)
                    f.write(new)
                    if verbose:
                        print("[mkrootfs] patched %s @%#x: %s" % (path, off, fp.get("why", "")))
            hfs(out, "rm", path)
            hfs(out, "add", tmp, path)
            # re-added files lose their mode; executables set "mode" on a patch
            hfs(out, "chmod", next((fp["mode"] for fp in fps if "mode" in fp), "0644"), path)
        # 2.x has no dyld shared cache: its dylibs are signed individually, so
        # patching one invalidates its code directory. Only recipes that set
        # "resign" (rootfs/overlay-2.2.1.json) re-sign after file_patches; the
        # default 4.2.1 recipe never enters this loop.
        for path, mode in recipe.get("resign", {}).items():
            ldid = os.path.join(ROOT, "build", "toolchain", "bin", "ldid")
            if not os.path.exists(ldid):
                raise SystemExit("resign %s: %s missing (build the iOS toolchain)" % (path, ldid))
            tmp = os.path.join(td, "resign")
            with open(tmp, "wb") as f:
                f.write(hfs_cat(out, path))
            subprocess.run([ldid, "-Hsha1", "-S", tmp], check=True)
            hfs(out, "rm", path)
            hfs(out, "add", tmp, path)
            hfs(out, "chmod", mode, path)
            if verbose:
                print("[mkrootfs] re-signed %s (mode %s)" % (path, mode))
        for path, mode in recipe.get("chmod", {}).items():
            hfs(out, "chmod", mode, path)
        # Old versions cannot put applog into apps through SpringBoard's
        # SBGuardMallocAlways (4.x only) or a launchd-wide environment (hangs
        # the boot): add it as a real LC_LOAD_DYLIB dependency of the chosen
        # stock executables instead (scripts/inject_dylib.py, re-log 139).
        # Only overlays set "inject_dylibs"; 4.2.1's recipe has no such key.
        if recipe.get("inject_dylibs"):
            import inject_dylib
            for path, dylib in recipe["inject_dylibs"].items():
                if path.startswith("_") or not dylib:
                    continue
                tmp = os.path.join(td, "inject")
                with open(tmp, "wb") as f:
                    f.write(hfs_cat(out, path))
                new = inject_dylib.patch(open(tmp, "rb").read(), dylib)
                if new == open(tmp, "rb").read():
                    if verbose:
                        print("[mkrootfs] %s already depends on %s" % (path, dylib))
                    continue
                with open(tmp, "wb") as f:
                    f.write(new)
                hfs(out, "rm", path)
                hfs(out, "add", tmp, path)
                hfs(out, "chmod", "0755", path)
                if verbose:
                    print("[mkrootfs] injected %s -> %s" % (dylib, path))
    if music or test_song:
        import music as music_mod
        legacy_db = None
        if recipe.get("music_legacy_db"):
            legacy_db = os.path.join(ROOT, "build", "mkitdb")
            if not os.path.exists(legacy_db):
                raise SystemExit("[mkrootfs] this firmware needs iOS 2.x's iTunesDB; "
                                 "run 'make music-itdb' first (libgpod-dev)")
        music_mod.apply(out, hfs, music, test_song, verbose, legacy_db,
                        bool(recipe.get("music_wav_test")))
    if recipe.get("disable_daemons"):
        hfs(out, "mkdir", LD_OFF, check=False)
        n = 0
        for name in recipe["disable_daemons"]:
            if exists(out, LD + "/" + name):
                hfs(out, "mv", LD + "/" + name, LD_OFF + "/" + name)
                n += 1
            elif verbose:
                print("[mkrootfs] (daemon %s not present)" % name)
        if verbose:
            print("[mkrootfs] disabled %d launch daemons" % n)
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--staging", default=os.path.join(ROOT, "build", "staging"))
    ap.add_argument("--out", default=os.path.join(ROOT, "build", "sd", "ios3ds", "rootfs.hfs"))
    ap.add_argument("--no-cydia", action="store_true", help="leave Cydia out (docs/cydia.md)")
    ap.add_argument("--no-tlsfix", action="store_true", help="leave TLSFix out (docs/certificates.md)")
    ap.add_argument("--music", help="folder with audio files for the Music app (docs/audio.md section 10)")
    ap.add_argument("--no-test-song", action="store_true", help="leave the generated test song out")
    a = ap.parse_args()
    build(a.staging, a.out, cydia=not a.no_cydia, tlsfix=not a.no_tlsfix, music=a.music,
          test_song=not a.no_test_song)


if __name__ == "__main__":
    main()
