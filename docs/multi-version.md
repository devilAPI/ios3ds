# Multi-version support: research and design (stage 1)

Status: **design only** for the 1.x backend (not implemented in this task). The
4.2.1 path stays the default and must stay byte-identical. Stage 2 added the
version switches; stages 3–4 brought up iOS 4.1 in QEMU to the home screen
(§5.5), with the remaining blockers recorded at the end of this document.

Legend: **[V]** = verified in this task (a fetched API response, a file in
this repository, or an actual build/boot); **[G]** = reasoned guess, needs a
firmware and a test before it can be relied on. Nothing in this document is
claimed as tested on real 3DS hardware for other iOS versions.

## 1. Which firmwares exist for the iPod touch 2G (iPod2,1 / N72AP / S5L8720)

**[V]** All entries below share the platform `s5l8720x`, board `n72ap`, CPU
CPID 0x8720 (ARMv6). The list combines
<https://api.ipsw.me/v4/device/iPod2,1> (7 builds: 2.1.1, 2.2, 2.2.1, 4.0,
4.0.2, 4.1, 4.2.1), <https://ipsw.io/iPod2,1> (also 4.2 / 8C134),
<https://ipswbeta.dev/ios/3.x/iPod2,1> (five 3.x builds) and the Apple key API
(<https://api.ipsw.me/v4/keys/ipsw/iPod2,1/7E18>, which confirms 3.1.3 keys are
public).

| iOS | Build | IPSW / keys | Relevant to us |
|---|---|---|---|
| 2.1.1 | 5F138 | Apple URL still listed by ipsw.me | oldest iPod2,1 release |
| 2.2 | 5G77a | Apple URL still listed | |
| 2.2.1 | 5H11a | Apple URL still listed | last 2.x; **[G]** no dyld shared cache yet |
| 3.0 | 7A341 | list verified; keys API 404 today | **[G]** first with dyld shared cache |
| 3.1 | 7C144 | list verified; keys API 404 today | **[G]** |
| 3.1.1 | 7C145 | list verified; keys API 404 today | **[G]** |
| 3.1.2 | 7D11 | list verified; keys API 404 today | **[G]** |
| 3.1.3 | 7E18 | list + keys verified | preferred second version (stage 3) |
| 4.0 | 8A293 | Apple URL listed | **[G]** Darwin 10 branch begins here |
| 4.0.1 | — | not in the lists above for this device | probably not released for iPod2,1 **[G]** |
| 4.0.2 | 8A400 | Apple URL listed | |
| 4.1 | 8B117 | Apple URL + keys verified | fallback second version (stage 3) |
| 4.2 | 8C134 | ipsw.io list | short-lived release |
| **4.2.1** | **8C148** | Apple URL + keys verified; IPSW sha1 pinned in `scripts/fwprep.py` | **current, default** |

**[V]** Apple no longer serves a 3.1.3 iPod2,1 IPSW through the public
ipsw.me/IPSW.io links (the 3.1.3 page lists iPhone2,1/1,1/3,1 but not
iPod2,1), although its keys are published and third-party archives claim a
copy. The project has no IPSW downloader: `make fwprep IPSW=…` consumes a
user-supplied file and `scripts/fwprep.py` fetches only the *keys* from
api.ipsw.me. Stage 3 therefore tries 3.1.3 from an archived copy and falls
back to 4.1 (Apple URL still live) if no verifiable copy is obtainable.

iOS 1.x does **not** exist for iPod2,1. **[V]** It requires the first-generation
iPod touch / iPhone (`iPod1,1` / `iPhone1,1`, S5L8900, N45AP). The reference
project <https://github.com/roastme/ios3ds> boots iPhone OS 1.1 (3A101a) there.
No 1.x work is implemented here.

## 2. What differs for us per version

Everything in this section is about the *same* general approach: Apple's
prelinked kernelcache, patched at build time, with our ARM11 shim and the ARM9
monitor. The common boot contract (boot_args in FCRAM, device tree, flat
kernel image, r0 = physical boot_args, MMU off) is **[V]** documented in
`docs/architecture.md` / `docs/m1-design.md` and is shared by all these
kernelcaches **[G]** (prior art: `qemu-ios` boots 8C148 without iBoot; we have
not tested an older cache yet).

### 2.1 Kernelcache layout and entry point

- **[V]** `scripts/fwprep.py` decrypts the img3 `kernelcache.release.*`, inflates
  the complzss payload and writes a Mach-O; `scripts/mkboot.py` flattens its
  `__TEXT`/`__DATA`/… segments at `vmaddr - virtBase + physBase`, uses
  `mach_header.entry` as the jump address and computes `kernel_pa`,
  `devtree_pa`, `bootargs_pa` from the resulting length. Nothing in that path
  assumes 8.148 except the *patch addresses* and the `ba_version=5` line.
- **[V]** 8C148: `__TEXT` at 0x80001000, entry 0x80067050, 8,273,920-byte
  Mach-O, `kernel.bin` ends below the fixed shim slot at 0x20840000
  (`build/staging/manifest.json`, generated boot.cfg).
- **[G]** 2.1.1–3.1.3: smaller kernelcaches (roughly 5–7 MB) and different
  `entry`; the flat image should still fit below the shim. Locations of the
  five CPU instructions from `docs/kernel-strategy.md` move with the build;
  they are not fixed between iOS releases.
- **[G]** 4.0–4.2.1: xnu-1504 family, so the *code shapes* (S5L8720 pexpert,
  AppleM2CLCD, md0, VIC/timer hooks) are close to 8C148; function addresses
  move with every build. Verify by byte-pattern search with original-byte
  checks, not by assuming an address.

### 2.2 Kernel patches (`kernel/patches*.json`)

**[V]** All 28 current recipes use a fixed virtual address (`va`) plus a strict
original-byte check. `kernel/patcher.py` stops on any mismatch, so simply
running them on another kernelcache cannot silently corrupt it — it fails.
Groups and how they are located:

| Group | File | What it patches | Locating strategy for another build |
|---|---|---|---|
| CPU (PPMR ×2, IFAR ×2, ACTLR) | `patches.json` | 4-byte ARM instructions in `__text` | short pattern; needs a surrounding-context check or a symbol-derived anchor **[G]** |
| UART console (8 patches) | `patches.json` | S5L8720 UART driver functions | find the driver by its string/constant references, then match the instruction sequence **[G]** |
| Timer hooks (3), IRQ entry, md0 strategy | `patches.json` | `cpu_data` hooks, vector path, `mdevstrategy` | pattern = 8 original bytes; fairly distinctive **[G]** |
| RTC waits, CLCD sync/backlight | `patches.json` | Thumb/ARM immediates and branches | pattern must include neighbouring instructions **[G]** |
| Audio, boot, MCU, sandbox (opt-in) | `patches-*.json` | subsystem-specific | same approach; check the target kext is present in the cache **[G]** |

**[G]** Some kexts may not exist in older kernels (e.g. AppleM2CLCD in 2.x,
sandbox in 2.x/3.x). A version must therefore name the patch files it uses,
and individual patches must be skippable when their target is absent, with a
loud log line instead of a silent omission. The new patch locator should
support `va` (old behaviour, exact position) *and* a bounded byte-pattern
search with an original-byte check plus uniqueness requirement; both are
recorded in the applied-patch report.

### 2.3 Device tree

- **[V]** `kernel/dtgen.py` starts from the n72ap template extracted from the
  *same* IPSW, fills iBoot's slots, deletes S5L8720 nodes, maps fake register
  pages and sets CPU frequencies. It requires particular nodes/properties and
  a fixed number of `MemoryMapReserved-*` slots.
- **[G]** The n72ap template changed between releases (nodes added/removed by
  newer iOS). 4.0–4.2.1 should be close. 2.x/3.x may lack properties this
  generator insists on (`pram`, `vram`, `clcd`, audio nodes) or have different
  sizes; `_set_same_size()` deliberately refuses to change property sizes, so
  the failure will be explicit. The generator needs per-version node overrides
  and a fallback for missing-but-optional nodes before those versions can boot.

### 2.4 launchd / SpringBoard

- **[V]** 4.2.1 userland work is in `rootfs/recipe.json`: ~28 daemons disabled,
  liblockdown and AppleKeyStore stubbed by byte patches in the dyld cache,
  `DYLD_INSERT_LIBRARIES` for configd/securityd/SpringBoard, camera plists,
  trust store, Cydia/TLSFix, etc.
- **[G]** 2.x/3.x: no App Store in 2.x, no Game Center/Camera/Gyro before the
  relevant releases, no `MobileKeyBag` in 2.x, different launchd plist names
  and activation paths, no Substrate-compatible Cydia packages for those
  versions. Our `ios3ds-applog`, `ios3ds-a80211`, `ios3ds-input`, `ios3ds-mcud`
  interposers should be built per-version against that rootfs; the 4.2.1 stub
  SDK in `scripts/ios-sdk-symbols.json` is not proof that an API existed in
  iOS 3.x. Expect a reduced feature set first (no Cydia/TLSFix, fewer hooks).
- **[G]** 4.0/4.1/4.2: closest to current; launchd, SpringBoard and the
  dyld-cache layout largely match 8C148, but *all* file-patch offsets in
  `rootfs/recipe.json` are build-specific and must be relocated by original
  bytes, and the hardcoded strings "4.2.1"/"8C148" in the liblockdown table
  must become version variables.

### 2.5 dyld shared cache

- **[V]** 8C148 has `/System/Library/Caches/com.apple.dyld/dyld_shared_cache_armv6`;
  `mkrootfs.py` patches it at fixed offsets and creates
  `enable-dylibs-to-override-cache` so our `MBXGLEngine` can replace the cached
  one. Cache *absence* is already handled in one place: 3DS-specific binaries
  are installed as separate files, so only the override/patch entries depend
  on the cache.
- **[G]** The shared cache arrived with iPhone OS 3.x (2.x ships individual
  framework dylibs). On such a rootfs, skip all dyld-cache patches and instead
  patch/replace individual `liblockdownd.dylib`, `OpenGLES.framework`, etc.,
  or accept that activation/display workarounds need another mechanism. The
  `file_patches` step needs pattern-based location for 3.x/4.x too.

### 2.6 Our userland tools' SDK/ABI assumptions

**[V]** The tools are ARMv6 Mach-O built with the local iOS stub SDK
(`scripts/build-ios-toolchain.sh`, `scripts/ios-cc`, `scripts/ios-sdk-symbols.json`)
and use private APIs (IOSurface, IOKit, CoreMotion, Celestial, Substrate).
They are linked against 4.2.1's exported symbol set, not against each target
OS. **[G]** Within Darwin 10 (4.x) most of that ABI is stable, so the same
binaries should load, but some private selectors/classes moved; on Darwin 9
(3.x) and 2.x many are absent. Plan: a version-tagged subset of tools per
release, built with a matching stub-symbol list; missing-symbol checks at
install time rather than runtime crashes.

### 2.7 Rootfs recipe

**[V]** `mkrootfs.build()` already accepts `recipe_path`; a version can provide
a full or merged recipe. The default rootfs is grown to 1 GiB, `/etc/fstab`
points at `/dev/md0`, and Cydia/TLSFix/trust-store additions are optional.
**[G]** Older rootfs images are smaller (2.x: ~100–200 MB), so growth and the
RAMDisk size in the device tree must be computed from the actual image (the
code already does this). A version recipe should be a small overlay on the
base recipe: which daemons exist, which dyld/file patches to apply, which
binaries to install, which plist keys to set. It must never rewrite the 4.2.1
recipe.

## 3. Version plumbing design (stage 2)

1. New `scripts/versions.py`: table of `version -> {build, ipsw, ipsw_sha1,
   patch_files, disabled_patches, recipe, boot_cfg overrides, notes}`.
   Default entry is 4.2.1 and contains **no overrides**: existing code paths
   and file names stay exactly as they are.
2. `Makefile`: `IOS ?= 4.2.1`; non-default versions stage firmware under
   `build/fw-$(IOS)/staging/` and generate their recipe/patch list there;
   `--ios` is passed to `mksd.py`/`fwprep.py`. Default 4.2.1 keeps
   `build/staging` and the same command lines.
3. `kernel/patcher.py`: accept an optional list of recipe files and a bounded
   pattern locator; a recipe with `va` behaves exactly as today. Pattern
   patches require a unique hit and retain the original-byte check.
4. `scripts/mkrootfs.py`: support `offset` **or** `search` in `file_patches`
   (unique hit, original bytes must match); version recipes are generated by
   merging overlays into the base recipe under `build/fw-VERSION/`.
5. `scripts/mksd.py` / `scripts/mkboot.py`: pass the version through; the
   boot-args epoch (`ba_version=5` today) becomes a version setting. 4.2.1's
   generated `boot.cfg`, `kernel.bin`, `devtree.bin`, `fakeio.bin`, `shim.bin`
   and `rootfs.hfs` must hash the same before and after (verified by comparing
   outputs).
6. `scripts/install.py`: accept `--ios VERSION` (default 4.2.1), select the
   matching recipe and refuse unknown versions; the release's prebuilt
   shim/payload stay version-independent as long as the shim ABI is unchanged.

## 3.1 Stage 2 implementation (verified)

- **[V]** `make sd IOS=4.2.1` / `make sdimg IOS=4.2.1` (the default) now pass
  `--ios 4.2.1 --staging build/staging` to `mksd.py`. The version registry
  (`scripts/versions.py`) returns **no overrides** for 4.2.1: the patch list is
  the original `kernel/patches*.json` glob, the recipe is the original
  `rootfs/recipe.json`, `ba_version=5`, and the staging directory is unchanged.
- **[V]** `make fwprep IPSW=… IOS=4.1` extracts into the new
  `build/fw-4.1/staging/`; `scripts/fwprep.py --ios` checks the published IPSW
  sha1 and build number for that version (4.1's sha1 matched the downloaded
  Apple file). `scripts/install.py --ios VERSION` selects the same recipe.
- **[V]** 4.1 has its own patch set (`kernel/patchsets/4.1.json`), located by
  context/unique-byte search against the 8B117 kernelcache and checked by
  `kernel/patcher.py` before each write. `kernel/patcher.py` also supports a
  generic bounded `find` locator for future versions; fixed-`va` recipes are
  untouched. `mkrootfs.py` gained a `search` mode for `file_patches`; the
  4.1 recipe is generated by merging `rootfs/overlay-4.1.json` into the base
  recipe under `build/fw-4.1/` (never editing the base recipe).
- **[V]** `make test` passes with the 7 new offline tests (registry, overlay
  merge, fixed and pattern patch location, file-patch search).
- **[V]** The 4.2.1 firmware, patch and recipe paths produced identical bytes
  before and after the change; hashes are recorded in section 5. The FIRM's
  embedded version string changes with the git commit count, as it always has.
- **[V]** `emu/run.py` and `emu/viewer.py` honor `IOS3DS_QEMU=/path/to/qemu-system-arm`
  so a QEMU binary from a local build directory can be used without writing to
  the shared `build/qemu` symlink; the default path is unchanged.

## 4. iOS 1.x: trap-and-emulate vs. our patched-kernel approach

### 4.1 What the reference project does **[V]**

`roastme/ios3ds` (GPL-2.0-or-later, <https://github.com/roastme/ios3ds>;
`COPYING`, `README.md`, `notes/hw-inventory.md`, `notes/hd2-xnu-review.md`,
`shim/dabt.c`, `shim/start.S` read via raw.githubusercontent.com) boots Apple's
unmodified iPhone OS 1.1 (3A101a) kernelcache on a **New 3DS**:

- Its loader hands the stock kernel a boot_args record; a small shim replaces
  the data-abort vector. Every S5L8900 MMIO access from XNU faults; the shim
  decodes the faulting ARM load/store (including shifted offsets and
  LDRH/STRH/LDRSB/LDRSH), walks TTBR0 to recover the physical address, models
  timer (+0x80/+0x84 timebase, +0xA4/+0xA8 decrementer), UART TX, and shadows
  the rest, then resumes after the instruction.
- It maps the MPCore private timer into the kernel's L1 table as a fast
  timebase, uses the same "log to SD" idea, and documents hot paths: the idle
  hook touches clock/power registers ~10 times per idle cycle (candidate:
  patch `_processorIdle` to `bx lr`), the timebase is read 3× per
  `mach_absolute_time`, and cache set/way loops assume a 16 KB D-cache on the
  ARM1176 but the MPCore has 32 KB.
- Its XNU kernel maps devices at 0xE0000000+ and uses a linear map
  `VA = PA + 0xA0000000`, and its virtual kernel base is 0xC0000000 — our
  4.2.1 layout uses `virtBase=0x80000000`. That alone makes the two loaders
  different.
- Status by its own README: "created almost entirely by AI and was not tested
  extensively"; it uses qemu-ios and 3Beans harnesses, not our 3DS FIRM.

Licence: GPL-2.0-*or-later*, so ideas/source can be reused in this GPL-3.0-
or-later project (GPL-3 is a later version), with attribution. **This stage
copies no code from it**; if a later stage ports an algorithm (the abort
decoder, MMIO inventory, register maps), add a credit to `README.md` §Credits
and to the relevant file header, and keep its notices intact.

### 4.2 Comparison

| | ios3ds today (4.2.1, S5L8720) | roastme (1.1, S5L8900) | What 1.x would need here |
|---|---|---|---|
| Kernel | Apple's cache, ~28 binary patches | Apple's cache, **unmodified** | either route can work; ours needs a new patch set for the 1.1 cache, theirs needs a full abort decoder |
| MMIO | generated DT points drivers at fake pages in AXI WRAM; shim implements only the pieces we need (timer, VIC, CLCD, audio…) | **all** S5L8900 MMIO traps; shim decodes/resumes each access | our DT is n72ap-shaped; 1.1's template is n45ap and the device set is different (ADM/FTL NAND, AppleH1CLCD, Zephyr touch) |
| CPU | MPCore patches (PPMR, IFAR, ACTLR) | same core family, plus cache-geometry patch and idle-hook patch | New 3DS ARM11 is ARMv6K (no caches modelled in our QEMU); on Old 3DS the older kernel's cache loops must be checked |
| Boot layout | virtBase 0x80000000, boot_args version 5, kernel at 0x20001000 | virtBase 0xC0000000, linear map +0xA0000000, own loader | new ARM11 entry path, new memory map and DT generator |
| Rootfs | HFS+ served over md0 from ARM9; 1 GB grown image | NAND/FTL image in qemu-ios/3Beans | reuse ARM9 block server if the 1.1 kernel can use a RAMDisk/md0; otherwise implement FTL-style NAND on the ARM9 |
| Userland | 4.2.1 binaries, dyld cache patches | 1.1 SpringBoard, no dyld cache | all our tools must be rebuilt/gated for 1.1; no Cydia/TLSFix |

### 4.3 Realistic plan and effort (all numbers are **[G]**)

- **Separate backend, shared ARM9 monitor (recommended).** Keep our ARM9
  payload, SD/log/mailbox protocol, FIRM packaging and QEMU machine; add an
  alternative ARM11 path with the 1.1 loader, trap decoder and S5L8900 device
  models. The 3DS GPU path, Wi-Fi, cameras, MCU and iOS userland hooks are all
  out of scope for a first "home screen or panic log" milestone. Rough order:
  1. port the loader's boot_args/DT hand-off and get XNU's banner (2–4 days);
  2. trap decoder + timer/VIC/UART models with a host test, then early boot in
     our QEMU (5–10 days);
  3. NAND/rootfs delivery instead of qemu-ios's NAND model (5–10 days);
  4. CLCD + touch + SpringBoard bring-up (10–20 days);
  5. 3DS hardware validation and stability (5–10 days).
  **Total: ~1–2 person-months to a first 1.1 home screen**, more if the 1.1
  kernel needs the NAND/FTL path rather than our md0 block server.
- **Extend the existing shared shim instead.** Tempting because both projects
  use the same ARM9/ARM11 split, but the shim is linked at a fixed 4.2.1 VA,
  patched into 8C148 instructions at fixed addresses, assumes S5L8720 fake
  pages and 4.2.1 `cpu_data` hook offsets, and expects boot_args version 5.
  Supporting 1.1 would mean conditional DT generation, two patch sets, two
  shim layouts and two kernel entry paths in one binary. **Estimated 1.5–3×
  the separate-backend effort**, with much higher regression risk to 4.2.1.
  Only worth it if both backends end up sharing most device models.
- **Not recommended:** generic trap-and-emulate for *all* versions. The
  per-access cost is high (roastme's MMIO inventory shows tens of millions of
  timer reads per boot), and our 4.2.1 path already runs natively with far
  fewer patches. Keep trap-and-emulate as the 1.x backend's core idea, not as
  a replacement for the current DT + fake-page design.
- QEMU work: our `nintendo3ds` machine models the 3DS hardware, not the
  S5L8900; the trap decoder can be tested with a host harness (as the reference
  project does with `tools/hosttest/dabt_test.c`), but a 1.x guest needs an
  S5L8900-aware loader/boot path in QEMU before any visual result.

## 5. Verification and stage-3 blocker log

### 5.1 4.2.1 byte-for-byte check

`make userland` then `rm -f build/sd.img && make sdimg` (default, 4.2.1) was run
before the version plumbing landed; sha256 of the generated files:

| File | sha256 |
|---|---|
| `kernel.bin` | `505f451a44d9da6aae7034d427d5524d7392d9fc70640ec997c374b0b5a70e38` |
| `devtree.bin` | `25b1bd233447ddfc60eb69e30ad4cbd085c1963f86e77052fda969469d8327cc` |
| `fakeio.bin` | `e5fecd6645b3bc44d6510d7af7397c667fdc25da4517e2cd9fabc42fb673570d` |
| `shim.bin` | `78c2bae0156695765fb7c77cbc560e8bb5995f9cf35a18d907ad937d4e17c170` |
| `gpu.bin` | `9b0f1cc31f66144514f41b6c357d98e2166b8717b885e7ee152946a73dbe2a92` |
| `boot.cfg` | `e52fdeb118e0ad3d5f287b2096beb46482ee5a6be8eb24469ae308b5ffb32479` |
| `rootfs.hfs` | `7b63a5de25ba8d2e572476d83d6644a821f450a49d038f461854e4dbb97e2faf` |
| `sd.img` | `7776bb093a47f2f7184340359a5cb4db4bea339b5b208e8374b04bf0ccf8ce17` |

**[V]** After the version plumbing landed, a fresh 4.2.1 build (`make userland`,
`rm -f build/sd.img && make sdimg`, isolated output tree so other workers'
builds could not race it) produced exactly the six hashes above for
`kernel.bin`, `devtree.bin`, `fakeio.bin`, `shim.bin`, `gpu.bin` and `boot.cfg`.
For a stricter before/after test, the same six files were generated twice into
separate trees — once from `origin/main` (the old scripts) and once from this
branch — with the same staged IPSW, HFS+ tool, prebuilt shim, .deb cache and
`SOURCE_DATE_EPOCH=1763800000`. `cmp` found all six byte-identical; `boot.cfg`
differs in no line.

`rootfs.hfs` (and therefore `sd.img`) is **not** byte-reproducible even with a
fixed epoch: `scripts/music.py` puts a build timestamp in the generated Music
library and xpwn's `hfsplus` stamps the *current* mtime on every file it adds
(`hfsplus ls` shows the same file with a different date in the two images).
The fixed-epoch images hash `9e29cb82…` (old) and `2d4ca4ac…` (new); the
patched dyld cache, our iOS binaries, the modified plists and `/etc/fstab` all
have identical sha256 when read back from both images, so only HFS+ metadata
differs. Nothing about the 4.2.1 recipe or its generated boot files changed.
The generated device tree was additionally checked byte-for-byte with the old
and new `dt_audio.py` (§5.2). The FIRM embeds `git describe` and therefore
changes with each commit, as it always has.

### 5.2 4.1 (8B117) bring-up in QEMU

Firmware: Apple's `iPod2,1_4.1_8B117_Restore.ipsw` (348,027,174 bytes,
sha1 `97abde6207660bd876fd476275dd526d0dcf3d19`, matching api.ipsw.me),
downloaded into `build/fw-4.1/` and extracted there with `scripts/fwprep.py`.
No Apple file is committed.

Blockers found and resolved:

1. **3.1.3 IPSW unavailable.** **[V]** api.ipsw.me publishes keys for 7E18
   but neither ipsw.me nor IPSW.io lists an iPod2,1 3.1.3 download any more;
   third-party archives require a captcha and could not be verified. Per the
   task, stage 3 fell back to 4.1 (Apple CDN URL still live, sha1 verified).
2. **Device-tree phandles.** **[V]** `dtgen.generate()` on 8B117's n72ap
   template failed with `dt_audio: /arm-io/gpio is not phandle 0x90c9a0`:
   Apple reassigns phandles in every build (4.1 has 0x9040c0 / 0x90ba80 for
   gpio/vic). `tools/ios3ds-audio/dt_audio.py` now reads the phandle words
   from the template. The generated 4.2.1 device tree is byte-identical
   (verified by generating it with the old and new module: both
   sha256 `8cfaa50b4649e55b1420653e87385f1c19641dadfa435250622e5ba3d051e933`).
3. **Kernel patch addresses.** **[V]** 31 of 37 8C148 patches were relocated
   into 8B117 (`kernel/patchsets/4.1.json`) by fixed address, unique context
   around the instruction, or a nearby unique function anchor; every original
   byte sequence was checked in the target. Six were omitted: gyro usage and
   dispatch (driver changed), CLCD backlight log-enable and retry-off
   (cosmetic/wake), the clock controller's voltage-lookup literal and the
   IORTC wait literal (moved function addresses; the 2 s fallback patch is
   kept).
4. **dyld-cache patches.** **[V]** Plain byte searches match thousands of
   times in a 150 MB cache, so the 8C148 file patches were relocated using
   the cache's image/symbol tables: 8C148 patch file offset -> VA -> image
   symbol + offset -> 8B117 symbol VA -> file offset, with the original bytes
   verified at the result. 18 of 24 patches relocated; six were omitted
   because the code changed (`lockdown_copy_buid_identity`,
   `lockdown_copy_value`'s lookup body and its string table, the VirtualAudio
   routing table, CoreMotion's capability mask and MusicLibrary's signing
   check). The liblockdown literal strings in the `lockdown_copy_value` table
   were rewritten to "4.1"/"8B117" in the overlay where that patch is used.
5. **Cydia/TLSFix.** **[V]** The downloaded 4.2.1 packages are not used for
   4.1; the overlay disables them. All userland binaries still come from
   `make userland` and are ABI-tested only on 4.2.1.

### 5.3 Boot result (4.1, QEMU `nintendo3ds`, `--icount 2`)

**[V]** `make sdimg IOS=4.1` succeeded (kernel sha256
`e26135779d1b2f88aa912cd6afdea38dfc3eb6daad2bd780179b21b8210dfdc8`, devtree
`29ebba25ac24b6fe7fcf2f3ba9597f96ab4c2368c4089f9771cf8b58e665f002`, boot.cfg
`6189ee30b59fd596bd18025422f732d1c09efd3de12e75bd736731164571c506`, rootfs
`b91c758519c52bde7a47be8404c147acfc4c32ab8d2043e20f920a66ecedca71`).

With the **full 4.1 patch set**, XNU starts, prints its banner, brings up the
S5L8720 drivers (VIC, GPIO, power, clock controller, DMAC, I2C, timer, watchdog,
I2S, CLCD) and then panics in `OSDictionary::setOptions()+0x44`, fault address
`0x4`, before `launchd`. The CLCD's benign "Couldn't allocate AppleARMFunction
for function-lcd_enable" line also appears in working 4.2.1 boots, so it is not
the cause.

A control run with only the 19 CPU/timer/IRQ/UART/console patches (no kext or
md0 patches) got **much further**: `BSD root: md0`, launchd's filesystem
compression kext, screen copying and 100 s of heartbeat logs, but no home
screen (md0 strategy is still unpatched, so the root disk is not really read).
Adding the relocated `mdevstrategy` patch reaches `BSD root: md0` and then
takes an undefined-instruction exception at `pc 0xc38b3e94`, inside the fake
GPIO register page — a 4.1-specific interrupt/function-pointer path we have not
untangled. No 4.1 home screen yet. See `build/emu-4.1/last-4.1-log.txt` for the
full panic dump (build/ artifacts are not committed).

Remaining blockers found in this run, all **[V]** unless marked (items 1–2
were diagnosed and fixed in §5.5 — they were shim KPI addresses, not kernel or
device-tree bugs):

1. The optional 4.1 kernel patches need per-subsystem validation; at least one
   of the six relocated CLCD/audio/I2C patches causes the early
   `OSDictionary::setOptions` panic.
2. The md0/`mdevstrategy` path needs 4.1-specific work: the relocated patch
   gets the disk mounted but a later interrupt lands in a fake IO page.
3. `lockdown_copy_value` and its activation table, VirtualAudio routing,
   CoreMotion's capability mask and MusicLibrary's signing check are not
   patched on 4.1 (§5.2). Expect activation/UI problems even once 1–2 are fixed.
4. The 4.2.1 userland binaries are used unmodified on 4.1; none of the private
   API/selection behaviour has been validated on this firmware.

**[G]** Fixing 1–2 is a few days of disassembly and QEMU bisecting; fixing 3 is
another few days; a usable 4.1 home screen is therefore plausibly ~1–2 weeks
of work, not a one-line change.

### 5.4 Final default-build soak

**[V]** Four fresh-image soaks of the unmodified default 4.2.1 build were run
after the version work (`python3 emu/soak.py --cycles 5 --icount 2 --sd
build/sd-soak{,2,3,4}.img`; isolated images were needed because several other
workers were rebuilding `build/sd.img` concurrently). The last one also used
the soak script's new re-acquire-a-lit-home-reference logic. All four printed
`**Result: FAIL**`, but the only failing check was "not home after Home" —
screenshots stayed black/stale while boot-to-`SpringBoard` took 435–965 s
(normally ~100 s on an idle host) because many QEMU/build jobs from other
worktrees were sharing the CPU (the last run's own heartbeat log only reached
`t=30s` after 950 s of wall time). There were **0** kernel panics, ARM11 stalls,
md0/log errors, process crashes, watchdog/hang events or GPU mirror failures.
We could not obtain a PASS while the host was this loaded; this is a host
contention problem, not a change in the generated 4.2.1 boot files (§5.1).
The logs are in `build/soak-run{,2,3,4}.log` (not committed); some report
directories were removed by other workers' cleanups.

**[V]** The exact required sequence
(`rm -f build/sd.img && make sdimg && python3 emu/soak.py --cycles 5 --icount 2`)
was retried four further times with fresh images. `emu/soak.py` was also fixed
to re-acquire a lit home-screen reference when the first 180 s of screenshots
are black (a normal host never hits this). Boot still took 425–554 s and every
run printed `**Result: FAIL**` on "not home after Home" only — the screenshots
caught page animations or mis-tapped icons while other worktrees' QEMU/ninja
jobs kept the 12-core host at load 30–40. All eight failure checks (panic,
stall, md0, log, crash, heartbeat, watchdog, GPU) stayed at 0. An experiment
that pressed Home before each tap made the icon hits *worse* and was reverted.
This is a harness/host-timing limitation, not a 4.2.1 payload change (§5.1).
Logs: `build/resume-soak{,2,3,4}.log` and `build/emu/soak/report.md`
(not committed).

**[V]** Once the other workers' builds finished, the exact required sequence
(`rm -f build/sd.img && make sdimg && python3 emu/soak.py --cycles 5 --icount 2`)
was run again on a fresh image and printed **`Result: PASS`**: 5 cycles, no
"not home after Home", boot to SpringBoard in 197 s, zero panics/stalls/md0,
log, crash, heartbeat, watchdog or GPU failures. The final report is in
`build/emu/soak/report.md` (not committed); the earlier failures were host
contention, not a regression.

### 5.5 Stage 4: 4.1 reaches SpringBoard — recipe and shim relocation (2026-11-23)

This section continues §5.3. The firmware was fetched again into
`build/fw-4.1/` through the same API/CDN path (`sha1
97abde6207660bd876fd476275dd526d0dcf3d19`, verified by `fwprep.py`), so every
Apple file stays under `build/`.

**Blocker 5: the rootfs overlay's positional file-patch list went stale.**
**[V]** The first 4.1 rootfs build stopped at
`file patch .../dyld_shared_cache_armv6 @0x4fbd98: original bytes differ`.
`rootfs/recipe.json` had gained two MBX2D string patches on `main` after the
4.1 overlay was written; `versions.py` zipped the overlay's 24 offsets onto the
new 26-patch list, so every patch was applied at the wrong offset. It also
failed later with `21132 matches for original bytes` when the
`file_patches_search` fallback was used — a plain byte search is not unique in
a 150 MB cache. Fixed by keying the relocation map on the **base recipe's
8C148 offset** (`file_patch_offsets_by_base` in `rootfs/overlay-4.1.json`), so
inserting patches into the base recipe can no longer shift it. The two new MBX
strings have unique hits in 8B117 (`0x492c7cc`, `0xa899a4`) and their original
bytes were verified. All 20 applicable 4.1 file patches now apply. An unknown
base patch is skipped with a loud `[versions] no 4.1 offset ...` line.

**Blocker 6: the shim called 4.2.1 KPI addresses.** **[V]** This was the actual
cause of both panics in §5.3, not the fake GPIO pages. `kernel/shim/blk.c`
called `buf_count`, `buf_map`, `lck_mtx_lock`, … at hard-coded 8C148 addresses;
in 8B117 they all moved (e.g. `_buf_count` 0x800781c0 → 0x800781a0,
`_lck_mtx_lock` 0x80063000 → 0x80062cc0). The same was true of the sysctl and
IOKit calls in the input/Wi-Fi/MCU shims (`tools/ios3ds-*/shim/*.c`) and of
`gIOPlatform`. Calls into arbitrary kernel functions corrupted a stack in
FCRAM; QEMU's monitor showed the "fake GPIO page" PC `0xc38b3f3c` translating to
FCRAM `0x20adef3c`, i.e. a wild stack, not device memory. With the old shim the
early MCU hook (`PEGetGMTTimeOfDay`) also jumped into a wrong function and
panicked in `OSDictionary::setOptions()+0x44` (far 0x4), before `launchd`.

Fix: `scripts/kpi.py` resolves every KPI address by name from the staged
kernelcache's `LC_SYMTAB` (C++ names included), writes an `ios3ds-kpi.h` into a
temporary build directory, and `mkboot.build_shim()` passes it to the shim
compiler. `kernel/shim/kpi.h` keeps the old 8C148 constants as defaults, and
all shim sources now use those macros. `gIOPlatform` has no symbol; its 8B117
address `0x8029c49c` comes from the PEGetGMTTimeOfDay literal pool (0x801c7a94)
and is recorded in `kpi.py`. No shim layout/ABI changed.

**Blocker 7: SpringBoard crash loop from the skipped `lockdown_copy_value`
patch.** **[V]** A 900 s QEMU run with the relocated shim reached launchd and
SpringBoard but stayed on the spinner; 58 GL contexts were created and every
SpringBoard restarted after ~6 s. Crash reports in the image
(`LatestCrash-SpringBoard.plist`) showed `EXC_BAD_ACCESS (SIGBUS)` at
`liblockdown.dylib+0x2dfc` (0x304fbdfc), i.e. the unpatched
`lockdown_copy_value` (offset 0x4fbde4 in the 8B117 dyld cache). Its body and
table changed too much for the §5.2 symbol relocation, so the 4.1 overlay now
carries a complete replacement: the verified 44-byte original at 0x4fbde4 is
replaced by the 8C148 table-lookup stub (literal table VA 0x304f9650), and the
160-byte table is written into a verified zero gap of liblockdown. The keys are
the 8B117 addresses of the eight CFStringRef arrays (found by matching each
CFString payload and its array), the values are 8B117's `Activated`,
`kCFBooleanFalse` (0x3ebc182c) and `iPod` literals plus inline
`iPod2,1`/`4.1`/`8B117`/`N72AP` CFStrings with 8B117's `__CFString` isa
(0x3ebc28ac). Both original byte sequences are checked by `mkrootfs.py`.

**4.2.1 stays unchanged.** **[V]** A fresh default build (`make userland`,
`rm -f build/sd.img && make sdimg`) produced `kernel.bin`, `devtree.bin`,
`fakeio.bin`, `shim.bin`, `gpu.bin`, `boot.cfg` with the same sha256 as an
independent build from `origin/main` sources (`git archive HEAD`) using the
same staged firmware and shim. In particular the 4.2.1 shim hash stayed
`7906b596…` before and after the KPI change.

**Boot evidence after the fix.** **[V]** QEMU `nintendo3ds`, `--icount 2`,
4.1 patch set + relocated shim: XNU reaches `BSD root: md0`, starts launchd and
SpringBoard, loads the relocated dyld-cache patches (`[gl]` contexts on the
PICA200, `CA compositor: OpenGL ES on the PICA200`), serves md0 from the ARM9
(thousands of requests, 0 errors) and keeps the heartbeat/GPU logs running. No
`OSDictionary::setOptions` panic, no undefined-instruction exception. A control
run with only the 15 core + md0 kernel patches also reaches `BSD root: md0` and
SpringBoard with the relocated shim, confirming that the shim addresses, not a
kernel patch, were the blockers.

**Final 4.1 boot.** **[V]** `make sd IOS=4.1` plus `make sdimg IOS=4.1`, then
`IOS3DS_QEMU=/tmp/opencode/qemu-compat2/b/qemu-system-arm python3 emu/run.py
--sd build/sd.img --firm build/sd/ios3ds/ios3ds.firm --icount 2 --time 600
--out build/fw-4.1/run-final41b`: the screenshot at t=120 s (and at t=180 s)
shows the iPod touch home screen with the 4.1 stock icons (Contacts, Weather,
App Store, iPod, Settings, Mail, Music, Videos, Safari, Game Center, Phone,
Calendar, Notes, Camera...). `sd:/ios3ds/log.txt`: `BSD root: md0`, `launchd`
startup, `[mcud]`, `[gl]` CA compositor on the PICA200, heartbeat at t=60 s
(md0 3745 req / 79080 KiB / 0 err), **1** GL context (no restart loop) and
**0** fatal exceptions. Screenshot: `build/fw-4.1/run-final41b/screen_120.0s.png`
(build/ artifacts are not committed).

Remaining known limits: Cydia/TLSFix are disabled for 4.1 (their 4.2.1 packages
are in the overlay's skip list); six dyld-cache patches and six 4.1 kernel
patches from §5.2/§5.3 are still skipped; the 4.2.1 userland binaries and our
private-API hooks are only ABI-tested on 4.2.1.

## 6. Older iPod2,1 firmwares: 3.1.3 (7E18) and 2.2.1 (5H11a)

Continues §5.5. Everything in this section was built and booted in QEMU from
Apple files under `build/fw-3.1.3/` and `build/fw-2.2.1/`; no Apple files are
committed. **[V]** marks a verified build/boot/byte check, **[G]** a reasoned
conclusion that still needs more work.

### 6.1 Firmware source and prep

- **[V]** 7E18 is no longer served by the Apple CDN links in ipsw.me's public
  list (`403 Forbidden`). The archived Apple IPSW in
  `archive.org/details/ipod-touch-2g-ipsw` (263,275,211 bytes, sha1
  `5f4f5c01eda2f811f73167e7d1f82dbeed82367b`) was downloaded to
  `build/fw-3.1.3/`. It identifies itself as `iPod2,1 / 3.1.3 / 7E18`, and the
  *public* 7E18 keys decrypt its kernelcache, device tree and HFSX rootfs
  (440 MiB) with valid complzss/adler32, device-tree round-trip and HFS+
  signature. That is strong evidence the archive copy is the genuine restore
  file, but it is not an Apple-served sha1: the archive hash is pinned in
  `versions.py` so builds are reproducible.
- **[V]** 5H11a still comes from Apple's CDN URL published by ipsw.me; its
  sha1 `9af5625ea34acdd8abeb6fce71a72651d0c815d5` matches the API. Its keys
  decrypt the kernelcache (Darwin 9.4.1), n72ap device tree and 476 MiB HFSX
  rootfs.
- **[V]** `scripts/fwprep.py` needed two older-firmware fixes: 2.x IPSWs have
  `Restore.plist` instead of `BuildManifest.plist`, and the published key lists
  name e.g. `DeviceTree.n72ap.img3` while the IPSW stores it under
  `Firmware/all_flash/all_flash.n72ap.production/`. Key lookup now tries the
  exact path first, then a unique basename; the 4.2.1 path is unchanged.

### 6.2 Boot_args epoch and the 0xC0000000 kernel layout

- **[V]** Both caches are linked at `virtBase=0xC0000000` (3.1.3 entry
  `0xc0069040`, 2.2.1 entry `0xc00657f0`; `__TEXT` starts at
  `0xc0008000`). 4.x uses `0x80000000`. `versions.virt_base()` now feeds the
  value into `mkboot.layout()`, `dtgen.generate()`, the shim's jump-table
  symbols, `boot.cfg` and the generated KPI header.
- **[V]** The shim is still loaded at physical `0x20840000`, but for these
  firmwares it is linked at `0xC0840000` (GPU part `0xC08B0000`): `mkboot`
  generates a temporary linker script and passes `IOS3DS_SHIM_VA`,
  `IOS3DS_GPU_PART_VA` and `IOS3DS_GM_BLOB_VA` overrides. Boot/common headers
  keep the 4.2.1 defaults, so the default shim bytes do not change.
- **[V]** The boot_args epoch was read from the kernels, not guessed: in
  Thumb `_pe_identify_machine` 7E18 has `ldrh r3,[r0,#2]; cmp r3,#5` at
  `0xc01a7f22`, 5H11a has `cmp r3,#3` at `0xc017ab94`. The registry therefore
  sets `ba_version=5` for 3.1.3 and `ba_version=3` for 2.2.1 (4.x stays 5).
- **[V]** `mkboot.flat_kernel()` now ignores segments with `vmsize > 0` but
  `filesize == 0`: 2.2.1's `__PAGEZERO` at VA 0 otherwise made a 3.2 GiB flat
  image with a zero base. 4.2.1 has no such segment; its image and hashes are
  unchanged.
- **[V]** `dtgen.generate()` is lenient for nodes that only exist in newer
  templates (7E18 lacks `usb-complex`/`backlight`, 5H11a also lacks `xmc`);
  fields we actually depend on remain strict. 5H11a additionally deletes
  `/arm-io/usb-otg` via `versions.py:dt_delete` after its USB kext panicked.
- **[V]** `kernel/shim/entry.S` took `VIRT_BASE` and the IRQ continuation from
  hard-coded 8C148 constants. They now come from the generated KPI header
  (`0xC0000000` / `0xc0068458` for 7E18, `0xC0000000` / `0xc006540c` for
  5H11a). The first boot attempt panicked at shim+0x310c with
  `far=0x808c8020`: `audio.c`, `cam.c` and `pica.c` had their own hardcoded
  `0x80000000` VA for PA->VA and L2 maintenance. They now use the generated
  `IOS3DS_KPI_VIRT_BASE`; 4.2.1 expands to the same constants.

### 6.3 KPI resolution and fallbacks

**[V]** `scripts/kpi.py` resolves every shim call by `LC_SYMTAB` name; all 59
symbols used by the shim are present in both old kernelcaches (no symbol
fallback was needed for 7E18 or 5H11a). The data symbol `gIOPlatform` has no
LC_SYMTAB entry; its address is the literal loaded by PEGetGMTTimeOfDay:

| Firmware | PEGetGMTTimeOfDay literal | `gIOPlatform` |
|---|---|---|
| 8C148 | `0x801c84f0` | `0x8029d490` |
| 8B117 | `0x801c7a94` | `0x8029c49c` |
| 7E18 | `0xc01953f8` | `0xc025f53c` |
| 5H11a | `0xc016b4a0` | `0xc02064a0` |

**[G]** If a future kernelcache is stripped of `LC_SYMTAB`, or drops one of
these KPIs, the current fallback is still the 8C148 address and would call a
random function. A stripped cache needs a new locator (vtable/anchor matching)
per symbol, or a version-tagged KPI table with verified addresses; do *not*
silently reuse the defaults. PEGetGMTTimeOfDay, PESetGMTTimeOfDay,
IOKitInitTime's IORTC wait and PEHaltRestart are the critical ones for boot.

### 6.4 Kernel patch sets

**[V]** `kernel/patchsets/3.1.3.json` (24 patches) and
`kernel/patchsets/2.2.1.json` (15 patches) were derived from the 37 8C148
patches by: unique original bytes, surrounding-context search, or a symbol
address plus the 8C148 delta; every original byte sequence is checked at build
time by `kernel/patcher.py`. Examples where offsets moved:

| Patch | 8C148 | 7E18 | 5H11a |
|---|---|---|---|
| `ppmr-read` | `0x80060274` | `0xc0209938` | `0xc01d8e34` |
| `timer-get-abstime` | `0x80068038` | `0xc006a078` | `0xc0066750` |
| `irq-entry` (`_fleh_irq`) | `0x8006648c` | `0xc0068450` | `0xc0065404` |
| `mdevstrategy` | `0x80072bf8` | `0xc0073f94` | `0xc007056c` |

- **[V]** 7E18's `cpu_data` timer offsets are 4 bytes higher than 8C148's
  (`+0x78` instead of `+0x74`), so those three hooks carry target-specific
  original bytes rather than the 8C148 pattern. The IRQ handler's second
  instruction is `mrs sp,spsr` instead of `mrs sp,apsr`; the shim's
  `KERNEL_IRQ_CONT` is set to the instruction after both patched ones.
- **[V]** The 7E18 `mdevstrategy` patch targets `0xc0074020`, not the similar
  `(dev,bp)` helper at `0xc0073f94`: a GDB trace of the root-mount loop showed
  `bdevsw[2]` installed with `0xc0074021` as its strategy entry, while the old
  patch site was never called. `0xc0074020` calls `buf_count`/`buf_biodone`
  exactly like 8C148's `0x80072bf8` and takes `buf_t` in r0. With the patch at
  the real function, 7E18 mounts `/dev/md0 on / (hfs, local, noatime)`, starts
  launchd and mDNSResponder and serves 2,251 md0 requests with 0 errors.
- **[V]** Skipped on 7E18: `uart-baud` (the init stub returns first),
  `clcd-sync-swap`, the three CLCD backlight log/retry patches, the I2S LRCK
  sync patch, the clock-controller voltage and performance-state waits, the
  watchdog PMU wait, `iortc-wait-mcu` (wait literal's meaning changed; the 2 s
  wait is patched), gyro usage/dispatch (driver differs), and Seatbelt
  (disabled by default). None of these is needed to reach BSD root; the CLCD
  ones matter for wake, the clock ones for power management, and gyro for
  CoreMotion. (Later: the performance-state wait is now patched on 7E18 and
  5H11a as `s5l-perf-nowait`, which the Lock/wake path needs; re-log 132.)
- **[V]** Skipped on 5H11a: the same group plus IFAR/ACTLR (Darwin 9's fault
  path differs), all PE/RTC MCU hooks (the functions exist but their Thumb
  prologues and vtable slot indices changed), `iortc-wait-2s` and
  `clcd-backlight-log-set`. Needed for a *complete* port: PE GMT/time/halt
  (time and clean shutdown), IORTC wait (boot latency), and CLCD backlight
  messages (log spam on wake). Not needed for the first boot stage reached here.

### 6.5 Rootfs and userland

- **[V]** `rootfs/overlay-3.1.3.json` and `rootfs/overlay-2.2.1.json` use
  `{"_replace": true}` to drop the whole 4.2.1 `add`, `file_patches`,
  `plist_set`, `chmod` and Cydia sets. Both keep only `/etc/fstab`, syslog and
  the grow step. One large cache file is removed first (7E18's
  DataDetectors cache, 5H11a's stale kernelcache copy): xpwn's `grow` needs
  free allocation-map blocks and both stock images are 100 % full.
- **[V]** 5H11a has no `com.apple.dyld` directory at all: its frameworks are
  separate dylibs, so the 4.x cache patches and the
  `enable-dylibs-to-override-cache` marker are meaningless there. SpringBoard's
  liblockdown calls end in `/var/run/lockdown/localcomm`, which does not exist
  on the 3DS. `rootfs/overlay-2.2.1.json` therefore patches the standalone
  `/usr/lib/liblockdown.dylib` in place: `lockdown_connect` returns a fake
  handle, `lockdown_copy_value` is replaced by an ARM table lookup (file
  offsets `0x2ea8`, `0x2c20`, `0x544`) and the result is re-signed with
  `ldid -Hsha1` because 2.x verifies dylib signatures per page. A 4.2.1-linked
  `DYLD_INSERT_LIBRARIES` interposer was tried first and abandoned: 2.x dyld
  reported `could not load inserted library` (an unsigned variant produced
  `CODESIGNING ... INVALID PAGE` instead). None of this is installed on 4.x.
- **[V]** 7E18 does have a dyld cache (96,117,081 bytes). The 8C148
  `lockdown_copy_value` stub and its key/value table were relocated by symbol
  into 7E18's liblockdown (cache offsets `0x176a008` / `0x1767600`), with all
  original bytes verified at build time; this gives 3.1.3 offline activation
  without a lockdownd.
- **[V]** Our userland binaries are ARMv6 Mach-O linked against 4.2.1's
  exported symbol list (`scripts/ios-sdk-symbols.json`) and use private APIs
  (IOSurface, IOKit, Celestial, CoreMotion, Substrate). They are **not**
  installed on either old build: Darwin 10.0 renamed/moved many of those
  selectors, and Darwin 9 lacks several frameworks entirely. A real port needs
  a per-version stub-symbol list generated from that rootfs, a version-tagged
  subset of tools (applog/input/mcu first, GL/gps/camera later), and install-
  time missing-symbol checks. Cydia and TLSFix stay disabled for both versions.

### 6.6 Boot results and exact blockers

**[V]** Build outputs (sha256, generated with `make userland` + the version's
`mksd.py`):

| File | 3.1.3 | 2.2.1 |
|---|---|---|
| `kernel.bin` | `129e7d02…` | `48678cac…` |
| `devtree.bin` | `73191b10…` | `af606820…` |
| `boot.cfg` | `a3ed4593…` | `e6accdda…` |

- **[V]** 3.1.3, first QEMU run with the relocated shim: `BSD root: md0`,
  AppleFSCompression and AMFI start, then the root mount retries forever
  ("BSD root: md0" every ~1 s) with **0** md0 requests served by the ARM9 and
  no launchd/SpringBoard. Heartbeats, timers and the screen copy stay alive;
  there is no panic, stall or watchdog. The block mailbox at PA `0x20846000`
  showed `magic=BLKM`, `dev_sectors=0x200000`, but `seq/nreq/nerr = 0`.
- **[V]** The cause was **the wrong patch target, not a `buf_t` ABI change**.
  A GDB trace (`emu/gdb/rsp.py`) with a breakpoint at the old patch site never
  hit it, while `mdevopen` was called on every retry; `bdevsw[2]` held
  `0xc0074021`, the real 7E18 strategy at `0xc0074020`. Relocating the patch
  there (same `buf_t`-in-r0 contract as 8C148) mounts the root filesystem:
  `/dev/md0 on / (hfs, local, noatime)`, launchd, fsck, mDNSResponder and the
  display come up; 2,251 md0 requests with 0 errors in the first 100 s. The
  remaining 3.1.3 stop point is userland, not the block server: even with
  offline liblockdown answers (below), the screen stays on the boot spinner
  and SpringBoard writes nothing to the console (§6.5).
- **[V]** 2.2.1, first QEMU run used a stale `sd.img` built before the shim's
  `VIRT_BASE` fix and panicked at shim+0x310c with `far=0x808c8020`, the same
  audio/cam PA->VA bug as 3.1.3's first run.
- **[V]** With the fixed shim, 2.2.1 starts XNU and the S5L8720 drivers (VIC,
  GPIO, power, clock controller, DMAC) but panics at `pc 0xc05fc55c`,
  `fault_addr=0x0` in a prelinked USB/OTG kext immediately after
  `AppleSynopsysOTG2::start` (`IOService::startCandidate` in the backtrace).
  It never reaches BSD root, launchd or any md0 request. A per-version DT
  override now removes `/arm-io/usb-otg` (5H11a's 2.2-era USB stack expects
  nodes/properties the n72ap 4.x template we generate no longer satisfies).
- **[V]** With `/arm-io/usb-otg` deleted, 5H11a gets much further: `BSD root:
  md0`, Jettisoning kernel linker, AMFI, and userland starts
  (`mDNSResponder[11]` seatbelt profiles). The ARM9 serves 1,630 md0 requests
  (32 MiB, **0** errors) and heartbeats/VBlank/screen copy run for the whole
  run, with no panic or stall. It stops there: **no SpringBoard and no UI**
  (screenshots are black; no crash report was written). A 2.x-specific
  liblockdown patch + re-sign (§6.5) was added later: it removes the codesign
  page faults, but SpringBoard still does not start. The remaining blocker is
  elsewhere in 2.x userland, not activation state alone **[G]**.

### 6.7 4.2.1 and 4.1 stay byte-identical

**[V]** Six boot files were generated twice from the same staged firmware:
once with this branch and once from a `git archive origin/main` source tree
(both under `build/verify-*/`, with the copied tree on a space-free path so the
PICA host build works). Every hash matched:

| File | 4.2.1 (branch = main) | 4.1 (branch = main) |
|---|---|---|
| `kernel.bin` | `505f451a44d9da6aae7034d427d5524d7392d9fc70640ec997c374b0b5a70e38` | `e26135779d1b2f88aa912cd6afdea38dfc3eb6daad2bd780179b21b8210dfdc8` |
| `devtree.bin` | `25b1bd233447ddfc60eb69e30ad4cbd085c1963f86e77052fda969469d8327cc` | `29ebba25ac24b6fe7fcf2f3ba9597f96ab4c2368c4089f9771cf8b58e665f002` |
| `fakeio.bin` | `e5fecd6645b3bc44d6510d7af7397c667fdc25da4517e2cd9fabc42fb673570d` | `e5fecd6645b3bc44d6510d7af7397c667fdc25da4517e2cd9fabc42fb673570d` |
| `shim.bin` | `7906b59655035f16d988f6ad6d7cce64d20fd219a9e3734bfa763dc1bded4d4d` | `3ffe091c33a4f34f785d31ca6c6e247b1d0ab61ce4826047b62964d9c740a116` |
| `gpu.bin` | `1b62d38b658e529cb143e4f20663f5747c3f0abf10ab25a6204b4d7e4c68a764` | `cddc7d670eb2b6928921991a85ded4b2c0d12a0f3932aedeb050959f2752a74e` |
| `boot.cfg` | `414fa594325b7dd5b8805380d5df6f5908c6d45254ee8eae910a743d6096314d` | `ee3c9bff4c812a7f15c0c812463cc66f3995500fee728ab4493465b629366893` |

(The `gpu.bin`/`boot.cfg` values differ from the older table in §5.1 because
main gained GPU/boot-option changes after that table was written; both trees
here produce the same bytes.)

### 6.8 4.1 open items (resolved, re-log 131 "iOS 4.1 completeness")

All items below were ported or shown unnecessary; kept for the history.

- **Cydia/TLSFix:** still disabled. The downloaded `.deb` files and Substrate
  tweak are built against 8C148's dyld cache and SpringBoard; making a 8B117
  set needs new packages and a 4.1-specific install recipe, not a flag.
  Needed for HTTPS in Cydia/Safari on 4.1, not for booting.
- **Six skipped kernel patches** (`gyro-usage`, `gyro-dispatch`, CLCD
  backlight log-enable and retry-off, clock voltage lookup, IORTC wait
  literal): gyro only for CoreMotion, CLCD ones cosmetic/wake, the last two
  can add boot/shutdown waits but the MCU shim already supplies time. None is
  needed for the home screen 4.1 already reaches.
- **Six skipped dyld patches:** `lockdown_copy_buid_identity` and
  `lockdown_copy_value`'s lookup body/string table (the activation table was
  replaced wholesale in §5.5, so only the BUID identity lookup is still
  open), VirtualAudio routing, CoreMotion capability mask, MusicLibrary
  signing check. Needed for activation, audio routing, gyro and Music
  respectively; not for the home screen.

### 6.9 Current status and userland/driver matrix (2026-11-25)

| Version | QEMU state | Fresh-image soak |
|---|---|---|
| 4.2.1 (8C148) | home screen, apps, input, Cydia/TLSFix | **PASS** (5 cycles) |
| 4.1 (8B117) | home screen, apps, input, Cydia/TLSFix (APT against the local repo, Cydia opens); all re-log 101/111 open items resolved (re-log 131, iOS 4.1 completeness) | **PASS**, 10 cycles with `--require-app` |
| 3.1.3 (7E18) | **home screen, apps open and render, clock runs, taps and buttons.** DataMigrator skipped (`SBLastSystemVersion`), shim `mrs sp,spsr` IRQ replay, 822-slot GL dispatch, GSEvent input daemon; CoreAnimation composites on the PICA200 with the GPU mirror on (re-log NEXT; was software + ARM9 copy, re-log 128) | **PASS**, 10 cycles with `--require-app` (every tap opened an app) |
| 2.2.1 (5H11a) | **home screen, apps open and render, taps and buttons.** Kernel `cs-invalid-page-allow`, fixed liblockdown stubs, `CA_ENABLE_MBX2D=0`, 2.x tool ABI (classic binding, r9, 48-byte GSEvents) | **PASS**, 10 cycles with `--require-app` |

Driver matrix (QEMU-verified unless marked [G]; "off" = cleanly not installed
or not registered, with the reason):

| Driver | 3.1.3 | 2.2.1 |
|---|---|---|
| Touch + Home/Power/volume buttons | works: `ios3ds-input -DINPUT_IOS3`, GSEvents to PurpleSystemEventPort | works: `-DINPUT_IOS2` (48-byte records), 2.x link |
| Display | works: CA composites on the PICA200, GPU mirror (`gpu_mirror=1`) | works: software CA, ARM9 CPU copy every frame |
| GPU (PICA200) | works: CoreAnimation renders through `MBXGLEngine -DGL_IOS3` (QuartzCore `new_server` asked EAGL for API 2, which 7E18 OpenGLES never gives the MBX engine; patched to API 1), mirror on, `[gl] CA compositor: OpenGL ES on the PICA200`; slower than the software path in QEMU, [G] faster on hardware is expected, not measured; after Home the icon fly-in's last frame often needs a second Home/touch to show | off: 2.x OpenGLES.framework *is* the IMG MBX driver (no engine bundle); a GL engine means replacing the framework (EAGL classes + ~214 GLES/IMGegl exports) with one built on the ios3ds GL core: possible, not done |
| Battery | works: shim IOPMPowerSource, refreshed by mcud polling | works: shim IOPMPowerSource; kernel patch `audio-pm-msg-no-store` (AppleEmbeddedAudio's PM handler zeroed IOPMrootDomain's retain count) and the Darwin 9 vtable bias (`IOS3DS_KPI_VT_BIAS`); level + charging in the status bar and on the lock screen |
| Volume slider | works: mcud `-DMCU_OLD` → AVSystemController | works (same) |
| No-sleep assertion | works: mcud | works: mcud |
| Lid (shell close/open) | works: close = Lock (POWER tap) + LCDs off, open = Home tap + LCDs on (needs the mcud backlight above) | works: same |
| Lock / auto-lock | works: kernel patch `s5l-perf-nowait` (the clock controller's performance-state wait blocked the CLCD wake); SELECT/POWER lock, Home wakes, slide unlocks; SpringBoard's default auto-lock | works: same (5H11a patch at 0xc05e2b40) |
| Brightness | works: mcud `-DMCU_OLD` reads `SBBacklightLevel2` (Settings slider) from mobile's springboard plist, factor 0 while the screen is off; auto-brightness (New 3DS camera ALS) on top | works: same |
| Accelerometer / gyro | accelerometer works (Apple's AppleLIS302DL on the emulated LIS302DL, as 4.2.1): SpringBoard orientation 0→1→4, Safari turns to landscape. Gyro: n/a (no CoreMotion before 4.0) | accelerometer off: 5H11a's AppleLIS302DL never sends an I2C transfer (no probe of reg 0x0f), not traced. Gyro: n/a |
| Audio | works in QEMU: speaker output (UI sounds, `ios3ds-tone`; DMA TC IRQs injected through the version's IRQ path), volume → CSND, mic input via the VirtualAudio N82-slot swap at unity gain (re-log 130); [G] real sound, headphones, keyboard click; test song not shown (4.x library format) | works in QEMU: mediaserverd runs and reaches IOAudio/I2C, `ios3ds-tone` output streams with TC IRQs and a 5999-peak CSND WAV (re-log 134). No mic route: the 0x9a358 product swap breaks category activation and was removed; headset-jack emulation still reports no input. [G] real sound |
| Wi-Fi (`ios3ds-wifid`/a80211) | works: Settings toggle/scan/WPA2 join/DHCP, auto-join from the keychain, status-bar bars, Safari loads pages (first cold-start page fails, later ones load); shim mailbox VA from the KPI base, 3.x event device + 144-byte scan results (re-log 133) [G] | off: no IOUserEthernet in 5H11a and a dynamic Apple80211.framework (Aeropuerto); needs a kernel ifnet (re-log 133) |
| Camera (`vcd`) | n/a: iPod2,1 7E18 has no Camera.app and no MediaCapture plugin (Camera.framework/Celestial capture code exists but has no driver to bind); a 3DS camera here means a new capture stack, not a driver | n/a: same |
| GPS | partial: `ios3ds-applog` hooks work on 3.x (2.0 `CLLocation` initializer, instance `-locationServicesEnabled`): `ios3ds-gpstest` gets 52.52,13.405 at 1 Hz. Stock apps do not load applog yet (SBGuardMalloc does not reach them); no `/bin/sh`, so `ios3ds-gps set` from gpstest fails | partial: same, with 2.x builds (dylib glue `-DDYLD2_DYLIB`) |
| Cydia / TLSFix | Cydia 1.1.30 + APT/dpkg installed and Cydia opens [V]; TLSFix left out (needs Substrate via ios3ds-applog, not installed on 3.x); refresh untested (Wi-Fi works on 3.1.3 since re-log 133, refresh not tried) | off: Cydia 1.1.30/APT binaries use LC_DYLD_INFO_ONLY, which 2.x dyld cannot load (icon appeared, tap launched nothing) |

4.1 (8B117) Wi-Fi: works with the 4.2.1 tools unchanged (wifi_test 20/20,

The 4.2.1 and 4.1 builds are unchanged by all of this: every older-version
switch is either in `rootfs/overlay-*.json`, `kernel/patchsets/*.json`, or a
`versions.py` flag that only non-default versions set (six 4.2.1 hashes
re-checked, re-log 128).
