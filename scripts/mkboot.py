#!/usr/bin/env python3
"""Turn the staged 4.2.1 kernelcache + device tree into what the 3DS loader boots.

  scripts/mkboot.py [--staging build/staging] [--out build/sd/ios3ds] [--cmdline "..."]

Writes:
  kernel.bin   flat physical image of the kernelcache, loaded at kernel_pa
               (segments placed at vmaddr - virtBase + physBase, bss zeroed),
               with the patches from kernel/patches.json applied
  devtree.bin  device tree generated from the n72ap template (kernel/dtgen.py)
  boot.cfg     loader configuration read by the ARM9 (layout + boot-args)

Memory layout (all inside FCRAM, see docs/m1-design.md):
  0x20000000                 physBase (VA 0x80000000)
  0x20001000                 kernelcache (VA 0x80001000 ..)
  kernel_end (4 KiB aligned) device tree
  next 4 KiB                 boot_args
  next 4 KiB                 XNU log ring (256 KiB)
  1 MiB aligned              topOfKernelData (kernel builds its page tables here)
"""
import argparse
import hashlib
import json
import os
import struct
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "scripts"))
sys.path.insert(0, os.path.join(ROOT, "kernel"))
from fwlib.macho import MachO  # noqa: E402
import versions  # noqa: E402

PHYS_BASE = 0x20000000
VIRT_BASE = 0x80000000
MEM_SIZE = 0x08000000
RING_BUF = 0x40000            # power of two; ring header (16 bytes) precedes it
RING_SIZE = RING_BUF + 16
SHIM_PA = 0x20840000          # fixed: the shim is linked at VA 0x80840000 (boot/common/ios3ds.h)
SHIM_SIZE = 0x10000
DEFAULT_CMDLINE = "rd=md0 debug=0x8 serial=1 amfi_allow_any_signature=1 cs_enforcement_disable=1 amfi_unrestrict_task_for_pid=1"


def align(x, a):
    return (x + a - 1) & ~(a - 1)


def flat_kernel(mo, blob):
    # __PAGEZERO (iOS 2.x caches) has a vmsize but no file contents: including
    # it would allocate a 3 GiB flat image with a 0x0 base. 4.2.1 has no such
    # segment, so its flat image and hashes stay exactly as before.
    segs = [s for s in mo.segments if s["vmsize"] > 0 and s["filesize"] > 0]
    lo = min(s["vmaddr"] for s in segs)
    hi = max(s["vmaddr"] + s["vmsize"] for s in segs)
    img = bytearray(hi - lo)
    for s in segs:
        n = min(s["filesize"], s["vmsize"])
        off = s["vmaddr"] - lo
        img[off:off + n] = blob[s["fileoff"]:s["fileoff"] + n]
    return lo, img


def layout(kernel_va, kernel_len, dt_len, virt_base=VIRT_BASE):
    kernel_pa = kernel_va - virt_base + PHYS_BASE
    devtree_pa = align(kernel_pa + kernel_len, 0x1000)
    bootargs_pa = align(devtree_pa + dt_len, 0x1000)
    shim_pa = SHIM_PA
    if bootargs_pa + 0x1000 > shim_pa:
        raise SystemExit("kernel + device tree + boot_args overlap the shim at %#x" % SHIM_PA)
    ring_pa = shim_pa + SHIM_SIZE
    top_pa = align(ring_pa + RING_SIZE, 0x100000)
    return {"kernel_pa": kernel_pa, "devtree_pa": devtree_pa, "bootargs_pa": bootargs_pa,
            "shim_pa": shim_pa, "ring_pa": ring_pa, "ring_size": RING_SIZE, "top_pa": top_pa}


def build_shim(outdir, staging=None, version=versions.DEFAULT, virt_base=None):
    """Build kernel/shim (entry.S, blk.c, the input and GPU services) linked at its kernel VA.

    Darwin 9/10 kernelcaches use virt_base 0xC0000000 instead of 0x80000000,
    so the shim must be linked at the matching VA (same physical address).
    """
    if virt_base is None:
        virt_base = versions.virt_base(version)
    import subprocess
    import kpi
    import tempfile
    cross = subprocess.run([os.path.join(ROOT, "scripts", "setup-toolchain.sh")],
                           capture_output=True, text=True).stdout
    m = __import__("re").search(r"cross compiler: (\S+)gcc ", cross)
    if not m:
        raise SystemExit("no arm-none-eabi toolchain (scripts/setup-toolchain.sh)")
    gcc, objcopy = m.group(1) + "gcc", m.group(1) + "objcopy"
    sd = os.path.join(ROOT, "kernel", "shim")
    flags = ["-mcpu=mpcore", "-marm", "-mfloat-abi=soft", "-Os", "-ffreestanding", "-fno-builtin",
             "-fno-tree-loop-distribute-patterns", "-nostdlib", "-Wall", "-Wextra",
             # iOS' ABI lets callees clobber r9; keep our C code off it so calls into the kernel are safe
             "-ffixed-r9",
             "-I" + os.path.join(ROOT, "boot", "common"),
             "-I" + sd,
             "-I" + os.path.join(ROOT, "tools", "ios3ds-pica", "include")]
    if virt_base != VIRT_BASE:
        shim_va = virt_base + (SHIM_PA - PHYS_BASE)
        flags += ["-DIOS3DS_SHIM_VA=0x%08x" % shim_va,
                  "-DIOS3DS_GPU_PART_VA=0x%08x" % (virt_base + (GPU_BIN_PA - PHYS_BASE)),
                  "-DIOS3DS_GM_BLOB_VA=0x%08x" % (virt_base + (GPU_BIN_PA + GPU_PART_SIZE - PHYS_BASE))]
    srcs = [os.path.join(sd, "entry.S"), os.path.join(sd, "blk.c"), os.path.join(sd, "gpu_glue.c"),
            os.path.join(sd, "quiet.c"),        # IOKit quiet at boot (docs/boot-perf.md)
            os.path.join(ROOT, "tools", "ios3ds-input", "shim", "input.c"),
            os.path.join(ROOT, "tools", "ios3ds-wifi", "shim", "net.c"),
            os.path.join(ROOT, "tools", "ios3ds-mcu", "shim", "mcu.c")]
    srcs += sorted(__import__("glob").glob(os.path.join(sd, "audio*.[cS]")))     # docs/audio.md
    # the GPU part: link.ld places every object named *pica*.o at GPU_PART_VA (gpu.bin)
    srcs += [os.path.join(sd, "pica.c"),
             os.path.join(sd, "pica_stereo.c"),
             os.path.join(ROOT, "tools", "ios3ds-pica", "src", "pica_drv.c"),
             os.path.join(ROOT, "tools", "ios3ds-pica", "src", "pica_mirror.c"),
             os.path.join(sd, "cam.c")]                     # camera data side (docs/camera.md)
    # pica_diag.c (the staged [gpu-diag] bring-up diagnostics, docs/pica200.md
    # section 12) is NOT linked into the shim any more: the GPU part was at
    # 32624/32768 bytes and the mirror/perf work needs room. The diagnostics
    # already served their purpose (runs 12/13 pinned the NPOT-texture and
    # GX-clock facts); the source and the host tests against picasim stay, so
    # the staged tests remain runnable on the build host. gpu_diag=1 in
    # boot.cfg is now a no-op (no [gpu-diag] lines).
    with tempfile.TemporaryDirectory() as td:
        # KPI call addresses are version-specific; generate them from the
        # staged kernelcache. For 4.2.1 the values equal kpi.h's defaults.
        kpi.write_header(staging or os.path.join(ROOT, "build", "staging"), td)
        flags += ["-I" + td]
        link = os.path.join(sd, "link.ld")
        if virt_base != VIRT_BASE:
            # The script's four absolute addresses (and its two size asserts)
            # move with the kernel's virtual base; the gap from PHYS_BASE is
            # the same, so the shim still lands at SHIM_PA / GPU_BIN_PA.
            text = open(link).read()
            for old, new in ((0x80840000, virt_base + SHIM_PA - PHYS_BASE),
                             (0x80845F00, virt_base + SHIM_PA - PHYS_BASE + 0x5F00),
                             (0x808B0000, virt_base + GPU_BIN_PA - PHYS_BASE),
                             (0x808B8000, virt_base + GPU_BIN_PA - PHYS_BASE + GPU_PART_SIZE)):
                text = text.replace("%08X" % old, "%08X" % new).replace("%08x" % old, "%08x" % new)
            link = os.path.join(td, "link.ld")
            with open(link, "w") as f:
                f.write(text)
        elf = os.path.join(td, "shim.elf")
        objs = []
        for s in srcs:
            o = os.path.join(td, os.path.splitext(os.path.basename(s))[0] + ".o")
            # main shim part: -Os (it must end below +0x5F00; CI's newer gcc
            # overflowed it at -O2). GPU part: -O2 only for the hot paths
            # (pica_mirror.c's IRQ frame and the three functions marked PICA_HOT
            # in pica.c); the driver core, the camera data side and the PICA
            # diagnostics at -Os. The camera's frame-buffer allocator shares
            # this part, so keep ~1 KiB of room for CI's larger codegen.
            # The GPU part must end below 0x808B8000, and CI's devkitARM emits
            # ~600 bytes more than the Arm GNU toolchain here. -Os on the
            # cold-ish objects leaves ~1.6 KiB of headroom with this gcc.
            b = os.path.basename(s)
            opt = ["-O2"] if b == "pica_mirror.c" else []
            subprocess.run([gcc] + flags + opt + ["-c", s, "-o", o], check=True)
            objs.append(o)
        subprocess.run([gcc] + flags + ["-T", link] + objs +
                       ["-lgcc", "-o", elf, "-Wl,--no-warn-rwx-segments"], check=True)
        out = os.path.join(outdir, "shim.bin")
        subprocess.run([objcopy, "-O", "binary", "-R", ".gpu", elf, out], check=True)
        gpart = os.path.join(td, "gpu_part.bin")
        subprocess.run([objcopy, "-O", "binary", "-j", ".gpu", elf, gpart], check=True)
        gpu = open(gpart, "rb").read()
        nm = objcopy[:-len("objcopy")] + "nm"
        syms = {}
        for line in subprocess.run([nm, elf], capture_output=True, text=True).stdout.splitlines():
            parts = line.split()
            if len(parts) == 3 and parts[1] in "Tt":
                syms[parts[2]] = int(parts[0], 16)
    blob = open(out, "rb").read()
    print("[mkboot] shim main part %d of %d bytes, GPU part %d of %d bytes" % (len(blob), 0x5F00, len(gpu), GPU_PART_SIZE))
    if len(blob) > 0x6000 or blob[4:8] != b"SHIM":
        raise SystemExit("bad shim build (%d bytes)" % len(blob))
    build_gpu_bin(outdir, gpu)
    return blob, syms


GPU_PART_SIZE = 0x8000        # boot/common/gpu_mirror.h: GPU part at 0x208B0000, blob at 0x208B8000
GPU_BIN_PA = 0x208B0000


def build_gpu_bin(outdir, part):
    """gpu.bin = the shim's GPU part (padded to 32 KiB) + the mirror's command lists."""
    import subprocess
    if len(part) > GPU_PART_SIZE or struct.unpack_from("<I", part, 0)[0] != 0x30555047:
        raise SystemExit("bad shim GPU part (%d bytes)" % len(part))
    subprocess.run(["make", "-s", "-C", os.path.join(ROOT, "tools", "ios3ds-pica"), "mirror"], check=True)
    mirror = open(os.path.join(ROOT, "build", "pica-host", "gpumirror.bin"), "rb").read()
    with open(os.path.join(outdir, "gpu.bin"), "wb") as f:
        f.write(part + bytes(GPU_PART_SIZE - len(part)) + mirror)


def configure_shim(outdir, cfg):
    """Patch the shim's config words (+0x70 vic0 PA, +0x74 clcd PA)."""
    p = os.path.join(outdir, "shim.bin")
    blob = bytearray(open(p, "rb").read())
    struct.pack_into("<II", blob, 0x70, cfg.get("vic0", 0), cfg.get("clcd", 0))
    i = blob.find(b"AUD0cfg!")      # aud_cfg_t (kernel/shim/audio.h): fake PL080 / I2S pages
    if i >= 0:
        struct.pack_into("<II", blob, i + 8, cfg.get("dmac0", 0), cfg.get("i2s0", 0))
        struct.pack_into("<I", blob, i + 40, cfg.get("gpio", 0))    # aud_cfg_t.gpio_pa
    open(p, "wb").write(blob)


def use_prebuilt_shim(outdir, prebuilt):
    """Copy a released shim.bin + gpu.bin (as `--shim-only` writes them) instead of compiling."""
    import shutil
    for fn in ("shim.bin", "gpu.bin"):
        shutil.copyfile(os.path.join(prebuilt, fn), os.path.join(outdir, fn))
    blob = open(os.path.join(outdir, "shim.bin"), "rb").read()
    if len(blob) > 0x6000 or blob[4:8] != b"SHIM":
        raise SystemExit("bad prebuilt shim.bin (%d bytes)" % len(blob))
    return blob, {}


def build(staging, outdir, cmdline=None, patches=True, verbose=True,
          prebuilt=None, hfsplus=None, rootfs_add=None, cydia=True, cydia_cache=None,
          tlsfix=True, sandbox_open=False, music=None, test_song=True,
          version=versions.DEFAULT):
    """prebuilt: directory with shim.bin + gpu.bin (no cross compiler needed);
    hfsplus / rootfs_add / cydia / cydia_cache / tlsfix / music / test_song: passed on to mkrootfs.build;
    sandbox_open: apply the Seatbelt sb_evaluate file-read relaxation (opt-in, docs/cydia.md);
    version: iOS version from scripts/versions.py (default 4.2.1 = original behaviour)."""
    import dtgen
    import patcher

    virt_base = versions.virt_base(version)
    blob = open(os.path.join(staging, "kernelcache.macho"), "rb").read()
    mo = MachO(blob)
    kva, kimg = flat_kernel(mo, blob)
    template = open(os.path.join(staging, "devicetree.bin"), "rb").read()
    os.makedirs(outdir, exist_ok=True)
    rootfs = os.path.join(staging, "rootfs.hfs")
    ramdisk_len = os.path.getsize(rootfs) & ~0xFFF if os.path.exists(rootfs) else 0
    if ramdisk_len:
        import mkrootfs
        mkrootfs.build(staging, os.path.join(outdir, "rootfs.hfs"), verbose=verbose,
                       hfsplus=hfsplus, rootfs_add=rootfs_add, cydia=cydia,
                       cydia_cache=cydia_cache, tlsfix=tlsfix, music=music, test_song=test_song,
                       recipe_path=versions.recipe_path(version, os.path.dirname(staging.rstrip("/"))))
        ramdisk_len = os.path.getsize(os.path.join(outdir, "rootfs.hfs")) & ~0xFFF
    shim, shim_syms = (use_prebuilt_shim(outdir, prebuilt) if prebuilt
                       else build_shim(outdir, staging, version, virt_base))
    # the DT size doesn't depend on the layout values, so two passes converge
    lenient_dt = version != versions.DEFAULT
    dt_extra_delete = versions.get(version).get("dt_delete", ())
    lay0 = layout(kva, len(kimg), len(template), virt_base)
    lay0["ramdisk_len"] = ramdisk_len
    dt = dtgen.generate(template, lay0,
                        PHYS_BASE, virt_base, MEM_SIZE, len(kimg), lenient=lenient_dt, extra_delete=dt_extra_delete)
    lay = layout(kva, len(kimg), len(dt), virt_base)
    lay["ramdisk_len"] = ramdisk_len
    dt = dtgen.generate(template, lay, PHYS_BASE, virt_base, MEM_SIZE, len(kimg),
                        lenient=lenient_dt, extra_delete=dt_extra_delete)
    chk = layout(kva, len(kimg), len(dt), virt_base)
    chk["ramdisk_len"] = ramdisk_len
    assert chk == lay
    applied = []
    if patches:
        syms = {"shim": lay["shim_pa"] - PHYS_BASE + virt_base}
        syms.update(shim_syms)      # shim_sb_evaluate & co for branch-to-symbol patches
        if os.environ.get("IOS3DS_SANDBOX_OPEN", "").lower() in ("1", "yes", "on", "true"):
            sandbox_open = True
        disabled = () if sandbox_open else ("sandbox-sb-evaluate",)
        disabled += tuple(x for x in os.environ.get("IOS3DS_DISABLE_PATCHES", "").split(",") if x)
        patch_files = versions.patch_files(version)
        if os.environ.get("IOS3DS_PATCH_FILES"):
            # bring-up only: an alternative recipe (e.g. a subset under build/)
            patch_files = os.environ["IOS3DS_PATCH_FILES"].split(",")
        applied = patcher.apply_all(kimg, kva, os.path.join(ROOT, "kernel", "patches.json"), syms, disabled,
                                    files=patch_files)
        if verbose and sandbox_open:
            print("[mkboot] sandbox-sb-evaluate applied (SANDBOX=1): TLSFix reaches sandboxed apps; "
                  "Safari OOMs on 128 MB (docs/cydia.md)")

    entry_pa = mo.entry - virt_base + PHYS_BASE
    os.makedirs(outdir, exist_ok=True)
    with open(os.path.join(outdir, "kernel.bin"), "wb") as f:
        f.write(kimg)
    with open(os.path.join(outdir, "devtree.bin"), "wb") as f:
        f.write(dt)
    with open(os.path.join(outdir, "fakeio.bin"), "wb") as f:
        f.write(dtgen.fakeio_image())
    page = lambda n: dtgen.FAKEIO_BASE + dtgen.PAGE_MAP[n][0] * 0x1000 if n in dtgen.PAGE_MAP else 0
    configure_shim(outdir, {"vic0": page("vic"), "clcd": page("clcd"),
                            "dmac0": page("dmac0"), "i2s0": page("i2s0"), "gpio": page("gpio")})
    cfg = [
        "# ios3ds loader config - generated by scripts/mkboot.py",
        "# kernel sha1 %s, %d patches applied" % (hashlib.sha1(kimg).hexdigest(), len(applied)),
        "kernel=0:/ios3ds/kernel.bin",
        "devtree=0:/ios3ds/devtree.bin",
        "kernel_pa=0x%08x" % lay["kernel_pa"],
        "entry_pa=0x%08x" % entry_pa,
        "devtree_pa=0x%08x" % lay["devtree_pa"],
        "bootargs_pa=0x%08x" % lay["bootargs_pa"],
        "# fake S5L8720 register pages (AXI WRAM), see kernel/dtgen.py",
        "load=0:/ios3ds/fakeio.bin,0x%08x" % dtgen.FAKEIO_BASE,
        "# fake S5L CLCD registers: the ARM9 mirrors window 1 onto the top screen",
        "display_regs=0x%08x" % (dtgen.FAKEIO_BASE + dtgen.PAGE_MAP["clcd"][0] * 0x1000),
        "shim=0:/ios3ds/shim.bin",
        "# the shim's GPU part + the display mirror's PICA200 command lists (boot/common/gpu_mirror.h)",
        "load=0:/ios3ds/gpu.bin,0x%08x" % GPU_BIN_PA,
        ("rootfs=0:/ios3ds/rootfs.hfs" if ramdisk_len else "# no rootfs.hfs staged"),
        "shim_pa=0x%08x" % lay["shim_pa"],
        "ring_pa=0x%08x" % lay["ring_pa"],
        "ring_size=0x%x" % lay["ring_size"],
        "top_pa=0x%08x" % lay["top_pa"],
        "phys_base=0x%08x" % PHYS_BASE,
        "virt_base=0x%08x" % virt_base,
        "mem_size=0x%08x" % MEM_SIZE,
        "# boot_args header: 4.2.1's pe_identify_machine panics 'Epoch Mismatch' unless version=5",
        "ba_revision=1",
        "ba_version=%d" % versions.ba_version(version),
        "# Video: framebuffer handed to XNU (0 = none)",
        "video_pa=0x0",
        "video_display=0",
        "video_rowbytes=0",
        "video_width=0",
        "video_height=0",
        "video_depth=0",
        "# ARM11 instruction probes: a bring-up check of the instructions XNU uses (0.25 s;",
        "# diag=1 runs them too)",
        "probes=0",
        "# set jump=0 to stop before starting XNU; hold B during the countdown to stay in the monitor,",
        "# or press A to skip the wait and start XNU at once. boot_delay=0 skips it by default",
        "# (the 1 s grace period is only there to make B available; old cards may use countdown_ms)",
        "jump=1",
        "boot_delay=1000",
        "# iOS screen mode, X cycles at run time: 0 = top (log on bottom), 1 = both (bottom = touch map), 2 = bottom only",
        "screen=1",
        "# PICA200 GPU (docs/pica200.md): gpu=0 keeps the kernel away from the GPU entirely",
        "# (GL falls back to the CPU rasterizer, the ARM9 copies the screen); gpu_mirror=0",
        "# keeps only the screen copy on the ARM9. The mirror self-tests itself and falls back",
        "# to the ARM9 copy on any mismatch or timeout. gpu_vbl_div: a GPU frame at most every",
        "# Nth VBlank (2 = 30 fps). gpu_spin_us: IRQ-time wait for the GPU (0 = never wait)",
        "gpu=1",
        "gpu_mirror=1",
        "gpu_vbl_div=2",
        "gpu_spin_us=0",
        "# gpu_clk=1: set the GX clock register to 0x70100 (GSP/libn3ds value) if Luma left it 0;",
        "# gpu_diag=1: staged GPU tests at bring-up, logged as [gpu-diag] (docs/pica200.md section 12)",
        "gpu_clk=1",
        "gpu_diag=1",
        "# ca_gpu=1: SpringBoard's CoreAnimation composites on the PICA200 when the GPU self-checks pass",
        "# (else its software compositor, as with ca_gpu=0; docs/gpu.md section 7)",
        "ca_gpu=1",
        "# system controller (docs/mcu.md): RTC, battery, HOME/POWER, sliders, accelerometer.",
        "# mcu=0 never touches it (clock at 1970, no battery). rtc_offset: seconds added to the",
        "# 3DS RTC to get UTC, e.g. -3600 if the RTC runs on CET (UTC+1). mcu_acc=0 hides the",
        "# accelerometer from iOS (no rotation)",
        "mcu=1",
        "rtc_offset=0",
        "mcu_acc=1",
        "# gyroscope (I2C bus 3) for CoreMotion (docs/mcu.md section 11). gyro=0 hides it from iOS;",
        "# gyro_axes: iOS x,y,z from the chip's axes (x right, y to the hinge, z out of the lower",
        "# screen if the chip sits like the console; unverified on hardware)",
        "gyro=1",
        "gyro_axes=-y,x,z",
        "# powersave=0: keep copying the screen and polling at full rate while iOS' screen is off",
        "powersave=1",
        "# notify_led=1: the notification LED blinks red at <= 10 % battery (pattern unverified on hardware)",
        "notify_led=0",
        "# hp_route=0: iOS is never told about the headphone jack and stays on its speaker route",
        "# (the CODEC still switches speaker/headphones itself; docs/audio.md)",
        "hp_route=1",
        "# hp_codec=0: headphone output as in 0.4.0 (only the CODEC's output select switches);",
        "# 1 = also libn3ds' headphone filters, the headphone driver re-powered on plug-in and",
        "# the Old 3DS de-pop line driven low (docs/audio.md section 4)",
        "hp_codec=1",
        "# camera=0: the kernel never touches the 3DS cameras; iOS's camera device reports",
        "# none and Camera.app shows no picture (docs/camera.md)",
        "camera=1",
        "# cam_test=1: camera self-test before iOS starts (each sensor x CAM set-up, adds ~6 s to",
        "# the boot; [cam-test] lines in log.txt, docs/camera.md section 7)",
        "cam_test=0",
        "# New 3DS / New 2DS only, ignored on other models (docs/n3ds.md). n3ds_clock=0 keeps the",
        "# ARM11 at 268 MHz; n3ds_l2=1 turns on the 2 MiB L2 cache (experimental, write-through);",
        "# n3ds_hid=0 never reads the C-stick / ZL / ZR chip; n3ds_ram=0 keeps iOS at 128 MiB instead of",
        "# the 256 MiB of FCRAM a New 3DS has (the extension is probed first)",
        "n3ds_clock=1",
        "n3ds_l2=0",
        "n3ds_hid=1",
        "n3ds_ram=1",
        "# auto_brightness=0: never use the New model's ambient light measurement (the inner",
        "# camera's AE luminance), so iOS keeps the brightness from its slider (docs/mcu.md section 12).",
        "# On Old models this switch has no effect at all.",
        "auto_brightness=1",
        "# diag=1: diagnostics for bring-up (docs/boot-perf.md): adds the boot-arg ios3ds_diag=1,",
        "# which starts the test launchd jobs (ios3ds-gltest, the [smp] sampler)",
        "diag=0",
        "# gps=LAT,LON[,ACC]: fake GPS location reported to apps (empty = off). Example:",
        "# gps=52.5200,13.4050",
        "gps=",
        "# stereo3d=1: experimental stereoscopic 3D home screen (docs/stereo3d.md).",
        "# The GPU display mirror stays on: flat frames (slider down, apps) cost the",
        "# same as stereo3d=0, and with the slider up the PICA200 builds both eyes.",
        "# The ARM9 two-eye copy is the fallback if the mirror stops. Needs a console test.",
        "stereo3d=0",
        "# ioprof=1: an [io] md0 line per second (2: plus a [blk] line per request); log_ts=1:",
        "# [s.ms] timestamps on the forwarded console lines (docs/boot-perf.md)",
        "ioprof=0",
        "log_ts=0",
        "# XNU boot-args (edit freely, max 255 chars)",
        "cmdline=%s" % (cmdline if cmdline is not None else DEFAULT_CMDLINE),
        "",
    ]
    if version != versions.DEFAULT:
        cfg.insert(1, "# iOS %s (%s)" % (version, versions.get(version)["build"]))
        # per-version boot.cfg values (scripts/versions.py); 4.2.1 has none
        for key, (val, why) in sorted(versions.boot_cfg(version).items()):
            hit = [i for i, l in enumerate(cfg) if l.startswith(key + "=")]
            if len(hit) != 1:
                raise SystemExit("[mkboot] boot_cfg override %r: no unique line" % key)
            cfg[hit[0]:hit[0] + 1] = ["# iOS %s: %s" % (version, why), "%s=%s" % (key, val)]
    with open(os.path.join(outdir, "boot.cfg"), "w", newline="\r\n") as f:
        f.write("\n".join(cfg))
    if verbose:
        print("[mkboot] kernel %d bytes @0x%08x, entry 0x%08x, %d patches" % (
            len(kimg), lay["kernel_pa"], entry_pa, len(applied)))
        for p in applied:
            print("[mkboot]   patch %s @0x%08x: %s" % (p["name"], p["va"], p["why"]))
        print("[mkboot] devtree %d bytes @0x%08x, boot_args @0x%08x, shim %d bytes @0x%08x, ring @0x%08x, top 0x%08x" % (
            len(dt), lay["devtree_pa"], lay["bootargs_pa"], len(shim), lay["shim_pa"], lay["ring_pa"], lay["top_pa"]))
    return lay


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--staging", default=os.path.join(ROOT, "build", "staging"))
    ap.add_argument("--out", default=os.path.join(ROOT, "build", "sd", "ios3ds"))
    ap.add_argument("--cmdline", default=None)
    ap.add_argument("--no-patches", action="store_true")
    ap.add_argument("--ios", default=versions.DEFAULT, help="iOS version (default 4.2.1)")
    ap.add_argument("--shim-only", metavar="DIR",
                    help="only compile shim.bin + gpu.bin into DIR (unconfigured; for releases)")
    a = ap.parse_args()
    if a.shim_only:
        os.makedirs(a.shim_only, exist_ok=True)
        build_shim(a.shim_only, a.staging, a.ios)
        print("[mkboot] shim.bin + gpu.bin -> %s" % a.shim_only)
        return
    build(a.staging, a.out, a.cmdline, patches=not a.no_patches, version=a.ios)


if __name__ == "__main__":
    main()
