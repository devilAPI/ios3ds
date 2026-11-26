# The `nintendo3ds` QEMU machine (development aid)

As of 2026-11 there is no 3DS machine in upstream QEMU, and none online
(2020 qemu-discuss thread; existing 3DS emulators are HLE or need dumped
boot ROMs). So ios3ds ships its own minimal model:
`emu/qemu/hw/arm/nintendo3ds.c`, built into QEMU v10.2.4 by `make qemu`.

**It is not a 3DS emulator.** It models only what the ios3ds payload and the
early XNU boot touch. Everything we learn from it still has to be confirmed
on the console.

## What is modelled

| Part | Model |
|---|---|
| ARM11 MPCore core 0 | QEMU `arm11mpcore` (core 1 is not created, which matches "parked") |
| ARM9 | QEMU `arm946`, with its own address space (ITCM + mirrors, ARM9 RAM, DTCM, ARM9-only IO) |
| CPU clusters | separate clusters, because QEMU requires that for different CPU types |
| MPCore private region | `arm11mpcore_priv` at 0x17E00000 (SCU, GIC with 128 IRQs, private timer, watchdog) |
| RAM | FCRAM 128 MiB (`new3ds=on`: 256 MiB, the GPU model reaches the first 128), VRAM 6 MiB, AXI WRAM 512 KiB, DSP RAM, all shared between both CPUs |
| Boot ROMs | stub vector ROMs: boot9 → 0x08000000 veneers, boot11 → 0x1FFFFFA0 RAM vectors, plus the boot11 wait loop on 0x1FFFFFFC |
| Luma chainload | `-kernel x.firm`: sections copied, ARM9 gets argc=2 and argv with the fb pointers when the FIRM sets screen-init, ARM11 jumps to its entry. MMU/caches off. |
| SD | TMIO host at 0x10006000 (the command, status, FIFO32 and auto-CMD12 subset that Normmatt's driver uses) on top of QEMU's real SD card model (`-drive if=sd`) |
| Screens | PDC0/PDC1 framebuffer registers; formats 0–4, rotated storage; a 400×480 display (top screen above bottom). `screendump` gives PNGs. |
| Buttons | HID at 0x10146000. Keyboard: A=a, B=s, X=x, Y=y, L=q, R=w, START=Enter, SELECT=Backspace, plus the arrow keys |
| MCU (I2C bus 2, 0x4A) | IRQs, RTC from the host clock, battery, sliders, accelerometer, power off (QEMU exits). Keys: HOME=h, POWER=p ("held" after 3 s, which starts iOS's clean shutdown through `ios3ds-input`), volume slider -/=, shell c, turn the console [ / ]. QOM properties on `/machine` for tests: `mcu-battery`, `mcu-charging`, `mcu-volume`, `mcu-shell-open`, `mcu-orient`, `lcd-backlight`, `mcu-wifi-led` and `mcu-wifi-led-writes` (read only). [mcu.md](mcu.md) §6, §12, §14 |
| Gyroscope (I2C bus 3, 0xD0) | ITG-3200 registers (WHO_AM_I, PWR_MGM sleep, GYRO_XOUT..ZOUT at 14.375 LSB per deg/s). QOM: `gyro-x`, `gyro-y`, `gyro-z` (millidegrees/s about the console's axes), `gyro-awake`, `gyro-reads` (read only). [mcu.md](mcu.md) §11 |
| Ambient light | QOM `ambient-light` (uint32, lux, read/write on `/machine`): the inner camera model reports the matching 8-bit AE luminance (MCU var 0x2218) the ARM9 reads on New models; the backlight follows it in `ios3ds-mcud` when auto-brightness is on. [mcu.md](mcu.md) §13 |
| Cameras | Three MT9V113 sensors on I2C (0x10161000: 0x7A inner, 0x78 outer right; 0x10144000: 0x78 outer left; they answer only with CFG11_CAMERA_CNT bit 0), CAM0/CAM1 at 0x10120000/0x10121000, FIFO windows 0x10320000/0x10321000 (pop on address change, 0x500 units, overrun), a moving test pattern at 15 fps on the virtual clock (`ctr_cam.c`). Use `--icount 2`: with plain timing every frame overruns. `IOS3DS_CAM_LOG=1` logs register writes and transfers. [camera.md](camera.md) §5 |
| LCD backlight | 0x10202240 / 0x10202A40; once written, the emulated screens are dimmed by it (and blacked out when the MCU switches a backlight off) |
| CFG9 / CFG11 | SDMMCCTL is stored; SOCINFO reports an Old 3DS (New 3DS with `new3ds=on`, below) |
| New 3DS (`-M nintendo3ds,new3ds=on`, `emu/run.py --new3ds`, `emu/soak.py --new3ds`, or `IOS3DS_NEW3DS=1`) | SOCINFO 7, the `PDN_LGR_SOCMODE` clock switch with its GIC 88 handshake (PERIPHCLK follows: 402 MHz at 804), the L2C-310 registers at 0x17E10000 (maintenance counted, no cache), the C-stick/ZL/ZR chip on I2C bus 3 at 0x54 (Z = ZL, N = ZR, keypad 8/2/4/6 = C-stick). QOM: `n3ds-socmode`, `l2c-ctrl`, `l2c-inv-lines`, `l2c-syncs`, `cstick-x/-y`, `exthid-buttons`, `exthid-reads`. [n3ds.md](n3ds.md) section 4 |
| GPU (PICA200) | The GX block at 0x10400000: PSC memory fill, PPF display transfer / texture copy and the P3D command-list processor, rendered by a software model (`tools/ios3ds-pica/sim/picasim.c`, [pica200.md](pica200.md) §5). GIC 40/41/44/45 follow its status bits. `PDN_GPU_CNT` reads 0x1007F (clocked, out of reset), `CFG11_GPUPROT` is stored. Compared with hardware since run 13: like the real GPU, fills outside VRAM write nothing and non-power-of-two textures sample garbage; started operations finish later in virtual time ([pica200.md](pica200.md) §13). |
| Wi-Fi | TMIO controller 2 at 0x10122000 with an AR6014-like SDIO card (BMI, HTC/WMI) and a WPA2/WPA1 (CCMP or TKIP) access point bridged to `-nic user,model=ctr-wifi` (slirp); CFG11_WIFICNT and GPIO 2_2/3_12 power it. HTC sends per-endpoint credits in RX trailers and refuses messages with `Mbox Full` when they run out; QOM: `htc-credits-used`, `htc-credit-reports`, `htc-credit-waits`, `htc-overflows`, `wifi-tx/rx-frames`, `wifi-tx/rx-drop`. `emu/wifi_test.py` runs the end-to-end tests (including a 4 MiB credit/bulk transfer); scans also report two decoys (NeighborNet, CoffeeShop); `emu/wifi_ui_test.py` joins through iOS Settings. [wifi.md](wifi.md) §7, §10 |
| Unmapped addresses | **external abort**, like a real bus. This is how the emulator caught XNU touching S5L8720 peripherals. Known IO windows are `unimplemented-device` (reads return 0, logged with `-d unimp`). |

## Known fidelity gaps (verify on hardware)

- **CP15 encodings missing from MPCore don't trap in QEMU**:
  `c15,c2,4` and `c6,c0,2` read back 0. The real behaviour is
  *UNPREDICTABLE* per the TRM, so the M1 probes report what the console
  actually does. The kernel patches remove both encodings regardless.
- No cycle accuracy. Busy-wait delays run faster than on hardware: the
  "monitor alive" counter climbs about 10× faster.
- Caches are not modelled, so coherency bugs won't show up here. That's why
  the design keeps caches off everywhere except inside XNU, and the shim
  cleans every line it writes.
- No PXI or DSP. I2C, SPI, the MCU and the GPU are development models, not
  compared with hardware. The LCDs are already "on", as after Luma's screen
  init.

## Usage

```sh
make qemu                    # once (~10 min)
make sdimg                   # payload + kernel + devtree + shim -> build/sd.img
make emu                     # boot, screenshot at 9.5 s, print sd:/ios3ds/log.txt
python3 emu/run.py --time 10 --trace int,unimp,guest_errors   # exceptions -> build/emu/qemu.log
python3 emu/run.py --regs                      # register dump of both CPUs at the end
python3 emu/mon.py --boot 7 "info registers" "x/16wx 0x80251ef0"   # poke a stopped VM
python3 emu/run.py --gdb     # waits for arm-none-eabi-gdb on :1234
python3 emu/run.py --time 420 --out build/emu-x --hmp "300=qom-set /machine mcu-battery 15"
python3 emu/tap.py --time 400 --key 280:h --key 340:p:5 --set 255:mcu-battery=50 --get 300:lcd-backlight
python3 emu/run.py --icount 2 --time 300   # real-hardware timing (see below)
IOS3DS_QEMU=$PWD/build/qemu-local/b/qemu-system-arm \
  python3 emu/soak.py --cycles 5 --icount 2   # soak a branch with its own QEMU build
```

`IOS3DS_QEMU=/path/to/qemu-system-arm` overrides the binary path in
`emu/run.py`, `emu/viewer.py` and `emu/soak.py`; handy when building a private
copy of the machine model without touching the shared `build/qemu` tree.

### Real-hardware timing: `--icount`
By default QEMU's timers follow the host's wall clock while TCG runs the
ARM11 far slower than the real 268 MHz part, so timer interrupts arrive
"early" relative to the code. That hid a real-hardware deadlock (re-log §25).
`--icount 2` (`-icount shift=2,sleep=off`, single-threaded TCG) ties time to
the instruction count, ~250 MHz, which reproduces the hardware's ordering.
Use it for any boot-time ordering question; the plain mode stays the default
because it is faster for long boots.

### Boot and app-launch profiles: `emu/bootprof.py`
`python3 emu/bootprof.py` boots `build/sd.img` with `--icount 2`, boot.cfg
`ioprof=2 log_ts=1` and QEMU's profiler (`N3DS_PROF=<file>`: per 250 ms of
guest time ARM11/ARM9 busy %, md0 pending %). It waits for the boot to
settle, cold-launches Settings, Notes and Safari, and writes a phase table
to `build/emu/bootprof/report.md`. `emu/hfsmap.py` maps the md0 sectors to
HFS+ files. See [boot-perf.md](boot-perf.md).

### Driver stress: `emu/stress.py`
`python3 emu/stress.py --cycles 5 --icount 2` opens the apps whose home-screen
slot is known from the rootfs recipe (Photos, Settings, Notes, Safari, Music),
drags in them, swipes pages with the D-pad, scrolls with the circle pad,
starts/stops Camera.app, plugs and unplugs the emulated headphones, turns the
console through the accelerometer poses and locks/unlocks twice. It then
checks the SD log for panics, stalls, md0 errors, GPU timeouts/present
failures, camera overruns and audio DMA errors. `--diag` adds `diag=1` to the
image's boot.cfg (gltest probe, gyrotest, `[smp]` sampler) and the default
`--mem` adds `ios3ds_mem=1` (the `[mem]` report). Both edit `sd:/ios3ds/boot.cfg`
inside the disposable image; every run needs a fresh image. Artifacts and the
PASS/FAIL report are in `build/emu/stress/`. The inventory of drivers, tests
and open hardware checks is [driver-status.md](driver-status.md).

### Audio on older versions: `emu/audio_check.py`
`python3 emu/audio_check.py --sd build/sd-4-1.img --ios 4.1 --tests jack,click,song --icount 2`
boots one image and checks the emulated headphone jack (guest route changes in
`sd:/ios3ds/log.txt` plus QEMU's `[jack]` lines), keyboard clicks (a new iOS
output stream and CSND start after typing in Spotlight or Notes) and Music.app's
test song (a new stream plus a non-silent CSND WAV). Per-version tap positions
are in `UI`; artifacts and the PASS/FAIL report go to `--out`. The script does
not edit the image; a fresh one is still recommended for repeatable logs.
See docs/audio.md §10 and re-log NEXT.

### Safari and memory: `emu/safari_test.py`
`python3 emu/safari_test.py --icount 2` boots with the Wi-Fi model, opens
pages from a host HTTP server in Safari (`ios3ds-diag -urlpoll`, boot-arg
`ios3ds_urltest=1`, which opens each new URL through SpringBoardServices),
fails a page on any Safari alert and prints the `[mem]` report (boot-arg
`ios3ds_mem=1`: vm_stat, jetsam level, mbufs, resident size per process)
before Safari, after the first page and at the end. `--host 10.0.2.2.nip.io`
makes Safari resolve a name, `--www DIR --urls a.html,b.html` serves saved
pages, `--mem-size 0x06000000` gives XNU less RAM, `--new3ds` the New 3DS
model with 256 MiB. Load errors are logged as `[applog] MobileSafari load
error (…): domain code userInfo` (re-log 75).

### Checking the root disk after a shutdown

`python3 scripts/hfs_clean.py --sd build/sd.img` reads the HFS+ clean-unmount
flag of `ios3ds/rootfs.hfs` from the SD image (exit 0 clean, 1 dirty, 2 no
header). A killed QEMU leaves the flag clear; a clean shutdown from the slider
or a long POWER press leaves it set, and the same image boots again
([mcu.md](mcu.md) §12). Each test needs a fresh `build/sd.img`:
`rm -f build/sd.img && make sdimg`.

### Crash reports from an image: `scripts/crashlogs.py`

`python3 scripts/crashlogs.py build/sd.img build/crashlogs` copies every
`*.plist`, `*.crash` and `*.panic` under `/private/var/mobile/Library/Logs/CrashReporter`
(and the other standard CrashReporter folders) out of a QEMU SD image or a
`rootfs.hfs`, flattening the HFS paths into a host directory. It uses
`build/tools/hfsplus` (`make hfsplus`) and, for a raw SD image, `mcopy` to make
a temporary copy of `::/ios3ds/rootfs.hfs` under `build/` (delete it afterwards).
`emu/soak.py --crashlogs` runs it automatically into the soak report directory.
`python3 emu/slider_test.py --icount 2` wakes the device, shows the power-off
slider and cancels it twice, and fails if SpringBoard/input restarts; its
screenshots and report go to `build/emu-slider/`.
The same command works on a real card or a copy of its `rootfs.hfs`, so a
console user can send the reports that explain a process crash
([README troubleshooting](../README.md#troubleshooting)).

### Scripted debugging: `emu/gdb/`
The Arm gdb 14.2 has no Python and the host gdb has no ARM target, so the
scripts talk to QEMU's gdbstub directly through `emu/gdb/rsp.py`:
- `reads.py`: start QEMU with `-s -S`; logs every clock read (`shim_get_abstime`) with CPSR, deadline and IRQ counters.
- `panic.py`: start QEMU with `-s -S`; stops at `_panic`, prints the format string, its args and the stack.
- `postmortem.py`: start QEMU with `-s` (running); attach after a hang, print registers, shim counters and the strings and code addresses on the stack.


## Interactive window: `emu/viewer.py`
Our QEMU build has no GTK/SDL/VNC display, so `emu/viewer.py` starts QEMU headless with a QMP socket. It shows the 3DS screens plus an upright iOS view (the top screen rotated back) at ~8 fps, and sends real key down/up events.

Keys:
- Enter/H = START (iOS Home)
- Backspace/K = SELECT (iOS lock)
- Q/W = L/R (volume)
- A, S, X, Y = A, B, X, Y
- arrows = D-pad

There is no touch input yet (no touchscreen model in the board).

## Azahar: the `.3dsx` launcher

The launcher is ordinary Horizon homebrew, so it runs in [Azahar](https://github.com/azahar-emu/azahar) (the AppImage; with AppImageLauncher installed, start it with `APPIMAGELAUNCHER_DISABLE=1`). Azahar cannot run `ios3ds.firm` itself: it emulates Horizon, not a FIRM chain-load to the ARM9.

Set up Azahar's virtual SD card in `~/.local/share/azahar-emu/sdmc/`:
- symlink the files of `build/sd/ios3ds/` into `sdmc/ios3ds/`;
- create a dummy `sdmc/boot.firm` (`FIRM` and zeros) standing in for Luma.

Then run `azahar.AppImage build/launcher/ios3ds.3dsx`. Checked on 2026-11-20 with Azahar 2126.2:
- Missing files are reported (`Missing sdmc:/ios3ds/ios3ds.firm`, `Missing sdmc:/boot.firm`).
- With `nwm.code` absent, the Wi-Fi firmware step runs. Azahar has no NWM system title, so it ends in one red line, `cannot open NWM .code (FS C8804478, no Luma3DS for FSPXI): Wi-Fi off`. No `nwm.code.tmp` is left behind, and the menu continues to `A: launch ios3ds`.
- Don't press A in Azahar: it would rename the symlinks for a reboot that Azahar cannot do.
