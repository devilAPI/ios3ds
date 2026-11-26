#!/usr/bin/env python3
"""ios3ds installer: your own iPod2,1 IPSW + this release -> a ready SD card.

  python3 install.py [--ios 4.2.1|4.1|3.1.3|2.2.1] IPSW [SD]

  --ios  firmware to install (default 4.2.1, the only supported version;
         4.1, 3.1.3 and 2.2.1 are HIGHLY EXPERIMENTAL, QEMU-only and may not boot)
  IPSW   the matching iPod touch 2G restore file (e.g.
         iPod2,1_4.2.1_8C148_Restore.ipsw for the default); its sha1 is checked
         against scripts/versions.py before anything is decrypted
  SD     where to put the files: the root of your mounted SD card, or any folder
         (default: ./ios3ds-sd, copy its contents to the card yourself)

What it does (README.md, section Install):
  1. decrypts the kernel, device tree and root filesystem from the IPSW
     (scripts/fwprep.py; the public keys for the selected build come from
     api.ipsw.me, or --keys FILE)
  2. patches the kernel and builds the device tree, boot.cfg and the iOS disk
     image rootfs.hfs (scripts/mksd.py --ios VERSION -> mkboot.py, mkrootfs.py:
     the same build path as 'make sd IOS=VERSION')
  3. writes sd:/ios3ds/, sd:/3ds/ios3ds.3dsx and sd:/luma/payloads/ios3ds.firm

Needs Python 3.8+ with the 'cryptography' package (or the openssl command) and
~4 GB of free disk space while it works. No cross compilers: the 3DS and iOS
binaries come prebuilt in this release. On Linux x86_64 the HFS+ tool is
prebuilt too; elsewhere it is compiled from src/xpwn-hfsplus (needs cc + zlib).
Nothing here talks to the 3DS or writes anything but files in SD and --work.
"""
import argparse
import hashlib
import json
import os
import platform
import shutil
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE) if os.path.basename(HERE) == "scripts" else HERE
SCRIPTS = os.path.join(ROOT, "scripts")
sys.path.insert(0, SCRIPTS)
import versions  # noqa: E402

# Only these four have a patch set, a rootfs overlay and a documented install
# path. versions.py also lists firmwares that are inventory only (no build).
INSTALL_VERSIONS = ("4.2.1", "4.1", "3.1.3", "2.2.1")
GiB = 1 << 30


def say(msg=""):
    print(msg, flush=True)


def step(n, msg):
    say("\n== %d. %s" % (n, msg))


def die(msg):
    say("\nERROR: " + msg)
    sys.exit(1)


def check_version(value):
    """Argparse type: refuse anything but the four installable iOS versions."""
    if value not in INSTALL_VERSIONS:
        raise argparse.ArgumentTypeError(
            "unknown iOS version %r (supported: %s; default: %s)"
            % (value, ", ".join(INSTALL_VERSIONS), versions.DEFAULT))
    info = versions.get(value)      # also catches a drift in the registry
    if value != versions.DEFAULT and (not info["patch_set"] or not info["overlay"]):
        raise argparse.ArgumentTypeError(
            "iOS %s has no installer build recipe yet" % value)
    return value


def ipsw_name(ios):
    """The exact restore file expected for `ios` (from the version registry)."""
    return versions.get(ios)["ipsw"]


def warn_experimental(ios, emit=None):
    """Loud, unavoidable notice for every non-default version."""
    if ios == versions.DEFAULT:
        return
    emit = emit or say
    emit("")
    emit("*" * 72)
    emit("WARNING: iOS %s (%s) is HIGHLY EXPERIMENTAL." % (ios, versions.get(ios)["build"]))
    emit("It has been tested only in the QEMU emulator, NEVER on a real 3DS")
    emit("console, and it MAY NOT BOOT. Only iOS %s is supported." % versions.DEFAULT)
    emit("Back up your SD card and read docs/other-versions.md before continuing.")
    emit("*" * 72)


def sha1_file(path):
    h = hashlib.sha1()
    with open(path, "rb") as f:
        for b in iter(lambda: f.read(1 << 20), b""):
            h.update(b)
    return h.hexdigest()


def verify_ipsw(path, ios):
    """Refuse an IPSW whose sha1 is not the one pinned for this iOS version.

    --force never bypasses this: a wrong firmware would be decrypted with keys
    for a different build and could produce an image that boots incorrectly.
    """
    info = versions.get(ios)
    want = info["sha1"]
    if not want:
        die("no published sha1 for iOS %s; refusing to build" % ios)
    got = sha1_file(path)
    if got != want:
        die("this is not the iOS %s IPSW: sha1 %s, expected %s.\n"
            "       Download %s (see docs/other-versions.md for where to get it).\n"
            "       The sha1 check cannot be bypassed with --force."
            % (ios, got, want, ipsw_name(ios)))
    say("   IPSW sha1 %s: verified iOS %s (%s)" % (got, ios, info["build"]))


def check_staging(staging, ios):
    """A pre-extracted staging folder must match the selected version."""
    man = os.path.join(staging, "manifest.json")
    if not os.path.isfile(man):
        say("   note: no %s; cannot verify the staged firmware version" % man)
        return
    try:
        with open(man) as f:
            src = json.load(f)["source"]
    except (OSError, ValueError, KeyError) as e:
        die("cannot read %s: %s" % (man, e))
    want = versions.get(ios)["build"]
    if src.get("product") != "iPod2,1" or src.get("build") != want:
        die("%s holds %s %s, not iOS %s (%s). Extract the right IPSW first\n"
            "       (make fwprep IPSW=... IOS=%s, or install.py without --staging)."
            % (staging, src.get("product"), src.get("build"), ios, want, ios))


def free_bytes(path):
    p = os.path.abspath(path)
    while not os.path.exists(p):
        p = os.path.dirname(p)
    return shutil.disk_usage(p).free


def check_python():
    if sys.version_info < (3, 8):
        die("Python 3.8 or newer is needed (this is %s)" % sys.version.split()[0])
    try:
        import cryptography  # noqa: F401
        return "cryptography"
    except ImportError:
        if shutil.which("openssl"):
            say("note: the Python 'cryptography' package is missing; using the openssl command\n"
                "      instead, which is much slower (pip install cryptography, or\n"
                "      sudo apt install python3-cryptography)")
            return "openssl"
        die("install the Python 'cryptography' package: pip install cryptography\n"
            "       (Debian/Ubuntu: sudo apt install python3-cryptography)")


def hfsplus_tool(prebuilt, work):
    """xpwn's hfsplus: the static Linux binary from the release, else compile it."""
    def works(p):
        try:
            r = subprocess.run([p], capture_output=True, text=True)
        except OSError:
            return False
        return "usage" in (r.stdout + r.stderr)
    arch = platform.machine().lower()
    cand = os.path.join(prebuilt, "hfsplus-linux-%s" % arch)
    if platform.system() == "Linux" and os.path.exists(cand):
        try:
            os.chmod(cand, 0o755)       # zip tools often drop the x bit
        except OSError:
            pass
        if works(cand):
            return cand
    built = os.path.join(work, "hfsplus")
    if os.path.exists(built) and works(built):
        return built
    src = os.path.join(ROOT, "src", "xpwn-hfsplus")
    cc = os.environ.get("CC") or shutil.which("cc") or shutil.which("gcc") or shutil.which("clang")
    if not cc or not os.path.isdir(src):
        die("no HFS+ tool for this system (%s %s) and no C compiler to build it.\n"
            "       Linux: sudo apt install build-essential zlib1g-dev; macOS: xcode-select --install;\n"
            "       Windows: run this installer inside WSL (Ubuntu)." % (platform.system(), arch))
    say("building the HFS+ tool from src/xpwn-hfsplus with %s ..." % cc)
    os.makedirs(work, exist_ok=True)
    import glob
    files = sorted(glob.glob(os.path.join(src, "hfs", "*.c"))) + \
        [os.path.join(src, "common", f) for f in ("abstractfile.c", "base64.c")]
    r = subprocess.run([cc, "-O2", "-w", "-I" + os.path.join(src, "includes")] + files +
                       ["-lz", "-o", built], capture_output=True, text=True)
    if r.returncode or not works(built):
        die("could not build the HFS+ tool:\n%s\n       (needs zlib headers: sudo apt install zlib1g-dev)"
            % (r.stderr.strip()[-2000:]))
    return built


def fwprep(ipsw, staging, keys, force, ios=versions.DEFAULT):
    man = os.path.join(staging, "manifest.json")
    info = versions.get(ios)
    say("firmware: iOS %s (%s); public keys from api.ipsw.me unless --keys is given"
        % (ios, info["build"]))
    if os.path.exists(man) and os.path.exists(os.path.join(staging, "rootfs.hfs")):
        try:
            src = json.load(open(man))["source"]
            if src.get("ipsw") == os.path.basename(ipsw) and src.get("build") == info["build"]:
                say("already extracted in %s (delete it to redo)" % staging)
                return
        except (ValueError, KeyError):
            pass
    cmd = [sys.executable, os.path.join(SCRIPTS, "fwprep.py"), ipsw, "-o", staging,
           "--ios", ios]
    if keys:
        cmd += ["--keys", keys]
    if force:
        cmd += ["--force"]
    if subprocess.run(cmd).returncode:
        die("extracting the IPSW failed (see the [fwprep] lines above)")


def build_tree(staging, tree, prebuilt, hfsplus, cydia=True, cydia_cache=None, tlsfix=True,
               music=None, test_song=True, ios=versions.DEFAULT):
    import mksd
    args = mksd.parser().parse_args([])
    args.staging = staging
    args.out = tree
    args.firm = os.path.join(prebuilt, "ios3ds.firm")
    args.launcher = os.path.join(prebuilt, "ios3ds.3dsx")
    args.prebuilt = prebuilt
    args.hfsplus = hfsplus
    args.rootfs_add = os.path.join(prebuilt, "rootfs-add")
    args.no_cydia = not cydia
    args.cydia_cache = cydia_cache
    args.no_tlsfix = not tlsfix
    args.sandbox_open = False
    args.music = music
    args.no_test_song = not test_song
    args.ios = ios
    # the installer carries from the user's card (see main), never from
    # CARRY_FROM/IOS3DS_CARRY_FROM, which is only for developer 'make sd' builds
    args.carry_from = None
    mksd.build_tree(args)


def copy_file(src, dst):
    size = os.path.getsize(src)
    tmp = dst + ".part"
    done, t0, last = 0, time.time(), 0
    with open(src, "rb") as fi, open(tmp, "wb") as fo:
        while True:
            b = fi.read(8 << 20)
            if not b:
                break
            fo.write(b)
            done += len(b)
            if size > 64 << 20 and time.time() - last > 2:
                last = time.time()
                print("\r   %s: %3d%% (%d/%d MiB)" % (os.path.basename(dst), 100 * done // size,
                      done >> 20, size >> 20), end="", flush=True)
        fo.flush()
        os.fsync(fo.fileno())
    if size > 64 << 20:
        print("\r   %s: done (%d MiB, %.0f s)          " % (os.path.basename(dst), size >> 20,
              time.time() - t0), flush=True)
    os.replace(tmp, dst)


def deploy(tree, sd):
    """Copy the card tree into sd, leaving everything else on the card alone."""
    for d, _, fs in os.walk(tree):
        rel = os.path.relpath(d, tree)
        os.makedirs(os.path.join(sd, rel), exist_ok=True)
        for fn in sorted(fs):
            src, dst = os.path.join(d, fn), os.path.normpath(os.path.join(sd, rel, fn))
            if fn == "boot.cfg" and os.path.exists(dst):
                old = open(dst, "rb").read()
                if old != open(src, "rb").read():
                    shutil.copyfile(dst, dst + ".old")
                    say("   kept your previous boot.cfg as ios3ds/boot.cfg.old")
            copy_file(src, dst)
            say("   %s" % os.path.relpath(dst, sd))


def parser():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("ipsw", nargs="?", help="iPod2,1 restore IPSW for the selected --ios version")
    ap.add_argument("sd", nargs="?", default="ios3ds-sd",
                    help="SD card root (mounted) or output folder (default: ./ios3ds-sd)")
    ap.add_argument("--ios", default=versions.DEFAULT, type=check_version, metavar="VERSION",
                    help="iOS version: 4.2.1 (default, supported), 4.1, 3.1.3 or 2.2.1 "
                         "(experimental, QEMU only; docs/other-versions.md)")
    ap.add_argument("--work", default="ios3ds-work", help="scratch folder (default: ./ios3ds-work)")
    ap.add_argument("--keep-work", action="store_true", help="keep the scratch folder (faster re-runs)")
    ap.add_argument("--keys", help="firmware keys JSON (ipsw.me v4 format) instead of downloading")
    ap.add_argument("--staging", help="use an already extracted fwprep folder instead of the IPSW")
    ap.add_argument("--force", action="store_true",
                    help="try an IPSW whose plist identity differs from the selected version "
                         "(the sha1 check always applies)")
    ap.add_argument("--no-cydia", action="store_true",
                    help="don't download and pre-install Cydia (docs/cydia.md)")
    ap.add_argument("--no-tlsfix", action="store_true",
                    help="leave out TLSFix, the TLS 1.2/1.3 tweak (docs/certificates.md)")
    ap.add_argument("--music", metavar="DIR",
                    help="your audio files (.mp3/.m4a/.wav/.aif) for the Music app (default: SD/ios3ds/music "
                         "if that folder exists; docs/audio.md section 10)")
    ap.add_argument("--no-test-song", action="store_true", help="leave the generated test song out of the Music app")
    ap.add_argument("--fresh", action="store_true",
                    help="start with a fresh iOS disk: don't carry user data over from an "
                         "existing sd:/ios3ds/rootfs.hfs (also: IOS3DS_FRESH=1)")
    ap.add_argument("--backup-old", action="store_true",
                    help="keep an extra backup copy (rootfs.hfs.bak) of the previous iOS disk; "
                         "a backup is also kept automatically if the carry-over fails")
    ap.add_argument("--prebuilt", default=os.path.join(ROOT, "prebuilt"), help=argparse.SUPPRESS)
    ap.add_argument("--check", action="store_true",
                    help="only check this release and the tools on this PC, then exit")
    return ap


def main():
    a = parser().parse_args()
    warn_experimental(a.ios)

    label = ("ios3ds installer (%s)" % open(os.path.join(a.prebuilt, "VERSION")).read().strip()
             if os.path.exists(os.path.join(a.prebuilt, "VERSION")) else "ios3ds installer")
    say("%s, target iOS %s" % (label, a.ios))
    need = ["ios3ds.firm", "ios3ds.3dsx", "shim.bin", "gpu.bin", "rootfs-add"]
    missing = [n for n in need if not os.path.exists(os.path.join(a.prebuilt, n))]
    if missing:
        die("this release is incomplete, missing prebuilt/%s. Unpack the whole zip." % ", ".join(missing))

    step(1, "Checking this PC")
    crypto = check_python()
    hfs = hfsplus_tool(a.prebuilt, a.work)
    say("Python %s, %s, HFS+ tool %s" % (platform.python_version(), crypto, os.path.relpath(hfs)))
    if a.check:
        import mkboot  # noqa: F401  (imports kernel/, fwlib: catches a broken unpack)
        import mkrootfs  # noqa: F401
        import cydia  # noqa: F401
        import certs  # noqa: F401  (needs sqlite3)
        import music  # noqa: F401  (Music app library, needs sqlite3)
        import fwprep as _fwprep_check # noqa: F401
        say("OK: everything needed is here")
        return

    if a.staging and a.ipsw and not a.ipsw.lower().endswith(".ipsw"):
        a.ipsw, a.sd = None, a.ipsw         # install.py --staging DIR SD
    if not a.ipsw and not a.staging:
        parser().print_usage()
        die("give the path of your %s" % ipsw_name(a.ios))
    if a.ipsw and not os.path.isfile(a.ipsw):
        die("no such file: %s" % a.ipsw)
    if a.ipsw and os.path.basename(a.ipsw).lower() != ipsw_name(a.ios).lower():
        say("   note: the usual file for iOS %s is %s; checking its sha1 instead"
            % (a.ios, ipsw_name(a.ios)))
    sd = os.path.abspath(a.sd)
    work = os.path.abspath(a.work)
    if sd == work or sd.startswith(work + os.sep) or work.startswith(sd + os.sep):
        die("the SD folder and --work must be separate folders")
    if os.path.exists(os.path.join(sd, "ios3ds", "luma.firm")):
        die("%s has ios3ds/luma.firm: a launch from the Homebrew Launcher was not finished.\n"
            "       Start ios3ds once (it restores Luma by itself), or use the launcher's\n"
            "       'Y: restore Luma', or rename ios3ds/luma.firm back to boot.firm on the PC." % sd)
    if free_bytes(work) < 4 * GiB:
        die("need about 4 GB free for %s (have %.1f GB)" % (work, free_bytes(work) / GiB))
    if free_bytes(sd) < 2 * GiB:
        die("need about 2 GB free on %s (have %.1f GB)" % (sd, free_bytes(sd) / GiB))

    staging = os.path.abspath(a.staging) if a.staging else os.path.join(work, "staging")
    if not a.staging:
        step(2, "Checking your IPSW, then extracting it (a few minutes)")
        verify_ipsw(a.ipsw, a.ios)
        fwprep(a.ipsw, staging, a.keys, a.force, a.ios)
    else:
        step(2, "Using the extracted firmware in %s" % staging)
        check_staging(staging, a.ios)

    step(3, "Patching the kernel, building the iOS disk image (a few minutes)")
    tree = os.path.join(work, "sd")
    music = a.music
    if not music and os.path.isdir(os.path.join(sd, "ios3ds", "music")):
        music = os.path.join(sd, "ios3ds", "music")
        say("   music for the Music app from %s" % music)
    if music and not os.path.isdir(music):
        die("no such folder: %s" % music)
    build_tree(staging, tree, a.prebuilt, hfs, cydia=not a.no_cydia,
               cydia_cache=os.path.join(work, "cydia"), tlsfix=not a.no_tlsfix,
               music=music, test_song=not a.no_test_song, ios=a.ios)

    # carry user data over from a rootfs.hfs already on the card, if any
    fresh = a.fresh or os.environ.get("IOS3DS_FRESH", "").lower() in ("1", "true", "yes", "on") \
            or os.environ.get("FRESH", "").lower() in ("1", "true", "yes", "on")
    old_img = os.path.join(sd, "ios3ds", "rootfs.hfs")
    new_img = os.path.join(tree, "ios3ds", "rootfs.hfs")
    if not fresh and os.path.exists(old_img) and os.path.exists(new_img):
        say("\nCarrying your data over from the previous iOS disk")
        sys.path.insert(0, SCRIPTS)
        import carryover
        try:
            result = carryover.carry_over(old_img, new_img, hfs, backup=a.backup_old, verbose=True)
            say("   carry-over result: %s" % result)
            if result.status != "carried" and not result.backup:
                # carry_over() keeps a backup itself on any failure; this is
                # the last resort if even that could not be done
                carryover.keep_backup(old_img, result.reason or result.status)
        except Exception as e:
            say("   warn: carry-over failed, continuing with a fresh disk: %s" % e)
            carryover.keep_backup(old_img, "failed")
    else:
        if fresh:
            say("\n(--fresh: starting with a new iOS disk)")

    step(4, "Writing the SD card files to %s" % sd)
    os.makedirs(os.path.dirname(sd), exist_ok=True)
    if not os.path.exists(sd) and os.stat(os.path.dirname(sd)).st_dev == os.stat(work).st_dev:
        os.rename(tree, sd)             # a new folder on the same disk: just move it
    else:
        os.makedirs(sd, exist_ok=True)
        deploy(tree, sd)
    if not a.keep_work:
        shutil.rmtree(tree, ignore_errors=True)
        if not a.staging:
            shutil.rmtree(staging, ignore_errors=True)
        for leftover in ("hfsplus",):
            if os.path.exists(os.path.join(work, leftover)):
                os.remove(os.path.join(work, leftover))
        shutil.rmtree(os.path.join(work, "cydia"), ignore_errors=True)   # downloaded .debs
        try:
            os.rmdir(work)
        except OSError:
            pass
    say("\nDone. On the SD card (%s):" % sd)
    say("  3ds/ios3ds.3dsx              start this from the Homebrew Launcher")
    say("  luma/payloads/ios3ds.firm    or hold START while powering on (Luma's chainloader)")
    say("  ios3ds/                      iOS itself (~1 GB) and its settings (boot.cfg)")
    if os.path.basename(sd) == "ios3ds-sd":
        say("\nCopy the CONTENTS of %s to the root of your SD card." % sd)
    say("Then see README.md, section 'Install' -> 'Start it'.")


if __name__ == "__main__":
    main()
