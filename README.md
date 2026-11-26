<div align="center">

# ios3ds

**Apple's real iOS 4.2.1 running natively on a Nintendo 3DS.**

Not an emulator: the iPod touch 2G's own kernel, SpringBoard and apps run directly on the 3DS's ARM11 CPU.

[![Build](https://github.com/devilAPI/ios3ds/actions/workflows/build.yml/badge.svg)](https://github.com/devilAPI/ios3ds/actions/workflows/build.yml)
[![Release](https://img.shields.io/github/v/release/devilAPI/ios3ds?include_prereleases&label=release)](https://github.com/devilAPI/ios3ds/releases)
[![License: GPL-3.0](https://img.shields.io/badge/license-GPL--3.0-blue.svg)](LICENSE)

[**Install**](#install) · [Controls](#controls) · [What works](#what-works) · [Troubleshooting](#troubleshooting) · [How it works](#how-it-works) · [Build from source](#build-from-source)

</div>

---

## What works

iOS boots to the home screen in about a minute. Apps open, Safari loads web pages over Wi-Fi, and Cydia comes pre-installed.

| | Feature | Notes |
|:-:|---|---|
| ✅ | Boot to the home screen, built-in apps | about a minute from power-on |
| ✅ | Touchscreen, HOME, POWER, START/SELECT, L/R | see [Controls](#controls); slide to unlock and popups over apps work |
| ✅ | Graphics on the 3DS GPU (PICA200) | the iOS interface is drawn by the GPU |
| ✅ | Sound, volume slider | speaker |
| ✅ | Battery level, charging, date and time, brightness | through the 3DS system controller |
| ✅ | Wi-Fi: scan, WPA2, WPA1 (TKIP) and WPA/WPA2 mixed networks (TKIP group), also routers in "WPA2 + WPA3" mode | needs the console's own Wi-Fi firmware, copied automatically by the launcher (or the GodMode9 script); stays connected through the router's key renewals; WPA1 is set up in `wifi.cfg` |
| ✅ | Safari | loads pages; big pages load in QEMU (3.6 MB HTML + 20 large images); TLS 1.2/1.3 with TLSFix, no opt-in patch needed |
| ✅ | Cydia | pre-installed, on the first home screen page |
| ✅ | Screen rotation (accelerometer) | apps rotate where iOS allows; the iPod home screen does not rotate (see [FAQ](#faq)) |
| ✅ | D-pad, circle pad | turned 90° to match the upright iOS screen: ← → pages, ↑ ↓ and the stick scroll |
| ✅ | Cameras (Camera.app, back and front, photos) | 320×240 live preview, about 12 fps; photos go to the Camera Roll. No video recording or 3D photos. Confirmed on an Old 3DS and a New 2DS XL |
| 🧪 | Microphone | recording works in the emulator; Voice Memos records Apple Lossless (playback fixed after 0.4.0, 🧪) |
| ⚠️ | Headphone jack | iOS switches to headphones when plugged in, but 0.4.0 was silent in them; CODEC fix after 0.4.0 not yet confirmed, see [docs/audio.md](docs/audio.md) §4 |
| 🧪 | Music app: a test song and your own songs | `install.py --music DIR` (or files in `SD/ios3ds/music/`): .mp3, .m4a (AAC/ALAC), .wav, .aif; [docs/audio.md](docs/audio.md) §10 |
| 🧪 | Gyroscope (CoreMotion) | |
| 🧪 | Stereoscopic 3D home screen (GPU stereo merged, needs a console test) | `stereo3d=1` in `boot.cfg`: the PICA200 builds both eyes from one uploaded frame and enables the barrier with the slider up, with **no per-frame CPU read-back** (batched alpha stamps); flat frames use the normal GPU mirror; see [docs/stereo3d.md](docs/stereo3d.md) |
| 🧪 | Fake GPS location (CoreLocation) | set with `boot.cfg` `gps=` or `/usr/bin/ios3ds-gps` from a terminal; 1 Hz updates in apps, including Safari, see [docs/location.md](docs/location.md) |
| 🧪 | Modern HTTPS: TLS 1.2/1.3 (TLSFix), current root certificates | Cydia, Safari and other apps; see [Limitations](#limitations) |
| 🧪 | Cydia Substrate (tweaks) | pre-installed; installing tweaks no longer hangs the 3DS; tweaks load into SpringBoard and apps, see [docs/cydia.md](docs/cydia.md) |
| 🧪 | New 3DS: 804 MHz CPU, C-stick, ZL/ZR | L2 cache optional, see [docs/n3ds.md](docs/n3ds.md) |
| 🧪 | New 3DS: auto-brightness from the ambient light sensor | emulator only, off on Old models; `auto_brightness=0` to disable, see [docs/mcu.md](docs/mcu.md) §13 |
| ❌ | IR, NFC, video recording | currently unplanned |

✅ works on a real 3DS · ⚠️ works on a real 3DS with known problems · 🧪 works in the emulator, not yet confirmed on a console · ❌ not working / not supported

### Compatibility

| Console | Status |
|---|---|
| **Old 3DS** | ✅ supported, the main test console |
| **Old 3DS XL** | ✅ boots to the home screen ([#7](https://github.com/devilAPI/ios3ds/issues/7); release 0.3.0 could panic in the accelerometer driver right after boot, fixed in `main`) |
| **Old 2DS** | ✅ supported (same hardware as the Old 3DS) |
| **New 2DS XL** | ✅ boots to the home screen with sound, power button, battery, Wi-Fi and cameras ([#1](https://github.com/devilAPI/ios3ds/issues/1), [#5](https://github.com/devilAPI/ios3ds/issues/5)) |
| New 3DS, New 3DS XL | untested; expected to behave like the New 2DS XL. On New models ios3ds runs the CPU at 804 MHz and uses the extra 128 MiB of RAM (`n3ds_ram=1`); Safari's memory figures are in [docs/n3ds.md](docs/n3ds.md) §6 |

All models need Luma3DS (boot9strap). Reports for any model are welcome in the [issues](https://github.com/devilAPI/ios3ds/issues).

---

## Install

You need about 15 minutes, a PC and your own copy of the iPod touch firmware. Nothing is installed to the 3DS itself; everything lives on the SD card.

### What you need

- A 3DS with **Luma3DS** (boot9strap) and the **Homebrew Launcher**, set up the usual way ([3ds.hacks.guide](https://3ds.hacks.guide)).
- An SD card with about **1.2 GB** free.
- A PC with **Python 3.8+** and internet access, needed once to fetch the firmware keys and Cydia:

  | System | Setup |
  |---|---|
  | Linux | `sudo apt install python3 python3-cryptography` |
  | macOS | Python 3, `pip3 install cryptography`, `xcode-select --install` |
  | Windows | use WSL (Ubuntu) and follow the Linux steps; the SD card is `/mnt/<letter>` |

### 1. Download

1. **ios3ds:** `ios3ds-<version>.zip` from the [Releases page](https://github.com/devilAPI/ios3ds/releases). Unzip it anywhere.
2. **The iOS firmware** (ios3ds can't ship it): `iPod2,1_4.2.1_8C148_Restore.ipsw`, the iPod touch 2nd generation on iOS 4.2.1, from Apple's servers via [ipsw.me](https://ipsw.me/download/iPod2,1/8C148).

### 2. Run the installer

Put the SD card in your PC, open a terminal in the unzipped folder and run:

```sh
python3 install.py ~/Downloads/iPod2,1_4.2.1_8C148_Restore.ipsw /media/$USER/YOUR_SD_CARD
```

Use your own paths. The card is `/Volumes/NAME` on macOS and `/mnt/e` (or another letter) in WSL.

It takes a few minutes and needs about 4 GB of free space on the PC while it works. It:
1. decrypts the kernel and the iOS disk from your IPSW, and checks that it is the right file;
2. patches the kernel and builds the iOS disk, including Cydia and Cydia Substrate (6.6 MB from apt.saurik.com; add `--no-cydia` to skip it) and TLSFix (3.2 MB from GitHub; `--no-tlsfix` to skip it);
3. writes three things to the card and touches nothing else:

| On the SD card | What it is |
|---|---|
| `3ds/ios3ds.3dsx` | the app you start from the Homebrew Launcher |
| `luma/payloads/ios3ds.firm` | the same, for Luma's chainloader menu |
| `ios3ds/` | iOS itself (`rootfs.hfs`, 1 GB), the patched kernel, and `boot.cfg` (settings) |

Without an SD card path, the files go to `ios3ds-sd/`. Copy what is **inside** that folder to the root of the card.

> **Updating:** re-running the installer on a card that already has ios3ds replaces the iOS disk but carries your data (Wi-Fi networks and passwords, settings, photos, voice memos, preferences, SpringBoard layout) over from the previous `rootfs.hfs`. Installed Cydia packages and the Music library are not carried over. Add `--fresh` to discard everything and start clean; `--backup-old` keeps a `rootfs.hfs.bak` copy of the previous disk. If the data does not fit or any file fails to copy, the previous disk is kept as `sd:/ios3ds/rootfs.hfs.bak` automatically and you get a warning.

### 3. Start it

Open the **Homebrew Launcher**, start **ios3ds** and press **A**. The 3DS reboots into iOS.

Alternatively, hold **START** while powering on and pick `ios3ds` in Luma's chainloader menu.

**First start:**
- The launcher copies your console's Wi-Fi firmware to the card, reading it only. It shows `Wi-Fi firmware: reading NWM .code ...`, writes one file, `ios3ds/nwm.code`, and skips this step afterwards.
- The bottom screen shows the version, `SD ok`, `ARM11 ok` and a short countdown (1 s). Press **A** to skip it and start XNU at once, or hold **B** to stop in the monitor. Set `boot_delay=0` in `boot.cfg` to skip it on every boot.
- The top screen scrolls a boot log. After about a minute the iOS home screen appears.
- iOS is upright, so turn the 3DS 90° clockwise. The bottom screen shows a copy of iOS that you tap with the stylus.

> **Wi-Fi:** in iOS, open Settings → Wi-Fi and pick your network.
>
> If the launcher showed a red `Wi-Fi fw: ...` line, copy the firmware once with GodMode9 instead:
> 1. Hold START while powering on and choose GodMode9.
> 2. Press HOME → `Scripts...` → `ios3ds-wifi-fw`, then confirm with A.
>
> Details: [docs/wifi.md](docs/wifi.md) §3.

---

## Controls

| 3DS | iOS |
|---|---|
| Touchscreen | tap, swipe, type |
| **HOME** or **START** | Home button |
| **POWER** (press) or **SELECT** | lock / sleep |
| **L / R** | volume down / up |
| **Circle pad** | scroll (in the direction of the upright iOS screen) |
| **D-pad** | with the 3DS turned so iOS is upright: ← → previous / next page, ↑ ↓ scroll by about a page |
| **X** | screen layout: iOS on top + touch copy below → iOS on the bottom only → iOS on top only |
| **C-stick** (New 3DS) | like the D-pad: flick ← → = previous / next page, ↑ ↓ = scroll by about a page |
| **ZL / ZR** (New 3DS) | scroll to the top / screenshot |
| **POWER** (hold ~3 s) | clean shutdown: iOS unmounts its disk and the 3DS powers off (same path as "slide to power off") |
| **POWER** (hold ~10 s) | hardware power cut if iOS is stuck; the next power-on is your normal Luma + HOME Menu |
| SELECT + START (hold) | debugging only: crash iOS on purpose and write a report to the log |

---

## Is it safe?

**ios3ds never writes to the 3DS's internal memory (NAND) or firmware.** It has no code that could. It only reads and writes files on the SD card.

A homebrew app can't start another system directly, so the launcher swaps files on the SD card and reboots:
1. It creates `ios3ds/swap.flg`, a "swap in progress" note.
2. It renames your `boot.firm` (Luma) to `ios3ds/luma.firm`.
3. It renames `ios3ds/ios3ds.firm` to `boot.firm`.
4. It reboots.

The first thing ios3ds does is undo this, so Luma is back as `boot.firm` before iOS even starts. These are renames, never copies, so a power cut can't leave a half-written `boot.firm`. If it ever stops halfway:
- the 3DS falls back to the backup `boot.firm` in its internal storage (set up by the 3ds.hacks.guide);
- the launcher offers **Y: restore Luma** when it finds a leftover `ios3ds/luma.firm`;
- or rename `ios3ds/luma.firm` back to `boot.firm` on a PC.

As with any homebrew, **back up your SD card** first. More: [docs/real-hardware.md](docs/real-hardware.md).

---

## Troubleshooting

- **Logs:** every boot writes **`sd:/ios3ds/log.txt`**, and the previous one is kept as `log_prev.txt` (the tools read those names). At mount, the older `log_prev.txt` is *renamed* into **`sd:/ios3ds/logs/`** as `YYYY-MM-DD_HH-MM-SS.log`, dated from that boot's own RTC line (`undated` if there was none); the newest 32 are kept. Please [open an issue](https://github.com/devilAPI/ios3ds/issues/new/choose) and attach `log.txt`, plus a dated file from `logs/` if the problem was in an earlier boot.
- **Crash reports:** when an app or SpringBoard dies, iOS writes a `.plist`, `.crash` or `.panic` file into `sd:/ios3ds/rootfs.hfs` at `/private/var/mobile/Library/Logs/CrashReporter`. To send them, put the SD card in your PC and run `python3 scripts/crashlogs.py /path/to/SD_CARD crash-reports` (Linux: the whole card device, e.g. `/dev/sdX`; macOS: `/dev/disk2`) or point the script at a copy of `ios3ds/rootfs.hfs`. It writes every report into `crash-reports/` and prints the file names; attach those files to the issue. It only reads the image; for a raw card image it makes a temporary 1 GiB copy of the iOS disk under `build/` (`--tmpdir` changes that), because the HFS+ editor needs a plain file. The card itself is never written. Needs `build/tools/hfsplus` (`make hfsplus`) and `mtools`.
- **Settings:** they live in **`sd:/ios3ds/boot.cfg`**, a text file. Useful switches:

  | Switch | Effect |
  |---|---|
  | `gpu=0` | don't use the 3DS GPU at all: try this first if the screen stays black or garbled |
  | `ca_gpu=0` | draw the iOS interface in software (slower) |
  | `screen=0/1/2` | start in screen layout top / both / bottom |
  | `camera=0`, `gyro=0`, `hp_route=0` | turn off the cameras, the gyroscope, or headphone switching |
  | `cam_test=1` | run the ARM9 camera self-test before XNU; sensors and CAM blocks are reset afterwards before iOS starts (hardware check still pending; see [docs/camera.md](docs/camera.md)) |
  | `gps=52.5200,13.4050` | report a fake location to apps (latitude,longitude[,accuracy in metres]); empty = off |
  | `wifi_led=0` | don't light the console's Wi-Fi LED while connected |
  | `boot_delay=0` | skip the 1 s wait before XNU (or press **A** during it); the old name `countdown_ms` still works |
  | `diag=1` | extra diagnostics in the log |
  | `safe_mode=1` | no Cydia Substrate tweaks this boot (same as holding **L** while the 3DS loads iOS); see the FAQ |
  | `stereo3d=1` | stereoscopic 3D home screen (merged GPU stereo: PICA200 two-eye mirror + barrier via GPU present, no CPU read-back; needs a console test); flat frames use the normal GPU mirror |

### FAQ

- **Why doesn't the home screen rotate (including 180°)?** Stock iOS 4 on an
  iPod touch never rotates the SpringBoard home screen, regardless of the
  accelerometer. Apps decide for themselves which orientations they draw: our
  driver reports all four, including upside-down
  (`UIDeviceOrientationPortraitUpsideDown = 2`), and QEMU confirms it; Safari's
  bookmarks view, for example, ignores 2 and 4 on this iOS build. See
  [docs/mcu.md](docs/mcu.md) §6.2.
- **Why is the screen still lit with the brightness slider at 0?** Real iPods
  never turn the backlight fully off from the slider. We clamp the minimum to
  a readable level; locking or sleeping the device still switches the screen
  off.
- **Why is there a pause before XNU starts?** A 1 s grace period leaves time to
  hold **B** and stay in the monitor; press **A** to skip it once, or set
  `boot_delay=0` in `boot.cfg` to skip it every boot.
- **Settings shows no Wi-Fi switch ("no way to enable WiFi")?** Wi-Fi needs
  your console's `nwm.code` (the launcher copies it on the first start, see
  [Start it](#3-start-it)), but **no `wifi.cfg`**: Settings scans and joins
  without one. If `log.txt` shows `HTC service setup failed: -5` right after
  `service 0100 -> status 0`, the 0.4.0 driver ran out of its own HTC control
  credits before the chip was ready; fixed after 0.4.0 (issue #11). Any other
  `FAILED in stage ...` line, or a red `Wi-Fi fw:` line from the launcher, is
  a different problem — attach the whole log.
- **A home WPA2 network joins as WEP, or the log says `key type 2 / 13 bytes`?** A long country/HT IE list could push the RSN/WPA IE out of the ARM9's 76-byte scan record, so iOS saw the privacy bit without RSN and sent the password as a WEP key. The scan record now copies SSID, rates, DS, RSN, WPA and extended-rate IEs first; an overflow that still cannot fit logs `[wifi-diag] scan: N needed IE(s) did not fit`. The fix has a host test but no console join report yet — attach `log.txt` with the network's security mode and channel (no password needed); see [docs/wifi.md](docs/wifi.md) §10.9.
- **Camera fails after a boot with `cam_test=1`?** After the self-test the ARM9 now soft-resets every sensor, power-cycles the CAM blocks and probes again, so iOS starts as after a `cam_test=0` boot. This reset is not yet confirmed on a console; attach `log.txt` and look for `[cam-test] sensors reset ...` (re-log 145).
- **A tweak broke SpringBoard / the 3DS no longer boots after installing one?**
  Hold **L** while the 3DS loads iOS (until `SAFE MODE` shows on the top
  screen), or put `safe_mode=1` into `boot.cfg`: Substrate tweaks are not
  loaded in SpringBoard or any app, so you can remove the tweak in Cydia.
  Without that, if SpringBoard dies twice in a row within 90 s of starting,
  its next start is a safe one by itself (`[safe]` lines in `log.txt`). This
  only skips Substrate tweaks; a boot that hangs in launchd before SpringBoard
  starts is not covered (see [docs/cydia.md](docs/cydia.md)).
  Pressing SELECT+START (the forced panic) or pulling power while Cydia or a
  tweak writes files can leave iOS's disk inconsistent; let installs finish
  and use the iOS power-off slider where you can.
- **Why is the wallpaper only on the lock screen?** Apple did not enable
  home-screen wallpaper on the iPod touch 2G (`iPod2,1`): SpringBoard's
  `homescreen-wallpaper` capability is absent from that device's capabilities
  plist, although the key exists in iOS 4.2.1's SpringBoard/dyld cache. That
  is stock behaviour, not a rendering bug; lock-screen wallpaper works and can
  be set in Settings → Wallpaper.

### Fake GPS location

The iPod has no GPS, so Maps and geolocation in Safari get no fix by default.
Add `gps=LAT,LON` (optionally `,ACC`) to `boot.cfg`, or run
`ios3ds-gps set LAT LON [ACC]` / `ios3ds-gps show` / `ios3ds-gps off` in a Cydia
terminal or over SSH. That sets a 1 Hz CoreLocation fix in apps without
rebooting; details and limits: [docs/location.md](docs/location.md).

- **Re-running the installer** replaces `boot.cfg`; the old one is kept as `boot.cfg.old`. The installer refuses a card with a leftover `ios3ds/luma.firm`: start ios3ds once, or restore Luma first.
- **What is kept on an update:** re-running the installer (the user's card) carries Wi-Fi networks and passwords, settings, photos, voice memos, preferences and the SpringBoard layout from the previous `sd:/ios3ds/rootfs.hfs`. Use `--fresh` to start clean, `--backup-old` to keep an extra copy of the old disk. Developer builds are different: `make sd` / `make sdimg` never carry data from the last build (QEMU images stay reproducible); opt in with `make sd CARRY_FROM=/path/to/old/rootfs.hfs`. Caches, logs, transient `*.sqlite-journal`/`*.lock` files, installed Cydia packages and the Music library (`Media/iTunes_Control`, rebuilt from `ios3ds/music` and the test song) are not carried. If anything fails, the old image is kept as `sd:/ios3ds/rootfs.hfs.bak` and a warning is printed.
- **Uninstall:** delete `ios3ds/`, `3ds/ios3ds.3dsx` and `luma/payloads/ios3ds.firm` from the card.

### Limitations

- **iOS 4.2.1 is the newest possible:** later versions need an ARMv7 CPU, and the 3DS's ARM11 is ARMv6.
- **HTTPS:** iOS 4.2.1 itself speaks TLS 1.0 at most. The pre-installed TLSFix tweak adds TLS 1.2/1.3 and EC certificates for apps that use iOS's networking, and current root certificates are added ([docs/certificates.md](docs/certificates.md)); leave it out with `--no-tlsfix`. **Safari and App Store apps** run in Apple's sandbox, which denies `/Library`: the build also installs the two dylibs TLSFix needs under `/usr/lib` (allowed by the profile) and `ios3ds-applog` loads those when the `/Library` `dlopen` fails, so Safari speaks TLS 1.2/1.3 too, without the old `make sd SANDBOX=1` kernel patch ([docs/cydia.md](docs/cydia.md)). Sites with expired or untrusted certificates are refused everywhere. The App Store icon opens the app, but the store's own requests now fail with an error (Apple's 2010-era store endpoints are gone); that is expected, and SpringBoard stays up.
- **OpenGL ES apps and games:** 3D runs through our own OpenGL ES 1.1 engine. PVRTC 2bpp/4bpp textures are decoded in software, and the ES 1.1 extensions games probe (`OES_draw_texture`, `OES_point_sprite`, `OES_framebuffer_object`, `OES_matrix_palette`, mipmap generation) are implemented. There is no ES 2.0: EAGL refuses ES2 contexts and ES2-only calls fail with `GL_INVALID_OPERATION`, so games that fall back to ES1 keep running. Mipmap *sampling* still uses the base level. See [docs/gpu.md](docs/gpu.md) §3.
- **Tweaks (Cydia Substrate):** Substrate is pre-installed and held at its version; don't remove or "upgrade" it in Cydia. Tweaks load into SpringBoard and apps, not into system daemons (injecting into launchd hangs iOS 4.2.1 on the 3DS, so ios3ds blocks it and repairs `/etc/launchd.conf` at boot).
- **Memory:** iOS gets the iPod's 128 MB. Safari's "Cannot allocate memory" on big pages was measured in QEMU with a local 3.6 MB page and 20 × 1024² images: all of it loaded, ending with 20.1 MiB free and no jetsam kill; the hardware alert is still not reproduced in QEMU. The console `[mem]` lines show why it looks contradictory: `free` includes speculative file pages, leaving only 1.9-3.9 MiB immediately usable while Safari runs. The unused `profiled` daemon is disabled (2.4 MiB), Safari's WebKit caches are trimmed, and the Old 3DS GPU arena shrank from 12 to 10 MiB (2 MiB more for apps; the camera keeps its 450 KiB block). Failed large allocations are logged by the app interposer. See [docs/re-log.md](docs/re-log.md) 112 for the before/after numbers and the hardware checklist. New 3DS models use the extra 128 MiB of RAM (`n3ds_ram=1`): Safari has 168.5 MiB free there, against 48.1 MiB on an Old 3DS.
- **Cameras:** 320×240 preview at about 12 fps (photos are scaled up from it); no video recording and no 3D photos.
- **Home-screen wallpaper:** not present on the iPod touch 2G in stock iOS 4.2.1 (Apple enabled it on newer devices); the lock screen does show the wallpaper you pick in Settings. See the [FAQ](#faq).
- **Updates rebuild the iOS disk:** user data (settings, saved Wi-Fi networks and passwords, photos, voice memos, preferences) is carried over automatically by the installer; installed Cydia packages and the Music library are not (reinstall packages after updating). Use `--fresh` (installer) to start with a clean disk.
- **No hardware for:** Bluetooth (the 3DS has none), GPS, IR, NFC. The 3D screen path (PICA200 two-eye mirror, GPU present with no CPU read-back) is merged but not yet confirmed on a console ([docs/stereo3d.md](docs/stereo3d.md)). A user-set fake location works in CoreLocation apps ([docs/location.md](docs/location.md)).
- **Leaving iOS:** a short POWER press locks/unlocks; holding it for about 3 s now starts iOS's own clean shutdown (the same path as "slide to power off") and switches the 3DS off through the MCU. Holding it for ~10 s still forces a hard power cut if iOS is stuck. Apple's boot-time `fsck` cannot check the root disk (`/dev/md0` is already mounted, `EBUSY`); the ARM9 instead logs at boot whether `rootfs.hfs` was cleanly unmounted and sets that flag after a clean shutdown ([docs/mcu.md](docs/mcu.md) §12). A hard cut can still damage files: back up your card.
- **Not supported:** activation, iTunes sync and the data-protection keybag are stubbed out.

---

## How it works

```
3DS boot (Luma3DS chainloader)
 ├─ ARM9   our "monitor": both screens' consoles, SD card, the iOS root disk (served from rootfs.hfs),
 │          Wi-Fi (AR6014 driver + WPA2), system controller, cameras, gyro, screen copy
 └─ ARM11  Apple's XNU kernel, stock, + binary patches (kernel/patches.json) + our "shim"
            ├─ timers, interrupts, console → mapped onto the 3DS's MPCore
            ├─ display → Apple's own LCD driver on a fake register page; the PICA200 GPU copies to the LCDs
            ├─ audio, touch, buttons, battery, Wi-Fi … → mailboxes to the ARM9
            └─ launchd → configd, mediaserverd, … → SpringBoard → your apps
```

Apple's kernel believes it is running on an iPod touch. Where the 3DS hardware differs, small patches and the shim translate. The ARM9 handles everything slow or Nintendo-specific. Full picture: [docs/architecture.md](docs/architecture.md).

The project has its own QEMU machine (`-M nintendo3ds`, [docs/emulator.md](docs/emulator.md)) that models the 3DS hardware, from the GPU to Wi-Fi, cameras and gyro. Every change is tested there before it reaches a console. Citra/Azahar only emulate 3DS *apps*; they can run the launcher but not ios3ds itself.

---

## Build from source

Linux. Requirements: Python 3 with `cryptography`, an `arm-none-eabi` toolchain (Arm GNU Toolchain or devkitARM), `mtools`, `dosfstools`, `sfdisk`, git. The iOS toolchain builds `ldid`, which needs OpenSSL 3 headers; on systems with OpenSSL 4 also install OpenSSL 3 (the script finds `/usr/include/openssl-3` and similar).

```sh
make toolchain                                       # check the tools
make fwprep IPSW=~/iPod2,1_4.2.1_8C148_Restore.ipsw   # decrypt kernel, device tree, rootfs -> build/staging
scripts/build-hfsplus.sh                             # HFS+ image editor (xpwn)
scripts/build-ios-toolchain.sh                       # armv6 iOS toolchain for our iOS programs (once)
make userland                                        # our iOS programs -> build/rootfs-add/
make sd                                              # SD card tree -> build/sd/  (copy its contents to the card)
make test                                            # offline unit tests
```

Experimental: `make sd IOS=VERSION` selects another iPod2,1 firmware from
`scripts/versions.py` (firmware staged under `build/fw-VERSION/`); 4.2.1 stays
the default and unchanged. In QEMU, **4.1, 3.1.3 and 2.2.1 all boot to the home
screen, open apps and pass fresh-image soaks**; none of these three has been
tested on a real console. Cydia is installed on all three: 4.1 and 3.1.3 use
the 4.x package set (TLSFix only on 4.1), and 2.2.1 uses Cydia 1.0.3222 with no
default source. 3.1.3 uses software CoreAnimation and the ARM9 screen copy by
default; for a console test of its experimental PICA200 compositor, build with
`IOS3DS_GPU313=1 make sd IOS=3.1.3` (slower in QEMU, not the default). The
per-version driver status is in [docs/multi-version.md](docs/multi-version.md) §6.9.

Optional:
- `make qemu` builds the emulator, about 10 minutes.
- `make emu` boots `build/sd.img` in it.
- `make launcher` builds `ios3ds.3dsx` and needs devkitARM + libctru.
- `make release` builds the release zip ([docs/release.md](docs/release.md)).

**No Apple files in this repository.** You supply the IPSW, and everything Apple-derived is generated into `build/` (gitignored). The patch recipes hold only addresses and the few original bytes needed to check that the right firmware is being patched.

### Documentation

| | |
|---|---|
| [architecture.md](docs/architecture.md) | boot chain, memory map, every hardware bridge |
| [multi-version.md](docs/multi-version.md) | other iOS versions (4.1/3.1.3/2.2.1 QEMU status and driver matrix), 1.x feasibility |
| [real-hardware.md](docs/real-hardware.md) | testing safely on a console |
| [emulator.md](docs/emulator.md) | the QEMU `nintendo3ds` machine, Azahar |
| [gpu.md](docs/gpu.md) · [pica200.md](docs/pica200.md) | OpenGL ES engine, CoreAnimation and the screen copy on the PICA200 |
| [perf.md](docs/perf.md) | Old 3DS frame drops: folder open / Safari profiling, the `[perf]` frame histogram, `emu/perf.py` |
| [audio.md](docs/audio.md) | speaker, headphones, microphone |
| [mcu.md](docs/mcu.md) | battery, clock, buttons, sliders, accelerometer, gyroscope |
| [wifi.md](docs/wifi.md) · [certificates.md](docs/certificates.md) | Wi-Fi driver, WPA2, iOS integration; HTTPS roots |
| [camera.md](docs/camera.md) | cameras in Camera.app |
| [input-design.md](docs/input-design.md) | touch, buttons, circle pad |
| [app-matrix.md](docs/app-matrix.md) | one launch of every stock app icon in QEMU |
| [cydia.md](docs/cydia.md) | the pre-installed Cydia |
| [location.md](docs/location.md) | fake GPS location: boot key, runtime tool, CoreLocation hooks |
| [re-log.md](docs/re-log.md) | the reverse-engineering log, step by step |
| [hwlogs/](docs/hwlogs/README.md) | logs from real hardware runs |

---

## Credits

- [samhenrigold/qemu-ios](https://github.com/samhenrigold/qemu-ios) (Light Touch): research on booting 8C148, invaluable.
- [roastme/ios3ds](https://github.com/roastme/ios3ds): iPhone OS 1.1 / S5L8900 trap-and-emulate reference (GPL-2.0-or-later); its design informed the 1.x feasibility study in [docs/multi-version.md](docs/multi-version.md) §4. No code was copied.
- [devos50/qemu-ios](https://github.com/devos50/qemu-ios): iPod touch emulation and S5L8720 notes.
- [darwin-on-arm](https://github.com/darwin-on-arm/xnu) (winocm): boot_args and pexpert reference.
- [libn3ds](https://github.com/profi200/libn3ds), [Luma3DS](https://github.com/LumaTeam/Luma3DS), [linux-3ds](https://github.com/linux-3ds), [dsiwifi](https://github.com/shinyquagsire23/dsiwifi), GBATEK and 3dbrew: 3DS hardware references.
- Normmatt's SD driver, FatFs by ChaN, [xpwn](https://github.com/planetbeing/xpwn), and The Apple Wiki.
- Everyone testing on real consoles in the [issues](https://github.com/devilAPI/ios3ds/issues).

## License

GPL-3.0-or-later ([LICENSE](LICENSE)), with these exceptions:
- `boot/arm9/sdmmc.c` and `emu/qemu/` are GPL-2.0-or-later.
- `boot/arm9/fatfs/` is under FatFs's 1-clause BSD licence.
- `boot/common/font8x8_basic.h` is public domain.

Downloaded during install, never shipped in the release zip: Cydia and its base
packages and Cydia Substrate (Jay Freeman, from apt.saurik.com), and **TLSFix**
1.3 ([ObscureMosquito/TLSFix](https://github.com/ObscureMosquito/TLSFix), fork of
[nfzerox/TLSFix](https://github.com/nfzerox/TLSFix); it contains OpenSSL 1.1.1w,
under the OpenSSL and SSLeay licences), under this licence:

> MIT License
>
> Copyright (c) 2026 nfzerox
>
> Permission is hereby granted, free of charge, to any person obtaining a copy
> of this software and associated documentation files (the "Software"), to deal
> in the Software without restriction, including without limitation the rights
> to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
> copies of the Software, and to permit persons to whom the Software is
> furnished to do so, subject to the following conditions:
>
> The above copyright notice and this permission notice shall be included in all
> copies or substantial portions of the Software.
>
> THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
> IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
> FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
> AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
> LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
> OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
> SOFTWARE.

iOS, iPod and XNU are trademarks of Apple; Nintendo 3DS is a trademark of Nintendo. This is an independent research project, not affiliated with either.
