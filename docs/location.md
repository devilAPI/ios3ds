# Fake GPS location (emulator-tested)

The iPod touch 2G has no GPS chip, its Wi-Fi positioning database is long gone,
and ios3ds disables `locationd` (`rootfs/recipe.json`). Without help, every
CoreLocation client sees location services off: Maps shows a grey dot and
Safari's `navigator.geolocation` gets no fix. This feature makes iOS report a
location the user chooses. It is **off unless configured**.

## Why hook the apps, not locationd

| Option | Verdict |
|---|---|
| Feed a location provider to `locationd` | iOS 4.2.1 has no public simulation provider; `locationd` is disabled in our rootfs, and injecting into launchd-managed daemons hangs the boot ([cydia.md](cydia.md), re-log 71/73). No daemon injection is used here. |
| Substrate tweak in every app | Substrate itself still cannot load from `/Library` in sandboxed MobileSafari ([cydia.md](cydia.md), re-log 71/72); TLSFix now does through `/usr/lib` copies (re-log 104), but a general tweak loader is not what this needs. |
| Our own dylib, inserted into SpringBoard and every app | **Chosen on 4.x.** `ios3ds-applog.dylib` is already inserted into SpringBoard (launchd plist) and into every app via `libgmalloc.dylib` → applog ([cydia.md](cydia.md)); it lives in `/usr/lib`, which the Safari sandbox allows. Sandboxed apps get the hooks too, with no kernel sandbox patch. On 3.x/2.x the `SBGuardMallocAlways` switch does not exist, so Maps and MobileSafari get a real `LC_LOAD_DYLIB` for applog in their executables instead (`scripts/inject_dylib.py`); a launchd-wide `DYLD_INSERT_LIBRARIES` hangs those boots. |

The hooks are installed on `-locationServicesEnabled` / `+authorizationStatus`
(report on/authorised while a location is set), on `-[CLLocationManager
location]`, `startUpdatingLocation` and `stopUpdatingLocation`. A
`CFRunLoopTimer` on the main run loop calls the delegate's
`locationManager:didUpdateToLocation:fromLocation:` about once a second with a
real `CLLocation` (timestamp = now, horizontal/vertical accuracy = configured
value, speed and course = -1 = invalid). Apps that only poll `-location` get
the same object. When the location is turned off, every hook calls the original
implementation, so behaviour is stock again.

## Setting it

**At boot** — `sd:/ios3ds/boot.cfg`:

```cfg
gps=52.5200,13.4050        # latitude,longitude[,accuracy in metres]
```

The ARM9 copies this into the XNU boot-arg `ios3ds_gps=...` exactly as it does
`diag=1` → `ios3ds_diag=1`. Empty/missing means off; bad values are ignored by
the iOS side. Latitude must be -90..90, longitude -180..180, accuracy > 0
(default 50 m).

**At runtime** — from Cydia's terminal or SSH (the tool is installed as
`/usr/bin/ios3ds-gps`):

```sh
ios3ds-gps set LAT LON [ACC]   # e.g. ios3ds-gps set 40.7128 -74.0060 20
ios3ds-gps show                # print the current setting
ios3ds-gps off                 # back to no fake location
```

It validates the same ranges, writes
`/var/mobile/Library/Preferences/com.ios3ds.location.plist` (`Enabled`,
`Latitude`, `Longitude`, `Accuracy`) and posts the Darwin notification
`com.ios3ds.location.changed`. A copy of the plist goes to
`/private/var/tmp/com.ios3ds.location.plist` for sandboxed apps; applog reads
the two files directly (on iOS 4 `CFPreferencesCopyAppValue` caches a domain
per process and does not reliably pick up a notification-time rewrite).
Running apps reload on the notification; newly started apps read the file at
dylib load. The plist wins over the boot key, so `off` stays off until the next
install (which resets the iOS disk) or a new `set`.

`ios3ds_gpslog=1` in the boot.cfg `cmdline` adds `[gps] ...` lines to the log
for bring-up; it is not needed for the feature.

## Testing

- `make test` runs `tools/ios3ds-gps/tests/test_gps.c`: bounds, defaults,
  non-finite values, whitespace and malformed input.
- QEMU (`emu/session.py start --icount 2`, boot.cfg `gps=52.5200,13.4050`,
  `ios3ds_gpslog=1`): the gated `com.ios3ds.gpstest` job runs
  `/usr/libexec/ios3ds-gpstest`, a tiny CLLocationManager client. It prints
  `services=1 authorization=3`, a `delegate:` update and a `location:` every
  second at 52.52,13.405 / 50 m, then calls `ios3ds-gps set 40.7128 -74.0060
  20` at tick 6: `[gps] Darwin notification received` and the following
  `delegate:`/`location:` lines show 40.7128,-74.006 / 20 m. Details: re-log
  section 83.
- `python3 emu/soak.py --cycles 5 --icount 2` on a fresh image with no `gps=`
  key: PASS, and no `[gps] on` line appears (hooks inactive).
