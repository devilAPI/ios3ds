# Releases and CI

How the release zip is built, what is in it, and why nothing in it is Apple's. For users: [README → Install](../README.md#install).

## Workflows

| Workflow | Runs on | Does |
|---|---|---|
| `.github/workflows/build.yml` | every push and PR; called by `release.yml` | builds all our code, runs `make test`, packages the zip, smoke-tests the installer from the zip |
| `.github/workflows/release.yml` | tag `v*`, or *Run workflow* with a version | runs `build.yml`, then creates a GitHub Release with `ios3ds-<version>.zip` and its SHA-256 |

`build.yml` jobs:

| Job | Environment | Produces (artifact) |
|---|---|---|
| `firm` | `devkitpro/devkitarm` container | `build/boot/ios3ds.firm`, `VERSION`; `build/shim/shim.bin` + `gpu.bin` (`make shim`); `build/launcher/ios3ds.3dsx` (`make launcher`) |
| `userland` | ubuntu-24.04 + cached `build/toolchain` (key: hash of `scripts/build-ios-toolchain.sh`) | `build/rootfs-add/`: `ios3ds-input`, `ios3ds-sample`, `ios3ds-gltest`, `ios3ds-wifid`, `MBXGLEngine` (our GL engine) |
| `test` | ubuntu-24.04 | `make test` (Python unit tests, PICA, GL host test, Wi-Fi host tests) |
| `package` | ubuntu-24.04 | static `hfsplus`; `ios3ds-<version>.zip` (`scripts/mkrelease.py`); then `install.py --check` from the unpacked zip |

The same locally: `make firm shim launcher userland`, `scripts/build-hfsplus.sh --static`, `make release VERSION=v0.1`.

### The iOS toolchain without an IPSW

`scripts/build-ios-toolchain.sh` builds cctools/ld64, ldid and compiler-rt from pinned sources. Its stub SDK normally reads each library's exported symbol names from the dyld shared cache in the user's IPSW. CI has no IPSW, so `stage_sdk` falls back to `scripts/ios-sdk-symbols.json`:
- what it holds: the install names and versions of the 8 stub libraries, and the names of only the ~150 symbols our own tools import (`_CFRelease`, `_IOHIDEventCreateKeyboardEvent`, ...);
- it contains no code and no data from Apple's libraries;
- binaries linked against these stubs are **byte-identical** to the ones linked against the full stubs (checked for all five rootfs binaries).

If a tool starts calling a new library function, the CI link fails with an undefined symbol. Then regenerate the list on a machine with the IPSW, and commit it:

```sh
scripts/build-ios-toolchain.sh symbols
```

`IOS_SDK_FROM=list|cache` forces either source.

## What is in the zip

```
ios3ds-<version>/
  install.py                      runs scripts/install.py
  INSTALL.md README.md LICENSE    (INSTALL.md as docs/INSTALL.md)
  docs/other-versions.md          user guide for the experimental 4.1/3.1.3/2.2.1 installs
  prebuilt/                       our builds + MANIFEST.json (size, sha256, origin of each)
  scripts/ kernel/ rootfs/ tools/ the installer's Python scripts, patch recipes and launchd plists
  src/xpwn-hfsplus/               source of prebuilt/hfsplus-linux-x86_64 (GPL-3.0)
```

`install.py IPSW SD` runs `fwprep.py` (keys from api.ipsw.me, as in a repo build), then `mksd.py` with the prebuilt payload, shim, launcher and rootfs binaries (`--firm/--launcher/--prebuilt/--rootfs-add/--hfsplus`). The output is the same as `make sd`.

## Version choice in the release

The installer accepts `--ios 4.2.1|4.1|3.1.3|2.2.1` (default **4.2.1**; the zip's
root `install.py` forwards all arguments to `scripts/install.py`). Only those
four are accepted: other versions in `scripts/versions.py` are inventory only
and produce an `unknown iOS version` error. For every non-default version the
installer prints a prominent **HIGHLY EXPERIMENTAL / QEMU only / may not boot**
warning before doing any work.

After selecting the version, the installer:

- expects the matching `iPod2,1_<version>_<build>_Restore.ipsw` (a differently
  named file only gets a note — the hash is what matters) and verifies its
  sha1 against `scripts/versions.py`. A mismatch is an error, and `--force`
  does **not** bypass it;
- fetches the public keys for that build from `api.ipsw.me/v4/keys/ipsw/iPod2,1/<build>`
  via `fwprep.py` (or uses `--keys FILE`), so no keys are shipped in the zip;
- calls `mksd.py --ios VERSION` and `mkboot.py`/`mkrootfs.py` with that version's
  patch set and rootfs overlay — the same build path as `make sd IOS=VERSION`;
- writes the same three card paths as the 4.2.1 install. `--staging DIR`
  reuses a pre-extracted firmware only if its manifest names the chosen build.

The IPSW itself is never downloaded or shipped: 4.1/2.2.1 come from ipsw.me,
and Apple's CDN no longer serves the 3.1.3 (7E18) file, so that one needs an
archived copy with the pinned sha1. The PICA200 flags `IOS3DS_GPU313=1` and
`IOS3DS_GPU221=1` are developer build-from-source switches; the release
installer builds the default software-compositor images. See
[docs/other-versions.md](other-versions.md) for the user guide and per-version
status; the default 4.2.1 output is unchanged by this option. CI smoke-tests
the zip's `--ios` parsing and rejection of unknown versions.

## Provenance: what may ship

`scripts/mkrelease.py` packs an explicit allowlist and refuses files that look Apple-derived, by name (`kernel*`, `devtree*`, `devicetree*`, `rootfs.hfs`, `*.ipsw`, `*.img3`, `*.dmg`, `keys-*`, `boot.cfg`, `fakeio.bin`, ...) and, for the binaries, by content (IMG3 / kernelcache / dyld cache signatures, HFS+ headers). The CI smoke test checks the unpacked zip again.

| File | Shipped? | Why |
|---|---|---|
| `ios3ds.firm` | yes | built from `boot/` (ours; FatFs BSD, sdmmc.c GPL-2.0+) |
| `ios3ds.3dsx` | yes | built from `tools/ios3ds-launcher` + libctru (zlib licence) |
| `shim.bin`, `gpu.bin` | yes | built from `kernel/shim`, `tools/*/shim`, `tools/ios3ds-pica`. Shipped unconfigured; `mkboot.py` patches in the fake-register addresses (our constants from `kernel/dtgen.py`). |
| `rootfs-add/*` (input, sample, gltest, wifid, MBXGLEngine) | yes | our C sources, linked against stub libraries that contain only symbol names (above) |
| `hfsplus-linux-x86_64` | yes | xpwn (GPL-3.0); its source is in the zip (`src/xpwn-hfsplus`) |
| Python scripts, `kernel/patches*.json`, `rootfs/recipe.json`, plists | yes | already public in this repository. The recipes hold addresses and the few original bytes needed to check that the right firmware is being patched. |
| `kernel.bin` | **no** | Apple's kernelcache (decrypted, flattened, patched) |
| `devtree.bin` | **no** | generated from Apple's device tree (the IPSW's DeviceTree img3) |
| `rootfs.hfs` | **no** | Apple's root filesystem |
| firmware keys (`keys-*.json`) | **no** | fetched from api.ipsw.me at install time, like `make fwprep` |
| `boot.cfg` | **no** (generated) | ours, but it holds the patched kernel's SHA-1 and a memory layout computed from Apple's kernel size; `mkboot.py` writes it during install, defaults in `mkboot.py` |
| `fakeio.bin` | **no** (generated) | ours (`dtgen.fakeio_image()`: PrimeCell IDs and CLCD defaults), but written by `mkboot.py` during install anyway; not shipping it keeps one source of truth |
| Cydia, its base packages, Cydia Substrate (`*.deb` from apt.saurik.com), TLSFix (`*.deb` from github.com/ObscureMosquito/TLSFix) | **no** (downloaded) | third-party; `rootfs/cydia.json` pins each by sha256 and the installer fetches them (docs/cydia.md). TLSFix is MIT-licensed; its notice is in README.md |
| `rootfs/certs/roots.pem`, `roots-ec.pem` | yes | public CA certificates (Mozilla's list) |
| `build/toolchain/cache/` (dyld cache), full stub dylibs | **never** | Apple-derived; only exist on machines with the IPSW, never on CI or in the zip |

## Installer behaviour on update

When the installer runs into a folder that already has `ios3ds/rootfs.hfs`, it rebuilds the iOS disk and then carries the user's data over from **that card's** old image via `scripts/carryover.py` (Wi-Fi networks and passwords, settings, photos, voice memos, preferences, SpringBoard layout). `--fresh` skips the carry-over; `--backup-old` keeps a `rootfs.hfs.bak` copy of the previous disk. The developer path (`make sd` / `make sdimg`) never carries from the previous build output — that would carry stale SpringBoard defaults into QEMU images and between builds. Pass `make sd CARRY_FROM=/path/to/old/rootfs.hfs` (or set `IOS3DS_CARRY_FROM`) to opt in with an explicit old image; `IOS3DS_FRESH=1` still disables it. `install.py` ignores `CARRY_FROM`.

Not carried: installed Cydia packages (dpkg state would desync from the new system), `Library/Caches` (MobileInstallation's app list cache would be stale), `Library/Logs`, `*.sqlite-journal` / `*.lock` leftovers, and `/private/var/mobile/Media/iTunes_Control`: the Music library is rebuilt on every install by `scripts/music.py` from `SD/ios3ds/music` (and the test song), so the new library must win. Owners and modes come from the new image's parent directories, as in `mkrootfs.py` (hfsplus has no `chown`): `/private/var/mobile/…` stays 501:501, Keychains and SystemConfiguration stay root. Every carried entry is verified against the old image's mode/uid/gid/size.

Before copying, the old data's total size is compared with `hfsplus ls`'s free-space count. If it does not fit, or any entry fails to copy or verify, `carry_over()` returns `nothing`/`partial` (otherwise `carried`), keeps the card's old image as `sd:/ios3ds/rootfs.hfs.bak` and prints a warning with that path. The result also carries a `reason` and the `backup` path for callers.

## Verification done for the first version

- Installer from an unpacked zip (with `--staging build/staging`, because no IPSW was on the test machine; the `fwprep` step is unchanged from `make fwprep`) vs. `make sd` from the same commit:
  - `kernel.bin`, `devtree.bin`, `boot.cfg`, `shim.bin`, `gpu.bin`, `fakeio.bin`, both `ios3ds.firm`, `ios3ds.3dsx`: byte-identical;
  - `rootfs.hfs`: 95 bytes differ, all HFS+ dates. 19,291 catalog entries have identical modes/owners/sizes, and 13,560 files have identical data.
- The installer-built card (as a 2 GiB image, `mksd.build_image`) booted in QEMU (`emu/run.py --firm <card>/luma/payloads/ios3ds.firm --sd <img>`) to the SpringBoard home screen (screenshot at 240 s), with `gltest: PASS` and `[input] connected to SpringBoard's HID event system`.
- Installing into an existing card tree keeps unrelated files and `log.txt`, saves a changed `boot.cfg` as `boot.cfg.old`, and refuses a card with a pending launcher swap (`ios3ds/luma.firm`). Without the static `hfsplus`, the installer compiles it from `src/xpwn-hfsplus`.
