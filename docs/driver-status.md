# Driver status and stability (agent/drvstab)

Inventory of the drivers this work covers: where the code is, how it is
tested, what is still open, and a stability verdict. The other drivers
(Wi-Fi, clean shutdown, ambient light, stereo 3D, multi-version build) are
owned by other workers and are not assessed here.

Legend for **tested**: *host* = `make test` / a host test; *QEMU* = the
`nintendo3ds` machine; *hardware* = a console run whose log is in
`docs/hwlogs/` or in the re-log entry named.

| Driver | Code | How it is tested | Known issues / open points | Stability verdict |
|---|---|---|---|---|
| GPU / PICA200 service and screen copy | `kernel/shim/pica.c`, `kernel/shim/gpu_glue.c`, `tools/ios3ds-pica/*`, `boot/arm9/gpumirror.c`, `boot/arm9/display.c` | host (`test_pica`, `test_mirror`); QEMU (`gltest`, soak, `--icount 2`); hardware runs 12–16, 21, 25, 29, New 2DS XL run 1 | The screen copy is confirmed on hardware (run 14: GPU copies at 22–26 fps, spot checks 0 of 512 sampled LCD pixels differ) and the mirror self-test matched all 518,400 bytes on the New 2DS XL; CA on the GPU, scatter surfaces and the GPU present check are QEMU-verified; the `PRESENT`/`SCATTER` shim paths have not run on a console since the scatter change (re-log 79). Any other refused surface still falls back to CPU read-back for the whole process (by design). | Stable. QEMU soak PASS; the hardware paths use the same code with a CPU read-back fallback. |
| Core Animation back end | `tools/ios3ds-gl/{ca,pica_be,iface}.c` | host (`make -C tools/ios3ds-gl host-test`); QEMU (`emu/soak.py`); hardware runs 15/16/21/25/29 | Per-frame cost on hardware is higher than in QEMU; the CA scene/self checks are the only on-device validation. Engine state lives per process; `pica_forget`/eviction is host-tested. | Stable; CPU rasterizer and CA software fallback remain available. |
| Touch, buttons, circle pad, C-stick | `boot/arm9/tsc.c`, `boot/arm9/exthid.c`, `boot/common/input_proto.h`, `tools/ios3ds-input/shim/input.c`, `tools/ios3ds-input/daemon/*`, `kernel/shim/blk.c` (registration) | host (`test_cpad`, `test_n3ds`); QEMU (soak, session drags); hardware runs 8–14 (touch, HOME, volume), New 3DS features untested | A cached PurpleSystemEventPort send right was leaked on every app switch and could stay a dead name across an app relaunch (fixed here). The frontmost id also lagged Home, so a touch right after Home went to the closing app: the daemon now uses `SBSCopyFrontmostApplicationDisplayIdentifier` and a 1.5 s SpringBoard grace after Menu (fixed here; QEMU-only panic on app-targeted drags is documented in re-log 85). C-stick axes/range/ZR bit and the circle-pad raw range are still hardware-unverified. | Stable in QEMU; the leak and stale-target fixes remove the two unbounded/lost-touch modes in this daemon. |
| Accelerometer | `boot/arm9/mcu.c` (MCU regs), `tools/ios3ds-mcu/shim/mcu.c` (LIS302DL I2C), `tools/ios3ds-mcu/daemon/mcud.c` (old route) | host (`test_mcu`); QEMU (soak + `mcu-orient`); hardware run 14 (orientation 0→6) and Old 3DS XL issue #7 fix | Axis map for all poses is only spot-checked on hardware; the old client-side `acc` route is superseded by the kernel driver. | Stable; the kernel driver is the shipping path and has a QEMU model. |
| Gyroscope → CoreMotion | `boot/arm9/gyro.c`, `tools/ios3ds-mcu/dt_mcu.py`, `tools/ios3ds-mcu/shim/mcu.c`, `tools/ios3ds-mcu/gyrotest` | host (`test_gyro`); QEMU (`--icount 2`, diag gyrotest); hardware not tested | Axes, range, chip address variants and New 3DS behaviour are assumptions; the chip sleeps until CoreMotion asks. | QEMU-stable; hardware-unverified. A one-line `[gyro]`/`[gyrotest]` log from a console settles it. |
| Audio out (speaker + headphones) | `boot/arm9/audio.c`, `kernel/shim/audio.c`, `tools/ios3ds-audio/*`, QEMU `ctr_csnd.c` | host (`tests/test_audio.py` + `tools/ios3ds-audio/tests`); QEMU (tone, soak, `headphones` QOM); hardware run 32 (speaker confirmed), runs 14/16 (streams) | Headphones: iOS switches route and the CODEC path was reworked (`hp_codec=1`), but sound in the headphones is still unconfirmed on hardware (docs/audio.md §4/§8). The 3DS volume slider is not read by the CODEC. | Speaker path stable and hardware-confirmed; headphone path QEMU-clean, hardware pending. |
| Microphone | `kernel/shim/audio.c` (`mic_*`), `boot/arm9/audio.c` (`mic_poll`), QEMU `ctr_mic.c`, `ios3ds-tone rec` | host (mic FIFO / interpolation / odd-stream tests); QEMU (`IOS3DS_MIC=...`, Voice Memos ALAC); hardware not tested | MIC_CNT bit layout, CODEC ADC power sequence and the 1 ms drain margin are unverified on a console; Voice Memos now records ALAC because the AAC encoder is absent. | QEMU-stable (0 overruns); hardware-unverified. |
| Cameras | `boot/arm9/camera.c`, `kernel/shim/cam.c`, `tools/ios3ds-camera/{vcd,analyze_dump.py}`, QEMU `ctr_cam.c` | host (`tests/test_camera.py`, `test_cam.c`); QEMU Camera.app preview/photo, frame dumps; hardware runs 23, 25, 29, 30, New 2DS XL | Hardware run 29 moved data (~12 fps, 0 flagged overruns) but the preview showed frame-window drift; run 30/31 added blank-aligned publishing, IOSurface re-upload and still diagnostics. Colour order, mirroring and a 1 ms CPU drain on the console remain to be re-checked. | QEMU-stable through long start/stop cycles; on hardware the fallback is a still photo/preview, never a kernel hang. |
| MCU daemon (battery, volume, brightness) | `tools/ios3ds-mcu/daemon/mcud.c`, `tools/ios3ds-mcu/shim/mcu.c`, `boot/arm9/mcu.c` | host (`test_mcu`); QEMU (soak, `mcu-*` QOM properties); hardware run 14 | Brightness and the accelerometer go through SpringBoard's HID event system. A SpringBoard restart used to leave `ios3ds-mcud` holding a dead client forever; it now reconnects (fixed here). RTC offset handling was fixed in run 14/re-log 34. | Stable in QEMU; reconnect fix covers the remaining failure mode I found. |
| SD block I/O (read path) | `boot/arm9/blksrv.c`, `boot/arm9/sdmmc.c`, `boot/arm9/diskio.c`, `kernel/shim/blk.c` | host (carry-over/rootfs tests use the same FatFs data); QEMU soak (`[hb] … 0 err`); hardware runs 8–16, 29 | Write/clean-shutdown behaviour belongs to the shutdown worker and is deliberately untouched. Reads are synchronous; a dead ARM9/SD leaves the ARM11 waiting (no timeout), which only a hardware fault can cause. | Stable on hardware (run 29: 6648 requests / 118,846 KiB / 0 errors; the New 2DS XL run: 18,105 requests, 0 errors) and in soak runs. |

## What this work changed

- `tools/ios3ds-input/daemon/input.c`: the cached frontmost-app Mach send
  right is now released when the app changes and re-resolved when it has
  become a dead name (an app relaunched under the same bundle id). The
  frontmost identifier comes from `SBSCopyFrontmostApplicationDisplayIdentifier`
  (the non-Copy call can keep returning the app that just went away; NULL
  means SpringBoard), is always NUL-terminated, and touches go to the system
  port for 1.5 s after Menu so a tap right after Home cannot reach the
  closing app.
- `tools/ios3ds-mcu/daemon/mcud.c`: detects a dead SpringBoard HID client
  after a SpringBoard restart and reconnects; the `NoIdleSleepAssertion` and
  its two CFStrings are created once, not once per reconnect.
- `emu/stress.py`: repeatable driver stress (app open/close, touch drags,
  D-pad page swipes, circle-pad scroll, camera start/stop, headphone
  plug/unplug, screen rotation, lock/unlock, optional `[mem]`/`[smp]`
  reports) with a PASS/FAIL report. It also checks the *real* GPU-mirror log
  lines (`[gpu] display mirror: self-test FAILED|aborted`, `... did not
  finish`) instead of the old `[pica] TIMEOUT`/`display mirror FAILED`
  strings, which no code writes, and counts headphone changes from the
  `[audio] headphones ...` lines, not a `[jack]` prefix that does not exist.
  The continuous drags run on SpringBoard; dragging inside the opened app is
  behind `--app-drag` because an app-targeted GSEvent move makes the QEMU
  model panic ("undefined kernel instruction", r11 = the app's shared-cache
  address) while the same path works on hardware (re-log 78). A cycle longer
  than iOS' auto-lock is recovered with Home + slide-to-unlock.
- `emu/soak.py`: the same real GPU-mirror strings, plus camera init/FIFO
  overrun and audio DMA/CODEC error checks, so a long soak fails on those.
- **QEMU:** `make test` green; `emu/soak.py --cycles 5 --icount 2` on a fresh
  image PASSES on the Old 3DS (boot 100 s, no recoveries, 0 panics / stalls /
  crashes / md0 errors, 0 GPU fallback/timeouts) and on the New 3DS model
  (boot 100 s, same zero-error checks).
- docs: this inventory, the stress harness in `emulator.md`, and the two
  daemon behaviours in `input-design.md` (§4.2) and `mcu.md` (§5).

## How to re-run the evidence

```sh
make userland
rm -f build/sd.img && make sdimg
python3 emu/stress.py --cycles 5 --icount 2 --diag   # apps, input, camera, jack, rotation, locks
python3 emu/soak.py --cycles 5 --icount 2            # long soak, Old 3DS
rm -f build/sd.img && make sdimg
python3 emu/soak.py --cycles 5 --icount 2 --new3ds   # long soak, New 3DS model
```

Both scripts write their screenshots, the final SD log and
`report.md`/`report.json` under `build/emu/`. Every run that lets QEMU write
the image needs a fresh `build/sd.img`; a killed QEMU can corrupt it.

## What only a console can settle (and the log to capture)

| Area | Capture | Read |
|---|---|---|
| Headphones | `sd:/ios3ds/log.txt`, plug/unplug with music playing | `[audio] headphones plugged in`, `[audio] iOS route -> headphones`, `[audio] CODEC headphones:` page-100/101 readbacks (docs/audio.md §4) |
| Microphone | same, Voice Memos record | `[audio] mic on`, `[audio] mic off: … FIFO overruns`, `ios3ds-audio: mic:` |
| Cameras | same, Camera.app back/front for ~10 s each | `[cam] … streaming`, `0 FIFO overruns`, `drain gap max`, `[cam] monitor … max FIFO fill`, `photos/CAM*.RAW` + `analyze_dump.py` |
| Gyroscope | `diag=1`, run `ios3ds-gyrotest` | `[gyro]` chip line, `[gyrotest]` rates per axis |
| SpringBoard restart | `sd:/ios3ds/log.txt` after a respring, a SpringBoard crash or `launchctl stop com.apple.SpringBoard` | two `[input] connected to SpringBoard` lines with `[input] SpringBoard went away; restarting` between them, and `[mcud] SpringBoard's HID event port went away; reconnecting` followed by a second `[mcud] connected`; brightness (`[mcud] iOS brightness …`) and orientation lines continue afterwards |
| GPU present / scatter | normal use, or `IOS3DS_GL_SCATTER=1` in SpringBoard's environment | `[pica] present: … not physically contiguous`, `[gl] pica: surface … through an arena copy`, no `reading surfaces back through the CPU` |
| SD load | normal use; `[hb]` every 10 s | `[hb] … md0 … 0 err` |
