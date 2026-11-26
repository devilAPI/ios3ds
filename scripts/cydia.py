#!/usr/bin/env python3
"""Cydia pre-installed: put Saurik's Cydia + APT/dpkg base system into the rootfs
image the way a 4.2.1 jailbreak left it (docs/cydia.md).

The .deb files are third-party: they are never committed or shipped in a
release. rootfs/cydia.json pins name, version, size and sha256 of each; they are
downloaded from their public repository (Telesphoreo, apt.saurik.com) at build
or install time, cached, verified, then unpacked into the HFS+ image with
xpwn's hfsplus, and /var/lib/dpkg is written to match. A failed download only
warns: the image is then built without Cydia.

  scripts/cydia.py fetch [--cache build/cydia]   download + verify the pinned debs
  scripts/cydia.py pin                            (maintainers) refill size/sha256
                                                  in rootfs/cydia.json from the repo
Turn it off: recipe.json "cydia": false, IOS3DS_CYDIA=0, `make sd CYDIA=0`,
or `install.py --no-cydia`.

Packages with a "group" are optional parts of the set: "tlsfix" (TLSFix, a
Substrate tweak from its GitHub release, "url") is on by default; recipe
"tlsfix": false, IOS3DS_TLSFIX=0, `make sd TLSFIX=0` or `--no-tlsfix` leave it
out. A failed download of an optional package drops only its group.
"""
import argparse
import bz2
import gzip
import hashlib
import io
import json
import os
import lzma
import os
import re
import sys
import tarfile
import tempfile
import time
import urllib.request

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MANIFEST = os.path.join(ROOT, "rootfs", "cydia.json")
CACHE = os.path.join(ROOT, "build", "cydia")
# the iOS rootfs reaches these through symlinks; operate on the real directories
ALIASES = (("/var/", "/private/var/"), ("/etc/", "/private/etc/"), ("/tmp/", "/private/var/tmp/"))
CONTROL_SCRIPTS = ("preinst", "postinst", "prerm", "postrm", "extrainst_", "triggers", "md5sums",
                   "conffiles", "shlibs")


def say(msg):
    print("[cydia] " + msg, flush=True)


def enabled(recipe=None):
    if os.environ.get("IOS3DS_CYDIA", "1").lower() in ("0", "no", "off", "false"):
        return False
    return bool(recipe is None or recipe.get("cydia", False))


def tlsfix_enabled(recipe=None):
    if os.environ.get("IOS3DS_TLSFIX", "1").lower() in ("0", "no", "off", "false"):
        return False
    return bool(recipe is None or recipe.get("tlsfix", True))


def load_manifest(path=None):
    with open(path or MANIFEST) as f:
        return json.load(f)


def sha256_file(path):
    with open(path, "rb") as f:
        return hashlib.sha256(f.read()).hexdigest()


def deb_name(p):
    return os.path.basename(p["path"])


def urls(m, p):
    """Where a pinned deb comes from: its own "url", else every mirror + path."""
    if p.get("url"):
        return [p["url"]]
    return [base.rstrip("/") + "/" + p["path"] for base in m["mirrors"]]


# --- download -----------------------------------------------------------------

def _get(url, timeout=60):
    req = urllib.request.Request(url, headers={"User-Agent": "ios3ds-build"})
    with urllib.request.urlopen(req, timeout=timeout) as r:  # follows the 302 to cache.saurik.com
        return r.read()


def fetch(manifest=None, cache=None, verbose=True, groups=("tlsfix",)):
    """Return [(package entry, local .deb path)] with every hash verified, or None
    (with a warning) if any base package can't be had: then no Cydia at all.
    Packages of an optional "group" are fetched only if the group is in groups;
    if one of them can't be had, only that group is left out."""
    m = manifest or load_manifest()
    # a manifest with its own "cache" (the 2.x set, rootfs/cydia-2.x.json) keeps
    # its debs in build/<cache>, apart from the 4.x set's build/cydia
    own = os.path.join(ROOT, "build", m["cache"]) if m.get("cache") else None
    cache = cache or os.environ.get("IOS3DS_CYDIA_CACHE") or own or CACHE
    os.makedirs(cache, exist_ok=True)
    out, dropped = [], set()
    for p in m["packages"]:
        g = p.get("group")
        if g and (g not in groups or g in dropped):
            continue
        dst = os.path.join(cache, deb_name(p))
        if os.path.exists(dst) and sha256_file(dst) == p["sha256"]:
            out.append((p, dst))
            continue
        data, err = None, None
        for url in urls(m, p):
            for attempt in range(2):
                try:
                    data = _get(url)
                    break
                except Exception as e:   # network down, 404, timeout: try again / next mirror
                    err = "%s: %s" % (url, e)
                    time.sleep(1)
            if data is not None:
                break
        bad = None
        if data is None:
            bad = "could not download %s (%s)" % (p["package"], err)
        else:
            h = hashlib.sha256(data).hexdigest()
            if h != p["sha256"] or len(data) != p["size"]:
                bad = "%s: sha256/size mismatch (got %s, %d bytes)" % (deb_name(p), h, len(data))
        if bad and g:
            say("WARNING: %s; building without %s" % (bad, g))
            dropped.add(g)
            out = [(q, d) for q, d in out if q.get("group") != g]
            continue
        if bad:
            say("WARNING: %s; building WITHOUT Cydia" % bad)
            return None
        with open(dst + ".part", "wb") as f:
            f.write(data)
        os.replace(dst + ".part", dst)
        if verbose:
            say("downloaded %s (%d KiB)" % (deb_name(p), len(data) >> 10))
        out.append((p, dst))
    return out


# --- .deb parsing -------------------------------------------------------------

def ar_members(blob):
    if blob[:8] != b"!<arch>\n":
        raise ValueError("not a .deb (ar) file")
    off = 8
    while off + 60 <= len(blob):
        hdr = blob[off:off + 60]
        name = hdr[:16].decode("ascii").strip().rstrip("/")
        size = int(hdr[48:58])
        yield name, blob[off + 60:off + 60 + size]
        off += 60 + size + (size & 1)


def open_tar(name, blob):
    if name.endswith(".lzma"):
        raw = lzma.decompress(blob, format=lzma.FORMAT_ALONE)
    elif name.endswith(".xz"):
        raw = lzma.decompress(blob)
    elif name.endswith(".gz"):
        raw = gzip.decompress(blob)
    elif name.endswith(".bz2"):
        raw = bz2.decompress(blob)
    elif name.endswith(".tar"):
        raw = blob
    else:
        raise ValueError("unknown deb member " + name)
    return tarfile.open(fileobj=io.BytesIO(raw), mode="r:")


def read_deb(path):
    """-> (control text, {script name: bytes}, data TarFile)"""
    with open(path, "rb") as f:
        members = dict(ar_members(f.read()))
    ctl_name = next(n for n in members if n.startswith("control.tar"))
    data_name = next(n for n in members if n.startswith("data.tar"))
    ctl = open_tar(ctl_name, members[ctl_name])
    control, scripts = None, {}
    for ti in ctl.getmembers():
        base = os.path.basename(ti.name)
        if not ti.isfile():
            continue
        if base == "control":
            control = ctl.extractfile(ti).read().decode("utf-8", "replace")
        elif base in CONTROL_SCRIPTS:
            scripts[base] = ctl.extractfile(ti).read()
    if control is None:
        raise ValueError("%s: no control file" % path)
    return control, scripts, open_tar(data_name, members[data_name])


def norm(name):
    """tar member name -> absolute path ('./usr/bin/' -> '/usr/bin'); None for the root.
    Rejects '..' so a deb can't escape the image root."""
    p = name
    while p.startswith("./"):
        p = p[2:]
    p = p.strip("/")
    if p in ("", "."):
        return None
    if ".." in p.split("/"):
        raise ValueError("bad path in deb: %r" % name)
    return "/" + p


def control_field(control, key):
    m = re.search(r"^%s:\s*(.*)$" % re.escape(key), control, re.M | re.I)
    return m.group(1).strip() if m else None


# --- image writer -------------------------------------------------------------

class Image:
    """Small cache on top of mkrootfs.hfs(): which names exist in which directory."""

    def __init__(self, img, hfs, tmpdir):
        self.img, self.run, self.tmp = img, hfs, tmpdir
        self.dirs = {}
        self.calls = 0

    @staticmethod
    def real(path):
        for a, b in ALIASES:
            if path == a.rstrip("/"):
                return b.rstrip("/")
            if path.startswith(a):
                return b + path[len(a):]
        return path

    def listing(self, d):
        d = self.real(d) or "/"
        if d not in self.dirs:
            out = self.run(self.img, "ls", d, check=False)
            names = {}
            for line in out.splitlines():
                # hfsplus pads one-digit dates with a space (" 7/ 5/2010" on
                # 4.1's stock files), so the date is matched, not split
                m = re.match(r"^(\d{6})\s+\d+\s+\d+\s+\d+\s+\d+/\s*\d+/\d+\s+\d+:\d+\s(.*)$", line)
                if m:
                    names[m.group(2)] = int(m.group(1), 8)
            self.dirs[d] = names
            self.calls += 1
        return self.dirs[d]

    def mode(self, path):
        path = self.real(path)
        d, _, n = path.rpartition("/")
        return self.listing(d or "/").get(n)

    def _note(self, path, mode):
        path = self.real(path)
        d, _, n = path.rpartition("/")
        self.listing(d or "/")[n] = mode
        if mode & 0o170000 == 0o040000:
            self.dirs.setdefault(path, {})

    def _hfs(self, *args):
        self.calls += 1
        return self.run(self.img, *args)

    def mkdir(self, path, mode=0o755):
        m = self.mode(path)
        if m is not None:
            if m & 0o170000 not in (0o040000, 0o120000):
                raise SystemExit("[cydia] %s exists and is not a directory" % path)
            return False
        self._hfs("mkdir", self.real(path))
        self._note(path, 0o040000 | 0o755)
        if mode & 0o7777 != 0o755:
            self._hfs("chmod", "%o" % (mode & 0o7777), self.real(path))
        return True

    def makedirs(self, path):
        cur = ""
        for part in path.strip("/").split("/"):
            cur += "/" + part
            self.mkdir(cur)

    def put(self, path, data, mode=0o644):
        m = self.mode(path)
        if m is not None:
            if m & 0o170000 != 0o100000:
                raise SystemExit("[cydia] %s exists in the image and is not a file" % path)
            self._hfs("rm", self.real(path))
        tmp = os.path.join(self.tmp, "f")
        with open(tmp, "wb") as f:
            f.write(data)
        self._hfs("add", tmp, self.real(path))
        self._note(path, 0o100000 | 0o644)
        if mode & 0o7777 != 0o644:
            self._hfs("chmod", "%o" % (mode & 0o7777), self.real(path))

    def symlink(self, path, target):
        m = self.mode(path)
        if m is not None:
            if m & 0o170000 == 0o040000:
                raise SystemExit("[cydia] %s is a directory in the image" % path)
            self._hfs("rm", self.real(path))
        self._hfs("symlink", self.real(path), target)
        self._note(path, 0o120755)


# --- install ------------------------------------------------------------------

def status_entry(control, extra=(), hold=False):
    lines = control.strip("\n").splitlines()
    out = []
    for line in lines:
        out.append(line)
        if line.startswith("Package:"):
            out.append("Status: %s ok installed" % ("hold" if hold else "install"))
    out.extend(extra)
    return "\n".join(out) + "\n"


def pseudo_entry(name, version, desc, title=None):
    e = ("Package: %s\nEssential: yes\nStatus: install ok installed\nPriority: required\n"
         "Section: System\nInstalled-Size: 0\nArchitecture: iphoneos-arm\nVersion: %s\n"
         "Description: %s\nMaintainer: Jay Freeman (saurik) <saurik@saurik.com>\nTag: role::cydia\n"
         % (name, version, desc))
    if title:
        e += "Name: %s\n" % title
    return e


def patch_file(m, path, data, verbose=True):
    """Manifest "patch": byte patches {path, offset, orig, new, why} applied to a
    package file as it is unpacked (the 2.x set's Cydia, rootfs/cydia-2.x.json).
    A mismatch of "orig" fails the build: the pinned sha256 makes it a bug."""
    for p in m.get("patch", []):
        if p["path"] != path:
            continue
        off, orig, new = int(p["offset"], 16), bytes.fromhex(p["orig"]), bytes.fromhex(p["new"])
        if len(orig) != len(new) or data[off:off + len(orig)] != orig:
            raise SystemExit("[cydia] patch %s @%s: bytes %s, expected %s"
                             % (path, p["offset"], data[off:off + len(orig)].hex(), p["orig"]))
        data = data[:off] + new + data[off + len(new):]
        if verbose:
            say("patched %s @%s" % (path, p["offset"]))
    return data


def install(img, hfs, debs, manifest=None, verbose=True):
    """Unpack debs [(entry, path)] into the HFS+ image img (hfs = mkrootfs.hfs) and
    write the dpkg database. Returns the number of files written."""
    m = manifest or load_manifest()
    t0 = time.time()
    with tempfile.TemporaryDirectory() as td:
        im = Image(img, hfs, td)
        status, nfiles, owner = [], 0, {}
        keep = set(m.get("keep_stock", []))
        for p, path in debs:
            control, scripts, tar = read_deb(path)
            pkg = control_field(control, "Package")
            listed = ["/."]
            md5 = {}
            for ti in tar.getmembers():
                dst = norm(ti.name)
                if dst is None:
                    continue
                listed.append(dst)
                if ti.isdir():
                    im.mkdir(dst, ti.mode)
                    continue
                if dst in owner and owner[dst] != pkg and verbose:
                    say("note: %s (%s) replaces %s's file" % (dst, pkg, owner[dst]))
                elif dst not in owner and im.mode(dst) is not None:
                    if pkg in keep:
                        # firmware-sbin: empty placeholders claiming Apple's own /sbin
                        # tools for dpkg; its maintainer scripts keep the stock files
                        owner[dst] = pkg
                        continue
                    if verbose:
                        say("note: %s (%s) replaces a stock file" % (dst, pkg))
                owner[dst] = pkg
                if ti.issym():
                    im.symlink(dst, ti.linkname)
                elif ti.isfile() or ti.islnk():
                    data = patch_file(m, dst, tar.extractfile(ti).read(), verbose)
                    md5[dst] = hashlib.md5(data).hexdigest()
                    im.put(dst, data, ti.mode)
                else:
                    say("skipping %s (tar type %r)" % (dst, ti.type))
                    continue
                nfiles += 1
            extra = []
            conff = [c for c in scripts.get("conffiles", b"").decode().split() if c]
            if conff:
                extra.append("Conffiles:")
                extra += [" %s %s" % (c, md5.get(c, "newconffile")) for c in conff]
            # held packages (manifest "hold"): APT/Cydia never upgrade or reinstall them
            status.append(status_entry(control, extra, pkg in m.get("hold", ())))
            info = "/var/lib/dpkg/info/" + pkg
            im.makedirs("/var/lib/dpkg/info")
            im.put(info + ".list", ("\n".join(listed) + "\n").encode())
            for s, data in scripts.items():
                if s in ("md5sums", "conffiles", "triggers", "shlibs"):
                    im.put("%s.%s" % (info, s), data)
                else:
                    im.put("%s.%s" % (info, s), data, 0o755)
            if verbose:
                say("installed %s %s (%d paths)" % (pkg, control_field(control, "Version"), len(listed) - 1))

        # what firmware.sh (Cydia's first-boot script) would add: virtual packages
        # for the OS, CPU, model, kernel and CoreFoundation (it re-does this each boot)
        for v in m.get("pseudo", []):
            status.append(pseudo_entry(v["package"], v["version"], v["description"], v.get("name")))
            im.put("/var/lib/dpkg/info/%s.list" % v["package"], b"/.\n")
        status.sort(key=lambda e: e.split("\n", 1)[0])
        im.put("/var/lib/dpkg/status", "\n".join(status).encode())
        im.put("/var/lib/dpkg/available", b"")
        for d in m.get("mkdirs", []):
            im.makedirs(d)
        for path, text in m.get("write", {}).items():
            im.put(path, text.encode())
        for path, target in m.get("symlink", {}).items():
            im.symlink(path, target)
        if verbose:
            say("%d packages + %d virtual, %d files, %d hfsplus calls, %.1f s"
                % (len(debs), len(m.get("pseudo", [])), nfiles, im.calls, time.time() - t0))
        return nfiles


def apply(img, hfs, recipe, cache=None, verbose=True, tlsfix=True):
    """mkrootfs hook: fetch + install if enabled; never fails the build on a
    download problem (returns None then, else the manifest for
    substrate_setup). tlsfix=False leaves TLSFix out."""
    if not enabled(recipe):
        if verbose:
            say("disabled (recipe 'cydia' / IOS3DS_CYDIA=0): no Cydia in this image")
        return None
    mpath = recipe["cydia"] if isinstance(recipe.get("cydia"), str) else MANIFEST
    m = load_manifest(os.path.join(ROOT, mpath) if not os.path.isabs(mpath) else mpath)
    groups = ("tlsfix",) if tlsfix and tlsfix_enabled(recipe) else ()
    if verbose and not groups:
        say("TLSFix left out (recipe 'tlsfix' / IOS3DS_TLSFIX=0 / --no-tlsfix)")
    # older firmwares: the virtual firmware/OS/kernel/CF packages Cydia
    # filters packages by carry that firmware's versions (recipe overlay
    # "cydia_pseudo"); the default recipe has none, so 4.2.1 is unchanged
    for p in m.get("pseudo", []):
        if p["package"] in recipe.get("cydia_pseudo", {}):
            p["version"] = recipe["cydia_pseudo"][p["package"]]
    debs = fetch(m, cache, verbose, groups)
    if not debs:
        return None
    install(img, hfs, debs, m, verbose)
    return m


def substrate_setup(img, hfs, hfs_cat, recipe, manifest, verbose=True):
    """mkrootfs hook, after the recipe's own files and plist edits: make every
    process load Cydia Substrate next to our DYLD_INSERT_LIBRARIES interposers
    (manifest "substrate", docs/cydia.md)."""
    import plistlib
    sub = manifest.get("substrate")
    if not sub:
        return
    ins = sub["insert"]
    with tempfile.TemporaryDirectory() as td:
        im = Image(img, hfs, td)
        modes = sub.get("mode", {})
        for path, text in sub.get("write", {}).items():
            im.makedirs(path.rsplit("/", 1)[0])
            im.put(path, text.encode(), int(modes.get(path, "644"), 8))
        for path, target in sub.get("symlink", {}).items():
            im.symlink(path, target)
        # Copies under /usr/lib for sandboxed apps (MobileSafari, App Store
        # apps): their profile allows /usr/lib but not /Library, so applog's
        # load_substrate() falls back to these when the /Library dlopen fails.
        # A dylib whose LC_LOAD_DYLIB points into /Library (TLSFix depends on
        # CydiaSubstrate's absolute path) gets that path rewritten to its copy;
        # the shorter string is NUL-padded inside the same load command.
        patches = sub.get("sandbox_patch_load", {})
        for src, dst in sub.get("sandbox_copies", {}).items():
            if im.mode(src) is None:
                continue
            im.makedirs(dst.rsplit("/", 1)[0])
            data = hfs_cat(img, im.real(src))
            for spec in patches.get(src, []):
                old, new = spec["old"], spec["new"]
                needle, repl = old.encode() + b"\0", new.encode() + b"\0"
                if len(repl) > len(needle):
                    raise SystemExit("[cydia] %s: replacement path too long" % src)
                if needle not in data:
                    raise SystemExit("[cydia] %s: no occurrence of %s" % (src, old))
                data = data.replace(needle, repl + b"\0" * (len(needle) - len(repl)))
                if verbose:
                    say("substrate: %s: %s -> %s" % (src, old, new))
            im.put(dst, data, 0o755)
            if verbose:
                say("substrate: %s -> %s (sandboxed apps)" % (src, dst))
        # TLSFix's EC-chain certificate check reads the profile CA payloads
        # from /var/mobile/Library/ConfigurationProfiles, which the sandbox
        # denies too; the copy it now looks in (path patched above) lives here.
        for src, dst in sub.get("sandbox_profile_copy", {}).items():
            if im.mode(src) is None:
                continue
            im.makedirs(dst.rsplit("/", 1)[0])
            im.put(dst, hfs_cat(img, im.real(src)), 0o644)
            if verbose:
                say("substrate: %s -> %s (sandboxed apps)" % (src, dst))
        # dpkg diversions (local, package ":"): the package's file moves to
        # <path>.real (a later mobilesubstrate upgrade unpacks there too) and our
        # guard takes its place: cynject refuses pid 1 (launchd injection hangs 4.2.1)
        div = []
        for path, src in sub.get("divert", {}).items():
            real = im.real(path)
            if im.mode(path) is not None:
                im.put(path + ".real", hfs_cat(img, real), 0o2755)
            im.put(path, hfs_cat(img, im.real(src)), 0o755)
            div.append("%s\n%s.real\n:\n" % (path, path))
            if verbose:
                say("substrate: %s diverted to %s.real, guard from %s" % (path, path, src))
        if div:
            old = b""
            if im.mode("/var/lib/dpkg/diversions") is not None:
                old = hfs_cat(img, im.real("/var/lib/dpkg/diversions"))
            im.put("/var/lib/dpkg/diversions", old + "".join(div).encode())
        # a launchd.conf with Substrate's cynject line would hang the boot
        if im.mode("/etc/launchd.conf") is not None:
            conf = hfs_cat(img, im.real("/etc/launchd.conf")).decode("latin-1")
            keep = "".join(l for l in conf.splitlines(True) if "cynject" not in l)
            if keep != conf:
                im.put("/etc/launchd.conf", keep.encode("latin-1"))
        edits = {}
        # launchd jobs that set their own DYLD_INSERT_LIBRARIES don't see launchd's
        # global one: append Substrate to theirs (ours first, so our interposers stay)
        daemons = sub.get("daemons")   # which plist_set jobs get Substrate (None = all)
        for path, keys in recipe.get("plist_set", {}).items():
            if daemons is not None and os.path.basename(path) not in daemons:
                continue
            env = keys.get("EnvironmentVariables", {})
            if "DYLD_INSERT_LIBRARIES" in env:
                edits.setdefault(path, []).append(("insert", env["DYLD_INSERT_LIBRARIES"]))
        for path, names in sub.get("plist_delete", {}).items():
            edits.setdefault(path, []).extend(("delete", n) for n in names)
        for path, ops in edits.items():
            pl = plistlib.loads(hfs_cat(img, im.real(path)))
            for op, arg in ops:
                if op == "insert":
                    cur = [x for x in pl["EnvironmentVariables"]["DYLD_INSERT_LIBRARIES"].split(":") if x]
                    if ins not in cur:
                        cur.append(ins)
                    pl["EnvironmentVariables"]["DYLD_INSERT_LIBRARIES"] = ":".join(cur)
                else:
                    pl.pop(arg, None)
            im.put(path, plistlib.dumps(pl, fmt=plistlib.FMT_XML))
            if verbose:
                say("substrate: %s: %s" % (path, ", ".join(
                    "DYLD_INSERT_LIBRARIES += %s" % ins if op == "insert" else "no %s" % arg for op, arg in ops)))


# --- maintainer helper ------------------------------------------------------------

def pin(path=MANIFEST):
    m = load_manifest(path)
    for p in m["packages"]:
        data = _get(urls(m, p)[0])
        p["size"], p["sha256"] = len(data), hashlib.sha256(data).hexdigest()
        print("%-28s %-20s %8d %s" % (p["package"], p["version"], p["size"], p["sha256"]))
    with open(path, "w") as f:
        json.dump(m, f, indent=1)
        f.write("\n")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("cmd", choices=("fetch", "pin"))
    ap.add_argument("--cache", default=None)
    ap.add_argument("--manifest", default=MANIFEST)
    a = ap.parse_args()
    if a.cmd == "pin":
        pin(a.manifest)
        return
    got = fetch(load_manifest(a.manifest), a.cache)
    if not got:
        sys.exit(1)
    say("%d packages ready in %s" % (len(got), os.path.dirname(got[0][1])))


if __name__ == "__main__":
    main()
