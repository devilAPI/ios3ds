# Other iOS versions (highly experimental)

> 🚨 **EVERYTHING OTHER THAN 4.2.1 IS HIGHLY EXPERIMENTAL.** 🚨
>
> **iOS 4.1, 3.1.3 and 2.2.1 have been tested only in the QEMU emulator, not on
> a Nintendo 3DS console, and may not boot.** They are offered so that people
> can help test them; there is no guarantee that the home screen, apps or your
> data will survive. **Back up your SD card first.** Only iOS **4.2.1** is the
> supported, console-tested build described in the [README](../README.md).

This page is the short user guide. The evidence, the full driver-by-driver
matrix and the build-time details are in
[docs/multi-version.md](multi-version.md) §6.9 and [docs/re-log.md](re-log.md).

## The four installable versions

| iOS | Build | IPSW file | sha1 of the IPSW |
|---|---|---|---|
| **4.2.1** (supported default) | 8C148 | `iPod2,1_4.2.1_8C148_Restore.ipsw` | `b9efddc7bb4350c237a8d3846af61bbfc8a2f647` |
| 4.1 🧪 | 8B117 | `iPod2,1_4.1_8B117_Restore.ipsw` | `97abde6207660bd876fd476275dd526d0dcf3d19` |
| 3.1.3 🧪 | 7E18 | `iPod2,1_3.1.3_7E18_Restore.ipsw` | `5f4f5c01eda2f811f73167e7d1f82dbeed82367b` |
| 2.2.1 🧪 | 5H11a | `iPod2,1_2.2.1_5H11a_Restore.ipsw` | `9af5625ea34acdd8abeb6fce71a72651d0c815d5` |

You supply the IPSW: ios3ds never ships Apple's firmware and the installer does
not download it. 4.1 and 2.2.1 are still linked from [ipsw.me](https://ipsw.me/device/iPod2,1).
Apple's CDN no longer serves the 3.1.3 (7E18) file (HTTP 403); the bring-up used
an archived iPod2,1 IPSW (search the `ipod-touch-2g-ipsw` collection on
archive.org). Its sha1 is pinned above: **the installer refuses any other
file**, whatever its name, so check the hash before you start. Other iPod2,1
versions in `scripts/versions.py` are inventory only and are refused by the
installer (`--ios` accepts only the four in the table).

## Install

Use the same release zip as for 4.2.1 ([README → Install](../README.md#install)),
pick the matching IPSW and add `--ios` to the installer command:

```sh
# iOS 4.1 (8B117) -- experimental, emulator only
python3 install.py --ios 4.1 ~/Downloads/iPod2,1_4.1_8B117_Restore.ipsw /media/$USER/YOUR_SD_CARD

# iOS 3.1.3 (7E18) -- experimental, emulator only
python3 install.py --ios 3.1.3 ~/Downloads/iPod2,1_3.1.3_7E18_Restore.ipsw /media/$USER/YOUR_SD_CARD

# iOS 2.2.1 (5H11a) -- experimental, emulator only
python3 install.py --ios 2.2.1 ~/Downloads/iPod2,1_2.2.1_5H11a_Restore.ipsw /media/$USER/YOUR_SD_CARD
```

Use your own paths (`/Volumes/NAME` on macOS, `/mnt/<letter>` in WSL). Without
an SD-card path, the files go to `ios3ds-sd/`; copy its contents to the card.
`--fresh`, `--backup-old`, `--no-cydia`, `--no-tlsfix`, `--music DIR` and
`--keys FILE` work exactly as in the 4.2.1 install. Omitting `--ios` installs
4.2.1, with the same output as before.

For every non-default version the installer prints a prominent
`WARNING: iOS X is HIGHLY EXPERIMENTAL ... may not boot` notice before it does
any work. It then:

1. checks the IPSW sha1 against the pinned value in `scripts/versions.py`
   (a mismatch is an error; `--force` does **not** bypass it);
2. fetches the **public keys for that build** from `api.ipsw.me` (or uses
   `--keys FILE`), and decrypts the kernel, device tree and iOS disk;
3. builds the card with the same kernel patch set, rootfs recipe and userland
   binaries as `make sd IOS=X`, writing only `ios3ds/`, `3ds/ios3ds.3dsx` and
   `luma/payloads/ios3ds.firm`.

The advanced `--staging DIR` option reuses an already extracted firmware
(`make fwprep IPSW=... IOS=X`); its `manifest.json` must name the selected
build. There is no need for a toolchain, HFS+ editor source tree or any other
developer setup: the release zip carries the prebuilt binaries.

When switching **between** iOS versions (for example from 4.2.1 to 2.2.1), use
`--fresh`: settings and SpringBoard layouts from another major iOS version can
be incompatible. On a same-version update, the usual data carry-over applies.

## What works in each version

All three rows below are **QEMU results only**. None has a console report;
"PASS" means a fresh-image `emu/soak.py` run with apps opened, not a boot on a
real 3DS. See the [driver matrix](multi-version.md#69-current-status-and-driver-matrix-2026-11-26)
for the full table.

### iOS 4.1 (8B117) 🧪

- Home screen, stock apps, touch/buttons, lock/wake and auto-lock, brightness
  and battery all work; 10-cycle `--require-app` soak PASS.
- Wi-Fi and Safari work with the 4.x tools: `wifi_test` 20/20, Settings join
  8/8, three pages loaded; TLSFix gives TLS 1.2/1.3.
- Speaker, volume, headphone route, keyboard clicks, the ALAC test song and the
  microphone (`ios3ds-tone rec`) work in QEMU; the accelerometer reports all
  four orientations. Gyro works (`gyroAvailable 64`), but `deviceMotion` rates
  stay 0. Fake GPS works via `ios3ds-gpstest`.
- Cydia 1.1.30 opens and local-repo package installs pass. The camera chip
  initialises (`[cam] chip id 2280`), but Camera.app was not exercised; the GPU
  path is the same CoreAnimation + GPU mirror as 4.2.1.

### iOS 3.1.3 (7E18) 🧪

- Home screen, stock apps, touch/buttons, clock, lock/wake, slide unlock,
  brightness and battery work; 5-cycle `--require-app` soak PASS.
- Wi-Fi/Safari work (`wifi_test` 20/20, Settings join 8/8, 3 pages), with
  keychain auto-join. Fake GPS works through the applog hooks.
- UI sounds, `ios3ds-tone`, volume, headphone route, keyboard clicks and the
  test song work. No CoreMotion (no gyro before 4.0) and no Camera.app on this
  firmware.
- Cydia 1.1.30 opens, with APT/dpkg but **no TLSFix** (no Substrate loader on
  3.x). By default the interface is composited in **software** and the ARM9
  copies the screen (`gpu_mirror=0`); the PICA200 path is opt-in (below).

### iOS 2.2.1 (5H11a) 🧪

- Home screen, stock apps and taps work with a 2.x build of our input daemon;
  10-cycle `--require-app` soak PASS. Lock/wake, brightness and battery work.
- Wi-Fi/Safari work with a Darwin-9 ifnet shim: `wifi_test` PASS, Settings join
  8/8 and a typed URL loads. Fake GPS works through 2.x dylib builds.
- `ios3ds-tone`, headphone route, keyboard click and the PCM test song work;
  the microphone is **off** (no VirtualAudio patch, no built-in mic in N72's
  routing database). The accelerometer reports orientation changes. No
  Camera.app and no CoreMotion on this firmware.
- Cydia **1.0.3222** opens, with APT/dpkg but no default source, no TLSFix and
  no Substrate. By default the interface is composited in software with the
  ARM9 screen copy (`gpu_mirror=0`); the PICA200 path is opt-in (below).

If a non-default version does not boot, attach `sd:/ios3ds/log.txt` and the
exact `--ios` value and IPSW sha1 when you [open an issue](https://github.com/devilAPI/ios3ds/issues/new/choose).
State clearly that this was an emulator or console run: console reports for
these versions are especially valuable.

## Opt-in GPU flags (build from source)

On 3.1.3 and 2.2.1 the release installer builds the safe, default software
compositor. The PICA200 CoreAnimation path is **build-time opt-in** because it
needs a dyld-cache patch and a SpringBoard plist value baked into the image:

```sh
IOS3DS_GPU313=1 make sd IOS=3.1.3    # 3.1.3: GL compositor + display mirror
IOS3DS_GPU221=1 make sd IOS=2.2.1    # 2.2.1: ios3ds-gles2.dylib EGL/GLES interposer
```

These are developer-build flags: they require the source tree and `make
userland`. Both paths are slower in QEMU than the software compositor and
neither has been tested on a console, so they are not the default and are not
selected by `install.py`. Add `IOS3DS_GPU313_DIAG=1` or
`IOS3DS_GPU221_DIAG=1` to those commands plus `diag=1` in `boot.cfg` for the
extra `[gl] [swapdiag]` / `[gl] CA:` log lines (see the GPU sections of
[docs/multi-version.md](multi-version.md) §6.9). Everything else — including
the ability to install these versions without a developer setup — is the same
as the default 4.2.1 install.
