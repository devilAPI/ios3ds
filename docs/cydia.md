# Cydia pre-installed

ios3ds images come "pre-jailbroken" the way greenpois0n or redsn0w left an iOS 4.2.1
device: Cydia on the home screen, working `dpkg`/APT, and the base system Cydia
treats as essential. Code signing is already off in our boot-args
(`amfi_allow_any_signature=1 cs_enforcement_disable=1`), so nothing else is needed
for unsigned code to run.

The packages are Saurik's (Telesphoreo). **They are never committed and never in a
release zip.** `make sd` and the installer download them on your machine from the
original repository, check every file against a pinned sha256, and unpack them
into the rootfs image. Without network the image is built without Cydia (one
warning line), never fails.

## What gets installed

`rootfs/cydia.json` pins 35 packages: Cydia and the closure of its `Depends`/
`Pre-Depends`, plus every `Essential: yes` package, each at the newest version in
`http://apt.saurik.com/` `dists/tangelo-3.7` (identical to the `ios` dist; `.deb`
files redirect to `cache.saurik.com`). 6.5 MB download, about 40 MB unpacked
(the volume has ~400 MB free after it).

| Package | Version | | Package | Version |
|---|---|---|---|---|
| cydia | 1.1.30 | | dpkg | 1.18.10-12 |
| cydia-lproj | 1.1.12 | | apt7 / apt7-lib / apt7-key | 0.7.25.3-9 / -16 / -3 |
| base | 1-4 | | bash | 4.0.44-16 |
| coreutils / coreutils-bin | 8.12-13 / 8.12-9 | | debianutils | 3.3.3ubuntu1-1p |
| darwintools | 1-5 | | uikittools | 1.1.12 |
| system-cmds | 433.8-14 | | shell-cmds | 118-7 |
| firmware-sbin | 0-1 | | profile.d | 0-2 |
| sed, grep, findutils, diffutils | 4.1.5-8, 2.5.4-3, 4.2.33-7, 2.8.1-6 | | tar, gzip, bzip2, lzma | 1.29-10, 1.6-7, 1.0.5-8, 4.32.7-5 |
| gnupg, berkeleydb | 1.4.8-4, 4.6.21-5 | | ncurses, readline | 5.7-15, 6.0-8 |
| pam, pam-modules | 32.1-4, 36.1-5 | | org.thebigboss.repo.icons | 1.0 |
| mobilesubstrate (Cydia Substrate) | 0.9.6301 | | com.saurik.substrate.safemode | 0.9.6001 |
| com.skyglow.tlsfix (TLSFix, from GitHub, group `tlsfix`) | 1.3 | | | |

Why these versions: Cydia 1.1.30 (2017) is the last release and still has an
**armv6** slice with `LC_VERSION_MIN_IPHONEOS 2.0`; it links only libraries 4.2.1
has (libstdc++, WebKit/WebCore, SpringBoardServices, ...). Every Mach-O in the set
was checked to have an armv6 or generic-ARM slice. The 2011 package versions a
greenpois0n bootstrap installed are no longer all served, and an old Cydia would
immediately offer these upgrades anyway.

Also installed: **Cydia Substrate** 0.9.6301 (`mobilesubstrate`, the last
Telesphoreo release; armv6 slices, `LC_VERSION_MIN_IPHONEOS 2.0`) and its
**Substrate Safe Mode** 0.9.6001, and the **TLSFix** 1.3 tweak (TLS 1.2/1.3 for
iOS, [certificates.md](certificates.md) section 4) from
`github.com/ObscureMosquito/TLSFix/releases` (entry with its own `url` and
`"group": "tlsfix"`: left out with `TLSFIX=0` / `--no-tlsfix` / `IOS3DS_TLSFIX=0` /
recipe `"tlsfix": false`; if only its download fails, the image is built without
it). How Substrate gets into processes on
ios3ds is not how Substrate does it itself, see the next section.

## Cydia Substrate on 4.2.1 (re-log 71)

**What Substrate 0.9.6301 would do.** Its `extrainst_` (a Mach-O, not run here)
appends `bsexec .. /usr/bin/cynject 1 .../SubstrateLauncher.dylib` to
`/etc/launchd.conf` (or links `/etc/rc.d/substrate` for jailbreaks with an rc.d
untether). 4.2.1's `launchctl` reads `/etc/launchd.conf` at boot and waits for each
`bsexec`, so no untether is needed: `cynject` (`task_for_pid(1)`, allowed by our
boot-arg `amfi_unrestrict_task_for_pid=1`) loads `SubstrateLauncher.dylib` into
launchd, which hooks `posix_spawn` and adds `DYLD_INSERT_LIBRARIES=
/Library/MobileSubstrate/MobileSubstrate.dylib` to everything launchd starts.
**On 4.2.1 this hangs the boot:** `cynject` returns 0, launchd forks its ~38 jobs,
and every one of them blocks for good (QEMU: no daemon ever execs, CPU idle; the
children's last system calls are `kill`/`waitpid` from the hook's helper-process
path). Putting the same `DYLD_INSERT_LIBRARIES` into launchd's global environment
(`setenv` in `/etc/launchd.conf`, how pre-2011 MobileSubstrate worked) also hangs
the boot. Not debugged further (Substrate is closed source).

**What ios3ds does instead** (`rootfs/cydia.json` `substrate`, applied by
`cydia.substrate_setup` after the recipe):

| Process | How Substrate gets in | Our own library |
|---|---|---|
| SpringBoard | its launchd plist inserts `/usr/lib/ios3ds-applog.dylib`; applog's constructor `dlopen`s MobileSubstrate.dylib (as for apps, below) | ios3ds-applog |
| every app | SpringBoard inserts `/usr/lib/libgmalloc.dylib` (= ios3ds-applog, `SBGuardMallocAlways`) into each app, replacing anything else; ios3ds-applog's constructor `dlopen`s MobileSubstrate.dylib in executables inside a `.app` (after adding it to `DYLD_INSERT_LIBRARIES`, where Substrate's loader looks for and removes itself) | ios3ds-applog |
| ios3ds-nettest | `dlopen` after it loads CFNetwork (TLSFix filters on CFNetwork) | |
| configd, securityd, other daemons | none (nothing targets them; it would only cost memory) | configd: Wi-Fi bridge interposer; securityd: AES interposer (unchanged) |
| **sandboxed apps** (MobileSafari with its built-in `MobileSafari` profile, App Store apps) | **the `/usr/lib` copies**: the sandbox refuses every file under `/Library`, so applog's `dlopen` of `MobileSubstrate.dylib` fails and it falls back to `/usr/lib/ios3ds-CydiaSubstrate.dylib` + `/usr/lib/ios3ds-tlsfix.dylib` (TLSFix only, loaded by a timer once CFNetwork is up; re-log 104) | ios3ds-applog (it is in `/usr/lib`, which the profile allows) |

No launchd plist names `MobileSubstrate.dylib` (`substrate.daemons` is empty;
`cydia.substrate_setup` can still append it to listed recipe jobs): a
(re)installed `mobilesubstrate`'s `extrainst_` removes that entry from the
LaunchDaemons plists, which in QEMU left SpringBoard without Substrate after a
reboot. Loading it from applog survives that. Without Cydia (`CYDIA=0`) nothing
changes.

Inside a process, `MobileSubstrate.dylib` (= `SubstrateInjection.dylib`) loads
`SubstrateLoader.dylib` only if CoreFoundation is loaded, and the loader then loads
each `/Library/MobileSubstrate/DynamicLibraries/*.dylib` whose plist filter
matches (Bundles / Executables / Classes / CoreFoundationVersion). Installed
extensions: `MobileSafety` (safe mode, SpringBoard) and `tlsfix` (any process with
`com.apple.CFNetwork`; TLSFix itself skips SpringBoard, launchd, lockdownd and
securityd).

## Reaching sandboxed apps (Safari): /usr/lib copies (re-log 104)

A sandboxed process (MobileSafari, App Store apps, Mail's helpers) runs under a
Seatbelt profile that denies `open()` of everything under `/Library` and
`/var/mobile/Library/ConfigurationProfiles`, but allows `/usr/lib` (that is where
`ios3ds-applog` and our other interposers load from). So the build puts what
TLSFix needs there, next to the dylibs from the pinned debs:

| Copy under `/usr/lib` | From |
|---|---|
| `ios3ds-CydiaSubstrate.dylib` | `/Library/Frameworks/CydiaSubstrate.framework/CydiaSubstrate` |
| `ios3ds-tlsfix.dylib` | `/Library/MobileSubstrate/DynamicLibraries/tlsfix.dylib`, with two strings rewritten: its CydiaSubstrate `LC_LOAD_DYLIB` and the `ConfigurationProfiles` directory path (same-length NUL padding inside the load command/string) |
| `ConfigurationProfiles/ios3ds-roots.plist` | the CA payloads TLSFix's EC-chain check reads |

`ios3ds-applog`'s `load_substrate()` first tries the `/Library` Substrate as
before (SpringBoard and unsandboxed apps are unchanged). If that `dlopen` fails
— a sandboxed app — it loads the `/usr/lib` TLSFix copy from a run-loop timer
once CFNetwork is up (an app's constructors run before its frameworks), so
TLSFix's hooks are installed before the first connection. This needs no kernel
patch and no wider sandbox; it costs about 2.5-3 MiB of resident memory in
MobileSafari (`--icount 2`: 8.2 MiB after a plain page, 10.7-11.7 MiB with TLSFix
after an HTTPS page, 38-41 MiB still free of 128 MiB) and the old out-of-memory
failure did not reappear (re-log 104).

**The old sb_evaluate patch remains an opt-in** (`kernel/patches-sandbox.json`,
`SANDBOX=1` / `--sandbox-open` / `IOS3DS_SANDBOX_OPEN=1`; re-log 72): it replaces
the Sandbox kext's `sb_evaluate` first instruction (`com.apple.security.sandbox`
0x803cd000, Sandbox-58.12, 0x803ced98) with a branch to `shim_sb_evaluate`, which
allows the file-read operations (op 1, 6-9, 11) and runs the real evaluator for
everything else. It is no longer needed for TLSFix: in a QEMU run with it applied,
MobileSafari's probe still got `EPERM` opening `/Library/MobileSubstrate/...`
(even with a temporary shim that allowed every operation), so the vnode-open
check evidently does not reach that function in this build. It stays as an
experiment for other tweaks, off by default.

## Installing tweaks: Substrate stays as it is (re-log 73)

Installing a tweak from Cydia must never set up Substrate's launchd injection,
which hangs 4.2.1 (on builds without the pre-installed Substrate, installing
anything that depends on `mobilesubstrate` hard-locked the device: Cydia installs
it and runs its `extrainst_`, which calls `cynject 1` at once and writes it into
`/etc/launchd.conf`). The build therefore:

- **holds** `mobilesubstrate` and `com.saurik.substrate.safemode` (`Status: hold ok
  installed`, manifest `hold`) and pins their versions in `/etc/apt/preferences`
  (priority 1001): Cydia's Upgrade and installs of dependent tweaks keep the
  pre-installed 0.9.6301, which satisfies `Depends: mobilesubstrate` including
  versioned ones such as `(>= 0.9.3997)`;
- **diverts** `CydiaSubstrate.framework/Commands/cynject` (a local dpkg diversion
  in `/var/lib/dpkg/diversions`): the real binary is `cynject.real`, and in its
  place a guard (`/usr/libexec/ios3ds-cynject`) refuses pid 1 and passes every
  other use (e.g. cycript) to the real one. A reinstalled or newer
  `mobilesubstrate` unpacks its cynject to `cynject.real` as well, so its
  `extrainst_` (which injects only through `system("/usr/bin/cynject 1 ...")`)
  and any `bsexec .. cynject 1` line in `/etc/launchd.conf` do nothing;
- runs **`com.ios3ds.substrate-guard`** once at every boot
  (`/usr/libexec/ios3ds-substrate-guard`): removes cynject lines from
  `/etc/launchd.conf` and `/etc/rc.d/substrate`, and puts the guard back if
  something replaced cynject without the diversion. The build itself drops any
  cynject line from `launchd.conf` too (a fresh image has none).

**Safe mode:** MobileSafety (filter `com.apple.springboard`) and the loader's
crash handlers load in SpringBoard as on a jailbroken device, so a tweak that
crashes SpringBoard should bring it back in safe mode; not exercised in QEMU.
Our interposers are plain `DYLD_INSERT_LIBRARIES` entries, not extensions, so
they load in safe mode too.

**ios3ds safe mode (issue 1).** Every Substrate load on ios3ds goes through
`ios3ds-applog`'s `load_substrate()`, which now asks `substrate_allowed()`
first:

- boot-arg `ios3ds_safe=1` (boot.cfg `safe_mode=1`, or **L** held while the
  ARM9 loads iOS; the ARM9 logs `safe mode (...)`): no tweaks in SpringBoard or
  any app for that boot (`[safe] boot-arg ios3ds_safe=1 ...` on the console);
- automatic: SpringBoard counts its starts in
  `/var/mobile/Library/Caches/ios3ds-springboard-starts` and clears the count
  after 90 s of running. After two starts in a row that did not get that far,
  the next start loads no tweaks and creates `ios3ds-substrate-safe` next to
  it, which keeps apps tweak-free too. After 90 s of a stable safe start the
  flag goes and the next respring tries the tweaks again.

What it cannot catch: a boot that hangs before SpringBoard starts (launchd or
a daemon). Tweak daemons that Cydia installs (e.g. `sbsettingsd`,
`rocketd`) are ordinary launchd jobs and are not affected by safe mode.

## What the build does (scripts/cydia.py)

Hooked into `scripts/mkrootfs.py` right after the volume is grown and before our own
files are written (so the recipe always wins over a deb):

1. **Fetch**: for each pinned entry, `mirrors[i] + path` -> `build/cydia/` (installer:
   `<work>/cydia/`, deleted afterwards). A cached file whose sha256 matches is reused.
   Any failure (offline, 404, wrong size/hash) -> `[cydia] WARNING ... building
   WITHOUT Cydia`, and the image is built as before.
2. **Unpack** each `data.tar.{lzma,gz,bz2,xz}` with xpwn's `hfsplus`: directories
   (`mkdir`, mode), files (`add` + `chmod`, keeping setuid bits such as
   `cydo` 6755), symlinks (`symlink`). Owners are inherited from the parent folder,
   which gives root for system paths and `mobile` (501) under `/var/mobile`, as
   dpkg would. Paths with `..` are refused. `/var`, `/etc`, `/tmp` are written
   through `/private/...`.
3. **Maintainer scripts** are not run (they are iOS binaries and shell scripts);
   their effects that matter are reproduced:
   - `firmware-sbin` ships empty placeholders for Apple's `/sbin/fsck`, `mount`, ...
     (its pre/extrainst keep the stock files): the stock files are kept
     (`keep_stock`).
   - Cydia's postinst: `/var/mobile/Library/Cydia`,
     `/var/mobile/Library/Caches/com.saurik.Cydia` (mobile-owned).
   - Cydia's `firmware.sh`: the virtual packages `firmware` 4.2.1, `cy+os.ios`,
     `cy+cpu.arm`, `cy+model.ipod` 2.1, `cy+kernel.darwin` 10.4.0,
     `cy+lib.corefoundation` 550.52, and `/var/lib/cydia/firmware.ver` = 6. (It still
     runs at each boot from `com.saurik.Cydia.Startup` and re-derives them.)
   - **No stashing**: `/.cydia_no_stash`. Stashing moves `/Applications`,
     `/usr/share`, ... to `/var/stash` because a real iPod's system partition is
     small; our rootfs is one 1 GiB volume, so it would only cost time and add
     symlinks. `pam`'s preinst (`move.sh /usr/lib/pam`) is skipped for the same reason.
   - `uikittools`' extrainst rebuilds SpringBoard's app cache; the image has no
     cache yet, SpringBoard builds it (with Cydia) on first boot.
4. **dpkg database**: `/var/lib/dpkg/status` (each control + `Status: install ok
   installed`, `Conffiles:` with md5s, sorted), `info/<pkg>.list` (`/.` + every path
   in the deb), `info/<pkg>.{md5sums,conffiles,preinst,postinst,prerm,postrm,
   extrainst_}`, empty `available`, `updates/`, `triggers/`; APT's
   `/var/lib/apt/lists/partial` and `/var/cache/apt/archives/partial`.

Sources: none are written. On its first launch Cydia sets up its default list
itself (`/var/mobile/Library/Caches/com.saurik.Cydia/sources.list`, with
`/etc/apt/sources.list.d/cydia.list` symlinked to it): `apt.saurik.com ios/550.52`,
BigBoss, ModMyi, ZodTTD and, for iOS 4, `repo666.ultrasn0w.com`, exactly as on a
2011 device. ModMyi, ZodTTD and repo666 no longer exist, so a refresh reports
errors for them; remove them in Sources > Edit.

## iPhone OS 2.x: Cydia 1.0.3222 (rootfs/cydia-2.x.json)

`rootfs/overlay-2.2.1.json` points `"cydia"` at its own manifest,
`rootfs/cydia-2.x.json`; 3.1.3, 4.1 and 4.2.1 keep `rootfs/cydia.json`.

- **Set:** Cydia 1.0.3222-73 (the last 1.0.x without `firmware (>= 4.0)`,
  `DTSDKName iphoneos2.0.saurik`, `MinimumOSVersion 2.0`, armv6, setuid-root
  `Cydia_` behind a bash `Cydia` wrapper) plus the closure of its
  Depends/Pre-Depends, `essential` and the repo's Essential `base`/`debianutils` (29 packages, newest versions on
  apt.saurik.com: apt7-lib/apt7-key 0.7.25.3, dpkg 1.18.10, apr-lib, pcre,
  uikittools 1.1.12, ...). No Substrate, TLSFix, cydia-lproj or BigBoss icons.
  Every Mach-O in it has no `LC_DYLD_INFO(_ONLY)` and all of its non-weak
  imports resolve in 5H11a's own dylibs/frameworks or in the set (only lazy
  `___floatundidf`/`___floatundisf`, missing from 2.2.1's libgcc_s, in a few
  size-printing paths of coreutils/tar/find/sysctl).
- **Own cache:** the manifest's `"cache": "cydia-2.x"` keeps its debs in
  `build/cydia-2.x`, never in the 4.x set's `build/cydia`.
- **Stash check patched out:** Cydia 1.0.x "Reorganizes" (moves
  `/Applications`, `/usr/share`, `/usr/libexec`, ... to `/var/stash`, then stops
  SpringBoard) whenever one of them is a real directory; it has no
  `/.cydia_no_stash` check. On our one-volume rootfs that never finished and
  every launch ended on the home screen. The manifest's `"patch"` list
  (`scripts/cydia.py patch_file`) turns the seven `bne` after its `readlink()`
  calls into `b` while the deb is unpacked; the original bytes are checked.
- **Preset role and sources:** `/var/lib/cydia/metadata.plist` sets
  `Settings/Role = User`, so the first launch opens the home tab instead of
  "Who Are You?" (Manage > Settings changes it);
  `/etc/apt/sources.list.d/saurik.list` lists Telesphoreo (`tangelo-3.7`),
  which 1.0.x does not add itself. `/etc/apt/preferences` pins `cydia` to
  1.0.3222-73 (priority 1001), so the repo's 1.1.30 (which never launched on
  2.2.1, re-log 131) is not offered; a dpkg hold instead made Cydia's refresh
  fail with "pkgProblemResolver::Resolve generated breaks". `base` and
  `debianutils` are in the set because they are the repo's other Essential
  packages.
- **Home page:** with Wi-Fi, Cydia loads saurik's live home page for this
  version, which says "This version of Cydia is no longer supported: please
  upgrade to 1.1.x". That is the server's text for 1.0.x clients.

## Turning it off

- `make sd CYDIA=0` / `make sdimg CYDIA=0` (passes `--no-cydia` to `scripts/mksd.py`)
- `python3 install.py --no-cydia IPSW SD`
- `IOS3DS_CYDIA=0` in the environment (any build path)
- `"cydia": false` in `rootfs/recipe.json`

To remove it from a built image, rebuild with one of the above. To prefetch
(e.g. before going offline): `python3 scripts/cydia.py fetch`.

## Updating the pins

Edit `version`/`path` in `rootfs/cydia.json` (or `rootfs/cydia-2.x.json`, with
`--manifest`), then `python3 scripts/cydia.py pin`
re-downloads each listed deb and rewrites `size` and `sha256`. Check new Mach-Os
still carry an armv6 (or generic ARM) slice.

## Verified in QEMU (re-log 60)

- Build: the hook adds 4-8 s to `mkrootfs` (978 files, ~1460 `hfsplus` calls);
  32 debs + 6 virtual packages in `status`.
- Boot to SpringBoard as before; the Cydia icon is on the first home screen page (re-log 77).
- Cydia launches straight into its UI: no "Preparing Filesystem", no reboot
  request. Installed shows the 32 packages; on first launch Cydia writes its default
  sources.
- At boot, `com.saurik.Cydia.Startup` runs `firmware.sh` in the guest (bash, sed,
  coreutils, gssc): `status` gains the `gsc.*` capability packages.
- With Wi-Fi (`ctr-wifi` model + slirp): Cydia's refresh downloads the
  Telesphoreo (`ios/550.52`) and BigBoss package lists, the Telesphoreo sections list
  appears, and installing `cpio` through Cydia (APT download + dpkg in the guest)
  leaves `cpio 2.9-2p` "install ok installed" with `/usr/bin/cpio`.
- `emu/soak.py --cycles 10` on the image after all of the above.

## Refresh and install time (re-log 105)

A tester on a New 2DS XL (public issue #1) reported that adding a repository
and installing a tweak take "a really long time". QEMU with `--icount 2` can
separate the parts; the local-repo harness `emu/cydia_perf.py` builds one small
`.deb` itself (32 files, gzip, `iphoneos-arm`), serves `Release`/`Packages.gz`
and the `.deb` from the host over slirp (`http://10.0.2.2:<random port>/repo`),
and puts a temporary launch daemon in the image that times apt and dpkg with
`[cperf] ... start/done` lines in `sd:/ios3ds/log.txt`. No third-party packages
or internet hosts are used; the source is added before the boot, so "refresh"
below is apt's backend work, not Cydia's URL-entry UI.

The harness also reports each phase's guest CPU (`bash time`, `[cperf] ...
cpu real= user= sys=`), can synthesize a larger index (`--fill N`), serve
`Packages.bz2` as well (`--bz2`), add a second (dead) source
(`--extra-source`), override apt config (`--apt-extra`, installed as
`99zzios3ds-perf`) and skip the refresh to isolate `dpkg -i`
(`--skip-refresh`). Run it on a fresh image (the script's docstring has the
commands).

### First measurement

Guest seconds from the ARM9's `log_ts=1` stamps, same image and same `.deb`
except for the fixes; host was otherwise idle:

| Phase | before (`origin/main`) | after | change |
|---|---:|---:|---:|
| add source + `apt-get update` | 1.797 s | 1.747 s | −0.050 s |
| download the one `.deb` | 0.173 s | 0.078 s | −0.095 s |
| `dpkg -i` that `.deb` | 2.493 s | 2.423 s | −0.070 s |
| **total** | **4.463 s** | **4.248 s** | **−0.215 s (−4.8 %)** |

All runs returned rc=0; no md0 errors, panic or ARM9 exception. During
`dpkg -i` the `[io]` lines went from 618 md0 chunks / 316 ms ARM9 service time
to 556 chunks / 288 ms with the same ~600 KiB written: the 64 KiB bounce
removes about one mailbox round trip in ten, and the write-back cache batches
the small writes instead of calling `f_sync` once per dpkg file.

A later re-run of the same A/B on a loaded host shows how much the guest wall
clock moves with host load (same harness, fresh images, `fill=0`):

| Phase | before (base 8fe190d) | after (this branch) | guest CPU before → after |
|---|---:|---:|---:|
| `apt-get update` | 2.993 s | 2.443 s | 0.72 → 0.67 s |
| download | 0.132 s | 0.277 s | 0.07 → 0.10 s |
| `dpkg -i` | 3.438 s | 2.664 s | 0.67 → 0.58 s |
| **total** | **6.563 s** | **5.384 s** | **1.46 → 1.35 s** |

QEMU's SD card is instant, so the remaining seconds are mostly apt/dpkg CPU on
the emulated ARM11 plus emulator scheduling. On a real card the saved
`f_sync` commands and smaller request count matter much more
([boot-perf.md](boot-perf.md) §5, §8). Compare chunks/KiB/ARM9 time, not a
single wall-clock run.

### Where the time goes

- **apt's index work is CPU, not network.** With a synthetic 2,000-package
  `Packages` (440 KB; still built locally), `apt-get update` takes 3.8–5.1 s
  and 1.3–1.6 s of guest CPU. Most of the rest is blocked time in md0 reads
  (apt paging in its libraries and writing `/var/lib/apt/lists`) and QEMU
  scheduling.
- **Compression matters.** apt 0.7 prefers bzip2 over gzip when a source
  offers both. Decompressing the same index as bz2 took 5.121 s / 1.60 s CPU;
  with gzip it was 3.762–4.257 s / 1.30–1.43 s CPU. Images now order gzip
  first (`Acquire::CompressionTypes::Order`).
- **The dead default sources are the refresh outlier.** Cydia 1.1.30 still
  writes its five 2011 sources on first launch; ModMyi, ZodTTD and
  repo666.ultrasn0w.com are gone. Measured with one dead source
  (`cydia.zodttd.com`) added to the local repo's list: refresh 2.544 s
  without a fix (apt followed a redirect and retried) vs **1.490 s** when
  `/private/etc/hosts` points the host at 127.0.0.1, so the connect is refused
  at once. `apt.modmyi.com` already fails through DNS here but not on every
  network; the hosts entry removes that variable. The 10 s
  `Acquire::http::Timeout`/`Retries 0` stay as a backstop, and the sources are
  still listed until the user removes them in Sources > Edit.
- **`Dir::Cache::pkgcache` off is not a win:** it makes refresh look cheaper
  by skipping the binary cache, but then `apt-get --download-only` and the
  install re-parse the text lists (download phase CPU 0.07 → 0.47 s), so it is
  not shipped.
- **HTTPS handshakes** are software work in TLSFix's OpenSSL
  ([certificates.md](certificates.md) §4 measures 2–9 s per Cydia fetch) and
  are not removed by an apt option. **Substrate re-signing is not a cost:**
  code signing is disabled in our boot-args and dpkg only unpacks the files;
  nothing in our build re-signs a tweak.

### What changed

- `kernel/shim/blk.c` + `boot/common/ios3ds.h`: 127-sector (63.5 KiB) bounce
  in the free FCRAM after the XNU log ring instead of 32 KiB.
- `boot/arm9/blksrv.c` + `blkcache.c/h`: 256 KiB write-back cache, deferred
  `f_sync` (500 ms idle / 2 s cap), flush-before-clean-flag, O(1) dirty count
  and a one-`f_read` fast path when nothing is dirty.
- `rootfs/cydia.json`: `/private/etc/hosts` fail-fast entries, gzip index
  order, 10 s timeouts, no retries, no translations
  (`/private/etc/apt/apt.conf.d/99ios3ds-speed`).
- `emu/cydia_perf.py`: the measurement harness above; `tests/test_blkcache.c`
  covers eviction, flush, invalidation and error retention.

**Verification:** `make test` passes (including the cache test, the CI stub
symbol check and the conflict-marker check) and a fresh-image
`python3 emu/soak.py --cycles 5 --icount 2` printed **Result: PASS** after the
rebase onto `origin/main`. Re-checked on a freshly rebuilt image from the
final tree (`make userland`, `rm -f build/sd.img && make sdimg`): the local
repo harness gave 1.645 s for refresh, 0.385 s for the download and 2.618 s
for `dpkg -i` (4.648 s total, rc=0, no md0 or blk errors; guest CPU
0.23/0.05/0.19 s user + 0.44/0.04/0.40 s sys), and another fresh-image
5-cycle soak printed **Result: PASS**. These wall times are between the two
A/B runs above on the same loaded host; the comparison columns remain the
stable result.

## Known issues

- **HTTPS:** the current root store ([certificates.md](certificates.md)) and
  TLSFix (TLS 1.2/1.3, EC chains) inside Cydia: Cydia's home page loads; HTTPS
  depictions/repositories on TLS 1.2-only servers go through the same CFNetwork
  path that ios3ds-nettest tested (not tried in the Cydia UI). BigBoss's HTTPS host still
  fails: its certificate expired on 2026-06-15 (its repository is plain HTTP).
- Dead default sources (ModMyi, ZodTTD, repo666) show errors on refresh; they now fail at once (`/private/etc/hosts`).
- The startup daemon re-runs `firmware.sh` on every boot (stock behaviour); it
  is a few seconds of background shell work.
- Substrate is not in launchd daemons (only in SpringBoard and apps): tweaks
  for daemons don't load.
