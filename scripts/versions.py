#!/usr/bin/env python3
"""Registry of iOS versions the ios3ds build can target.

The 4.2.1 entry is the *default* and deliberately carries no overrides: all
existing code paths (build/staging, kernel/patches.json, rootfs/recipe.json,
boot_args version 5) are used unchanged, so default builds stay byte-identical.

Other versions name their own patch set and an optional rootfs recipe overlay.
An overlay is a JSON file in rootfs/ whose dictionaries merge into the base
recipe and whose lists/scalars replace it; `file_patches_search: true` converts
the base file patches from fixed offsets to unique byte-pattern searches.
Generated full recipes are written under build/fw-VERSION/ (gitignored).
"""
import json
import os

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT = "4.2.1"

# IPSW sha1 values below are the ones published by api.ipsw.me; the value for
# 4.2.1 is also pinned in scripts/fwprep.py (kept there for backwards compat).
VERSIONS = {
    "2.1.1": {"build": "5F138", "ipsw": "iPod2,1_2.1.1_5F138_Restore.ipsw",
              "sha1": "c3c700be49ad227d1152188e7c1e46b8958fd1e4", "ba_version": None,
              "patch_set": None, "overlay": None,
              "status": "inventory only: no patch set or recipe yet"},
    "2.2": {"build": "5G77a", "ipsw": "iPod2,1_2.2_5G77a_Restore.ipsw",
            "sha1": "34a0a489605f34d6cc6c9954edcaaf9a050deedc", "ba_version": None,
            "patch_set": None, "overlay": None, "status": "inventory only"},
    "2.2.1": {"build": "5H11a", "ipsw": "iPod2,1_2.2.1_5H11a_Restore.ipsw",
              "sha1": "9af5625ea34acdd8abeb6fce71a72651d0c815d5", "ba_version": 3,
              "virt_base": 0xC0000000, "irq_cont": 0xC006540C, "irq_spsr": True,
              "dt_delete": ["arm-io/usb-otg"],
              # (no_battery_ps is gone: the IOPMrootDomain free was
              # AppleEmbeddedAudio's PM handler, patch audio-pm-msg-no-store)
              # Darwin 9 vtable pointers point at __ZTV itself (+8 on 3.x/4.x)
              "vt_bias": 8,
              # no IOUserEthernet in 5H11a: the shim registers en0 itself
              # (tools/ios3ds-wifi/shim/kif_eth.c, scripts/kpi.py KIF_SYMBOLS)
              "kernel_ifnet": True,
              # no GL engine on 2.x: CoreAnimation composites in software and
              # nothing marks a new frame for the PICA200 mirror (see 3.1.3)
              "boot_cfg": {"gpu_mirror": ("0", "CoreAnimation composites in software (no GL "
                                          "engine on 2.x): the ARM9 copies the screen, re-log 128")},
              "patch_set": "kernel/patchsets/2.2.1.json", "overlay": "rootfs/overlay-2.2.1.json",
              "status": "experimental: Darwin 9, no dyld cache; see docs/multi-version.md"},
    "3.0": {"build": "7A341", "ipsw": "iPod2,1_3.0_7A341_Restore.ipsw", "sha1": None,
            "ba_version": None, "patch_set": None, "overlay": None,
            "status": "inventory only; keys no longer published by ipsw.me"},
    "3.1": {"build": "7C144", "ipsw": "iPod2,1_3.1_7C144_Restore.ipsw", "sha1": None,
            "ba_version": None, "patch_set": None, "overlay": None,
            "status": "inventory only; keys no longer published by ipsw.me"},
    "3.1.1": {"build": "7C145", "ipsw": "iPod2,1_3.1.1_7C145_Restore.ipsw", "sha1": None,
              "ba_version": None, "patch_set": None, "overlay": None,
              "status": "inventory only"},
    "3.1.2": {"build": "7D11", "ipsw": "iPod2,1_3.1.2_7D11_Restore.ipsw", "sha1": None,
              "ba_version": None, "patch_set": None, "overlay": None,
              "status": "inventory only"},
    "3.1.3": {"build": "7E18", "ipsw": "iPod2,1_3.1.3_7E18_Restore.ipsw",
              # The Apple CDN link published for 7E18 now returns 403; this is
              # the sha1 of the archived Apple IPSW used for the bring-up
              # (archive.org/ipod-touch-2g-ipsw). Its keys decrypt the kernel,
              # device tree and HFS+ rootfs, so it is the genuine restore file.
              "sha1": "5f4f5c01eda2f811f73167e7d1f82dbeed82367b", "ba_version": 5,
              "virt_base": 0xC0000000, "irq_cont": 0xC0068458, "irq_spsr": True,
              # mic input: 7E18's VirtualAudio route (N82 slot, overlay) adds
              # no +23.65 dB like 4.2.1's N81 route, so the shim passes the
              # 3DS mic at unity (QEMU: 1000 -> 46.6 with 4.2.1's 2151)
              "mic_gain_q15": 32768,
              # 7E18's final CA swap can switch the CLCD to the other
              # IOSurface after the GL present that marked the frame dirty;
              # the mirror must notice the new source PA as well (re-log 135)
              "clcd_swap_dirty": True,
              # The PICA200 CoreAnimation path is opt-in on 3.1.3 (re-log
              # NEXT): it is slower than the software compositor in QEMU and
              # the last frame after Home can be missed. The default is
              # software CA + the ARM9 copy (gpu_mirror=0, CA_ENABLE_OGL=0);
              # IOS3DS_GPU313=1 at build time selects the GPU path, with the
              # EAGLServer API patch and the display mirror on.
              "boot_cfg": {"gpu_mirror": ("0", "3.1.3 defaults to software CoreAnimation + the ARM9 "
                                          "screen copy; build with IOS3DS_GPU313=1 for the PICA200 path")},
              "patch_set": "kernel/patchsets/3.1.3.json", "overlay": "rootfs/overlay-3.1.3.json",
              "status": "experimental: Darwin 10.0, archived IPSW; see docs/multi-version.md"},
    "4.0": {"build": "8A293", "ipsw": "iPod2,1_4.0_8A293_Restore.ipsw",
            "sha1": "c026c373bc535496a6f901de2ba37d4a487413bf", "ba_version": 5,
            "patch_set": None, "overlay": None, "status": "inventory only"},
    "4.0.2": {"build": "8A400", "ipsw": "iPod2,1_4.0.2_8A400_Restore.ipsw",
              "sha1": "06a42297d94461264eb64d7c8640cc5d1c19edeb", "ba_version": 5,
              "patch_set": None, "overlay": None, "status": "inventory only"},
    "4.1": {"build": "8B117", "ipsw": "iPod2,1_4.1_8B117_Restore.ipsw",
            "sha1": "97abde6207660bd876fd476275dd526d0dcf3d19", "ba_version": 5,
            "patch_set": "kernel/patchsets/4.1.json", "overlay": "rootfs/overlay-4.1.json",
            "status": "experimental: see docs/multi-version.md"},
    "4.2": {"build": "8C134", "ipsw": "iPod2,1_4.2_8C134_Restore.ipsw", "sha1": None,
            "ba_version": 5, "patch_set": None, "overlay": None,
            "status": "inventory only"},
    "4.2.1": {"build": "8C148", "ipsw": "iPod2,1_4.2.1_8C148_Restore.ipsw",
              "sha1": "b9efddc7bb4350c237a8d3846af61bbfc8a2f647", "ba_version": 5,
              "patch_set": None, "overlay": None,
              "status": "supported, the default"},
}


def get(version=DEFAULT):
    if version not in VERSIONS:
        raise SystemExit("[versions] unknown iOS version %r (known: %s)"
                         % (version, ", ".join(VERSIONS)))
    return VERSIONS[version]


def gpu313(version=DEFAULT):
    """Is the experimental 3.1.3 PICA200 CoreAnimation path enabled?

    A build-time switch, not a boot.cfg key: the EAGLServer API fix is a
    dyld-cache patch and SpringBoard's CA_ENABLE_OGL comes from the rootfs
    recipe, so both have to be baked into the image. Off by default on every
    version except when IOS3DS_GPU313=1 is exported for 3.1.3.
    """
    return version == "3.1.3" and os.environ.get("IOS3DS_GPU313", "").lower() in (
        "1", "yes", "on", "true")


def boot_cfg(version=DEFAULT):
    """boot.cfg overrides for `version` (the 4.2.1 default has none).

    The opt-in 3.1.3 GPU path keeps the generated gpu_mirror=1; otherwise
    the version's gpu_mirror=0 override turns the display mirror off.
    """
    cfg = dict(get(version).get("boot_cfg") or {})
    if gpu313(version):
        cfg.pop("gpu_mirror", None)
    return cfg


def patch_files(version=DEFAULT):
    """Absolute patch recipe paths. None means the 4.2.1 default set."""
    info = get(version)
    if not info["patch_set"]:
        return None
    return [os.path.join(ROOT, info["patch_set"])]


def ba_version(version=DEFAULT):
    v = get(version)["ba_version"]
    if v is None:
        raise SystemExit("[versions] boot_args epoch for iOS %s is unknown" % version)
    return v


def virt_base(version=DEFAULT):
    """Kernel virtual base: Darwin 9/10 pre-4.x caches use 0xC0000000.

    Versions without an override get the 4.2.1 value, so their boot.cfg and
    shim link address are unchanged.
    """
    return get(version).get("virt_base", 0x80000000)


def _merge(base, overlay):
    out = dict(base)
    for k, v in overlay.items():
        if k.startswith("_"):
            continue
        if isinstance(v, dict) and v.get("_replace") is True:
            # Older firmwares have no MBXGLEngine/Substrate/etc.; dropping a
            # whole base-recipe dictionary needs an explicit marker, because a
            # plain {} would normally merge nothing.
            out[k] = {kk: vv for kk, vv in v.items() if not kk.startswith("_")}
        elif isinstance(v, dict) and isinstance(out.get(k), dict):
            out[k] = _merge(out[k], v)
        else:
            out[k] = v
    return out


def recipe_path(version=DEFAULT, workdir=None):
    """Path of a rootfs recipe for `version` (None = rootfs/recipe.json).

    For an overlay version the merged recipe is generated in `workdir` (create
    it first), so the base recipe in the repository is never modified.
    """
    info = get(version)
    if not info["overlay"]:
        return None
    if not workdir:
        raise SystemExit("[versions] workdir needed for the %s recipe" % version)
    with open(os.path.join(ROOT, "rootfs", "recipe.json")) as f:
        base = json.load(f)
    with open(os.path.join(ROOT, info["overlay"])) as f:
        overlay = json.load(f)
    search = overlay.pop("file_patches_search", False)
    replacements = overlay.pop("new_hex_replace", [])
    extra = overlay.pop("extra_file_patches", [])
    fp_offsets = overlay.pop("file_patch_offsets", None)
    by_base = overlay.pop("file_patch_offsets_by_base", None)
    if version == "3.1.3" and not gpu313(version):
        # The EAGLServer API patch only belongs to the opt-in GPU image.
        # Software CA never asks that server for an OGL renderer.
        kept = [fp for fp in extra if fp.get("offset") != "0x1bde788"]
        if len(kept) == len(extra):
            raise SystemExit("[versions] 3.1.3 overlay lost the EAGLServer API patch")
        extra = kept
    recipe = _merge(base, overlay)
    if version == "3.1.3":
        # The overlay describes the default (software CA + ARM9 copy). The
        # opt-in build flips SpringBoard to the GL compositor. No 4.x recipe
        # or overlay is touched by either branch.
        env = recipe["plist_set"]["/System/Library/LaunchDaemons/com.apple.SpringBoard.plist"][
            "EnvironmentVariables"]
        if gpu313(version):
            env["CA_ENABLE_OGL"] = "1"
            env.pop("LK_ENABLE_OGL", None)
            if os.environ.get("IOS3DS_GPU313_DIAG") in ("1", "yes", "on", "true"):
                env["IOS3DS_CA_SWAP_DIAG"] = "1"
        else:
            env["CA_ENABLE_OGL"] = "0"
            env["LK_ENABLE_OGL"] = "0"
    if by_base is not None:
        # Key by the *base* recipe's fixed offset, not by list position: patches
        # are added to rootfs/recipe.json in other branches/versions, and a
        # positional list would silently apply one patch's bytes to another
        # patch's location. Unmapped patches are skipped with a loud message.
        relocated = []
        for base_fp, fp in zip(base.get("file_patches", []), recipe.get("file_patches", [])):
            key = base_fp.get("offset")
            if key not in by_base:
                print("[versions] no %s offset for base file patch %s: skipped"
                      % (version, key))
                continue
            off = by_base[key]
            if off is None:      # patch not applicable on this firmware
                continue
            fp["offset"] = off
            fp.pop("search", None)
            relocated.append(fp)
        recipe["file_patches"] = relocated
    elif fp_offsets is not None:
        relocated = []
        for fp, off in zip(recipe.get("file_patches", []), fp_offsets):
            if off is None:      # patch not applicable on this firmware
                continue
            fp["offset"] = off
            fp.pop("search", None)
            relocated.append(fp)
        recipe["file_patches"] = relocated
    for fp in recipe.get("file_patches", []) + extra:
        h = fp["new"]
        for old, new in replacements:
            h = h.replace(old, new)
        fp["new"] = h
    recipe["file_patches"] = recipe.get("file_patches", []) + extra
    if search and fp_offsets is None and by_base is None:
        # relocate every fixed-offset file patch by its verified original bytes
        for fp in recipe.get("file_patches", []):
            fp.pop("offset", None)
            fp["search"] = {}
        recipe["_file_patches_search"] = "offsets replaced by original-byte searches"
    os.makedirs(workdir, exist_ok=True)
    path = os.path.join(workdir, "recipe.json")
    with open(path, "w") as f:
        json.dump(recipe, f, indent=1)
    return path


def staging_dir(version=DEFAULT, build="build"):
    """Where firmware for this version is extracted (4.2.1 keeps build/staging)."""
    get(version)
    if version == DEFAULT:
        return os.path.join(build, "staging")
    return os.path.join(build, "fw-" + version, "staging")


def main():
    import argparse
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("version", nargs="?", default=DEFAULT)
    a = ap.parse_args()
    info = get(a.version)
    print("iOS %s (%s): %s" % (a.version, info["build"], info["status"]))
    print("IPSW: %s" % info["ipsw"])
    print("sha1: %s" % (info["sha1"] or "not published"))
    print("staging: %s" % staging_dir(a.version))


if __name__ == "__main__":
    main()
