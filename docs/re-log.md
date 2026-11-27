# Reverse-engineering log

Every analysis step, with the command and what it showed, so anyone can
re-run it against their own IPSW. Newest entries go at the end.

Inputs: `iPod2,1_4.2.1_8C148_Restore.ipsw` (363,553,480 bytes, sha1
`b9efddc7bb4350c237a8d3846af61bbfc8a2f647`, same as the ipsw.me listing for
Apple's `secure-appldnld.apple.com/iPhone4/061-9855.20101122.Lrft6/...` URL).
Keys: `https://api.ipsw.me/v4/keys/ipsw/iPod2,1/8C148`. The Apple Wiki puts a
CAPTCHA in front of automated clients, so the tooling doesn't scrape it.

## 2026-11-16: M0

### 1. IPSW contents
`python3 -c "import zipfile; ..."` lists:
- `kernelcache.release.n72` (IMG3, 4.37 MB)
- `Firmware/all_flash/all_flash.n72ap.production/DeviceTree.n72ap.img3`
- root filesystem `038-0014-002.dmg` (311 MiB)
- restore ramdisk `038-0049-002.dmg` and update ramdisk `038-0050-002.dmg`
- iBoot, LLB, iBEC and iBSS, plus the boot logos

### 2. IMG3 decryption gotcha
The kernelcache IMG3 has the tags TYPE, DATA, SEPO, KBAG, SHSH and CERT.
Decrypting only `dataLength & ~15` bytes of DATA (as some docs describe)
leaves a 15-byte encrypted tail, and LZSS then comes up 64 bytes short with an
adler32 mismatch. The ciphertext is padded to 16 bytes inside the tag's slack
space. Decrypting `roundup(dataLength, 16)` bytes and truncating fixes it, and
the adler32 then matches (`1c4f9e86`). Fixed in `scripts/fwlib/img3.py`.

### 3. Kernel identity and layout (`make kscan`)
- `Darwin Kernel Version 10.4.0: Wed Oct 20 20:06:26 PDT 2010; root:xnu-1504.58.28~3/RELEASE_ARM_S5L8720X`
- Mach-O: cputype 12 (ARM), cpusubtype 6 (`CPU_SUBTYPE_ARM_V6`), MH_EXECUTE.
- `__TEXT` vmaddr **0x80001000**; entry (LC_UNIXTHREAD pc) **0x80067050**.
  The kernel lives at 0x80000000 and up, not 0xC0000000.
- Segments: `__TEXT`, `__DATA`, `__KLD`, `__LAST`, `__PRELINK_TEXT`
  (0x802cf000, 5 MB of kexts), `__PRELINK_INFO` (XML), `__CTF`, `__LINKEDIT`.
- LC_SYMTAB holds 4391 symbols. These are the exported KPI that kexts link
  against, e.g. `_ml_io_map`, `_PE_cpu_start`, `_ml_processor_register`,
  `_IOFlushProcessorCache`, `_kprintf`, `_panic`, `_PE_parse_boot_argn`,
  `_PE_i_can_has_debugger`. Internal symbols such as `_arm_init` and
  `_PE_init_platform` are stripped.
- 99 distinct prelinked kext bundle IDs (see `build/staging/kernelcache.txt`).
- Panic strings include source paths. The ARM-specific files are
  `osfmk/arm/{cpu,locks_arm,loose_ends,machine_routines,pcb,pmap,rtclock,status,trap}.c`,
  `osfmk/console/arm/serial_console.c`, `osfmk/console/video_console.c`,
  `bsd/dev/arm/{systemcalls,unix_signal}.c` and `osfmk/kdp/ml/arm/kdp_machdep.c`.

### 4. Entry stub disassembly (0x80067050)
I wrapped `__TEXT` in an ELF and disassembled it:
`llvm-objcopy -I binary -O elf32-littlearm` then
`llvm-objdump -D --triple=armv6k-none-eabi --adjust-vma=0x80001000`.
The stub runs in ARM mode with the MMU off:
```
mov r1,#0 ; ldr lr,=<virt continuation> ; cpsid if
ldr r8,[r0,#8]    ; boot_args->physBase
ldr r9,[r0,#4]    ; boot_args->virtBase
ldr r10,[r0,#0xc] ; boot_args->memSize
ldr r4,[r0,#0x10] ; boot_args->topOfKernelData  -> L1 table goes HERE
TTBR0 = TTBR1 = r4 | 0x18 ; TTBCR = 2
zero 9*1024 words at r4 (16 KB L1 + 20 KB of L2 tables)
identity-map the 1 MB section containing PC (V=P, so the MMU-on switch survives)
map [virtBase, virtBase+memSize) -> physBase as SUPERSECTIONS (bit18), C+B, AP=01
L2 page table at r4+0x8000 for 0xFFF00000; maps the vector page at 0xFFFF0000
clean+invalidate D-cache by set/way: 128 sets x 4 ways x 32 B  (16 KB geometry hard-coded)
SCTLR |= M|C|W|Z|I|V|U|XP (0x00C0380D), high vectors, ARMv6 extended page tables
r0 = r0 - physBase + virtBase ; jump to virtual continuation
```
What this means for the loader:
- r0 = **physical** pointer to boot_args, IRQ/FIQ masked, MMU off.
- `topOfKernelData` must be **16 KB aligned** (it becomes TTBR) and is followed
  by 36 KB that the kernel zeroes.
- physBase must be **16 MB aligned**, because supersections are used. FCRAM at
  0x20000000 qualifies.
- boot_args and the device tree must sit inside `[physBase, physBase+memSize)`
  so their virtual addresses are valid after the switch.
- Vectors are **high (0xFFFF0000, SCTLR.V)**, mapped by the kernel's own page
  table. VBAR is never used.

### 5. CP15 / secure-instruction scan (`scripts/kscan.py`)
This is a word scan of ARM-mode code. ARMv6 Thumb cannot issue coprocessor
instructions, so the scan is complete apart from literal-pool false positives.

Results:
- **No SMC, no MCRR block cache ops, no c12 (VBAR) access** in the kernel or the kexts.
- `MRC/MCR p15,0,c15,c2,4` at 0x80060274 (read) and 0x80064208 (write, in
  the reset/resume path of the vector page). This is the ARM1176 *Peripheral
  Port Memory Remap* register, so it needs patching.
- `MCR p15,0,c1,c0,1` (ACTLR write) at 0x80067bf0 is a setter function. It
  needs review because the MPCore ACTLR has the SMP/AMP bit.
- `MRC p15,0,c6,c0,2` (IFAR) at 0x800662e0 and 0x8006632c, in the
  prefetch-abort path.
- PMU `c15,c12,0..3` in the kernel (0x8006f1xx–0x8006f28c) and in a kext
  (AppleARM11xxProfile, 0x805810d4+).
- Everything else is standard ARMv6: c7 cache and barrier ops by MVA and
  set/way, `c7,c0,4` WFI, `c8` TLB ops, `c13,c0,1..4` (CONTEXTIDR and the thread
  ID registers; `c13,c0,4` = TPIDRPRW is read 106 times, it holds the
  current-thread pointer), TTBR0/1, TTBCR, DACR, DFSR, IFSR, FAR, CPACR.

### 6. SoC layer inside the kernel (strings plus pointer xrefs)
`pe_identify_machine` matches `/arm-io` `device_type` against a table at
0x801dc0e4–0x801dc4ac. The table has entries for: `s5l8900x-io`,
`s5l8720x-io`, `s5l8722x-io`, `s3c2410-io`, **`integratorcp-io`**,
`olocreek-io`, `omap3430sdp-io` and `s5i3000-io`. Each entry carries
clock/timebase constants (for example 200/100/50 MHz for Integrator/CP, and
19.2 MHz next to OMAP3430). The panic string is `pe_identify_machine: Epoch Mismatch`.

Built-in serial drivers are selected by `uart,16550`, `uart,s5i3000` and
`uart,s5l89x0x` (8900/8720/8920/8930/8940/8950). `boot-console`,
`-progress` and `BootCLUT` are also present, along with the 32 bpp CLUT
format string `BBBBBBBBGGGGGGGGRRRRRRRR` used by the video console.
**Implication:** early timer, serial and framebuffer console support live in the
kernel proper (pexpert, mostly Thumb code). Everything else is a kext matched
through the device tree.

### 7. Device tree template (`build/staging/devicetree.txt`)
77 nodes. Root `compatible = N72AP|iPod2,1|AppleARM`.
`/cpus/cpu0 compatible = ARM,1176jzf-s|ARM,v6`.
`/arm-io device_type = s5l8720x-io` with `ranges` = (0x00000000 → 0x38000000,
size 0x08000000) and (0x10000000 → 0x18000000, size 0x10000000). Children
include: vic (`vic,pl192`), edgeic, gpio, power, clkrstgen, dmac0/1, sha1,
usb-complex, amc, clcd, flash-controller0, aes, sdio, h264decoder, scaler,
tv-out, mbx, mpvd, prng, spi0/1/4 (multi-touch on spi4), otgphyctrl,
i2c0 (accelerometer, pmu, audio0, spkr-amp), i2c1 (als, mikey), timer (`timer,s5l8720x|timer,s5l8900x`),
wdt, i2s0, uart0/1 (bluetooth on uart1), pke, mipi-dsim/lcd, swi and xmc.
The template's `/chosen/memory-map`, `/memory reg` and the frequencies are all
zero, because iBoot fills them in. Wi-Fi is **Broadcom** (`AppleBCMWLAN`).

### 8. Root filesystem
The VFDecrypt key works. The decrypted UDIF has 7 blkx partitions; the
`disk image (Apple_HFSX : 5)` one is 1,287,512 sectors (629 MiB) and carries the
HFSX signature `HX`. `7z l` lists `sbin/launchd`, `usr/lib/dyld`,
`System/Library/CoreServices/SpringBoard.app/SpringBoard`, and
`System/Library/Caches/com.apple.dyld/dyld_shared_cache_armv6` (167 MB, larger than RAM!).
GL lives in `OpenGLES.framework/MBXGLEngine.bundle` (the only GL engine) and
`PrivateFrameworks/MBX2D.framework`. IOMobileFramebuffer, IOSurface and
CoreSurface are present.

## 2026-11-16: M1/M2 (emulator)

### 9. QEMU development machine
- Upstream QEMU has no 3DS machine, and none was found online.
- I wrote `emu/qemu/hw/arm/nintendo3ds.c` (docs/emulator.md), built against
  QEMU v10.2.4.
- The two different CPU types need separate `cpu-cluster` objects.
- Unmapped physical addresses raise external aborts, which is how the S5L8720
  accesses below were found.

### 10. M1 payload in the emulator
`make emu` gives:
- SD mounts (QEMU SD card, SDHC, 2 GiB);
- the ARM11 handshake works;
- CP15 ID registers are reported (QEMU arm11mpcore: MIDR 410fb022 r0p2,
  CTR 1d192992, FPSID 410120b4, MVFR0 11111111);
- the kernel loads and the jump happens.

Probing `c15,c2,4` and `c6,c0,2` does **not** trap in QEMU (they read 0). The
real console must answer that.

### 11. "pe_identify_machine: Epoch Mismatch"
- After the jump, the ARM11 PC was at 0x80015dd0, just past `_panic`.
- Return addresses on the stack (`x/48wx 0x80251ef0`) were 0x801dc137
  (pe_identify_machine), 0x801dc743 and 0x80248960.
- Disassembly at 0x801dc124: `ldrh r3,[r0,#2]; cmp r3,#5; beq ok; ldr r0,="pe_identify_machine: Epoch Mismatch"; bl _panic`.
- **boot_args.Version must be 5 for iOS 4.2.1.** It's now `ba_version=5` in boot.cfg.

### 12. Serial console = S5L UART ops table
- The kernel was spinning at 0x801dcef4 (`ldr r0,[base,#0x10]; and r0,#4`):
  the UTRSTAT TX-empty poll.
- The ops table at 0x80266e48: init 0x801dcdd0, baud 0x801dce5c,
  tx-ready 0x801dcee8, tx-data 0x801dcf00 (`strb r0,[base,#0x20]` = UTXH),
  rx-ready 0x801dcf18, rx-data 0x801dcf30.
- A DCC (cp14) variant exists at 0x80266e30.
- All six were patched. tx-data jumps into `kernel/shim/shim.S`, which writes
  to the ring that the ARM9 drains.
- Result: the kernel's own panic text appears on the 3DS screen and in log.txt.

### 13. `/pram` must be filled
- Translation-fault data abort at VA 0xC0001000, at PC ~0x801dcb8a.
- The kernel reads the DT `reg` of the `/pram` node, `ml_io_map`s it, and
  compares the first word with 0x4F4F5053 ('OOPS', the persistent panic log).
- The template's reg is (0,0), so nothing was mapped.
- Fixed in kernel/dtgen.py: `/pram` = AXI WRAM 0x1FFA0000 (64 KiB),
  `/vram` = VRAM 0x18000000 (3 MiB). Both lie outside XNU's RAM, as iBoot's
  carve-outs do.

### 14. First real panic: the S5L timer
`sleh_abort at interrupt context` (trap.c:484):
- external abort, write to VA 0xC0013088;
- the store at 0x801dbf94 writes the timer-4 config register in the
  s5l8720x branch of `pe_identify_machine` (its literals: "interrupt-controller",
  "master", "timer", "cpu-debug-interface", `_gPEClockFrequencyInfo`).

The timer primitives in osfmk/arm are:
- 0x80068038: 64-bit counter at +0x80/+0x84
- 0x80068064: time left
- 0x80068078: set deadline (+0xA8 count, +0xA4 = 3)

The FIQ handler is near 0x80068024 and uses FIQ-banked r8, r10 and r11.
Analysis and plan: docs/m2-first-panic.md.

## 2026-11-16: from the first panic to SpringBoard drawing (emulator)

### 15. Timer and interrupts
- XNU-1504's timer is pluggable through per-CPU hooks:
  - `cpu_data+0x6C`: the FIQ handler, loaded into r9_fiq
  - `+0x74`: the timer register base
  - `+0x70` / `+0x64` / `+0x68`: get-abstime / get-decrementer / set-decrementer, implemented at 0x80068038 / 0x80068064 / 0x80068078
- The three hooks are patched into the shim:
  - time base = MPCore watchdog in timer mode (free-running, extended to 64 bits)
  - deadlines = MPCore private timer (GIC 29)
  - both at PERIPHCLK/128 = 1,047,311 Hz, which is also `timebase-frequency`
- The IRQ entry (0x8006648C) is patched into a trampoline. For IRQ 29 it re-enters the kernel's **FIQ** handler (`mov pc, r9` in FIQ mode).
- **Gotcha:** the SWI fast path for `mach_absolute_time` (~0x80066290) calls get-abstime with no usable stack (sp just under the vector page), so that hook is stackless.
- `/arm-io/timer` and the VIC, edge IC, GPIO, power, clkrstgen, watchdog and uart0 nodes point at fake register pages in AXI WRAM (0x1FFB0000+).
- pexpert translates child addresses as `ranges[0].parent + child` and ignores the other ranges, while IOKit checks every range. So `ranges[0]` = child 0 → fake pages, and `ranges[1]` = identity map of the 3DS IO window.
- AppleARMPL192VIC needs its PrimeCell ID registers (0xFE0–0xFFC) filled in (`fakeio.bin`).

### 16. IOKit and the root device
- RTC wait: 30 s timeout patched to 0 (0x802E78B4).
- `IOPMU` waits only block side threads.
- `secure-root-prefix=md` made the md root wait for a SecureRoot callback, so the property is removed.
- Root = md0 via memory-map `RAMDisk` + `rd=md0`. `mdevstrategy` (0x80072BF8, Thumb) is patched into `kernel/shim/blk.c`, which hands I/O to the ARM9 block server reading `sd:/ios3ds/rootfs.hfs` with FatFs fast seek.
- Bugs found on the way:
  - preemption race on the single mailbox → interrupts off per transfer;
  - lazily mapped UPL pages → pre-fault them before masking interrupts.

### 17. Prefetch-abort fault address
The IFAR replacement was `lr-4`, but the handler had already done `sub lr,lr,#4` (0x800662AC). The result was 2 million prefetch aborts at the same address; the fix is `mov r1/r5, lr`.

### 18. Userland
- launchd starts.
- Root filesystem changes (`rootfs/recipe.json`, applied with xpwn hfsplus):
  - fstab `/dev/md0 / hfs rw`
  - grown to 1 GiB (after removing CoreLocation's consolidated.db, since Apple's image has 0 free blocks)
  - 28 daemons disabled (keybagd: FATAL KEYBAG ERROR → restore reboot)
  - SpringBoard stderr routed to `/dev/console`
- SpringBoard fixes:
  - "No window server display found" → keep `arm-io/clcd` on 2 fake pages with iBoot's CLCD state, and drop `function-lcd_enable`.
  - The kernel `beq` at 0x807A9B68 is NOP'd, so swap-done is synchronous.
  - liblockdown is stubbed in the dyld cache (no lockdownd).
  - `AppleKeyStoreGetDeviceLockState` is stubbed (it blocks without a system keybag).
- QuartzCore uses its software compositor (no AppleMBXDevice).
- Rendering is vsync-driven: the shim turns the 3DS PDC0 VBlank (GIC 42) into the S5L CLCD frame IRQ through the fake VIC.
- `tools/ios3ds-sample` prints every process's thread state to the console.
- **Result: SpringBoard draws the "Connect to iTunes" screen, mirrored onto the 3DS top screen by the ARM9.**

## 19. Input reaches SpringBoard (2026-11-16)
- Wired: ARM9 `input_poll` (pad + TSC) → ring at shim+0x6200 → sysctl `kern.ios3ds_input` (registered from the first `mdevstrategy` call) → `/usr/libexec/ios3ds-input`.
- QEMU: the daemon connects ("connected to SpringBoard's HID event system", "mailbox up: touch absent"; no TSC model yet) and logs `key 40 down/up` for START.
- Keyboard IOHIDEvents dispatched with `IOHIDEventSystemClientDispatchEvent` are **ignored** by SpringBoard: no visible effect for Home (0x40) or volume (0xE9).
- Buttons now go as GSEventRecords (52 bytes; `infoSize` at +0x30, checked in `_GSSendEvent` 0x352632c4) through `GSSendSystemEvent` (0x352635a4, resolved with dlsym) to PurpleSystemEventPort. **Delivered:** SELECT (lock 1010/1011) brings up "slide to power off" and an "Edit Home Screen" hint.
- **Open:** both are *hold* reactions. The down is honoured, but the up (sent 0.3 s later) is not, or not in time. Suspects: SpringBoard measures hold time against "now" while its run loop lags in the emulator; or the up needs extra record fields. Touch (GSHandInfo to the frontmost app) is still IOHID-only and untested (no TSC model).

## 20. Button timing in SpringBoard (2026-11-16)
- `-[SpringBoard lockButtonDown:]` (0x8a48) arms `lockButtonWasHeld` with a **2.5 s** NSTimer (`0x4004000000000000`). `lockButtonUp:` (0x9a2c) does nothing more once the power-down sheet `isOrderedFront`.
- `menuButtonUp:` (0x8db8) returns at 0x918e when no menu timer was set by the down. A tap then goes through the double-tap delay (`performSelector:_handleMenuButtonEvent afterDelay:`).
- **Reproducible:** a down sent immediately and an up 0.3 s later → "slide to power off" plus the "Edit Home Screen" hint (a *hold*).
- **Daemon now defers:** a tap sends down+up back to back on release; the down of a hold goes out after ~0.8 s (counted in poll ticks, because `mach_timebase_info` gave no usable value).
- With deferral, neither a lock tap nor a 5 s lock hold (tick-stamped: down @22836, up @22969) changes the screen. A Home tap on page 1 doesn't open Spotlight either. **Unresolved.**
- **Debug aids tried:**
  - `SBLogging` default: no output.
  - Setting SpringBoard's trace byte (0x18c264 → file 0x18b264): runs, but NSLog goes to ASL, and syslogd persists nothing (`/var/log/syslog` 0 bytes).
  - `CFLOG_FORCE_STDERR=1`: no visible output.
  - mkrootfs file patches now take an optional `mode`; they used to force 0644, which broke the patched SpringBoard executable.
- **Next:** get syslogd writing (or patch NSLog's ASL path to stderr), then trace which button handlers run.

## 21. Buttons work; the "holds" were the test harness (2026-11-16)
- `emu/run.py` turned every `key_down` into `sendkey <key> 3000` and ignored `key_up`. **Every test press was therefore a 3 s hold**, which explains the power-off sheet and the hint in entries 19/20.
- run.py now uses the down→up interval as the sendkey hold time.
- **Results:**
  - Home tap on page 1 → Spotlight ("Search iPod" with keyboard);
  - 4 s lock hold → "slide to power off";
  - lock tap → no visible change (screen-off isn't mirrored).
- The daemon's deferred-down for taps stays: a slow SpringBoard run loop can still lose a separate up to the 2.5 s hold timer on real hardware.
- The SpringBoard trace patch and syslog-to-console line were removed. NSLog never reached the console in any configuration we tried (open).

## 22. Touch works (2026-11-16)
- **QEMU:** CTR CODEC/TSC on NSPI bus 2 plus I2C/MCU, and an absolute pointer, all in `nintendo3ds.c` (from the QEMU agent's draft). The ARM9 driver reports `tsc_ok`, and pen samples land in the input ring with correct ADC values.
- **Ignored path:** digitizer IOHIDEvents dispatched by our daemon reach SpringBoard's event system but are ignored, as keyboard events were (entry 19).
- **Working path:** `kGSEventHand` (3001) GSEvents, layout from 8C148:
  - `_GSEventGetHandInfo` copies 0x24 bytes at record+0x34;
  - `_GSEventGetPathInfoAtIndex` = record+0x58+24*i;
  - `_UIApplicationHandleEvent` reads `pathInfosCount` at hand+0x21, treats type 8 as cancel, and treats a path as touching while proximity bit 1 is set.
  - Target: the frontmost app's purple port (`SBFrontmostApplicationDisplayIdentifier` → `GSCopyPurpleNamedPort`), else `GSSendSystemEvent` (SpringBoard).
- **Daemon:** needs `flipx` since the display blit rotates (`ix = rr*4/3`).
- **Result:** tapping Settings at iOS (47,320) opens Settings (`emu/tap.py`).
## 23. OpenGL ES 1.1: a software engine in MBXGLEngine's slot (2026-11-16)
- **Engine discovery** (OpenGLES `_eagl_init`, 0x34ff870c):
  1. `GLEngine.bundle` is tried first and is absent.
  2. `IOServiceGetMatchingServices(IOServiceMatching("AppleMBXDevice"))` must return ≥1 service.
  3. `CFBundleGetDataPointerForName(MBXGLEngine.bundle, "GLESGetEGLInterface")` is called and its result goes into `_mbx_dispatch`.
  - The MBX engine serves ES1 only (`initWithAPI:` checks `api == 1`).
- **Getting past the check:** one file patch on OpenGLES's own copy of the string (cache file 0x4ff9458) makes it read `"IOResources"`. The copies in MBX2D (0x30770c68) and MBXGLEngine (0x32ee0fac) are left alone.
- **Loading our engine:** `enable-dylibs-to-override-cache` makes dyld load our on-disk `MBXGLEngine` in place of the cache copy. The only `.dylib` files the rootfs keeps on disk are 12 symlinks into cache-only frameworks.
- **Engine table and call shapes:** read from every `_mbx_dispatch` call site in 8C148's OpenGLES. The table has 11 entries; 4.2.1 adds Set/GetProperty at +0x24/+0x28.
  - `CreateGC(sg, X+0x10, X+0xD34, X+0xC)` passes two 841-slot dispatch tables.
  - `attachImage:toCoreSurface:invertedRender:` calls `BindCoreSurface(gc, target, surface)`. It accepts `GL_RENDERBUFFER` only together with invertedRender=YES, and target 0x84F5 only with NO.
  - `swapNotification:` calls `SwapNotification(gc, IOMobileFramebufferGetID(fb), transaction, layer)`. The non-MBX path calls `IOMobileFramebufferSwapSignal(fb, transaction, layer)` instead.
- **CA drawable protocol:** read from the stock `_GLESBindView` (0x32eb4ba4), `_ViewTextureBeginIfNeeded` (0x32eb4a64), `_GLESPresentView` (0x32eb4628) and `_GLESCreateBuffer`/`_GLESDestroyBuffer`. The drawable is called through:
  - +4 `bind(d, 'BGRA'|'L565', {ctx, create, destroy})`
  - +8 `unbind`
  - +0xC `dequeue` → IOSurface
  - +0x10 `present(d, 1)`
- **The engine:** `tools/ios3ds-gl` is the CPU rasterizer behind a swappable `Backend`. The design is in docs/gpu.md. All 44 `gl*` imports of QuartzCore are among the 177 implemented entry points.
- **QEMU, run 1:**
  - The engine loads in `ios3ds-gltest` and in SpringBoard (`[gl] context created`, pid 8 and pid 16). SpringBoard creates an EAGL context even with `CA_ENABLE_OGL=0`. No unimplemented slot was hit.
  - Offscreen: all 10 pixel probes and `glGetError` pass (`gltest: PASS`).
  - On screen: drawing into the display's IOSurface through `attachImage:toCoreSurface:` runs at ~13.6 fps, including a 50 ms sleep per frame. The top LCD shows the scene: triangle, textures, blend, scissor bar and lit cube.
- **Regression:** with the demo off, the home screen stayed **black** (400/449 s). SpringBoard's context came from `-[CAWindowServer _detectDisplays]` → `M2CLCDDisplay::open` → `M2Display::M2Display` (0x346de5b8) → `Display::eagl_context(1)`.
  - The constructor probes ES2 and then ES1, and on success stores the API at `+0x17c`. API 1 picks 'L565' at `+0x180`.
  - Once a context exists, the display is GL-backed and the software compositor stops. Even with `CA_ENABLE_OGL=0`, no engine call followed, so nothing was drawn.
- **Fix:** a cache patch at file 0x46de620 (`bl eagl_context` → `mov r0,#0`) skips the ES1 probe.
- **Run 4:**
  - the home screen is back at 415 s;
  - the GL scene is drawn from ~425 s (449 s shot);
  - `gltest: PASS`;
  - SpringBoard creates no context.
- **Open:** CA's GL compositor needs L565 render targets plus the BindView/SwapNotification path, which RE covers but no run has exercised yet.
## 24. Audio: Apple's kexts on emulated PL080/I2S/I2C (2026-11-16)
- Kept `dmac0`/`i2c0`/`i2s0` on fake pages; see `docs/audio.md` for the design and addresses.
- `AppleARMIICDevice` transfer (0x802e4d1e, vtable 0x35c) takes `(this, op, sublen, sub*, len, buf*)`; op 1 = read, 2 = write (the CS42L58 driver's read-modify-write pairs in the boot log confirm it). Patched to a shim register file.
- `AppleARMPL080DMAC` filter (0x807d115c) reads the TC status once and writes IntTCClear once, so the shim can apply write-1-to-clear at its next service.
- **QEMU boot (450 s):** `AppleARMPL080DMAC::start`, `AppleS5L8900XI2SController::start`, three `_initDMAChannel`s, `AppleCS42L58Audio::start` and 78 I2C register accesses; SpringBoard home screen renders as before; mediaserverd idles with 19 threads.
- Volume-up (R) and lock (SELECT) presses reach SpringBoard (`[input] key e9/30`), but no HUD appeared and no output stream started, so the DMA/CSND path is covered only by the host test (`tests/test_audio.py`) so far.
- The ARM9 CODEC init reports "id 00" in QEMU (SPI stub) and carries on.
## 25. Real-hardware hang after "iBoot version": the clock base was 0 (2026-11-17)
- **Symptom (hardware runs 3–6):** silent stop right after `iBoot version: ios3ds-loader`; probes showed 34 clock reads (last one with CPSR `0x93`), one 10 ms deadline (caller `0x800695d3`), zero IRQ entries. Plain QEMU never hung.
- **Reproduction:** `-icount shift=2,sleep=off` (QEMU time = instruction count, ~250 MHz, roughly the Old 3DS ARM11) gives exactly the hardware probe values: `abs 34 cpsr 80000093 | dec 1 last 10473 caller 800695d3`. Without icount, QEMU's wall-clock timer runs far ahead of the slow TCG CPU: in a plain run clock reads 2–7 already come from IRQ mode (CPSR `0x192`), i.e. the first tick fires before read 2. `emu/run.py --icount 2` now does this.
- **Where it hangs** (`emu/gdb/postmortem.py`, attach after the hang): ARM11 at `0x80069186`–`0x80069198`, SVC, IRQs masked, in a loop
  `do { base = rtclock[+0x30..+0x34]; … } while (base == 0 || base changed)` (function `0x8006916c`, abstime → seconds/microseconds; `rtclock` = `0x80255128`). It is called from the calendar path at `0x80014eec`, between a lock/unlock pair (`0x80063420` / `0x800634c0`) that masks IRQs.
- **Who writes `rtclock+0x30`:**
  - `rtclock_init` at `0x8006993e`/`0x80069998`: the result of `mach_absolute_time()` (`bl 0x800694d4` at `0x80069936`);
  - the clock tick handler at `0x800696e4`: "now".
- **Cause:** `rtclock_init` runs before the first `set_decrementer`, and the shim was only initialised there. Until then `shim_get_abstime` returned **0**, so the base was 0. The spin then needed the first tick, which cannot arrive with IRQs masked. On Apple hardware the timer always runs; on QEMU the tick happened to come first.
- **Fix:** `shim_get_abstime` calls `shim_init` itself on the first read (stackless: r3/lr/CPSR parked in pc-relative words, IRQ/FIQ masked). In the icount run the kernel now goes on through IOKit, launchd and SpringBoard (`build/hwlogs/qemu-icount-fixed.txt`).
- **Not the cause (checked on the way):** the vector page and page tables. The new probe snapshot reads TTBR0/1, TTBCR, DACR, MPIDR, L1[0xFFF], L2[0xF0], the 8 vector words, the IRQ literal and the first two handler words from the ARM11 view; after a stall the ARM9 walks the same tables in FCRAM and compares. In the icount reproduction both views match (`e59ff018`…, IRQ literal `8006648c`, handler `e51ff004 8084001c`).
- **Tooling:** `emu/gdb/rsp.py` is a minimal GDB remote-protocol client. The Arm gdb 14.2 has no Python and the host gdb has no ARM target, so scripted traces go through this client: `reads.py` logs every clock read, `panic.py` breaks at `_panic`, `postmortem.py` attaches after a hang. The monitor writes all probe values to `log.txt` (`[probe]` lines) after ~5 s without a clock read or console output, so photos are no longer needed.
## 26. Real-hardware boot to SpringBoard, and boot-time performance (2026-11-17)
- **run8 (`9867d28`):** first hardware boot to SpringBoard. Touch, volume and Mail work (docs/hwlogs/run8.txt). It is slow: ~19 md0 requests/s, launchd at ~115 s, input at ~320 s.
- **Why md0 latency is the whole story:** `shim_mdevstrategy` (kernel/shim/blk.c) waits for each ≤32 KiB request with **IRQs off**. The ARM9 only answers between its other jobs. Those jobs were:
  - console output written one byte per f_write + f_sync;
  - a ~330k-pixel screen copy every 16 loop passes;
  - touch over SPI.
- **ARM9 PC samples** (`emu/pcsample9.py`, QEMU with `--icount 2`): 49% `send_command` (SD), 26% `display_blit`.
- **run9 (`763f374`):** chunked log with a 0.5 s sync; the block mailbox is polled inside the blit; one word read per VRAM pixel; screen copies paced (≤15 fps, 2/s while md0 is busy). SpringBoard and input come up in under 83 s (docs/hwlogs/run9.txt).
- **The 30 s boot wait** was `IOKitInitializeTime()` (0x801a8004, called from bsd_init at 0x8024a53e): `waitForService(resourceMatching("IORTC"), {30, 0})`. Nothing publishes IORTC. Found with a SELECT+START thread dump in QEMU during the wait: the bootstrap thread `0x80284664` was blocked under 0x801a8021. The earlier `rtc-wait-0s` patch only covered the later `getGMTTimeOfDay` wait. New patch `iortc-wait-0s` (`movs r3,#30` → `#0` at 0x801a800a). `BSD root` now follows immediately.
- **md0 linger:** after serving a request, the ARM9 polls the mailbox for up to 2 ms (at most 100 ms in total) before running the rest of its loop, so streamed reads go back to back. In QEMU with icount the emulated SD controller dominates, so this needs a hardware run to measure.
- **IORTC timeout 0 was too aggressive** (hardware run10 and one QEMU run): `gltest` measured `0 ms` per frame, so early userland `gettimeofday` deltas were frozen. With timeout 0, `clock_initialize_calendar()` can run before AppleARMPE (gIOPlatform) has registered. The "RTC did not show up" line is missing in those runs, which shows the platform expert's `getGMTTimeOfDay` was never called. The result looks like a race: a clean `--icount 2` run with timeout 0 measured 2486 ms. Patch changed to **2 s** (`iortc-wait-2s`, `movs r3,#2`). AppleARMPE is up within 1 s even in plain QEMU, so the original order is kept, while 28 of the 30 s are still saved. Clean `--icount 2` run: the RTC line is back, gltest takes 2531 ms, md0 starts at ~2 s, and boot reaches SpringBoard and input.
- **Lesson:** build releases from a clean worktree (`ios3ds-wt/rel`). Other agents' uncommitted edits in the main checkout (shim, input daemon) leaked into a test build and produced a broken boot (FS errors, `launch_msg(): Socket is not connected`) unrelated to the change under test.

## 27. PICA200 back end for the GL engine (2026-11-16)
Design and sources are in docs/pica200.md. This entry records what was checked and how.
- **Kernel facts for the GPU service** (`scripts/ksym_lib.py` over the kernelcache symbol table; the `n_desc & 8` bit says Thumb):
  - `copyin` 0x80067c84 and `copyout` 0x80067d04 are ARM. Disassembly shows `copyin(r0 = user address, r1 = kernel address, r2 = length)`, i.e. a 32-bit `user_addr_t`.
  - `IOMallocContiguous` 0x801a70e0, `ml_io_map` 0x80067780, `IODelay` 0x801a6c2c, `IOSleep` 0x801a6c40, `printf` 0x8001c4c4, `proc_selfpid` 0x8014f1be, `proc_find` 0x8014f94c, `proc_rele` 0x8014f862 are Thumb.
  - `_sysctl__children` (top-level OID list) is at 0x8027f7a8.
- **ABI:** iOS lets callees clobber r9; our shim C code is EABI and assumed r9 is preserved across calls into the kernel. The shim is now built with `-ffixed-r9` (`scripts/mkboot.py`). This also covers `blk.c` and the input service.
- **References fetched for semantics:** Azahar `src/video_core` (`renderer_software/sw_rasterizer.cpp`, `sw_clipper.cpp`, `sw_framebuffer.cpp`, `sw_blitter.cpp`, `sw_texturing.cpp`, `pica/pica_core.cpp`, `pica/regs_*.h`) from `raw.githubusercontent.com/azahar-emu/azahar/master`, and the local libn3ds copy (`gfx.c`, `gx.h`, `pdn.h`). Read, not copied.
- **Earlier draft reviewed** (`tools/ios3ds-pica` from the interrupted session): encoders, tiling, texenv mapping and the shader assembler were kept. Three things were wrong or missing and are fixed: the unit test expected a truncated f31 mantissa (the encoder was right); colour-buffer and PPF formats number RGB565/RGB5A1 differently (one enum was used for both); `GL_COMBINE` was not mapped. Its user-mapped register design was replaced by the shim service.
- **Host:**
  - `make -C tools/ios3ds-pica test` → `328 checks, 0 failures`. The last group renders one triangle through driver core + model and checks pixel positions with and without the transfer's flip flag.
  - `make -C tools/ios3ds-gl host-test` → 10 probes ok on both back ends; `scene 1, pica vs software: 27584 of 153600 pixels differ at all, 10 by more than 24`; `scene 2 … 39319 … 320 by more than 24`. Scene 2 (off-target viewport, NPOT texture, two units, alpha test, GL_COMBINE, masks, render-to-texture) found one back-end bug: with a viewport that does not fit, the stale GL scissor box was applied although the scissor test was off.
- **QEMU run 1** (node registered as `kern.ios3ds_gpu`): `[gl] pica: GPU not available: no kern.ios3ds_gpu service`, then `GPU pass skipped`; the software pass and the boot were unaffected. The errno was not logged in that run. The node was moved to the top level (`ios3ds_gpu`) and the transport now logs the failing step and errno.
- **QEMU run 2** (`python3 emu/run.py --time 160 --shots 150,159`, gltest arguments `45 300`):
  ```
  [pica] bring-up: PDN_GPU_CNT 0x0001007f, GPUPROT 0x00000000, GX clock 0x00000000, GX stat 0x00000000
  [pica] arena 8192 KiB at VA 0xcb114000 = PA 0x24684000; driver ready (state 0)
  [gl] pica: GPU up: arena 8192 KiB at PA 0x24684000 (8064 KiB free), PDN_GPU_CNT 0x1007f, PICA200
  [gl] context 0x904c00 uses the ios3ds PICA200
  gltest: GL_VERSION "OpenGL ES-CM 1.1 ios3ds", GL_RENDERER "ios3ds PICA200"
  (10 probes ok)
  gltest: pica vs software: 27584 of 153600 pixels differ at all, 10 by more than 24 (max 229) ok
  gltest: PICA200 PASS (0 failed)
  gltest: drawing into the display surface 320x480, status 0x8cd5
  ```
  The 150 s screenshot shows the animated scene on the top LCD, drawn through the emulated GPU into the display IOSurface. QEMU printed no `picasim:` diagnostic. (`GX clock 0` is the model; hardware should show 0x70100.)
- **QEMU run 3** (full boot, `--time 450 --shots 400,449`, gltest arguments `340 60`): the same `[pica]` / `PICA200 PASS` lines; the 400 s screenshot is the SpringBoard home screen as before, the 449 s one the GL scene drawn through the emulated GPU (10-11 fps including the 50 ms sleep per frame). No `picasim:` diagnostics, no `[pica] TIMEOUT`.
- **QEMU run 4** (committed configuration, gltest arguments `0 0`, with scene 2): `scene 1 … 27584 … 10 by more than 24 … ok`, `scene 2 … 39319 … 320 by more than 24 … ok`, `gltest: PICA200 PASS (0 failed)`: the same numbers as on the build host.
- **Not verified:** anything on a real PICA200. docs/pica200.md §5 says what the model can and cannot show, §8 lists what to look at on the first hardware run.

## 28. GPU display mirror; the PICA200 path reviewed for real hardware (2026-11-17)
Branch `agent/gpu` = `agent/pica` + main. Design: docs/pica200.md §9-11.
- **Why:** entry 26 measured 26% of the ARM9's time in `display_blit`, and the ARM11 kernel waits for md0 with IRQs off while the ARM9 is busy. The screen copy is the one big ARM9 job that a GPU can take.
- **The copy is a scaling.** In the LCD framebuffers' own layout (row j = screen column j, 240 pixels from the bottom up) display.c's rotation disappears: top = iOS (⌊4i/3⌋, ⌊4(j−20)/3⌋) for j 20..379, bottom = iOS (⌊(3i−40)/2⌋, ⌊3j/2⌋) for i 14..226. Written down once as `gm_map()` (boot/common/gpu_mirror.h).
- **Exact nearest sampling:** texcoord iOS x = 4/3·q − 1/2 at render-target column q puts every pixel centre 1/6, 1/2 or 5/6 into a texel, never near a boundary; same for the other three axes (3/2·q − 41/2, …). With truncating sampling (Citra, our model) that reproduces the integer maths exactly.
- **Colour without a BGRA texture format:** iOS bytes B,G,R,A read as RGBA8 (A,B,G,R) give sample.g = R, .b = G, .a = B; three TEV stages with constant colours (1,0,0), (0,1,0), (0,0,1) and MODULATE / MULT_ADD rebuild (R,G,B) exactly.
- **Host test** (`make -C tools/ios3ds-pica test`, `test_mirror`): boot/arm9/display.c compiled for the host (VRAM mapped at 0x18000000 with MAP_FIXED so its 32-bit addresses work) against the engine + model: `mode top/both/bottom: GPU mirror vs ARM9 CPU copy (display.c): 0 of 1566976 bytes differ`, also with a GPU that finishes operations only several polls later (`3 frames needed 72 busy steps`). The self-test catches a deliberately wrong list (`249666 bad bytes … caught`) and an unreachable list times out at the P3D stage.
- **First QEMU boot** (plain, `--time 450`): self-test variant 0 exact, mirror on, home screen on both LCDs. But the model ran each operation inside the MMIO write, so the 52 ms model draw ran inside the VBlank handler: `ARM11 IRQ time 55055 us/frame` — a QEMU artefact, and the multi-interrupt stepping was never exercised. **Fix:** the QEMU GX model now holds back the starting write in virtual time (PSC 100 µs, PPF 400 µs, P3D 1 ms) and runs it from a timer.
- **QEMU, plain, async model** (`--time 450`, X at 405/420/435 s): `[gpu] mirror self-test variant 0 (as modelled): 0 of 518400 bytes differ, guard intact`, ~10 fps (`frame avg ~75000 us`, 6 interrupt steps per frame, `ARM11 IRQ time 129-529 us/frame`), `spot check (GPU copy): 0 of 512 sampled LCD pixels differ` every 30 s, `gltest: PICA200 PASS (0 failed)` while the mirror runs (`3 busy skips`), all three screen modes correct with the console redrawn on the screen given back (build/emu-plain2/screen_415/430/449).
- **QEMU, `--icount 2`** (`--time 300`): CPU copy before the mirror starts: `44 copies in 30 s, 161.5 ms each`, later `103 copies, 56.0 ms each` (ARM9, emulated timing); once the mirror is on (after the GL test), `671 frames in 30 s (22.3 fps)`, spot checks 0/512, `gltest: PICA200 offscreen frame took 1088 ms` vs `2098 ms` software. The GPU stage times in QEMU are the model's delays plus interrupt latency, not hardware figures.
- **Real-hardware review of the existing GPU service** (docs/pica200.md §10): the arena came from `IOMallocContiguous` with whatever dirty lines its previous owner left: a write-back of one of them could land on GPU output at any later time. Now the whole D-cache is cleaned and invalidated once after allocation. boot.cfg `gpu=0` stops bring-up before any register access. The GL engine checks a PPF round trip and a PSC fill through the service before `pica`/`auto` use the GPU, and logs what came back if not. No GIC line was added: completion stays polled, the mirror is stepped from the existing VBlank (42) and timer (29) interrupts.
- **Shim size:** with the mirror the shim ended at +0x741C, past the +0x5F00 limit (probe snapshot). The GPU code (pica.c, pica_drv.c, pica_mirror.c, 16 KiB incl. bss) is now linked at 0x808B0000 and loaded from `gpu.bin` (with the command lists at 0x208B8000); the main shim shrank to 0x3414 and calls the part only through `gpu_glue.c` when its magic is present. The ARM9 clears that magic before the `load=` entries so a stale copy from a previous boot in FCRAM is never called.
- **CPU copy change:** a screen that starts showing iOS is cleared first. Before, the top screen's 20-column side margins kept console text in "both"/"top" mode; the GPU writes them black, and so does the CPU copy now.
- **Not verified on hardware:** everything in this entry. docs/pica200.md §11 lists the log lines that settle each assumption.
- **GPU vs CPU screenshots:** the same build with `gpu_mirror=0` (`[gpu] display mirror disabled (boot.cfg)`, `CPU screen copy: 109 copies in 30 s, 36.7 ms each`): its 400 s and 449 s screenshots are pixel-identical to the GPU-mirror run's (0 of 192000 pixels differ, both screens, "both" mode). Touch is unaffected: the input daemon's panel → iOS mapping (`flipx`, 2/3 scale) was never touched and the picture under the finger is the same.
- **Fallback, injected:** the final build with gpu.bin's list 0 replaced by list 1 (wrong sampling for the model): all 8 variants logged with their first difference, then `[gpu] display mirror FAILED (self-test mismatch): 249666 bytes differed, first at 14403: 01 expected, 00 seen; the ARM9 keeps copying the screen`; CPU copy continues (`spot check (CPU copy): 0 of 512`), gltest still `PICA200 PASS`, and the 399 s screenshot equals the CPU-only run's. The unmodified final build again: variant 0 exact, mirror on, 449 s screenshot identical to the CPU-only run's.

## 29. Swipes never ended: the shim starved vsync (2026-11-17)
- **Symptom (QEMU, viewer and `emu/tap.py --drag`):** after a fast swipe the home-screen page froze half-dragged. Every later touch moved it again and froze again; a tap left the Spotlight "W" key popup up.
- **Not the touch path:** the daemon logs a down and an up for every gesture. GraphicsServices backlogs full-queue sends and re-sends them with a blocking `mach_msg` (`_GSSendEvent` 0x352632c4, thread 0x352638a8). UIKit ends a touch on hand type 6 with proximity bit 1 clear (`_UIApplicationHandleEvent` 0x3381ecf4). Pacing and coalescing the daemon's events changed nothing.
- **Cause:** UIScrollView pages and bounces through `UIViewHeartbeat` → `GSHeartbeat`. `GSHeartbeatCreate` (0x35264a6c) ticks on `IOMobileFramebufferGetVSyncRunLoopSource`, i.e. CLCD frame interrupts. AppleM2CLCD keeps its frame-IRQ enable (+0x08 bit 0, shadow ivar 0x254) set while anyone wants frames, and clears it itself when idle (interrupt action 0x807a9df8). The shim cleared it after every injected VBlank ("one-shot"), and the driver, trusting its shadow, never set it again until the next swap. Once the finger lifted nothing swapped, so the heartbeat never ticked.
- **Fix:** the shim leaves the enable alone and injects on every VBlank while it is set.
- **Result (`emu/tap.py --drag 450:280,240:40,240 --drag 462:40,240:280,240 --tap 475:47,320`):** the page snaps back after the swipe and the tap opens Settings. A 240 pt right swipe in 0.4 s did not page to Spotlight (open).
- `emu/viewer.py`: a mouse release outside the views used to be dropped (pen stuck down); drags now stay in the view they started in and always end. `emu/tap.py` gained `--drag`.

## 29b. Hardware run 12: the PICA200 does not fill or draw; GX clock was never set (2026-11-17)
- **run12** (`m1-21f26fc`, docs/hwlogs/run12.txt): mirror self-test wrong in every variant (first byte 0x80, 0x40 with the read-out flip; ~451k of 518k bytes), GL self-check: `memory fill left 4096 of 4096 words wrong (word 0: 0x5a5aa5a5)`. Fallbacks fine. ARM9 CPU copy: 313-433 ms per frame.
- **Reading the failure:** the GL self-check's transfer round trip passed first, so PPF, addresses (PA>>3) and the cache maintenance work. The fill left the old data, yet its done bit came on. The mirror's read-out produced correct black margins (first 14400 bytes), so its last stage ran; the render targets held wrong data.
- **GX clock:** `GX clock 0x00000000`. Compared the register sequences with libn3ds (`source/arm11/drivers/gfx.c hardwareReset`, `include/arm11/drivers/gx.h`) and Luma (`arm11/source/main.c initScreens`): libn3ds writes `gpu_clk = 0x70100`, clears the VRAM bank-disable bits (+0x030 bits 8-11), sets `psc_dma_prio0/1` (+0x050/+0x054); Luma writes none of them, only PDN_GPU_CNT and the PDC registers. Our bring-up skipped them because "Luma leaves the GPU running" was only checked through PDN_GPU_CNT.
- **Also from libn3ds gx.h:** GX +0x70..+0xBC are memory-traffic counters (PSC external/VRAM reads and writes, P3D vertex/texture/depth/colour, PPF, fills). They show whether an engine touched memory at all, independent of done bits.
- **Changes:** bring-up applies the clock/VRAM/priority setup (boot.cfg `gpu_clk`, default 1). `pica_diag.c` runs staged tests before and after it (fills to VRAM and FCRAM; PPF; P3D constant quad into VRAM and FCRAM; 64x64 and 48x40 textures drawn 1:1) with counter deltas (boot.cfg `gpu_diag`, default 1); docs/pica200.md §12 says how to read them. A failed mirror self-test also dumps a texture tile and a render-target tile with their expected values. The diagnostic lists are built by genmirror (blob version 2).
- **Host:** the diagnostics pass on the model (`diagnostics on the model: 0x7f`); the dump's expectations equal what a passing self-test leaves (`16 of 16`). The model has no traffic counters (they read 0) and does not need the clock register.
- **QEMU** (this build, `--time 450` and `--icount 2 --time 300`): `[pica] GX setup as libn3ds hardwareReset: clock 0x00000000 -> 0x00070100 …`, `[gpu-diag] summary: fill VRAM ok->ok, fill FCRAM ok->ok …; PPF ok; P3D solid VRAM ok, FCRAM ok; texture POT ok, NPOT ok` (the model fills and draws without the clock, so "as found" passes there), mirror self-test variant 0, `gltest: PICA200 PASS`, home screen; screenshots identical to the CPU-copy run in both modes; icount mirror 23.9 fps, spot checks 0/512.

## 30. Run 13's answer: no FCRAM fills, no NPOT textures; the model now agrees; fixes (2026-11-18)
- **run13** (`eab9618`, docs/hwlogs/run13.txt): `[gpu-diag] summary: fill VRAM ok->ok, fill FCRAM FAIL->FAIL; PPF ok; P3D solid VRAM ok, FCRAM ok; texture POT ok, NPOT FAIL`. Counters for the FCRAM fill: `fill0 +256 ext-wr +0` (the engine ran, nothing reached external memory); NPOT 48x40: 0/1920 texels, `tex +256`. The GX clock setup changed nothing (VRAM fills worked before it).
- **Texture tile dump explained** (run13 lines 288-291): decoding the words with the self-test pattern gives iOS pixels (0,479) (1,479) (0,478) (1,478) (2,479) …: a correct linear→tiled upload done *with* the flip flag, i.e. the last variant tried (7). The target tile holds (128..130, 300) throughout: NPOT sampling. The dump now re-runs variant 0 first.
- **Model = hardware:** picasim writes nothing for fills outside VRAM and samples noise from NPOT textures. Proof that it reproduces the hardware: the previous build (c0ee62e) on the new QEMU gives the run13 summary line verbatim, `[gpu] display mirror FAILED (self-test mismatch)` and `[gl] pica: GPU self-check: memory fill left 4096 of 4096 words wrong (word 0: 0x5a5aa5a5)`, the exact hardware message (build/emu-hwmodel); the host tests failed the same way before the fixes.
- **Fixes:** mirror texture 512x512 in the FCRAM arena, filled by a PPF texture copy (60 tile rows of 10240 bytes, output gap 6144) from the 320-wide upload in VRAM; GL clears as P3D quads, depth initialised by CPU write, self-check draws a quad. docs/pica200.md §13.
- **CPU copy** (docs/pica200.md §14): row reads and column writes as 16-byte LDM/STM bursts, 4 pixels packed into 3 words, only the framebuffer set the PDC shows (the shim publishes PDC0/1 +0x78; QEMU: `LCDs show framebuffer set 0 on top, 0 on the bottom`). Host test: byte-identical to the GPU model and to the old copy's mapping.
- **QEMU `--icount 2`, CPU copy only (`gpu_mirror=0`), same boot phases, old (c0ee62e) → new:** 126.4 → 48.1 ms, 181.0 → 54.6 ms, 194.3 → 65.3 ms, 51.5 → 14.4 ms per copy (instruction-clocked: fewer instructions; the hardware gain from bursts and from writing one set instead of two plus a read-back is not visible in QEMU). Screenshots of the three icount runs differ only in the status-bar clock.
- **QEMU with the fixes:** plain 450 s and `--icount 2` 300 s: diagnostics summary as on hardware, `[gpu] display mirror: self-test passed (variant 0)`, `screen copy: PICA200`, `[gl] pica: GPU up (self-check passed)`, `gltest: PICA200 PASS (0 failed)`, spot checks 0/512; icount mirror ~20 fps. Plain 449 s screenshot identical to the CPU-copy run's.
- **Open on hardware:** the PPF texture copy with an output gap and texture fetches from FCRAM. Both are covered by the mirror self-test (failure → CPU copy, logged with a dump).

## 31. Stability soak; md0 no longer freezes the kernel (2026-11-18)
- **`emu/soak.py`:** boots QEMU, waits for `[input] connected to SpringBoard`, then cycles through the page-1 icon grid and the dock:
  - per cycle: tap, settle, Home, page swipes every 4th cycle, lock/wake/slide-to-unlock every 5th, a screenshot per step;
  - recovery when a cycle doesn't end on the home screen (second Home, then Dismiss), recorded per cycle;
  - afterwards the log is scanned for panics, `[probe]` stalls, md0/log errors, crashes, jetsam and SpringBoard restarts.

  Output: `build/emu/soak*/report.md`.
- **Soak 1** (main `15f2a30`, plain QEMU, 40 cycles, ~19 min): no panic, stall, I/O error, crash or memory warning, and SpringBoard never restarted.
  - Cycles 13 and 33 "failed" only because the tap missed an icon and Home on page 1 opens Spotlight (correct iOS behaviour). Hence the recovery step.
  - **Real bug, cycle 0:** the first app launch after boot (Game Center). The input daemon sent Home as down+up in the same poll (`key 40 down @1644` / `up @1644`), yet SpringBoard showed the "Edit Home Screen" alert, its reaction to a Home *hold*. The alert then blocked every tap for ~5 minutes.

    It did not reproduce in 8 quiet Game Center cycles (`--targets 47,50 --settles 2,20`). It needs the busy first launch.
- **Cause (by design reading):** `shim_mdevstrategy` waited for each md0 request with **IRQs off**:
  - ~20 ms per request on hardware, thousands of requests for a cold app launch;
  - meanwhile no timer, scheduler or VBlank ran anywhere in the kernel.

  If SpringBoard handled the Home down, then page-faulted its own code from md0 before handling the up, its hold timer (`menuButtonDown:` → `menuButtonUp:`, re-log 20) could expire during the freeze.
- **Fix (`kernel/shim/blk.c`):**
  - md0 requests are serialized by a kernel mutex. The XNU KPIs come from the kernelcache symbol table:
    - `lck_grp_attr_alloc_init` 0x8001ae22, `lck_grp_alloc_init` 0x8001ae00, `lck_attr_alloc_init` 0x8001ade8, `lck_mtx_alloc_init` 0x80062008 (Thumb);
    - `lck_mtx_lock` 0x80063000 and `lck_mtx_unlock` 0x800631a0 (ARM);
    - `assert_wait_timeout` 0x8001f02a and `thread_block` 0x8001edcc (Thumb).
  - The wait for the ARM9 runs with interrupts on: it spins ~200 polls, then sleeps in 250 µs steps (`assert_wait_timeout` + `thread_block`).
  - The whole buffer is faulted in **before** taking the lock, so no fault ever waits on md0 behind its own request.
  - If the lock can't be allocated, the old IRQs-off path is used.
- **QEMU `--icount 2`:** by the first heartbeat (t=36 s) md0 had served 3687 requests (78 MB), with launchd, gltest and the input daemon already up. Before: ~1100 requests and 32 MB by t≈120 s.
  - Much of that gain is an emulation effect. Single-threaded icount TCG shares one virtual clock between the CPUs, so the spinning ARM11 used to burn the time the ARM9 needed.
  - On hardware the gain is responsiveness: other threads, timers, VBlank and input keep running during disk reads.
- **Soak 2** (this change, 40 cycles):
  - **PASS:** no panic, stall, md0/log error, crash or jetsam; ~9,400 md0 requests served through the new lock/sleep path.
  - Spotlight cases (13, 14, 33, 34) recovered with a second Home.
  - The "Edit Home Screen" alert came back once, again on cycle 0, the first app launch after boot. So the IRQs-off freeze was not the cause.
- **Correction (soak 4, §39):** the alert is **not** a hold. It is SpringBoard's one-time `REORDER_INFO` tip, remembered in the default `SBDidShowReorderText`, which a fresh rootfs (every QEMU soak) doesn't have. The timer analysis below is still accurate, but it wasn't what happened here.
- **Why a Home tap can become a hold** (`SpringBoard` `-[SpringBoard menuButtonDown:]` 0x8bc0, `_menuButtonWasHeld` 0x8874, found through the objc method lists):
  - The down schedules `scheduledTimerWithTimeInterval:… _menuButtonWasHeld` with one of three intervals:
    - **2.0 s** normally;
    - **1.0 s** when a SpringBoard flag is set and `allowAllInBackground` (0x3ff00000);
    - **5.0 s** while a sheet `isOrderedFront` (0x40140000).
  - `menuButtonUp:` cancels it. The daemon queues down and up together, so a hold only happens when SpringBoard needs more than 1–2 s between handling the two GSEvents. That happens on the busy first app launch after boot.
  - The remedy is a faster SpringBoard (GPU compositing, re-log 30 / agent/gpu), not the input path.
  - `emu/soak.py` allows one such "Dismiss + Home" recovery per run and fails on more, or on any "stuck" cycle.

## 32. Audio: "could not start DMA: device is not ready", fixed; PCM reaches CSND (2026-11-18)
- **Symptom (hardware runs 8-11):** each UI sound (a tap, the Home key) logged `AppleEmbeddedAudioDevice: could not start DMA: device is not ready` and no stream ever started. Reproduced in QEMU: two Home presses, two errors (agent/audio2 `build/evidence/run1-before.txt`).
- **Where the message comes from:**
  - The format string `%s: could not start DMA: %s` is in AppleARMIISAudio (0x80612e0c), printed by `startDMA` (0x80611b48) when 0x80611584 fails. The text "device is not ready" is IOKit's string for kIOReturnNotReady (0xE00002D8).
  - The call chain is 0x80611474 → 0x806113c8 → I2S controller vtable 0x358.
- **The PL080 is not the culprit:**
  - `startDMACommand` (vtable 0x354, 0x807d00e4) does return NotReady for channel state ≤ 1.
  - But `_initDMAChannel` leaves channels at state 2, and the boot log shows channels 3, 4 and 5 initialised.
  - State map: 1 = allocated, 2 = ready, 3 = running.
- **The I2S controller's DMA start (AppleS5L8900X kext, 0x8066d448):**
  - For `i2s-version` 0 (the DT has no such property, so the boot log reads "i2sVersion: 0") with frame sync requested, it sets state 1 (0x8066d490), enables its interrupt and arms a timeout.
  - It then sleeps on the state.
  - The interrupt action (0x8066caf0) moves the state 1 → 2 on one edge and 2 → 3 on the next. The timeout (0x8066cb84) sets 4.
  - Anything but 3 returns NotReady; a timeout returns 0xE00002EB.
- **That interrupt is GPIO pin 44:**
  - i2s0's `interrupts` is the string `','` = 0x2c, and its `interrupt-parent` 0x0090c9a0 is `/arm-io/gpio`, not the VIC.
  - So the driver waits for two edges of a pin (presumably the word clock) to start DMA on a frame boundary.
  - The 3DS has no such pin, and the GPIO block is a fake page.
- **Fix:** kernel patch `i2s-start-no-lrck-sync` (`kernel/patches-audio.json`): `mov r3,#1` → `mov r3,#3` at 0x8066d490.
  - The wait loop falls through as if synced. The interrupt is disabled and the timeout cancelled as on success.
  - A DT `i2s-version` ≥ 3 would also skip the wait, but it changes register and clock programming. Not taken.
- **QEMU CSND model** (`emu/qemu/hw/arm/ctr_csnd.c`): plays channels from FCRAM on the virtual clock, ST_ADDR reads the play position, and a stereo mix sink logs `[csnd]` statistics and writes a WAV with `IOS3DS_CSND_WAV`.
- **Test client:** `ios3ds-tone` (AudioQueue via dlopen; left = tone, right = half amplitude).
- **Run 2 (plain TCG):** no more NotReady. The tone stream started (`tx start ch5`) and CSND played L/R at the exact amplitudes, but only 1,914 of 17,640 frames were tone.
  - The fragments are whole 1,024-frame AudioQueue buffers: underruns.
  - QEMU's guest runs about 3x slower than the 3DS (home screen at ~200 s vs ~70 s) while the audio clock is real time.
- **Run 3 (`--icount 2`):** after the first seconds of boot, every 1 s tone is 44,099 frames, gap-free, 999 Hz, peak 5999 L / 2999 R: bit-exact.
- **Volume is not applied in the PCM:**
  - Volume keys change neither the samples nor the codec level.
  - Each press writes one byte to the LM48821 speaker amp: `ab a3 9b 8b 83` down, `8b … c3` up.
  - AppleLM48821Amp setGain (0x807b8108) maps whole dB to a 5-bit step (19 = 0 dB, 31 = +18 dB) and writes `step << 3 | 3`.
  - Its mute (0x807b8048) writes `0x04`; its range is −38..+18 dB (0x807b800c).
  - Run 4: volume-up stops at step 31, volume-down goes 31 … 11 and then mute (`0x04`, plus CS42L58 regs 6 = 0, 0x1a = 0x80).
- **Shim volume mapping:** a step becomes the CSND channel volume `10^((dB − 18)/20)` (step 31 = unity) and `0x04` becomes 0.
  - It is applied to the running channels at the next service.
  - Run 4's recording follows it exactly: step 16 → peak 3007 = 6000 × 16422/32768; step 11 → 239.
- **Headphones:**
  - iOS stays on its speaker route (no `function-hp_detect`), so its volume always arrives as amp steps.
  - The CSND volume is digital, so it applies to either 3DS output.
  - The ARM9 now polls the jack (GPIO2 bit 0, debounced) and repeats the init's `HEADSET_SEL` routing on a change.
- **Run 5 (final code, `--icount 2`, 85 tones):**
  - Home pressed twice: 0 NotReady errors.
  - Volume swept up to step 31, down to mute and back up. Every recorded tone plays at exactly dB(step) − 18: step 22 → −12.0 dB, step 31 → 0.0 dB, step 11 → −34.0 dB.
  - While muted, 28.2 s of digital silence although tones kept playing; the level returned on the next step.
  - The ARM9 logs `[audio] iOS volume: amp step N -> CSND volume V/32768`.
- **Hardware safety:** the patch only skips a wait for a pin the 3DS does not have. The volume path only writes CSND channel VOL. The tone client logs and exits on any AudioToolbox error. Jack polling only runs when the CODEC answered at boot.

## 33. RTC: the calendar from the 3DS MCU (2026-11-18)
Tools: the flat kernel image as an ELF (`llvm-objcopy -I binary -O elf32-littlearm --change-addresses 0x80001000`), `llvm-objdump-18 --triple=thumbv6k-none-eabi`, the LC_SYMTAB dump (`scripts/ksym.py`).
- **Who asks for the time.** `IOKitInitializeTime` (0x801a8004, from bsd_init) = `waitForService(resourceMatching("IORTC"), {t, 0})` (literals 0x801a8028 `resourceMatching` 0x801ae02d, 0x801a8030 `waitForService` 0x801b18f1), then `clock_initialize_calendar` (0x80014ba0, literal 0x801a8034).
- **`clock_initialize_calendar`** (0x80014ba0): `secs = PEGetGMTTimeOfDay()` (bl 0x801c84d8), `commpage_set_timestamp(0, …)` (0x8006ade0, see §31), boot time at 0x80283e40 set once, `clock_get_system_microtime` (0x80069818), epoch at 0x80283dc8 = secs − uptime, offset via `nanoseconds_to_absolutetime`, adjustments cleared (0x80283de8), then `host_notify_calendar_change` (0x80016dcc).
- **`PEGetGMTTimeOfDay`** (0x801c84d8, Thumb): `gIOPlatform` (0x8029d490) ? vtable +0x390 : 0. `PESetGMTTimeOfDay` (0x801c84f4): vtable +0x394. AppleARMPE's vtable (pointer to it + 8 = 0x802f483c): +0x390 = `getGMTTimeOfDay` 0x802e78ad (found by searching the image for that word: one hit, 0x802f4bcc), +0x394 = `setGMTTimeOfDay` 0x802e7835, +0x388 = haltRestart 0x801c8645 (IOPlatformExpert's).
- **AppleARMPE::getGMTTimeOfDay** (0x802e78ac): `waitForService(resourceMatching("IORTC"), {30 s})` (the `rtc-wait-0s` patch), `getProperty("IORTC")` (vtable +0x9c), `OSDynamicCast(IOService)`, then `callPlatformFunction("getGMTTimeOfDay", false, &t, 0, 0, 0)` (vtable +0x1f8). So iOS expects a PMU driver to publish an `IORTC` resource whose value answers that platform function. **setGMTTimeOfDay** (0x802e7834) has the same 30 s wait and was never patched: a `settimeofday()` would have blocked for 30 s.
- **Chosen design:** not a fake IORTC provider (a C++ object with a hand-made vtable in the shim), but the two C entry points: `PEGetGMTTimeOfDay` → `shim_mcu_get_gmt` and `PESetGMTTimeOfDay` → `shim_mcu_set_gmt` (Thumb `ldr r3,[pc,#0]; bx r3; .word`, as for mdevstrategy). The time comes from the ARM9's MCU mailbox (docs/mcu.md), filled before XNU starts. Without a valid RTC the shim runs the original body, so the fallback is unchanged.
- **The IORTC wait:** the literal 0x801a8030 now points at `shim_mcu_wait_rtc`. With an RTC it releases the matching dictionary (vtable +0x14) and returns at once; else it calls `waitForService` with the 2 s of `iortc-wait-2s`. The reason for the 2 s (§26: calendar before AppleARMPE) is gone: the calendar no longer goes through the platform expert. The "0 ms" symptom it was blamed for was something else (§31).
- **QEMU** (plain, `build/emu-r2`): `[mcu] version 13/38: RTC 2026-11-17 23:21:31 Tue (offset 0 s) = Unix 1794957691`, then `ios3ds-mcu: 3DS RTC present: not waiting for IORTC`, `ios3ds-mcu: calendar from the 3DS RTC: 1794957691`; no "RTC did not show up" line any more. SpringBoard shows 3:26 PM and the calendar icon "Tuesday 17" (host clock 23:26 UTC, iOS time zone US/Pacific).

## 34. gettimeofday() froze for up to a second: the commpage reciprocal (2026-11-18)
- **Symptom:** gltest's first QEMU run with the RTC: `gettimeofday … tv 1794957716.452931 -> 1794957716.452931; mach_absolute_time 221564 ticks = 212 ms`, i.e. "frame took 0 ms" again although the calendar was now initialised early with a real time. So neither the IORTC wait nor `secs = 0` was the cause (§26).
- **Userland:** `gettimeofday` (libSystem, dyld cache 0x32ca92c4) calls `__commpage_gettimeofday` (0x32d4b748) first and only falls back to the syscall when it returns non-zero. That reads the commpage at 0x40000040: base abstime (+0), seconds (+8), microseconds (+0xc), limit (+0x10), divisor (+0x14), reciprocal M (+0x18), "add" flag (+0x20), shift (+0x24). Base 0 or `now - base >= limit` → syscall; else `usec += q(now - base)` with `q = umulhi(d, M)`, `add ? ((d - q) >> 1 + q) >> (shift - 1) : q >> shift`.
- **Kernel:** `commpage_set_timestamp` (0x8006ade0) recomputes M/add/shift (Hacker's Delight "magicu") whenever the divisor changes. Its caller `clock_gettimeofday_set_commpage` (0x8006948c) passes `rtclock+0` (ticks per second) as the limit and `rtclock+4` (ticks per µs) as the divisor. Read from QEMU (`xp /16wx 0x20255128`): `0x000ffb0f 0x00000001 0x3b9aca00 0x000ffb0f` = 1047311 ticks/s, **divisor 1**, timebase 10^9/1047311.
- **Divisor 1:** the algorithm overflows: M = 0, add = 1, shift = 0 (Python transcription; for 2 or 24 it is fine). Confirmed in QEMU (`emu/tap.py --hmp "245:x /12wx 0x40000040"`): `0x0f012b4f 0x00000000 0x6afce4c6 0x0002ba6f | 0x000ffb0f 0x00000001 0 0 | 0x00000001 0`. Userland then shifts by −1, i.e. by 255: elapsed time is always 0. gettimeofday returns the value of the last syscall refresh until a second of ticks has passed. Every earlier symptom fits: "0 ms" frames, hardware run12/13 `1063 ms` vs `mach_absolute_time 412 ms` (the first sample was ~650 ms stale), and the QEMU differences between the 2 s and 0 s IORTC waits (just where the frame fell relative to a refresh).
- **Fix** (`commpage-gtod-off`, kernel/patches-mcu.json): the `beq` at 0x8006adfc that skips the update when there is no commpage becomes `b`, so the timestamp is never published, the base stays 0 and userland always uses the (exact) syscall. Cost: one syscall per gettimeofday.
- **After the fix:** plain QEMU `gltest: timing: … tv 1794958858.470788 -> 1794958858.651883; mach_absolute_time 190712 ticks = 182 ms` (181 vs 182 ms); `--icount 2`: 2254 ms both. ios3ds-mcud's cross-check: `RTC +118 s, gettimeofday +117610 ms, mach_absolute_time +117613 ms` (plain), `RTC +153 s, gettimeofday +154086 ms, mach_absolute_time +154084 ms` (icount, `-rtc clock=vm`).
- **mach_absolute_time is right:** the timebase is consistent (10^9 / 1047311), and the shim's clock follows the MPCore watchdog.
- **Still inexact, but bounded:** the kernel's own microtime path (0x8006916c) also adds raw ticks as microseconds inside a one-second window (`rtclock+0x30` base): up to 4.7 % fast for under a second, corrected at the next full conversion.

## 35. Battery: an IOPMPowerSource from the shim (2026-11-18)
- **What SpringBoard reads:** `strings` on SpringBoard (from the rootfs with `build/tools/hfsplus … cat`) shows `IOPMPowerSource`, `CurrentCapacity`, `MaxCapacity`, `ExternalConnected`, `FullyCharged`, `IsCharging`, `ExternalChargeCapable`, `AppleRawBatteryVoltage`, `kIOPMPSAtCriticalLevelKey` ("Shutting down immediately …"); its imports (`arm-apple-darwin10-nm -u`) include `IOServiceGetMatchingService`, `IOServiceMatching`, `IORegistryEntryCreateCFProperty`. The kernelcache has no battery driver the 3DS could use (the n72 PMU is a D1759 on I2C, deleted from our device tree).
- **Kernel KPI (LC_SYMTAB):** `IOPMPowerSource::powerSource()` 0x801bb624 (`new` 0xa8 bytes + `init()` via vtable +0x44), setters `setCurrentCapacity` 0x801bb3fc, `setMaxCapacity` 0x801bb3d0, `setIsCharging` 0x801bb1f8, `setExternalConnected` 0x801bb288, `setExternalChargeCapable` 0x801bb258, `setBatteryInstalled` 0x801bb228, `setVoltage` 0x801bb348, `setPSProperty` 0x801bb0dc, `updateStatus` 0x801bb428 (copies the PS dictionary at +0x54 into the registry with `setProperty` (vtable +0x6c) when +0x50 is set, then `messageClients(0xe0024100)` = kIOPMMessageBatteryStatusHasChanged, vtable +0x238); `IOService::attach` 0x801adc54, `registerService` 0x801b0844, `getPlatform` 0x801ac744; `OSSymbol::withCStringNoCopy` 0x8019a5d0, `OSNumber::withNumber(u64, bits)` 0x80197684; `release` = vtable +0x14.
- **Gotcha:** `kOSBooleanTrue` / `kOSBooleanFalse` (0x80232e4c / 0x80232e50) are C++ *references*: the words hold `&gOSBooleanTrue` (0x8029d168). Passing the word itself made `OSDictionary::setObject` call through a vtable at 0: `sleh_abort: prefetch abort in kernel mode: fault_addr=0x0`, `lr 0x8018e521` (= `OSDictionary::setObject+0x68`), r0 0x8029d168 (QEMU run 1). Fixed by dereferencing twice.
- **QEMU** (`build/emu-r2`, `build/emu-r3`): `ios3ds-mcu: battery 100%, charger, 4200 mV: IOPMPowerSource registered`; the status bar shows a full battery with the charging bolt; after `qom-set /machine mcu-charging off` + `mcu-battery 50` a half-full battery without the bolt; with `mcu-battery 15` SpringBoard's own "Low Battery — 20% of battery remaining" alert.

## 36. HOME, POWER, shell and shutdown (2026-11-18)
- **Input path:** `input_proto.h` already reserved `INBTN_HOME` / `INBTN_POWER`, and ios3ds-input maps them to the Menu (0x40) and Power (0x30) usages. The ARM9 now ORs the MCU state into each input sample (`mcu_buttons()` in `input_poll`).
- **MCU IRQs** (regs 0x10–0x13, clear on read; mask 0x18–0x1B, 1 = masked; libn3ds `mcu.h`): bit 0 POWER press, 1 POWER held, 2/3 HOME press/release, 5/6 shell close/open. POWER has no release IRQ, hence the tap/long-hold scheme (docs/mcu.md §3).
- **Halt:** `PEHaltRestart` (0x801c98b0) waits (30 s deadline, 0x801c98d2) for the shutdown handlers for types 0/1/3, then `gIOPlatform->haltRestart(type)` (vtable +0x388 → IOPlatformExpert's 0x801c8645). The call site at 0x801c98f6 becomes `adds r1,r5,#0; ldr r3,[pc,#0]; b +2; .word shim+0x38`, keeping the original `blx r3` at 0x801c9900; the shim forwards every other type to the platform.
- **Shutdown hang:** sliding "slide to power off" logged `launchd: System: About to call: reboot(RB_HALT).`, then the CPU idled forever (heartbeat PCs all 0x80067b34, the idle WFI). SELECT+START dump, launchd's thread (annotated with `scripts/btmap.py`): `_boot+0x7a` → `PEHaltRestart+0x3e` (the `blx` at 0x801c98ec to 0x801db12b, IOPMrootDomain's halt/restart broadcast) → `messageClient` → IOMobileGraphicsFamily+0x2738 → AppleM2DisplayDrivers+0x4c40 (AppleM2CLCD `disable_backlight`) → AppleARMPlatform+0x4895/+0x6019 → AppleS5L8720X+0x17f0 → `IOCommandGate::runAction` → AppleS5L8720X+0x37cc → `commandSleep`. The S5L SWI backlight function waits for an interrupt that never comes. Patch `pe-halt-skip-pm` (0x801c98ec `blx r3` → `nop`). Then: `[mcu] iOS shut down: log synced, powering off through the MCU`, and QEMU exits.
- **QEMU** (`build/emu-r3`): `h` → `[input] key 40 down/up` and SpringBoard opens Spotlight (Home on page 1); `c` → `[mcu] shell closed: locking iOS (POWER tap)` → `[input] key 30 down/up`; `c` again → `[mcu] shell opened`. The lock press does not blank the screen in this build (SBDontLockEver; re-log §21 saw the same with SELECT), so the shell now also switches the LCD backlights off/on through MCU reg 0x22 (0x14 / 0x28, libn3ds `MCU_LCD_PWR_*`).

## 37. Volume, brightness, accelerometer (2026-11-18)
- **Volume:** Celestial's `AVSystemController` (SpringBoard imports its notifications); selectors present in the dyld cache: `sharedAVSystemController`, `setActiveCategoryVolumeTo:`, `getActiveCategoryVolume:andName:`.
- **Brightness:** SpringBoard's strings `DisplayBrightness`, `DisplayBrightnessFactor`, `…FactorWithFade`, `…Auto`; imports `IOHIDEventSystemSetProperty`. The keys occur once more in the dyld cache (IOKit's HID event system), so SpringBoard keeps its backlight state as HID event system properties; `IOHIDEventSystemClientCopyProperty` (0x337fce40) exists for clients. Kernel side: `AppleARMPWMBacklight` / `AppleARMBacklight` drive the n72 PMU through `function-backlight_*`, which our device tree does not have.
- **Accelerometer:** the kernelcache has `AppleEmbeddedAccelerometer` (LIS302DL / LIS331DLH on I2C, IOHID event service); SpringBoard has `SBAccelerometerInterface` and `com.apple.springboard.rawOrientation`. `IOHIDEventCreateAccelerometerEvent` (0x337faca4): `(alloc, timestamp r1:r2, x r3, y [sp], z [sp+4], options [sp+8])` → the common 3-axis creator with type 13.
- **QEMU** (`build/emu-r2/3`): `[mcud] volume slider 40/63 -> iOS volume 0.63`; `[mcud] iOS brightness 0.50 x factor 1.00 -> LCD backlight 500/1000` → `qom-get lcd-backlight` 79 (0x4F) and the dimmed screenshots; accelerometer `raw -16384 0 0 -> iOS 0 -1 0 g` (portrait) and `16384 0 0 -> iOS 0 1 0` after `]` `]`.
- **Accelerometer events are ignored by SpringBoard.** Its HID callback's type switch (0x51b1c: `IOHIDEventGetType` − 3, jump table; type 3 → 0x5028c keyboard, type 13 → 0x4fb44) and the accelerometer handler 0x4fb44 (reads fields 0xd0000–0xd0003; flag 0x18d3d1 = `CFPreferencesGetAppBooleanValue("SBIgnoreAccelerometerEvents")`, set at 0x50bf8; posts `com.apple.springboard.rawOrientation` at 0x4fef8, the name resolved from the literal at 0x4fed4) were never hit in 30 s of events: hardware breakpoints through QEMU's gdbstub (`-gdb tcp::12347`, `emu/gdb/rsp.py`). Same as keyboard/digitizer events in §19/§22. The handler is also reached from 0x50054, which polls accelerometer *services* with `IOHIDServiceCopyEvent(service, 13, …)`; the way in is therefore a kernel service (Apple's LIS331DLH driver on an emulated I2C device), not a client. The daemon keeps the accelerometer off unless started with `acc`.

## 38. iOS shows the 3DS clock's time (2026-11-18)
- The 3DS RTC keeps **local** time (it's set in the 3DS System Settings). The MCU driver (§33) hands it to XNU as UTC, and iOS then displayed it in its default zone, `/private/var/db/timezone/localtime` → US Pacific: RTC 00:27 Wednesday showed as 4:29 PM Tuesday.
- rootfs/recipe.json now replaces that symlink with `rootfs/files/localtime-UTC`, a 54-byte TZif v1 file generated for this project (one ttinfo: offset 0, "UTC"; no Apple data). §50 replaced it with a symlink to `/usr/share/zoneinfo/UTC`: as a regular file it made CoreFoundation scan all of zoneinfo in every process.
- iOS now shows exactly the 3DS clock: QEMU RTC 00:38 → status bar "12:38 AM", calendar "Wednesday".
- To get a true UTC epoch plus a real zone instead, set `rtc_offset=` in boot.cfg and pick a zone in Settings (docs/mcu.md).

## 39. The "Edit Home Screen" alert was iOS's one-time tip (2026-11-18)
- **Soak 4** (main `e9ddbba`, all drivers merged, 40 cycles):
  - no panic, stall, md0/log error (19,008 md0 requests), crash or jetsam;
  - again only cycle 0 showed "Edit Home Screen" after leaving the first app.
- **Cause:** the alert's strings are `REORDER_INFO_TITLE` / `REORDER_INFO_BODY` (SpringBoard.app English.lproj), and SpringBoard keeps the default **`SBDidShowReorderText`** next to them.
  - iOS shows the tip once, on the first Home out of an app on a fresh device, then sets the key.
  - QEMU soaks always start with a fresh rootfs, hence cycle 0 every time. On a real card it shows once and is remembered.
  - The §31 "Home seen as a hold" explanation was wrong.
- **Fix:** rootfs/recipe.json's com.apple.springboard.plist now sets `SBDidShowReorderText = true`.
- **Soak recovery** is now second Home, then Dismiss alone, then Dismiss + Home. Pressing Home after Dismiss on page 1 opens Spotlight, which soak 4 recorded as "stuck".
- **The same soak showed a GPU problem:** `mirror self-test: GPU operation 4 of 4 (PPF transfer) did not finish, register 0x00000000` in plain (MTTCG) QEMU. It was later reported as "self-test mismatch: 0 bytes differed". The CPU copy carried on correctly. Handed to the GPU work (agent/gpu).

## 40. Accelerometer: Apple's LIS302DL driver on an emulated sensor (2026-11-18)
- **Node:** `/arm-io/i2c0/accelerometer` (`compatible 'accelerometer,lis331dl|accelerometer,lis302dl'`, `reg 0x1d`, `interrupts 0x63` on `/arm-io/gpio`, `orientation 4`). dt_audio deleted it with the PMU. The personality `AppleLIS302DL` (provider AppleARMIICDevice, match `accelerometer,lis302dl`) lives in com.apple.driver.AppleEmbeddedAccelerometer at 0x8061a000 (`scripts/kexts.py Accel`). The kext is ARM code: disassembled with `--triple=armv6k`, and literal loads annotated with LC_SYMTAB names and kext strings.
- **Base class AppleEmbeddedI2CAccelerometer** (start at ~0x8061c5xx): an `IOTimerEventSource` (+0x8c, action 0x8061c314), a provider-less `IOInterruptEventSource` (+0x80, action 0x8061bf98) and one on the I2C nub's interrupt 0 (+0x78, action 0x8061c418). The interrupt one must exist or start fails. `/arm-io/gpio` already has `AppleS5L8900XGPIOIC` running on its fake page (`AppleS5L8900XGPIOIC::start: gpioBaseAddress …` in every boot log), so registration works; the interrupt itself never fires.
- **Polling:** `setProperties` → `ReportInterval` (0x8061b988): values 1..9999 become 10000 (µs); then the timer is armed (vtable +0x8c) or cancelled (+0xb4). The timer action reads a sample through vtable +0x3f8 (0x8061b1b8), dispatches it (+0x3c0) and re-arms with the interval minus the time spent. So no interrupt is needed while SpringBoard wants data.
- **LIS302DL methods** (vtable at 0x8061f7c4: +0x3f4 enable 0x8061b2e4, +0x3f8 read 0x8061b1b8, +0x3fc serviceInterrupts 0x8061b428). I2C helpers: read 0x8061be38, write 0x8061bed8, and a write with sub-address bit 7 (auto-increment) at 0x8061be80, all through the nub's transfer (vtable +0x35c).
  - Probe: read `WHO_AM_I` (0x0F); only the transfer result is checked.
  - Enable: `CTRL_REG1` (0x20) |= 0x40; `CTRL_REG2` (0x21) = 0x40 (BOOT), then poll 0x21 every 500 µs until bit 6 clears, `panic` after 1000 tries ("Boot bit did not return to zero"); `CTRL_REG3` (0x22) |= 0xC0.
  - Read: `OUT_X/Y/Z` 0x29/0x2B/0x2D, one byte each, signed. Value × 1187 = 16.16 fixed g (≈ 55.2 counts per g). Then the DT `orientation` flags: bit 3 swaps x/y, bits 0/1/2 negate x/y/z.
  - serviceInterrupts: `FF_WU_SRC_1` (0x31) bit 6.
- **Emulation** (`shim_mcu_iic`, tools/ios3ds-mcu/shim/mcu.c): `shim_iic_transfer` (kernel/shim/audio.c) asks it first.
  - The nub is recognised by its `reg` property (`getProperty("reg")`, vtable +0x9c; `OSData::getBytesNoCopy` 0x8018d534), cached per nub.
  - BOOT reads back cleared, `WHO_AM_I` = 0x3B, `STATUS` (0x27) = 0x0F, `FF_WU_SRC` = 0, outputs from the MCU mailbox as in docs/mcu.md §6.2. `CTRL_REG1` bit 6 switches the ARM9's sampling.
  - The shim has no divide instruction and no libgcc, so it has its own shift-subtract divide and integer square root.
- **QEMU** (`build/emu-a1`: home screen only): probe at boot, sensor powered on once SpringBoard is up, but no samples. On an iPod touch the home screen does not rotate, so nobody sets `ReportInterval`. `build/emu-a2` (Safari opened by a tap): samples start at Safari's launch; SpringBoard's `com.apple.springboard.rawOrientation` goes 0 → 1, → 4 for model pose 1, → 3 for pose 3, → 1 for pose 0. Safari's bookmarks view rotates both ways and back.

## 41. Lock turns the screen off: SBDontLockEver removed, clock-controller wait patched (2026-11-18)
- SpringBoard reads `SBDontLockEver` with `CFPreferencesGetAppBooleanValue` at 0xb42c (Thumb; CFString at 0x15848c, found by resolving `ldr/add pc` pairs against the string's address) into a global flag, next to `SBDontLockAfterCrash` and others. With it set, a lock press (`key 30`) leaves `DisplayBrightnessFactor` at 1 (§21, §36).
- **Without it, first try** (QEMU `build/emu-l1`…`l5`): `key 30` → factor 0 → LCD backlight 0; HOME → factor 1. But the screens kept showing the frame from before the lock (clock frozen), and iOS blanked again after ~10 s unless touched. That was its lock screen, which never reached the display. The fake CLCD page (`xp /4wx 0x1ffba000`) read `00000003 00000010 …` before the lock and `00000002 00000000 00000000 00000001` after the wake: AppleM2CLCD had not re-enabled the controller.
- **Cause:** the same wait as the shutdown hang (§36): AppleS5L8720X+0x37cc, in `AppleS5L8720XClockController`, is a loop `while (byte +0x38c) commandGate->commandSleep(+0x38c)` after starting a performance-state change (flags from vtable +0x39c). The clear comes from a completion interrupt the fake clock/power page never raises. Display sleep lowers the performance state, so the CLCD's power path blocked there.
- **Patch `s5l-perf-nowait`** (0x807517cc): `ldrb r3,[r5,#0x38c]; cmp r3,#0; bne` → `mov r3,#0; strb r3,[r5,#0x38c]; nop`.
- **Result** (`build/emu-l6`): CLCD `00000003 00000010` after the wake. The lock screen shows "1:32 Wednesday, November 18", the charging battery and "slide to unlock". The slide returns to the home screen, and the clock advances (1:33, 1:34). The rootfs drops `SBDontLockEver`; `SBDontLockAfterCrash` and `SBDisableAutoDim` stay.

## 42. "Where is my backlight?" flood after lock/wake (2026-11-18)
- **Soak 6** (main + accelerometer + lock, 40 cycles): PASS, but the last `[hb]` line was at t=150 s, because the console was never quiet. After the first lock/unlock, `AppleM2CLCD::safe_enable_backlight(), Where is my backlight?` appeared 3,504 times, about 3 per second.
- **Three AppleM2CLCD functions** share the format string `%s, Where is my backlight?` (VA 0x807b0b58, literal refs 0x807a9564/95c4/965c):
  - the level setter (0x807a9514);
  - `disable_backlight` (0x807a9570);
  - `safe_enable_backlight` (0x807a95d0).

  Each looks up the backlight service (`bl 0x807a9490`). When there is none, which is always on the 3DS, each only `IOLog`s (0x801a6be9) and returns. The flow is otherwise harmless; the cost was console/SD traffic. The 3DS backlight is driven by ios3ds-mcud through the MCU (§36, §41).
- **Patches** `clcd-backlight-log-set` / `-disable` / `-enable`: the `blx r3` (IOLog) at 0x807a9558, 0x807a95b0 and 0x807a9648 becomes `nop`.
- **The heartbeat stopped after ~100 s in every soak.**
  - In QEMU, 70% of console lines are empty: 1,603 of them after t=111 s in soak 7, arriving in triples, starting right after `iBoot version`. Hardware logs (run9, run13) have none, so it is a QEMU-side artefact.
  - On top of that, the daemons now print real text every few seconds.
  - Fix: the monitor counts only non-whitespace console bytes as activity, and adds a heartbeat every 30 s regardless. QEMU: heartbeats at t=10, 20, 33, 61, 76, then every 30 s to the end.

## 43. Screen-off power saving; no system sleep; the CLCD backlight retry (2026-11-18)
- **System sleep with no way back:** after a lock without `SBDontLockEver`, iOS slept the whole system after ~60 s (`System Sleep`, then AppleARMPL192VIC's `vic[0] ena 00010000`); the HOME press later reached nobody (QEMU `build/emu-p1`). The iPod wakes from the PMU interrupt, which the 3DS does not have. `IOPMAssertionCreateWithName("NoIdleSleepAssertion", 255, …)` (IOKit 0x337f0d78; the type string occurs 6 times in the dyld cache) held by ios3ds-mcud prevents it (`[mcud] NoIdleSleepAssertion held`; 94 s locked, then wake fine).
- **Where the screen-off cost was:** the ARM9 monitor loop ran ~1700 iterations/s with the screen on and ~4400/s with it off (no copies to do, so it spun faster), reading the touch ADC over SPI each time. The ARM11 still took the VBlank (GIC 42) at full rate, although nothing was injected with the CLCD off. Changes and numbers: docs/mcu.md §9.
- **ARM9 WFI:** the monitor runs with CPSR I/F masked (`cpsr=600001d3` in every log). ARM9 timer 1 (prescaler 64, 2 ms) with IE bit 9 asserts the IRQ line; CP15 c7,c0,4 returns without taking the exception; IF is cleared by hand. QEMU's arm946 halts in WFI and `arm_cpu_has_work` wakes it on the pending line. Result: the ARM9 vCPU thread goes from 39 % to 2 % host CPU.
- **`safe_enable_backlight`** (AppleM2CLCD, ARM, 0x807a95d0): `bl 0x807a9490` (find the backlight) → NULL → `IOLog("%s, Where is my backlight?", "void AppleM2CLCD::safe_enable_backlight()")` and return, leaving byte +0xea0 (enable pending) set; the success path clears it (0x807a9634). Called per frame while pending, i.e. ~3/s for the rest of the wake (soak 6). Patch `clcd-backlight-retry-off` at 0x807a963c: the three IOLog argument loads become `mov r3,#0; strb r3,[r4,#0xea0]; b epilogue` (the IOLog call at 0x807a9648 is skipped, so main's nop of that call can coexist).

## 44. Notification LED (2026-11-18)
- MCU register 0x2D takes a 100-byte pattern stream (3dbrew "MCU Services"; libn3ds `McuInfoLedPattern`, `MCU_REG_INFO_LED`): step time, smoothing, loop delay, unknown, red[32], green[32], blue[32]. The QEMU model already treated 0x2D as a stream register; it now records the pattern (`mcu-led-rgb0`, `mcu-led-steps`).
- ARM9: boot.cfg `notify_led=1` → red blink at ≤ 10 % battery without charging, off otherwise and before a power-off. QEMU: `[mcu] notification LED blinking red: battery low` (16 of 32 steps lit, 0xA00000), `[mcu] notification LED off` once the charger is back. The timing bytes are unverified, so the default is off.

## 45. Wi-Fi end to end in QEMU: an AR6014 + access point model (2026-11-18)
- **Goal:** prove the whole chain (ARM9 driver → network mailbox → `kern.ios3ds_net` → ios3ds-wifid → iOS networking) without a console. Before this, Wi-Fi had only host unit tests.
- **Model** (`emu/qemu/hw/arm/ctr_wifi.c`, docs/wifi.md §7): TMIO controller 2 at 0x10122000, an SDIO card (CCCR/FBR/CIS, function 1), the AR6014 BMI ROM and HTC/WMI firmware behaviour, and an access point with its own WPA2-PSK authenticator, bridged to `-nic user` (slirp). Power from CFG11_WIFICNT (mapped over the board's CFG11 page) and GPIO 2_2/3_12 (a small GPIO block at 0x10147000).
- **Written from the references, not from our driver:**
  - ath6kl `target.h`: function-1 addresses (`HOST_INT_STATUS` 0x400, `RX_LOOKAHEAD_VALID` 0x405, lookahead 0x408, `COUNT` 0x420, `COUNT_DEC` 0x440, window 0x474/0x478/0x47C). `htc.h`: `ENDPOINT1` = 0, so the BMI credit is `COUNT_DEC` + 4·4 = 0x450, the same register GBATEK calls COUNT4. `sdio.c`: one credit read before each BMI command, lookahead-valid wait before BMI reads, window address LSB written last.
  - dsiwifi (runs on real DSi/3DS hardware): 2-byte WMI command header, enumerated auth/crypto values (WPA2-PSK = 5, AES = 4), BSSINFO/CONNECT/DISCONNECT layouts, `00 1C` data header, `0x4000 - len` mailbox addresses, `HTC_MSG_READY` handshake.
  - IEEE 802.11i (PRF-384, EAPOL-Key, message checks), RFC 3394 key wrap, hostapd's RSN IE comparison.
- **First boot against the model:** the unchanged driver went SDIO → BMI → upload → HTC → WMI → scan → associate → 4-way → keys → link up in 1.6 s of virtual time. Both installed keys were equal to the AP's (`WMI_ADD_CIPHER_KEY pairwise ... equals the AP's TK`, `group ... equals the AP's GTK`).
- **Driver changes** (the model exposed some; the references require the others):
  - WMI and data use the endpoints from the CONNECT_SERVICE responses (status checked), not fixed 1/2.
  - The RX trailer is stripped only with `HTC_FLG_RX_TRAILER` (bit 1). Before, any flag counted.
  - Time-outs on the monitor clock: firmware start 3 s, HTC_READY and WMI_READY 5 s, scan 10 s. A missing HTC_READY used to wait forever in state `htc`.
  - A disconnect takes the link down and reconnects after 5 s. Before, it reconnected at once (a tight loop with a wrong PSK). Three failed handshakes log a psk hint.
  - **FIFO self-test:** after "SDIO up" the driver writes a pattern into the function-1 scratch registers with CMD52 and reads it back with one CMD53. If that fails, the mailbox falls back to CMD52 per byte. Reason: dsiwifi's 3DS ARM11 build reads TMIO2's data FIFO at +0x200000, not +0x10C, and nobody has tried +0x10C from the ARM9.
- **iOS side, seen in QEMU:**
  - `IOEthernetControllerGetIONetworkInterfaceObject` + "BSD Name" gave nothing (`[wifid] interface ? up`). IONetworkStack names the interface 20+ s later (`still no BSD interface with our MAC after 20 s`, then `interface is en0`). The daemon now finds it by MAC in `getifaddrs` AF_LINK.
  - **configd configures en0 by itself.** The default network set does DHCP, IPMonitor adds the default route and mDNSResponder gets the DHCP DNS server. In the first run the daemon never ran `ipconfig` (no name yet), yet en0 had 10.0.2.15. `ipconfig set` is now only a fallback after 15 s without an address.
  - `ios3ds-nettest` (new; `nettest=` in wifi.cfg, passed through the mailbox, protocol version 2):
    ```
    [nettest] en0 address 10.0.2.15 netmask 255.255.255.0 (after 11 s)
    [nettest] default route via 10.0.2.2 (interface index 2)
    [nettest] dns example.com -> 172.66.147.243
    [nettest] http 10.0.2.2:8080/ios3ds-nettest -> HTTP/1.0 200 OK | ios3ds-wifi-test-aa4ee772 path=/ios3ds-nettest (185 bytes, 0 s)
    [nettest] all steps passed
    ```
    The host's HTTP server logged `GET /ios3ds-nettest`.
- **Status-bar Wi-Fi icon: not reachable.** SpringBoard's Wi-Fi state comes from configd's `WiFiManager.bundle`. Its strings show it only drives an `IO80211Interface`/`IO80211Controller` through Apple80211 ("Could not obtain IO80211Interface bsd name"), so an IOUserEthernet `en0` stays invisible to it.
- **Unverified on hardware** (docs/wifi.md §6 lists the log line that settles each):
  - the ARM9 powering the chip and driving TMIO2;
  - the ARM9 FIFO at 0x1012210C;
  - the stub setting `hi_board_data_initialized`;
  - Type4 firmware answering HTC/WMI as modelled;
  - BSSINFO from an all-channel scan with BSS filter 1;
  - capability bits 0 in the firmware's association RSN IE;
  - dsiwifi's post-REG_DOMAIN host-interest write, which we skip.

## 46. CoreAnimation's compositor on the PICA200 (2026-11-18)
Design and RE notes: docs/gpu.md §7.
- **RE (QuartzCore in the user's dyld cache, 8C148):** helper scripts resolved `__picsymbolstub4` stubs through the indirect symbol table and disassembled all of `__text` (ARM). `M2Display::M2Display` (0x346de5b8) probes `eagl_context(2)`, then `(1)`, storing the API at `+0x17c` and the surface format at `+0x180` from a literal pool: `'BGRA'` (0x346de6a4) for API 2, `'L565'` (0x346de6a8) for API 1. `GLESContext::make_buffer_current` (0x3470d274) binds the display IOSurface as a 0x84F5 texture via `attachImage:toCoreSurface:invertedRender:NO`, NEAREST/CLAMP, and attaches it to an FBO. `glGetString` is called with `GL_EXTENSIONS` and 0x8A1E (0x3470dd18/dd24).
- **Runs (QEMU, plain):** (1) `glBindTexture(0x84F5)` and `glGetIntegerv(0x84F8)` errors, `CoreAnimation: rendering error 500`, black screen with one icon; (2) after accepting 0x84F5: GL error in `GetString` (traced with the new `IOS3DS_GL_TRACE`: `engine+0x49bc` → `_api_GetString`); (3) no errors, still black but one icon; (4) cause: `glFlush` was a no-op and `glFinish` synced only the bound framebuffer while CA flushes with FBO 0 bound — the GPU frames never reached the IOSurface; the one icon came from a draw that fell back to the CPU rasterizer. With `flush_all` the home screen appears.
- **Pixels:** home screen GPU compositor vs software compositor, same moment: 2181 of 192000 screenshot pixels differ, by at most 1. Spotlight, keyboard, page swipes render the same way (later frames differ only where the two runs are at different animation times or the status-bar clock differs). `--icount 2`: same.
- **Gate:** `[gl] CA scene check: GPU vs CPU rasterizer: 6 of 9216 pixels differ by more than 24 (max 102)`, `[gl] CA compositor: OpenGL ES on the PICA200 (GPU self-check and scene check passed)`; with boot.cfg `ca_gpu=0`: `[gl] CA compositor: software (boot.cfg ca_gpu=0 or gpu=0)` and the software compositor runs as before.
- **Cost (QEMU):** per CA frame on the GPU path, render thread CPU 9.7–15.2 ms (plain), 19–27 ms (`--icount 2`), with one read-back of the whole display surface (~616 KiB) per frame. SpringBoard CPU in 60 s sampler windows during page swipes: GPU 4.97 / 7.05 s, software 4.16 / 5.40 s, i.e. ~25 % more in QEMU. QEMU runs CA's software compositor at host speed and models GPU operations with fixed delays, so this says little about hardware; the per-frame read-back (PPF + copy-out + per-pixel byte rotation) is the known overhead and the next thing to remove (docs/gpu.md §7). The `[gl] CA:` lines measure it on hardware.
- **App launch / exit:** my own scripted taps right after the home screen came up opened nothing in *either* compositor (identical behaviour, not a rendering issue). `emu/soak.py --cycles 6` on the GPU compositor (merged main): **PASS**, apps open (Game Center, Photos "Updating Library", Contacts, …; app-vs-home difference 39–99) and Home returns to an identical home screen (difference 0.0) every cycle; no panic, crash, stall or mirror failure; `[gl] CA: 600 frames; per frame … render thread CPU 8933 us, 1.0 command lists, 1.0 read-backs (604 KiB)`.

## 47. Mirror self-test "mismatch" that was a timeout (soak 4) (2026-11-18)
- **Report (soak 4, plain MTTCG, main e9ddbba):** `[gpu] mirror self-test: GPU operation 4 of 4 (PPF transfer) did not finish, register 0x00000000`, then `self-test FAILED (0 bytes differ)` and `FAILED (self-test mismatch)`. The failure dump right after shows upload, texture and target exactly as expected: the read-out was late, not wrong. "4 of 4": the self-test mirrors one framebuffer set (upload, texture copy, draw, one read-out); live frames have 5 operations with two sets.
- **Why late:** the QEMU GX model completes an operation from a virtual-time timer that needs the BQL; the shim's self-test polled GX registers in a tight loop (each MMIO read takes the BQL). On a busy host (several QEMUs) the timer can wait a long time. Measured with the merged (rel) QEMU on a loaded host: upload 44871 ticks, read-out 75494 ticks (~45/75 ms) for operations modelled at 0.4 ms — the 200 ms limit had a margin of ~3. Not reproduced as a failure in this worktree.
- **Fixes:** 1 s per operation; thread-context waits poll in ~1 ms slices with a 10 µs `IODelay` between; the QEMU model runs due operations when a GX register is read; `pm_selftest` returns `PM_SELFTEST_TIMEOUT` with the operation and register, logged as `self-test aborted, GPU operation N did not finish` and reported as `GM_WHY_TIMEOUT` (ARM9: `display mirror FAILED (GPU timeout: operation N of a frame did not finish, register …)`). Host test: a hung GPU gives `PM_SELFTEST_TIMEOUT`, operation 3.
- **After the fix (plain QEMU, this worktree, merged main):** self-test read-out ~55 ms → passes with margin; mirror active in plain and `--icount 2` boots.

## 48. Timer interrupts while the screen is off (2026-11-18)
- **Shim counters** (`xp /24wx 0x20840040`: +0x4C timer IRQs, +0x58 deferred FIQ retries, +0x78 VBlanks, +0x80 clock reads, +0x8C `set_decrementer` calls, +0x98 IRQs): with the screen on, 87 timer IRQs/s, 296 decrementer sets/s, 1363 clock reads/s. With it off, 140, 415 and 2055. No deferred retries in either.
- **Sampling** (`set_decrementer` = shim+0x18, hardware breakpoint through QEMU's gdbstub, frame-pointer backtraces 18 deep):
  - User frames end in `usleep` (dyld cache 0x32c9f117), called from ios3ds-input 0x70bc and ios3ds-mcud 0x7008 (both ARM `bl`, read with `llvm-objdump --macho`), or in `sleep` (0x32ce31ef, ios3ds-wifid).
  - Kernel-only chains go through `thread_call_allocate+0x2bc` (0x80025019, the thread_call worker's block), through `assert_wait_deadline+0x212`, and through `STRDUP+0x764` (0x8005c52d).
  - 0x8005c4f0 is XNU's `serial_keyboard_poll`: a loop calling the UART rx hook (0x8006b004 with 0,1,0,1), then `clock_interval_to_deadline(16, …)`, `assert_wait_deadline`, `thread_block`, i.e. 62.5 wakeups/s forever, with nothing to read (our rx-ready patch always says no). The boot-arg `serial=3` (bit 1 = keyboard) started it; `serial=1` keeps the serial console without it. After the change the chain is gone from the samples, and console output is unchanged.
  - The thread_call worker's function call (`blx r8` at 0x80024fa6) is mostly BSD networking callouts: `ip_output+0x86f4`, `net_del_domain+0x5c` and `+0xb8`, `in6addr_local+…`, `sodisconnect+0xb9a`, `netboot_root+0x4` (nearest exported symbols). The mix is the same with the screen on and off; only their share of all timer arms doubles when off.
- **No CoreAnimation:** no SpringBoard or QuartzCore user frames among the timer arms, so the guess in §43 was wrong.
- **Changes:**
  - `serial=1`;
  - ios3ds-input at 10 Hz while `screen_off` (read from the MCU mailbox; samples queue in the ring, and hold timing now counts real sleep);
  - ios3ds-mcud at 4 Hz while off.
- **Result** (QEMU): off 146 IRQs/s (143 timer) instead of 245; on 127 instead of 144; ARM11 vCPU thread 10 % instead of 14 % host CPU while off. Table in docs/mcu.md §9.

## 49. Microphone: iOS records the 3DS mic (2026-11-18)
- **No input at first:** AudioSession reported input available 0, route "", and `AudioSessionSetActive(PlayAndRecord)`/`AudioQueueNewInput` failed with `'nopi'`. An iPod2,1 records only from a wired headset.
- **Codec side:** AppleCS42L58Audio's input data sources come from `audio0` properties (0x80660294): `line` always, `imic` with `ain2b-intmic`, `emic` with `ain2a-extmic`. AppleEmbeddedAudio names them (0x80655a04). Adding `ain2b-intmic` alone changed nothing.
- **Routing:** the decision is made by mediaserverd's HAL plug-in `/Library/Audio/Plug-Ins/HAL/VirtualAudio.plugin` (found through the HFS catalog; stripped).
  - It picks `RoutingSettings_<product>` by matching `hw.model` against a 4-byte table at file offset 0x287210 (`K48 K93 K94 K95 K66 N18 N72 N81 N82 N88 N90 N92 N94`).
  - N72 has no built-in mic port.
  - Patch (`rootfs/recipe.json`): the N81 slot (iPod touch 4G) reads `N72`, the N72 slot `X72`. Result: input available 1, route `SpeakerAndMicrophone`, speaker output and amp volume unchanged.
- **Data path:**
  - The 3DS MIC block (0x10162000, GBATEK's DSi layout) runs at I2S2 / 4 = 11,901 Hz.
  - First version: drained by the ARM9 monitor loop. In QEMU it overran the 16-word FIFO constantly. It now runs in the shim (ARM11), which services every 1 ms while the mic is on; the 3DS's own mic service also runs on the ARM11.
  - The ARM9 powers the CODEC ADC while the shim says iOS records. `boot.cfg mic=0` keeps everything off.
  - Safety: no data for 250 ms, a FIFO that never empties, or the block switching itself off each lead to one log line, mic off, and silence.
- **Findings on the way (QEMU):**
  - iOS's input chain high-passes the input and adds a fixed +23.65 dB (×15.23; an 8000 sine clipped). The shim takes it off again (`AUD_MIC_GAIN_Q15`).
  - The 1 ms service deadline also has to hold while the RX channel is idle between two queued commands.
  - The mic must stay on across such gaps (1 s hold); switching it off there punched 50 ms holes.
  - With main's older md0 code (interrupts off while waiting), services stalled and samples were lost. Fixed by rebasing onto the md0-with-interrupts change.
  - Some recordings were corrupted, always bit-identically, by isolated spikes while the shim's own output was clean. Cause: the L/R word phase was carried over from the previous input stream; after an odd-length stream every frame straddled two. The phase now comes from the buffer address.
  - A 10 ms write-ahead of the input ring, tried first, made no difference and was removed.
- **Result (`IOS3DS_MIC=sine:440:1000`, real FIFO, icount 2):** five of five recordings in RecordAudio (run 16) and five of five in PlayAndRecord with the speaker output running (run 17) fit 440.03 Hz at amplitude 1003.0, residual 41 dB. 0 overruns, 0 underruns, 0 resyncs.
- **Bit-exactness:** iOS's own filtering makes app-level bit-exactness impossible. The host test shows it below iOS: a counter through the shim's FIFO drain and RX emulation arrives as an exact scaled, interpolated ramp, L = R in every frame, including after an odd-length stream.

## 50. Boot time and cold app launches under `--icount 2` (2026-11-18)
- **Instrumentation** (docs/boot-perf.md §1):
  - ARM9 `ioprof=1/2` (`[io]` and `[blk]` lines; the shim passes `xnu_bytes` and `buf_flags` in blkmbox line 1) and `log_ts=1`;
  - `[boot]` phase stamps (the ARM9 clock now starts at a9_main, not at the monitor);
  - QEMU `N3DS_PROF`, which samples `CPU(cpu)->halted` and blkmbox `seq`/`done_seq` (PA 0x20846020/0x20846040) every 1 ms of virtual time;
  - `emu/bootprof.py` and `emu/hfsmap.py`. The latter is a small HFS+ parser: volume header forks, catalog leaf records (data and resource forks), extents overflow, journal.
- **Timestamp pitfall:** the first stamp version put the stamp before a line's first character. The console sends `\r` and blank lines long before the text, so an `[input] down` was stamped 1.6 s early. The stamp now goes before the first visible character.
- **5 s with nothing running** after `/dev/md0 on /` (10.7–15.7 s): no md0, ARM11 0 %. launchctl imports `_IOKitWaitQuiet` (strings of /bin/launchctl, extracted with hfsplus) and calls it with a 5 s timeout.
  - `ios3ds-diag -iobusy` walks the IOService plane: `IORegistryEntryCreateIterator(root, "IOService", recursive)`, then `IOServiceGetBusyState` and `IOServiceGetState`, all through `dlsym` because the stub SDK lacks them. It found `clkrstgen` and `wdt` (AppleARMIODevice, state 0xa: matched, not registered) busy 1, and AppleS5L8720XIO busy 2.
  - SELECT+START dump at 13 s, annotated with scripts/btmap.py: two matching threads in `IORecursiveLockSleep` ← `IOService::waitForMatchingService+0xc0` ← `waitForService+0x2e`:
    - ← AppleARMPlatform+0x26f3/+0x2831 ← AppleS5L8720X+0x2f68 (`AppleS5L8720XClockController::start`);
    - ← AppleARMPlatform+0xa017 ← AppleS5L8900X+0x9d6c (`AppleARMWatchDogTimer` start via super::start).
  - **Clock controller:** 0x80750f38 formats `"function-core_voltage_%ld"` (literal 0x8075109c) and calls the lookup (literal 0x807510a4 = 0x802e4815). The lookup reads that property of the provider (vtable +0x94 getProperty), takes the phandle, builds a matching dictionary and waits with no timeout. A NULL result goes to `panic("…Bad DPSM Tables in Device Tree")` (0x807510a8, `_panic` 0x80015c7c).
  - **Watchdog:** 0x802ec006–0x802ec014: `waitForService(serviceMatching("IOPMU") [0x802f089c], 0)`. On NULL it skips to 0x802ec02e: it only reads `IOPMUBootDebug` and then parses `-s`.
  - **Patches** (kernel/patches-boot.json):
    - `wdt-no-pmu-wait`: 0x802ec014 `blx r3` → `movs r0,#0`;
    - `clkctl-voltage-quiet`: the literal → shim +0x28 `shim_quiet_clk_function` (kernel/shim/quiet.c). It calls `IOService::adjustBusy(-1)` (0x801aef24) on `IORegistryEntry::fromPath("IODeviceTree:/arm-io/clkrstgen")` (0x801aae60), calls the original lookup (still endless), and adds `+1` if it returns.
  - Result: `[iobusy] 0s: 0 of 80 services busy`; wifid starts at 10.9 s instead of 16.8 s.
- **zoneinfo scans** (hfsmap): 339 reads of /usr/share/zoneinfo/* at 19, 32 and 57 s; Settings' cold launch had 454 of its 571 chunks there. Apple's `/private/var/db/timezone/localtime` is a 30-byte symlink (staging rootfs). The re-log 38 regular file makes CoreFoundation look for a zone file with the same contents. Now a symlink to `/usr/share/zoneinfo/UTC` (recipe `symlink`, xpwn `hfsplus symlink`).
- **Mail at boot:** `[smp]` showed MobileMail with 1.7 s of CPU after boot. Its Info.plist has `UIBackgroundModes = (audio, continuous)`. SpringBoard's strings include `autoLaunchIfNecessaryOnBootOrInstall:` and `Not autolaunching Mail since the SBDisableMailAutoLaunch default is set`, so the recipe sets that default.
- **md0 sizes:**
  - `mdevioctl` (0x80072a14) answers MAXBLOCKCOUNT/MAXSEGMENTCOUNT read/write with 32 (0x80072a9e). MAXBYTECOUNT gets ENOTTY, so `vfs_init_io_attributes` (literal pool 0x80087920–0x8008797c, mount fields 0x8b8–0x8d8) fails and the mount keeps MAXPHYS = 128 KiB.
  - Page-in runs from the VM pre-heat reach 1.7 MiB. The 800 KiB read at launchd start is the shared cache's code signature (cache offset 0x9ee4000, after LINKEDIT 0x8646000+0x189e000).
- **Numbers:** docs/boot-perf.md §2. Rebased on 13ee8ec (microphone): settled at 26.5 s. soak.py with 20 cycles in plain QEMU, including the 130 s lock: PASS.

## 51. CoreAnimation frames without the CPU read-back (2026-11-18)
Design: docs/gpu.md §7 ("Presenting without the read-back"), docs/pica200.md §15.
- **Before:** each GPU-composited CA frame was copied back by the CPU: PPF tiled→linear into arena scratch, `READ` into the IOSurface, per-pixel A,B,G,R → B,G,R,A rotation; ~616 KiB per frame (§46).
- **Now:** a swizzle pass on the GPU (one quad, three TEV stages: `tex.a·(1,0,0) + tex.r·(0,1,0) + tex.g·(0,0,1)`, alpha `tex.b`) writes iOS BGRA byte order into an arena buffer, and the new shim op `PICA_OP_PRESENT` (protocol 3) PPF-transfers it straight into the caller's IOSurface. Surface render targets get a power-of-two colour buffer (run 13: NPOT textures sample garbage on hardware) so the swizzle pass can sample it.
- **Page-table walk in the shim:** the request carries a user VA; the shim reads TTBR0/TTBCR, walks L1 (section or coarse) and L2 (small or large page) for every page, and accepts only user read/write pages (AP = 3, APX = 0) that are physically contiguous and inside FCRAM or VRAM; then cleans+invalidates the range by VA (`c7,c14,1`) and runs the transfer.
- **First run (plain QEMU):** refused: `[pica] present: destination 0x00325000 (+614400) not usable: not user-writable (… L1 257ca001 L2 18000823 … TTBCR 2 DACR 00000001 …)`. L1 0x257ca001 = coarse table at PA 0x257ca000, domain 0; L2 0x18000823 = small page, PA 0x18000000, AP = 2 (user read-only), APX = 0; TTBCR.N = 2 (TTBR0 covers 0–1 GiB), DACR = client for domain 0. XNU's ARM pmap emulates the modified bit: writable pages stay AP = 2 until the first write faults. CA had never written the surface since mapping it (the GPU had). The engine now writes one word per destination page right before `PRESENT`. The read-back fallback kept the screen right meanwhile.
- **Second run:** `[pica] present: GPU writes the caller's surface directly: user VA 0x00325000 = PA 0x18000000 (VRAM, 600 KiB)`: CA's display IOSurface is AppleM2CLCD's scan-out framebuffer at the start of `/vram`, contiguous. `[gl] CA: … 1.0 GPU presents, 0.0 CPU read-backs (0 KiB)` from then on. Same lines in `--icount 2`.
- **Pixels:** screenshots GPU vs software compositor at the same moments: home screen 1528 (plain) / 1582 (icount) of 192000 pixels differ, 43/49 by more than 24, all inside the status-bar clock (the runs show different times); the rest by ≤ 8. Spotlight with keyboard after 22/50 swipes: same, plus the text cursor. Mid-swipe frames differ where the runs are at different animation positions.
- **Cost, render thread per CA frame (`[gl] CA:` line):** `--icount 2`: 6.3–7.2 ms over 1200 frames (four 300-frame windows), was 19–27 ms with the read-back (§46). Of that, ~2.7 ms is the shim polling for the QEMU GX model, which delays every P3D list by 1 ms and every PPF by 0.4 ms of virtual time (guesses, not hardware numbers); a CA frame is ~1.3 lists + the swizzle list + one PPF. Plain QEMU: 23 ms (the picasim rasterizer runs on the host while the vCPU polls; meaningless for hardware).
- **Cost, SpringBoard CPU in 60 s sampler windows during page swipes** (same method as §46, plain QEMU, same build, `ca_gpu=1` vs `ca_gpu=0`; 10 and 5 swipes in the two windows):
  - GPU: 6.00 / 6.48 s (user 2.10 / 1.89, system 3.90 / 4.59)
  - software: 4.39 / 5.30 s (user 2.62 / 3.50, system 1.77 / 1.80)
  
  User time is 20–46 % lower on the GPU path; the extra system time is the shim waiting for the GPU model (above). In QEMU the GPU path therefore still costs more in total; on hardware the waits are real GPU time (well under the modelled 1 ms for a 320x480 quad) and could become sleeps.
- **`--icount 2`, 50 swipes:** SpringBoard 7.24 s for 24 swipes on the GPU path (0.30 s/swipe, user 0.17) vs software 17.86 s / 20 swipes and 17.21 s / 21 swipes (0.89 / 0.82 s/swipe, user ~0.8). Not a like-for-like number: the bench paces swipes in wall time, and with `sleep=off` guest time advances differently in the two runs (the GPU run's 50 swipes fell into 40 s of guest time, the software run's into 150 s), so the software run animated more frames per swipe. Per-frame numbers above are the reliable ones.
- **Fallbacks:** a refused or failed `PRESENT` logs once and returns the process to the read-back; `IOS3DS_GL_NO_PRESENT=1` forces it; boot.cfg `ca_gpu=0` still selects CA's software compositor. Host test runs gltest twice (normal, and with every eligible target presented through `IOS3DS_GL_PRESENT_ALL=1`): PASS.
- **Unverified on hardware:** the PPF writing VRAM the LCD path reads (the GPU mirror's own read-out works this way in QEMU only), the swizzle TEV stages, the walk's descriptor reading (ARM ARM v6 layout), and that no outer cache sits between the CPU's cleaned lines and the GPU. Each fails into the read-back, not into a wrong screen: a refused walk or GPU error directly, a wrong swizzle or row order through the present check (until one non-uniform frame matches, each present is compared with the CPU read-back of the same frame; a mismatch logs `GPU present check FAILED` and switches the process to the read-back; host test with R/B-swapped presents: caught, gltest PASS).

## 52. Wi-Fi in iOS Settings and the status bar: an Apple80211 bridge in configd, a keychain for securityd (2026-11-18)
- **Goal:** Settings > Wi-Fi lists networks and joins one with a password typed on the on-screen keyboard; the status bar shows bars. Before, en0 (IOUserEthernet, §45) worked but iOS did not know it was Wi-Fi ("No Wi-Fi" in Settings, no bars), and the network had to be preset in `wifi.cfg`.
- **Who decides:** configd's `WiFiManager.bundle` (MobileWiFi's server; Settings and SpringBoard are its clients). It only drives an `IO80211Interface` whose parent conforms to `IO80211Controller`, through Apple80211 ioctls `SIOCGA80211`/`SIOCSA80211` and a PF_SYSTEM 802.11 event socket. Apple80211 is linked statically into WiFiManager (and a copy into IPConfiguration).
- **Design** (agent/wifi3, docs/wifi.md §10): the ARM9 driver keeps the chip idle without `wifi.cfg` and executes `scan`/`join(ssid, PMK)`/`leave` from a control area `wlanmbox_t` (NET_PA+0xC200, sysctl `kern.ios3ds_wlan`); `ios3ds-a80211.dylib`, inserted into configd with `DYLD_INSERT_LIBRARIES`, interposes the IOKit lookups, `SIOCGIFMEDIA`, the Apple80211 ioctls (~25 request types, layouts from WiFiManager's code) and the event socket. The QEMU access point adds two decoys, NeighborNet (WPA2) and CoffeeShop (open).
- **Seen in QEMU, step by step:**
  - WiFiManager's logging switched on from the dylib (`LoggingEnabled`/`LoggingFile` = `/dev/console`, now `IOS3DS_WIFI_LOG=1`): `Could not obtain IO80211Interface bsd name`. `WiFiDeviceCreate` reads "BSD Name" 31 times 10 ms apart and gives up; configd's InterfaceNamer names the IOUserEthernet interface 20+ s after ios3ds-wifid publishes it. The first-match notification is now deferred (it gets a class that never matches) and, once en0 exists, a timer on the run loop of the client's notification port hands the callback an `IOBSDNameMatching(en0)` iterator.
  - Some boots: Settings still "No Wi-Fi", no WiFiManager ioctl at all. Two configd clients register for IO80211Interface, in either order; the dylib only remembered the last one. Every registration is served now; run loops come from `CFRunLoopAddSource` of the port's source. Then: `Device power state = ON`, `Device RSSI bounds (-95, -30)`, ~20 GET types, Settings: "Wi-Fi: Not Connected".
  - Settings was killed while launching: CrashReporter (read from the md0 image after the run) `0x8badf00d`, "Settings failed to launch in time", main thread in UIKit layout, 10 s CPU. SpringBoard's `SBNoWatchdogs` (string in SpringBoard) is now set in com.apple.springboard.plist. Taps right after the home screen appears are sometimes ignored; Home first, then tap, works (wifi_ui_test.py retries).
  - Wi-Fi page: `Async scan requested by "Preferences"` → `[a80211] scan requested` → `[wifi] iOS: scan` → `[a80211] scan done: 3 networks` → `Completed Apple80211ScanAsync (0)`; list: CoffeeShop (open), ios3ds-qemu and NeighborNet (lock icons).
  - Join: `No password for ios3ds-qemu`, `Failed to join(-100)`, Settings: "Incorrect password". WiFiManager reads the password from the keychain (`SecItemCopyMatching`, service `AirPort`): -25300, and the keychain database had no rows. A probe `SecItemAdd` from configd returned **-25308** (errSecInteractionNotAllowed): no keychain write ever worked on ios3ds.
  - securityd (8C148): items are CCCrypt'ed with a random key that AppleKeyStore wraps (selector 10, 32 → 40 bytes; 11 unwraps) with the class key from the system keybag; keybagd is disabled (§18: FATAL KEYBAG ERROR) and there is no IOAESAccelerator ("unable to access hwaes key", the 0x835 key, request layout at securityd 0xaacc: 76 bytes). New `ios3ds-hwaes.dylib` in securityd: AppleKeyStore open/KeyWrap/KeyUnwrap (RFC 3394 with a software key; host check against the RFC test vector) and IOAESAccelerator selector 1 in software. Probe: `SecItemAdd -> 0`.
  - Join again: `SecItemCopyMatching -> 0`, `Attempting to join WPA network`, `[a80211] associate "ios3ds-qemu" (WPA2-PSK, PMK from WiFiManager, auth 1/8)`, `[wifi] EAPOL event 1/2`, `[wifi] link up`, `Joined: ios3ds-qemu`, `Added ios3ds-qemu to list of known networks`, ~30 s later `Address: 10.0.2.15`. Settings shows the check mark, the status bar the Wi-Fi bars (also on the home screen).
- **Association result race** (found by the test's second run): WiFiManager disassociates (SET 22) right before associating (SET 20); the watcher saw state idle before the ARM9 had taken the join and reported "failed" (GET 21 = 9) while the join went on. The result is now judged only after `ack_seq` reaches the join's command number.
- **Display state:** WiFiManager logs `Device display is OFF` at every boot and keeps auto-join off (and would run its 30-minute disassociation timer on battery): it reads notify state `com.apple.iokit.hid.displayStatus`, which IOKit's HID session code (IOHIDSessionCreate) handles on an iPod and nobody posts on ios3ds. The dylib mirrors SpringBoard's `com.apple.springboard.hasBlankedScreen` into it.
- **Keyboard:** iOS 4's keyboard rows are at y 291/347/400/457 (measured on a Spotlight screenshot); after `-` the 123 page stays up. `emu/session.py` remembers the page between calls.
- **Test:** `emu/wifi_ui_test.py` does it all through the touch screen: 8/8 PASS in 358 s (docs/wifi.md §10.5). A second boot of the same image, nothing touched, joins the known network by WiFiManager's auto-join (password from the keychain) and shows the bars. `emu/wifi_test.py --case ok` (HTTP fetch, preset network) and `--case nocfg` (no firmware, no `wifi.cfg`) still PASS; `make test` passes; `emu/soak.py --cycles 10` PASS (no panic, stall, md0 error, crash or jetsam; Home always back; boot to SpringBoard + input 60 s).
- **Security:** the keychain key is a constant in the dylib, so the keychain on the SD card is not bound to the console (docs/wifi.md §10.3).
- **Unverified on hardware:** docs/wifi.md §10.6 (open scan without probed SSID, RSSI units, link-loss events, timing on a slow console).

## 53. New 2DS XL (issue #1): the shim's .bss was never zeroed (2026-11-19)
- **Log from the issue** (docs/hwlogs/n3ds-issue1-run1.txt, build fe45ac4): ARM11 MPCore r0p5, SCU/SMP on, XNU up to `BSD root: md0`.
- **The fault:** `kernel abort type 4: fault_type=0x1, fault_addr=0xa5ff7918` at pc 0x80063000 (`lck_mtx_lock`), r0 = 0xa5ff7910, lr 0x80840bac (shim_mdevstrategy). The md0 mutex pointer was garbage.
- **Cause:**
  - `blk_lock` / `blk_lock_tried` are zero-initialised statics in the shim's `.bss`.
  - kernel/shim/link.ld had `.bss` as its own NOBITS output section, so `objcopy -O binary` (scripts/mkboot.py) left it out of shim.bin. The ARM9 loads shim.bin and nothing more.
  - `.bss` therefore held whatever RAM had: zero on the Old 3DS and in QEMU by luck, not on the New 2DS XL.
- **Fix:** `.bss` input sections now go into the `.data` output section, so shim.bin carries them as zeros (23088 → 23300 bytes). The GPU part already did this. QEMU boot unchanged.
- **Next on the New 3DS, if it boots further:**
  - the `[hb] clock: … ratio` line shows whether the MPCore timers run at the assumed 134 MHz PERIPHCLK (the New 3DS can run the ARM11 at 804 MHz);
  - the New 3DS has an L2 cache (L2C-310).

## 54. Volume "does not change" on hardware (run14): output was 18 dB too low (2026-11-19)
- **run14:** the slider changed iOS's volume (`[mcud] volume slider ... -> iOS volume`) and the HUD showed, but the shim saw no I2C traffic between stream starts, only `amp step 13`/`19` at start and `amp off` at stop. The user heard only keyboard clicks.
- **QEMU (repeating 6000-amplitude tone, keys and `qom-set /machine mcu-volume`):** the CSND recording's level follows both, e.g. peak 936 → 353 after five volume-downs, 2650 at slider 63. Since the VirtualAudio N81 patch (§49), iOS applies media volume in software and leaves the amp at a fixed step 19 (0 dB).
- **The bug:** the shim's amp mapping still took step 31 (+18 dB) as CSND unity, so all media played 18 dB too quietly. Full volume gave a peak of 2650 for a 6000 tone: barely audible on the 3DS speaker, while UI clicks remain short and percussive.
- **Fix:** `AUD_AMP_REF_STEP` 31 → 19: step 19 = unity, louder steps clamp, step 13 (UI sounds) = −12 dB.

## 55. Hardware run 15: depth tests drew nothing, the arena filled up, NPOT textures repeated their padding (2026-11-19)
Design: docs/pica200.md §16, docs/gpu.md §7.
- **Run 15** (`f45dc07`, `ca_gpu=1`): gltest's cube was drawn only with `GL_DEPTH_TEST` off (all 9 variants: drawn iff depth off), scene 1 GPU vs CPU 12070 pixels off by > 24 (max 230), scene 2 24900. CA ran on the GPU and passed its scene check (0 of 9216), but after ~2 min of use `[pica] arena full: 1048576 bytes wanted, 828928 free in 78 blocks` → `[gl] pica: GPU arena full (texture): sampled as opaque black`, then a 128 KiB request failing on every frame. The user's photos (Settings): black rectangles behind cells, light stripe garbage in the grouped table background.
- **Depth, cause:** the back end stored GL depth as is (`DEPTHMAP` scale = far − near, offset = far; `glClear` = quad at z/w = 0 → depth exactly 1.0) with `GL_LESS`. Hypothesis: the PICA200's conversion of depth 1.0 to 24-bit wraps to 0, so every clear writes "nearest" and nothing passes. Modelled in picasim (`fragment`: depth ≥ 1.0 → 0, one quirk line); with the old convention the host gltest then fails with scene 1 **12070 / max 229** and the cube missing, i.e. run 15 to the pixel. Everything homebrew does on hardware avoids 1.0: citro3d's `C3D_DepthMap(true, -1, 0)` + `GPU_GREATER` (far = 0).
- **Depth, fix:** `pica_raster_t.depth_conv`: reversed by default (stored = K(1 − d), K = 1 − 2⁻¹⁰; scale −K(far − near), offset K(1 − far − bias); LESS↔GREATER, LEQUAL↔GEQUAL; new buffers start at 0). A **depth probe** at bring-up draws a depth scene (clear, LESS with a hidden and a visible quad, GREATER) under each convention against the CPU rasterizer and logs which match; reversed is used if it matches, else scaled standard, else depth-tested drawing goes to the CPU. Host: `reversed 0 ok, standard 0 ok, unscaled (run 15) 4608 FAIL`.
- **Arena, cause:** not a leak (deletes free, exited clients are reaped when full). CA keeps one texture per visible layer image, many padded to 512×512 = 1 MiB; each render target also held a full-size linear staging buffer used only during transfers (1 MiB for the display); the mirror holds 1 MiB. 8 MiB does not hold it all, and nothing ever gave memory back.
- **Arena, fix:** LRU eviction of texture copies on a failed allocation (re-uploaded from the engine's image on next use; the current draw's textures stay), staging buffers only for the duration of a transfer, and if memory still cannot be had the process writes back its GPU images, frees its arena memory and draws on the CPU from then on (one line). Black is no longer drawn for lack of memory. Shim's `arena full` line rate-limited. `[gl] CA:` line: `GPU memory: N KiB held (T textures, K KiB), arena F KiB free, E evictions`. Host gltest: 24 × 1 MiB textures through the 16 MiB model arena, each drawn twice: 0 of 48 wrong, 39 evictions, GPU on.
- **NPOT + `GL_REPEAT`:** GPU textures were already power-of-two in memory, but a padded NPOT texture with REPEAT repeated at the padded size (coordinates scaled by w/pw). Such an axis is now stretched to the power of two on upload. CA scene check gained a 20×12 repeated 2D layer and a 20×12 rectangle layer, limit 1 %: old code 750 pixels (fails), new 6.
- **QEMU (rel's QEMU binary, i.e. the old depth model), `--icount 2`, 300 s, ca_gpu=1:** `depth probe … reversed (citro3d) 0 ok, standard 0 ok, unscaled (run 15) 0 ok: using reversed (citro3d)` (the old model cannot tell them apart — the new picasim does; QEMU picks it up at its next rebuild), `CA scene check: 6 of 9216`, `CA compositor: OpenGL ES on the PICA200`, present check passed, home screen up, no panic.
- **QEMU soak, `emu/soak.py --cycles 10` (plain, ca_gpu=1): PASS** (no panic/crash/stall, apps open and Home returns). The model's arena filled too (`[pica] arena full` 4 lines, then one every 256th failure): `[gl] CA: 300 frames … GPU memory: 3444 KiB held (62 textures, 1508 KiB), arena 3595 KiB free, 210 evictions`, at 600 frames `2415 KiB held (60 textures, 1503 KiB), 4624 KiB free, 313 evictions` — bounded, no black, no CPU fallback. Seen on the way (old behaviour, not changed here): one app surface was not physically contiguous (`present: … not usable: not physically contiguous`), and `P.no_present` then switched the *whole* process to CPU read-backs (0.0 presents, ~1 read-back/frame afterwards); per-surface refusal would keep the display on `PRESENT`.
- **Next hardware log should show:** the depth probe line (expected `unscaled (run 15) … FAIL`, the others `ok` — confirms the wrap; if `reversed` fails too, depth goes to the CPU and the line says so), gltest `cube centre is drawn … ok` and all cube variants drawn, the CA scene check with the NPOT layers, and every 300 CA frames the GPU-memory figures with evictions instead of `sampled as opaque black`; at worst one `this process draws on the CPU from now on` line.

## 56. Wi-Fi firmware from the console in one GodMode9 step; `[wifi-diag]` bring-up log for the first hardware run (2026-11-19)
Design: docs/wifi.md §3 (firmware), §6/§6.1 (diag lines, model guesses). Run 14 had `[wifi] 0:/ios3ds/wifi-fw.bin: not found (-4): Wi-Fi off`.
- **Firmware, user side:** `tools/ios3ds-wifi/ios3ds-wifi-fw.gm9` (shipped by `mksd.py` as `sd:/gm9/scripts/`): `find 1:/title/00040130/00002d02/content/*.app` → `ask` → `extrcode` → `0:/ios3ds/nwm.code`. Read-only on NAND (no `allow`, no SysNAND write permission asked); commands checked against GodMode9's `scripting.c`/`HelloScript.gm9` (`extrcode` decrypts the NCCH and decompresses `.code`). GBATEK: 00002d02 is the normal-mode NWM on all 3DS models.
- **Firmware, loader side (`nwmcode.c`):** without `wifi-fw.bin`, the ARM9 reads `nwm.code` into the top 4 MiB of FCRAM (before XNU runs), undoes BLZ in place if needed, finds GBATEK's literal pools (newer large pool; older large pool with type1/4 from its tail or the small pool), accepts text base 100000h or 10000h only when stub data/stub code/database are exactly 38h/316h/1E8h, builds the 'I3WF' container and zeroes the FCRAM copy. GBATEK's pool tables re-read for this entry: offsets match field by field (also `nwmfw.py`, which gained the older-pool tail). Host tests: the C loader and `nwmfw.py` produce byte-identical containers for newer/older/older-tail pools, type1/4/5, plain and BLZ; non-NWM input and wrong sizes rejected by both.
- **Direct NAND read rejected:** needs an eMMC driver, CTRNAND AES with the console-unique keyslot (state depends on the boot chain), NCCH keyslot 2Ch and FAT; nothing of it testable in QEMU, and GodMode9 does it once already.
- **`[wifi-diag]`** (wifi.cfg `diag=1`, default): one line per step with ms since power-up — CFG11/GPIO before→after, CFG9_SDMMCCTL, SD_OPTION, CMD5 tries + both R4 values, RCA/CCCR/bus width, function-ready polls, CIS MANFID, BMI version + register values, per-part upload time, BMI_EXECUTE result, time to `hi_board_data_initialized`, raw HTC_READY, per-service status/endpoint, raw WMI_READY, REG_DOMAIN, GET_CHANNEL_LIST as a channel mask, scan request, one line per network (SSID, BSSID, MHz/channel, RSSI, SNR, frame type, security) for the first 3 scans, SCAN_COMPLETE time/status/counts, unhandled events (≤ 24), `WMI command … rejected` always, longest driver step ≥ 20 ms, a summary line. A failure logs the stage, dumps the host interest area (after the upload), holds the chip in reset; iOS/md0 unaffected.
- **Timeouts added:** warm-reset wait no longer blocks (100 ms on the clock), 30 s watchdog over the bring-up, iOS scan without SCAN_COMPLETE published after 10 s, HTC service failure now goes through `fail()`.
- **QEMU** (own QEMU build: rel's `build/qemu` has no slirp, `-nic user` fails): `emu/wifi_test.py --case nwm` PASS (BLZ `nwm.code` → `firmware from 0:/ios3ds/nwm.code: 58774 bytes (BLZ-compressed), main type4, 31 ms`, bring-up OK in 1836 ms, scan lists the 3 model networks, link up, DHCP, HTTP fetch); `badnwm` (one red line, card never powered), `nofw`, `nocfg`, `ok` PASS; `make test` passes. Not run this time: `wifi_ui_test.py --nwm` and the 10-cycle soak (cut for time).
- **Next hardware log should show** (with `nwm.code` on the card): the `fw:` pool line (type4 size vs GBATEK's three), `power:` readbacks, `sdio: CMD5 xN` — the first unverified step; wherever it stops, the `FAILED in stage …` line plus the lines above it name the register or step (table in docs/wifi.md §6). If it gets through, the `scan:` lines are the network list Settings should show.

## 57. Hardware run 17: the console could not be woken after many locks (2026-11-19)
- **Run 17** (main 427680e, `ca_gpu=1`, `powersave=0`): many POWER and shell close/open locks. Every screen-off logged `[gpu] screen copy: ARM9 CPU again (GPU frames stopped …)` and every wake `screen copy: PICA200` again a moment later: the mirror did resume each time. The shim skips frames while AppleM2CLCD's CLCD is disabled (display off), so "frames stopped" was only the display being off. The busy skips (207 → 225) are VBlanks that found a GL request on the GPU; `busy` = 1 is only held around one synchronous request (bounded by the driver's 1 s timeouts), so it does not leak.
- **What did break:** the ARM9 monitor loop rate. 57-59 passes/s until `[monitor] screen mode: bottom` (X: log on the top LCD), then 34, 1 (during a wake) and 15. With the log console visible, every scrolled line repainted the whole console inside the log call: 30 × 50 glyphs, 64 pixels each, into both framebuffer sets, one function call and three byte writes into VRAM per pixel. The log is busy (touches, audio, `[smp]` dumps), so the loop, and with it `input_poll`/`mcu_poll`, ran about once per second. The last presses were not seen (no `[input] key` after the final lock).
- **Fixes:**
  - (`console.c`) once the monitor loop runs, log text only goes into the buffers; the loop paints 2 changed rows per pass. `draw_char` writes glyph columns directly, with no call per pixel. Boot-time output is still drawn immediately.
  - (`display.c`, `main.c`) the CPU screen copy also polls the pad, touch screen and MCU (every 8 columns, at most every 10 ms) alongside md0.
  - (`gpumirror.c`) `gm_live(now, display_on)`: a display that is off is not "frames stopped" (no log, no CPU copy), and after a wake the GPU gets 1 s to resume. Before, one 150-350 ms CPU copy ran on every wake.
  - (`kernel/shim/pica.c`) a VBlank that finds GL busy marks the mirror frame pending. New GL requests wait until the next interrupt that finds the GPU free has started that frame; the flag is cleared whenever the mirror is not active. In QEMU the first lockburst run showed 7 one-second CPU fallbacks *with the display on* while CA kept the GPU busy; with this change there were none.
- **QEMU, `emu/lockburst.py --rounds 12`** (new: X for the log console, then rounds of POWER lock/wake, shell close/open and four rapid POWER taps; `powersave=0` as on run 17): **PASS**. 17 screen-offs, 16 wakes, 8 of 8 shell events seen, no `ARM9 CPU again`, the GPU copy never dropped, and iOS on screen at the end. Plain QEMU draws the console at host speed, so the loop rate there (≥ 544/s) cannot show the hardware cost; run 18's `[power] … loops/s` lines will.
- **Settings > Wallpaper** shows an empty grey page in QEMU with `ca_gpu=1` **and** with `ca_gpu=0` (software compositor), so it is not a GPU or arena problem. Not investigated further here.
- **Run 17's depth probe:** `reversed 0 ok, standard 4608 FAIL, unscaled 4608 FAIL`. Reversed depth works on hardware: gltest's cube is drawn now. The scaled standard convention also fails on hardware, which the depth-1.0-wraps model in picasim does not reproduce, so the model is still incomplete there. 4608 = half of the 96×96 probe.


## 58. Headphone jack: iOS switches its route (2026-11-19)
Design: docs/audio.md §3, §4, §6, §8 item 3.
- **Path:** ARM9 polls GPIO2 bit 0 (debounced, as before) → `aud_mic_t.jack` → shim `hp_service` → pin 0x0d00 of the fake GPIO page (word +0x1a4 bit 0) + VIC0 source 12 injected like the DMA interrupt → AppleEmbeddedAudio's rebuilt `function-hp_detect` / `audio0` interrupt (`dt_audio.py`). Started from the parked WIP `99f3a33`; its QEMU GPIO hook moved out of `ctr_wifi.c` into a 4-byte window in `ctr_csnd.c` (machine property `headphones`).
- **QEMU run 1** (`--icount 2`, diag image, `ios3ds-tone` 12 × 12 s, toggles at 150/200/240/270 s): `ios3ds-audio: headphones in, jack interrupt` → `tone: route change, reason 1 (new device available): "Speaker" -> "Headphone"`; unplug → `reason 2 (old device unavailable): "Headphone" -> "Speaker"` (the iPod app's pause trigger; tone now pauses on it: `tone: paused after 3240 ms`). Volume HUD reads "headphones" (screenshots).
- **Found:** on the headphone route iOS shuts the speaker amp down (`0x04`) — the shim turned that into CSND volume 0, i.e. silent headphones — plays the PCM unscaled (tone peak 5999 = 6000) and sets the volume on CS42L58 reg 0x1a: 7-bit two's-complement dB, bit 7 = mute. Slider → value: 0.51 → 0x64 (-28 dB), 0.63 → 0x6c (-20), 0.76 → 0x74 (-12), 0.89 → 0x7b (-5), 1.00 → 0x02 (+2); idle/stopped 0x44 (-60); 0x80 around route changes. Matches the driver's -60 dB / 1 dB range table (0x8065fb38) and clamp (0x8065f3a4).
- **Fix:** shim CSND volume = `10^((dB - 2) / 20)` from reg 0x1a while iOS is on headphones (`hp_gain`, +2 dB = unity, so full volume = full scale as amp step 19 on the speaker), the amp's otherwise; the switch re-evaluates it. Host test covers both routes, mute, clamp, re-plug.
- **QEMU run 2** (same image): headphones 63/63 → `CS42L58 02 (2 dB) -> CSND volume 32768/32768`, 40/63 → `6c (-20 dB) -> 3277`; unplug, speaker slider to 20/63, re-plug → `6c (-20 dB)` again (separate volume memory), 48/63 → `74 (-12 dB) -> 8231`; speaker streams back at amp step 19 = unity. HUD screenshots: headphones 63/63, speaker 20/63, headphones 48/63.
- **QEMU run 3** (default image, final code, `--time 300 --icount 2`): plug/unplug/plug → `[audio] iOS route -> headphones|speaker (jack change #1..3 passed to iOS)`, HUD "headphones" at 56/63, speaker HUD at 16/63; no panic. `make test` OK; `emu/soak.py --cycles 10`: PASS (no panic/hang/jetsam; the one "SpringBoard restart" is the boot connection, as in earlier soaks).
- **Fail-safe (unverified on hardware):** GPIO2 bit 0 = jack, 1 = plugged (libn3ds; hardware boots log "speaker" with nothing plugged, so 0 = unplugged is consistent). Reads "plugged" at boot → iOS stays on speaker until it reads "unplugged" once (`[audio] jack: GPIO2 bit 0 reads 'plugged' at boot ...`); > 8 changes in 10 s → iOS on speaker until reboot (`[audio] jack: GPIO2 bit 0 changed N times ...`). `boot.cfg hp_route=0` turns the iOS side off (CODEC still follows the jack).
- **Next hardware log should show** (plug, play, unplug): `[audio] headphones plugged in: CODEC output -> headphones, iOS route -> headphones (CODEC headset detect XX)` and `unplugged ... (CODEC headset detect YY)` — settles the bit and its polarity (XX ≠ YY would mean the CODEC's own detection agrees; QEMU's CODEC stub reads 91 both ways); `[audio] iOS headphone volume: CS42L58 .. -> CSND volume ..` while playing; sound only from the headphones while plugged. If the polarity were inverted the boot line would already say "headphones" and the speaker would be silent (it is not, runs 14-16).

## 59. Wi-Fi firmware without GodMode9: the launcher reads NWM's .code through Horizon's FS (2026-11-19)
Design: docs/wifi.md §3.1 (launcher), §3.2 (GodMode9 fallback, unchanged).
- **What:** `tools/ios3ds-launcher/main.c` `fetch_nwm_code()` (launcher version 2). On the normal start screen, when neither `sd:/ios3ds/nwm.code` nor `wifi-fw.bin` exists, it opens NWM's `exefs:/.code` (title 0004013000002D02, `MEDIATYPE_NAND`) read-only, reads it in 64 KiB chunks with an on-screen KiB counter, checks 64 KiB ≤ size ≤ 4 MiB and bytes read = size, writes `nwm.code.tmp` and renames it to `nwm.code`, zeroes and frees the buffer. Any failure: one red line, nothing left on the card, the launch goes on with Wi-Fi off. Skipped once the file exists.
- **Lowpaths:** archive `0x2345678A` with `{tid_lo, tid_hi, mediatype, 0}` (libctru `romfsMountFromTitle`, FBI `listtitles.c`/`extractsmdh.c`, Checkpoint `smdh.cpp`); file `{0 high-level NCCH, 0 content index, 1 exefs:/.code, ".code" as 8 bytes}` (3dbrew "Filesystem services" title-access tables; FBI/Checkpoint use the same 20-byte form with type 2 + "icon"; Luma3DS `loader.c` `codeContentPath` = type 1 + ".code" in `0x2345678E`).
- **fs:USER vs FSPXI:** 3dbrew's "FS Limitations" for title access: fs:USER allows only RomFS, System Menu Data and the seed checks, not `exefs:/.code`; FSPXI allows all. The launcher still tries `FSUSER_OpenFileDirectly` first, then (only under Luma3DS, checked with `svcGetSystemInfo(0x10000)`) borrows a `PxiFS0` client session with Luma's `svcControlService(SERVICEOP_STEAL_CLIENT_SESSION)` (SVC 0xB0, inline asm; same call as Checkpoint `archive.cpp`) and uses `FSPXI_OpenArchive/OpenFile/GetFileSize/ReadFile`. Disassembly checked: r0 = 0, r1 = &handle, r2 = "PxiFS0" at the `svc 0xB0`; the two lowpaths in .rodata as above.
- **BLZ:** FS hands out the ExeFS file as stored (Luma's loader decompresses `.code` itself when `compress_exefs_code` is set), so the written file is normally BLZ-compressed; `nwmcode.c` already accepts that (`emu/wifi_test.py --case nwm` uses a BLZ file).
- **Verified:** `make launcher` builds without warnings (local devkitARM/libctru; CI job `firm` builds the same target). **Not verified:** any of it on a console (no hardware run yet); whether Process9 accepts the FSPXI `.code` open from a .3dsx session; the actual NWM `.code` size.
- **Next hardware run should show** on the launcher's top screen: `<k> / <k> KiB via FSPXI` (or `via FS`) then `Wrote /ios3ds/nwm.code (<n> bytes)`, and in the ios3ds log `[wifi] firmware from 0:/ios3ds/nwm.code: <n> bytes (BLZ-compressed), main type4`. A red `cannot open NWM .code (FS …, FSPXI …)` line gives both result codes; then use the GodMode9 script (§3.2).

## 60. Cydia pre-installed: Telesphoreo base system unpacked into the rootfs at build time (2026-11-19)
Design and user docs: docs/cydia.md. Code signing was already off, so "pre-jailbroken" only needed the packages.
- **Repository state (2026-11):** `http://apt.saurik.com/dists/tangelo-3.7` (= `dists/ios`, 364 packages) still serves the iphoneos-arm pool; `debs/*.deb` 302-redirects to `cache.saurik.com`. Old 2011 bootstrap versions are mostly gone; the newest ones still run on armv6: Cydia 1.1.30 is fat armv6+arm64 with `LC_VERSION_MIN_IPHONEOS 2.0` and links only libraries 4.2.1 has (libstdc++, WebCore/WebKit, SpringBoardServices, libapt-pkg 4.6). Every Mach-O in the 32-package set has an armv6 or CPU_SUBTYPE_ARM_ALL slice (audit script over the debs, none armv7-only). Set = closure of Cydia's Depends/Pre-Depends + all `Essential: yes`; download sizes and MD5s match the index; pinned with sha256 in `rootfs/cydia.json`.
- **Maintainer scripts** read one by one: Cydia's postinst is an arm Mach-O (strings: makes `/var/mobile/Library/{Cydia,Caches/com.saurik.Cydia}` as mobile, migrates `/var/db/stash`); `firmware.sh` writes the `firmware`/`cy+*`/`gsc.*` virtual packages from `sw_vers`, `sysctl`, `gssc`, `cfversion`; `firmware-sbin` ships *empty* `/sbin/fsck`, `mount`, ... and its pre/extrainst restore Apple's files (we keep the stock ones); `pam`/`pam-modules` preinst stash `/usr/lib/pam`; `uikittools` extrainst rebuilds the installation cache; `ncurses` preinst handles an old `/usr/lib/_ncurses`. Only the effects that matter are reproduced; `/.cydia_no_stash` (checked by the Cydia binary) skips stashing, which only helps a two-partition iPod.
- **hfsplus:** xpwn follows symlinks in path components (`add /var/x` lands in `/private/var/x`), new files/folders inherit the parent's owner (root under `/usr`, 501 under `/var/mobile` as dpkg would give), `chmod` keeps setuid (`cydo` 06755). `addall` can't be used (it follows symlinks and would write into a symlink record), so `scripts/cydia.py` walks each tar: 978 entries, ~1460 hfsplus calls, 4-8 s.
- **QEMU** (wifi4's QEMU, has slirp): boots to SpringBoard (page 2: Cydia icon); Cydia launches in ~10 s into its UI, no "Preparing Filesystem"; Installed lists the packages. First launch wrote Cydia's own defaults (`ios/550.52`, BigBoss, ModMyi, ZodTTD, repo666.ultrasn0w.com) to `Caches/com.saurik.Cydia/sources.list` and symlinked `cydia.list` to it, so the build writes no source lists (an earlier `saurik.list` duplicated it). The boot-time `firmware.sh` ran in the guest: `status` gained 24 `gsc.*` packages. With `ctr-wifi` + slirp: refresh fetched `apt.saurik.com_dists_ios_550.52_main_binary-iphoneos-arm_Packages` (174237 B) and BigBoss's (8.2 MB); Sources > Cydia/Telesphoreo shows its sections; installing `cpio` from Cydia gave `cpio 2.9-2p` "install ok installed", `/usr/bin/cpio`, `info/cpio.list`. HTTPS pages (home, depictions) fail: "certificate for this server is invalid" (4.2.1's root store). `emu/soak.py --cycles 10` after that: PASS (0 panics/stalls/crashes). Screenshots: `build/emu-cydia/` (not committed).
- **Not done:** Cydia Substrate (would add `DYLD_INSERT_LIBRARIES` everywhere next to our configd/SpringBoard environment; left for a separate test).

## 61. Current root certificates: user anchors in securityd's TrustStore.sqlite3 (2026-11-20)
Design and user docs: docs/certificates.md.
- **Where 4.2.1 keeps roots** (8C148 `/usr/libexec/securityd`, ARM, disassembled with capstone): 174 system anchors are compiled in (`__DATA,__const` 0x496e4 = {len, ptr, flags}; normalized-subject hash table 0x49f1c, lookup 0x7f80..0x8360); no anchor file exists in Security.framework. User anchors: `/Library/Keychains/TrustStore.sqlite3` (literal path, 0x17b8c; `/Library/Keychains` -> `../private/var/Keychains`, owner `_securityd`), opened at 0xa204, schema `tsettings(sha1,subj,tset,data)`. Contains (0x9a44) = row exists for the SHA-1 (`tset` unused); CopyParents (0x9aec) = rows whose `subj` equals `SecCertificateGetNormalizedIssuerContent(child)`. `SecPathBuilderInit` (0x8e40) appends system (0x8364/0x8430) and user (0x67c8/0x67a0, SecTrustStoreForDomain(2)) anchor sources to the parent and anchor sources unless `anchorsOnly`; evaluation runs in securityd for every client, so a row there is a root for Safari, Cydia and all apps.
- **Normalized subject:** RDN SET contents with PrintableString values upper-cased, trimmed, space runs collapsed, other string types kept, lengths re-encoded. `scripts/certs.py` reproduces all 174 compiled-in normalized subjects byte for byte (146 differ from the raw subject).
- **Algorithms:** Security.framework (dyld cache) has md5/sha1/sha256/sha384/sha512-with-RSA OIDs, imports CC_SHA224..512, but only `ecdsa-with-SHA1` and no P-256/P-384 OID: EC roots skipped (40 of Mozilla's 121). 18 of the compiled anchors are RSA-4096, and RSA-4096 roots verify (ISRG Root X1).
- **Build:** recipe `trust_store` -> `scripts/certs.py` writes the DB (SQLite format 4, rollback journal; iOS has SQLite 3.6.23.2) from `rootfs/certs/roots.pem` (81 RSA roots: Mozilla via Ubuntu ca-certificates 20260816, plus GTS Root R2 from an older certifi). `ios3ds-nettest` fetches https:// URLs (wifi.cfg, or body lines of the test server's answer) through CFNetwork, looked up with dlsym (the stub SDK has no CFNetwork); `emu/wifi_test.py --https`.
- **QEMU** (wifi4's QEMU, ctr-wifi + slirp, `--icount 2`), stock store vs new store: cydia.saurik.com -9807 -> 200; tls-v1-0.badssl.com (Let's Encrypt, ISRG Root X1) -9807 -> 200; Cydia's home page now loads; Safari shows tls-v1-0.badssl.com and https://www.google.com with the lock icon. Still failing: moreinfo.thebigboss.org (all-SHA-384 Sectigo chain, -9807 with both roots present; cause in the signature check not pinned down), letsencrypt.org and other TLS-1.2-only servers (-9836 errSSLPeerProtocolVersion). Controls untrusted-root/expired.badssl.com stay -9807. nettest's bare CFNetwork GET to www.google.com gets -9806 while Safari loads it (not a certificate issue). No opt-in bypass was needed.

## 62. Settings > Wallpaper empty: no DeviceClass from the lockdown stub, and 'L555' album surfaces; app NSLog on the console (2026-11-20)
- **App logs.** SpringBoard gives apps a stderr only for a debug launch. `-[SBApplication _addInternalDebugVariablesToEnvironment]` (SpringBoard 0x26fd0, called from the launch path at 0x2ba04) reads SpringBoard defaults: `SBMallocStackLogging`, `SBMallocGuardEdges`, `SBMallocCheckHeap*`, `SBZombieEnabled`, `SBDeallocateZombies` and `SBGuardMalloc[Always]`; the last adds `DYLD_INSERT_LIBRARIES=/usr/lib/libgmalloc.dylib` to every app's environment (the non-`Always` keys are one-shot: removed after use). The device has no libgmalloc, so the rootfs now links that name to `ios3ds-applog.dylib` (tools/ios3ds-diag/applog.c) and sets `SBGuardMallocAlways`; SpringBoard gets the library from its plist. Active only with boot-arg `ios3ds_diag=1` or `ios3ds_applog=1` (boot-perf.md §3): `CFLOG_FORCE_STDERR=1`, stdout/stderr → `/dev/console` (0622 in devfs, so `mobile` can open it), `[applog]` lines for alert titles/texts (UIModalView/UIAlertView setters + popup, objc runtime via `dlsym`) and the view tree on each `viewDidAppear:`.
- **Bug: Settings > Wallpaper shows an empty page** (QEMU, both `ca_gpu=1` and `ca_gpu=0`). With no photos, Wallpaper.bundle's `-[WallpaperController specifiers]` points the preview straight at `WallpaperAlbumController`; the view tree showed `WallpaperAlbumPane > PLAlbumView > PLHighlightingTableView` with **no cells**. `-[WallpaperAlbum init]` (0x3713a0a4) builds `/Library/Wallpaper/<model>`, model = `-[UIDevice model]`, which was **"Unknown"**: UIKit 0x338831a8 asks `lockdown_copy_value(conn, NULL, kLockdownDeviceClassKey)`, and our liblockdown stub (re-log 18) knew only ActivationState and BrickState. So the album looked in `/Library/Wallpaper/Unknown`.
  - **Fix 1:** `lockdown_copy_value` (cache 0x684b8e0) is now a table lookup: `{key variable, value}` pairs in the dead body of `lockdown_copy_buid_identity` (0x684b9b4, stubbed to return 0), compared by pointer to `*kLockdown…Key` (the callers use liblockdown's exported constants), plus new constant CFStrings (the recipe checks that 208-byte region by `orig_sha256`, new in mkrootfs, so no Apple code goes into the recipe; isa `__CFConstantStringClassReference` 0x3e925550, flags 0x7c8) for "iPod2,1", "4.2.1", "8C148", "N72AP". Values: ActivationState "Activated", BrickState false, DeviceClass/DeviceName "iPod" (UIKit's CFSTR), ProductType, ProductVersion, BuildVersion, HardwareModel. Keys referenced in the cache (non-lazy pointers to the key variables): DeviceClass (UIKit, MobileSync), DeviceName, ProductType (GameKit, ManagedConfiguration), ProductVersion/BuildVersion (UIKit, GameKit), HardwareModel (GameKitServices), UniqueDeviceID (left NULL). `UIDevice.model` is now "iPod touch"; the album finds 31 wallpapers (`/Library/Wallpaper/iPhone` and `iPod`; the name regex is `^(\d+)(_\w+)*[^thumbnail]<scale>\.png$`, scale "" since `PLScreenScale()` = 1).
  - **Then, `ca_gpu=1` only:** the grid stayed white; `[gl] IOSurface pixel format 'L555' not supported` + `GL error 0x501` from `BindCoreSurface`. `PLAlbumViewCell setCGImageRef:` puts one CGImage per row into `contentView.layer.contents`; the thumbnails are baked by `MLPhotoBakedThumbnails` in MusicLibrary's formats, which come from `lockdown_copy_value(Checkpoint domain, "ImageSpecifications")`, else `/System/Library/Lockdown/Checkpoint.xml` (format 3011 = 79×79, PixelFormat `'L555'`). The rows are IOSurface-backed, and CA binds the surface as a texture. CA's software compositor reads them (`ca_gpu=0` showed the grid).
  - **Fix 2:** tools/ios3ds-gl/iface.c converts `'L555'` (and `'L565'`) surfaces bound to a texture target into a 32 bpp copy (`[gl] IOSurface pixel format 'L555' texture: converted to 32 bpp at bind`, once).
  - **QEMU (`ca_gpu=1`):** the grid shows all thumbnails; tapping one opens "Wallpaper Preview" (`PLWallpaperImageViewController`), Set returns to the grid, and after lock (charger off, so the big battery does not cover it) the lock screen shows the chosen wallpaper.
- **Status bar "Searching…"** comes from `-[SBStatusBarDataManager _updateServiceItem]` whenever `+[SBTelephonyManager sharedTelephonyManagerCreatingIfNecessary:]` returns a manager; unrelated to the lockdown values and not changed here.
- **"Invisible alert after boot" (dimmed home, taps ignored until (160,333)): not reproduced.** 9 QEMU boots (`ca_gpu` 1/0, `diag` 0/1, plain and `--icount 2`, before and after the lockdown fix): no `[applog] … alert` line and the first icon tap opened its app each time; the dimming in screenshots is the emulated LCD backlight (`lcd-backlight` 500/1000 scales the mirror), not an alert. A real alert does render on the PICA200: `qom-set mcu-battery 15` → `[applog] SpringBoard alert shown, title "Low Battery"`, drawn with its Dismiss button at y≈283. SpringBoard's single-button "Dismiss" alerts (sb strings `DISMISS_ALERT`, `LOW_DISK_SPACE_DISMISS`): SBLowPowerAlertItem, SBDiskSpaceAlertItem (kqueue VQ_LOWDISK; the 1 GiB rootfs has 378 MiB free, HFS warns below 10 %), SBDismissOnlyAlertItem from SBLockdownManager (only with telephony), the thermal check, and the unusable-FireWire-charger check (`AdapterInfo` = 0xe00002c7). Next time it shows: boot with `ios3ds_applog=1` (or `diag=1`) and read the `[applog] SpringBoard alert …` lines.

## 63. 30-cycle soak at hardware timing: a half-copied home screen after a long lock (2026-11-20)

- **Run:** `emu/soak.py --cycles 30 --icount 2`. No panics, stalls, crashes or md0 errors. The soak counted "SpringBoard restart" once, but that entry is the first `[input] connected` line, not a restart. Flagged:
  - **cycles 9 / 19 / 29 (after a 130 s lock):**
    - cycles 9 and 29: a full home screen. Its difference to the reference (15.5 / 16.8, threshold 12) is the dimmed fade-in after unlock plus the clock. The backlight is back at 500/1000, as before the lock.
    - cycle 19 (35.0): a black screen with a sliver of the first icon row.
  - **cycle 13:** HOME on page 1 opened Spotlight (known soak behaviour).
- **Cause of cycle 19:**
  - At wake, `[gpu] screen copy: ARM9 CPU again (GPU frames stopped: state ACTIVE, 305 busy skips, 0 timeouts)`.
  - `gm_live` allows 1 s after the display comes on for mirror frames to resume.
  - After a long lock, CoreAnimation redraws everything with GL and holds the GPU, so the shim skips mirror frames as busy.
  - The ARM9 took that for a stall and switched to its CPU copy. Under `--icount 2` that copy is slow, so the screenshot caught a half-copied frame.
- **Fix:** `boot/arm9/gpumirror.c` counts a changing `busy_skips` as progress, for at most 3 s per busy stretch.
- **Re-run (10 cycles, long lock at cycle 9):**
  - The unlock screenshot is the full home screen (diff 12.8: fade-in and clock).
  - The CPU fallback still fired once, briefly, and switched straight back to `PICA200`.

## 64. Gyroscope: how 4.2.1's CoreMotion finds a gyro, and a gyro service from Apple's accelerometer kext (2026-11-20)
Design and user docs: docs/mcu.md section 11. Disassembly from the dyld cache (`build/toolchain/cache/dyld_shared_cache_armv6`, CoreMotion at 0x3320d000, ARM code) and the kernelcache (kexts have no symbols; vtables and IOHIDFamily strings).
- **Where the data comes from:** CoreMotion reads the sensors in the app's own process (`CLSensorInterfaceIoHid` over `CLIoHidManager`); locationd is not involved (it is disabled in our rootfs anyway). `CLIoHidManager::initializeHid` (0x3323c37c) makes an `IOHIDEventSystemClient` (SpringBoard's event system, as for ios3ds-mcud), fills a map event type -> HID usage (page 0xff00: accelerometer 13 -> usage 3, gyro 20 -> 9, compass 21 -> 10), matches with `IOHIDEventSystemClientSetMatchingMultiple`, and `refreshHidServiceRefs` (0x3323cba8) picks the service whose `PrimaryUsage` equals the entry's usage. Rates go to the service as its `ReportInterval` property (µs).
- **Sample format:** `onGyroIohidEvent` (0x33244b74): event type 20, `IOHIDEventGetFloatValue` fields 0x140000/1/2 = x/y/z, integer field 0x140004, time stamp in mach ticks. The values stay in CoreMotion's internal units until `CLMotionTypeRotationRateToCMRotationRate` (0x33231db8) multiplies by pi/180 for `CMGyroData.rotationRate`: the HID event carries **deg/s** (16.16 in the kernel event). The first QEMU run sent rad/s and CoreMotion reported 1/57.3 of the set rate, which is how this was noticed.
- **gyro dt:** `CLSensorInterfaceIoHid::getGyroDt` (0x33245220) looks up the service matching `IOPropertyMatch {name = gyro}` and reads `gyro-interrupt-calibration`: 8 bytes, u16 version (must be 1), u16 at +4 = sample period in µs; dt = interval / 0.005 × period (default period 0.00476 s, and `CLDeviceMotion::startSensorFusion` logs "Could not get gyro dt"). Our node sets 1, 0, 5000, 0: dt = the requested interval.
- **The gate:** `-[CMMotionManager isGyroAvailable]` returns an ivar; behind it `CLSensorInterfaceIoHid::isGyroAvailable` (0x332451f0) = `CLIoHidManager::isEventTypeAvailable(20)` (0x3323bf50) = bit 6 of `CLPlatformInfo::getProviders()` (0x33224b18; compass: bit 5; accelerometer: always). getProviders switches on `getSystemHardware()`, which matches the model string against M68/N45/N82/N72/N18/N88/N90/N81/K48. Masks: N72 0x2012 (no gyro, no compass), N88 (3GS) 0x323f, N90 (iPhone 4) 0x337f, N81 (iPod touch 4) 0x2052 = N72 | gyro. No MobileGestalt / plist involved. The N72 literal is at 0x33224dd4 (shared with N45 and N18): one word in the rootfs recipe, 0x2012 -> 0x2052.
- **No gyro driver for the n72:** the iPod2,1 kernelcache has no gyro kext. But `com.apple.iokit.IOHIDFamily`'s `AppleEmbeddedHIDEventService` has the embedded dispatchers in its vtable: +0x3c0 dispatchAccelerometerEvent (0x80546771 -> IOHIDEvent factory 0x805455f9, type 13: ts, x, y, z, type, subtype, options), +0x3c4 dispatchGyroEvent (0x80546719 -> 0x805455b5, type 20: ts, x, y, z, type, subtype, 4 more fields, options), +0x3c8 compass (type 21). Each builds the event and calls `dispatchEvent` (+0x3ac).
- **AppleEmbeddedAccelerometer** has three vtables: AppleLIS302DL 0x8061f7c4 (our accelerometer, re-log 40), the abstract base 0x8061fc30, AppleLIS331DLH 0x806200d0. Shared base methods: +0x3b0 getPrimaryUsagePage (0x8061b970, 0xff00), +0x3b4 getPrimaryUsage (0x8061b978, 3). The ReportInterval timer (0x8061c314) calls 0x8061c294, which reads a sample through +0x3f8 and calls `this->vtable[0x3c0](this, ts, x, y, z, 0, 0, 0)`.
- **AppleLIS331DLH** (unused on the n72): enable (0x8061cc50) sets CTRL_REG1 (0x20) bit 5 (PM normal) / clears bits 7:5 to disable, writes CTRL_REG2 (0x21) = 0x80 (BOOT) and polls it until bit 7 clears (panics after 1000 x 500 µs), CTRL_REG3, CTRL_REG4 = 0x80. Read (0x8061d594): 16-bit OUT_X/Y/Z at 0x28/0x2A/0x2C through a 3x3 calibration matrix (DT `factoryCalibration`, `accel-sensitivity-calibration`, `low-temp-accel-offset`, all optional). WHO_AM_I 0x32.
- **Bridge:** a DT node `/arm-io/i2c0/gyro` (`accelerometer,lis331dlh`, I2C 0x6b, GPIO 0x62) gives a second instance of that kext on its own class, so its vtable can be patched without touching the accelerometer: +0x3b4 -> 0x8034433c (an existing ARM `mov r0,#9; bx lr`), +0x3c0 -> shim +0x2C `shim_gyro_dispatch`, which calls +0x3c4 with the ARM9's sample. The emulated LIS331DLH register file answers WHO_AM_I and clears BOOT; CTRL_REG1's PM bits become `req_gyro` for the ARM9. Everything else (IOHIDEventService registration, user client, ReportInterval timer, power on/off as clients come and go) is Apple's code.
- **3DS side:** libn3ds' I2C table (source/arm11/drivers/i2c.c): bus 3 devices 9/10/11 at 0xD6 / 0xD0 / 0xD2, 10 = "Old 3DS only? InvenSense ITG-3270". Horizon's HID reports 14.375 LSB per deg/s for the Old 3DS gyro (Citra's hwtest value), which is ITG-3200 FS_SEL 3. The ARM9 drives bus 3 like bus 2 (GodMode9's table covers all three buses from the ARM9; unverified for bus 3 on our side).
- **QEMU** (`--icount 2`, ITG model at (10, 30, −45) °/s then changed by `qom-set`): CoreMotion's `gyroData.rotationRate` = (−0.523, 0.175, −0.786) rad/s, exactly the model's rates in iOS axes; then 1.571 / 0 / 3.142 as the model changed; `deviceMotion` rate and attitude follow; the chip sleeps before the first and after the last client. Log in docs/mcu.md 11.1.

## 65. Cameras: Celestial's virtual-camera slot, the CAM FIFO from the ARM11, Camera.app in QEMU (2026-11-20)
Design: docs/camera.md. All addresses: the 8C148 iPod2,1 shared cache.
- **Gating:** Camera.app (in the rootfs) has `SBAppTags = (still-camera)`; capabilities come from `SpringBoard.app/N72AP.plist`. Adding `still-camera` + `front-facing-camera` (recipe `plist_merge`, new nested merge in `mkrootfs.py`) puts the Camera icon on the home screen. Reference: the iPod touch 4G 4.2.1 rootfs (N81, public firmware; only plists read, nothing copied): N81AP.plist `still-camera`, `video-camera`, `front-facing-camera`, `hd-video-capture`; `Celestial.framework/N81/AVCapture.plist` mode schema.
- **Where capture runs:** mediaserverd (FigRecorderRemote server, `com.apple.coremedia.recorder`, user mobile); Camera.app only talks to it.
- **Device discovery:** `figCreateVideoCaptureDevice` 0x32e01520: `CFPreferencesGetAppBooleanValue("vcd", "com.apple.coremedia")` → `FigLoadAspenCaptureDevice("/System/Library/MediaCapture/vcd.mediacapture", "FigVirtualCameraDeviceCreate")` (0x32e0133c: `dlopen` + `dlsym`) before `IOServiceMatching("AppleH4CamIn"/"H3"/"H2"/"H1")` → `H4ISP`..`H1ISP.mediacapture`. Our plug-in goes into that slot; no patch anywhere.
- **Interface,** from Apple's H3ISP.mediacapture (still in the cache): vtable `{0, base, derived}` at 0x3e07e5b8 (device) / 0x3e07e5c4 (stream); base class `{1, storage 0xa4/0x4c, 0, invalidate, finalize, 0, copyProperty, setProperty}`; stream class `{1, start 0x30387804, stop 0x3038c774}`; `FigDerivedObjectCreate(alloc, vtable, classID, &obj)`. Device copy: `StreamArray`, `Clock` (`FigClockGetHostTimeClock`), `SupportedPropertiesDictionary`, else -12784. Stream: the plug-in creates `BufferQueue` and `StillImageBufferQueue` (`CMBufferQueueCreate` with the unsorted-sample-buffer callbacks) and returns them retained; supported formats = dicts `{FormatDescription, VideoMaxFrameRate, VideoScaleFactor, VideoIsBinned}`; frames = `CMSampleBufferCreateForImageBuffer` of IOSurface CVPixelBuffers, PTS host ns; start/stop post `StreamStarted`/`StreamStopped` with the stream as object.
- **Private constants:** `kFigCaptureStreamProperty_PortType`, `kFigCapturePortType_Camera/FrontFacingCamera`, `..._FrameDroppedFromQueue`, `..._ScaledPreviewPixelBuffer` are bound into Apple's plug-ins by the cache but not exported to `dlsym` (first run: `[cam] missing symbol ...`, the device was retried ~25 times). Their CFStrings are `PortType`, `PortTypeBack`, `PortTypeFront`, `FrameDroppedFromQueue`, `ScaledPreviewPixelBuffer`; Celestial compares with `CFEqual`, so equal strings work.
- **QEMU run 1** (plug-in in pattern mode, no kernel path): Camera.app opens with the front/back switch; the preview stays black, console full of `AppleM2CLCD, swap_queue error: e00002c8` / `failed: contiguousLength: 0x1d000`: CoreAnimation puts a 4:2:0 video layer into a CLCD overlay window (needs contiguous memory, and ios3ds only mirrors window 1). With BGRA frames the preview shows (CA composites them; screenshots in this run). Preview format `BGRA` in AVCapture.plist with 420f sensor frames did not help: the preview layer gets the sensor format.
- **Photos, debugged with a temporary `DYLD_INSERT_LIBRARIES` interposer in mediaserverd** (logs `CMNotificationCenterPostNotification` names and VT/ImageIO results; not committed): `WillCapturePhoto` → `DidCapturePhoto` → `VTPixelTransferSessionTransferImage -12905` from `figrecorder_createPreviewImageSurfaceFromSbuf` 0x32e2f7ec → JPEG fine (`FigAspenCreateJPEGFromSbuf` → no AppleJPEG service → `softwareJPEGEncode` → `CGImageDestinationFinalize` 1) → `Recorder_PhotoTaken`, but no `Recorder_PhotoPreviewImageReady`, and Camera.app saved nothing ("No Photos"). The review image is always 512x384 BGRA; VT on this device refuses to scale 320x240 up (same with 420f sources). `figrecorder_serviceStillImageQueue` (0x32e30a58) uses the sample's `ScaledPreviewPixelBuffer` attachment (sample attachments array, 0x32e31710) directly when present, so the plug-in attaches the frame's own pixel buffer.
- **Photo saved:** with the attachment the review image needs no scaling, `Recorder_PhotoTaken` reaches Camera.app, the thumbnail shows the photo and Photos lists "Camera Roll (1)" (pattern mode, QEMU). The still itself goes to the still queue at 640x480 (2x interpolated; 320x240 gave the same -12905 when Celestial scaled it).
- **Front/back switch** (gdb on QEMU's stub, `emu/session.py start --extra=-s`, hardware breakpoints in mediaserverd's Celestial): the device is created with the mode's stream plus its `FastSwitchCompanion` (`FigCreateCaptureDevicesAndStreamsForPreset` 0x32e01c70, outputs livesource +0x18c/+0x190); the switch stops stream 0, `livesource_swapVideoStreams` (0x32e252a8) swaps them, then `livesource_startSource` returned -12914 from `livesource_setupVideoScalerForPreview` (0x32e22c70) → `PreviewFailed`, stream 1 never started. The front mode had `PreviewVideoMirror`; a mirroring preview scaler cannot be made without the M2 scaler. Without the key the switch works (stream 1 starts, preview from the inner camera). Without `FastSwitchCompanion` the switch button does nothing. Also: `StreamStopped` is now only posted by a stream that ran (Celestial stops both).
- **Kernel path in QEMU** (`ctr_cam.c`): the ARM9 finds all three MT9V113 (`chip id 2280`), brings the outer left one up in ~5-10 ms, the shim streams. Plain QEMU timing: every frame overruns (0 frames, ~60 overruns / 10 s: the guest runs ~3x slower than the virtual clock). `--icount 2`: 0 overruns, live preview of the model's pattern in Camera.app, photo saved.
- **Final QEMU run** (rebased on main with the gyroscope, default image with Cydia, `--icount 2`): outer preview → photo → switch → inner preview → photo → Photos "Camera Roll (2)"; `[cam] inner camera: 144 frames, 0 FIFO overruns in the last 10 s`; `cameras off (frames 611, FIFO overruns 0, CAM restarts 0)`. `make test` OK; `emu/soak.py --cycles 5`: PASS (0 panics/hangs; the one "SpringBoard restart" is the boot connection).
- **Unverified on hardware:** everything 3DS-side (docs/camera.md section 7): CFG11_CAMERA_CNT, the I2C bus/address map, the DSi code list on the 3DS sensors, CAM_CNT = 0x8010 without DRQ, the FIFO window semantics, whether a 1 ms CPU drain keeps up on the console.

## 66. WPA2 + WPA3 mixed networks: detect them and join with WPA2-PSK (2026-11-20)

- **The report:** the user's "Adlerhorst 2" is a FRITZ!Box in "WPA2 + WPA3" mode. In hardware run 22 the AR6014 answered every WMI_CONNECT with NO_NETWORK_AVAIL, although our scans saw the AP at -76 dBm.
- **The mixed RSN IE:** AKMs PSK (2) and SAE (8), RSN capabilities MFPC, often a PMKID count and the BIP group management cipher. The 2010-era firmware's own profile match most likely rejects that IE.
- **`wpa2_rsn_parse`** (boot/arm9/wifi/wpa2.c, host-tested) classifies an AP's IE as WPA2, MIXED, WPA3_ONLY or OTHER:
  - WPA3_ONLY: SAE without PSK, or MFPR set;
  - OTHER: no PSK at all (802.1X), or a malformed IE.
- **The ARM9 driver:**
  - **MIXED:** it always connects to the scanned BSSID and channel with CONNECT_ASSOC_POLICY_USER | CONNECT_PROFILE_MATCH_DONE. The firmware builds a WPA2-PSK/CCMP association IE, and message 2 carries that IE.
  - **WPA3_ONLY:** one log line saying so and what to set on the router, then idle. No retry loop.
  - **The target's RSN IE** is logged in hex.
  - **Scan-complete path:** it now re-reads the scan results before connecting. Before, a join that had to scan first connected "not seen in scan" with the BSSID unknown (run 22).
- **QEMU:**
  - `ctr-wifi.security=wpa2-wpa3` and `=wpa3` announce those IEs.
  - In mixed mode the model refuses a connect without PROFILE_MATCH_DONE, which is the suspected firmware behaviour.
  - New `emu/wifi_test.py` cases:
    - `mixed`: detected, 4-way OK, link up;
    - `wpa3`: refused with the log line;
    - `ok` still 20/20.
- **Also:** `emu/build-qemu.sh` treated `#undef CONFIG_SLIRP` as "slirp present", so a QEMU built without the user-mode network backend was never reconfigured. It now looks for `#define`.

## 67. New 3DS: 804 MHz before XNU, timers at /192 with a halved count, L2C-310 write-through, the C-stick chip (2026-11-20)
Design and tester checklist: docs/n3ds.md.
- **Sources for the clock switch:** libn3ds `source/arm11/drivers/pdn.c` (`PDN_core123Init`, `PDN_setSocmode`) and `include/arm11/drivers/{pdn,cfg11}.h`: `PDN_LGR_SOCMODE` 0x10141300 (0 CTR 268, 1 LGR2 268, 2 LGR1 268, 3 LGR1 536, 5 LGR2 804; bit 15 ack), `PDN_LGR_CNT` 0x10141304 (bit 0 extension RAM, bit 8 L2C), `CFG11_SOCINFO` 0x10140FFC (bit 0 CTR, 1 LGR1, 2 LGR2), `CFG11_GPU_N3DS_CNT` 0x10140400 (bit 0 N3DS mode, 1 texture fix); GIC 88 for core 0, prio 0, a ~400-cycle wait after `LGR_CNT`, WFI until the ack, write back, clear pending 88. linux-3ds `firm_linux_loader/arm11/source/smp.c` does the same (`set_socmode`/`upclock`). libn3ds keeps CTR mode unless built for extra cores/QTM RAM and switches a non-CTR mode back to 0; our `n3ds_clock=0` does that too.
- **Who sets the clock on the way to us:** boot9strap leaves 268 MHz (issue #1's New 2DS XL log behaved like an Old 3DS). Neither Luma's chainloader nor the launcher's reboot path touch `PDN_LGR_SOCMODE`, hence the stale-ack write-back first.
- **MPCore timers:** PERIPHCLK = CPU/2, so 402 MHz at 804. The 8-bit prescaler can't reach /384 (= 1,047,311 Hz). Options weighed: a new `timebase-frequency` (touches the device tree and every 1,047,311 Hz constant in audio/camera/GPU/ARM9 code), 536 MHz (/256 exact, but mode 3 is an LGR1 mode, unknown on LGR2), or /192 with every count shifted by one in the shim. Took the shift: `get_abstime` (stackless, `movs r1,r1,lsr #1; mov r0,r0,rrx`), `get/set_decrementer`, and `shim_now32()` for the C users (audio.c, cam.c, pica.c), which keeps 32-bit wraps right. The stub measures PERIPHCLK against the ARM9's 65,457 Hz timer (watchdog at prescaler 0 over 1309 ARM9 ticks) instead of assuming it.
- **Kernelcache facts used for the L2 decision:** the boot code at file offset 0x66000 maps RAM with 16 MiB supersections, descriptor bits 2 | 0xC | AP 1 (offsets 0x660a0..0x660e4): TEX=000 C=1 B=1, outer and inner write-back, no write-allocate, not shared. Every TTBR write (0x6602c, 0x66070, 0x66bc0, 0x66bcc) ORs 0x18: RGN = 11, so table walks are outer-cacheable and go through the L2. XNU's own cache ops are ARM1176 L1 c7 operations only.
- **L2C-310 shared override:** per the Linux `arm,shared-override` binding text, without aux bit 22 the controller turns normal non-cacheable shared accesses into "cacheable no-allocate" reads and write-through writes, which can hit stale lines. We set the bit.
- **Why write-through:** with debug control DWB the L2 never holds dirty lines, so only the inbound direction (ARM9/GPU wrote RAM, the ARM11 reads it) needs new work. That direction already had an L1 invalidate in every shim helper, and those now call `shim_l2_inv` (invalidate by PA + sync). A spurious invalidate of a clean line is harmless, so non-linear VAs fall back to a best guess. The full audit is the table in docs/n3ds.md section 2. `st_panic_req` is read without any invalidate (it relies on L1 eviction), which is one reason the L2 stays off by default.
- **C-stick chip:** 3dbrew "I2C Registers" device 17 (no register index; byte 0 = 0x80, byte 1 buttons ZL 0x4, ZR 0x2), GBATEK "3DS I2C New3DS C-Stick and ZL/ZR-Buttons" (bytes 2/3 coarse X/Y, 4 = 0xFF, 5/6 fine X/Y signed; >0x1A bytes can hang the chip; needs CNTEX clock stretching), libn3ds `I2C_DEV_EXTHID` = {bus 3, 0x2A << 1}. 3dbrew "IRRST Shared Memory": Horizon's range ±0x9C after its own conversion.
- **QEMU:** `-M nintendo3ds,new3ds=on`. QEMU patch 0001 only re-read PERIPHCLK when the prescaler field changed, so the first New-model run measured 134 MHz after the switch. It now re-reads it on every control write. Results: section 67.1.
- **Two bugs the New-model QEMU runs found:**
  1. `rtclock_init`'s tick base was 0. The first clock read comes one raw tick after `shim_init` starts the watchdog, and 1 >> 1 = 0, so the calendar conversion spun forever with IRQs masked (PC 0x8006918e, the hang described in entry.S). The shifted clock now adds 1.
  2. With the three new lines, the generated boot.cfg grew to 4249 bytes. The ARM9 read only 4095 (`cfgbuf[4096]`), so the `cmdline=` line at the end was lost: no `rd=md0`, and the kernel waited for a root device until the watchdog panic. The buffer is now 16 KiB, and the ARM9 logs an error if a boot.cfg ever fills it.

### 67.1 QEMU evidence (`--icount 2`)
- **New model, defaults** (`emu/run.py --new3ds --icount 2`): `[a11] New 3DS: SOCINFO 7, SOCMODE 5, LGR_CNT 101, PERIPHCLK 402236315 Hz, timer /192 >>1` (the model's 402,167,784 Hz measured to 0.02 %). `[n3ds] … prescaler /192, tick shift 1 -> 1047490 Hz`. The heartbeat compares the ARM11 clock with the ARM9 timer: `[hb] clock: arm11 watchdog +589026 ms vs arm9 timer +589021 ms (ratio 1000/1000)`. `mcud`: `gettimeofday +70546 ms, mach_absolute_time +70546 ms`. The kernel reaches SpringBoard, and the GPU mirror runs with `0 of 512 sampled LCD pixels differ`.
- **Old model:** `[a11] Old 3DS: SOCINFO 1, SOCMODE 0, LGR_CNT 0, PERIPHCLK 133893818 Hz, timer /128 >>0`. Nothing else changes; 0x17E10000 stays unmapped.
- **C-stick and buttons** (keypad 6, Z, N; `emu/tap.py` in Settings):
  - keypad 6 on the home screen → `[input] cpad down 272,240` / `up 47,240`: page 2.
  - keypad 2 twice in Settings scrolls about a page each time without a fling.
  - Z (ZL) → the list animates back to Airplane Mode at the top.
  - N (ZR) → the white screenshot flash (`[exthid] 80 02 … ZR`, `[input] ZR: screenshot`).
  - First mapping tried for ZL: a double HOME press for the app switcher. In QEMU it only scrolled page 2 back to page 1: no multitasking on the iPod touch 2G.
- **`n3ds_l2=1`** (boot.cfg edited in the image): `[n3ds] L2 cache: on, write-through (L2C-310 id 410000c9 aux 02490000 ctrl 1)`. After 190 s: `l2c-ctrl 1`, `l2c-debug 2` (DWB), `l2c-inv-ways 1`, `l2c-inv-lines 6810479`, `l2c-syncs 868416` (the shim's invalidates reach the controller), clock ratio 999/1000, SpringBoard up. That the maintenance is *enough* can only be shown on hardware.
- **Soak:** `python3 emu/soak.py --cycles 5` PASS on the Old model and with `--new3ds` (0 panics, 0 hangs, 0 watchdogs). `make test` OK (test_n3ds: 35 checks).

## 68. Cameras, hardware run 23: sensors answer, no data reaches the CAM FIFO (2026-11-20)
Log: build/hwlogs/run23.txt (main + agent/camera, `camera=1 diag=1`), lines 689-902 and 2387-2399. Design: docs/camera.md section 7.
- **Seen:** first device creation `cameras present 0x0` (the ARM9 probes the sensors only at the first start; the plug-in does not reject on it, the second creation says 0x3); all three sensors answer `chip id 2280` on GBATEK's map (outer left 0x10144000/0x78, inner 0x10161000/0x7A, outer right 0x10161000/0x78); `outer left camera streaming 320x240 YUV422 into CAM1 (init 136 ms)` (QEMU: 5 ms, so the waits really waited on the sensor); then `0 frames, 0 FIFO overruns` every 10 s until stop. A third start later: `init failed (step 5)` = the sequencer refresh did not finish within 100 ms.
- **Reading:** with no overruns the CAM1 FIFO never filled: either the block never saw a frame (no sync/data from the sensor, or the block not enabled/clocked), or the data went to the other block, or the shim's drain never enabled CAM_CNT (no counter told). 3dbrew has no CAM register page ("Camera Registers" redirects to Y2R); GBATEK's 3DS CAM section is the only register reference; Citra/Azahar emulate the cam service at HLE level (no register sequence); no open bare-metal 3DS camera driver was found.
- **Ranking:** (1) the CAM block needs more than `CAM_CNT 0x8010`: +6 (bits 0-5, GBATEK suspects the DSi's CAM_MCNT, 0x22 = on) and/or the DRQ bit the official driver (CDMA) uses; (2) the outer left sensor feeds CAM0, not CAM1; (3) the DSi code list leaves the 3DS sensor in standby or with parallel output off; (4) `CFG11_CAMERA_CNT` bit 1 (GBATEK: "reportedly turn on/off cameras") is needed; (5) the shim never started the drain (mailbox/IRQ path; ran in QEMU).
- **Changes:** shim watches both blocks until data appears and moves to the one that gets it; set-up variants 0 (0x8010), 1 (+6 = 0x22), 2 (+ bit 14 DRQ, bit 9 IRQ) cycled every 2 s while nothing arrives; restart at a frame start once data is seen; diagnostics in a new mailbox line (services, services with data, CAM_STAT of both blocks, max fill, CAM_CNT readback, variant, port). ARM9: `CFG11_CAMERA_CNT` |= 3 with readback; `[cam-diag]` dumps of both CAM blocks at boot, before the set-up and 0.2/1/5 s after a start, a 50 ms sampler counting CAM_CNT bit 0/1 changes and FIFO fill changes per block, sensor registers (PLL, CLOCKS, STANDBY, RESET_MISC, 0x301A, SEQ_CMD, SEQ_STATE, SEQ_CAP_MODE, output size/format, read mode); sensor waits 500 ms; sensors probed at boot (present known before the first start, then standby and clock off). Host test: no data for 5 s → variant 2 still probing, then frames whole and in order.
- **Next hardware log must show** (open Camera.app, wait 15 s): the `[cam-diag]` toggle lines (does CAM_CNT bit 0/1 change on CAM0 or CAM1?), the sensor dump (`STANDBY` bit 14 clear, `RESET_MISC` bit 9 set, `SEQ_STATE`), and the 10 s stats line with `services` > 0 and the variant/port. Frames > 0 with some variant/port = that is the fix to make default.

## 69. Launcher: NWM .code path rejected on hardware (issue #5) (2026-11-20)

- **Report:** first hardware report of the launcher's firmware copy, on a New 2DS XL with Luma3DS: `cannot open NWM .code (FS E0E046BE, FSPXI E0E046BE)`.
- **Decoding E0E046BE:** level Usage, summary InvalidArgument, module FS (17), description 702 = FS InvalidPath. FS and FSPXI both rejected our path layout: archive 0x2345678A with a 20-byte file path `{0, 0, 1, ".code"}`.
- **What Luma3DS's own loader uses** (`sysmodules/loader/source/loader.c`): archive 0x2345678E (SaveDataAndContent2), FS_ProgramInfo `{u64 programId, u8 mediaType, pad}` as the archive path, and the 12-byte file path `{u32 1, ".code"}`.
- **The launcher now tries, in order**, each through FS and then FSPXI:
  1. Luma's layout `678E/1` (only under Luma3DS: Azahar 2126.2 crashes on that archive ID instead of returning an error);
  2. `678A/1`;
  3. `678A/2` (FBI's type for ExeFS files such as `icon`).
- **Reporting:** every attempt's result goes on screen and into `sd:/ios3ds/wififw-status.txt`, which the ARM9 copies into `log.txt` when Wi-Fi has no firmware.
- **Azahar check:** `678A/1 FS C8804478; 678A/2 FS C8804478` (no NWM title there).

## 70. Cameras, hardware run 25: the sensor streams, neither CAM block sees anything (2026-11-20)
Log: build/hwlogs/run25.txt lines 67-962 (agent/camera-hw diagnostics, re-log 68).
- **Seen:** `CFG11_CAMERA_CNT 00 -> 01` (bit 1 does not stick: GBATEK's "unused" is right). At boot CAM0 `cnt 0010`, CAM1 `0000`: someone before us (boot ROM, Luma) wrote CAM0's bit 4. After our start CAM0 reads `8010`, CAM1 `8000`: **bit 4 does not stick on CAM1** (GBATEK lists it R/W, "must be 1, else data transfer hangs, maybe reset"). Variant 2 reads back `c210`/`c200`, +6 `0022` on both. `CAM_STAT` always 0000; the 50 ms samplers (15k reads per block) never saw CNT bit 0/1 or the fill change, in all three variants, for 2 min; shim services up to 62103, 0 with data, so the drain ran (hypothesis 5 of re-log 68 is out).
- **Sensor (outer left), identical before and 5 s after the start:** PLL `b04a` (locked, bypass off), CLOCKS `42ff`, STANDBY `0028` (bit 14 clear = awake), RESET_MISC `0210` (bit 9 parallel output on; the Linux MT9V113 driver writes the same 0x0210), CORE_301A `121c` (bit 2 streaming), SEQ_STATE 3 (preview), output 320x240 YUV. The sensor says it streams; no block sees sync or data. The board (GBATEK pinout P9) wires the DSi pair (I2C bus 0x10161000) and the extra camera (I2C bus 0x10144000) to separate 8-bit buses with their own HSYNC/VSYNC/PCLK, as our map assumes.
- **Remaining hypotheses:** (1) the CAM block needs set-up bits we never set: the DSi's CAM_CNT init sets bits 8-9 = 2 and bit 10 = 1 before enabling the transfer (GBATEK DSi "Camera Init"; the 3DS register is "alike DSi", and GBATEK's "no effect?" for bits 8/10/11 was observed with the cam module's set-up already in place); (2) CAM1 is held in reset/unclocked (bit 4 not sticking) while CAM0 is not: the inner camera on CAM0 may work where the outer left one does not (run 25 never tried it); (3) the sensor's outputs are gated by its OE pin (RESET_MISC bit 4 set = "OE_N" control; not documented for the MT9V113); (4) pad drive (DSi PAD_SLEW 0x0201 vs Linux 0x0777).
- **Changes:** set-up variants now 0x8010, 0x8610 (DSi bits 8-10), 0x8E10 (+ bit 11), 0x8610 with +6 = 0x22, 0xC610 + DRQ with +6 = 0x22, the configuration bits written before the transfer bit. **Boot self-test with diag=1** (ARM9, before XNU): every sensor (outer left, inner, outer right) is brought up alone and all five variants are tried on both blocks for 200 ms each, then RESET_MISC = 0x0200 (bit 4 clear) with 0x8010/0x8610 and PAD_SLEW 0x0777; a `[cam-test]` line per set-up with both blocks' CNT readback, stat and maximum fill, `<-- DATA` when anything arrived, and the sensor registers. QEMU: data on CAM1 for the outer left camera and on CAM0 for the inner/outer right ones in every variant.
- **Next hardware log must show** (diag=1, before XNU starts): the `[cam-test]` matrix and `[cam-test] done: N set-ups saw data`. Any `<-- DATA` line names the working sensor/block/set-up, which then becomes the default. If none: compare which CNT bits stick on each block (CAM1's bit 4) and try the front camera in Camera.app too.
## 71. Cydia Substrate and TLSFix: TLS 1.2/1.3 for iOS's own networking (2026-11-20)
Design: docs/cydia.md (Substrate), docs/certificates.md section 4 (TLSFix). Packages: `mobilesubstrate` 0.9.6301 + `com.saurik.substrate.safemode` 0.9.6001 (apt.saurik.com, newest; armv6 slices, min iOS 2.0), `com.skyglow.tlsfix` 1.3 (github.com/ObscureMosquito/TLSFix release, sha256 ca2f6bb4...cb94; OpenSSL 1.1.1w statically, armv6/armv7/arm64), pinned in `rootfs/cydia.json`.
- **Substrate's own boot hook:** `extrainst_` (Mach-O; strings) writes `bsexec .. /usr/bin/cynject 1 .../SubstrateLauncher.dylib` to `/etc/launchd.conf` (or `/etc/rc.d/substrate`). 4.2.1's `/bin/launchctl` reads `/etc/launchd.conf` during bootstrap and waits for each `bsexec` (a test script logged before/after). `cynject` exits 0 (`task_for_pid(1)` works with `amfi_unrestrict_task_for_pid=1`), but afterwards every process launchd forks (38 pids in a `kill -0` scan) stays blocked forever and nothing execs; QEMU `-d int` shows no faults, the children's last syscalls are `kill`/`waitpid` (SubstrateLauncher's `posix_spawn` hook, 0x11ae: `kill(pid, 9)` + `waitpid` on a helper child; it also forks/execs `/usr/libexec/MSUnrestrictProcess`). `setenv DYLD_INSERT_LIBRARIES /Library/MobileSubstrate/MobileSubstrate.dylib` in launchd.conf (the pre-2011 way) hangs the boot the same way. A bash child with `DYLD_INSERT_LIBRARIES` = libsubstrate / SubstrateLoader / MobileSubstrate.dylib runs fine (`DYLD_PRINT_LIBRARIES`); `SubstrateInjection` loads `SubstrateLoader` only if CoreFoundation is loaded (imports: `getenv`, `dlopen`, `_exit`; strings `MSExitZero`, AppIndexer, Security, CoreFoundation).
- **What ships:** Substrate only where needed: appended to the recipe's own `DYLD_INSERT_LIBRARIES` jobs (SpringBoard, configd, securityd; boots, Wi-Fi and the a80211/hwaes interposers unaffected), apps via ios3ds-applog (`SBGuardMallocAlways` → `/usr/lib/libgmalloc.dylib`, which replaces any other insert for apps) `dlopen`ing MobileSubstrate.dylib, ios3ds-nettest after it loads CFNetwork. `cydia.substrate_setup()`; `_NSGetExecutablePath` added to scripts/ios-sdk-symbols.json.
- **Sandbox:** MobileSafari runs under launchd's `Builtin profile: MobileSafari`. A debug build of applog that writes to `/private/var/tmp` from inside Safari: `open()` → `EPERM` for everything under `/Library` (`/Library/MobileSubstrate/...`, `/Library/Frameworks/CydiaSubstrate.framework/...`, `/Library/Ringtones`) and for `/var/mobile/Library/ConfigurationProfiles`, `/usr/lib/*` allowed (symlinks into /Library are not). So `dlopen(MobileSubstrate.dylib)` fails and Safari keeps the stock TLS 1.0 ("could not establish a secure connection" for github.com and www.apple.com). Cydia is not sandboxed: Substrate and TLSFix load (`loader` and `tlsfix` handles non-NULL) and the home page loads. SpringBoard has `SBEnableDebugSandboxProfiles` / `/usr/share/sandbox/%@.sb` and libsandbox has the SBPL compiler (`sandbox_compile_file`), but that would replace Apple's built-in profile with our own; not done. Fix later: a Sandbox kext patch (com.apple.security.sandbox at 0x803cd000) as iOS 4 jailbreaks had.
- **TLSFix 1.1 (nfzerox) first:** hooks applied (`SSLHandshake` starts `04f01fe5` = `ldr pc,[pc,#-4]` after loading), `ensure_ready` = 1, raw OpenSSL `SSL_connect` to github.com from the guest: TLSv1.3. Failures were (a) EC chains (letsencrypt, wikipedia, github's ECDSA certificate) that 4.2.1's `SecTrustEvaluate` cannot verify, (b) a stale per-context state: CFNetwork disposes a failed context without `SSLClose`, the next context at the same malloc address inherited TLSFix's "failed" state and went to the stock TLS 1.0 stack (found by swapping TLSFix's `o_Handshake` pointer for a logging wrapper). TLSFix 1.3 fixes both: `SSLNewContext` hook resets the state, and EC chains are checked with OpenSSL against the CA payloads of `/var/mobile/Library/ConfigurationProfiles/*` (not .stub) — so the build writes `ios3ds-roots.plist` there with the RSA roots + the 40 EC roots of Mozilla's bundle (`rootfs/certs/roots-ec.pem`).
- **BigBoss:** moreinfo.thebigboss.org / apt.thebigboss.org serve a certificate that **expired 2026-06-15** (and is for moreinfo only). github.com's RSA certificate is on the same SHA-384 Sectigo R36/R46 hierarchy and verifies through `SecTrustEvaluate` (TLSFix 1.1 with RSA-only signature algorithms), so the "SHA-384" explanation in certificates.md section 3 was wrong; corrected.
- **Results** (TLSFix 1.3, nettest): github.com, en.wikipedia.org (EC), www.apple.com, www.google.com, cydia.saurik.com, ecc256.badssl.com 200; expired / untrusted-root / wrong.host.badssl.com -9807; letsencrypt.org TLS ok but CFNetwork HTTP error -4. `--icount 2`, TLSFix vs `TLSFIX=0`: cydia.saurik.com 2 s / 2 s, google 3 s / 2 s, apple.com 4 s / -9836, wikipedia 9 s / -9836. Boot to SpringBoard 80 s (plain QEMU, within the usual 50-100 s); `emu/soak.py --cycles 5` PASS (0 panics, hangs, crashes). Cydia: home page loads with Substrate + TLSFix loaded in it. Safe mode not exercised.
- **Unclear, not chased:** with the `ios3ds_applog=1` boot-arg no `[applog]` line appeared on the console in any run (also with the unmodified library), and in two runs with Substrate in SpringBoard Safari did not come up after a tap; without the boot-arg apps launch normally (soak).

## 72. Reaching sandboxed apps: the Seatbelt sb_evaluate patch, and Safari's memory wall (2026-11-20)
Design: docs/cydia.md "Reaching sandboxed apps". Goal: get TLSFix into MobileSafari (sandboxed), which re-log 71 left on TLS 1.0.
- **Why Safari was excluded:** a debug ios3ds-applog in MobileSafari (writing to /private/var/tmp, which the profile allows) `open()`s the candidate dylibs: every path under `/Library` (`/Library/MobileSubstrate/*`, `/Library/Frameworks/CydiaSubstrate.framework/*`) and `/var/mobile/Library/ConfigurationProfiles` returns `EPERM`; `/usr/lib/*` is allowed. launchd logs `Builtin profile: MobileSafari (sandbox)`. So Substrate's `dlopen` fails and Safari keeps the stock TLS stack.
- **The Sandbox kext** is `com.apple.security.sandbox` (prelinked at 0x803cd000, size 0xff000; Sandbox-58.12 from the `SourceCache` strings). Its MAC policy is registered via a `mac_policy_conf` at 0x803d46fc (`mpc_name` "Sandbox" 0x803d3740, `mpc_ops` 0x803d4728). Walking the 135 non-null function pointers in `mpc_ops` and histogramming their BL targets, the function 75 of them call is **sb_evaluate at 0x803ced98** (builds a 0xe0-byte eval context; `r2` = operation index). The bytecode interpreter it calls (references "bad opcode") is a separate function at 0x803d1b9c.
- **Operation indices** come from the `operation_names` char* array at 0x803d4320: file* 1, file-chroot 2 ... file-read* 6, file-read-data 7, file-read-metadata 8, file-read-xattr 9, file-revoke 10, file-search 11, ... MobileSafari 84, etc.
- **Patch** (kernel/patches-sandbox.json, opt-in SANDBOX=1): sb_evaluate's first instruction (`push {r4,r5,r6,r7,lr}` = e92d40f0) -> `b shim_sb_evaluate` (kernel/shim/entry.S; the branch word is computed from the shim ELF symbol by the patcher's new `branch` key, as the New-3DS work took the +0x0C jump-table slot this first used). shim_sb_evaluate: `cmp r2` against {1,6,7,8,9,11} -> `mov r0,#0; bx lr` (allow); otherwise replay the push and `ldr pc,=0x803ced9c` into sb_evaluate's body. Write/exec/network/iokit/mach checks stay enforced; only file reads of the dylib paths (and, incidentally, any file read by a sandboxed app) are allowed.
- **Result:** with the patch, MobileSafari's sandbox probe shows `open()` of the /Library paths now returns 0, and Safari gets past the TLS stage — github.com and example.com change from "could not establish a secure connection" to **"Cannot allocate memory"**. QEMU models the real 128 MB (`MEM_SIZE` 0x08000000); TLSFix's static OpenSSL 1.1.1w (1.9 MB armv6 text + 144 KB `__DATA` + the handshake heap) does not fit in the WebKit content process, so every HTTPS load OOMs, even a 1 KB page. The patch therefore turns a TLS failure into an OOM failure for Safari.
- **Decision:** off by default (`SANDBOX=1` opt-in). It is stable at the kernel level — boot, Wi-Fi, Cydia HTTPS and `emu/wifi_test` all PASS with it applied, and Mail / App Store launch (MobileMe account screen) without regression — but it makes Safari worse on 128 MB, so "default on only if stable" keeps it out. Cydia and other unsandboxed apps get TLSFix regardless.
- **Memory thrift:** Cydia Substrate is no longer appended to configd/securityd (nothing targets them; `substrate.daemons` = SpringBoard only), saving the SubstrateInjection/Loader + CoreFoundation scan in two daemons. At the home screen, before any app that uses CFNetwork is launched, only SpringBoard has loaded MobileSubstrate.dylib (TLSFix's deny-list excludes SpringBoard, so tlsfix.dylib itself loads nowhere until an app opens an HTTPS connection); so free memory at boot is essentially the same with TLSFix and `TLSFIX=0`. iOS 4.2.1 does not expose `vm.page_free_count` via sysctl, so the per-process cost is the dylib accounting above, paid only when an unsandboxed app (or, with SANDBOX=1, Safari) makes an HTTPS connection.

## 73. Installing Substrate tweaks without the launchd hang: hold, pin, cynject diversion, boot guard (2026-11-20)
Design: docs/cydia.md "Installing tweaks". Hardware report (main without pre-installed Substrate): installing anything that depends on `mobilesubstrate` hard-locks the install. That matches re-log 71: Cydia installs mobilesubstrate and runs its `extrainst_`, which calls `cynject 1 ...SubstrateLauncher.dylib` at once and writes the same into `/etc/launchd.conf`.
- **extrainst_ (0.9.6301, armv6 slice):** imports `system`, `fork`, `waitpid`, `vm_protect`, Foundation (`NSPropertyListSerialization`, `NSFileManager`), no `task_for_pid`: the injection goes only through `system("/usr/bin/cynject 1 ...")`. Strings: `/etc/launchd.conf`, `bsexec .. /usr/bin/cynject 1 ...`, `/etc/rc.d/substrate`, `/boot/untether`, `/System/Library/LaunchDaemons` + `%@/%s.plist`, `DYLD_INSERT_LIBRARIES`, `/usr/libexec/cydia/move.sh /Library/MobileSubstrate/DynamicLibraries` (a no-op with `/.cydia_no_stash`), `finish:%s` (asks Cydia for a respring).
- **Build changes:** `mobilesubstrate` + `com.saurik.substrate.safemode` get `Status: hold ok installed` and a version pin (priority 1001) in `/etc/apt/preferences`. `Commands/cynject` becomes a local dpkg diversion to `cynject.real`, with `/usr/libexec/ios3ds-cynject` in its place (refuses pid 1, execs the real one otherwise). `com.ios3ds.substrate-guard` (bash, once per boot) drops cynject lines from `/etc/launchd.conf`, removes `/etc/rc.d/substrate`, and re-installs the guard if a cynject binary ever shows up un-diverted.
- **QEMU, real APT** (ctr-wifi + slirp; a one-shot root job, not committed, ran apt-get against `http://apt.saurik.com/ ios/550.52 main` and `http://apt.thebigboss.org/repofiles/cydia/ stable main`):
  - `apt-cache policy mobilesubstrate`: installed 0.9.6301, candidate 0.9.6301, `Package pin: 0.9.6301` (nine older versions listed in the repo).
  - `apt-get -s upgrade`: 0 upgraded.
  - `apt-get install com.saurik.iphone.nlnf com.abart97.nosbscreen`: Inst/Conf only for those two. nlnf is "Net.-Local Number Format" from saurik, `Depends: mobilesubstrate (>= 0.9.3997)`, UIKit filter. NoSBScreen is from BigBoss, armv6, SpringBoard filter. rc 0; `dpkg -l` shows `hi mobilesubstrate 0.9.6301` and `hi com.saurik.substrate.safemode 0.9.6001`, still held.
  - Ran mobilesubstrate's own `extrainst_ install`, as Cydia does after a (re)install: rc 0. It wrote `bsexec .. /usr/bin/cynject 1 ...` into `/etc/launchd.conf`, and new processes still spawned (`/bin/date` ran): the guard refused pid 1.
  - It also rewrote `com.apple.SpringBoard.plist` (now binary) and **removed `MobileSubstrate.dylib` from SpringBoard's `DYLD_INSERT_LIBRARIES`**, leaving `/usr/lib/ios3ds-applog.dylib`. configd and securityd were unchanged.
  - Respring (`launchctl stop com.apple.SpringBoard`; SpringBoard ran with `DYLD_PRINT_LIBRARIES=1`): `SubstrateLoader.dylib`, `DynamicLibraries/NoSBScreen.dylib`, `DynamicLibraries/PhoneNumberFormatting.dylib` and `tlsfix.dylib` loaded. Both tweaks load.
- **Consequence:** no launchd plist names Substrate any more (`substrate.daemons` = []). SpringBoard gets it the way apps do, from ios3ds-applog's constructor (SpringBoard.app is a `.app` executable), so extrainst_'s plist cleanup has nothing to remove.

## 74. Cameras: how the 3DS CAMERA sysmodule brings up a CAM block (2026-11-20)
Source: the user's own CAMERA sysmodule code (title 0004013000001602, BLZ-decompressed `.code` copied read-only by the launcher since commit 5ec8986), disassembled in a scratch directory outside the repository; nothing of it is in the tree. Load base 0x00100000, Thumb. Below are only the facts derived from it.
- **Services:** the module gets handles for `pdn:c`, `i2c::CAM`, `mcu::CAM` (plus y2r, cfg, ptm, fs). It never touches CFG11/PDN registers itself; the CAM blocks it maps at VA 0x1EC20000/0x1EC21000 (PA 0x10120000/0x10121000), the FIFOs at 0x1EE20000/0x1EE21000 (PA 0x10320000/0x10321000), read by inter-process DMA.
- **pdn:c:** two commands. 0x00010040 with one byte (0/1): camera power/clock off/on. 0x00020000: returns a byte (whether it is already on). "Power on" = query, then on, then (only if it was off) a short busy wait. "Power off" = short wait, then off. Which register pdn:c drives is not visible from the module; the ARM9 uses CFG11_CAMERA_CNT bit 0 (0x10141224, PDN_CAMERA_CNT on 3dbrew), the only bit that sticks there (run 25).
- **mcu::CAM:** one command, 0x00010040 with one byte, called with 0 or 2: the camera LED state. Not part of the data path; ios3ds leaves the LED alone.
- **Block reset (driver initialisation and shutdown):** CAM0 CAM_CNT bit 4 cleared; two short waits; pdn:c on; short wait; CAM0 bit 4 set; wait (~1 ms); pdn:c off; both CAM_CNTs read and written back. Bit 4 exists only on CAM0 (matches run 25: it does not stick on CAM1): it is the reset of both blocks, released while the camera clock runs. All waits are busy loops of n x 48 iterations (n = 100, 500, 15, 4100), well under a millisecond each except the last.
- **Driver initialisation, both blocks (port mask 3), in this order:** reset as above; CAM_CNT bit 15 cleared (transfer off); +6 bit 15 set (overrun clear, read-modify-write of the 32-bit word at +4); transfer unit 256 bytes (+6 bits 0-5 = bytes/256 - 1, at most 10240 bytes); CAM_CNT bit 13 cleared (trimming off); +0x10 = 0x00010000, +0x14 = 0x00010000; CAM_CNT bits 8-9 = 2; bit 10 set; bit 11 cleared; then bit 14 set on CAM0 and CAM1. All CAM_CNT accesses are 16-bit.
- **Activate (per sensor, through i2c::CAM):** power on (pdn:c as above), the sensor scripts (below), then standby off (0x0018 bit 0 clear), wait 0x0018 bit 14 clear (10 ms steps, 100 ms), wait 0x301A bit 2 set, then RESET_MISC 0x001A bit 9 set (parallel output on). Same as ios3ds' `apt_activate`.
- **Sensor scripts** (a table of named scripts in the data section: register writes, MCU-variable writes and bursts, delays, waits): "Base" = reset 0x001A 3 then 0 with 1 ms after each, wakeup 0x0018 = 0x4028, PAD_SLEW 0x001E = 0x0201, CLOCKS 0x0016 = 0x42DF, the two wakeup waits, the I/O current MCU writes, SEQ_CAP_MODE 0xA115 bit 1, 10 ms, output formats 0x2755/0x2757 = 2 (YUV), MCU 0xA111 = 0x0A. "Image Setting ExtClk=16.76MHz Op_Pix=17.8MHz" = the DSi PLL values (0x0014 = 0x2145, 0x0010 = 0x0111, 0x0012 = 0, 0x0014 = 0x244B, 1 ms, 0x0014 = 0x304B, wait lock, bypass off), flicker detection MCU values (0x222D, 0xA402-0xA410, 0x2411-0x2417), output size A 256x192 and B 640x480, the DSi sensor timing, crop B full frame. Then lens correction, AE, AWB, noise reduction, gamma, and per camera read mode (0x25 outer, 0x24 inner, as GBATEK's DSi list) and AE window, then the sequencer refresh (0xA103 = 6, wait, = 5, wait). The 3DS sequence is the DSi list plus the waits and those MCU values: the sensor side of ios3ds was already right (run 25 saw it streaming).
- **StartCapture:** transfer unit = lines x width x 2 bytes (+6 bits 0-5), overrun clear, then CAM_CNT bit 15 set; the inter-process DMA reads the FIFO window in transfer-unit blocks. **StopCapture:** clears bit 15 only.
- **Ports:** camera select bits 1 = outer right, 2 = inner, 4 = outer left; port bits 1 = CAM0, 2 = CAM1 (outer right and inner on CAM0, outer left on CAM1, as GBATEK says).
- **Status reads:** CAM_CNT bit 1 (per port, a busy flag), +4 bit 15 (both ports), +4 bits 0-10 (fill), bit 13 (trimming on), +0x10/+0x14.
- **What ios3ds did differently (run 25):** (1) the clock was switched on with CAM0 bit 4 already set (left by whatever ran before), so the blocks' reset was never released with the clock running; (2) bit 14 only on the drained block, and only in one variant; (3) the shim wrote CAM_CNT = 0 to stop or to leave a block, which clears CAM0 bit 4 and so holds both blocks in reset; (4) transfer unit 0x22 (9216 bytes, not whole lines). The CNT bits themselves (0xC610) had been tried.
- **Changes:** ARM9 `power(true)` now runs the module's block reset and initialisation before the sensor set-up (`cam_reset`, `cam_block_init`, logged as `[cam-diag] after the cam module's block reset/init`), `power(false)` its shutdown; the sensor set-up gets the module's waits and extra MCU writes. Set-up variant 0 is the module's (0xC610, transfer unit 2560 bytes = 4 lines of 320); the earlier five follow. The shim stops a block by clearing bit 15 only. QEMU: CAM1 bit 4 reads 0, data only flows while CAM0 bit 4 is set.
- **Next hardware log must show:** `after the cam module's block reset/init` with CAM0 cnt `4610` and CAM1 `4600` (bits 8-10 and 14 stuck, CAM0 bit 4 released), then data in variant 0 (`variant 0` in the stats line, `<-- DATA` in the diag=1 self-test). If still nothing: pdn:c drives more than CFG11_CAMERA_CNT bit 0 (another PDN bit, to be found from the pdn module), the next thing to reverse.

## 75. Memory: Safari's "Cannot allocate memory", a [mem] report, the GPU arena, 256 MiB on the New 3DS (2026-11-20)

- **Report** (Old 3DS and New 2DS XL): Safari's first page load often fails with "The operation couldn't be completed. Cannot allocate memory" (NSPOSIXErrorDomain 12); a retry works. The hardware logs (run23/24, issue 1, issue 5) carry no Safari error and no memory figures. They show the GPU arena trouble: `[pica] arena full: 262144 bytes wanted, 1082368 free in 140 blocks`, `… 858112 free in 160 blocks` (160 = the whole block table), and after `[gl] pica: writing a surface back on the GPU failed` a 1 MiB CPU read-back on every frame (render thread 20 → 65 ms per frame, `arena full: 1048576 bytes wanted` 256 times).
- **Tools added:**
  - `[mem]` report: `ios3ds-sample -mem 20`, a launchd job behind boot-arg `ios3ds_mem=1` (`ios3ds-diag -g name` runs a job when boot-arg `name=1`); with `diag=1` the `[smp]` sampler prints it after every round. vm_stat (`host_statistics`; `free` includes the speculative pages), jetsam's `kern.memorystatus_level`, `kern.ipc.mbstat`, resident/virtual size per process (kernel zones need the host-priv port, which `host_get_host_priv_port` does not give the job: no zone line).
  - `[applog] MobileSafari load error (provisional|load): domain code userInfo`: `TabDocument`'s WebFrameLoadDelegate failures with `_kCFStreamErrorDomainKey/CodeKey`, i.e. which layer produced an error (with `ios3ds_applog=1` or `diag=1`).
  - `emu/safari_test.py` (docs/emulator.md): Safari page loads over the Wi-Fi model, URLs opened by `ios3ds-diag -urlpoll` through `SBSOpenSensitiveURLAndUnlock` (SpringBoard 8C148 wants the `com.apple.springboard.opensensitiveurl` entitlement: without it the call returns 0).
- **Memory, Old 3DS, QEMU `--icount 2`:** after boot (Wi-Fi up, 60 s settled): free 51.6 MiB of which 50.6 MiB speculative (file pages, reclaimable), active 23.3, inactive 8.4, wired 33.4 MiB (12 MiB of it the GPU arena; 17.7 MiB before SpringBoard), memorystatus_level 63. 20 processes resident 32.5 MiB: SpringBoard 11.2, mediaserverd 4.0, configd 2.8, profiled 2.4, lsd 1.8, installd 1.3, mDNSResponder 1.3, ios3ds-mcud 1.2, everything else < 0.7. MobileSafari: 14 MiB after the first page, 29 MiB after six (level 48).
- **Trying to reproduce:** no Safari alert, no jetsam kill in any of: 6/6 generated pages (images + JS) by IP; 6/6 by name through slirp DNS (`10.0.2.2.nip.io`); 8/8 with only 96 MiB for XNU (free down to 2 MiB, level 39; the 12/8/4/2 MiB GPU arena could not be had, so CA ran in software); 3/3 saved Wikipedia articles (4-5.5 MiB each, 147-390 s per page at hardware timing).
- **Ruled out by RE:** the network path cannot produce errno 12. `IOUserEthernetController::outputPacket` (0x80514110) always returns 0; a failed enqueue to the daemon's kernel-control socket is only logged (`failed to enqueue mbuf`, never seen in a hardware log). The shim's sysctls return only EINVAL/EFAULT/EBUSY. mbuf shortage is ENOBUFS in XNU 1504, not ENOMEM.
- **What the logs do show**, and what was fixed:
  - **GPU arena** (`pica_drv.c`, `pica_be.c`): blocks of 256 KiB and more go first-fit from the top of the arena, textures from the bottom; the block table grew from 160 to 224 entries (issue 1 hit 160, and a full table fails every allocation whatever is free); when a big block does not fit, the GL engine evicts the textures highest in the arena first instead of in LRU order. `test_pica`'s alloc test (150 target allocations in a 3/4 full 12 MiB arena with texture churn) evicts 9.8 MiB of textures that way against 42 MiB at random.
  - **Read-back mode** (after a refused `PICA_OP_PRESENT`): one staging buffer is kept instead of taking 1 MiB from the arena (and evicting textures for it) every frame. Making the fallback per surface (the VRAM display keeps its GPU present while a scattered 600 KiB snapshot surface is read back) was tried: QEMU reproduces the refusal (`present: destination … not physically contiguous` after an app returns to the home screen) and with that change the soak's tap after a page swipe did nothing in 2 of 2 runs (stale home screen); the old process-wide fallback passes, so it stays.
  - **ios3ds-wifid** polled the mailbox every 4 ms when idle (three syscalls and a 4 KiB copy, 250 times a second: 28.7 s of system time in 630 s in run 24, ~4.5 % of the CPU); after ~0.25 s without a frame it now polls every 20 ms. A frame from iOS still wakes it at once.
- **New 3DS: 256 MiB** (docs/n3ds.md section 6): the stub sets `PDN_LGR_CNT` bit 0 and probes 0x28000000-0x2FFFFFFF for RAM of its own (`N3RES_RAM`); the ARM9 patches `boot_args.memSize` and the device tree's `/memory reg`; the GPU arena is given back and retried if `IOMallocContiguous` places it above 0x28000000 (GPU reach unverified). QEMU's `new3ds=on` machine now has 256 MiB (its GPU model reaches the first 128). XNU needed no patch: the entry stub maps memSize with supersections and the VM takes the rest.
- **Results** (QEMU, `--icount 2`): New 3DS model, free memory after boot 180.7 MiB (Old: 51.6), memorystatus_level 81 (63); after six Safari pages 141 MiB free, level 70 (Old: 23 MiB, 48); the GPU arena landed at PA 0x26206000 and CA stayed on the GPU. Safari 6/6 pages on both models with the changes; `emu/soak.py --cycles 5` PASS on Old and New (one Old run failed only the lock round's unlock screenshot, as `rel`'s soak on main did; the rerun on a fresh image passed). `make test` green.
- **Root cause, as far as QEMU shows:** not physical memory (96 MiB with 2 MiB free still loads), not jetsam, not our network path (above). Re-log 72 gets the same text deterministically for HTTPS in Safari once TLSFix is in it (SANDBOX=1), which points at an allocation inside Safari's own process rather than system RAM; the stock hardware reports (runs 23/24 predate TLSFix) could not be reproduced. MobileSafari now always logs the failing NSError with `_kCFStreamErrorDomainKey/CodeKey` (`[applog] MobileSafari load error …`), so the next hardware log names the layer.

## 76. Cameras, run 30: the 1-2 s preview loop was the GPU's texture cache; frame alignment at the vertical blank; stills (2026-11-21)
Log: build/hwlogs/run30.txt (build 44), frame dumps reviewed by the coordinator (private, not in the tree).
- **Loop:** the kernel delivered fresh frames (words = frames x 38400, 0 overruns; consecutive dumped frames differ), the plug-in converts every frame into a pool CVPixelBuffer and enqueues it, yet the user saw 1-2 s of frames repeating. Run 30's `[gl] CA` lines: **0.0 texture uploads per frame** while the camera ran at ~15 fps. CA binds each IOSurface of the plug-in's pool to a texture once; on Apple's GPU that texture is the surface memory, but ios3ds' PICA200 back end keeps a tiled copy and re-uploaded only when GL said the image changed. So the preview cycled through the pool's first frames. Fix (`tools/ios3ds-gl/pica_be.c`): a surface-backed texture is re-uploaded when the surface's seed (`IOSurfaceGetSeed`, if present) or a 64-texel sample of it changed. vcd itself was right (a fresh pool buffer per frame, written before enqueueing).
- **QEMU check:** the camera pattern now carries a frame counter (a grey 64x48 patch in the middle, Y = 40 + frame mod 160) and `tools/ios3ds-camera/tests/preview_counter.py` reads it off screenshots of a running session. Same session recipe, image without the GL fix: `FAIL (steps back by -5: the preview repeats old frames)`; with it: `PASS` for the outer and the inner camera (QEMU delivers 2-3 fps under load, the counter climbs and never steps back).
- **Alignment:** a dumped frame was shifted by 16 pixels (8 words) with its right edge wrapped: words lost (or doubled) without the overrun bit; run 30's fill maximum sits at exactly 560 units in every window, likely the FIFO's real cap. The shim cannot see VSYNC, but the vertical blank shows as two or more 1 ms services in a row with an empty FIFO. A frame is now published only if its 38400th word is the last before such a blank; a partial frame at a blank or words after a complete one drop that frame (`resyncs`, "blank drops" in the 10 s line) and the next frame starts aligned. A pause counts as the blank only within 256 words of a frame boundary (QEMU pauses mid-frame under load). Host tests: a lossy FIFO model (8 words vanish every 3rd frame, 4 come twice every 5th) delivers only whole frames; mid-frame pauses cost under 10 % of the frames.
- **Stills:** run 30 has no `StillImageCaptureNow` at all (0 stills; one tap near the shutter at 149,437 while the outer camera ran): the request never reached the plug-in. In QEMU the same build takes photos upright and with the console face down (`emu/session.py set mcu-orient 5`, run 30's accelerometer read as face down, SpringBoard orientation 6), and Camera Roll gets them. The plug-in now logs every still request (`still requested (frame N)`) and any failure on the still path, so the next log says whether Camera.app asked.
- **Next hardware log must show:** `[gl] CA` lines with texture uploads per frame > 0 while the camera runs, a preview that moves; `0 blank drops` or a small count in the 10 s line (a large one means the FIFO keeps losing words: CDMA drain next); after a shutter tap `still requested` and `still image #1`, or nothing (then Camera.app never asked: look at the shutter's state on the screen).

## 77. Cydia on the first home page, and a warm SpringBoard folder (2026-11-21)

- **Home screen layout:** a freshly built rootfs had no `/private/var/mobile/Library/SpringBoard/IconState.plist`, so SpringBoard created its own on first boot and put Cydia alone on page 2 (re-log 60). The build now writes Apple's 4.2.1 iPod2,1 layout into the image: the 4 dock icons, the 15 stock page-1 icons in their original order including the Utilities folder (Clock, Calculator, Voice Memos), and `com.saurik.Cydia` in the first free slot of page 1. It lives in a `cydia_plist_write` group in `rootfs/recipe.json`; `scripts/mkrootfs.py` applies it only when Cydia is actually installed (creating the parent directory with its inherited owner, 501 under `/var/mobile`), so `--no-cydia` keeps the stock first-boot behaviour. `tests/test_cydia.py` checks the plist is serialisable, has 21 unique icons, only the one Utilities folder and at most 16 icons per page.
- **Folder warm-up:** the first folder open on a fresh image stalls ~2 s of guest time (QEMU `--icount 2`; several seconds on the console) while `lsd` renders every icon's ghostly image (`getIconImage:4`), `+[SBLinenView _generateLinenStripsIfNeeded]` draws the folder linen and `+[SBIcon homeScreenIconOverlayImage]` builds the overlay. The caches survive reboots. `tools/ios3ds-diag/applog.c` (inserted into SpringBoard) now does that work in the background: a CFRunLoopTimer starts 30 s after SpringBoard loads, runs one item per 0.5 s tick in the default runloop mode (so not during a touch drag) and logs `[sbwarm] linen, icon overlay and N of N ghostly icons ready (X ms on the main thread, done Y s after the first step)`. Boot-arg `ios3ds_sbwarm=0` disables it. `sb_responds()` now masks the BOOL in r0's low byte instead of testing the whole register.
- **QEMU check** (`emu/session.py start --icount 2`, fresh image): `[input] connected to SpringBoard` at ~80 s; the warm-up reported 28/28 icons, 2050 ms of main-thread work done 15 s after its first step. `page1.png` (t=139 s) shows Cydia on the **first** page (row 4, column 3, right of Camera and below iTunes) and the Utilities folder still in row 3, column 2, with the stock dock at the bottom. Tapping Utilities at iOS point (120,237) left the home screen unchanged at 1 s and 2 s, was mid-slide at 4 s (dark linen panel rising, chevron, dimmed home icons behind) and fully open (Utilities: Clock, Calculator, Voice Memos) at ~10 s of QEMU wall time; a clean re-run matched the fully-open screenshot pixel-for-pixel at 10 s and stayed open through 15 s. `emu/soak.py --cycles 5 --icount 2` on a fresh image: PASS (boot 80 s, 5 cycles, 0 panics/stalls/crashes/md0 errors, no recoveries). `make test` green; `make userland` and `make sdimg` clean.
- **Also:** `rootfs/files/com.ios3ds.sample.plist` now samples every 10 s instead of 60 s, so short QEMU runs have `[smp]` data early.

## 78. New 2DS XL run 1: scattered CA surfaces through an arena copy, SELECT+START's lost write (2026-11-21)

- **Log** (`docs/hwlogs/n2dsxl-run1.txt`, build 0.4.0 m1-0.3.0-62-g6d963b6): the New 3DS work runs on hardware: 804 MHz, timers /192 >>1 at 402.15 MHz, clock ratio 1000/1000 over 33 min, `FCRAM extension: iOS gets 256 MiB`, C-stick/ZL/ZR, no panic.
- **The read-back mode:** line 814, `[pica] present: destination 0x04d94000 (+614400) not usable: not physically contiguous (PA 0x271fa000 at +0x1000 …)` after an app went back to the home screen, then `reading surfaces back through the CPU from now on`. CA's render thread went from 13.5-16 ms per frame (GPU presents) to 31-37 ms (1.0 CPU read-back of 600 KiB per frame) for the remaining 30 minutes. The same fallback is in run 21, 25, 29 and issues 1 and 5 on Old consoles.
- **Fix** (`tools/ios3ds-gl/pica_be.c`): a target whose present is refused with `PICA_ERR_NOT_CONTIG` keeps its swizzle pass and runs the same PPF transfer (format, flip) into a linear 600 KiB arena buffer; the shim's `READ` then copies it out with `copyout` through the caller's own mapping, so the scattered pages are handled by XNU and no pixel passes through the CPU in userland. All other surfaces keep the direct present; other refusals still switch the process to the read-back. CA's 300-frame line counts these as "via an arena copy". Host test: every destination refused (`IOS3DS_HOST_PRESENT=0`) and the present check still passes against the CPU read-back. The QEMU soak reproduces the refusal: CA's 320x480 surfaces at 0x4a74000 / 0x4882000 after the 3rd and 4th app.
- **The "stale home screen" of re-log 75** was the soak's timing, not the display. Screenshots every 2 s of the soak's steps (`emu/soak.py` cycle 4): 4 s after the swipe back the page is still scrolling (offset, page 2's edge in view); the tap then only stops the scroll and YouTube does not open; the page settles on page 1 correctly. With the frames presented on the GPU each one costs QEMU's software PICA200 a full swizzle draw, which takes host time but no guest time, so a scroll lasts longer in wall-clock seconds than with the CPU read-back; with a longer wait the same tap opens YouTube. `emu/soak.py` now waits for the screen to stop changing after the swipes (and holds a touch 0.15 s before moving, since `--icount`'s idle time warp made a 0.4 s swipe start 50 points late: `[input] down 88,240`).
- **QEMU, `--icount 2`, after the fix:** two scattered surfaces in the soak steps (0x4882000, 0x4904000), CA keeps 1.0 GPU presents and 0.0 read-backs per frame, render thread 7.5-8.6 ms per frame over 600 frames of page swipes (before: from the first scattered surface on, every frame was a read-back; re-log 51 measured 19-27 ms per frame for that under `--icount 2`). Hardware before (this log): 13.5-16 ms with presents, 31-37 ms after the fallback.
- **SELECT+START** never panicked on hardware (two presses, line 3515 and later). The request word was shim+0x6C, in the cache line of `irq_save0-2` and `st_vblanks`, which the ARM11 writes on every interrupt: the line stayed dirty in the L1, the shim read its own stale 0, and each write-back put the 0 over the ARM9's 1 in RAM. QEMU has no caches, so it always worked there. The word is now shim+0x6500, a line only the ARM9 writes (zeroed at boot, it is outside shim.bin); `shim_set_decrementer` invalidates it in the L2 (New 3DS, `n3ds_l2=1`) and the L1 before reading, and `shim_user_panic` cleans its 0 back. QEMU, Old and `--new3ds`: `panic(cpu 0 …): ios3ds: panic requested from the 3DS (SELECT+START)`.
- **Volume slider:** not a bug. MCU register 0x09 reads the same on the New 2DS XL (the log has the slider moving 0 → 56 → 2 → 44 → 0/63); iOS's volume follows it only when the slider moves, so L/R (HID volume up/down) still change iOS's volume with the slider at 0. Whether the CODEC also applies the slider in hardware is not checked.

## 79. PICA_OP_SCATTER: per-page copy into a scattered CA surface (2026-11-21)

- **Entry 78's implementation was only an intermediate step.** Copying the frame out of the arena with `PICA_OP_READ`+`copyout` works on the host (the host "physical" memory is one flat allocation), but the shim's `READ` needs one contiguous *physical* range to validate; the very destination that `PRESENT` refused cannot be named that way. Protocol version 4 adds **`PICA_OP_SCATTER`** instead: the engine still runs the swizzle pass and the same PPF transfer into a linear arena block; then the shim walks the caller's page tables again for every 4 KiB page (user read/write, AP=3/APX=0, FCRAM only — VRAM has no kernel linear map), maps that page at `pa + 0x60000000`, cleans+invalidates the caller's own lines for it, copies the 4 KiB through the kernel mapping, writes those lines back and invalidates the New 3DS L2 line. Any page that fails the walk, or a GPU error, is the ordinary process-wide read-back fallback; only a non-contiguous destination takes the detour.
- **Host tests** (`make test`): 6678 pica checks + 39 input checks, 0 failures; `ios3ds-gl` host-test passes five runs, including every destination forced non-contiguous (`IOS3DS_HOST_PRESENT=0`), the new PRESENT test flag (`IOS3DS_GL_SCATTER=1`) and the wrong-swizzle run. `test_pica`'s `test_scatter` copies a source block into three scattered pages (0x30000000, 0x30004000, 0x30011000), checks three caller-line invalidations and three write-backs, checks a partial last page and refuses unmapped, unaligned, zero-length and non-arena requests. Both forced runs log "through an arena copy" and "GPU present check passed" with no CPU read-back. `ca_frame()` also no longer underflows its CPU-time average when SpringBoard's render thread restarts (the short-interval hook made that visible).
- **Shim size:** the GPU part was 32144 of 32768 bytes — 624 bytes of headroom, less than CI's devkitARM overhead (~600). `scripts/mkboot.py` now compiles only `pica.c` and `pica_mirror.c` (the hot request/IRQ paths) at -O2; `pica_drv.c`, `cam.c` and the already -Os `pica_diag.c` are cold-ish and compile at -Os: GPU part **31216** bytes with the test flag (1552 free, ~950 left after CI's compiler). mkboot prints both sizes on every `make sdimg`.
- **Soak harness:** a first 5-cycle `--icount 2` soak reached the scattered surface and kept it on the arena copy (no "reading surfaces back through the CPU" line), but cycle 4's unlock screenshot was black: `settled()` accepted two identical black frames while the backlight was fading in. It now requires brightness > 10 as well as two equal frames (the home-screen reference loop already did); the rerun on a fresh image is in the bullet below.
- **QEMU proof, Old and New 3DS, fresh images, `--icount 2`:** `emu/soak.py --cycles 5` printed `Result: PASS` on both models (0 panics, stalls, I/O errors, crashes; home-screen diff 0.0-2.4 per cycle, after-unlock diff 0.00-0.02). Both logs contain a real refusal from CA's second surface — `[pica] present: destination 0x04af6000 (+614400) … not physically contiguous` (Old; 0x04a74000 on New) — followed by `[gl] pica: surface … through an arena copy` for two surfaces, and **no** "reading surfaces back through the CPU" line. The screen is not stale: every Home tap restores the reference screenshot and the lock round ends on the home screen. A separate 5-cycle run with `IOS3DS_CA_LOG_FRAMES=10` **and** `IOS3DS_GL_SCATTER=1` (test hooks in SpringBoard's environment of that image only) makes the detour deterministic — `[pica] present: destination 0x04800000 … not physically contiguous`, `[gl] pica: surface 0x4800000 … through an arena copy` — and brackets it in `[gl] CA:` lines: frame 77 `render thread CPU 26999 us, 1.4 GPU presents (0.0 via an arena copy)`; frame 88 `CPU 28999 us, 1.6 GPU presents (0.1 via an arena copy)`; frame 143 `CPU 25999 us, 1.6 GPU presents (0.1 via an arena copy)`. The 10-frame windows include app launches (and texture uploads), so they are not a clean per-frame benchmark: the steady-state 300-frame averages are in entry 78 (7.5-8.6 ms on the GPU, 19-27 ms in QEMU's CPU read-back, 31-37 ms on hardware). What these lines show is that the bounce adds one copy with **no** process-wide "reading surfaces back through the CPU from now on" line. The VRAM display keeps its direct present throughout.
- **SELECT+START (7a0d4ac, re-verified here):** QEMU, Old and `--new3ds`, both pressed after SpringBoard came up: `[monitor] SELECT+START: asking XNU to panic (thread dump)`, then `panic(cpu 0 caller 0x80840710): ios3ds: panic requested from the 3DS (SELECT+START) - thread dump follows`, with 101 (Old) and 44 (New) `backtrace:` lines. The dedicated cache line at shim+0x6500 is what the hardware fix needs; QEMU has no caches, so this re-run only checks the wiring, not the hardware failure mode.

## 80. Voice Memos playback, silent headphones, songs in the Music app (2026-11-21)
Design: docs/audio.md §4 (headphones), §9 (Voice Memos), §10 (Music). Hardware input: run 32 (0.4.0-era build).
- **Voice Memos, cause:** run 32 and QEMU both log `Remaking completed with status: AVRemakerNotification_Failure ... OSStatus error -50`. The `.mov` pulled from the QEMU image is iLBC 8 kHz mono (`stsd` `ilbc`), its `mdhd` at 44,100: not a rate problem (the 3DS mic's 11,901 Hz is resampled to the I2S frame rate in the shim). VoiceMemos `-[RCAudioInputDevice _createCaptureController]` (0x1e5b4, disassembled with capstone): `VMDebugLosslessRecording` ? `'lpcm'` : `GSSystemHasCapability(kGSEncodeAACCapability)` ? `'aac '` : `'alac'`. The only AAC encoder in the dyld cache is `ACAACEncoder_IOA2` (`AudioCodecHWCodecLookup`, S5L8720 hardware; software encoders: ALAC, IMA4, AMR, iLBC), so Celestial records iLBC and the pass-through remake into `.m4a` fails.
- **Voice Memos, fix:** `encode-aac` = false in N72AP.plist (recipe `plist_merge`) -> `'alac'`. QEMU (`IOS3DS_MIC=sine:440:3000`, `--icount 2`): `Remaking completed with status: AVRemakerNotification_Complete`; the `.m4a` is ALAC 44,100 Hz mono, decodes on the host to the sine at peak 3024 (source 3000); playing it from the list gives `[csnd]` non-silent frames.
- **Headphones, cause (static, no hardware access):** run 32 shows the whole iOS side working (`headphones plugged in`, `iOS route -> headphones`, `CS42L58 6a (-22 dB) -> CSND volume 2068/32768`, stream peak 8583) and silence. The logged "CODEC headset detect 91" is page 100 reg 0x43, the detection control we write, so it says nothing. Against libn3ds `CODEC_init`/`soundInit` (open_agb_firm's sequence; GBATEK "3DS TSC, Register Summary") 0.4.0 skipped the headphone filter coefficients (page 11), never drove the Old 3DS de-pop line (GPIO3 bit 0; GBATEK's post-bootrom GPIO3 value 0x0DFB has it high) and powered the headphone driver only at boot, while the output select pointed at the speaker.
- **Headphones, change:** `boot.cfg hp_codec=1` (default): page 11 filters (pass-through + 10 Hz high-pass, hw_cal.c defaults) before the DACs power up; de-pop output low once the MCU reports system model 0 (reg 0x7F byte 9; New 3DS untouched, the pin is the C-stick IRQ there); on each plug-in page 101 reg 0x0b 0 -> 0x3c/0x1c, 0x0c unmute, 0x16/0x17 analog volume, 38 ms. `hp_codec=0` = 0.4.0. Diagnostics: `[audio] CODEC at boot|headphones|speaker: ...` read back page 100 0x2c (the CODEC's own jack status) 0x43 0x45 0x25 0x26 0x76, page 101 0x0a 0x0b 0x0c 0x16 0x17 0x11, page 0 0x3f-0x42, page 1 (TSC2117) 0x1f 0x24 0x25 0x28 0x29, GPIO2/GPIO3; the jack line now prints 100:2c. QEMU (CODEC stub reads 0): `[audio] de-pop (Old 3DS): GPIO3 bit 0 was input 0 -> output 0`, dump lines present, boot unchanged.
- **Music, format (dyld cache strings + disassembly, MusicLibrary-720.20):** 4.2.1 reads `iTunes_Control/iTunes/iTunes Library.itlp/{Library,Locations,Dynamic,Extras,Genius}.itdb` (no `MediaLibrary.sqlitedb`, that is iOS 5). With no library `_MLSDBLoadAndInitSharedInstances` only runs `/usr/local/bin/ddbtosql` (not on retail builds) on an `iTunesDB`; the rootfs has no template; writers are only `com.apple.mobileipod`, iPodOut and `itdbprepserver` (`MLProcessHasDatabaseWritePermissions` 0x30094488). A first QEMU boot with the Music app created nothing. `SQLMusicLibraryPostProcessCommands.plist` carries command set "8" = the user_version libgpod's iTunes 9 schema declares (`db_schema_upgrade_info` 8, 19).
- **Music, cbk:** `MLSSqliteVFSOpen` (0x3000a4f4) asks `_AOU243378_7_1` (0x3008530c) for `<file>.cbk` when the process may write and the file is in the managed list, then verifies SHA-1 blocks plus iTunes' device-keyed signature (libgpod `mk_Locations_cbk`). Patched to `mov r0,#0; bx lr` (cache file offset 0x8530c).
- **Music, build:** `scripts/music.py`: libgpod-style rows (items, albums, artists, genre_map, location_kind_map, hidden master playlist "iPod", `location` relative to base 1 `iTunes_Control/Music`, item_stats, container_ui), files as `Music/F00/XXXX0000.ext`; test song = own melody, additive synthesis, ALAC `.m4a` with escape frames (host test decodes it back bit-exact; ffmpeg/ffprobe read it as alac 44.1 kHz stereo). `--music DIR` in install.py (default `SD/ios3ds/music/`), mksd.py, mkrootfs.py.
- **Validation (this branch):** `make test` (55 tests incl. `test_music.py`), `make userland` and a fresh `make sdimg` (1 song in the library) pass. `python3 emu/soak.py --cycles 5 --icount 2` on a fresh image: **Result: PASS** (0 panics, hangs, md0 errors or crashes; home screen recovered in all 5 cycles). Several fresh-image soak runs failed only the cycle-4 lock round's unlock screenshot: the slide-after-wake gesture was swallowed while the input daemon still polled at 10 Hz (re-log 75). `emu/soak.py` now retries a missed unlock slide (recorded as `unlock_attempts` in report.json); the final run above passed.

## 81. Slide to power off over an app: SBAlertWindow is modal too (2026-11-21)

- **Report** (hardware run 27): with Camera open, holding POWER and dragging the power-off slider did nothing. The daemon's debug log showed `[input] touch target com.apple.camera` for the drag: the touch still went to the app's purple port because SpringBoard's frontmost app stays Camera while its slider covers it. The modal-state publisher in `tools/ios3ds-diag/applog.c` checked `SBAwayController isLocked`, `SBAlertItemsController hasVisibleAlert` and `UIApplication topMostAlert`, but the slider is an `SBAlert` in an `SBAlertWindow`, which `topMostAlert` does not report.
- **Fix:** in `modal_tick()`, when bit 2 is not already set, walk `UIApplication sharedApplication`'s `windows`, look for a visible (`isHidden` NO) window whose `object_getClassName()` is `SBAlertWindow`, and set bit 2 of `*com.ios3ds.sb.modal`. `object_getClassName` is resolved once in `modal_start()` next to `objc_msgSend`; the window access uses the existing `respondsToSelector:` guards and reads the BOOL from r0's low byte. The daemon's diagnostic now logs a touch target on every touch while modal (QEMU's frontmost-app id can stay empty while an app loads) and names an alert (bits 1/2 of the state) before the lock screen, so the log says `touch target SpringBoard (alert)` for the slider.
- **QEMU check** (`emu/session.py start --icount 2`, fresh image): `[input] connected` at 110 s; tapping Settings (51,323) logged `touch target com.apple.Preferences`; lock-button holds brought up the slider (screenshot `slider.png`); `drag 30 60 300 60 1.2` logged `touch target SpringBoard (alert)` and iOS shut down (`ios3ds-mcu: halt: asking the ARM9 to power off through the MCU`, launchd's shutdown line, QEMU exited). Repeated after rebasing this entry onto the current `main`: connected at 140 s, the same `touch target SpringBoard (alert)` and shutdown. An instrumented run first confirmed the causal chain: `[applog] sb.modal 5` (locked + alert) and `[input] modal 5` on the drag's touch-down. `make userland` and `make sdimg` clean.
- **Soak:** `python3 emu/soak.py --cycles 5 --icount 2` on a fresh image: PASS (boot 93 s, 5 cycles, 0 panics/stalls/crashes/md0 errors). After the rebase, a first soak hit the known Spotlight-after-Home recovery flake (cycles 1 and 2, no panics or I/O errors); the rerun on a fresh image passed (boot 162 s, 5 cycles, 0 panics/stalls/crashes/md0 errors, one recovered "Dismiss" cycle). `make test` green before and after the rebase.

## 82. User data carry-over on update (revised) (2026-11-21)

- **Problem:** every install/update rebuilt `sd:/ios3ds/rootfs.hfs` (the iOS disk), wiping saved Wi-Fi networks, settings, photos, voice memos and any other user data.
- **Fix:** `scripts/carryover.py` copies the user-data subtrees from the old image into the new one with xpwn's `hfsplus` (extract/add/mkdir/rm/chmod), preserving mode and the inherited owner: `/private/var/mobile` (Media, Library/Preferences, Library/SpringBoard, Library/Keyboard, ...), `/private/var/preferences/SystemConfiguration` (saved Wi-Fi networks) and `/private/var/Keychains` (Wi-Fi passwords and the keychain). `/private/var/lib/dpkg` and installed Cydia package files are deliberately not carried: an update can change Cydia/Substrate/the base system, so carrying dpkg state plus package files would desync from the new image and leave broken packages; reinstall packages from Cydia after updating.
- **Exclusions:** `Library/Caches` and `Library/Logs` under `/private/var/mobile` are skipped (MobileInstallation's app list cache would be stale after an update), plus any `*.sqlite-journal` / `*.lock` leftovers. `/private/var/mobile/Media/iTunes_Control` is skipped too: re-log 80's Music library is rebuilt from `SD/ios3ds/music` and the test song on every install, so the new library must win over one carried from the old card.
- **Safety:** before copying, the old data's total size is compared with the new image's free space (parsed from `hfsplus ls`); if it does not fit, or any entry fails to copy or verify, the old image is kept on the card as `sd:/ios3ds/rootfs.hfs.bak` and a warning with that path is printed. `carry_over()` returns a string-like result `"carried"` / `"partial"` / `"nothing"` with a `reason` and the `backup` path for callers.
- **Opt-in for developer builds:** `make sd`/`make sdimg` no longer carry from the previous build output (stale SpringBoard defaults would move from build to build and soak images would stop being reproducible); pass `CARRY_FROM=/path/to/old/rootfs.hfs` (or `IOS3DS_CARRY_FROM`) to carry from an explicit image. `install.py` (the user's card) still carries by default, with `--fresh` to skip, and ignores those env vars.
- **Ownership:** xpwn's hfsplus has no `chown` command (its usage lists `ls|cat|mv|mkdir|add|rm|chmod|extract|extractall|rmall|addall|debug`; its source is in the shared `build/xpwn` symlink, not this repo). Like `scripts/mkrootfs.py`, `add`/`mkdir` inherit owner and group from the parent directory, so carried files keep 501:501 under `/private/var/mobile` and root under Keychains/SystemConfiguration. A post-copy pass compares mode/uid/gid/size against the old image for every entry, so a wrong owner becomes a `partial` result instead of silent data loss; a host test builds images with 501-owned parents to check it.
- **Speed:** one `ls` per directory in the old image is cached and reused for the size check, the copy and the verification; directories are created once (no `exists` check per file). `tests/bench_carryover.py` (1000 small files plus 50 x 300 KiB in `Media/DCIM`): `carried` in **32.4 s**, well under the two-minute budget, so no further process batching was needed.
- **Validation:** `make test` green after the rebase onto main (63 Python tests, incl. the carry-over cases and the `make sd` opt-in check, plus the pica/gl/wifi/input host tests). End-to-end `install.py --staging build/staging --no-cydia --no-tlsfix` into a temporary folder used as a card: an old 16 MiB `rootfs.hfs` with a 501:501 plist, a 64:0 keychain file, a root-owned SystemConfiguration plist, a photo, a stale cache and an old `iTunes_Control/old.itdb` produced `carry-over result: carried`; the new card has the four user files with their old contents, modes and owners, no stale cache/old library, and a freshly built 1-song Music library. No `.bak` was created. With a deliberately root-owned keychain file (a parent owner change the hfsplus CLI cannot repair) the same path returned `partial`, kept `sd:/ios3ds/rootfs.hfs.bak` and printed the warning — no silent loss. A fresh `rm -f build/sd.img && make sdimg` (no `CARRY_FROM`) has no carried IconState and `python3 emu/soak.py --cycles 5 --icount 2` printed **Result: PASS** (boot 140 s, 5 cycles, 0 panics/stalls/md0 errors/crashes).
## 83. Fake GPS location: boot key, runtime tool, app-side CoreLocation hooks (2026-11-21)
Design: docs/location.md. No daemon is injected: `locationd` is disabled in the
recipe and Substrate's launchd injection hangs 4.2.1 (re-log 71/73). We already
insert `ios3ds-applog.dylib` into SpringBoard and every app (including sandboxed
MobileSafari, which can read `/usr/lib` but not `/Library`), so the hooks live
there.
- **Path of the key:** `boot.cfg gps=52.5200,13.4050` → `boot/arm9/main.c`'s
  `cfg.gps` (`parse_bootcfg`) → `ba->CommandLine` boot-arg `ios3ds_gps=...` →
  `sysctlbyname("kern.bootargs")` in applog's constructor. The tool writes an
  XML plist under `/var/mobile/Library/Preferences/com.ios3ds.location.plist`
  and posts `com.ios3ds.location.changed`; the dylib's Darwin observer reloads
  it. `gps_parse` (shared by tool, dylib and host test) validates -90..90 /
  -180..180 / acc > 0, default 50 m; the plist wins over the boot key, so
  `ios3ds-gps off` stays off until the disk is rebuilt.
- **Hooks:** `+locationServicesEnabled` → 1, `+authorizationStatus` → 3,
  `-location` → a fresh `CLLocation` (now timestamp, configured horizontal and
  vertical accuracy, course/speed -1), `startUpdatingLocation` → register a
  manager in a *non-retaining* CFArray, `stopUpdatingLocation` → unregister.
  A 1 Hz `CFRunLoopTimer` calls `locationManager:didUpdateToLocation:fromLocation:`
  on each registered delegate. Without a configured location no hook is
  installed; the Darwin observer is the only always-on addition (needed for
  runtime `set` from an off state).
- **Three QEMU traps found and fixed:**
  1. `CLLocationCoordinate2D` is 16 bytes: calling `-[CLLocation coordinate]`
     through a plain `objc_msgSend` function-pointer cast crashes (status 11 in
     a guarded probe child). `objc_msgSend_stret` is required. The initializer
     with the struct as an *argument* works with the ordinary cast.
  2. `CFPreferencesCopyAppValue` kept returning the boot value after
     `notify_post()` — the domain is cached per process on 4.2.1. The dylib now
     reads the plist files directly; the tool also writes a copy to
     `/private/var/tmp/com.ios3ds.location.plist` for sandboxed apps.
  3. A plain C client has no main run loop: neither the 1 Hz timer nor the
     Darwin notification fired until the test tool called
     `CFRunLoopRunInMode(kCFRunLoopDefaultMode, 1.0, 0)` each second.
- **QEMU evidence** (`emu/session.py start --icount 2`, image with
  `gps=52.5200,13.4050` and `ios3ds_gpslog=1` in `cmdline`; the test job is
  gated by `ios3ds-diag -g ios3ds_gps`):
  `[gpstest] services=1 authorization=3`, then one `[gpstest] delegate: 52.520000,13.405000 acc 50.0 m`
  and one `[gpstest] location: ...` per tick; at tick 6
  `location set to 40.712800, -74.006000 (accuracy 20.0 m)`,
  `[gps] Darwin notification received`, `[gps] ios3ds-gpstest: on 40.712800,-74.006000 acc 20.0 m`,
  and the following delegate/location lines carry 40.7128,-74.006 / 20 m.
- **Host tests:** `make test` includes `tools/ios3ds-gps` (bounds, non-finite,
  whitespace, malformed values). Soak result: see below.
## 84. WPA2+WPA3 from the "Ask to Join Networks" popup (2026-11-21)
Design: docs/wifi.md §10.8. Hardware report: Settings joins the user's WPA2+WPA3 home AP
(a FRITZ!Box in WPA2+WPA3 mode), the "Select a Wi-Fi Network" popup does not.

- **Where the two paths differ:** both issue the same Apple80211 ASSOCIATE once
  iOS has accepted the network. QEMU logs `req_len 384, ssid_len 11,
  key_len 32, cipher 6 (APPLE80211_CIPHER_PMK), auth 1/8` for Settings and for
  the popup. Section 66's ARM9 translation (`wpa2_rsn_parse` -> mixed ->
  `CONNECT_PROFILE_MATCH_DONE`, WPA2-PSK/CCMP) works for either request.
- **Cause (QEMU, mixed AP, before the fix):** with `IOS3DS_WIFI_LOG=2` and
  WiFiManager's own log on `/dev/console`, joining from Settings printed
  `Attepting Apple80211AssociateAsync`, `Attempting to join WPA network:
  ios3ds-qemu`, `Failed to associate with ios3ds-qemu: -100` and
  `Completed Apple80211AssociateAsync (-100)`: **no SET 20** reached
  `ios3ds-a80211.dylib`, so the failure is WiFiManager's classification of
  the scan record, not the driver. The mixed RSN IE (AKM PSK + SAE, MFPC) is
  copied verbatim into every 0x8C-byte scan result (GET 11/201); iOS 4.2 has
  no SAE, and the popup uses the same records to decide what it may offer.
- **Fix:** `wlan_ie_wpa2_view()` in `boot/common/wifi_proto.h` rewrites the
  *iOS copy* of each scan IE: keep AKM PSK (00-0F-AC:2), drop SAE/PSK-SHA256,
  clear MFPC/MFPR, close the gap, shorten the element (and move later IEs).
  Used from `fill_result()` in the configd dylib, so both UI paths see WPA2.
  The ARM9's scan table still holds the original IE: it logs
  `"ios3ds-qemu" is a WPA2 + WPA3 network: joining with WPA2-PSK` and still
  sets the firmware's PROFILE_MATCH_DONE bit. Pure SAE/MFPR IEs are left as
  they are (one ARM9 refusal line, re-log 66); plain WPA2 and open IEs are
  unchanged. A diagnostic `[a80211] associate ... req_len/key_len/cipher`
  line makes the two paths comparable without logging the PMK.
- **Host tests:** `make -C tools/ios3ds-wifi test` covers mixed AKMs in either
  order, a following vendor IE, plain WPA2, SAE-only, open IEs and a
  truncated RSN; `make test` passes.
- **QEMU:** `emu/wifi_popup_test.py` drives Settings > Wi-Fi (password typed on
  the on-screen keyboard), then forgets the network, opens Mail (no known
  network, "Ask to Join Networks" on), picks ios3ds-qemu from the SpringBoard
  `Select a Wi-Fi Network` alert, types the password and joins again. The
  exact popup screenshot and the `[wifi] link up` / `[wifid] en0 configured
  by configd` lines are in `build/emu-wifi-popup/`; results TBD.

## 85. Driver stability: inventory, stress harness, input and MCU fixes (2026-11-22)
Goal (user): every driver stable inside iOS 4.2.1. This section covers the
drivers no other worker owns: GPU/Core Animation and the screen copy, touch +
buttons + circle pad/C-stick, accelerometer/gyroscope, audio out (speaker and
headphones), microphone, cameras, the MCU daemon and the SD read path. The
per-driver inventory (code, tests, open hardware checks, verdict) is
[docs/driver-status.md](driver-status.md); it is kept there instead of
repeating the table here.

- **Stress harness.** `emu/stress.py` (docs/emulator.md "Driver stress") opens
  the five apps whose home-screen slot `rootfs/recipe.json` pins (Photos,
  Settings, Notes, Safari, Music), drags in them, swipes pages with the D-pad,
  scrolls with the circle pad, starts/stops Camera.app, plugs and unplugs the
  emulated headphones, cycles the accelerometer poses and locks/unlocks.
  `--diag` turns on the boot jobs (gltest, gyrotest, the `[smp]` sampler) and
  the default `--mem` adds `ios3ds_mem=1`; it reads `sd:/ios3ds/log.txt`
  while the guest runs and reports panics, stalls, md0 errors, GPU
  timeouts/present-check failures/CPU fallback, camera init failures and FIFO
  overruns, audio DMA/CODEC errors, input stale-port messages, the MCU
  reconnect line, `[mem]` free memory and `[smp]` samples. It needs a fresh
  image per run (it edits that image's boot.cfg) and fails when a requested
  camera cycle produced no start/stop.
- **`emu/soak.py`** gained the same driver error checks, so the long soak
  catches a GPU timeout, present-check failure or CPU fallback, a camera
  init failure/overrun and an audio DMA/CODEC error, not just a panic. Its
  cycle now waits for the app to render before pressing Home: on a loaded
  `--icount 2` host the app's uniform gray launch image differs from the home
  screen enough to look like a finished frame, and Home pressed on it is
  ignored (the old fixed wait then made every such cycle "stuck").
- **Input: leaked app send right and dead-name touches.**
  `send_touch_gs()` cached the frontmost app's `PurpleSystemEventPort` send
  right in a static. `GSCopyPurpleNamedPort` returns +1 and the old value was
  overwritten on every app switch: one leaked send right per switch (the
  32-bit port-name table is finite). The same cache could outlive its app: a
  relaunch under the same bundle id kept `strcmp(id, app)` equal while the
  cached name was dead, so touches were silently dropped. The daemon now
  releases the previous right (`app_port_set`) and checks
  `mach_port_type(..., MACH_PORT_TYPE_DEAD_NAME)` before reuse, re-resolving
  and logging `[input] stale touch port ...` under `debug`.
- **Input follow-up: the cached id was stale for the lookup, and home taps
  need the system port.** Re-resolving a dead port was not enough:
  `SBFrontmostApplicationDisplayIdentifier` can keep returning the app that
  just went away, so the lookup used the stale bundle id again.
  `SBSCopyFrontmostApplicationDisplayIdentifier` returns NULL when SpringBoard
  is frontmost (and the app id otherwise) and is used when available, with the
  old call as the fallback. With an empty id home-screen touches go through
  `GSSendSystemEvent`, which does hit-test (apps opened and `touch target`
  lines appeared only for app switches in the PASS runs). Touches also go to
  SpringBoard for 1.5 s after a Home press (`home_grace_until`): SpringBoard's
  frontmost state lags the close animation, and an app-targeted hand event in
  that window is what triggers the QEMU panic below.
- **QEMU-only panic on app-targeted drags.** A press/move/release delivered
  to the frontmost app's `PurpleSystemEventPort` makes QEMU's ARM11 panic in
  the kernel copy path: `undefined kernel instruction`, pc 0x80067e44,
  r11 = 0x35ed5658 (the shared-cache address the app GSEvent points at), then
  `Debugger message: Fatal Exception`. The same events work on hardware
  (re-log 78). `emu/stress.py` keeps in-app drags behind `--app-drag` and
  does the continuous drags on SpringBoard; both harnesses now match
  `undefined kernel instruction`/`Fatal Exception` as a panic.
- **MCU daemon: SpringBoard restart left brightness/rotation dead.**
  ios3ds-mcud connected to SpringBoard's HID event system once. If
  SpringBoard restarted, the per-client port at `client + 8` became a dead
  name and every `DisplayBrightness`/accelerometer call failed silently until
  reboot. It now checks the port every 2 s, drops the dead client and
  reconnects (`[mcud] SpringBoard's HID event port went away; reconnecting`);
  the `NoIdleSleepAssertion` and its two CFStrings are created once per
  process instead of once per reconnect.
- **Host tests:** `make test` passes (63 Python tests plus pica, GL, Wi-Fi,
  GPS, `test_cpad` 206 checks and `test_n3ds` 35 checks).
- **QEMU soak, Old 3DS** (`--cycles 5 --icount 2 --settle 45`, fresh image,
  final tree and input daemon): **Result: PASS**, boot 100 s, 5 cycles, no
  recoveries, 0 panics / stalls / crashes / md0 errors, 0 GPU fallback or
  timeouts, last heartbeat `md0 4295 req 88771 KiB 0 err`.
- **QEMU soak, New 3DS model** (`--new3ds`, same command, fresh image, final
  tree): **Result: PASS**, boot 100 s, 5 cycles, no recoveries, 0 panics /
  stalls / crashes / md0 errors, 0 GPU fallback or timeouts, last heartbeat
  `md0 4342 req 90023 KiB 0 err`.
- **Only a console can settle:** the hardware checks are listed in
  [driver-status.md](driver-status.md) §"What only a console can settle",
  with the exact log lines/files to capture (headphone CODEC readbacks, mic
  FIFO overruns, camera frame dumps, gyro/`[gyrotest]`, the GPU
  present/scatter path, `[hb] ... md0 ... N err`, a SpringBoard restart's
  `[mcud] ... reconnecting`).

## 86. Leaving iOS: long POWER = clean shutdown, HFS+ clean flag, and fsck's EBUSY (2026-11-21)

- **Why:** holding POWER ~10 s used to be the only way out, and a killed QEMU left `rootfs.hfs` unclean. Every boot said `Running fsck on the boot volume...` then `GetMasterBlock: Error 16 opening /dev/md0` (EBUSY, also in `docs/hwlogs/run*.txt`); damage was never repaired.
- **HFS+ state, measured on the image** (volume header at byte 1024; big-endian attributes at +4; bit 8 = `kHFSVolumeUnmountedMask`): pristine build 0x80000100; after `BSD root: md0` 0x80000000; after a "slide to power off" 0x80000100; after `kill -9` on QEMU 0x80000000. So bit 8 *is* a reliable clean/dirty record, and the slider already unmounts cleanly (`launchd: System: About to call: reboot(RB_HALT).` → `[mcu] iOS shut down: log synced, powering off through the MCU`). The next boot of that same image reaches SpringBoard normally.
- **fsck:** the check runs *after* XNU has mounted md0 as root; launchd opens the same device for writing and gets EBUSY. A boot-args `ro` test (`cmdline=ro rd=md0 …` edited into `boot.cfg` on the image) still printed `GetMasterBlock: Error 16 opening /dev/md0`. Making fsck work would require changing Apple's mount/launchd sequence (initial read-only root, remount rw after the check); not attempted. Instead the ARM9 now reads the flag when it opens the image, logs `[blk] rootfs.hfs: (NOT) cleanly unmounted (attributes …)`, and never marks a dirty image clean at boot (that would hide damage). `scripts/hfs_clean.py` does the same check on the host (`--sd build/sd.img`; 4 unit tests in `tests/test_hfs_clean.py`).
- **Long POWER press:** the MCU's "held" IRQ already produced a 6 s synthetic POWER down (the slider sheet) and bumped `box->power_holds`. `ios3ds-input` now watches that count through `kern.ios3ds_mcu` and, on a new value, does `sync(); reboot(RB_HALT)` (0x08), the same call Apple's launchd makes after unmount. QEMU (`--icount`-free session, `p` held 4 s from the home screen): `[mcu] POWER held: iOS power-off slider`, `[input] POWER held on the 3DS (count 1): sync + clean shutdown`, `[mcu] iOS shut down: log synced, powering off through the MCU`; QEMU exits and the image's flag is 0x80000100. The 3DS's ~10 s hardware cut is unchanged (last resort when iOS is hung, then the flag stays clear and the next boot reports it).
- **Safety:** on `req_power` from `PEHaltRestart` (after iOS unmounted) the ARM9 sets bit 8 in the volume header if XNU has not; `f_sync()` makes it durable. In the QEMU runs XNU had already set it. The slider shutdown still works after the change.
- **Dirty-boot limits:** a `kill -9` after a settled boot leaves bit 8 clear; the next boot logs the warning above and reports the same EBUSY from Apple's fsck. A `kill -9` *during* boot-time writes is worse: that image logged `Bug: launchctl.c:3849 … lstat(de->d_name) != -1` and stalled before SpringBoard. Setting bit 8 by hand on it removed the lstat bug but it still stalled, and the unmodified firmware stalled the same way — so the damage is in the catalog, not in our code. Without a working `fsck_hfs` in the guest there is no safe repair; the ARM9 only reports the condition and never claims a dirty volume is clean.

## 87. Multi-version build plumbing and a 4.1 bring-up (2026-11-22)

Design and the full verified/guessed analysis: docs/multi-version.md. This entry
records the concrete steps and blockers of stages 2–3.

- **Version selector.** `IOS=4.2.1` (default) and `IOS=4.1` select a build from
  scripts/versions.py; `make fwprep IPSW=… IOS=4.1` stages firmware in
  build/fw-4.1/staging instead of the shared build/staging symlink. The 4.2.1
  path has no overrides, so its kernel/devtree/rootfs/boot.cfg are the same
  recipes as before (hashes compared in docs/multi-version.md §5.1).
- **Firmware source.** 4.1's IPSW came from Apple's own CDN link published by
  api.ipsw.me; its sha1 (97abde6207660bd876fd476275dd526d0dcf3d19) matches.
  All extracted Apple files live in build/fw-4.1/. The 3.1.3 (7E18) keys are
  public but current firmware lists no downloadable iPod2,1 IPSW, so 4.1 is
  the second version under test.
- **Kernel patches.** 8C148's fixed virtual addresses cannot be reused: 4.1's
  xnu-1504.55.33 kernelcache is 7,938,048 bytes (vs 8,273,920) but its entry
  point is the same 0x80067050. The 8B117 set was derived by fixed address,
  context search and function anchors; every original byte was verified. Six
  patches (gyro ×2, two CLCD wake/log fixes, clock voltage lookup, IORTC wait)
  changed too much and are skipped for now.
- **Device tree.** dt_audio.py had hard-coded gpio/vic phandles from the 8C148
  template; 8B117 assigns different ones. It now reads the phandles from the
  tree. The 8C148 generated device tree stayed byte-identical.
- **Rootfs.** File patches in the dyld cache were relocated by parsing the
  cache's image and symbol tables (the technique already used by
  tools/ios3ds-gl/genslots.py): 8C148 offset -> VA -> image symbol + delta ->
  8B117 symbol -> file offset, with original bytes checked. 18 of 24 patches
  relocated; the liblockdown lookup body, its table, the VirtualAudio routing
  table, CoreMotion capability mask and MusicLibrary signing check are open
  blockers. Cydia/TLSFix are disabled in the 4.1 overlay.
- **QEMU.** This worktree's build/qemu is a shared symlink without a built
  binary; emu/run.py and emu/viewer.py now accept IOS3DS_QEMU=/path/to/qemu-system-arm
  so a local copy can be used without touching that symlink.
- **Boot result (QEMU, 4.1 + full patch set):** XNU starts and drives the
  S5L8720 peripherals, then panics in `OSDictionary::setOptions()+0x44`
  (fault address 0x4) before launchd. With only the 19 CPU/timer/IRQ/UART
  patches it reaches `BSD root: md0`, loads the filesystem-compression kext
  and runs the display copy and heartbeats for 100 s; adding the relocated
  md0 strategy gets to `BSD root: md0` and then takes an undefined-instruction
  exception in the fake GPIO page (pc 0xc38b3e94). No 4.1 home screen yet;
  the full panic logs and the remaining blockers are in
  docs/multi-version.md §5.3.
- **Final soak:** four fresh-image 5-cycle soaks of the default build on this
  heavily shared host printed Result: FAIL only on "not home after Home"
  (black/stale screenshots, boot 435–965 s; heartbeats barely advancing);
  zero panics, md0 errors, crashes or hangs. The last run used the soak
  script's new lit-home-reference re-acquisition. No PASS could be obtained
  while other workers saturated the CPU. The 4.2.1
  kernel/devtree/fakeio/shim/gpu/boot.cfg hashes were unchanged.
- **Soak harness:** four more exact-command runs with fresh images (boot 425–554 s,
  host load 30–40) still printed FAIL only on "not home after Home": black
  references and mid-page-animation screenshots, zero panics/stalls/md0/crash/
  hang events. `emu/soak.py` now re-acquires a lit reference after a dark boot;
  a pre-tap Home recenter was tried and reverted because it made icon hits
  worse. Once the host quietened down, the exact required fresh-image command
  printed **Result: PASS** (5 cycles, boot 197 s, all eight failure checks at
  0); the earlier FAILs were host contention, see docs/multi-version.md §5.4.

## 88. Auto-brightness on New models: where the ambient light comes from, and a camera AE reading from the ARM9 (2026-11-21)

Design/checklist: docs/mcu.md §12, docs/n3ds.md §7. Status: **QEMU only; the
register-level source is unverified on hardware.**

- **Research, primary sources.** 3dbrew "I2C Registers", device 3's complete
  MCU register table has no lux/ambient register (0x0A–0x0F battery and flags,
  0x45–0x4A accelerometer, 0x4F/0x7F diagnostics), so the "read an MCU
  register" candidate is ruled out. 3dbrew "QTM Services" lists `qtm:s`
  command 0x04020000 `GetCameraLux`, and libctru's `qtm.h` documents
  `QTMS_GetCameraLuminance` as "average ambient luminance as perceived by the
  inner camera (in lux)", affected by auto-exposure and fixed at 400.0 on New
  2DS XL. Wikipedia's New Nintendo 3DS hardware section says the face-tracking
  sensor is also the ambient light sensor for automatic brightness; Nintendo's
  support pages say the setting is unavailable on New 2DS XL. 3dbrew "I2C
  Registers" device 16 is the New-3DS-only QTM device at bus 1 / 0x40 but has
  no documented luminance register. BlocksDS `nds/camera.h` (Aptina MT9V113,
  the 3DS inner camera already driven by `boot/arm9/camera.c`) defines
  `MCUREG_APT_AE_LAST_LUMA` (0x2218), the sensor's own measured luminance,
  reachable through the camera's variable-address registers 0x098C/0x0990 on
  I2C bus 1 / 0x7A. libn3ds `I2C_DEV_EXTHID` / GBATEK show the same ARM9 I2C
  pattern used by our C-stick driver. **Confirmed:** a camera-based ambient
  measurement exists on New 3DS and Horizon exposes it as lux. **Guess:** the
  AE luma byte is the value QTM turns into lux, and that this works the same
  way on a New 2DS XL. Nothing was measured on a console.
- **Path.** `camera_als_sample()` (`boot/arm9/camera.c`) brings the inner
  sensor up once (PLL + AE + sequencer refresh) and reads var 0x2218 at 2 Hz.
  It never touches *any* camera while a client streams (inner, outer left or
  outer right): `mcu.c` then sets `als_state=2` and publishes no new luma,
  mcud holds its last factor, and both resume when the client stops (the
  shared AE set-up is redone if Camera.app had the inner sensor). This follows
  3dbrew's note that QTM refuses head tracking while another process uses a
  camera. `mcu_set_als()` (`boot/arm9/mcu.c`) is armed by `main.c` **only**
  after `n3ds_setup()` says New; `auto_brightness=0` prevents even that call
  and `camera=0` disables the whole camera driver. The byte lands in line 5 of
  `mcubox_t` (which had 20 spare bytes; all existing line offsets and the
  256-byte size stay put), alongside `MCUV_ALS`. ios3ds-mcud reads SpringBoard's
  `SBEnableALS` preference (absent on an iPod2,1 → on), converts luma → lux →
  multiplier with hysteresis/slew (`boot/common/als_curve.h`) and multiplies
  `DisplayBrightness × DisplayBrightnessFactor` before sending the backlight
  through `kern.ios3ds_mcuctl`. The factor is kept across daemon loops: the
  shim is also written on a slider change, so a local factor would cause a
  brief jump to full brightness between two light samples. The shim and the
  PWM registers are unchanged.
- **Cost / LED.** The inner camera has to stay awake: a standby sensor has no
  fresh AE result. Only the two-register read is duty-cycled (2 Hz, no CAM
  FIFO or PICA work, no new ARM9 wakeups); re-running `apt_init()` every few
  seconds would write ~80 registers, wait for PLL/AE and yield stale luma.
  There is no documented standalone lux sensor (3dbrew's MCU table device 3
  and I2C device 16), so this is the only ARM9-readable source. 3dbrew lists
  an MCU "Camera LED state" register 0x2B, which we never write; whether the
  LED is tied to camera power needs a console. Hardware current draw is
  unmeasured (docs/mcu.md §12.2.1).
- **Curve tests** (`tests/test_als.c`, added to `make test`): 765 checks —
  monotonic luma→lux anchors, bounded monotonic multiplier, first-sample
  snap, slew limit, one-frame flicker ignored, sustained change followed, no
  oscillation on luma jitter. Host test of the ARM9 side
  (`tools/ios3ds-mcu/tests/test_mcu.c`) also checks that the mailbox stays
  empty unless the feature is enabled and the sensor answers, that a camera
  client pauses sampling (`als_state=2`, last luma held) and that the sample
  resumes afterwards.
- **QEMU** (`emu/qemu/hw/arm/{nintendo3ds.c,ctr_cam.c}`): `/machine
  ambient-light` (uint32 lux) feeds the inner camera's AE luma through the
  existing I2C register model, including after a sensor soft reset. The
  binary path can be overridden with `IOS3DS_QEMU` in run/viewer/soak so a
  private QEMU build can be tested without touching the shared `build/qemu`
  symlink (which other worktrees use). Runs of 2026-11-22 (fresh image,
  `--icount 2`, user brightness 0.50): on a New model with
  `auto_brightness=1`, `qom-set /machine ambient-light 5 / 800 / 80` gave
  `lcd-backlight` **52 / 78 / 65**; the SD log has
  `[mcu] auto-brightness on (New model; inner camera AE luminance)`,
  `[cam] inner camera awake for ambient light (AE luma var 2218)`, luma
  160 → 78 → 218 → 154/255 and `[mcud] auto-brightness: ... -> factor ...`
  slewing the backlight (no jumps). With `auto_brightness=0` in boot.cfg the
  same changes left `lcd-backlight` at **79** and logged `auto-brightness off`;
  on the Old model there was no `[cam] inner camera awake` or `[mcu] ambient
  light` line and the value stayed **79**. `make test`, `make userland` and
  `make sdimg` clean (the image was built `--no-cydia --no-tlsfix` to keep the
  shared Cydia cache untouched while several other workers were building).
- **Soak harness:** earlier runs on a loaded host (load average 10–15, other
  emulators and compilers) failed with "not home after Home" and lock rounds
  ending on the lock screen while the SD log showed 0 panics, 0 stalls, 0 md0
  errors: short synthetic key presses missed the ARM9 input polls. `emu/soak.py`
  now holds keys 0.4 s (below the input daemon's 0.8 s long-press threshold),
  waits up to 45 s for the screen to settle, retries Home before giving up,
  wakes with two Home presses and retries the slide-to-unlock gesture. Those
  are harness timing fixes, not changes to the pass criteria.
- **Soaks (2026-11-22, final): PASS, with a private QEMU build in
  `/tmp/opencode/qemu-als-src/b/qemu-system-arm` and `IOS3DS_QEMU` pointing at
  it (the shared `build/qemu` was left untouched).** Each run got its own
  fresh `build/sd.img` (`make sdimg CYDIA=0 TLSFIX=0`) and
  `python3 emu/soak.py --cycles 5 --icount 2`, the second with `--new3ds`;
  both printed `Result: PASS` (5 cycles, no "not home after Home", no
  lock-round failures, 0 panics / stalls / md0 / log-write / crash / hang
  failures). Earlier attempts on the overloaded host were killed during the
  image build (`make: *** [Makefile:79: sdimg] Killed`) and were recorded as
  unfinished in the interim; these final runs complete them.

## 89. Experimental stereoscopic 3D home screen (2026-11-22)
Design: docs/stereo3d.md. Status: experimental, off by default; the hardware
path described in section 90 is implemented but still needs a console test.

- **Mechanism:** CoreAnimation composites SpringBoard through our GL engine
  (`tools/ios3ds-gl`), which stamps a depth class into the composed frame's
  alpha byte with a second, alpha-only pass. The classes are wallpaper (0x33),
  icon grid (0x66), dock/status bar (0x99), folders/alerts (0xCC), flat
  (0x00). The ARM9 CPU screen copy reads that class map and builds the two
  eye images with a horizontal offset per class; the 3D slider scales the
  offset (0..4 px) and 0 turns the barrier off.
- **Gate:** `boot.cfg stereo3d=1` appends `ios3ds_stereo=1` to the kernel
  boot-args; only the CA render server (`IOS3DS_CA=1`) stamps. With the flag
  off `stereo_prepare()` returns -1 at once and the normal path is unchanged.
  Frames that are not the home screen are wiped flat (one full-screen quad),
  so apps and the lock screen give both eyes the same image.
- **Files:** `boot/common/stereo.h` (shared classes, offsets, classifier),
  `tools/ios3ds-gl/stereo.c`, `tools/ios3ds-gl/{api,ca,draw,iface,pica_be}.c`,
  `boot/arm9/main.c` (flag + boot-arg + gpu_mirror off for stereo),
  `emu/qemu/hw/arm/nintendo3ds.c` (slider QOM property + anaglyph top screen),
  `tests/test_stereo.py`.
- **Tests:** `make test` passes (host tests for the depth/offset logic);
  soak with the flag off and on both PASS.

## 90. Stereoscopic 3D on real hardware: two eyes and the parallax barrier (2026-11-22)
Review of section 89 found that `stereo3d=1` only made the display slower on
an actual console: the ARM9 copied one image to both top-screen framebuffers,
the top LCD stayed in 2D mode and nothing ever enabled the parallax barrier.
This section closes that gap. The verdict on a console is still pending; the
paths below are exercised in QEMU.

- **ARM9 copy** (`boot/arm9/display.c`): `display_set_stereo()` receives the
  boot flag and the MCU slider every monitor loop. With the flag on, a slider
  above 16 and a stamped home frame (32x32 alpha grid, at least two distinct
  non-flat classes), `blit_top_stereo()` reads one source row per screen
  column, samples the left/right pixels at `x - stereo_eye_delta(cls,k,right)`
  and writes BGR8 (alpha masked) to `top_fb[0]`/`top_fb[1]`. A flat frame, a
  slider below the threshold, `SCREEN_BOTTOM` or no second framebuffer uses
  the old `blit_top()` byte for byte; the bottom copy and touch map are
  shared unchanged. The barrier is enabled after the copy and only while a
  stereo frame was actually produced; any transition back clears it.
- **Mailbox and shim**: `gmbox_t` line 5 gained `stereo_on`, `stereo_seq`,
  `stereo_ack_seq` (still 256 bytes, same cache-line ownership). The ARM9's
  `gm_set_stereo()` publishes a request and waits at most 200000 polls for the
  ack. `kernel/shim/pica_stereo.c` (new GPU-part object, added to mkboot's
  source list) checks the request in thread context via `shim_gpu_thread_poll()`,
  saves Luma's 2D PDC0 preset on the first switch and writes the 3D preset.
  Per 3dbrew GPU/External Registers: PDC0 `+0x68/+0x6C` = left-eye A0/A1,
  `+0x94/+0x98` = right-eye B0/B1, `+0x70` bits 5-4 = 2 (AB interlace), bit
  6 clear. Per 3dbrew LCD Registers: barrier enable `0x10202000` =
  `0x00010001`, PWM `0x10202004` = `0x0A390A39` (libn3ds value); the ARM9
  writes these after the ack and clears the enable on the way back to 2D.
  The GPU part is 31744 of 32768 bytes (1024 bytes headroom).
- **QEMU** (`emu/qemu/hw/arm/nintendo3ds.c`): the old shortcut (read iOS's
  alpha from VRAM and draw an anaglyph) is gone. The model now has PDC0's
  B0/B1 framebuffer registers, the PDC0 format bits and the barrier register
  at `0x10202000` (aliased into the ARM9 address space), with QOM properties
  `pdc-3d-mode` and `lcd-3d-barrier`. The anaglyph is composited from whatever
  framebuffers those registers point at: left eye in red, right eye in cyan.
  No 3D mode or barrier, no anaglyph: the ordinary top-screen copy runs.
  `emu/run.py`/`emu/soak.py`/`emu/viewer.py` accept `IOS3DS_QEMU` (and soak
  has `--qemu`, `--slider3d`) so this worktree builds and uses its own
  private QEMU (`build/qemu-hy4`, binary copied to `build/qemu-local/b/`),
  never the shared `build/qemu` symlink.
- **Tests**: `tools/ios3ds-pica/tests/test_stereo_copy.c` (new) maps the CLCD,
  LCD and framebuffer pages at their physical addresses and runs the real
  `boot/arm9/display.c`: flag off is byte-for-byte the old copy with no
  barrier or PDC request; the stamped frame gives class-dependent offsets
  (front plane +/-4 px at slider 255, icon plane flat), opaque output and a
  barrier request; slider down and unstamped frames restore the flat copy.
  `test_mirror.c` gained the stubs for the new dependencies. `make test`
  passes; fresh-image soaks with `stereo3d=0` and `stereo3d=1` both PASS.
- **Frame time**: in QEMU under heavy host load, the flag-off run logged
  `[monitor] CPU screen copy: 31 copies in 30 s, 15.4 ms each (ARM9)`; with
  `stereo3d=1` and the slider up it logged `42 copies in 30 s, 120.3 ms each`
  and, in a longer run, `33 copies in 38 s, 186.4 ms each`. The two-eye copy
  costs roughly 8-12x the single-image copy in the emulator because it packs
  both eye columns and writes both LCD buffers; on hardware this replaces the
  GPU mirror (which is off in this mode), so the real console cost still has
  to be measured. The GL side logs `CA stereo3D: N frames, ... stamped/wiped`
  and its usual per-frame line with `diag=1`; no stamp pass runs with the flag
  off.

**Soak status on the shared, heavily loaded host:** the flag-on soak
`--cycles 5 --icount 2 --slider3d 255` passed (`Result: PASS`, 5 cycles, no
panic/IO/watchdog hits). Repeating both flag-off and flag-on runs on fresh
images produced clean crash/IO checks but the soak's screenshot-based "home
restored" check was flaky while the host ran 20-50 concurrent builds; one
flag-off run did not reach SpringBoard within its 1200 s boot timeout. The
soak input dwell/timeouts were increased (commits above) to cope with that
host load; no code path other than the test harness was changed for those
retries. A final confirmation run on an idle machine is still desirable.

## 91. HTC TX credits: the "Mbox Full" back-pressure (2026-11-21)

**Symptom:** dsiwifi calls it "Mbox Full"; with sustained traffic the AR6014
can be asked to send more mailbox messages than it has buffers for. The host
driver sent every WMI/data message as soon as iOS handed it a frame and never
read the credit reports the target puts in RX trailers (ath6kl
`htc_mbox.c`, `htc.h`: record 1 is `endpoint, credits`; `NEED_CREDIT_UPDATE`
asks for a report when nothing else is sent).

**What the target announces.** `HTC_READY` carries `cred_cnt, cred_sz` (the
model: 8 x 1664 bytes). A message costs `ceil(padded_len / cred_sz)` credits,
at least one; the padded length is what goes over SDIO (our block size 128).
The pool is split the way ath6kl does it: one credit for EP0, one for each
connected service, the rest for the first (BE) data endpoint. The firmware
returns a credit for every message it consumes; a host that ignores the
reports runs the pool to zero.

**Driver** (`boot/arm9/wifi/ar6k.c`, `sdio.h`, `wifi_frame.c`):
- `credit_init()`/`credit_assign()` distribute the pool; `mbox_send()` takes
  `ceil()` credits before writing, sets `NEED_CREDIT_UPDATE` when an endpoint
  falls below one maximum-sized message, and refuses data sends with the new
  `SDIO_ERR_BUSY` (-5) instead of writing. `wifi.c` maps that to its "busy"
  return, so the frame stays in the ARM9<->iOS TX ring for the next pass.
  WMI/control sends wait for a report (bounded), rather than dropping a
  command.
- A credit wait must not re-enter the WMI/HTC state machine, so whole
  packets that arrive while waiting are copied into a four-slot deferred
  queue and run from the next `pump()` pass. The trailer parser itself moved
  to `wifi_frame.c` (`wifi_htc_credits()`) so it can be tested on the host;
  it ignores out-of-range endpoints and stops at a torn record.
- `[wifi-diag]` logs every endpoint assignment and the first 16 reports,
  plus a credit total when the link comes up.

**QEMU model** (`emu/qemu/hw/arm/ctr_wifi.c`): it now *has* credit accounting,
not just trailers. Host messages are dropped with `Mbox Full`, TX-overflow
set and an `htc-overflows` counter when the endpoint is out of credits;
credits are owed back for every accepted message and travel in the next RX
trailer or an empty EP0 report one virtual ms after `NEED_CREDIT_UPDATE`.
QOM (HTC credits used/reported/waits/overflows, frame counts) makes the
evidence readable without a debugger. This is a real back-pressure test: a
host that ignores credits fails in QEMU, where the old model let it through.

**Host tests:** `make -C tools/ios3ds-wifi test` covers cumulative reports,
unknown records, torn trailers, saturation and out-of-range endpoints.

**QEMU evidence (2026-11-22, this worktree's QEMU):**
`emu/wifi_test.py --case credit` PASS: the 4 MiB body was served with
`[nettest] bulk 4194461 bytes (4194304 body, 0 bad) in 255 s: PASS`, the
driver logged `[wifi-diag] ... htc: credit report: +1 ...` and the model
`ctr-wifi: ... HTC credits: consumed 1216, returned 1216, 218 wait(s),
0 overflow(s)`. No `Mbox Full`, TX overflow or lost byte was seen. Early
runs on a loaded host timed out on an unrelated route/DNS check; the final
run with a 900 s guest-time budget passed every check. `emu/run.py` and
`emu/soak.py` now both accept `IOS3DS_QEMU` so a branch with its own QEMU
build is tested with that binary. A fresh SD image with `wifi.cfg` and the
dummy firmware, soaked with `emu/soak.py --cycles 5 --icount 2 --settle 20`,
printed **Result: PASS** (boot to SpringBoard 441 s, 5 cycles, no panics,
stalls, md0 errors or crashes, no home/restore failures).

## 92. WPA2 with a TKIP group cipher, and WPA1 (TKIP pairwise) (2026-11-22)

**Firmware first.** GBATEK "WMI Connect Functions" lists the AR6014's
WMI_CONNECT values: auth 3 = WPA-PSK, 5 = WPA2-PSK, crypto 3 = TKIP, 4 = AES;
"WMI Channel and Cipher Functions" says the host installs WPA/WPA2 keys with
WMI_ADD_CIPHER_KEY after the 4-way handshake. Our key installer already put
32-byte TKIP keys into that command with the MIC halves swapped (dsiwifi's
order), so the firmware does the TKIP/Michael frame crypto itself. No software
TKIP or Michael was added: the supplicant only derives/installs the keys.

**TKIP group with WPA2 pairwise CCMP** was already handled in `wpa2.c` and the
QEMU AP: the RSN IE's group suite becomes 00-0F-AC-02, the GTK is 32 bytes and
the firmware gets a TKIP group key. The work here is a real end-to-end check
(`emu/wifi_test.py --case tkip`) and keeping it working while WPA v1 landed.

**WPA v1 differences** (IEEE 802.11i/D10 and hostapd's `wpa_auth.c`,
`wpa_common.c`, `rsn_supp/wpa.c`): a WPA vendor IE (`DD 18 00:50:F2:01`)
instead of an RSN IE; EAPOL-Key descriptor 0xFE, Key Info version 0;
HMAC-MD5 MIC over the frame with the MIC field zeroed; a 64-byte PTK
(KCK 16, KEK 16, TKIP TK 32) from the same PRF-SHA1; Key Data RC4 with the
key `Key IV[16] || KEK[16]` and the first 256 stream bytes dropped; a 32-byte
TKIP GTK in the same 00:0F:AC:01 KDE; M2 echoes M1's Key Length and the WPA
IE; M4 does not. Message 3 carries the GTK key index in the bits that RSN
calls Install, so WPA v1 must not require the Install bit.

`boot/arm9/wifi/wpa2.c` gained MD5, HMAC-MD5, RC4-drop-256 and a 64-byte PTK,
plus a WPA-IE parser/builder; `ar6k.c` detects the vendor IE in BSSINFO and in
the iOS scan table, sends WMI_CONNECT with auth 3 / TKIP, uses the exact WPA
IE from the firmware's association request in M2 and installs the 32-byte TK.
The QEMU AP gained `security=wpa1` with the same authenticator rules; WPA2
paths are unchanged. WEP is documented as unsupported: a WMI WEP key type
exists, but open/shared-key association and WEP key management were not added.

**Host tests:** RFC 1321 MD5 and RFC 2202 HMAC-MD5 vectors, the well-known RC4
vectors and an independently computed RC4-drop-256 vector, a 64-byte PTK check,
WPA-IE parse/reject cases, and a full WPA1 4-way + group rekey against a small
authenticator in `tools/ios3ds-wifi/tests/test_host.c` (200 checks, 0 failed).
QEMU evidence (2026-11-22): `--case tkip` PASS — `[wifi] connect
"ios3ds-qemu" (WPA2-PSK/CCMP, TKIP group)` and `WMI_ADD_CIPHER_KEY group ...
equals the AP's GTK`. The first `--case wpa1` run exposed two bugs:
`target_lookup()` used its `RSN_MODE_WPA2` default as if an RSN IE had been
seen, clearing WPA1 selection (QEMU: WMI auth 5/4/4, status 43), and the
QEMU AP compared the pairwise TKIP key in raw order while both dsiwifi and
our installer swap the MIC halves. With `saw_rsn` and the model's swapped
comparison, `--case wpa1` PASS: `[wifi] connect ... WPA-PSK/TKIP`,
`pairwise ... equals the AP's TK`, `4-way: message 4 OK` and `[nettest] all
steps passed`. Host tests: 200 checks, 0 failed.

## 93. Wi-Fi LED follows the link (2026-11-22)

The blue Wi-Fi LED is MCU register 0x2A (4 bits, any non-zero value turns it
on; 3dbrew "I2C Registers", device 3). `boot/arm9/mcu.c` caches the value and
writes 1 while `netmbox_t.link` is up, 0 on link-down and before a power-off.
The monitor loop samples the link once per pass; only a transition costs one
I2C byte. `boot.cfg wifi_led=0` disables the feature (default on), next to
`notify_led` for the separate RGB notification LED.

The QEMU MCU now models reg 0x2A and counts writes: `/machine mcu-wifi-led`
and `mcu-wifi-led-writes` are read-only QOM properties. Evidence (2026-11-22,
`emu/wifi_test.py --case ok`, HMP `qom-get` at 180 guest seconds): default
setting gave `mcu-wifi-led = 1`, `mcu-wifi-led-writes = 2` (initial 0 and one
write of 1 at `[wifi] link up`); with `wifi_led=0` in `boot.cfg` both were 0
and there was no `[mcu] Wi-Fi LED on` line (docs/mcu.md §14).

## 94. Wi-Fi driver: end-of-task verification (2026-11-22)

`make test`: all suites pass, including 200 Wi-Fi host checks and the 8
`nwmfw` tests. `emu/wifi_test.py` with this worktree's QEMU: `ok`, `credit`,
`tkip` and `wpa1` PASS (sections 85-87). Each run used a freshly built SD
image; the Wi-Fi cases use a port outside the default 8080 so other workers
on this PC are not disturbed.

Fresh-image soak: `python3 emu/soak.py --cycles 5 --icount 2` printed
**Result: PASS** (boot to SpringBoard + input 370 s, 5 cycles, no kernel
panic, ARM11 stall, md0 I/O error, process crash or watchdog/hang). A
second fresh-image soak with `wifi.cfg` and dummy firmware (Wi-Fi link up
during the run) and `--settle 20` also printed **Result: PASS** (section 85).
The soak harness now retries black screendumps and compares home screens after
backlight normalisation: on a loaded host a raw comparison marked a lit
home screen as "not home" (sections 85-87). Repeating that soak with this
branch's own QEMU binary produced only the same home-screen heuristic
false positives (1-3 cycles per run needed recovery, while every crash and
I/O check stayed at 0); the Wi-Fi cases above all used that branch QEMU.
`IOS3DS_QEMU` now selects the binary for both `emu/run.py` and `emu/soak.py`.

## 95. NPOT textures: test the workaround, not the hardware quirk (2026-11-23)
Hardware log (New 2DS XL, build 149; `docs/hwlogs/n2dsxl-run1.txt` and the
user's report) says `[gpu-diag] summary: … texture POT ok, NPOT FAIL`. That is
run 13's confirmed rule, not a new fault: the PICA200 samples a texture whose
size registers are not powers of two as noise. The GL engine already works
around it (`pica_be.c use_texture()` pads the image to a power of two and
scales the texture matrix), and the display mirror has used a 512x512 texture
since run 13, so no rendering path sends an NPOT size to the GPU.

The diagnostic itself was still sending one. It drew a raw 48x40 texture and
so could only ever fail on hardware, which made the summary useless for
checking the workaround. Now:
- `genmirror.c` diag list 2 uses a 64x64 texture, with the gl_quad texcoords
  scaled by 48/64 and 40/64 (at a pixel centre, floor(t·64) is the used
  image's texel);
- `pica_diag.c` writes the 48x40 image into the used corner of the padded
  texture (zeros elsewhere) and compares the same 1920 texels;
- `test_mirror`'s expected diagnostic mask is now 0x7d (`PD_TEX_NPOT` set;
  only the FCRAM fill bit remains FAIL as the hardware rule), and picasim
  still produces noise for raw NPOT sizes.

`make test`: 6678 pica checks and 39 mirror checks, 0 failures; diagnostic
summary `0x7d` on the model; `gltest: PASS` on both back ends (scene 2
already includes a 48x24 CLAMP texture, compared pixel by pixel).

Docs: [pica200.md](pica200.md) §12/§13. On a console the NPOT line should
now say ok; if it doesn't, the fault is in padding/texcoords/tiling, not in
the hardware's NPOT rule.



## 96. FCRAM fills: route PICA_OP_FILL around the PSC engine (2026-11-23)
The hardware summary `fill VRAM ok->ok, fill FCRAM FAIL->FAIL` describes a
PSC limit, not a fault in our setup (run 13, docs/pica200.md §13): a fill of
FCRAM sets the done bit and its counter, but writes nothing. Audit of every
call site:
- GL clears, depth init and the GPU self-check have used P3D quads or CPU
  writes since run 13 (`pica_be.c`) — no PSC fill on the GL hot path;
- the display mirror's two fills are in VRAM (`pica_mirror.c`), which work;
- the raw PSC fill is only in `[gpu-diag]`, to report the hardware rule;
- `PICA_OP_FILL` (`kernel/shim/pica.c`) + `pica_drv_fill()` were the only way
  a client could still ask for a fill of the FCRAM arena, and would silently
  return success with the old contents.

`pica_drv_fill()` now writes the constant into the mapped arena with the CPU
(16/24/32-bit patterns, two slots, cache cleaned with `to_gpu`). A constant
is the same in linear and tiled memory, so no GPU operation is needed and
the caller gets what a working fill would have produced. The raw PSC quirk
stays in the model and in `[gpu-diag]`.

`make test`: 6691 pica checks + 39 mirror checks, 0 failures; the new
`test_pica` checks both slots, 16/24/32-bit patterns and a rejected range;
the diagnostic summary stays 0x7d (FCRAM fill still FAIL by design).

## 97. ES 1.1 compatibility for iOS 3/4 games (2026-11-23)
Games from that era probe for a handful of ES 1.1 extensions before they
load assets. Audit against `tools/ios3ds-gl`:
- `OES_draw_texture`, `OES_point_sprite`/`OES_point_size_array`,
  `OES_framebuffer_object`, `OES_mapbuffer`, separate blend func/equation and
  `APPLE_fence` were already implemented (gpu.md §3).
- **PVRTC** was the one texture format games actually ship that we rejected.
  `pvrtc.c` now decodes all four `IMG_texture_compression_pvrtc` formats
  (RGB/RGBA, 2/4 bpp) to the engine's RGBA texels in
  `api_CompressedTexImage2D`; 4bpp endpoints, 2-bit modulation and punch-
  through alpha follow the documented 64-bit block layout. 2bpp uses the
  neighbouring blocks' endpoint average as its low-frequency signal (an
  approximation, not the PowerVR reference filter). The extension string and
  `GL_COMPRESSED_TEXTURE_FORMATS` now list the four formats.
- **`OES_matrix_palette`**: four new entry points fill per-context palette
  matrices and index/weight arrays; `draw.c` blends 1..4 matrices per vertex
  in object space (in addition to modelview) for positions and normals.
- **`glGenerateMipmapOES`** now builds a box-filtered chain (level 0 stays the
  sampled level, as documented).
- **ES 2.0**: `EAGLSharegroup initWithAPI:` only uses our engine for api 1,
  so an ES2 context request is refused. Shader/program/vertex-attribute
  entry points reached through an ES1 context now set
  `GL_INVALID_OPERATION` instead of the dispatch stub's silent 0.

Tests: `make -C tools/ios3ds-gl host-test` passes a standalone PVRTC unit
test (hand-encoded blocks) and, on both back ends, the new gltest checks for
PVRTC upload, extension lists, palette-driven vertex motion, mipmap
generation and the ES2 error. `make userland` cross-builds the engine and
iOS gltest. `ios3ds-gltest` on iOS additionally probes EAGL with api 2 and
checks that the ES1 context remains current.

Docs: gpu.md §3/§5, README's OpenGL ES limitation.

## 98. MBX2D: which framework prints the messages (2026-11-23)
Every hardware log, including the New 2DS XL build 149 report, has:
`Iterator had no matching services`, `Failed to open connection to MBX`,
`Failed to initialized MBX2D driver (1).` — right after the first EAGL
context is created. No Apple file is kept in the tree, so the search was run
against the dyld cache in `build/staging/rootfs.hfs` (a build product made
from the user's own IPSW):

```
build/tools/hfsplus build/staging/rootfs.hfs cat \
  /System/Library/Caches/com.apple.dyld/dyld_shared_cache_armv6 > build/dyld-cache.tmp
# header: mappingOffset 0x48, mappingCount 3, imagesOffset 0xa8, 362 images,
# dyld_cache_image_info = {uint64 address, modTime, inode; uint32 pathOffset, pad}
# VA = file offset + 0x30000000 (0x4ff9458 -> OpenGLES VA 0x34ff9458)
```

- offset 0x770c68 (VA 0x30770c68) is inside `MBX2D.framework` (base
  0x3076a000): its `AppleMBXDevice` string and, at 0x7707d0+, `Iterator had
  no matching services`, `Failed to open connection to MBX`, `Failed to find
  an AppleMBXDevice service, error 0x%X`, `Failed to open the shared user
  client...`. That is the same service lookup OpenGLES patched in gpu.md §1;
  MBX2D's copy was deliberately left alone because GL no longer needed it.
- offset 0x47bb418 / 0x47bb470 (VA 0x347bb418 / 0x347bb470) is inside
  `QuartzCore.framework` (base 0x346dc000), next to `CA_ENABLE_MBX2D`,
  `LK_ENABLE_MBX2D`, `CA_ENABLE_OGL`, `MBX2D_PAGE_FLIP` and the
  `Failed to initialized MBX2D driver (%d).` format. QuartzCore's
  `CA::RenderMBX2D` class and its `blit_*` methods are in the same image:
  this is CoreAnimation's 2D blit renderer (layer images / video frames),
  distinct from the GL compositor. MBX2D's own strings mention YUV420
  texturing and `mbx3DQuadCopy*`, so CoreVideo/IOSurface frames are the other
  likely consumer.

The failure is benign in SpringBoard but not necessarily for a game that
reaches the MBX2D path. SpringBoard's environment now sets
`CA_ENABLE_MBX2D=0` and `LK_ENABLE_MBX2D=0` in `rootfs/recipe.json`; GL
compositing and `MBX2D_PAGE_FLIP=0` are unchanged. A QEMU boot with those
variables set still printed both lines (a test with `CA_ENABLE_OGL=0` also
left our `IOS3DS_CA` GL compositor in charge), so this 8C148 QuartzCore does
not skip the probe from those variables. The two format strings are
therefore patched to empty in the dyld cache: `0x7707cc` (32 bytes +
newline/NUL, MBX2D's `Failed to open connection to MBX`) and `0x47bb470`
(40 bytes + newline/NUL, QuartzCore's `Failed to initialized MBX2D driver
(%d).`). The callee still fails and QuartzCore still falls back; only the
console noise is gone. `tests/test_mbx2d.py` guards the recipe and the two
patches. Docs: gpu.md §8, architecture.md §SpringBoard.

## 99. Public issue #1 (New 2DS XL): brightness floor, "180° rotation", boot delay, wallpaper (2026-11-23)
Four small reports, one commit each. No GitHub reply was posted; the answers
are in the README FAQ and in mcu.md / m1-design.md.

- **Brightness 0 = black.** `ios3ds-mcud` used to send `DisplayBrightness ×
  DisplayBrightnessFactor` unchanged and the shim mapped level 0 to PWM 0.
  Factor 0 is iOS' *screen off* (lock / sleep) and is also the ARM9's
  `backlight == 1` (screen off) sentinel in `mcu_poll()`'s shell handling, so
  it must stay 0. Level 0 with the screen *on* is now clamped to 0.12 in the
  daemon and every lit level maps to at least `BL_PWM_MIN = 0x26` (libn3ds /
  Luma's darkest readable preset) instead of 0x10. Both limits are in
  `boot/common/backlight.h`; `tests/test_backlight.py` checks the whole range
  and the off sentinel. QEMU still logs the normal path
  `[mcud] iOS brightness 0.50 x factor 1.00 -> LCD backlight 500/1000` and
  `LCD backlight as Luma left it: top 0x5f, bottom 0x5f`. Real iPods keep the
  backlight lit at slider 0; a hardware check of the new minimum is welcome.
- **"No 180° rotation."** Not a missing sensor event. `build/emu-rot2` (Safari
  open, `qom-set /machine mcu-orient` 0/1/2/3, 40 s apart) logged
  `0 -> 1`, `1 -> 4`, `4 -> 2`, `2 -> 3`: pose 2 is MCU x = −1 g, which
  becomes iOS y = +1 g and `UIDeviceOrientationPortraitUpsideDown = 2`.
  `build/emu-rot5` repeats `0 -> 1` -> `1 -> 2` with the final shim. The axis
  map now lives in `boot/common/accel_axes.h`; `tests/test_orientation.py`
  checks all six QEMU poses and that the shim uses the shared map. Stock iOS
  4 on an iPod touch never rotates SpringBoard's home screen, and each app
  picks the orientations it draws (Safari's bookmarks view stays upright for
  2 and 4, rotates for 3 in the screenshots): no code change to the behaviour
  was made.
- **Wait before XNU.** The only ARM9 countdown is the 1 s grace period at the
  end of `a9_main()` (generated `boot.cfg` used `countdown_ms=1000`; there is
  no hardware reason for a longer one). It exists so **B** can stop the jump
  and leave the monitor running. Boot.cfg parsing moved to `boot/arm9/bootcfg.c`
  so it can be host-tested (`tests/test_bootcfg.py`): `boot_delay=N` is the
  user-facing alias of `countdown_ms`, `boot_delay=0` skips the wait, and the
  old key still works. **A** during the countdown jumps at once. QEMU: default
  `New 3DS check 11.625 s -> countdown over 11.880 s -> XNU 11.958 s`;
  `boot_delay=0` logs `boot.cfg boot_delay=0: no countdown` and
  `8.957 s -> 8.980 s -> 9.043 s` (guest timestamps differ between runs);
  with `boot_delay=60000` and A pressed at 8 s, `A pressed: skipping the rest
  of the countdown`, `8.163 s -> XNU 8.215 s` instead of a 60 s wait.
- **Wallpaper only on the lock screen.** SpringBoard's capability key is
  `homescreen-wallpaper`: the string is in the n72 SpringBoard binary (file
  offset 0x11fa88, CFString pointer at 0x15c794, object at VM 0x15d790) and
  in the dyld cache (0x54ca560, 0x57c73f8, 0x5f30d24). The iPod2,1's
  `/System/Library/CoreServices/SpringBoard.app/N72AP.plist` `capabilities`
  dict has no such key (it lists gamekit, nike-ipod, stand-alone-contacts,
  wifi, opengles-1, accelerometer, microphone, volume-buttons, …), so Apple
  disabled home-screen wallpaper on this model. Lock-screen wallpaper works
  after the DeviceClass fix (§62). `mkrootfs.py` already has a `plist_merge`
  recipe hook that could enable the key, but no memory or frame-time numbers
  exist for a full-screen wallpaper layer in SpringBoard on the 128 MB
  device, so no opt-in switch was added.
- **Verification:** `make test` passes (including the three new host tests);
  `make userland` + `rm -f build/sd.img && make sdimg` + `python3 emu/soak.py
  --cycles 5 --icount 2` on a fresh image: **Result: PASS** (5 cycles, no
  panics, stalls, crashes or md0 errors).

## 100. App Store tap and power-off slider: input port lifetime, crash reports from a card (2026-11-23)

- **Report** (New 2DS XL, build 149; `docs/hwlogs/n2dsxl-run1.txt` lines 816-846):
  `down 220,262`, `touch target com.apple.AppStore`, a Wi-Fi scan timeout, 21
  lines of `AMFI: Invalid signature but permitting execution` (the app launch),
  then `[input] connected to SpringBoard's HID event system`. There is no
  `panic(cpu`, no `[probe]` stall and — importantly — no
  `[input] SpringBoard went away; restarting` line. The last line is printed
  by `ios3ds-input` when *it* starts (`main()` in `input.c`, launchd `KeepAlive`); if
  SpringBoard's HID port had died, the daemon would first print "went away".
  So the reconnect alone does not prove a SpringBoard crash: the input daemon
  died and was restarted (the finger-up event is missing from the log too).
  The same signature appears in older hardware logs without an App Store tap:
  run19 line 527 (right after `down 49,308`), run23 line 699 (camera start),
  run28 line 761 (during gyrotest) — always a second `connected` with no
  `went away`. CrashReporter reports on the user's card would name the process;
  the new startup line includes the pid so two boots are unmistakable.
- **Making that checkable:** `scripts/crashlogs.py IMAGE OUTDIR` reads a raw SD
  image (FAT at 1 MiB, like `emu/session.py`), a mounted card directory or a
  `rootfs.hfs` and copies every `*.plist`/`*.crash`/`*.panic` under
  `/private/var/mobile/Library/Logs/CrashReporter` (plus the other standard
  CrashReporter locations) into OUTDIR. It is shipped with releases and
  documented in README's Troubleshooting and docs/emulator.md; `emu/soak.py
  --crashlogs` runs it after a QEMU session. Only reads: for a raw image it
  makes a temporary HFS copy under `build/`. Host tests cover the HFS+ header,
  `ls` parsing, recursion and extraction from a real small HFS+ image.
- **Fix:** the app touch path in `tools/ios3ds-input/daemon/input.c` cached the
  frontmost app's `PurpleSystemEventPort` send right without ever releasing it
  (`GSCopyPurpleNamedPort` returns +1, so one port name leaked per app switch),
  reused a dead name when an app was relaunched under the same bundle id, and
  trusted `SBFrontmostApplicationDisplayIdentifier`, which keeps returning the
  app that just went away. With a finite 4.2.1 port-name table a misrouted hand
  event can kill the receiving process; app launch and the power-off slider
  (modal touch while an app is frontmost) both take that path. The daemon now
  releases the old right (`app_port_set`), checks `MACH_PORT_TYPE_DEAD_NAME`
  before reuse, prefers `SBSCopyFrontmostApplicationDisplayIdentifier`, and
  routes touches to SpringBoard for 1.5 s after a HOME press, while
  SpringBoard's frontmost state lags the close animation. A diagnostic
  `[input] stale touch port ...` / `[input] no touch port ...` line names a
  repeat of the original failure in a future log.
- **Restart detection:** `emu/soak.py` used to print "SpringBoard restart" for
  the first `[input] connected` line of every boot (re-log 63). It now fails
  the run only when a *second* one appears, and prints the count in the report.
- **QEMU app survey:** three 19-icon runs with the final input daemon on fresh
  images (`emu/soak.py --unlock-first --no-extras --crashlogs --icount 2`,
  `--targets` listing all 15 icons on page 1 plus the 4 dock slots; 12 s per
  app, Home, wait for a settled screen). Every icon was tapped in every run;
  no run logged a panic, process crash, watchdog, md0 error or *second*
  `[input] connected` line. `scripts/crashlogs.py` found no CrashReporter file
  in any of the three images — nothing crashed in QEMU, so the hardware
  reports are still needed to name the process there. The App Store opened in
  run 2 (`build/emu/apps-survey/c011_app.png`: the gray "Loading…" screen)
  and Home brought the icons back; that is the expected "store servers are
  gone" outcome. Screenshots/reports: `build/emu/soak/` (run 1),
  `build/emu/apps-survey/` (run 2), `build/emu/apps-survey3/` (run 3).
  Two runs had a harness-only issue: when a tap landed while SpringBoard was
  still settling, the following Home opened Spotlight on page 1, so the next
  few screenshots show the keyboard instead of an app. No process died; the
  runs were not used as evidence for the affected apps.

  | App (icon) | QEMU result | Notes |
  |---|---|---|
  | Game Center | opened | run 2, app/home diff 29/0 |
  | Calendar | opened | run 1, 93/0 |
  | Photos | opened | run 1, 95/4 |
  | Contacts | opened | run 1, 96/0 |
  | YouTube | opened | run 1, 95/2 |
  | Stocks | opened | run 1, 45/2 |
  | Maps | opened | run 1, 65/0; Spotlight on the next tap in runs 2/3 |
  | Weather | opened | run 1, 41/0; Spotlight on the next tap in runs 2/3 |
  | Notes | opened | run 1, 83/2 |
  | Utilities (folder) | opened | run 1, 23/8 (folder close still animating) |
  | iTunes Store | opened | run 2, 76/0 |
  | App Store | opened, "Loading…" | run 2, 61/3; store requests fail, no restart |
  | Settings | tapped; no crash | screenshot ambiguous in all three runs (Spotlight/home); needs a console check |
  | Camera | tapped; no crash | screenshot ambiguous in all three runs (Spotlight/home) |
  | Cydia | tapped; no crash | screenshot ambiguous in all three runs (Spotlight/home) |
  | Music (dock) | opened | run 1, 95/2 |
  | Mail (dock) | opened | run 1, 94/3 |
  | Safari (dock) | opened | run 1, 94/2 |
  | Videos (dock) | opened | run 2, 26/3; run 1 screenshot was still gray |

- **Power-off slider:** `docs/re-log.md` §81 fixed the touch routing; this
  change covers the restart report. On the final image, holding POWER brought
  up the slider and tapping **Cancel** dismissed it, then the same sequence
  ran a second time: `build/emu-session/{pre_slider,slider_cancel,slider2,
  slider2_cancel}.png` and `build/emu-slider/` (`emu/slider_test.py --icount 2`,
  two show/cancel rounds, `Result: PASS`, 0 panics, 0 `connected` lines beyond
  the boot one). A later automated rerun timed out during boot because the
  host was loaded by other emulators; it logged no crash.

## 101. iOS 4.1: rootfs recipe and shim KPI relocation, SpringBoard in QEMU (2026-11-23)

Continues §87 / docs/multi-version.md §5.3–5.5. All Apple files were fetched
again into `build/fw-4.1/` via the API/CDN URL (sha1 verified by fwprep);
nothing Apple-derived was committed.

- **Recipe blocker.** The 4.1 dyld-cache patch list was positional. Two MBX2D
  patches were added to `rootfs/recipe.json` on main afterwards, so the 24
  relocated offsets were zipped onto 26 patches and every byte check failed
  (`original bytes differ` / `21132 matches`). `rootfs/overlay-4.1.json` now
  maps 8C148 base offsets → 8B117 offsets by key; the two new MBX strings have
  unique verified hits (0x492c7cc / 0xa899a4). 20 patches apply, six skipped.
- **Shim blocker, not the fake pages.** The earlier "undefined instruction in
  the fake GPIO page" was a wild stack in FCRAM: QEMU `gva2gpa 0xc38b3f3c`
  returned `0x20adef3c`. `kernel/shim/blk.c` and the input/Wi-Fi/MCU shims
  called XNU KPIs at 8C148 addresses that all moved in 8B117; the early
  PEGetGMTTimeOfDay hook also landed in the wrong function and produced the
  `OSDictionary::setOptions()+0x44` panic. `scripts/kpi.py` now resolves the
  addresses by LC_SYMTAB symbol (plus the gIOPlatform literal) and generates
  `ios3ds-kpi.h` for the shim build; `kernel/shim/kpi.h` keeps the 4.2.1 values
  as defaults.
- **Byte-identical default.** A fresh 4.2.1 build was compared with one from
  `git archive HEAD` sources: kernel/devtree/fakeio/shim/gpu/boot.cfg sha256
  all match; the shim is `7906b596…` before and after the KPI change.
- **QEMU.** With the relocated shim the full 8B117 patch set reaches
  `BSD root: md0`, launchd, SpringBoard, `[gl]` PICA200 contexts and the CA
  compositor, 0 md0 errors and live heartbeat/GPU logs; no early panic. The
  control set (15 core + md0 patches) also gets past `BSD root: md0` now.
- **SpringBoard crash loop (4.1):** with the relocated shim the boot no longer
  panicked but never left the spinner. The image's
  `LatestCrash-SpringBoard.plist` showed `EXC_BAD_ACCESS` at
  `liblockdown.dylib+0x2dfc`: the 4.1 `lockdown_copy_value` patch had been
  skipped in §87. 58 contexts/58 restarts in 900 s. The overlay now contains
  the full 8B117 replacement (44-byte stub at 0x4fbde4 plus a 160-byte
  key/value table at 0x4f9650), built from the 8B117 CFStringRef arrays and
  CFString literals; all original bytes are verified at build time.
- **4.1 home screen:** after that table patch, `make sd IOS=4.1` +
  `make sdimg IOS=4.1` and `emu/run.py ... --icount 2` reached the stock iPod
  home screen at t=120 s (screenshot
  `build/fw-4.1/run-final41b/screen_120.0s.png`), 1 GL context (no crash loop),
  0 panics, heartbeats with 0 md0 errors. Cydia/TLSFix stay disabled for 4.1.
- **Final soak:** the exact sequence `rm -f build/sd.img && make sdimg &&
  python3 emu/soak.py --cycles 5 --icount 2` on a fresh image printed
  **Result: PASS**: 5 cycles, SpringBoard/input up after 90 s, 0 restarts,
  no "not home after Home", no failed lock rounds. Default 4.2.1 kernel and
  shim hashes are unchanged (`505f451a…`, `7906b596…`).

## 102. Older iPod2,1 firmwares: 3.1.3 and 2.2.1 in QEMU (2026-11-23)

See docs/multi-version.md §6 for the full tables and hashes. All Apple files
were downloaded into `build/fw-3.1.3/` and `build/fw-2.2.1/` and are never
committed.

- **Firmware.** Apple retired the public 7E18 IPSW link (403); the archived
  Apple IPSW from archive.org was used instead (sha1 `5f4f5c01…`, pinned in
  `versions.py`). Its public keys decrypt kernel, device tree and HFSX rootfs,
  so the archive copy is authentic. 5H11a still comes from Apple's CDN with
  the published sha1 `9af5625e…`. `fwprep.py` learned `Restore.plist` (2.x)
  and basename-only key filenames (old img3 paths).
- **Epoch and memory map.** Disassembling `_pe_identify_machine` gave
  `ba_version=5` for 7E18 and `ba_version=3` for 5H11a. Both caches are linked
  at `0xC0000000` (entry `0xc0069040` / `0xc00657f0`), so `mkboot.py`, the
  shim linker script and `kpi.py` now take the base from `versions.py`; the
  shim still sits at PA `0x20840000` but is linked at `0xC0840000`.
  2.2.1's `__PAGEZERO` segment forced a `flat_kernel()` fix (ignore
  file-size-zero segments).
- **Shim VA bug.** The first 7E18 boot panicked in the shim at
  `pc 0xc084310c`, `far 0x808c8020`: `audio.c`, `cam.c` and `pica.c` still
  converted PA->VA with hardcoded `0x80000000`. They now use the generated
  `IOS3DS_KPI_VIRT_BASE`; PEGetGMTTimeOfDay's `gIOPlatform` literal is also
  per-build (`0xc025f53c` / `0xc02064a0`). All KPI *functions* resolve by
  symbol on both old kernels; no symbol fallback was needed.
- **Patches.** 24 of 37 8C148 kernel patches relocated to 7E18, 15 to 5H11a,
  each with verified original bytes (new `kernel/patchsets/*.json`). Timer
  hooks, `_fleh_irq`, `mdevstrategy` and the UART functions needed completely
  new byte patterns. The 13/22 skipped patches are enumerated in §6.4 with
  what each is needed for (gyro, CLCD wake, power waits, etc.).
- **Rootfs/userland.** New overlays drop every 4.2.1 file patch, binary,
  interposer and Cydia/TLSFix entry: 7E18 has a 96 MB dyld cache but no
  relocated offsets yet, 5H11a has no dyld cache at all. One large stale cache
  file is removed first so xpwn's `grow` has free allocation-map blocks. Our
  4.2.1-linked userland tools need per-version stub symbols and a reduced
  tool set before they can be installed (§6.5).
- **QEMU (7E18):** after the shim fix, XNU boots the S5L8720 drivers, mounts
  `BSD root: md0`, starts AppleFSCompression/AMFI, and then retries the root
  mount forever with **0** md0 requests served by the ARM9; no launchd,
  SpringBoard, panic or stall. That is the exact stop point: the memdev path
  is patched but no buffer reaches the block server (§6.6).
- **QEMU (5H11a):** first run used the pre-fix image and hit the same
  `far=0x808c8020` shim fault. With the fixed shim XNU starts, brings up the
  S5L8720 VIC/GPIO/clock/DMA drivers and then panics at `pc 0xc05fc55c`,
  `far=0x0` in a prelinked USB/OTG kext right after
  `AppleSynopsysOTG2::start` (backtrace through `IOService::startCandidate`);
  no BSD root, launchd or md0 requests. Deleting `/arm-io/usb-otg` through a
  per-version `dt_delete` override gets past that panic: 5H11a then reaches
  `BSD root: md0`, AMFI and `mDNSResponder[11]` with 1,630 md0 requests and
  0 errors, but never starts SpringBoard (black screenshots, no crash
  reports) - the exact stop point is activation/userland, not the block server.
- **4.x unchanged.** A `git archive origin/main` tree and this branch produced
  byte-identical 4.2.1 *and* 4.1 kernel/devtree/fakeio/shim/gpu/boot.cfg
  hashes from the same staged firmware (§6.7). Cydia/TLSFix and the skipped
  4.1 patches are documented as open items, not changed (§6.8).
- **Final soak.** The exact sequence `rm -f build/sd.img && make sdimg &&
  python3 emu/soak.py --cycles 5 --icount 2` on a fresh default image printed
  **Result: PASS**: SpringBoard/input up after 80 s, 5 app/home cycles, zero
  panics, stalls, md0/log/crash/hang/GPU failures; `make test` passed (94
  offline tests plus all host harnesses and the stub-symbol check).
- **Continuation (2026-11-24).** `make test` was rerun (all offline tests, host
  harnesses, stub-symbol and conflict-marker checks pass), and a fresh default
  build (`make userland`, `rm -f build/sd.img && make sdimg`) reproduced all six
  4.2.1 boot-file sha256 values in §6.7. The 4.1 patch set, overlay and registry
  entry are unchanged relative to `origin/main`. A second fresh image with
  `IOS3DS_QEMU=... python3 emu/soak.py --cycles 5 --icount 2` again printed
  **Result: PASS**: 5 cycles, SpringBoard/input up after 241 s (host loaded),
  zero panic, stall, md0, log, crash, watchdog or GPU failures.

## 103. Public issue #11 (New 3DS): "No way to enable WiFi" (2026-11-24)

The reporter's `log.txt` (not committed) answers the two questions from the
report directly. The firmware and chip **did** come up: `nwm.code` was read
(348160 bytes, main type4), SDIO enumerated the AR6014 (chip id `0d000001`),
BMI uploaded all four parts and ran the stub, `hi_board_data_initialized = 1`,
and HTC answered: `HTC ready: 10 credits of 1544 bytes` followed by
`htc: service 0100 -> status 0, endpoint 1, max message 1538 (1 polls)`.
Then:

```
[wifi-diag]   5659 ms htc: tx waits for credits on endpoint 0 (0 < 1)
[wifi] HTC services connected: -5 (WMI endpoint 1, ...)
[wifi-diag]   5659 ms FAILED in stage htc: HTC service setup, error -5, sdio status 04212080
[wifi] HTC service setup failed: -5 ...
[wifid] waiting for the ARM9 network backend (state 10)
```

`-5` is `SDIO_ERR_BUSY` (`boot/arm9/wifi/sdio.h:17`): **our own** credit
wait, not a rejected service or an SDIO error (STAT1 is 0). The credit model
from §91 gives endpoint 0 one credit, and
`mbox_send()`/`credit_wait()` (`boot/arm9/wifi/ar6k.c:513-536`, `485-511`)
refuses the next CONNECT_SERVICE when the response does not return it. The
QEMU model returns credits in the CONNECT_SERVICE response trailer
(`emu/qemu/hw/arm/ctr_wifi.c:425-466`, `1690-1719`), but this firmware did
not within the ~17 ms wait. dsiwifi's `htc_handle_pkt`
(`arm_iop/source/ath/htc.twl.c:28-45`) sends all five CONNECT_SERVICE
messages and SETUP_COMPLETE back-to-back, with no credit wait, on this chip.

So "no way to enable WiFi" is downstream: the driver parked the chip in reset
in state `failed`, the mailbox never got a backend/MAC, `ios3ds-wifid` kept
waiting and configd had no interface to drive. No preset network is needed
for scanning (docs/wifi.md §6 and §10); `wifi.cfg` was absent by design and
is not the fix. `origin/main` is still at the reported commit `edf6444`, so
nothing landed there after 0.4.0-126.

**Change:** `mbox_send()` funds endpoint 0 locally for its fixed handful of
control messages when a credit is not returned (at most 6 per bring-up:
5 CONNECT_SERVICE + SETUP_COMPLETE), logs each refill and counts them in the
`credits so far` diag line. Credits that *are* returned still top EP0 up;
the credit accounting for WMI/data endpoints is unchanged. QEMU needed no
model change: its model returns the credits, so the new branch is not taken
there. Hardware still has to confirm that Type4 accepts the back-to-back
handshake, as dsiwifi does.

**Verification:** `make -C boot` and `make sdimg` clean; `make test` green;
`emu/wifi_test.py --case ok` (18/18 checks), `--case settings` (no wifi.cfg:
chip idle, a80211 loaded) and `--case nwm` (firmware from a synthetic
BLZ-compressed `nwm.code`, full bring-up to HTTP fetch) all PASS. The UI test
`emu/wifi_ui_test.py --nwm` also passed 8/8 in 535 s: with no `wifi.cfg`,
Settings > Wi-Fi scanned, listed three networks, joined the WPA2 AP by
touchscreen and got a DHCP lease — i.e. the control the reporter missed is
there once bring-up succeeds. No new EP0 refill lines in QEMU, as expected.

## 104. Safari's memory wall and TLS 1.2 in Safari: /usr/lib copies, no kernel patch (2026-11-23)

Two tester reports, measured with the QEMU `ctr-wifi` model and slirp, every run
`--icount 2` (Old 3DS timing unless `--new3ds`): a local HTTP server on
`10.0.2.2:18080` serves the generated pages, a TLS 1.2-only HTTPS server on
`:18443` (RSA leaf) and `:18444` (P-256 ECDSA leaf) both reject TLS 1.0 (minimum
and maximum TLSv1.2). The test CA was added to `rootfs/certs/roots.pem` only
while measuring and then reverted; it is not in the tree. The pages and certs
stay in `build/tlstest/` (gitignored). `ios3ds-diag -urlpoll` opens the URL in
Safari; the harness adds `ios3ds_mem=1 ios3ds_applog=1 ios3ds_urltest=1`, the
new `ios3ds_urlport=<port>` override (the host's 8080 is often busy) and
`ios3ds_probe=1`.

**1. Big pages on the 128 MiB device.** The old reports were "Cannot allocate
memory" on the first big page (NSPOSIXErrorDomain 12), a retry works.
- **Page A:** a 4,013,130-byte HTML (2,200 paragraphs) with 24 generated JPEG
  images (320x240, 24-58 KB each). Safari fetched the HTML and all 24 images;
  `MobileSafari` 7.7-12.6 MiB resident (7,900-12,932 KiB), free 42.4-45.3 MiB
  (43,416-46,380 KiB), `memorystatus_level` 59, no alert. Screenshot
  `build/tlstest/shots/tls-big-final-page.png`.
- **Page B:** a 3,648,172-byte HTML with 20 1024x1024 JPEGs (~480 KB each, ~3 MiB
  decoded each), opened 5 s after the home screen to catch the boot-time
  daemons. Safari fetched all 20 images; peak `MobileSafari` 27.0 MiB
  (27,664 KiB), free **20.1 MiB** (20,588 KiB), level 49, mbuf 136 and
  114/3806 clusters, no jetsam, page loaded.
- **What fails:** no allocation fails in QEMU. `vm_stat` and
  `kern.memorystatus_level` are the only figures available: `host_get_host_priv_port`
  and the `host_priv_self()` Mach trap are both refused to the root `[mem]` job,
  so `host_zone_info` (kernel zone sizes) cannot be read, and none of
  `vm.zone_map_size`, `vm.page_free_count`, `vm.pages`,
  `vm.page_pageable_internal_count`, `vm.max_proc_mmap`, `vm.max_map_count`,
  `kern.max_task`, `kern.max_task_pages` exist in 4.2.1's sysctl tree (the new
  `[mem N] sysctl` line is empty). Nothing points at a zone/VM limit.
- **GPU arena:** still **12 MiB** wired (`[pica] arena 12288 KiB`), by far the
  largest single allocation we own. It is used: at 301 CA frames SpringBoard
  held 4,887 KiB of GPU memory in 76 textures; the earlier 8 MiB arena fragmented
  on hardware (hwlogs/README, 704 KiB texture, 290 KiB free in 90 blocks). The
  fallbacks 8/4/2 MiB stay, the default does not shrink.
- **Daemons:** `profiled` (com.apple.managedconfiguration.profiled) was resident
  in every measurement (2,420-2,428 KiB) and nothing on ios3ds uses it, so it is
  now in `disable_daemons`; the reports no longer list it (**-2.4 MiB** at boot).
  The rest of the list is Apple's (mediaserverd 4.0, configd 2.8, lsd 1.9,
  installd 1.3, mDNSResponder 1.3) or ours and small (ios3ds-mcud 1.16,
  ios3ds-diag 0.65, ios3ds-wifid/input ~0.5); no further safe cut.
- **Conclusion:** QEMU cannot reproduce the hardware alert even with 20.1 MiB
  free and 27 MiB Safari. The 2.4 MiB from `profiled` and accurate accounting are
  all that is ours to fix here; the `[applog] MobileSafari load error` line
  (now also appended to `/private/var/tmp/ios3ds-applog.txt` when the sandbox
  denies `/dev/console`) is what a hardware log needs to name the layer.
- **New 3DS:** `--new3ds` (n3ds_ram=1) after boot: **168.5 MiB free**
  (172,576 KiB), level 79 (Old: 48.1 MiB / 49,212 KiB, 65); the same TLS 1.2
  page loads in Safari (`build/tlstest/shots/tls-n3ds-final-page.png`).

**2. TLS 1.2 in Safari.** The sandbox denies `/Library` and
`/var/mobile/Library/ConfigurationProfiles` but allows `/usr/lib` (re-log 71/72).
- **SANDBOX=1 is not the way.** With `make sdimg SANDBOX=1` (patch verified in
  the image: `b` from 0x803ced98 to shim+0xF4, `shim_sb_evaluate` at that offset)
  a probe in MobileSafari still got `EPERM` opening
  `/Library/MobileSubstrate/MobileSubstrate.dylib`. A temporary shim that
  returned allow for *every* `sb_evaluate` operation gave the same `EPERM`, so
  the vnode-open check does not reach that function in this build. Safari showed
  "could not establish a secure connection" and the TLS server saw no request;
  no out-of-memory alert. The old "SANDBOX=1 OOMs" explanation was not
  reproducible because TLSFix never loaded.
- **Fix:** the build copies TLSFix's two dylibs to `/usr/lib` and rewrites the
  strings that point at denied paths (same-length NUL padding, `scripts/cydia.py`
  `sandbox_copies` / `sandbox_patch_load` / `sandbox_profile_copy` in
  `rootfs/cydia.json`):
  - `/Library/Frameworks/CydiaSubstrate.framework/CydiaSubstrate` ->
    `/usr/lib/ios3ds-CydiaSubstrate.dylib`;
  - `tlsfix.dylib` -> `/usr/lib/ios3ds-tlsfix.dylib`, its CydiaSubstrate
    `LC_LOAD_DYLIB` and its `/var/mobile/Library/ConfigurationProfiles` string
    rewritten to the `/usr/lib` copies;
  - the CA payload `ios3ds-roots.plist` -> `/usr/lib/ConfigurationProfiles/`.
  `ios3ds-applog` still tries `/Library` first (SpringBoard, Cydia and other
  unsandboxed apps unchanged); when that dlopen fails it loads
  `/usr/lib/ios3ds-tlsfix.dylib` from a 0.25 s run-loop timer once CFNetwork is
  up (constructors run before frameworks). No kernel patch, no change to the
  sandbox profile.
- **Results** (Old 3DS, same image; before = no copy fallback, after = final):
  | | before | after |
  |---|---|---|
  | local TLS 1.2-only, RSA (Safari) | no connection, "could not establish a secure connection" (`tls-sandbox1-page.png`) | **loads**, host logs `TLSv1.2` (`tls-copy4-page.png`) |
  | local TLS 1.2-only, P-256 ECDSA (Safari) | -9836 | **loads**, host logs `TLSv1.2`; TLSFix's OpenSSL check read `/usr/lib/ConfigurationProfiles` (`tls-ec1-page.png`) |
  | MobileSafari resident, first page | 8.2 MiB (HTTP, 8,432 KiB) | 11.7 MiB with TLSFix (EC, 11,948 KiB), 12.6 MiB (big page, 12,932 KiB) |
  | free / level after the page | 48.1 MiB / 65 at boot | 40.5 MiB / 59, no OOM |
  The old OOM did not reappear; TLSFix costs about 2.5-3 MiB resident, its text
  is file-backed. Screenshots: `build/tlstest/shots/`.
- **Tools left in the tree:** `ios3ds-diag` honours `ios3ds_urlport=PORT` for
  `-urlpoll` (host 8080 conflicts); `ios3ds-applog` gained
  `ios3ds_probe=1` (writes executable path, `DYLD_INSERT_LIBRARIES`, `open()`
  results and dlopen handles to `/private/var/tmp/ios3ds-applog-probe.txt`) and
  falls back to `/private/var/tmp/ios3ds-applog.txt` for alerts/load errors when
  `/dev/console` is denied; `ios3ds-sample` tries `host_priv_self()` and prints
  the VM sysctls at the first report.
- **Files:** `scripts/cydia.py` (`sandbox_copies`, `sandbox_patch_load`,
  `sandbox_profile_copy`), `rootfs/cydia.json`, `rootfs/recipe.json` (profiled
  disabled), `tools/ios3ds-diag/applog.c` (fallback, probe, tmp logging),
  `tools/ios3ds-diag/diag.c` (`ios3ds_urlport`), `tools/ios3ds-sample/sample.c`
  (`host_priv_self`, VM sysctls); docs: README, certificates.md, cydia.md,
  location.md, n3ds.md, this section.
- **Validation:** `make test` green (Python, pica, gl, wifi, gps, cpad, n3ds, ALS,
  stub symbols, no conflict markers); a fresh `make sdimg` + `emu/soak.py
  --cycles 5 --icount 2` printed **Result: PASS** (boot 90 s, 5 cycles, 0 panics,
  0 ARM11 stalls, 0 md0 I/O errors, 0 process crashes, 0 jetsam; the one
  "SpringBoard restart" is the boot-time connection), and again after rebasing
  onto main's 4.1 work (fresh image, boot 81 s, same checks).

**Continuation (2026-11-24):** re-verified on the rebased tree. A fresh
`make userland` + `rm -f build/sd.img && make sdimg` printed the four
`sandbox_copies` lines (81 trust-store roots, 121 profile roots; the test CA is
not in the tree), `make test` was green, and `emu/soak.py --cycles 5 --icount 2`
on that image printed **Result: PASS** (boot 120 s, 5 cycles, 0 panics, 0 ARM11
stalls, 0 md0 errors, 0 crashes, 0 jetsam; the one "SpringBoard restart" is the
boot-time connection). The memory figures above were rechecked against the
sampler, which prints **KiB**: an earlier draft divided those values by 1000 and
labelled them MiB, so the numbers here are corrected (20,588 KiB = 20.1 MiB, not
20.6; 172,576 KiB = 168.5 MiB, not 172.6). Re-ran the pages: `huge.html` reached
26,384 KiB Safari and 22,348 KiB (21.8 MiB) free with no alert (a loaded host
meant the 240 s timeout caught 8 of 20 images; the full-load 20,588 KiB figure
stands). With `build/tlstest/ca.pem` appended to `rootfs/certs/roots.pem` for the
run and then reverted, Safari loaded the local TLS 1.2-only page over both the
RSA (`:18443`) and P-256 ECDSA (`:18444`) certificates; the host logged
`TLSv1.2` and Safari showed the lock icon
(`build/tlstest/shots/tls-tls-verify2-page.png`, `tls-ec-verify-page.png`).
New 3DS: 172,636 KiB (168.6 MiB) free / level 79, Old 49,004 KiB (47.9 MiB) /
64. No code changed in the continuation.

## 105. Cydia refresh/install time: local repo, md0 chunks, write-back (2026-11-23)

- **Report:** public issue #1, New 2DS XL: adding a Cydia repository and
  installing a tweak take "a really long time". The question was which part
  is ours: software TLS/DNS, apt's Packages parse on the 400 MHz ARM11,
  dpkg's `f_sync` storm on md0, the ARM9's block server, or Substrate
  re-signing.
- **Harness** (`emu/cydia_perf.py`): builds a 32-file `iphoneos-arm` `.deb`
  with host `dpkg-deb -Zgzip` (no downloads), writes `Packages`, `Packages.gz`
  and a `Release` with matching hashes, and serves them from a random high
  host port through slirp (`http://10.0.2.2:<port>/repo`). It copies
  `rootfs.hfs` out of a fresh `build/sd.img` with `mtype -b`, adds a launch
  daemon (`/usr/libexec/ios3ds-cydia-perf`,
  `com.ios3ds.cydia-perf.plist`), the source list, the `.deb` and
  `wifi.cfg`/dummy firmware, copies the image back, boots with
  `-icount shift=2,sleep=off`, and stops with QMP when `[cperf] ALL DONE`
  appears in `sd:/ios3ds/log.txt`. `boot.cfg` gets `log_ts=1` and `ioprof=1`.
  Two harness bugs bit the first runs and are fixed: `mtype` without `-b`
  truncated the 1 GiB HFS image at the first `^Z` (only 914 MB was copied),
  and host `dpkg-deb`'s
  default zstd-compressed members are unreadable by iOS's dpkg 1.18
  (`unknown compression for member 'control.tar.zst'`). The guest's bash has
  no `ifconfig`, so network-ready is a `/dev/tcp/10.0.2.2:<port>` probe.
- **Before** (stashed tree = `origin/main`, 215 s wall, guest times from the
  ARM9 stamps): network ready 19.584 s; `apt-get update` 19.584→21.381 =
  **1.797 s**; `--download-only install` 0.173 s; `dpkg -i` 21.555→24.048 =
  **2.493 s**; total **4.463 s**, rc=0 everywhere. Host HTTP log: 404s for
  `Release.gpg`/`Packages.bz2`/`Packages.lzma`, then 200 for `Release` and
  `Packages.gz`. `ioprof=1`: during `dpkg -i` 618 md0 chunks, ~584 KiB
  written, 316 ms ARM9 service time.
- **First after run:** the new 64 KiB bounce made the shim compute
  `secs = (0xFFE0 + 511) / 512 = 128`, i.e. 64 KiB, while the ARM9 accepts at
  most 0xFFE0 bytes: large page-ins failed with
  `[blk] read lba 287720 x128 ... failed (1000)` and iOS never reached
  SpringBoard. Fixed by masking the chunk length down to a
  whole 512-byte sector in `kernel/shim/blk.c`.
- **Second after run** (256-slot cache, O(n²) flush, 183 ms idle): rc=0 but
  *slower* than before (refresh 3.07 s, install 3.35 s). The `[io]` counter
  showed 2,143 sectors written back in 29 guest seconds; sorting up to 256
  dirty slots on every flush cost more than the saved SD commands.
- **Final after run** (104 s wall): refresh 19.696→21.443 = **1.747 s**;
  download **0.078 s**; `dpkg -i` 21.522→23.945 = **2.423 s**; total
  **4.248 s** (−4.8 %). rc=0, no `[blk] ... failed`, panic or ARM9 exception.
  During install: 556 chunks, ~612 KiB, 288 ms ARM9; the cache logged 0
  direct writes and 0 errors. The remaining seconds are dpkg/apt CPU in
  QEMU, where the SD is instant; the saved `f_sync` commands are worth much
  more on the tester's card.
- **Changes:** 64 KiB bounce in the FCRAM gap (docs/boot-perf.md §8);
  512-sector write-back cache with deferred `f_sync`, clean-flag ordering and
  host tests (`tests/test_blkcache.c`, `make test`); apt timeouts/retries and
  no translation downloads in `/etc/apt/apt.conf.d/99ios3ds-speed`. The dead
  2011 Cydia sources are still listed in Cydia, they now fail fast; HTTPS
  root store and TLSFix are untouched.
- **Not a cost:** no code path in our build re-signs Substrate or tweaks;
  boot-args disable code signing (`amfi_allow_any_signature=1
  cs_enforcement_disable=1`). DNS/TLS timings for real HTTPS repos are in
  [certificates.md](certificates.md) §4 and are unchanged by this work.
- **Re-run after the rebase onto `origin/main` edf6444** (same harness, fresh
  images, `fill=0`; guest wall time on this loaded host): refresh 2.993 →
  **2.443 s**, download 0.132 → 0.277 s, `dpkg -i` 3.438 → **2.664 s**, total
  6.563 → **5.384 s**. Guest CPU (user+sys): refresh 0.72 → 0.67 s, install
  0.67 → 0.58 s. The first, quiet-host numbers above are the fairer picture of
  the code change; the second run shows how far the wall clock moves with
  host load, which is why the docs ask for chunks/KiB/ARM9 time.
- **apt's Packages work** is CPU on the ARM11, not the local HTTP: 2,000
  synthetic stanzas (440 KB) take 1.3–1.6 s of guest CPU in `apt-get update`
  and another second or two of blocked md0 time. apt 0.7 prefers bz2: that
  run was 5.121 s / 1.60 s CPU; with `Acquire::CompressionTypes::Order
  { "gz"; ... }` shipped it takes 3.762–4.257 s / 1.30–1.43 s. Disabling
  `Dir::Cache::pkgcache` looked faster for `update` alone (6.1 s real but the
  later `--download-only` CPU went 0.07 → 0.47 s because the text lists are
  re-parsed), so it is not shipped.
- **Dead default sources** are the refresh outlier on real networks. Adding
  one (`cydia.zodttd.com`, still resolving) to the local repo's list:
  `apt-get update` **2.544 s** without a fix (apt followed a 308 redirect and
  retried) vs **1.490 s** with the host pointed at 127.0.0.1 in
  `/private/etc/hosts` (connection refused at once). The same entries cover
  ModMyi and repo666.ultrasn0w.com; the 10 s timeout and `Retries 0` stay as
  a backstop. A per-host `Acquire::http::Proxy` was tried first and had no
  effect in this apt build (it still resolved the origin host).
- **Cache read path:** `blkc_dirty()` used to scan all 512 slots on every
  monitor loop (the idle check) and `blkc_dirty_lba()` every sector of every
  read. A dirty-slot count makes both O(1); when no sector is dirty, a read
  takes the old single-`f_read` path instead of being split into runs. Host
  tests still pass. A coalescing flush (sort dirty sectors and write adjacent
  runs with one CMD25) was prototyped; QEMU's counters and ARM9 time could
  not show a reliable win, and its write-call accounting disagreed with the
  sectors-written counter, so it was dropped rather than shipping an
  unexplained cache change.
- **Harness additions:** `--fill N` (synthetic index stanzas), `--bz2`
  (offer bz2 next to gz), `--skip-refresh` (isolate dpkg), `--extra-source`
  (a second/dead source), `--apt-extra` (override config as
  `99zzios3ds-perf`) and per-phase guest CPU times. See
  [cydia.md](cydia.md) for the resulting tables.
- **Verification:** after the rebase, `make test` passes (cache test, CI
  stub-symbol check and conflict-marker check) and a fresh-image
  `python3 emu/soak.py --cycles 5 --icount 2` printed **Result: PASS**
  (5 cycles, 0 panics/restarts after boot, 0 md0 errors). The rebase conflict
  in this file was section 101 arriving at the same tail; the 4.1 section is
  kept before this one. On the continuation run the final tree was rebuilt
  (`make userland`, fresh `make sdimg`) and re-measured: refresh 1.645 s,
  download 0.385 s, `dpkg -i` 2.618 s, all rc=0 with no `[blk] ... failed`;
  a second fresh-image 5-cycle soak again printed **Result: PASS**. The
  `[io]` counters over the install still show the cache absorbing the small
  dpkg writes (0 direct-write sectors, 0 sync errors).

## 106. Both eyes without the 8x ARM9 copy (2026-11-23)

Section 90 left the flag-on path at 120-186 ms per ARM9 screen copy in QEMU
(section 90's numbers: 15.4 ms with the flag off), enough to break the soak's
timing checks. This entry makes the two-eye copy incremental instead of
rebuilding all 360 screen columns on every blit.

- **What is cached:** `boot/arm9/display.c` keeps a two-accumulator signature
  of the source row that feeds each screen column (all 320 pixels, including
  the alpha depth stamp). A column whose row signature is unchanged keeps the
  eye pixels already in `top_fb[0]`/`top_fb[1]`. When the frame changes
  everywhere (a page swipe, a return to the home screen), at most 120 columns
  are rebuilt per blit, so the extra work is spread over three copies rather
  than spent in one long ARM9 turn. The cache is dropped on a slider,
  `stereo3d`, screen-mode or shown-framebuffer-set change.
- **Hot loop:** a 256-byte alpha→class table replaces `stereo_class_of`'s
  switch per pixel; the two eye offsets are computed once per class per frame
  instead of twice per pixel; the source row is copied into a buffer with
  eight edge pixels of padding on each side, which removes the per-sample
  clamps (the largest offset is 2*k = 8 iOS pixels). The maths and pixel
  values are the same as section 90's; the host test compares a sampled grid
  of both eyes with the original class/delta rules after convergence.
- **Flag off:** none of this is reached. The normal `blit_top()`/`blit_bottom()`
  pair is untouched, as are the flat/slider-down and unstamped-frame
  fallbacks; `tools/ios3ds-pica/tests/test_stereo_copy.c` still checks the
  byte-for-byte flag-off and flat copies.
- **Why QEMU had no two-eye path at all:** instrumenting the CA frame
  classifier showed `50 draws, phase START, 0 icons, 0 stamps` on the first
  home frame: CA draws SpringBoard's wallpaper as many small layers, not as
  one full-screen quad, and `stereo_draw()` refused to classify anything
  while `phase == STP_START`. `stereo_draw()` now treats the first full-screen
  draw (opaque or not) or the first `STEREO_MIN_ICONS` small draws as the
  background; the following 46 draws of the QEMU home frame are stamped as
  icons/dock. A tiny partial update (the next CA frame had two draws) no
  longer wipes the class map from untouched pixels. `stereo_frame_t` gained a
  `stamps` counter so a phase reached without any stamps is not called home.
  Apps and the lock screen still go flat on a second full-screen opaque draw;
  new host tests cover all three cases.
- **Getting the stamp to the ARM9:** the PICA200's direct present swizzle uses
  the render target's alpha byte for the surface's blue channel, so the
  class map never reached `/vram` (a QEMU memory dump showed alpha 0 in every
  sampled word). `tools/ios3ds-gl/pica_be.c` now uses the CPU read-back for
  the display target when `stereo_enabled()`, which writes BGRA with the
  stamp intact. Sending the stamp through the swizzle's alpha instead was
  tried first: it broke the blue channel and the GPU present check (147199 of
  153600 pixels, `0xff0000ff` vs `0xff000000`), so that approach was dropped.
  With the flag off the direct present stays exactly as before.
- **Logging:** `gm_report()` now adds `[monitor] stereo eye columns: R rebuilt,
  U reused in N s` next to the 30-second CPU-copy line, so a log shows how
  much of the two-eye work was actually done.
- **Frame times, private QEMU, `--icount 2 --slider3d 255`:** before this work
  section 90 logged `42 copies in 30 s, 120.3 ms each` and, on a longer run,
  `33 copies in 38 s, 186.4 ms each` with the flag on, against `15.4 ms each`
  with the flag off. After the fixes and the cache, a 360 s run first logged
  `70 copies in 30 s, 14.3 ms each` (before the stamped home frame arrived)
  and `[monitor] stereo 3D: both eyes ... barrier on`, then `382 copies in
  30 s, 6.9 ms each` with `384 eye columns rebuilt, 4623 reused`, and
  `401/409/409 copies, 6.6/6.4/6.4 ms each` with `9/0/9` columns rebuilt per
  30 s. So the two-eye path is now cheaper than the old one-eye average:
  almost every column is reused frame after frame. The GL side still pays
  for the CPU read-back when the flag is on (the direct GPU present cannot
  carry the stamp); that is ARM11 work, not the ARM9 copy measured here. The
  flag-on soak logged `273 copies, 10.0 ms each` and `347 copies, 8.1 ms each`
  while cycling apps (4253/4080 columns rebuilt and ~80-113k reused per 30 s).
- **Soaks:** fresh `build/sd.img` each time, private QEMU, `emu/soak.py --cycles
  5 --icount 2`: `stereo3d=0` PASS (boot 90 s); `stereo3d=1 --slider3d 255` PASS
  (boot 80 s). One earlier flag-on run failed only its lock-round screenshot
  check: the unlock ended on Spotlight, which the harness had treated as a
  failed unlock after a single Home. `emu/soak.py` now dismisses Spotlight
  after that Home (page swipe + Home), the same recovery the normal cycle
  path already used; the final flag-on run passed with no lock-round failures.
  Both runs had 0 panic/stall/md0/crash/GPU failures, and no "not home after
  Home" failures.
- **Continuation on 2026-11-24 (after the previous worker was OOM-stopped):**
  reviewed the committed branch, rebuilt the shim (GPU part 31680/32768 bytes,
  1088 bytes headroom), rebuilt the private QEMU from
  `/tmp/opencode/qemu-stereoperf` (binary and bundled libslirp copied to
  `build/qemu-local/b/`), and repeated `make test`, `make userland` and fresh
  `--icount 2` soaks with that one binary. The flag-off run (boot 161 s) and
  the flag-on run (`--slider3d 255`, boot 90 s) both end in `Result: PASS`,
  with 0 panic/stall/md0/crash/GPU failures; each needed one "second Home"
  recovery, but no lock-round failure and no "stuck" cycle. Frame times from
  their logs: flag off `33 copies in 30 s, 14.7 ms each (ARM9)`; flag on
  `85 copies, 13.0 ms each` before the stamped frame, then `292 copies,
  9.4 ms each` (3826 eye columns rebuilt, 83129 reused) and `344 copies,
  8.2 ms each` (3806 rebuilt, 111167 reused). So the final two-eye path is
  at or below the ordinary copy's time in QEMU, down from 120-186 ms in
  section 90. Two earlier flag-on retries failed only the harness's
  screenshot checks: one unlock ended with the Spotlight keyboard up, one
  cycle left a typed Spotlight query and keyboard ("Game Center"). No guest
  failure was logged in either. `emu/soak.py` now shares one
  `dismiss_spotlight()` helper between the normal and unlock paths: up to
  three page-swipe + Home attempts, tapping the search field's X before the
  later attempts, plus a "clear query + Home" step in the "not home"
  recovery list. The passing runs above use that harness; screenshots and
  reports are in `build/emu/soak-on3/` and `build/emu/soak-off2/`.
## 107. Silent 3DS mic and a Music library that looked empty after carry-over (2026-11-24)

Design: docs/audio.md §7 (microphone), §10 (Music). Hardware input:
`issue-reports/run-2026-11-24.txt` (excerpts only): `mic on: 11901 Hz`,
`rx stop words=219806 mic jumps>16=9 maxd=cd underruns=0 resyncs=4c`,
`[music] 1 song(s)`, carry-over `partial`. The earlier run 14 has the same
`maxd=ab`, so this is reproducible, not a one-off.

**Mic, trace.** Samples really reach iOS: `rx start ch4`, 219,806 words,
0 underruns, and the shim's own `mic jumps`/`maxd` are computed from what it
delivered. On hardware the delivered frame-to-frame delta never exceeded
0xcd/0xb4. With `AUD_MIC_GAIN_Q15` (x0.0657) that implies raw MIC halfwords of
roughly 2000-3000, i.e. something reaches the ADC, but the absolute level is
far below what the QEMU sine path needs. Two concrete differences from
libn3ds `CODEC_init` were found:
- `microphoneInit()` writes three coefficient-RAM filter sets: page 4 reg 8
  (10 Hz high-pass), page 5 reg 8 and page 5 reg 72 (PRB_P25 chains). ios3ds
  skipped all three on the assumption that reset coefficients pass audio
  through; GBATEK's 3DS TSC summary lists exactly these as the 3DS microphone
  filters. They must be written before the DACs power up: page 100 reg 118
  then locks the coefficient RAM. `boot/arm9/audio.c` now writes them in the
  unlocked window, plus page 100 reg 49's non-GBA bit.
- MIC_CNT bits 0-1 were 0: GBATEK's "MakeStereo" duplicates every mono sample
  into both halves of MIC_DATA. The shim reads the halves as *consecutive*
  samples, so it must ask for format 2 ("Normal"). `mic_hw_start()` now writes
  0x0e rather than 0x0c; the QEMU MIC model honors the format bits and logs
  them at enable. The default CODEC bias, PGA (21.5 dB), ADC input selection
  and page 0 0x51/0x52 power/mute already matched libn3ds.

**Mic, diagnostics.** `aud_mic_t` gained a one-second report (three extra
cache lines, written by the shim): raw MIC peak/sum-of-squares/sample count and
delivered peak/sum-of-squares/frame count, plus a sequence number. The ARM9
prints `[mic] 1 s: FIFO peak ... rms ..., iOS peak ... rms ... (N frames)`
with one decimal and `[mic] CODEC on: ...` with the ADC/mute, MICBIAS/PGA,
input-select, coefficient-lock, gain and I2S registers at ADC power-up.
The next card run with `diag=1` decides it:
- small FIFO peak (tens) but normal CODEC registers -> analog/bias/PGA path,
  raise page 1 reg 47 or check the board's mic;
- FIFO peak in the thousands and small iOS peak -> the +23.65 dB compensation
  or the VirtualAudio route is wrong for RecordAudio, not the CODEC;
- both peaks similar to a spoken voice -> the fix worked; listen to the memo.

**Music, reproduction and fix.** A fresh `make sdimg CARRY_FROM=<old image>`
in QEMU reproduced the report: `[music] 1 song(s)`, `carry-over result:
carried`, Music.app opens, shows "Updating Library..." for ~45 s, then the
Playlists tab contains only "Add Playlist...". Forcing a `partial` result by
putting a symlink in the carried tree (hfsplus verification sees a different
target size) reproduced the user's `partial` too; the new image still has
1 `item`, a decodable ALAC `.m4a` and all post-processed DBs. So carry-over is
not the missing-song cause. `scripts/music.py` only wrote the *hidden* master
playlist ("iPod", `is_hidden=1`); no visible playlist existed. It now also
writes one ordinary `ios3ds` playlist with every track, following libgpod's
`itdb_sqlite.c` (`distinguished_kind=0`, `is_hidden=0`, `media_kinds=1`) and a
`container_ui` row. QEMU on a `CARRY_FROM` image with a forced `partial` now
shows `ios3ds` in the Playlists tab (`build/emu-session/music-visible.png`);
host `tests/test_music.py` checks the row and its track membership.

**Verification without hardware:** `make test` green (incl.
`tests/test_audio.py`'s new MIC_CNT-format and one-second-report checks and
`tests/test_music.py`'s visible-playlist check); `make userland` and
`make sdimg` clean; shim main part 24268/24320 bytes, GPU part 31680/32768.
A fresh QEMU image boots to SpringBoard with the local, format-aware MIC model
(built only under `build/qemu-local`; the shared `build/qemu` symlink was left
alone). `rm -f build/sd.img && make sdimg && python3 emu/soak.py --cycles 5
--icount 2` on a fresh image: **Result: PASS** (5 cycles, 0 panics, stalls,
md0 errors or crashes).
A full Voice Memos tap sequence in `emu/tap.py` was timing-dependent under the
shared host's load (the folder animation missed the scheduled tap), so the
`[mic]` lines still need a real console run; host tests cover the shim side.
## 108. Camera on the Old 3DS: START status 3 is out of frame-buffer memory, not a sensor fault (2026-11-24)

- **Report** (`issue-reports/run-2026-11-24.txt`, git-excluded; only short
  excerpts are quoted here): Old 3DS, `CFG11_SOCINFO 1`, `diag=1 powersave=0
  hp_codec=1`. All three sensors answer `chip id 2280` at boot, `cameras
  present 0x3`, then `stream 0: start camera outer -> status 3`,
  `frame request failed, status 6`, `stopped after 0 frames`, `monitor at stop:
  0 CAM_STAT samples`. Camera.app shows no camera and the shutter never opens.
- **Decoding (docs/camera.md §3.3):** status 3 at START is
  `CAM_ERR_NO_MEM` from `op_start()`'s `IOMallocContiguous()` loop; status 6 at
  FRAME is `CAM_ERR_NOT_RUNNING` because `C.running` was never set. The ARM9's
  `CAM_ST_FAILED` is *also* 3 but would come with `[cam] ... init failed
  (step N)` and `kernel state 3`; this run has neither. The absence of
  `[cam] start outer camera ...`, `[cam] iOS wants ...` and any `[cam-diag]`
  start dump pins the failure to the first 150 KiB allocation, before the
  mailbox request. `cameras present 0x3` is from the boot probe and does not
  mean a start happened.
- **History/old-vs-new bisect:** the card's own banner read
  `m1-0.4.0-21-g5800f1e-dirty-20261122.1622` (21 commits after the camera-working
  0.4.0 tag), not `edf6444` as reported. `git log`/`git diff` show **no changes
  to cam.c/camera.c/cam_proto.h/vcd.c/docs/camera.md** from 0.4.0 (`6d963b6`)
  to `5800f1e`; the last Old-3DS hardware run with pictures was run 29 (build
  43, `53304df`) and the register sequence in this run matches it byte for
  byte (`CFG11_CAMERA_CNT 00 -> 01`, CAM0 `4610`, CAM1 `4600`, the same I2C
  buses and IDs). What changed between those builds is the rest of the image
  (TLSFix/Substrate, Music library, GPS daemon, bigger rootfs, GPU arena
  churn) - i.e. XNU's contiguous/wired heap at the moment Camera.app starts.
  New 2DS XL has 256 MiB (`n3ds_ram=1`) and kept working; the 128 MiB Old 3DS
  did not. A full boot bisect over ~100 commits is not possible here (one QEMU
  boot is ~80 s and the failure needs hardware), so this is an evidence-based
  narrowing, not a proven commit-level bisect. The two later camera-related
  changes (`aab28fb`, `e7175ba`, ambient light) are not in the card's build.
- **Fix:** `kernel/shim/pica.c` reserves one 450 KiB block for the three
  150 KiB frames during GPU bring-up (single-threaded, private owner
  `0xFFFFFFFE`, full-size arenas only), publishing its PA to
  `kernel/shim/cam.c` with the arena VA/PA. cam.c uses those frames at START,
  falls back to `IOMallocContiguous` when no block was reserved, gives back a
  partial allocation on failure, and logs
  `[cam] buffers: arena|IOKit ...`, `[cam] NO_MEM ...`, `[cam] FRAME status 6
  ...` and the buffer source in `[cam] start ...`. `vcd.c` retries START four
  times on `CAM_ERR_NO_MEM` instead of giving up after one. `pica.c` is now
  built at `-Os` with `PICA_HOT` (-O2) on its three hot functions to keep
  gpu.bin at 31776/32768 bytes (CI's larger codegen needs the room).
  `docs/camera.md` §7.1/§7.2 record the diagnosis, the next-run checklist and
  the Old/New comparison.
- **Verified without the console:** `make test` green (host camera allocator:
  arena used first with 0 IOKit calls; IOKit fallback; forced partial failure
  freed and retried; pica: a private 450 KiB block survives GL free_all).
  Shim/gpu.bin builds at 23756/24320 and 31776/32768 bytes. QEMU Old model,
  `--icount 2`, fresh image: `[cam] reserved 3 x 150 KiB in the GPU arena at
  PA <PA>`, `[cam] buffers: arena 3x150 KiB @<PA>`, `src 1`, START
  status 0, frames delivered, 0 overruns, frame dump written;
  `emu/stress.py --cycles 0 --camera-cycles 2 --icount 2`: 2 starts/3 stops,
  0 camera start failures/OOM lines (the run's overall FAIL is the unrelated
  unlock/home-screen flake).
  Required fresh-image soak `rm -f build/sd.img && make sdimg && python3
  emu/soak.py --cycles 5 --icount 2`: **Result: PASS**. `emu/stress.py`'s
  camera icon coordinate was corrected to (123,360) for the current
  IconState; with earlier app cycles the first QEMU touch still sometimes
  lands in Spotlight (harness timing, unrelated to the camera), and that run's
  unlock rounds also report the known home-screen diff flake.
- **Still needs hardware:** reflash and open Camera.app on the same Old 3DS.
  The log must show `[cam] buffers: arena ...`, `src 1`, START status 0, the
  ARM9's start dump and frames, plus a live preview and a saved photo. If
  `[cam] NO_MEM ...` or `src 2` appears instead, the next card run has to
  include the `[pica] arena ...` line - that decides whether the arena was
  absent/full or the fallback also failed. Video, 3D photos and New 3DS models
  are not re-verified by this run (the change is common to both models).

## 109. Stereo 3D on hardware: keep the GPU mirror on, build both eyes on the PICA200 (2026-11-24)

Hardware run 3 (`issue-reports/run3-3d.txt`, git-excluded) answered the console
question from section 90: with `stereo3d=1` the 3D slider did **nothing**.
The log showed `[mcu] ... 3D slider 8` once at boot, then
`[monitor] CPU screen copy: 43 copies in 30 s, 301.4 ms each (ARM9)` and
`53 copies in 30 s, 229.1 ms each`. There were no `[stereo]` lines at all:
at that revision none existed, so neither slider changes nor PDC/barrier
transitions were observable.

- **Why:** `a9_main()` forced `cfg.gpu_mirror = 0` whenever `stereo3d=1`, so
  every frame was copied by the ARM9 (230-300 ms on an Old 3DS). The only
  thing that could open the barrier was `display_blit()` finding a stamped
  home frame, switching PDC0 through the mailbox, and getting an ack. That
  gate never fired on the console. The MCU slider itself is read correctly
  (reg 0x08 is "raw 3D slider position"; other logs show 0/8/9/10/14/253 at
  boot, and threshold 16 rejects the resting 8/14), but no log line ever
  proved whether the user's movement was published. The 3D LED is a separate
  MCU register (0x2C) that the MCU firmware lights for Horizon's barrier;
  writing the barrier register directly left it dark.
- **Fix:** `stereo3d=1` no longer switches the mirror off. The GPU copies the
  screen exactly as with the flag off while the slider is down or a frame is
  flat, and the existing v4 `gpumirror.bin` two-eye lists do the eyes on the
  PICA200 when a stamped home frame and a slider above 16 arrive. The ARM9
  two-eye copy remains only as the fallback when the mirror stops.
- **Diagnostics:** `[stereo] MCU 3D slider N -> M ...`, `[stereo] 3D LED ...`,
  `[stereo] switch: PDC0 3D/2D, path, ack and mode-frame counts`, and
  `[stereo] active path: ...` every 30 s. A hung mirror/PDC switch is logged
  as a FAILED switch, never as a lock-up: every wait is bounded and serves
  md0 with `blksrv_poll()`. The 3D LED (MCU reg 0x2C) follows the barrier.
- **GPU lists (v4):** `tools/ios3ds-pica/mirror/genmirror.c` emits, per
  sampling variant, a flat list and a two-eye list. Each eye draws a baseline
  quad (unshifted, all pixels) plus three passes alpha-tested (`==` on the
  stamp read through texture unit 1) for BG/FRONT/ALERT; unit 0's texture
  matrix is patched per pass from a precomputed shift table for the slider's
  k (1..4). The left eye renders into GM_RT_TOP_PA, the right eye into the
  upload buffer (free after the texture copy); both are read out into the
  two LCD framebuffer sets the PDC0 preset points at. The texture's unused
  columns 320..327 are replicated from x=319 after every upload
  (`pad_edge()`), so a shifted sample past the iOS edge clamps like the ARM9
  copy instead of reading the padding. The bottom screen stays flat.
- **Hand-over:** `gmbox_t` gained `stereo_k`, `st_frames`, `flat_frames`,
  `mirror_busy`, `stereo_ok`. `gm_set_shift()` publishes k without a PDC
  switch; `gm_set_stereo()` requests the 2D/3D preset and waits for both the
  ack and one GPU frame in the new mode before the ARM9 opens the barrier.
  Going back to 2D restores the preset before clearing the barrier. An old
  blob without stereo lists pauses the mirror while the ARM9 builds the eyes.
  All ARM11 cache ranges were widened to cover the whole mailbox (256 B):
  line 5 is shared and the new counters live there.
- **Sizes:** the stereo lists, shift patching and edge padding add about
  1.6 KiB to the GPU part; the camera allocator from sections 107/108 already
  took most of its remaining room. After rebasing on that work the final
  build is main shim 24268/24320 and GPU part 32624/32768 (144 bytes below
  the limit; compiling `pica_mirror.c` at -Os was rejected: at -Os the
  constant divisions pull `__aeabi_idiv` into the *main* part and overflow
  it, so mkboot keeps `pica_mirror.c` at -O2 and `pica.c` at -Os with
  `PICA_HOT`). `gpumirror.bin` is 32336/32768 bytes (flat lists 2240 + 2240,
  stereo lists 7360 + 7360, the rest vertex arrays, shift/pad tables and
  diagnostics). The new version-stamp rule made `make -C boot` choose the
  phony stamp as its default goal; `.DEFAULT_GOAL := all` restores the
  firmware build.
- **Version stamp:** `boot/Makefile` stamps the compiled `IOS3DS_VERSION`
  into `build/boot/.version` and rebuilds both stubs' `main.c.o` when it
  changes. Hardware run 3 showed a loader banner of `g64b0cd8` with an ARM11
  stub still saying `g89178de`: incremental builds changed VERSION without
  recompiling the untouched ARM11 object.
- **Test:** `make test` passes; `test_mirror` now checks the GPU two-eye
  output against the class/delta rule for all k=1..4 on a stamped picture
  (9330 sampled eye pixels per k) and still checks the flat path byte for
  byte against `boot/arm9/display.c`. `test_stereo_copy` runs the real ARM9
  copy through `display_stereo_probe()` for flag off, slider up/down, apps
  and a partial class-map update.
- **Frame times (QEMU, `--icount 2`, fresh images, `[monitor] CPU screen copy`
  averages; QEMU does not model ARM9 bus speed):**

  | configuration | before (ARM9 copy) | after (GPU mirror, frame avg) |
  |---|---|---|
  | `stereo3d=0` | 13.6 ms (33 copies/30 s, before the mirror took over) | 306 frames/30 s, 13.2 ms |
  | `stereo3d=1`, slider 0 | 8.7 ms then 7.3 ms (mirror forced off, ARM9 copied every frame) | 327 frames/30 s, 13.7 ms (flat list, k=0) |
  | `stereo3d=1`, slider 255 | 9.4 ms then 8.2 ms with the cached ARM9 two-eye copy (§106); 120-186 ms before that cache (§90) | 312 frames/30 s, 14.3 ms (two-eye list, k=4) |

  So the slider-down path is now the flag-off path (13.2 vs 13.7 ms in the
  emulator, within run-to-run noise), and 3D adds about 1 ms per GPU frame,
  against 120-186 ms for the original uncached ARM9 two-eye copy. The
  `[stereo] active path` line in each run names the side that built the eyes.
- **Soaks (fresh `build/sd.img` each, private QEMU, `--cycles 5 --icount 2`):**
  `stereo3d=0`, `stereo3d=1` slider 0 and `stereo3d=1` slider 255 all ended
  `Result: PASS`, with 0 panic/ARM11-stall/md0/crash/GPU-timeout findings.
  After rebasing onto sections 107/108 the same three fresh-image soaks were
  repeated on the final tree (`build/emu/soak-final-*`) and all three printed
  `Result: PASS` again; the table above uses the earlier, longer steady-state
  windows (the final runs were shorter under shared host load).
  The 3D run logged 30 PDC0/barrier on-off pairs as apps opened and the home
  screen returned, and its spot check was skipped while the eyes were offset.
  A separate 300 s toggle run set the QOM slider 0→255→0→255 at 180/225/255 s:
  each move gave one `[stereo] switch`, one `[stereo] 3D LED` line and one
  `[monitor] stereo 3D: ... barrier on/off` line, with flat screenshots after
  the 2D switches and anaglyph screenshots after the 3D switches
  (`build/emu/stereo-toggle/`). No tearing or stale eyes were visible.
- **Host:** `make test` passes; `test_mirror` checks the two-eye GPU output
  against the ARM9's class/delta rule for k=1..4 (9330 sampled eye pixels per
  k, 0 differences) and keeps the byte-for-byte flat comparison; host blob
  32336 bytes. Raw `picasim` alpha test and the second texture unit are used
  by the same list in QEMU, so the model and the machine agree.

## 110. Old 3DS frame drops: [perf] frame histogram, emu/perf.py, and the CA render-thread trim

Design: docs/perf.md. Hardware input: `issue-reports/run2-perf.txt`
(stereo3d=0, GPU mirror on) and `run3-perf.txt` (stereo3d=1, CPU copy; the
229-301 ms/frame there is agent/stereogpu's area). QEMU: `emu/perf.py --runs
3 --safari --icount 2` on a fresh `build/sd.img`.

- **Report:** an Old 3DS (128 MiB) still drops frames; the home screen is
  fine but opening a folder and opening a page in Safari are slow.
- **Hardware numbers (run2).** The ARM9 CPU copy of the framebuffer took
  **347.2 ms/frame** (25 copies in 30 s) until the GPU mirror's self-test
  passed, then the mirror runs **18.1 fps** (600 frames in 33 s) at
  `upload 9388 us + draw 3086 us + read-out 6475 us` ≈ 19 ms GPU/frame, with
  **764 busy skips** in 33 s (CoreAnimation holding the GPU). Free memory
  falls 99.3 → 26 MiB over the run (wired 17.9 → 33.3 MiB, the 12 MiB arena
  plus the kernel); one `[pica] present: … not physically contiguous` →
  the arena copy (re-log 78/79), no process-wide read-back.
- **QEMU, where a folder open goes** (`diag=1 ioprof=1 log_ts=1`, folder
  opened at ~guest 30 s): md0 is quiet by then (`[io] t=26.8 … rd 96 KiB …
  arm9 3 ms`) and `[mem 3]` shows 53 MiB free / 0 pageouts - so the folder
  open is **CPU** (SpringBoard's animation + the CA render thread), not I/O
  or memory. The `[smp]` sampler cannot dump SpringBoard's threads (its task
  port is unreachable), so the per-frame cost is the new histogram.
- **`[perf]` frame histogram (`tools/ios3ds-gl/ca.c`):** `ca_frame()` now
  measures the wall interval between CA swaps on the render thread (mach
  time) and, once per second while frames flow, prints one line -
  `[perf] CA: 42 frames in 1037 ms; frame ms min/avg/max 4/24/281;
  <17:21 17-33:18 33-66:1 66-100:1 >=100:1`. It prints only during an
  animation, never on an idle screen, so a hardware log shows second by
  second where a folder open or a scroll drops frames (next to the existing
  `[gpu] mirror: … fps` display line). A folder open in QEMU is ~40 frames
  in ~1 s at 24-26 ms/frame with a couple of drops; the `[gl] CA:` line puts
  the render thread at 14-24 ms/frame (2-3 texture uploads/frame on the
  first frames, ~1.2 after, presents 1.2-1.3/frame, ~0.2 read-backs/frame).
- **Benchmark (`emu/perf.py`):** boots, waits for a stable home screen and
  the `[sbwarm]` warm-up, taps the Utilities folder (120,237) and Home
  `--runs` times, then optionally a Safari cold launch (dock 199,440);
  writes `build/emu/perf/report.md`+`report.json` with the wall times and
  the `[perf]`/`[gl] CA:` lines. Full page loads over the Wi-Fi model stay in
  `emu/safari_test.py` (page-load time + memory).
- **CA render-thread trim (`tools/ios3ds-gl/pica_be.c`, `ca.c`):** the
  swizzle pass's command list and its six vertices are built once per render
  target (they are identical every frame; keyed on the target's buffers,
  dimensions and opacity) and resubmitted each frame instead of re-encoded
  on every present; `ca_frame()` no longer calls `thread_info` (three Mach
  syscalls) on every swap - the render-thread CPU is sampled only on the
  reporting frames. Host test (`make -C tools/ios3ds-gl host-test`) still
  passes, including the CA compositor gate's present check.
- **Validation:** `make test` green; `make userland` + fresh `make sdimg`
  clean; the required fresh-image soak `rm -f build/sd.img && make sdimg &&
  python3 emu/soak.py --cycles 5 --icount 2` printed **Result: PASS**.
- **Still needs hardware:** a card run with `diag=1` to see the `[perf] CA:`
  histogram during a real folder open/close and a Safari scroll on the Old
  3DS, next to `[gpu] mirror: … fps`, to decide whether a dropped frame is
  the compositor or the mirror; and `[gl] CA: … render thread CPU …` against
  re-log 78's 13.5-16 ms (GPU present) / 31-37 ms (read-back) hardware
  figures. The dominant Old 3DS cost - the mirror's ~20 ms/frame and the GPU
  split with CoreAnimation - is GPU-side and not fixed here.

## 111. iOS 3.1.3 root mount: the patch was on the wrong memdev helper (2026-11-24)

The 7E18 bring-up stopped in `BSD root: md0` with **0** md0 requests served by
the ARM9. The earlier guess was a Darwin 10.0 `buf_t` ABI change inside the
shim. It was not: the kernel patch was on the wrong function.

- **Trace.** A GDB remote-protocol client (`emu/gdb/rsp.py`, from `build/`,
  not committed) set a hardware breakpoint at the old 7E18 patch site
  `0xc0073f94` and never hit it. A breakpoint at `0xc0073b64` hit once per root
  mount retry (`mdevopen`), and a runtime dump of `bdevsw[2]` showed
  `0xc0074021` as its strategy entry. The symtab had no name for either
  function; the `bdevsw` pointer identified the real one.
- **The real strategy.** `0xc0074020` calls `_buf_count` (`0xc007950c`) and
  `_buf_biodone` (`0xc007ac6c`) just like 8C148's `0x80072bf8`, and it takes
  `buf_t` in r0 (no Darwin 10 argument shuffle). The function at
  `0xc0073f94` is a different `(dev,bp)` memdev helper that `bdevsw` never
  calls. `kernel/patchsets/3.1.3.json` now patches `0xc0074020` with the
  original Thumb `ldr r3,[pc]; bx r3; .word shim+0x20` stub.
- **Result.** `make sdimg IOS=3.1.3` + `emu/run.py --icount 2`:
  `/dev/md0 on / (hfs, local, noatime)`, launchd, fsck, mDNSResponder and the
  display come up; 2,251 md0 requests / 49 MiB / 0 errors by t=70 s, no panic
  or stall. Screenshots are still black (no SpringBoard): the next blocker is
  userland activation, as on 2.2.1. 4.2.1 files were not touched; only a
  3.1.3 patch recipe changed.

### 114 (same section, continued): 3.1.3 offline activation

- **[V]** 7E18's dyld cache has no 8C148 offsets, but its liblockdown symbols
  do resolve. `rootfs/overlay-3.1.3.json` now carries five file patches:
  the 8C148 `lockdown_copy_value` stub relocated to `0x3176a008`, a
  240-byte key/value table in a verified zero gap of liblockdown's `__TEXT`
  (`0x31767600`), and stubs for `lockdown_connect`, `lockdown_checkin` and
  `lockdown_copy_identity` (no lockdownd on the 3DS). The table's eight
  `__CFString` objects use 7E18's `___CFStringClass` isa `0x3852aa08`,
  confirmed by reading an existing CFString in the cache.
- **[V]** With these patches 3.1.3 still stops on the boot spinner;
  SpringBoard writes nothing to the console even with its plist's stdout and
  stderr redirected. A GDB breakpoint at the relocated `lockdown_copy_value`
  hit only three unrelated keys (TrustedHostAttached, VolumeLimit,
  ShutterClick), so activation state is not the observed blocker. The next
  step is a SpringBoard-side trace (its `main`/activation path, or a 3.x
  applog) rather than more liblockdown table work. 4.2.1/4.1 code paths are
  untouched; the 2.2.1 interposer is a separate version-only tool.

### 114 (same section, continued): 2.2.1 liblockdown, still no SpringBoard

- **[V]** The 4.2.1 `DYLD_INSERT_LIBRARIES` approach does not transfer to 2.x:
  a dylib built with the 4.2.1 stub toolchain produced
  `dyld: could not load inserted library` on every SpringBoard restart. An
  unsigned copy instead produced `CODESIGNING: vm_fault_enter ... INVALID PAGE`.
  The inserted-library source was therefore removed, not shipped.
- **[V]** The working path is to patch `/usr/lib/liblockdown.dylib` itself:
  `lockdown_connect` → 1, `lockdown_copy_value` → an ARM table lookup over two
  entries (ActivationState = an inline `Activated` CFString, BrickState =
  CFBooleanFalse), with the table in a verified zero gap of `__TEXT`. Because
  2.x checks each dylib page, `mkrootfs.py` re-signs patched files listed in
  the recipe's `resign` map (`ldid -Hsha1 -S`); only the 2.2.1 overlay sets it.
  Boot now has no `CODESIGNING` faults, but still no SpringBoard output and a
  black screen: activation alone is not the 2.x blocker.

### 114 (same section, continued): default build stays byte-identical

- **[V]** `make userland` + a fresh `make sdimg` produced the same six 4.2.1
  boot-file sha256 values as before any of this version work:
  `kernel.bin 505f451a…`, `devtree.bin 25b1bd23…`, `fakeio.bin e5fecd66…`,
  `shim.bin 25b7721c…`, `gpu.bin 94909f36…`, `boot.cfg 414fa594…`.
  All version-specific changes are in patch sets/overlays and one optional
  `resign` step that only the 2.2.1 recipe sets.
- **[V]** The exact fresh-image sequence `rm -f build/sd.img && make sdimg &&
  python3 emu/soak.py --cycles 5 --icount 2` printed **Result: PASS**
  (boot to SpringBoard + input 70 s, five app/home cycles, 0 panics, stalls,
  md0/log/crash/watchdog/GPU failures). The 4.1 soak and the two older-version
  runs are recorded below as they finish.

### 114 (same section, continued): 4.1 soak PASS after a harness fix

- **[V]** The first two 4.1 fresh-image soaks printed `Result: FAIL` with
  `not home after Home: {'stuck': [0..4]}` and a ~70 home diff. The cycle
  screenshots showed a normal 4.1 home screen, but `home_ref.png` was the
  green-on-black M1 boot console: on a loaded host the console stayed lit
  past the input daemon's connect, so the harness's "stable and bright"
  reference test accepted it. This is a harness bug, not a 4.1 regression.
- **[V]** `emu/soak.py` now rejects green-dominant console frames when picking
  (or re-acquiring) the home reference. A third fresh image then printed
  **Result: PASS**: five cycles, `not home after Home: never`, 0 panics,
  stalls, md0/log/crash/watchdog/GPU failures. The change affects only the
  harness; no firmware or userland payload was altered.

### 114 (same section, final hash check after the rebase)

- **[V]** Rebasing onto current `origin/main` brought in the stereo/GPU changes
  from `agent/stereogpu` (`dd54b1a`), which legitimately change `shim.bin`,
  `gpu.bin` and `boot.cfg`: after the rebase they are `c9791ac3…`, `ac5ea097…`
  and `8d4079cf…` instead of the pre-rebase `25b7721c…`, `94909f36…` and
  `414fa594…`. `kernel.bin`, `devtree.bin` and `fakeio.bin` are unchanged.
- **[V]** To prove those six files are still *our* 4.2.1 payload unchanged, a
  space-free `git archive origin/main` tree generated all six from the same
  staged firmware: every hash matched the branch build byte for byte. None of
  this task's files are on the 4.2.1 boot-file path (only patch sets/overlays
  for older versions, a recipe-gated `resign` step, docs and the soak harness).
- **[V]** Final fresh-image 4.2.1 soak after the rebase: **Result: PASS**,
  five cycles, 70 s to SpringBoard + input, no failures. Final 4.1 soak:
  **Result: PASS**, five cycles, 60 s to SpringBoard + input, no failures.
  `make test` passes after the final rebase.
## 112. Safari's "Cannot allocate memory": free pages, not jetsam; lean caches and a smaller Old-3DS arena (2026-11-24)

**Reports.** Old 3DS (128 MiB), `diag=1`: Safari shows "Cannot allocate memory"
(NSPOSIXErrorDomain 12) on bigger pages; a retry works. No zone/jetsam cause
was found in re-log 104. The git-excluded `issue-reports/mem-lines.txt` has
`[mem]` lines every few seconds from two console runs (run2: 61 samples, run3:
6; no per-process lines in that file).

**What those console numbers actually say.** `vm_stat`'s `free` *includes*
speculative file pages (`spec`): run2 boots at 101,704 KiB free / 42,304 KiB
speculative / 17,928 KiB wired / level 85, then settles at 25,276-40,820 KiB
free with 23,328-54,020 KiB of it speculative. Subtracting speculative -
now printed as `free_nospec` - leaves **1,948-3,856 KiB** of immediately
usable free memory while the browser is warm, with wired 31,420-33,672 KiB
(12 MiB of it the GPU arena) and `memorystatus_level` 60. That is why the
process can get ENOMEM from a large allocation while no jetsam kill happens
and the headline "free" figure still looks like 25 MiB. iOS 4.2.1 has no VM
compressor, so there are no compressed pages to count; the new `[mem] VM`
line says so explicitly.

**QEMU reproduction attempts (`--icount 2`, Old model).** The harness
`emu/safari_test.py` can now set `--settle` and `--port`. Tried: 65 MB HTML
with six 65 KB JPEGs, 8 × 1024² JPEGs, a 2-million-element JS array, 26
generated pages in one Safari (JS + 6 images each), and `mem_size` lowered to
96 MiB (2 MiB free, level 16). No "Cannot allocate memory" alert appeared;
one run with a 69 MB Safari process was killed with signal 9 instead (a
jetsam kill, a different failure). Baseline for this pass: four heavy pages
(`js.html`, `manyhuge.html`, `manybig.html`, `hugeimg.html`) loaded 4/4 in
618 s, boot `[mem 1]` 102,832 KiB free / level 85, after settle 52,620 KiB
free (`free_nospec` 1,188 KiB) / level 64, Safari peak 19,488 KiB, end
47,428 KiB free / level 55, wired 33,800 KiB. This is still not a hardware
reproduction of the alert; the console numbers above are the evidence that
physical free pages, not jetsam or a named zone, run out first.

**Fix (memory only).**
1. `rootfs/files/memory/com.apple.mobilesafari.plist` (installed via
   `rootfs/recipe.json`): WebPreferences is read from the app's standard
   defaults (WebKit's `WebPreferences.mm` calls
   `NSUserDefaults standardUserDefaults`). Set
   `WebKitCacheModelPreferenceKey=1` (`WebCacheModelDocumentViewer`, the
   smallest WebCore cache footprint), `WebKitUsesPageCachePreferenceKey=0`,
   `WebKitApplicationCacheTotalQuota=0` and a 60 s back-forward cache
   expiry. This keeps Safari's decoded images, page cache and app cache from
   accumulating across pages. Trade-off: going Back may re-load a page; the
   performance worker's Safari changes are separate (see overlap below).
2. `kernel/shim/pica.c`: the wired GPU arena is now **10 MiB on Old models**
   (skip the 12 MiB first size when `cfg_n3ds` lacks `N3RES_NEW`), 12 MiB on
   New models, with the same 8/4/2 MiB fallbacks. The camera's 450 KiB frame
   block is still reserved inside the arena (re-log 108) and the `>= 6 MiB`
   gate still applies. This gives every app 2 MiB more wired-free RAM, not
   just Safari. 10 MiB is above the 8 MiB size that fragmented in run16
   (704 KiB texture found 290 KiB free in 90 blocks), but it is **not yet
   confirmed on a console**; the `[pica] arena N KiB` log line must be
   checked, as must a live camera preview and a non-black home screen.
3. Diagnostics: `ios3ds-sample -mem` prints `free_nospec`, a
   `[mem N] VM: compressed n/a ...` note on the first report, and a
   `[mem N] VA MiB:` top-process list next to the resident list.
   `ios3ds-applog.dylib` interposes `mmap`, `vm_allocate` and
   `mach_vm_allocate`; failures >= 64 KiB are logged as
   `[applog] <proc> ENOMEM <call> size N arg K caller 0xPC` (max 16 per
   process; sandboxed Safari writes them to
   `/private/var/tmp/ios3ds-applog.txt`). These lines name the failing call
   and request size on the next hardware run; the kernel hook itself is not
   feasible: the main shim has ~50 bytes and the GPU part ~100 bytes free,
   and the 4.2.1 kernelcache has no exported allocation symbols in our stub.
   Three new symbols were added to `scripts/ios-sdk-symbols.json` so CI's
   stub SDK links the interposer.

**Before/after (QEMU, Old, `--icount 2`, four heavy pages, same images and
timing; each run a fresh image).** All memory figures are KiB (÷1024 = MiB):

| | before (12 MiB arena, stock WebKit defaults) | after (10 MiB arena, lean WebKit defaults) |
|---|---|---|
| `[pica] arena` | 12,288 | **10,240** (-2,048) |
| free after boot (`[mem 1]`) | 102,832 / level 85 | 103,104 / level 85 |
| wired after boot | 17,756 | 17,720 |
| free after 45 s settle (`[mem 2]`) | 52,620 / level 64 | **53,224 / level 66** |
| wired after settle | 33,408 | **31,336** (-2,072) |
| `free_nospec` after settle | 1,188 | 1,748 |
| MobileSafari resident, end of run | 19,488 | 18,976 (peak sample before its jetsam kill: 72,940, see below) |
| free at end | 47,428 / level 55 | **47,776 / level 55** |
| wired at end | 33,800 | **31,656** (-2,144) |
| pages loaded | 4/4, one jetsam kill + restart on page 1 | 4/4, one jetsam kill + restart on page 1 |

The two MiB saved are physical wired pages, visible in every `[mem]` sample;
the Safari footprint in this short run is unchanged within noise. Both runs
killed the 2-million-string JS page with signal 9 (jetsam), then restarted
Safari and loaded all four pages: QEMU still does not produce the hardware
alert. The new ENOMEM interposer logged nothing in these runs - no
`mmap`/`vm_allocate` call failed; the kill is a different path.

A 26-page stress run (the same generated pages as re-log 75's six-page run)
compares the longer session against the previous 26-page QEMU run
(`build/emu-mem6`, before this pass):

| | before (26 pages) | after (26 pages) |
|---|---|---|
| pages loaded | 26/26 | 26/26 |
| MobileSafari peak resident | 43,788 KiB | 44,912 KiB (no measured reduction) |
| minimum free during the run | 2,608 KiB / level 39 | **11,564 KiB / level 40** |
| free at end | 6,488 KiB / level 39 | **12,600 KiB / level 40** |
| wired at end | 31,352 KiB | **29,316 KiB** (-2,036) |

So the prefs did **not** measurably shrink Safari's heap in QEMU; the useful
gain is the 2 MiB of wired arena per app (and the additional slack in the
file-cache / speculative balance). The plist stays as a low-risk guard for
long sessions, but it is not claimed as a fix for the alert.

**Validation:** `make test` green (after adding the three imported symbols to
`scripts/ios-sdk-symbols.json`); `make userland` + fresh `make sdimg` shows
`[pica] arena 10240 KiB`, `[cam] reserved 3 x 150 KiB in the GPU arena`,
`[mkrootfs] added ... com.apple.mobilesafari.plist`, GPU shim 32,656/32,768
bytes. The required fresh-image soak `rm -f build/sd.img && make sdimg &&
python3 emu/soak.py --cycles 5 --icount 2` printed **Result: PASS** (boot 90 s,
5 cycles, 0 panics, 0 ARM11 stalls, 0 md0 errors, 0 crashes, 0 jetsam, 0 GPU
CPU fallbacks, 0 camera failures; the one "SpringBoard restart" is the
boot-time HID connection). The GPU self-test and scene check passed with the
10 MiB arena.

**Still needs hardware:** reflash the Old 3DS, open Safari with `diag=1` and
repeat the big page that shows the alert. The log must contain `[pica] arena
10240 KiB`, `[cam] reserved 3 x 150 KiB in the GPU arena`, the new
`[mem] free_nospec / VA` lines and, if the alert returns, the
`[applog] MobileSafari ENOMEM ...` line and the existing load-error domain /
code. That line will finally say whether JSC's heap reservation, an image
decode mapping, or another call is failing. Check the camera preview and a
saved photo after the arena shrink; if textures fall back to the CPU
(`[gl] CA: ... CPU from now on`), restore 12 MiB on Old models in
`kernel/shim/pica.c`.

**Overlap:** agent/perf owns Safari page speed and the CA render-thread trim;
the WebPreferences plist here is deliberately limited to cache/back-forward
memory, not animation or rendering settings. The arena size is a GPU-side
memory change; no display or stereo files were touched.

## 113. Old 3DS frame drops round 2: damage-tracked mirror, present-driven scheduling, faster uploads

Design: docs/perf.md. Hardware: `issue-reports/run2-perf.txt` (stereo3d=0):
`[gpu] PICA200: … every 2. VBlank`, `25 copies in 30 s, 347.2 ms each` before
the self-test passed, then `600 frames in 33 s (18.1 fps); upload 9388 us,
draw 3086 us, read-out 6475 us; frame avg 20661 us … 764 busy skips, 0
timeouts`, and `11 copies in 33 s, 75.6 ms each` when GL held the GPU.
QEMU: `emu/perf.py --runs 3/8 --icount 2` on a fresh `build/sd.img`.

### Space first
The shim GPU part was **32624/32768** bytes (re-log 109). The staged
`[gpu-diag]` bring-up diagnostics (`tools/ios3ds-pica/src/pica_diag.c`) are
no longer linked into the shim (`scripts/mkboot.py`): they already served
their purpose (runs 12/13 pinned the NPOT-texture and GX-clock facts), and
the source + the picasim host tests stay, so `gpu_diag=1` in boot.cfg is now
a no-op. That freed 6.7 KiB.

`make shim` on the final rebased tree: main **24268/24320**, GPU part
**26304/32768** bytes (after all the changes below; the arena-size work from
section 112 adds 32 bytes).

### Mirror: skip unchanged frames (damage tracking)
A GL present/scatter now sets `m_dirty` (`kernel/shim/pica.c`); the
VBlank-driven mirror only does GPU work when `m_dirty` is set and skips
unchanged frames by incrementing a new `gmbox_t.idle_frames` counter (line 6,
reusing a reserved word). The ARM9's `gm_live()` counts `idle_frames` as
liveness too, so an idle screen no longer falls back to the 75 ms ARM9 CPU
copy. On an idle home screen the mirror's ~19 ms/frame GPU cost drops to 0,
and the LCDs simply keep the last picture. `gm_report()` now prints
`[gpu] mirror: N idle VBlanks in 30 s (no new CA frame, no GPU work)` for such
a window, instead of no mirror line at all; the spot check labels the last
picture as the GPU copy while idle frames keep it alive. A line-0 `seq` change (screen
mode, `stereo_k`, `want`) or a PDC 2D/3D switch (`pica_stereo.c`) also sets
`m_dirty`, so the eyes are rebuilt even when the CA frame did not move (the
3D slider on an idle home screen).

### Mirror: CPU-written surfaces also mark the frame dirty
`m_dirty` was only set by `PICA_OP_PRESENT`/`PICA_OP_SCATTER`. A display
target that takes the CPU read-back path (every frame with `stereo3d=1`, so
the alpha stamp reaches `/vram`, and any surface after a refused present
switched the process to read-backs) writes straight into the IOSurface from
userland: the mirror would have kept showing an older frame. Protocol 5 adds
`PICA_OP_DIRTY` (`pica_dev_t.dirty()`); `sync_target()` calls it after a
successful `read_back()` into a surface target, and the shim just sets
`m_dirty`. Host test: `test_pica` calls the new op; `make test` green.
GPU part +16 bytes.

### Mirror: schedule right after a CA present
A frame the VBlank skipped because GL held the GPU (`m_pending`) now starts
in thread context right after that GL request finishes (`m_maybe_start` in
`shim_gpu_thread_poll`), instead of waiting for the next interrupt. The
frame stays VBlank-cadenced (`m_pending` is only set on a due VBlank), so
the mirror cannot oversubscribe the GPU. The busy-skips now only count a
present that collided, not a lost display refresh.

### Mirror self-test: pixel-wise compare (shorter 347 ms boot phase)
`pm_selftest` compared 518400 output bytes one at a time, re-deriving the
source pixel through `gm_map()` (two integer divisions) three times per
byte. It now compares one 24-bit BGR8 pixel at a time (one `gm_map` per
pixel). The self-test runs once at boot while the ARM9 still copies at
~347 ms/frame, so this shortens that phase. The host test still reports the
exact same `0 of 518400 bytes differ` and per-variant byte counts.

### CA render thread: faster texture tiling
`pica_tex_upload`'s common case (every CA texture: BGRA8888 into an RGBA8
texel) did four byte loads/stores per texel; it now does one 32-bit load, a
rotate-left-by-8 and one 32-bit store, falling back to the byte path when
either buffer is not 32-bit aligned. Host test: byte-identical.

### Before / after (`emu/perf.py`, fresh image each)

| metric | before (round 1) | after (final tree, fresh image) |
|---|---|---|
| boot to SpringBoard + input | 70 s | 70 s |
| folder open (wall s, 3 runs) | 12.2 / 14.2 / 12.2 | **6.1 / 8.1 / 8.1** (host-load noisy) |
| folder close (wall s, 3 runs) | 12.2 / 12.2 / 12.2 | 12.2 / 12.4 / 14.2 |
| Safari cold launch (s) | not measured | 4.0 |
| `[perf] CA:` frames per ~1 s window | 40–43, avg 23–24 ms | 43–47, avg 22–23 ms (same pace in QEMU) |
| `[gl] CA:` render thread CPU | 14–24 ms/frame (re-log 110, range) | **16066 us**, 3.0 lists, 1.1 presents, 0.1 read-backs, 1.9 uploads (112 KiB) in the 8-run window; a later 301-frame window read 7633 us with 0.5 uploads (30 KiB) |

The `[perf]` folder-open histogram keeps the same ~23–24 ms average because
QEMU's folder open is vsync/CA-paced, and the render thread (7.6–16.1 ms) is
already under that budget; the tiling win shows as fewer uploads and guest
instructions per upload, not as a faster histogram. The wall times are host-
load noisy (the same image has produced 4–16 s opens on this PC). On the Old
3DS the render thread and the 19 ms mirror are serialised on the GPU, which is
why the hardware `[perf] CA:` average (54 ms in run2) tracks the mirror's
18 fps: the fixes that matter there are the damage tracking and the
scheduling above, and they are GPU-side, so QEMU cannot show them (QEMU's GPU
model finishes every operation instantly). The soak log does show the damage
tracking working: a 30 s window with **901 idle VBlanks and no GPU work**.

### Validation
`make test` green (6698 + 59 + 2469 pica checks, gl host-test, wifi, gps,
input, blkcache); `make shim` clean: main **24268/24320**, GPU part
**26304/32768** bytes (PICA_OP_DIRTY added 16 bytes; the section-112 arena
change adds another 32 on the rebased tree).
`make userland` and fresh `make sdimg` clean.

Fresh-image `emu/soak.py --cycles 5 --icount 2` soaks on the final rebased
tree (`build/emu/soak-rebased-off/` and `soak-rebased-on/`):
- `stereo3d=0` (shared QEMU): **Result: PASS**, boot 70 s, 0
  panic/stall/md0/crash/GPU-timeout findings, no "not home after Home" or
  failed lock rounds. The 30 s log shows the damage tracker explicitly:
  `[gpu] mirror: 901 idle VBlanks in 30 s (no new CA frame, no GPU work)`
  next to a window with 5 GPU frames.
- `stereo3d=1`, `--slider3d 255` (private QEMU with the slider property):
  **Result: PASS**, boot 70 s, no guest failures or failed lock rounds; two
  cycles needed the harness's known "second Home" recovery for Spotlight.
  The log has `[stereo] switch: PDC0 3D, PICA200 two-eye mirror`,
  `[stereo] 3D LED on`, `[monitor] stereo 3D: both eyes … parallax barrier on`
  and the matching 2D switches when an app opens; `active path: PICA200
  two-eye mirror (k=4)`. No "reading surfaces back through the CPU" or GPU
  present-check failure: protocol 5's dirty notification from the stereo
  read-back path works.

Both harnesses had to be repaired for a loaded host: the soak/perf home
reference used to accept the still-lit boot console as "home" (greyscale
stddev ~92 against ~54 for the icons), which made every cycle look stuck.
`emu/soak.py` now waits for the `[sbwarm]` marker and `emu/perf.py` rejects
the console's texture; with that the flag-off soak and all perf runs recovered
in zero cycles (the flag-on soak only needed two Spotlight dismissals). The
final perf numbers above are in `build/emu/perf-rebased/`.

### Needs hardware
A card run with `diag=1` on the Old 3DS to confirm:
- `[gpu] mirror: … idle VBlanks …` on an idle home screen (damage tracking)
  and fewer busy skips during a folder open/scroll, with the `[perf] CA:`
  histogram avg dropping from 54 ms toward 30 ms;
- `[gpu] display mirror ACTIVE` and the spot check still pass; the stereo
  path (re-log 109) still opens the barrier when the slider moves on an idle
  home screen (`[stereo] switch: PDC0 3D`, one stereo frame before the
  barrier). With `stereo3d=1`, or after a refused present switches a process
  to CPU read-backs, the display must keep changing every CA frame: that is
  the new `PICA_OP_DIRTY` path, which QEMU cannot timing-test;
- the remaining mirror per-frame cost (upload 9388 + read-out 6475 us) is
  still there: the upload is linear→tiled + a texture re-stride (NPOT
  textures are broken, run 13), and the read-out writes both framebuffer sets
  to avoid tearing. Cutting those needs a console decision (RGB565 texture =
  banding; one-set read-out = possible tearing), so they were left alone.
## 114. iOS 3.1.3: SpringBoard starts, stops at the activation screen (2026-11-24)

Continues §111. The root mount has been solid since the previous round; this
round traced the userland stop point with GDB and dyld logging, all in this
worktree's `build/` (no Apple files are committed).

- **SpringBoard really was launching.** A GDB remote-protocol trace of
  `_execve`/`_proc_exit` (kernel symbols from 7E18's `LC_SYMTAB`) showed
  launchd at t≈9 s, fsck/mount/mount_hfs helpers exiting at t≈22–25 s, and
  processes named `SpringBoard` exiting with status 1 at t≈64 s and t≈72 s;
  launchd then stopped retrying. Before that the boot spinner was the only
  screen, which is why earlier rounds concluded "no SpringBoard".
- **Crash reports:** `rootfs/overlay-3.1.3.json` now asks `mkrootfs.py` for the
  optional `mkdirs` step (new key, ignored by 4.2.1) to create
  `/private/var/mobile/Library/Logs/CrashReporter`. No reports appeared: the
  exits are normal `exit(1)` paths, not crashes. `DYLD_PRINT_LIBRARIES=1` in
  SpringBoard's launchd plist (3.1.3 overlay only) showed dyld loading the
  complete framework set through `ArtworkCache.framework` and then stopping,
  with no dyld error.
- **Missing lockdown stubs were the reason SpringBoard survived.** SpringBoard
  imports `lockdown_disconnect`, `lockdown_remove_value` and
  `lockdown_set_value` in addition to the four functions the previous round
  had stubbed; the fake handle from `lockdown_connect` was passed to them.
  Their 7E18 offsets (`0x176a260`, `0x176a1c4`, `0x176a210`) were located from
  the 8C148 context and verified byte-for-byte against the cache, then stubbed
  to return 0. With those three extra patches SpringBoard no longer exits: it
  stays up, the heartbeat shows no kernel idle stall, and the screen changes
  from the spinner to the **"Connect to iTunes" activation screen**. The
  table's `BrickState` value was also corrected to 7E18's real
  `kCFBooleanFalse` (`0x38521ff8`, verified via the CFBoolean array at that
  address) from an out-of-cache pointer; this alone did not change the screen.
  Writing stock-style activation record/plist files was tried and reverted:
  it did not change the screen either.
- **Remaining blocker:** activation. A GDB breakpoint at the relocated
  `lockdown_copy_value` stub (`0x3176a008`) is not hit in this build, so the
  offline activation table is not the code path that decides the screen; the
  decision is inside SpringBoard's `SBLockdownManager`. The next step is
  its Objective-C method implementation, not more liblockdown table work.
- **4.x is unchanged:** `mkdirs` is a new optional recipe key and every other
  edit is in `rootfs/overlay-3.1.3.json` (plus crash dirs in the 2.2.1
  overlay). No 4.2.1 boot-file or shim code changed.

### 114. 2.2.1: the same lockdown stubs, dyld stops after CoreSurface (2026-11-24)

- **[V]** 2.2.1's SpringBoard imports `lockdown_disconnect`,
  `lockdown_remove_value` and `lockdown_set_value` just like 7E18's. Their
  file offsets in `/usr/lib/liblockdown.dylib` (`0x2be4`, `0x30a8`, `0x30f4`)
  came from that dylib's own `LC_SYMTAB` (VA `0x30893be4` / `0x308940a8` /
  `0x308940f4`, `__TEXT` base `0x30891000`); all three original byte
  sequences were verified before stubbing them to return 0. The existing
  `resign` step re-signs the file as before.
- **[V]** With `DYLD_PRINT_LIBRARIES=1` the 2.2.1 SpringBoard loads
  MultitouchSupport, IOKit, Apple80211, libIOAccessoryManager,
  IOMobileFramebuffer and CoreSurface, then prints nothing further for a
  600 s run (`--icount 2`, screenshots at 180/360/540/590 s are still the M1
  monitor console). The stop point is the dyld load/initialisation of
  `/usr/lib/liblockdown.dylib` (the next `LC_LOAD_DYLIB`) or CoreSurface's
  initialiser; extra `DYLD_PRINT_WARNINGS/INITIALIZERS/BINDINGS` variables
  produced no extra output. A control build without the three new stubs
  stops at exactly the same place, so they neither caused nor fixed this.
- **[G]** Next: a userland GDB trace at 2.x dyld/CoreSurface to see whether
  the signature on the re-signed liblockdown is accepted and where
  CoreSurface blocks; 2.x has no shared cache, so every dylib is a separate
  page-signed file. No 4.x code is involved in this overlay.

### 114. 3.1.3: activation-state experiments (2026-11-24, diagnostic only)

- **[V]** SpringBoard 3.1.3 contains `-[SBLockdownManager _setupActivationState]`
  at Thumb VA `0x6ec30` (IMP from the `__objc` method list in the executable,
  file offset `0x6dc30`). Its disassembly calls `lockdown_connect` then
  `lockdown_copy_value`, checks `CFGetTypeID == CFStringGetTypeID`, and uses
  several `CFEqual` comparisons to choose an activation state 0–7; state 0
  takes the branch that sets the "Connect to iTunes" screen.
- **[V]** Forcing the first `CFEqual` branch (`beq` → `b`, file offset
  `0x6dc8e`, original `0ad0` verified) removes the activation screen and
  leaves SpringBoard alive on the boot spinner. Forcing the state value
  (`mov r2,sl` → `movs r2,#N`, file offset `0x6dc94`) to 1, 2, 4, 5, 6 or 7
  also gives the spinner; 3 gives the activation screen again. A HOME key
  press does not unlock the spinner. These are diagnostic patches, **not**
  shipped in `rootfs/overlay-3.1.3.json`: the spinner is a different stop
  point and the correct activation semantics still need to be established.
- **[V]** Disabling the 18 4.2.1-style daemons that also exist on 7E18 made
  the boot worse (console full of dyld lines, no UI), so that experiment was
  reverted too.

### 114. Final byte-identical check and 4.x soaks after the rebase (2026-11-24)

- **[V]** Rebased `agent/compat5` onto current `origin/main` (conflict in this
  log resolved by keeping both sections). `make test` passes (94 offline tests
  plus host harnesses/stub-symbol check) after updating
  `tests/test_versions.py` for the three extra liblockdown stubs in each old
  overlay. Those overlays and the optional `mkdirs` recipe key are the only
  code changes on this branch; 4.x boot paths are untouched.
- **[V]** A clean `git archive origin/main` tree in a space-free `/tmp/opencode`
  path and this worktree generated the same six 4.2.1 boot files from the same
  staged firmware (SHA-256, origin/main = branch):
  `kernel.bin 505f451a…`, `devtree.bin 25b1bd23…`,
  `fakeio.bin e5fecd66…`, `shim.bin 5e522c4e…`,
  `gpu.bin 439cae2d…`, `boot.cfg 8d4079cf…`. The shim/GPU/boot.cfg values
  reflect the other workers' current main; our branch does not shift them.
- **[V]** The exact fresh-image sequence `rm -f build/sd.img && make sdimg &&
  python3 emu/soak.py --cycles 5 --icount 2` printed **Result: PASS** on
  4.2.1: SpringBoard + input up after 70 s, five app/home cycles, no failures.

- **[V]** 4.1 needed one more harness fix: the first fresh-image soak picked
  the **white-on-black M1 boot console** as its home reference (`home_ref.png`
  was frozen console text with RGB means 48/52/48), so every cycle reported
  `home diff 73.1` and **Result: FAIL**, even though the screenshot after each
  Home was a normal 4.1 home screen. The existing green-dominance test missed
  this white console. `emu/soak.py:is_bootlog()` now also rejects frames with
  almost no saturated pixels in the top screen (measured: 194 sampled
  saturated pixels on the console vs 3128 on the home screen and 13082 in an
  app). A fresh 4.1 image then printed **Result: PASS**: SpringBoard + input
  up after 60 s, five cycles with `home diff 0.0`, no failures. The harness
  change affects no boot files.
- **[V]** The exact documented sequence was rerun on a fresh image after the
  harness fix (`rm -f build/sd.img && make sdimg IOS=4.1 && python3
  emu/soak.py --cycles 5 --icount 2`): **Result: PASS**, SpringBoard + input
  up after 60 s, five cycles, zero failures. The 4.2.1 exact-sequence PASS is
  recorded just above.

## 115. Harness conflict after the perf2 rebase, and older-version bring-up (2026-11-24)

### Harness: restore the warm-up gate beside the console-colour test

- **[V]** The rebase of the six compat5 commits onto `f46d155` (perf2) had a
  conflict in `emu/soak.py:is_bootlog()`. Keeping compat5's whole function also
  dropped perf2's `[sbwarm]` gate (`0272d9d`): the home reference could be
  taken before SpringBoard's icon warm-up finished, i.e. from a lit boot
  console or a partially drawn first frame. That is the combined-harness
  regression; neither side had it alone (perf2 gated on `[sbwarm]`, compat5
  rejected console colours). A loaded host makes the window long enough to
  matter, and a run that waits 300 s for a reference plus up to 180 s per tap
  produces exactly the reported long, silent soak.
- **[V]** Measured on the rebased 4.2.1 guest (unmodified harness, idle host):
  the boot console is monochrome, the lit home screen has RGB means
  44/44/44, greyscale stddev **53.6** and **3264** saturated samples in the
  top-screen crop, against 194/3128 (console/home) recorded for 4.1 in §114.
  The saturation test alone can also reject a *dim* home frame: it has no
  idea whether the frame has console-like texture.
- **[V]** `emu/soak.py` now waits for the `[sbwarm]` marker (300 s bound, logs
  a fallback when a firmware does not emit it) *and* keeps the colour test,
  but only applies the saturation part to frames whose greyscale stddev is
  >= 70 (console text, measured ~92 in §113, vs 53.6 for the icons). The
  reference-acquisition line now logs its brightness/stddev so a future
  misclassification is visible in the run log instead of only in the report.
- **[V]** Fresh-image exact sequence with the fixed harness printed
  **Result: PASS** on 4.2.1: SpringBoard + input after 70 s, `[sbwarm]` after
  135 s, home reference at 145 s (stddev 53.6), five app/home cycles with
  `home diff 0.0`, no recoveries, zero failures. The 4.1 exact-sequence soak
  is recorded in the next subsection.
- **[V]** 4.2.1 boot files are unchanged: `git archive origin/main` and this
  branch, built from the same staged firmware, produce identical SHA-256 for
  `kernel.bin 505f451a…`, `devtree.bin 25b1bd23…`, `fakeio.bin e5fecd66…`,
  `shim.bin e0609de9…`, `gpu.bin 7e696f65…` and `boot.cfg 8d4079cf…`.
- **[V]** The documented 4.1 sequence with the fixed harness
  (`rm -f build/sd.img && make sdimg IOS=4.1 && IOS3DS_QEMU=… python3
  emu/soak.py --cycles 5 --icount 2`) also printed **Result: PASS**: input up
  after 60 s, `[sbwarm]` at 130 s, home reference at 140 s (stddev 53.3),
  five cycles with `home diff 0.0`, no recoveries or failures. Both harnesses
  now agree on what a home frame looks like; the guest payload is unchanged.

### 3.1.3: SpringBoard is alive but never enters the lockdown path

- **[V]** Rebuilt the archived 7E18 firmware in this worktree (sha1
  `5f4f5c01…` verified) and extracted SpringBoard from the staged HFSX image.
  Its ObjC method list puts `-[SBLockdownManager _setupActivationState]` at
  Thumb VA `0x6ec30` (file `0x6dc30`), `lockdownState` at `0x6e450`,
  `_lockdownChanged` at `0x6ebdc` and `init` at `0x6e964`. The CFString
  literals in `_setupActivationState` are `Unactivated`, `Expired`,
  `Activated`, `WildcardActivated`, `Unlocked`, …; the value returned by
  `lockdown_copy_value` selects state 1–7 from those comparisons.
- **[V]** A GDB trace (QEMU `-gdb`, hardware breakpoints, no periodic
  interrupts) on the unmodified 7E18 image: kernel `_execve` fires at t=13 s,
  SpringBoard's Mach-O entry `0x34e8` at t=38 s, and
  `-[SpringBoard applicationDidFinishLaunching:]` (`0xa6f4`) at t=33–38 s
  and **returns** at `0xad30` by t=32 s. Neither `-[SBLockdownManager init]`,
  `_setupActivationState`, `lockdownState` nor our relocated
  `lockdown_copy_value` breakpoint is hit during that time. The surviving
  SpringBoard therefore stops *after* its app delegate returns but *before*
  any lockdown/activation state is computed; the "Connect to iTunes" screen
  is a default view, not the result of a live activation query.
- **[V]** Diagnostic-only patch, reverted: force `-[SBLockdownManager
  lockdownState]` to return 3 (`Activated`) by replacing its first two
  Thumb words at SpringBoard file offset `0x6d450` (`b0b502af` → `03207047`)
  and re-signing the executable with `ldid -Hsha1 -S` as on 2.2.1. A fresh
  240 s QEMU run stays on the Apple logo/spinner at t=120/180/230 s, with the
  same heartbeat and zero md0 errors. Forcing one getter is not sufficient;
  the state is either not consulted on this path or another default view is
  already up. No 4.x code was changed by the experiment.

### 2.2.1: the liblockdown re-sign, not its byte patches, stops dyld early

- **[V]** Control experiment with the archived 5H11a IPSW (sha1
  `9af5625e…` verified) extracted in this worktree. Three fresh images were
  built from the same kernel/devtree and booted with `--icount 2`:
  1. the shipped overlay (six liblockdown byte patches + `ldid -Hsha1 -S`):
     dyld stops after `CoreSurface.framework` (re-log 114);
  2. no file patches but the **same re-sign step**: dyld also stops after
     `CoreSurface.framework`;
  3. no file patches and **no re-sign**: dyld continues through WebCore,
     WebKit, OpenGLES, MBX2D, MBXConnect, MobileSync, Message and
     `libCGFreetype.A.dylib`, with 1,498 md0 requests / 0 errors by t=187 s.
  So it is the pseudo-signature written by `ldid -Hsha1` on the 2.x dylib
  that prevents dyld from loading the rest of SpringBoard's framework set,
  not the activation bytes. (SpringBoard still draws no UI in the control:
  the CLCD never sees a first iOS frame, and the top screen stays on the M1
  console through t=280 s.)
- **[V]** `rootfs/overlay-2.2.1.json` now keeps the stock, signed
  `/usr/lib/liblockdown.dylib`: its six `extra_file_patches` and the `resign`
  step were removed, and `tests/test_versions.py` expects an empty 2.2.1
  patch list. 4.x recipes and boot files are untouched; no Apple file is
  committed. Activation on 2.x remains an open item, but the boot now gets
  substantially further than before.

## 116. Older-version round 4: 3.1.3's spinner and 2.2.1's codesign wall (2026-11-24)

Continues §114/§115. Both archived IPSWs were fetched again into this
worktree's `build/fw-3.1.3/` and `build/fw-2.2.1/` (sha1 `5f4f5c01…` and
`9af5625e…` verified by `fwprep.py`); nothing Apple-derived is committed. No
4.x source, patch set or recipe was changed by any experiment below.

### Baselines on current main

- **[V]** 7E18, fresh `make sdimg IOS=3.1.3` image, `--icount 2`, 240 s:
  `BSD root: md0`, launchd, the full dyld framework set, then a **stable Apple
  logo + spinner** from t≈30 s to t=230 s (screenshot mean brightness 5.3).
  2,116 md0 requests / 0 errors; heartbeat PC is the kernel idle loop;
  no panic, restart or `CODESIGNING` line. The GDB trace (`-s -S`, hardware
  breakpoints at `_execve` 0xc011d98c, `_proc_exit` 0xc011f188, SpringBoard
  entry 0x34e8, `-[SpringBoard applicationDidFinishLaunching:]`
  0xa6f4/return 0xad30) confirms SpringBoard stays up: only two `exit(1)`
  helpers (fsck/mount) ran, and none of `-[SBLockdownManager init]` (0x6e965),
  `_setupActivationState` (0x6ec31), `lockdownState` (0x6e451) or
  `_lockdownChanged` (0x6a2f4) fired. A kernel-only trace in a second run
  showed the same two exits in the first 13 s and no further process death.
- **[V]** With `DYLD_PRINT_INITIALIZERS=1`/`DYLD_PRINT_WARNINGS=1` added to
  7E18's SpringBoard plist, dyld called every initializer through
  `/System/Library/PrivateFrameworks/MusicLibrary.framework` (last one
  0x3134ac0c) and printed no warning; the screen still showed the spinner.
  So the spinner is not an unresolved dyld load or a SpringBoard crash.
- **[V]** 5H11a with the shipped **stock, signed** liblockdown, fresh image,
  240 s: dyld loads 79 libraries through
  `/System/Library/Frameworks/CoreGraphics.framework/Resources/libCGFreetype.A.dylib`,
  then nothing; the top screen stays on the M1 console (mean 53), 1,522 md0
  requests / 0 errors, no panic. Removing the compat6 re-sign therefore did
  get further than the old post-CoreSurface stall, but still produced no UI
  (this confirms re-log 115's control run).

### 2.2.1: why the in-place liblockdown patches cannot simply be restored

- **[V]** Restoring the six 5H11a liblockdown patches from re-log 114/115
  (connect/copy_value/table/disconnect/remove/set, all original bytes verified
  against the extracted dylib's own `LC_SYMTAB`) and refreshing the 14 SHA-1
  CodeDirectory page slots in the dylib's existing embedded signature (new,
  recipe-gated `codesign_rehash` step; `LC_CODE_SIGNATURE` at 0xd4c0,
  CodeDirectory at 0xd4e4, page hashes at 0xd54a) produced **no**
  `CODESIGNING` faults but dyld stopped again after CoreSurface (only 7
  `dyld: loaded` lines). The page-hash update, like the `ldid -Hsha1 -S`
  re-sign in re-log 115, appears to invalidate the signature blob that dyld
  checks before continuing.
- **[V]** Leaving the CodeDirectory untouched but patching the code produced
  126 `dyld: loaded` lines and 100s of `CODESIGNING: vm_fault_enter(0x30891000)
  … *** INVALID PAGE ***` lines; SpringBoard restarted and the console never
  became an iOS screen. Replacing `LC_CODE_SIGNATURE` with a zero `LC_UUID`
  was no better (105 libraries, 108 codesign faults). Both variants were
  scratch images only.
- **[V]** Two version-only kernel experiments also failed: making
  `cs_validate_page` (0xc011b6f0) branch straight to its success path panicked
  in dyld at user pc 0x2fe01028 before launchd; setting the initial
  `_cs_validation` word (0xc01f3bb4) to 0 caused 358,301 `CODESIGNING` lines
  and a SpringBoard restart loop. Both patch-set edits were reverted.
- **[V]** `/private/var/run/lockdown` is absent from the stock 5H11a rootfs.
  Creating it via the overlay's `mkdirs` with the stock dylib did not start
  `lockdownd` (no log line) and left the 79-library console stop unchanged.
  **[G]** A usable 2.x activation path needs either a real page-signature
  solution or a dyld-side bypass; byte patches and pseudo-signatures alone
  are not enough.

### 3.1.3: activation is not the only gate

- **[V]** Scratch variants: allowing the real `lockdown_connect` (no stub) gave
  the same 98 libraries, `systemShutdown false` and spinner without any
  `lockdownd` log line; also creating `/private/var/run/lockdown` made it
  **worse** (three SpringBoard loads, black screen), so both were reverted.
  Replacing the dyld cache's OpenGLES `AppleMBXDevice` literal at 0x2f91bb8
  with `IOResources` (4.2.1's EAGL workaround) and adding the 4.2.1
  `SBNoWatchdogs`/`SBDontLockAfterCrash`-style SpringBoard preferences each
  left the 98-library boot and the spinner unchanged. All diagnostics were
  scratch overlays under this worktree's `build/`; the shipped
  `rootfs/overlay-3.1.3.json` is byte-identical to main.
- **[G]** The remaining 3.1.3 stop is inside SpringBoard's post-dyld
  UI bring-up (its app delegate returns, the first CLCD frame is the Apple
  spinner, but no SBLockdownManager method is called). Further work should
  trace that path in SpringBoard itself, not add more liblockdown table keys.

### Tree state after the experiments

- **[V]** Every diagnostic edit to `rootfs/overlay-{2.2.1,3.1.3}.json`,
  `kernel/patchsets/2.2.1.json`, `scripts/mkrootfs.py` and
  `tests/test_versions.py` was reverted; only this log entry remains on the
  branch. 4.2.1's kernel/devtree/fakeio/shim/gpu/boot.cfg paths were never
  touched, and no Apple binary or key is in the commit.

### 4.x verification after the round-4 experiments

- **[V]** A fresh 4.2.1 build (`make userland`, then the exact
  `rm -f build/sd.img && make sdimg && python3 emu/soak.py --cycles 5 --icount 2`
  sequence) printed **Result: PASS**: SpringBoard + input after 70 s,
  `[sbwarm]` at 130 s, home reference at 140 s (stddev 53.6), five app/home
  cycles with `home diff 0.0`, no failures.
- **[V]** The six 4.2.1 boot files generated by this branch and by an
  independent `git archive origin/main` tree (same staged firmware and
  userland binaries, `SOURCE_DATE_EPOCH=1763800000`) matched byte for byte:
  `kernel.bin 505f451a44d9…`, `devtree.bin 25b1bd233447…`,
  `fakeio.bin e5fecd6645b3…`, `shim.bin e0609de92b95…`,
  `gpu.bin 7e696f657e80…`, `boot.cfg 8d4079cfe114…`.
- **[V]** The documented 4.1 sequence had to fetch and stage 8B117 again
  into this worktree first (Apple CDN sha1 `97abde620766…` verified);
  without a staged kernel, `make sdimg IOS=4.1` silently produces a payload-
  only image. With the firmware staged, `rm -f build/sd.img && make sdimg
  IOS=4.1 && python3 emu/soak.py --cycles 5 --icount 2` printed
  **Result: PASS**: SpringBoard + input after 60 s, `[sbwarm]` at 120 s,
  home reference at 130 s (stddev 53.3), five cycles with `home diff 0.0`,
  no failures. `make test` passes before and after the final rebase.

## 117. Older-version stall: which thread blocks (round 5, 2026-11-24)

Diagnostics only so far; every scratch overlay was reverted and no 4.x
source, patch set or recipe was changed. Both archived IPSWs were copied
(read-only) from a previous worktree's build artifacts into this worktree's
`build/fw-*/` and their published sha1 values were rechecked (`5f4f5c01…`
for 7E18, `9af5625e…` for 5H11a). `make test` passes on the starting tree.

### 3.1.3: SpringBoard is alive and idle in `mach_msg`, but the framebuffer never changes

- **[V]** A fresh 7E18 image (`make sdimg IOS=3.1.3`) booted for 180 s with
  `--icount 2`. Screenshots at t=60, 120 and 170 s are **pixel-identical**
  (Apple logo + progress bar, mean brightness ~5.3). The dynamic console log
  reaches `dyld: loaded` for 97 libraries and `systemShutdown false`, then
  nothing; md0 stays healthy (2,063 requests / 0 errors) and the heartbeat PC
  is the kernel idle loop (`kc006991c`). So the stop is not a crash, an md0
  timeout or a dyld failure: the UI never replaces the boot logo.
- **[V]** SELECT+START at t≈90 s (`--keys '110:ret-backspace_down,
  112:ret-backspace_up'`, `emu/run.py`) produced a full XNU thread dump.
  `launchd` has lockdownd (pid 10), mediaserverd (11), mDNSResponder (12),
  configd (16), CommCenter (22), notifyd (24), ReportCrash and **SpringBoard
  (pid 18, 2 threads)** running. Both SpringBoard threads are stopped in user
  `mach_msg` (`pc 0x33905488`, `lr 0x3390806c`, receive ports `0x1803` and
  `0x2003`, receive sizes `0x70` and `0x450`). The threads are waiting for
  messages, not spinning; there is no panic or crash report.
- **[V]** GDB (`-s -S`, `emu/gdb/rsp.py`) method breakpoints on the 7E18
  SpringBoard executable show `-[SpringBoard applicationDidFinishLaunching:]`
  (IMP `0xa6f5`), `-[SBUIController animateAppleDown:]` (`0xbad5`),
  `-[SBUIController finishLaunching]` (`0xc149`),
  `-[SpringBoard appleIconViewRemoved]` (`0x58bd`) and
  `-[SBUIController finishedFadingInButtonBar]` (`0xb015`) are all entered.
  The Apple-logo-removal path therefore runs; the missing piece is what the
  compositor draws afterwards. (Repeated identical hits in this trace are an
  artefact of stepping off hardware breakpoints; they are not process
  restarts: the console has only one set of dyld lines.) The same dump shows
  no `-[SBLockdownManager …]` call, confirming re-log 114/115/116: activation
  state is not the gate here.

### 3.1.3: our PICA200 compositor loads, but SpringBoard then restart-loops

- **[V]** Scratch overlay (not committed): installed the 4.2.1
  `MBXGLEngine.bundle/MBXGLEngine` from `make userland`, set the 4.2.1
  SpringBoard CA environment (`IOS3DS_CA=1`, `CA_ENABLE_OGL=1`,
  `MBX2D_PAGE_FLIP=0`, …), and changed the OpenGLES `AppleMBXDevice` literal
  in 7E18's dyld cache at file offset `0x2f91bb8`
  (`AppleMBXDevice\0` -> `IOResources\0\0\0\0`, original bytes verified).
  This goes beyond re-log 116's string-only experiment: EAGL now loads our
  bundle, logs `[gl] CA compositor: OpenGL ES on the PICA200` and creates a
  context (pid 18), and the screen changes from the static Apple logo/bar to
  a spinner.
- **[V]** That does not reach the home screen: the same 180 s run has nine
  `systemShutdown false` markers and GL contexts for pids 18, 41, 55, 65, 79
  — SpringBoard is relaunching once per dyld pass — and each launch ends with
  `Iterator had no matching services` / `Failed to open connection to MBX` /
  `Failed to initialized MBX2D driver (1)`. No crash report is written, so
  the exits are normal `exit(1)` paths. The overlay was removed again;
  `rootfs/overlay-3.1.3.json` is byte-identical to main. **[G]** The next
  step is to find which 3.x-only call in our 4.2.1-linked GL bundle (or the
  MBX2D fallback it enables) makes SpringBoard exit, not to add more
  activation patches.

### 2.2.1: SpringBoard is running; a guest job confirms the live launchd set

- **[V]** SELECT+START at t≈110 s on a fresh 5H11a image again reaches
  `panic()` (`panic requested from the 3DS`), whose task list contains
  launchd (pid 1), lockdownd (9), mediaserverd (10), configd (15),
  **SpringBoard (pid 16, 2 threads)**, CommCenter (19) and notifyd (21).
  The dump double-panics while walking a kernel thread before it prints the
  SpringBoard thread state, so this run does not identify their wait
  channels. The console has 274 `dyld: loaded` lines by t=177 s (through
  WebCore/WebKit, OpenGLES, libSystem and `libCGFreetype`), 1,552 md0
  requests / 0 errors and a kernel idle heartbeat; there is no panic or
  first iOS frame.
- **[V]** A scratch launchd job running `/bin/launchctl list` in the guest
  wrote the job list into the image: `com.apple.SpringBoard` (pid 18 in that
  run), `com.apple.mobile.lockdown` (11), `com.apple.mediaserverd` (12),
  `com.apple.configd` (17), `com.apple.CommCenter` (21) and mDNSResponder
  are all live. So "a missing daemon" is not the 2.x blocker either.
- **[V]** Trying to add our own thread dumper exposed two more 2.x dyld ABI
  differences: `ios-cc`'s `LC_DYLD_INFO_ONLY (0x80000022)` is rejected
  (`dyld: unknown required load command`), and after replacing it with a
  non-required command 2.x still refuses the binary because
  `dyld_stub_binder` is not exported by 5H11a's libSystem. A version-tagged
  stub-symbol list or a syscall-only tool is needed; no such tool is shipped.


### 2.2.1: a 600 s guest run does not become a slow boot

- **[V]** Stock 5H11a image, `--icount 2`, 600 s guest time
  (`build/emu-221-long/screen_{300,450,590}.png`): the top screen is still
  the M1 boot console at t=300, 450 and 590 s, with the same dyld tail
  (`libCGFreetype.A.dylib`), 1,515 md0 requests / 0 errors, and a kernel idle
  heartbeat. This rules out "dyld is merely slow on a loaded host": after ten
  minutes of guest time neither SpringBoard's UI nor any iOS frame appears.
- **[V]** The nine scratch overlays used for the GDB/job-list/GL experiments
  were all removed; `git diff origin/main -- . ':(exclude)docs/re-log.md'`
  is empty. The six 4.2.1 boot files generated by this tree and by a clean
  `git archive origin/main` tree (same staged firmware, same
  `SOURCE_DATE_EPOCH`) have identical SHA-256:
  `kernel.bin 505f451a…`, `devtree.bin 25b1bd23…`, `fakeio.bin e5fecd66…`,
  `shim.bin e0609de9…`, `gpu.bin 7e696f65…`, `boot.cfg 8d4079cf…`.

### Final 4.x verification for this round

- **[V]** `make test` passes (offline tests plus host harnesses and the stub
  symbol check).
- **[V]** Exact fresh-image sequence for the default build:
  `rm -f build/sd.img && make sdimg && python3 emu/soak.py --cycles 5 --icount 2`
  printed **Result: PASS** (SpringBoard + input after 70 s, `[sbwarm]` after
  135 s, home reference after 145 s, five cycles, 0 panic/stall/md0/crash/
  watchdog/GPU failures).
- **[V]** Exact documented 4.1 sequence after copying 8B117's published
  firmware into `build/fw-4.1/staging` (sha1 `97abde620766…` verified):
  `rm -f build/sd.img && make sdimg IOS=4.1 && IOS3DS_QEMU=/tmp/opencode/qemu-compat2/b/qemu-system-arm python3 emu/soak.py --cycles 5 --icount 2`
  printed **Result: PASS** (SpringBoard + input after 60 s, `[sbwarm]` after
  125 s, home reference after 135 s, five cycles, 0 failures).
- **[V]** No 3.1.3 or 2.2.1 fix is shipped: neither reaches the home screen,
  so their soaks were not run (a five-cycle soak on a spinner cannot pass).
  The blocker analysis above is the deliverable of this round.

## 118. Issue #9: Luma chainload of a stale payload vs the .3dsx path (2026-11-25)

Old 3DS XL, garbled screen and dead buttons after boot; works only when started
from the Homebrew Launcher `.3dsx`, fails when chainloaded by Luma.

**Root cause.** The chainloaded payload was an old build. Every failing log
that Retrow26 attached after "updating to main" (the six logs posted
2026-11-24, including the `gpu=0` one) starts with

    ios3ds m1-0.3.0-62-g6d963b6-20261121.0001  (M1 loader)

while the one working log (the `.3dsx` boot) starts with

    ios3ds m1-0.4.0-178-gf46d155-20261124.1701  (M1 loader)
    launcher swap: restoring Luma as sd:/boot.firm

The user updated the `ios3ds/` folder (new `shim.bin`, SHA-256 66C3D09F…,
new `ios3ds/ios3ds.firm`) but not `luma/payloads/ios3ds.firm`. The launcher
boots `ios3ds/ios3ds.firm` (new); Luma boots `luma/payloads/ios3ds.firm`
(old). So a 0.3.0-62 ARM9 monitor and ARM11 stub ran against a 0.4.0 shim and
gpu.bin. The shared layout changed in between. One concrete example:
`SHIM_PANIC_REQ` was `0x6C` at 6d963b6 and is `0x6500` since c1ae67b
(2026-11-21). The old monitor never clears shim+0x6500, so the new shim reads
whatever RAM held there. In the `gpu=0` log XNU panics with
"panic requested from the 3DS (SELECT+START)", and the ARM9's
"[monitor] SELECT+START: asking XNU to panic" line is missing, so nobody
pressed the buttons and the ARM9 did not ask. Mailboxes and the GPU and input
protocol have changed the same way, which explains the garbled screen
(`gpu=1`) and dead input. Hackerman501's first log (also 0.3.0-62) was the
separate damaged-shim case already handled by d01c35f.

Other differences between the logs come from the old build itself, not from
the boot path: the CFG9_SDMMCCTL 0200 vs 0248 values, the "unknown key" lines
for boot.cfg keys that build does not know, and the old Wi-Fi hint "start
ios3ds once from the Homebrew Launcher". Nothing in the current tree depends
on state that only the launcher sets up. The launcher's NWM firmware copy only
affects Wi-Fi.

**Fix.** `boot/arm9/swap.c: firm_is_stale()` runs right after the SD mount
(after the launcher swap restore). The payload scans `sd:/ios3ds/ios3ds.firm`
for its own `IOS3DS_VERSION` string. That file comes from the same zip as
shim.bin/gpu.bin. If the string is missing, the running payload is a different
build: it copies `ios3ds/ios3ds.firm` over `luma/payloads/ios3ds.firm` when
that file exists, prints "OUTDATED ios3ds.firm (Luma chainloader)" and stays
in the monitor without starting XNU. The next boot then runs the matching
build. Builds named `dev` skip the check. If the file is unreadable, the
check only logs a warning.

This fix cannot help payloads that are already installed and older than this
commit. Those users must copy `ios3ds/ios3ds.firm` to
`luma/payloads/ios3ds.firm` once by hand, or rerun install.py.

**CA fps** (12–17 fps, `[perf] CA: 15 frames … 66 ms` in the working log):
not changed in this round. Nothing found was both cheap and safe.

- **[V]** `make test` passes; `make shim`: main part 24268/24320, GPU part
  26304/32768.
- **[V]** Fresh soak `rm -f build/sd.img && make sdimg && IOS3DS_QEMU=… python3 emu/soak.py --cycles 5 --icount 2` printed **Result: PASS** (five cycles, 0 failures; the payload check passed without a false positive).
- **[HW]** Still needs a hardware test: (1) with matching files, Luma
  chainload boots to SpringBoard with working buttons; (2) with an old
  `luma/payloads/ios3ds.firm`, the new check cannot run from the old payload,
  so test the opposite case: put this build in `luma/payloads/`, the previous
  build in `ios3ds/`, and boot via Luma. It must show the OUTDATED message,
  rewrite the payload and boot on the next power-on.
## 119. Older-version round 6: where 3.x/2.x stop drawing (2026-11-25)

Continues §117. Both archived IPSWs were re-downloaded into this worktree's
`build/fw-*/` (sha1 `5f4f5c01…` / `9af5625e…` verified by `fwprep.py`); no
Apple file is committed. All scratch overlays and GDB harnesses live under
`build/`. `make test` passes on the starting tree.

### 3.1.3: reproducing §117's GL setup, and what actually runs

- **[V]** Installed the 4.2.1 `MBXGLEngine.bundle/MBXGLEngine` from
  `make userland` into a 7E18 rootfs, wrote the dyld override marker, set the
  4.2.1 SpringBoard CA variables and patched OpenGLES's
  `AppleMBXDevice` literal at cache offset `0x2f91bb8` to `IOResources`
  (all original bytes verified). A fresh 180 s `--icount 2` run stays on the
  Apple logo/spinner with **one** dyld pass (98 `dyld: loaded` lines, one
  `systemShutdown false`), **no** `[gl] CA compositor` line, **no** MBX2D
  failure lines and **no** `exit(1)`. This does not reproduce §117's nine
  SpringBoard restarts; that scratch build must have changed another 3.x path
  as well (its overlay was reverted, so it cannot be diffed here).
- **[V]** Scripted GDB (`emu/gdb/rsp.py`, hardware breakpoints; Thumb methods
  need the even address) on that image: `-[SpringBoard
  applicationDidFinishLaunching:]` (0xa6f4),
  `-[SBUIController finishLaunching]` (0xc148),
  `-[SBUIController animateAppleDown:]` (0xbad4) and
  `-[SpringBoard appleIconViewRemoved]` (0x58bc) are all entered, confirming
  the Apple-logo-removal path runs. No `exit(1)` is taken in this build.
- **[V]** The same trace with breakpoints on QuartzCore's render-server
  entry points — `_CARenderServerRegister` (0x31bae980),
  `_CARenderServerStart` (0x31bae990), `+[CAWindowServer server]`
  (0x31bf025c), `-[CAWindowServer _detectDisplays]` (0x31bf008c),
  `CA::WindowServer::Server::Server` (0x31bdf010), `M2Display::M2Display`
  (0x31bde4f8), `MBXServer::MBXServer` (0x31bddf28),
  `EAGLServer::EAGLServer` (0x31bda5a0) and QuartzCore's
  `_mbx2DInitialize` stub (0x31bf65dc) — got **zero** hits in a full boot.
  So 7E18 SpringBoard's UI never creates a CoreAnimation render server at
  all; the spinner is the last view it draws, and this is a layer above the
  MBX2D/EAGL choice.

### 3.1.3: which code calls the MBX2D driver open

- **[V]** 7E18's MBX2D service open is `_mbxConnectionOpen` at VA
  `0x33364c90` (cache file `0x33364c90`, from the image's own `LC_SYMTAB`).
  It calls `IOServiceMatching("AppleMBXDevice")` using the literal at VA
  `0x33365c68`, then `IOMasterPort`, `IOServiceGetMatchingServices`,
  `IOIteratorNext` and `IOServiceOpen`, and prints `Iterator had no matching
  services` / `Failed to find an AppleMBXDevice service` /
  `Failed to open connection to MBX` on the failure paths. OpenGLES's
  separate copy of the string is at VA `0x32f91bb8` (the one patched in §117).
- **[V]** QuartzCore's `CA::WindowServer::MBXServer::MBXServer` (`0x31bddd78`)
  calls `_mbx2DInitialize` and logs `Failed to initialized MBX2D driver (%d).`
  through `_x_log_warn` (`0x31bf3c90`) when it fails; `_x_log_warn` only logs,
  it does not exit. `CA::WindowServer::M2Display::new_server` (`0x31bde71c`)
  picks MBXServer for display kind 1 and EAGLServer for kind 2. Scratch
  patches that skip `_mbx2DInitialize` (`0x31bdddc4` bne→b), force EAGLServer
  (`0x31bde754` beq→b), change the ES1 surface literal `L565`→`BGRA`
  (`0x31bde5e4`) and repoint MBX2D's own string to `IOResources`
  (`0x3365c68`) all applied cleanly but left the same spinner, one dyld pass
  and no GL log. **`IOResources` alone does not give MBX2D a user client** on
  3.x; the 4.2.1 keychain shim intercepts `IOServiceOpen` precisely because
  the stock service does not hand one out.
- **[G]** The next 3.x step is to find why UIKit never reaches
  `+[CAWindowServer server]` / `_CARenderServerStart` (a SpringBoard/UIKit
  state gate, not activation and not MBX2D), then make that path create the
  render server before attempting EAGL or MBX2D.

### 2.2.1: method addresses for a UI trace

- **[V]** 5H11a SpringBoard's ObjC method list gives
  `-[SpringBoard applicationDidFinishLaunching:]` IMP `0x1f781`,
  `-[SBUIController finishLaunching]` `0xc129`, `-[SBUIController
  animateAppleDown:]` `0xb4c5` and `-[SpringBoard appleIconViewRemoved]`
  `0x3685` (all Thumb); libSystem's `_exit` is `0x31463018`.
  QuartzCore has `_CARenderServerRegister` `0x31e08ebc`,
  `_CARenderServerStart` `0x31e08c4c`, `+[CAWindowServer server]`
  `0x31e3ebac`, `CADisplayCoreSurface::CADisplayCoreSurface` `0x31e389d4`
  (the constructor that calls `_mbx2DInitialize` at `0x31e394bc`), and an
  `IOMobileFramebufferOpen` stub at `0x31e44154`.
- **[V]** 2.2.1 has **no on-disk MBXGLEngine.bundle** (unlike 4.2.1); its
  `AppleMBXDevice` string lives in `MBXConnect.framework/MBXConnect` at file
  offset `0x1d88`, and OpenGLES's copy at `0x36464`. Patching either dylib
  invalidates 2.x's per-page signatures (re-log 116), so a 2.x graphics fix
  cannot be a plain file patch. These are only addresses for the next trace;
  nothing is shipped in `rootfs/overlay-2.2.1.json`.

### 2.2.1: the `libCGFreetype` console tail is not a dyld stop

- **[V]** Scripted GDB on a fresh stock 5H11a image (`--icount 2`): the
  2.x SpringBoard method IMPs found above are entered —
  `-[SpringBoard applicationDidFinishLaunching:]` (`0x1f780`),
  `-[SBUIController finishLaunching]` (`0xc128`) and
  `-[SBUIController animateAppleDown:]` (`0xb4c4`). So the dyld log ending at
  `libCGFreetype.A.dylib` is not a dyld failure: dyld has finished loading
  what SpringBoard needs, the process is alive and its UI delegate runs.
- **[V]** The same trace puts **zero** hits on QuartzCore's
  `_CARenderServerRegister` (`0x31e08ebc`), `_CARenderServerStart`
  (`0x31e08c4c`), `+[CAWindowServer server]` (`0x31e3ebac`),
  `CADisplayCoreSurface::CADisplayCoreSurface` (`0x31e389d4`) and
  `_mbx2DInitialize` (`0x31e44af4`). As on 7E18, the missing piece is a
  CoreAnimation render server, not activation or a missing daemon; MBX2D and
  GL are downstream of that. No file patch or userland tool was shipped for
  2.2.1 in this round.

- **[V]** A final 7E18 GDB trace on the UI-view methods pins the stop more
  precisely: `-[SpringBoard applicationDidFinishLaunching:]` (`0xa6f4`) and
  `-[SBUIController finishLaunching]` (`0xc148`) are entered, but
  `-[SBUIController transition:toView:]` (`0xc0f64`),
  `-[UIWindow makeKeyAndVisible]` (`0xcd064`),
  `-[TPLCDView initWithLCDView:]` (`0xbdc88`),
  `-[SBLockdownManager _setupActivationState]` (`0x6ec30`) and
  `-[SBLockdownManager lockdownState]` (`0x6e450`) are never called. So
  SpringBoard keeps its boot LCD view and never promotes a UIWindow to be
  key/visible; activation is again not the gate. A future fix has to make
  that LCD-view → UIWindow transition happen (or drive the LCD view
  directly) before MBX2D/EAGL can matter.

### Verification for this round

- **[V]** `make test` passes (offline tests plus host harnesses and the stub
  symbol check). No 4.x source, patch set or recipe was modified: the only
  committed changes are this log and the status lines in
  `docs/multi-version.md`.
- **[V]** Exact fresh-image sequence on the default build:
  `rm -f build/sd.img && make sdimg && python3 emu/soak.py --cycles 5 --icount 2`
  → **Result: PASS** (SpringBoard + input after 70 s, warm-up after 140 s,
  home reference after 150 s with stddev 53.6, five app/home cycles, zero
  panic/stall/md0/log/crash/watchdog/GPU failures).
- **[V]** Exact documented 4.1 sequence after fetching and staging 8B117
  (`sha1 97abde620766…` verified by `fwprep.py`):
  `rm -f build/sd.img && make sdimg IOS=4.1 && python3 emu/soak.py --cycles 5 --icount 2`
  → **Result: PASS** (SpringBoard + input after 70 s, warm-up after 135 s,
  home reference after 145 s with stddev 53.3, five cycles, zero failures).
- **[G]** No 3.1.3 or 2.2.1 fix is shipped and their soaks were not run: both
  still stop on the boot spinner, so a five-cycle home-screen soak cannot
  pass. The deliverable is the traced stop point above, plus verified addresses
  for the next attempt.

## 120. 3.1.3: the tethered-display gate and the missing EAGL display server (2026-11-25)

Continues §119. This round ships a version-only rootfs change for 7E18;
`scripts/versions.py`, all 4.x recipes, kernel patch sets, the shim and the
4.2.1 boot files are untouched. Both archived IPSWs were fetched again
(`5f4f5c01…` / `9af5625e…` verified by `fwprep.py`); Apple files and all GDB
traces stay under `build/`.

### Two GDB addresses in §119 were strings, not methods

- **[V]** In the extracted 7E18 SpringBoard, VA `0xcd064` is inside
  `__cstring` and contains `beginRestoring…`; `0xbdc88` contains
  `setPlaying:`. They cannot be `-[UIWindow makeKeyAndVisible]` or
  `-[TPLCDView initWithLCDView:]` IMPs. The §119 "no hit" result for those two
  breakpoints says nothing about whether the methods ran.
- **[V]** Real IMPs were recovered from the images' own ObjC method lists and
  `LC_SYMTAB`: 7E18 UIKit `makeKeyAndVisible` `0x3206b6ac`, `makeKeyWindow`
  `0x3203e930`, `orderFront:` `0x3203e858`; QuartzCore
  `+[CAWindowServer server]` `0x31bf025c`, `-[CAWindowServer _detectDisplays]`
  `0x31bf008c`, `CA::WindowServer::M2Display::new_server` `0x31bde71c`,
  `EAGLServer::EAGLServer` `0x31bda5a0`, `_CARenderServerStart` `0x31bae990`,
  `_IOMobileFramebufferOpen` `0x332e9964`. A scripted GDB trace with these
  addresses and a fresh 180 s image is the evidence below.

### The condition: `SBTetherController isTethered`, then the display kind

- **[V]** `-[SBTetherController _demoCardConnection]` (7E18 SpringBoard
  `0x965dd`) calls `IOServiceMatching("AppleTetheredDevice")`, gets matching
  services and opens a user client; `-[SBTetherController isTethered]`
  (`0x96761`) reports that ivar. Our fake IO registry has no
  `AppleTetheredDevice`, so the negative branch leaves SpringBoard on the
  boot-LCD path. With the stock binary, `+[CAWindowServer server]` had **0**
  GDB hits in a full boot.
- **[V]** Patching only `isTethered` to return NO (Thumb `movs r0,#0; bx lr`,
  file offset `0x95760`, original `f0b503af` verified) changes that: GDB sees
  `+[CAWindowServer server]` at 52 s (3 hits), `_detectDisplays`,
  `M2Display::M2Display`, `IOMobileFramebufferOpen` returning 0 (success),
  `M2Display::new_server` and `CA::WindowServer::Server::Server`. The server
  object is returned to UIKit (`r0 = 0x21bf80`). This is the first 7E18 boot
  that reaches CoreAnimation display setup.
- **[V]** `M2Display::new_server` reads the display kind from `this+0xd0`:
  kind 1 picks `MBXServer`, kind 2 picks `EAGLServer`, anything else falls
  through to the plain `CA::WindowServer::Server` constructor. On our M2CLCD
  display the kind is neither, and GDB confirmed the fall-through (no EAGL or
  MBX constructor). Replacing `ldr r4,[r5,#0xd0]` with `mov r4,#2` at dyld
  cache VA `0x31bde74c` (file `0x1bde74c`, original `d04095e5` verified)
  makes `EAGLServer::EAGLServer` fire once per server creation, with no panic
  or restart. Both patches plus the 4.2.1-style CA environment variables are
  now shipped in `rootfs/overlay-3.1.3.json`.

### Still not a picture — exact remaining stop

- **[V]** After both shipped patches the 180 s screenshot is still the Apple
  logo + progress bar (mean brightness 7.4, unchanged at 60/120/170 s).
  `-[UIWindow makeKeyAndVisible]`/`makeKeyWindow` and `_CARenderServerStart`
  still have **0** GDB hits, so no CA context is created and the LCD keeps
  showing the boot spinner. The two patches are necessary to reach display
  setup but are not sufficient to create the key window.
- **[V]** Scratch-only experiments, all reverted: (a) the 4.2.1
  `MBXGLEngine.bundle`, dyld override marker and AppleMBXDevice→IOResources
  patches with the CA environment produced five SpringBoard restarts, no
  `[gl] CA compositor` line and no picture — the 4.2.1-linked bundle still
  cannot render on 3.1.3; (b) patching `-[SBAwayController isLocked]` to NO
  (`0x1d47d`, file `0x1c47d`) changed nothing; (c) branching UIKit's
  `orderFront:` to `makeKeyAndVisible` and stubbing `makeKey:` changed
  nothing; (d) a trampoline from `+[CAWindowServer server]` that also called
  `_CARenderServerStart` changed nothing. None is shipped.
- **[G]** The next gate is further upstream: find what makes 7E18 UIKit call
  `makeKeyWindow` (the 4.2.1 `SBUIController init` does it directly; 3.x uses
  `orderFront:`/`makeKey:` and neither reaches UIKit's IMPs in our traces),
  or drive an LCD view directly. Re-log §119's "activation is not the gate"
  still holds: no `SBLockdownManager` method fires.
- **[V]** 2.2.1 has the same `SBTetherController isTethered` method
  (`0x9bbe9`, file `0x9abe9`) and its QuartzCore has
  `CADisplayCoreSurface::CADisplayCoreSurface` (`0x31e389d4`) with an
  `_mbx2DInitialize` call at `0x31e394bc`. No 2.x patch is shipped until a
  3.x image actually draws, because 2.x page signatures invalidate naive
  dylib patches (§116).

### Verification of this round's tree

- **[V]** `make test` passes with the enlarged 3.1.3 patch list; only
  `rootfs/overlay-3.1.3.json` and `tests/test_versions.py` changed outside the
  docs. 4.x generated boot files are byte-identical (hashes in the final
  verification subsection below).
- **[V]** No 3.1.3/2.2.1 soak was run: their five-cycle home-screen check
  cannot pass while the screen is the boot spinner. This is the unfinished
  part of the round; the code change and the traced condition are the
  deliverable.

### Final verification for this round

- **[V]** `make test`: 94 offline tests plus host harnesses, stub-symbol check
  and `test_blkcache` all pass.
- **[V]** Fresh default build after the overlay change produced the same six
  4.2.1 SHA-256 values recorded in §119: `kernel.bin 505f451a…`,
  `devtree.bin 25b1bd23…`, `fakeio.bin e5fecd66…`, `shim.bin e0609de9…`,
  `gpu.bin 7e696f65…`, `boot.cfg 8d4079cf…`. No kernel, shim or GPU source
  was touched by this branch.
- **[V]** Exact fresh-image sequence `rm -f build/sd.img && make sdimg &&
  python3 emu/soak.py --cycles 5 --icount 2` → **Result: PASS**: SpringBoard
  + input after 70 s, `[sbwarm]` after 135 s, home reference after 145 s
  (stddev 53.6), five app/home cycles, zero panic/stall/md0/log/crash/
  watchdog/GPU failures. (An earlier attempt in this round raced with a
  concurrent 3.1.3 image build and failed during HFS+ file addition; the
  standalone rerun above is the valid one.)
- **[V]** Exact documented 4.1 sequence, after refetching 8B117
  (`sha1 97abde620766…` verified): `rm -f build/sd.img && make sdimg IOS=4.1
  && python3 emu/soak.py --cycles 5 --icount 2` → **Result: PASS**:
  SpringBoard + input after 60 s, warm-up after 125 s, home reference after
  135 s (stddev 53.3), five cycles, zero failures.
- **[V]** No 3.1.3/2.2.1 soak PASS is claimed. The shipped 3.1.3 overlay
  builds cleanly and reaches CA display setup, but the screen still shows
  the boot spinner; a five-cycle home-screen soak cannot pass yet.

## 121. 3.1.3: make the main UIWindow key, and where the home view still stops (2026-11-25)

Continues §120. Both archived IPSWs were fetched again into this worktree's
`build/fw-*/` (sha1 `5f4f5c01…` / `9af5625e…` verified by `fwprep.py`); all
Apple files, images, GDB scripts and scratch patches stay under `build/`.
Only `rootfs/overlay-3.1.3.json` and this log change in the tree.

### The gate in 7E18 `SBUIController init`

- **[V]** Recovered the real classes/methods from the executable's ObjC
  metadata (231 classes, 3532 methods) instead of guessing IMPs:
  `-[SBUIController init]` is Thumb `0xe141`, `finishLaunching` `0xc149`,
  `animateAppleDown:` `0xbad5`, `-[SpringBoard appleIconViewRemoved]` `0x58bd`,
  `-[SBUIController restoreIconList:]` `0xc975`. `SBUIController`'s `_window`
  ivar is at offset 4 (its ivar list at `0xef74c`). The Mach-O is loaded with
  vmaddr `0x1000` and file offset = VA − `0x1000`; runtime selrefs/sel strings
  are at the same low VAs (class `SBUIController` `0xe7628`,
  `sharedInstance` selref `0xea000`, `makeKeyAndVisible` selref `0xebd18`).
- **[V]** Disassembly of `-[SBUIController init]` shows the 3.x difference
  from 4.2.1: at `0xe344` it sends `orderFront:` to its UIWindow, then at
  `0xe374` sends `makeKey:` **with NO** to the *status bar* window. The main
  window is never made key, so no CA context/render server is created, which
  matches §120's zero hits on `_CARenderServerStart`/`makeKeyWindow`. The
  literal pool word at VA `0xe478` (file `0xd478`) is the `orderFront:`
  selref `0x000e9b50`, verified byte-for-byte in the 7E18 executable.
- **[V]** Shipped patch: change that single word to the `makeKeyAndVisible`
  selref `0x000ebd18` (original `509b0e00`, new `18bd0e00`, mode 0755).
  The existing message send then calls `[SBUIController's window
  makeKeyAndVisible]`; the leftover `r2=0` argument of `orderFront:` is
  ignored by the no-argument selector. This is a 4-byte, version-only file
  patch; no 4.x recipe, patch set, shim or GPU file is touched.
- **[V]** Fresh 3.1.3 image with this patch (plus §120's patches): CA now
  draws the boot UI through a real key UIWindow. Two runs at 120/170 s showed
  the spinner (top-screen mean 0.27) and the Apple logo + progress bar
  (mean 4.8, a later/restarted SpringBoard state); without the patch the
  screen stayed black/spinner at 0.12 with no CA drawing. Neither run reached
  icons. GDB hardware breakpoints on user VAs were not reliable in this
  harness (kernel breakpoints hit, user ones did not), so the visual check
  and a shim-memory marker were used for verification.

### Still not the home screen — scratch experiments, all reverted

- **[V]** A Thumb hook at `appleIconViewRemoved` (`0x58bc`) branching to an
  ARM trampoline in a verified zero gap of SpringBoard `__TEXT` (`0xdb850`,
  file `0xda850`) wrote `0x5a` to shim PA `0x20845f00`: `xp /4x 0x20845f00`
  after boot showed `0x5a`, proving the method runs. That trampoline called
  `[[SBUIController sharedInstance] makeKeyAndVisible]` and the screen stayed
  on the spinner.
- **[V]** Extending the same trampoline with `_CARenderServerRegister`
  (`0x31bae980`), `_CARenderServerStart` (`0x31bae990`) and
  `[[SBUIController sharedInstance] restoreIconList:YES]` (selref `0xea3b4`)
  left the spinner unchanged at 120/170 s. Adding `-[SBAwayController
  isLocked] -> NO` (Thumb IMP `0x1d47d`, file `0x1c47c`, original `034b1b68`)
  on top changed the picture to the Apple logo + progress bar (mean 4.8),
  not the icon list. None of these three patches is shipped.
- **[V]** 2.2.1 scratch: `-[SBTetherController isTethered]` is Thumb `0x9bbe9`
  (file `0x9abe8`, original `f0b5184e`); patching it to return NO built and
  booted, but both LCDs stayed on the 3DS boot console through 150 s. The
  patch was reverted; no 2.x overlay change is shipped this round. Further
  5H11a anchors for a future attempt: `-[SBUIController init]` `0xe4cd`,
  `finishLaunching` `0xc129`, `animateAppleDown:` `0xb4c5`,
  `-[SpringBoard appleIconViewRemoved]` `0x3685`, selrefs `orderFront:`
  `0xdd57c`, `makeKey:` `0xdd6e4`, `makeKeyAndVisible` `0xdf10c`,
  `SBUIController` class `0xc63fc`, `_window` ivar offset 4.
- **[G]** The remaining gate is above window creation: SpringBoard reaches
  `applicationDidFinishLaunching:`, `finishLaunching`, `animateAppleDown:`
  and `appleIconViewRemoved`, enumerates `allApplications` and calls
  `autoLaunchIfNecessaryOnBoot:`, but never executes the method that swaps
  the boot LCD view for the icon list (§119's `transition:toView:` trace).
  Finding that call site (or the notification/state it waits for) is the next
  step before 2.x can reuse this approach. Neither old version can pass a
  five-cycle home-screen soak while the screen is still the boot spinner.

### Verification of this round's tree

- **[V]** `make test`: all 94 offline tests plus host harnesses and the stub
  symbol check pass. `tests/test_versions.py` now includes the new 7E18
  `0xd478` patch in its expected 3.1.3 patch list; no 4.x expectation changed.
- **[V]** Fresh default build (`make userland`, then
  `rm -f build/sd.img && make sdimg`) produced the same six 4.2.1 boot-file
  hashes as §120: `kernel.bin 505f451a…`, `devtree.bin 25b1bd23…`,
  `fakeio.bin e5fecd66…`, `shim.bin e0609de9…`, `gpu.bin 7e696f65…`,
  `boot.cfg 8d4079cf…`. The 3.1.3 overlay cannot affect them.
- **[V]** Exact fresh-image sequence
  `rm -f build/sd.img && make sdimg && python3 emu/soak.py --cycles 5 --icount 2`
  → **Result: PASS**: SpringBoard + input after 70 s, warm-up after 130 s,
  home reference after 140 s (stddev 53.6), five app/home cycles, zero
  panic/stall/md0/log/crash/watchdog/GPU failures.
- **[V]** Fresh 3.1.3 build of the shipped overlay
  (`python3 scripts/mksd.py --image build/sd313-final.img --ios 3.1.3
  --staging build/fw-3.1.3/staging --no-cydia --no-tlsfix`) succeeds and
  boots to the CA-drawn boot UI as described above. No 3.1.3/2.2.1 soak PASS
  is claimed: neither version reaches a home screen yet.
- **[V]** Exact documented 4.1 sequence, after refetching 8B117
  (`sha1 97abde620766…` verified by `fwprep.py`):
  `rm -f build/sd.img && make sdimg IOS=4.1 && python3 emu/soak.py --cycles 5 --icount 2`
  → **Result: PASS**: SpringBoard + input after 60 s, warm-up after 120 s,
  home reference after 130 s (stddev 53.3), five cycles, zero failures.
  Kernel/devtree hashes `e2613577…` / `29ebba25…` match §5.3; `boot.cfg`
  is `5851eebe…` after the later main-branch boot-option changes (4.1 is not
  required to be byte-identical).

## 122. Older-version round 9: `transition:toView:` is an alert, the icon-list gate is the away controller and the power-down restore (2026-11-25)

Continues §121. Both archived IPSWs were fetched again into this worktree's
`build/fw-3.1.3/` and `build/fw-2.2.1/` (sha1 `5f4f5c01…` / `9af5625e…`
verified by `fwprep.py`), plus 4.1 (`97abde62…`). All Apple files, images,
scratch scripts and disassembly dumps stay under `build/`. Only
`rootfs/overlay-3.1.3.json`, `tests/test_versions.py` and this log change in
the tree; no 4.x recipe, patch set, shim or GPU file is touched.

### Static analysis: the §119 `transition:toView:` breakpoint was a red herring

- **[V]** Parsed 7E18 SpringBoard's real ObjC metadata instead of guessing IMPs:
  class at +0x10 points at `class_ro_t`; `class_ro_t` has name at +0x10,
  `baseMethods` at +0x14, ivars at +0x1c; `method_list_t` is
  `{entsize=0xc, count, [SEL, types, IMP]…}` from +8. This gives 231 classes
  and ~3400 instance methods with consistent names/IMP addresses.
- **[V]** `transition:toView:` has exactly one selref (`0xeadc8`) and one
  literal-pool word (`0x344f8`). The enclosing Thumb function starts at
  `0x342f4`: it sends `_replyView` to `self`, then `transition:8 toView:result`
  to that reply view. The enclosing method is
  `-[_SBAwayAlertView _replyClicked]` (IMP `0x342f5`); this is a UIKit alert
  transition on the lock-screen alert, **not** the boot-LCD → icon-list swap.
  The §119 breakpoint at `0xc0f64` was inside unrelated code.
- **[V]** The icon list is restored by `-[SBUIController restoreIconList:]`
  (IMP `0xc975`, Thumb; file `0xc974`, class `0xe7628`). Its first checks are
  `[[SBAwayController sharedAwayController] isLocked]`,
  `isDeviceLockedOrBlocked` and `isMakingEmergencyCall` (selrefs `0xcb20`,
  `0xcb24`, `0xcb28`). If any is true it calls
  `frontLocked:animate:automatically:` and returns at `0xcb02`, so the icon
  list is never inserted. If those pass, helper `0xe9a8` reads three `__bss`
  flags at `0x107220/1/2`; any non-zero flag also returns early. Only then
  does `restoreIconList:` insert/order the lists, unscatter and show the
  button bar.
- **[V]** The only in-binary call chain to it is
  `-[SBPowerDownController activate]` (IMP `0x465a1`) →
  `performSelector:withObject:afterDelay:` of
  `_restoreIconListIfNecessary` (IMP `0x466fd`) → four state checks
  (`0x46702`–`0x46720`) → `[[SBUIController sharedInstance] restoreIconList:NO]`
  at `0x46734`. This controller is not activated by the 7E18 boot path in our
  harness, which explains why §119/§121 never saw the icon swap.

### What the Apple logo + progress bar actually is

- **[V]** SpringBoard contains `SBResetManager`, `SBResetView` and
  `SBResetProgressView` (`beginReset` `0x8c98d`, `_beginReset:` `0x8cbad`,
  `_resetFinished` `0x8c8e1`, `SBResetView _updateProgress` `0x921ed`,
  `_resetStarted:` `0x92231`, `drawRect:` `0x927e5`). The vertical bar drawn
  next to the Apple logo is that reset view, not the activation spinner.
  `beginReset` maps reset mode 3/4 to `beginResetting:` on the shared manager.
- **[V]** With the shipped §120/§121 CA patches plus
  `isLocked -> NO`, a fresh 240 s 3.1.3 run stays on Apple logo + progress
  bar (screenshots at 90/150/210 s identical); one dyld pass, no crash loop.
  Patching `SBResetManager beginReset` and `SBResetView _resetStarted:` to
  return immediately changed nothing with those CA patches. With the CA
  patches *removed*, the same lock/reset patches produce the "Connect to
  iTunes" LCD screen instead, i.e. that state can also be drawn by the boot
  LCD view. Neither variant reached icons.

### Shipped patch and rejected experiments

- **[V]** Shipped: a 4-byte version-only patch at SpringBoard file offset
  `0x1c47c` (`034b1b68` → `00207047`, Thumb `movs r0,#0; bx lr`) makes
  `-[SBAwayController isLocked]` return NO. This is the first of the three
  gates in `restoreIconList:`; it advances the picture from the spinner to
  the reset view and is stable by itself. `tests/test_versions.py` now
  expects the extra offset. No 4.x file changed.
- **[G]/[V]** Not shipped: (a) `isDeviceLockedOrBlocked -> NO`
  (`0x1de1c`) and `isMakingEmergencyCall -> NO` (`0x1c4e4`) each make
  SpringBoard restart 3–5 times in 120 s (490 dyld lines instead of 98) and
  leave only a spinner; (b) an ARM trampoline hooked at
  `appleIconViewRemoved` (`0x58bc`) calling
  `[[SBUIController sharedInstance] restoreIconList:NO]`, plus forcing the
  three-flag early return in `restoreIconList:` (`0xb9f0` `beq`→`b`) and
  disabling the four early returns in `_restoreIconListIfNecessary`, left
  the Apple/progress picture unchanged and the shim-memory marker was not
  observed, so no claim is made that those hooks ran; (c) reset no-ops as
  above. All scratch overlays were reverted before the shipped patch was
  added.
- **[G]** The next gate is upstream of `restoreIconList:`: find what should
  activate `SBPowerDownController` at boot (or call
  `_restoreIconListIfNecessary` directly from a known-to-run boot method),
  then check the three `0x107220/1/2` flags. Do not stub the other two
  `SBAwayController` lock queries: they are used elsewhere and crash the
  process.
- **[V]** 2.2.1 has no new code this round; the 5H11a anchors in §119/§121
  still need a 2.x signature-safe mechanism (per-page `ldid -Hsha1`
  re-signing) before any of this can be reused there.

### Verification of this round's tree

- **[V]** `make test`: all offline tests plus host harnesses, stub-symbol and
  conflict-marker checks pass with the new expected 3.1.3 patch list.
- **[V]** Fresh default build (`make userland`, `rm -f build/sd.img`,
  `make sdimg`) produced the six unchanged 4.2.1 boot-file hashes:
  `kernel.bin 505f451a…`, `devtree.bin 25b1bd23…`, `fakeio.bin e5fecd66…`,
  `shim.bin e0609de9…`, `gpu.bin 7e696f65…`, `boot.cfg 8d4079cf…`.
- **[V]** Exact fresh-image sequence
  `rm -f build/sd.img && make sdimg && python3 emu/soak.py --cycles 5 --icount 2`
  → **Result: PASS** (SpringBoard + input after 70 s, home reference after
  140 s, five app/home cycles, zero panic/stall/md0/log/crash/watchdog/GPU
  failures).
- **[V]** Exact documented 4.1 sequence after staging 8B117:
  `rm -f build/sd.img && make sdimg IOS=4.1 && python3 emu/soak.py --cycles 5 --icount 2`
  → **Result: PASS** (five cycles, zero failures).
- **[V]** Fresh 3.1.3 image with the shipped overlay
  (`python3 scripts/mksd.py --image build/sd313-final9.img --ios 3.1.3
  --staging build/fw-3.1.3/staging --no-cydia --no-tlsfix`) boots to the
  Apple/progress reset view described above. No 3.1.3/2.2.1 soak PASS is
  claimed: neither version reaches a home screen.

## 123. 3.1.3: call the icon-list restore from a boot method, and why the picture still stops (2026-11-25)

Continues §122. Both archived IPSWs and 8B117 were fetched again into this
worktree's `build/fw-*/` (sha1 `5f4f5c01…` / `9af5625e…` / `97abde62…`
verified by `fwprep.py`). All Apple files, images, scratch disassemblers,
assembler output and GDB logs stay under `build/`; only
`rootfs/overlay-3.1.3.json`, `tests/test_versions.py`, the 3.1.3 status line in
`docs/multi-version.md` and this log change in the tree.

### A working 7E18 trampoline (and the branch-encoding trap)

- **[V]** Parsed the executable's ObjC metadata again (`build/fw-3.1.3/objc.py`,
  scratch): 231 classes, 3262 selectors. `-[SpringBoard
  applicationDidFinishLaunching:]` IMP `0xa6f5` (file `0x96f4`),
  `-[SBUIController finishLaunching]` `0xc149`, `-[SBUIController
  restoreIconList:]` `0xc975`, `-[SpringBoard appleIconViewRemoved]` `0x58bd`,
  `SBPowerDownController` class `0xe7bf0`. `restoreIconList:`'s first checks
  are `sharedAwayController` → `isLocked`, `isDeviceLockedOrBlocked`,
  `isMakingEmergencyCall` (branches `0xc9b2`/`0xc9d2`). Its three-flag helper
  is the 0x5a-byte Thumb function at `0xe9a8`: it reads bytes at literal
  addresses `0x107221`, `0x107220`, `0x107222`, and is called from file offsets
  `0xc9e8` (restoreIconList:), `0xf126` and `0x1c8c0`.
- **[V]** The trampoline pattern that works: patch the first six bytes of a
  method whose prologue is `push {r4-r7,lr}; mov r6,fp; mov r5,sl` with
  `push {r4-r7,lr}; blx <ARM trampoline>`; the push saves the caller's lr
  *before* BLX overwrites it. The ARM trampoline at VA `0xdb860` (verified
  zero-filled gap of SpringBoard `__const`, file `0xda860`) pushes r4-r11/lr,
  saves self/cmd in r8/r9, calls `objc_msgSend` through the verified stub
  `0xb1bc8` with classref `0xecd10` (`SBUIController`), selrefs `0xea000`
  (`sharedInstance`) / `0xea3b4` (`restoreIconList:`), replays
  `mov r6,fp; mov r5,sl`, pops and returns to the instruction after the hook.
- **[V]** Branch encodings must be assembled at the *final* VA (`.org` plus an
  expression relative to that VA), not at offset zero: the first two build
  attempts assembled `blx 0xdb860`/`bl 0xb1bc8` at base 0, so after relocation
  they branched to unrelated addresses and the hook never ran. The corrected
  bytes are `f0b5 d5f0 d0ef` (hook at `0x58bc`) and an ARM blob whose two
  `bl 0xb1bc8` encodings disassemble at `0xdb87c`/`0xdb894`.
- **[V]** The same trampoline hooked at `applicationDidFinishLaunching:`
  (`0xa6f4`) or `appleIconViewRemoved` (`0x58bc`) runs without crashes; a
  no-op trampoline gave one dyld pass (98 `dyld: loaded` lines) and the
  unchanged Apple/progress picture. A marker written to virtual
  `0x20845f00`/`0x107500` was not a reliable observation channel in this
  harness (HMP virtual reads only work on the CPU whose ASID owns the page,
  and writing those pages caused SpringBoard restarts), so the marker was
  dropped from the shipped hook.

### Shipped: bypass the three gates locally, call restoreIconList: from appleIconViewRemoved

- **[V]** Five version-only patches in `rootfs/overlay-3.1.3.json`: the hook at
  file `0x48bc`, the ARM trampoline at `0xda860`, and three byte patches inside
  `restoreIconList:` — file `0xb9b2` `bne`→`nop` (ignore
  `isDeviceLockedOrBlocked`), `0xb9d2` `beq`→`b` (ignore
  `isMakingEmergencyCall`), and `0xb9e8` replace the `0xe9a8` call with
  `movs r0,#0; nop` (all three `0x107220/1/2` flags look clear). The two
  SBAwayController queries are **not** stubbed globally, as §122's restarts
  warned. All original bytes were verified in the extracted 7E18 executable;
  `tests/test_versions.py` expects the 17-patch 3.1.3 list.
- **[V]** Fresh 3.1.3 image (`python3 scripts/mksd.py --image
  build/sd313-ship.img --ios 3.1.3 --staging build/fw-3.1.3/staging
  --no-cydia --no-tlsfix`) booted for 300 s at `--icount 2` with screenshots
  at 60/120/180/240/290 s: 98 dyld lines (one SpringBoard pass), 0 crashes,
  md0 0 errors, and the Apple-logo/progress-bar frame unchanged. The call
  therefore happens and the flags are cleared, but the boot LCD view is still
  not replaced. **No 3.1.3 home-screen soak PASS is claimed.**

### Rejected experiments (all reverted before the shipped patch was added)

- **[V]** Stubbing `-[SBSyncController resetState]` and `restoreState` to 0
  (`0x6ea84`/`0x6ea60`) gave 490 dyld lines (3–5 SpringBoard restarts).
  No-oping `-[SBAwayView updateUIForResetState:]`/`updateUIForRestorationState:`
  (`0x35150`/`0x35244`) or skipping the reset-state store in
  `beginResetting:` (`0x6ec60` `strb r2,[r0,r3]`→`nop`) stayed stable but did
  not change the picture. `SBResetManager beginReset`/`SBResetView
  _resetStarted:` no-ops were already tried in §122.
- **[V]** `[[SBAwayController sharedAwayController] unlockWithSound:NO]` from
  `appleIconViewRemoved` (classref `0xecd14`, selref `0xe9f90`) was stable but
  left the picture unchanged; calling `-[SBLockdownManager
  _setupActivationState]` (classref `0xed110`, selref `0xebcbc`) from either
  boot method caused 6 or 4 SpringBoard restarts.
- **[V]** Calling `_CARenderServerStart` (`0x31bae990`, via `ldr r3; blx r3`
  because the cache VA is beyond ARM `bl` range) plus `restoreIconList:NO`
  from `appleIconViewRemoved` briefly replaced the Apple/progress frame with
  the small spinner and printed `Error performing migration.`; a 300 s rerun
  had 392 dyld lines (4 passes), so it is unstable and is not shipped.
- **[G]** The remaining gate is above the icon list and the reset flags: the
  boot LCD view is never swapped for the icon view, and a plain
  `restoreIconList:` call does not change which surface the ARM9 copies to the
  LCD. Next step: find the 3.x view-swap call site (or make the EAGL render
  server draw into the LCD framebuffer), then re-check the home-screen soak.

### 2.2.1

- **[V]** 5H11a was staged again and its overlay inspected: it still carries
  no file patches and no `resign` map (re-log 115 showed that even a
  no-byte-change `ldid -Hsha1` re-sign stops dyld after CoreSurface), so the
  per-page signature-safe re-sign mentioned in §122 is **not implemented**.
  No 2.x code changed this round.

### Verification of this round's tree

- **[V]** `make userland` and `make test`: 94 offline tests + 8 host tests,
  all host harnesses, stub-symbol and conflict-marker checks pass.
- **[V]** Fresh default build (`make userland`, `rm -f build/sd.img`,
  `make sdimg`) reproduced the six 4.2.1 boot hashes from §122 exactly:
  `kernel 505f451a…`, `devtree 25b1bd23…`, `fakeio e5fecd66…`,
  `shim e0609de9…`, `gpu 7e696f65…`, `boot.cfg 8d4079cf…`.
- **[V]** Exact fresh-image sequence
  `rm -f build/sd.img && make sdimg && python3 emu/soak.py --cycles 5 --icount 2`
  → **Result: PASS** (SpringBoard + input after 70 s, five app/home cycles,
  no "not home after Home", zero panic/stall/md0/log/crash/watchdog/GPU
  failures).
- **[V]** Exact documented 4.1 sequence after staging 8B117
  (`sha1 97abde62…` verified): `rm -f build/sd.img && make sdimg IOS=4.1 &&
  python3 emu/soak.py --cycles 5 --icount 2` → **Result: PASS** (five cycles,
  zero failures).

## 124. 3.1.3: the icon list is not off-screen — dump every candidate surface, then trace the two boot views (2026-11-25)

Continues §123. The archived 7E18 IPSW was fetched again into this worktree's
`build/fw-3.1.3/` (sha1 `5f4f5c01…` verified by `fwprep.py`), and 8B117 was
staged for the 4.1 soak. All Apple files, memory dumps, screenshots, ObjC
parsers and scratch overlays stay under `build/`. No 4.x recipe, patch set,
shim or GPU file is touched by this round.

### The ARM9 already mirrors the surface SpringBoard draws

- **[V]** Booted the shipped 3.1.3 overlay for 230 s at `--icount 2` and used
  QEMU HMP `pmemsave` at t=212 s to dump the fake CLCD page at PA `0x1ffba000`
  (4 KiB), `/vram` (3 MiB at `0x18000000`), the 3DS LCD framebuffers (2 MiB at
  `0x18300000`) and all 128 MiB of FCRAM (`0x20000000`). The CLCD page reads
  `on=0x3, window-1 base=0x18096000, stride=320, 320x480`: AppleM2CLCD moved
  scan-out from iBoot's `0x18000000` to the second `/vram` frame. The SD log's
  spot check reports 0/512 LCD pixels different from that CLCD buffer, i.e.
  the ARM9 copy is faithful; there is no second scan-out path to fix.
- **[V]** Rendered every 320x480 slot of `/vram` as a BGRA image:
  `0x18000000` is black (cleared back buffer), `0x18096000` and `0x1812c000`
  both hold the Apple logo + progress bar; the remaining two slots are black.
  No icon grid exists off-screen in `/vram`.
- **[V]** Scanned all of FCRAM in 4 KiB steps for coherent 320x480 frames,
  using four independent filters (dark-background/colourful-icon, uniform
  background, adjacent-pixel smoothness in 32 bpp and in RGB565). Every hit
  was kernel data, a dyld cache or a compressed filesystem page when rendered;
  a byte search for a distinctive 32-byte gradient from the VRAM Apple logo
  found no CA-owned copy elsewhere. So the icons are not being drawn into a
  hidden CoreSurface buffer: the remaining gate is above the icon view.

### Where the two boot views come from

- **[V]** Parsed the 7E18 SpringBoard executable with the scratch ObjC parser
  (`build/fw-3.1.3/objc.py`; 231 classes, ~3400 methods). `applicationDidFinishLaunching:`
  (Thumb `0xa6f5`) sends `frontLocked:animate:automatically:` to the away
  controller at `0xaab8` and `unlockWithSound:` at `0xaade`, and
  `-[SBUIController wakeUp:]` (`0xaf75`) sends `frontLocked:…` again at
  `0xafae`. `-[SBUIController finishLaunching]` (`0xc149`) removes the view
  tagged 152 (the Apple icon image), and `-[SBUIController restoreIconList:]`
  (`0xc975`) sets `_restoringIconList`, sends `setAlpha:1.0` and
  `insertAndOrderIconListsForReordering:` to its `contentView`. Those calls
  are all reached in the shipped build; they do not replace the view covering
  the screen.
- **[V]** `-[SpringBoard showSimulatedScreenBlank]` (`0x8989`) allocates a
  UIWindow with `initWithContentRect:`, sets its window level and `orderFront:`s
  it, storing it in `_simulatedBlankingWindow` (SpringBoard ivar `+0x68`);
  `hideSimulatedScreenBlank` (`0x3da5`) does `orderOut:` + `release`. This is
  the centred spinner that appears when the away view is skipped.

### Scratch patches (all reverted; none shipped)

- **[V]** `applicationDidFinishLaunching:`'s `beq` at file `0x9aae`
  (`18d0`→`18e0`, i.e. always skip `frontLocked:`) is stable (98 dyld lines,
  one SpringBoard pass) and changes the picture from Apple logo + progress
  bar to the centred spinner. Forcing `unlockWithSound:` at `0x9ad0`/`0x9ad4`,
  or returning activation state 3 from `-[SBLockdownManager lockdownState]`
  (`0x6d450`) on top, does not reveal icons (168–180 s screenshots).
- **[V]** Extending the shipped ARM trampoline to send
  `[window bringSubviewToFront:contentView]` after `restoreIconList:` gives
  784 dyld lines (8 restarts) in 200 s: rejected. Calling
  `[self hideSimulatedScreenBlank]` from the same hook, or no-oping
  `showSimulatedScreenBlank` (`0x7988`), is either unchanged or unstable
  (588 dyld lines). Skipping *both* `frontLocked:` call sites
  (`0x9aae` and `SBUIController wakeUp:`'s branch at `0x9fe8`) is stable
  (98 dyld lines) but the Apple/progress frame remains: a third layer shows
  it. No scratch overlay is in the tree.
- **[G]** The away/reset view and the boot blanking window are two separate
  layers above the icon content view. A future fix needs to remove both from
  the window stack (or find the notification that dismisses them) before the
  restored icon list can be seen. The framebuffer path needs no changes.

### Shipped: a reusable surface dump tool

- **[V]** `emu/surfaces.py` boots any SD image headlessly, takes an LCD
  screenshot and uses QMP `pmemsave` to copy the fake CLCD page and `/vram`,
  prints `on/base/stride/width×height` from the register page and renders
  each 320x480 BGRA slot of `/vram` as an upright PNG under `build/`. It is
  version-independent, changes no guest bytes and leaves the 4.2.1 boot files
  untouched.

### Verification of this round's tree

- **[V]** `make test`: all 94 offline tests plus the host harnesses, stub
  symbol check, conflict-marker checks and `test_blkcache` pass.
- **[V]** `emu/surfaces.py` smoke run on a 3.1.3 image (8 s, `--icount 2`)
  printed the CLCD geometry and wrote a screenshot plus the five /vram slot
  PNGs under `build/`; the tool changes no guest bytes.
- **[V]** Fresh default build (`make userland`, `rm -f build/sd.img`,
  `make sdimg`) reproduced the six 4.2.1 boot hashes from §123 exactly:
  `kernel 505f451a…`, `devtree 25b1bd23…`, `fakeio e5fecd66…`,
  `shim e0609de9…`, `gpu 7e696f65…`, `boot.cfg 8d4079cf…`.
- **[V]** Exact fresh-image sequence
  `rm -f build/sd.img && make sdimg && python3 emu/soak.py --cycles 5 --icount 2`
  → **Result: PASS** (SpringBoard + input after 70 s, home reference after
  140 s with stddev 53.6, five app/home cycles, no "not home after Home",
  zero panic/stall/md0/log/crash/watchdog/GPU failures).
- **[V]** Exact documented 4.1 sequence after staging 8B117
  (`sha1 97abde62…` verified): `rm -f build/sd.img && make sdimg IOS=4.1 &&
  python3 emu/soak.py --cycles 5 --icount 2` → **Result: PASS** (SpringBoard +
  input after 60 s, home reference after 130 s with stddev 53.3, five cycles,
  no "not home after Home", zero failures).
- **[V]** No 3.1.3/2.2.1 soak PASS is claimed: neither version reaches a home
  screen yet. 3.1.3 is stable for 300 s with one dyld pass but keeps the
  Apple/progress frame; the seven scratch patches above were all reverted.

## 125. 3.1.3 reaches its home screen: the "reset view" was DataMigrator, plus an IRQ-replay and a dispatch-table bug (2026-11-25)

Continues §124. The 7E18 IPSW staged by §124 was copied into this worktree's
`build/fw-3.1.3/` (Apple files stay under `build/`). The 4.2.1 boot files are
byte-identical (see the verification list).

### The Apple logo + progress bar was never SpringBoard

- **[V]** A SELECT+START thread dump of the shipped §124 image lists a
  `DataMigrator` process (pid 27) next to SpringBoard. 7E18's
  `/System/Library/PrivateFrameworks/DataMigration.framework/Support/DataMigrator`
  links CoreSurface + IOMobileFramebuffer and draws its own Apple logo and
  progress bar straight into the framebuffer (`IOMobileFramebufferSwapBegin/
  SwapSetLayer/SwapEnd`). It is the only thing that ever turned the 3.1.3 display
  on: every earlier screenshot (spinner, logo + bar) and §124's /vram dump were
  its frames, which is why no SpringBoard patch in §119–§124 changed them.
- **[V]** SpringBoard starts it: the function at Thumb `0x36c1`–`0x37ec` of the 7E18
  executable compares `CFPreferencesCopyAppValue("SBLastSystemVersion",
  "com.apple.springboard")` with `_CFCopySystemVersionDictionary()[ProductBuildVersion]`
  and, if they differ, runs `_DMPerformMigration` on a thread while its main
  thread spins a run loop until it is done (log string "Migration complete").
  On the 3DS the migrators never finish. **Fix (shipped):** the 3.1.3 overlay
  writes `com.apple.springboard.plist` with `SBLastSystemVersion = 7E18` (plus
  the 4.2.1 debug keys and `SBDisableMailAutoLaunch`). DataMigrator no longer
  runs.
- **[V]** With DataMigrator gone the display stayed off (CLCD `on=0x3`,
  base still iBoot's `0x18000000`, all five /vram slots black): SpringBoard
  had never drawn a frame.

### Three real bugs on the way to a frame

- **[V]** **lockdown stubs.** `lockdown_connect`, `lockdown_checkin` and
  `lockdown_copy_identity` are ARM functions (even symbols, `push` at +0).
  The shipped stubs were Thumb bytes (`20014770`/`20004770`), which execute as
  ARM `subvc r0,r7,r0,lsr #2` and fall into the body with its `push` skipped.
  Now `mov r0,#1|#0; bx lr` (ARM), original 8 bytes verified.
- **[V]** **GL dispatch table.** With the 4.2.1 `MBXGLEngine` (and the
  `AppleMBXDevice` → `IOResources` match string, as on 8C148) the engine passes
  its PICA200 self-checks and creates SpringBoard's context, then
  `malloc: incorrect checksum for freed object`. 7E18 OpenGLES's
  `__GLIFunctionDispatchRec` has **822** slots, not 841 (from the ObjC
  `@encode` in the 7E18 cache): 4.x slots 761–776 and 838–840 do not exist,
  so 777–837 sit 16 lower. `tools/ios3ds-gl` now also builds
  `build/rootfs-add/ios3/MBXGLEngine` with `-DGL_IOS3`, which remaps slots
  when filling OpenGLES's tables (the internal scene-check tables keep the
  4.x numbering); only the 3.1.3 overlay installs it. The 3.1 EGL interface
  table has the same first nine entries as ours.
- **[V]** **IRQ replay (the big one).** Darwin 9/10 `_fleh_irq` starts
  `sub lr,lr,#4; mrs sp,spsr` (`04e04ee2 00d04fe1`, both 5H11a and 7E18);
  8C148's second instruction is `mrs sp,cpsr`. The shim's CLCD-frame-IRQ path
  replays the two displaced instructions and always used the 4.x form, so
  every injected frame IRQ that interrupted user mode was treated as a
  kernel-mode interrupt and the user thread died (crash report: SpringBoard,
  `KERN_INVALID_ADDRESS`, `pc 0xc00684c0`, i.e. inside the kernel IRQ
  continuation in user mode). `versions.py` now marks 2.2.1/3.1.3
  `irq_spsr`, `scripts/kpi.py` emits `IOS3DS_KPI_KERNEL_IRQ_SPSR`, and
  `kernel/shim/entry.S` assembles `mrs sp,spsr` only then. The 4.2.1 shim
  hash is unchanged.

### Result: a 3.1.3 home screen

- **[V]** With the three fixes, a fresh `make sdimg IOS=3.1.3` boots to the
  stock 3.1.3 icon grid (two pages, dock Music/Videos/Safari/App Store, status
  bar time) in the 195–225 s screenshots at `--icount 2` (screenshot kept at
  `build/shots/313-home-t6.png`, not committed); one SpringBoard pass.
- **[V]** The §121–§123 SpringBoard patches only fought the DataMigrator screen
  and are removed: `isLocked -> NO` (`0x1c47c`), the `appleIconViewRemoved`
  → `restoreIconList:` trampoline (`0x48bc`, `0xda860`) and its three gates
  (`0xb9b2`, `0xb9d2`, `0xb9e8`), and the `SBUIController init`
  `makeKeyAndVisible` literal (`0xd478`). The icon grid still appears.
  `SBTetherController isTethered -> NO` and the QuartzCore display-kind → EAGL
  patch stay: without the kind patch a one-tap soak never gets a stable home
  frame and SpringBoard crashes (scratch run, reverted).
- **[V]** `ios3ds-input` is now built a second time with `-DINPUT_IOS3`
  (7E18 IOKit has no `IOHIDEventSystemClient*`): it waits for
  `PurpleSystemEventPort` and sends GSEvents only (7E18 `GSEventGetHandInfo`
  / `GetPathInfoAtIndex` / `GetWindow` use the same record offsets as 8C148).
  Taps arrive: a tap on Settings makes SpringBoard load
  `Preferences.framework` and `SBSCopyFrontmost…` reports
  `com.apple.Preferences`.

### Still open

- **[V]** CoreAnimation composites only the **first** frame. The clock stays at
  the minute of that frame, a launched app (Settings) never appears, and
  `[gpu] mirror` reports only idle VBlanks afterwards. The engine sees no
  `BindCoreSurface`, `BindView` or `SwapNotification` call at all, so the
  EAGLServer is not rendering through GL; the CLCD page shows the frame-IRQ
  enable (`+0x08` bit 0, same layout as 8C148, 7E18 enable/ack code at kernel
  file `0x5a6bbc`) clear, i.e. QuartzCore's vsync source (`IOMFBServer`,
  whose run-loop thread is idle in `mach_msg`) is not asking for frames.
  `render_for_time` is only called from `IOMFBServer::link_callback`.
- **[G]** Next: trace why 7E18 `EAGLServer::render_update`/`renderer()`
  (`0x31bda440`/`0x31bda370`) does not use the EAGL context, and who enables
  vsync notifications after the first swap.
- **[V]** A one-tap soak (`--cycles 1 --targets 47,314 --no-extras`) prints
  **Result: PASS** (0 failures, 0 crashes) but its own log says "the tap did
  not open an app"; that check is informational in `soak.py`. No 3.1.3
  five-cycle soak PASS is claimed. 2.2.1 was not attempted this round (it
  shares the IRQ-replay fix).

### Verification of this round's tree

- **[V]** `make test`: offline unit tests (incl. the updated 3.1.3 recipe
  test), GL host tests, Wi-Fi/GPS/input/ALS/blkcache harnesses and the stub
  symbol check pass.
- **[V]** Fresh default build (`make userland`, `rm -f build/sd.img`,
  `make sdimg`) reproduces the six 4.2.1 hashes: `kernel 505f451a…`,
  `devtree 25b1bd23…`, `fakeio e5fecd66…`, `shim e0609de9…`, `gpu 7e696f65…`,
  `boot.cfg 8d4079cf…` (the shim's new `.if` assembles the 4.x path).
- **[V]** `rm -f build/sd.img && make sdimg && python3 emu/soak.py --cycles 5
  --icount 2` → **Result: PASS** (SpringBoard + input after 70 s, warm-up
  135 s, home reference 145 s with stddev 53.6, five app/home cycles, 0
  restarts, never "not home after Home").
- **[G]** The 4.1 soak was not re-run this round (8B117 not staged in this
  worktree); 4.1 keeps `irq_spsr` unset, so its shim source path is unchanged.

## 126. Issue #9 round 2: the chainloader still runs the old payload, and nothing updates it (2026-11-25)

Retrow26 retested with a new build (6 labelled logs, 2026-11-25 13:31Z):
hbmenu boots, every chainloader boot fails or shows the graphics error,
with `gpu=0`, `diag=1` or both.

**Evidence.** The first line already settles it. The hbmenu log:

    ios3ds m1-0.4.0-209-ga43f415-20261125.0639  (M1 loader)
    CFG9_SDMMCCTL 0248 -> 0248
    launcher swap: restoring Luma as sd:/boot.firm

All five chainloader logs (plain, failed, gpu=0, diag=1, gpu=0+diag=1):

    ios3ds m1-0.3.0-62-g6d963b6-20261121.0001  (M1 loader)
    CFG9_SDMMCCTL 0200 -> 0200
    boot.cfg:41: unknown key 'boot_delay'   (… hp_codec, auto_brightness, gps, stereo3d)

and no `payload check` line at all. `argc=2`, `cpsr=600000d3` and the
framebuffer addresses (`top 0x18300000/0x18400000 bottom 0x18346500/0x18446500`)
are identical in all six, so Luma's chainloader and boot9strap hand over the
same screen-init state and argv framebuffers; the ARM11 reaches `ready` in
both. The remaining diffs (SDMMCCTL, unknown keys, timings) are build
differences of the 0.3.0-62 binary, as in round 1. `gpu=0`/`diag=1` change
nothing because they are read by a loader that predates the shared layout of
the current shim.bin/gpu.bin.

**Root cause.** Round 1's diagnosis (old `luma/payloads/ios3ds.firm` against
new shim.bin/gpu.bin) is right; its fix could not reach this user.
`firm_is_stale()` runs inside the *chainloaded* payload, but the chainloaded
payload is 0.3.0-62, which has no such check. The current build only ever ran
from the launcher, i.e. from `ios3ds/ios3ds.firm`, where the check compares the
file with itself and passes. So no code path ever touched
`luma/payloads/ios3ds.firm`, and the "copy it by hand" instruction from the
round-1 reply was evidently not applied (or went to a different file name).
No code-level difference between the two entry paths was found.

**What round 1 got wrong.** It assumed the self-update would trigger on the
next chainload; that needs the new code to already be in the payload
(chicken-and-egg). It also only knew the one file name `ios3ds.firm`.

**Fix.** `boot/arm9/swap.c: heal_payloads()`: when the running build matches
`ios3ds/ios3ds.firm` (so it is the current build, whichever way it started),
it scans every `*.firm` in `sd:/luma/payloads/`. Files containing the loader
banner `ios3ds m1-` but not this `IOS3DS_VERSION` are overwritten with
`ios3ds/ios3ds.firm`; each decision is logged (`payload check: … is this
build` / `… is an older ios3ds build, updating it` / `… updated`). Booting once
from hbmenu therefore repairs the chainloader copy, under any file name.
Non-ios3ds payloads are never touched. `boot/arm9/main.c` also logs
`argv0="…"` (the path Luma/b9s booted) on the `ARM9:` line, so the next log
shows exactly which file ran.

- **[V]** `make test` passes; `make shim`: main part 24268/24320, GPU part 26304/32768.
- **[V]** Fresh soak `rm -f build/sd.img && make sdimg && IOS3DS_QEMU=… python3 emu/soak.py --cycles 5 --icount 2` printed **Result: PASS**.
- **[HW]** (1) Boot once via hbmenu: the log must show `payload check: 0:/luma/payloads/<name>.firm is an older ios3ds build, updating it` then `… updated`. (2) Chainload: the first log line must be the same version as the hbmenu log, `argv0=` must name the payloads file, and SpringBoard must show with working buttons. If the chainloaded log *still* shows an old version, the chainloader is booting a file outside `luma/payloads/` and `argv0` names it.

## 127. Stereo 3D on the Old 3DS: GPU present for the stamped frame, batched stamps, per-mode mirror cost (2026-11-25)

Goal: SpringBoard in real 3D on the Old 3DS, about as smooth as 2D. Input:
hardware run 3 (`stereo3d=1`: 229-301 ms ARM9 copies, slider "did nothing")
was before re-log 109/113; this round has no new hardware log, so every
claim below is tagged [V] (verified on the host model / QEMU) or [G]
(reasoned, needs a console).

### Design ("full 3D" as implemented)
- Depth: five classes from the CA layer tree's draw order and position
  (`boot/common/stereo.h`): wallpaper behind (−k), icon grid at the screen
  (0), dock + status bar in front (+k), folders/alerts further in front
  (+2k); k = 1..4 px from the slider. Stamped into the frame's alpha by the
  GL engine, applied as per-class eye offsets by the PICA200 mirror. [V]
- Slider up + home frame: both eyes on the GPU (two-eye list); slider down
  or an app frame: flat list, barrier and 3D LED off; ARM9 copy only as
  fallback (unchanged from 109). [V in QEMU]
- No tearing between eyes: both eyes are built from one uploaded texture in
  one GPU frame and read out to the two PDC0 sets together. [G on panel]

### Changes
- **The stereo display target uses the GPU present again** (`pica_be.c`).
  The swizzle pass writes output R = sample alpha, which is the surface's
  alpha byte, so the stamp survives; re-log 106's "alpha 0 in every word"
  was the same round's unstamped-frame bug, not the present. New host check
  `alpha_stamp_check` (sw, read-back, direct present, arena copy): 0 bad
  samples. [V] This removes a 600 KiB CPU read-back per CA frame with
  stereo on (re-log 78: 31-37 ms read-back vs 13.5-16 ms present on
  hardware). [G: the saving on hardware]
- **Deferred, batched stamp** (`stereo.c`): one alpha-only draw per home
  frame instead of one per CA draw; app frames only get the wipe. Scissor
  changes start new segments. `stereo_forget()` drops pending stamps when
  the target is freed (found by ASan in the new host test). [V]
- **Host test** `IOS3DS_GLTEST_STEREO=1` (in `make test`): wallpaper/icons/
  dock/status bar get 0x33/0x66/0x99/0x99 through stamp + GPU present,
  RGB untouched, the following app frame wiped to 0, 1 batch of 66 vertices. [V]
- **`[stereo] mirror GPU cost`** every 30 s: 3D vs 2D frame and draw-stage
  averages from new per-mode mailbox counters (`st_n/st_sum/st_sum_3d`,
  `fl_*`, shim GPU part +48 bytes). [V]
- `emu/perf.py --slider3d N`.

### Sizes
`make shim`: main 24268/24320 (unchanged), GPU part 26352/32768.
`gpumirror.bin` unchanged (32336/32768).

### Results
- `make test` green.
- Fresh-image soaks, `--cycles 5 --icount 2`, private QEMU
  (`/tmp/opencode/qemu-compat2`): `stereo3d=0` **PASS**; `stereo3d=1
  --slider3d 255` **PASS** (22 barrier-on transitions as apps closed,
  `GPU present check passed (320x480)`, `[gl] CA: ... 0.1 CPU read-backs`,
  `CA stereo3D: 308 frames, 198 home, 110 flat, 5597 stamped draws in 205
  batched alpha draws, 74 wipes`).
- Mirror cost (QEMU, instruction-clocked, not a hardware prediction):
  `3D 158 frames avg 12648 us (draw 1936 us); 2D 78 frames avg 8079 us
  (draw 1435 us)` and `3D 58 avg 14539 us; 2D 23 avg 10796 us`.
- `emu/perf.py --runs 3 --icount 2 --slider3d 255`, fresh `stereo3d=1`
  images, same firmware, only the GL engine differs:

  | | before (origin/main engine) | after |
  |---|---|---|
  | `[perf] CA:` folder windows (guest time) | 25 frames/1167 ms avg 46 ms; 28 frames/1014 ms avg 35 ms | 39 frames/1057 ms avg 26 ms; 40 frames/1012 ms avg 24 ms |
  | frames <17 ms | 4 / 3 | 17 / 19 |
  | folder open/close wall (host-load noisy) | 8.1/6.2/12.1, 12.2/12.7/10.2 s | 16.3/16.4/18.4, 18.3/14.5/18.3 s |

  The guest-time histogram is the robust number: stereo-on animation now
  runs at the pace re-log 113 measured with stereo off (43-47 frames,
  22-23 ms avg). Wall times were taken under heavier host load (two other
  QEMUs) and are not comparable. [V in QEMU; G on hardware]

### Not done / needs a console
- The two-eye list still draws ~9 full-screen quads (4 per eye + clear) vs
  2 for flat. Cheaper options (FRONT pass limited to the status/dock bands,
  margin-only clear of the right eye) need ~2.5 KiB of vertices that the
  32 KiB blob does not have; a single-pass TEV class select needs > 6 TEV
  stages. Decide after the hardware `[stereo] mirror GPU cost` line.

### Hardware test checklist (Old 3DS, `stereo3d=1`, `diag=1`)
1. Boot, home screen, slider at max: photo straight on; wallpaper behind,
   icons at the screen, dock/status bar in front. Slider to the bottom
   notch: flat, 3D LED off. Slider up again: 3D within ~1 s, LED on.
2. Open an app with the slider up: flat, barrier off; Home: 3D again.
3. Open/close a folder and swipe pages in 3D and in 2D; compare
   `[perf] CA:` histograms (target: 3D avg close to 2D).
4. Lock (power), wait 10 s, unlock; close/open the shell: 3D comes back on
   the home screen, no stale eye images.
5. Send `log.txt` with: `[stereo] mirror GPU cost`, `[stereo] active path`,
   `[gl] CA stereo3D`, `[gl] CA: ... CPU read-backs` (must be ~0.1/frame),
   `GPU present check passed`, `[gpu] mirror:` (busy skips, idle VBlanks),
   any `switch FAILED`/`no shim`.
## 128. 3.1.3 composites every frame: the frozen picture was the GPU mirror, not CoreAnimation (2026-11-25)

Continues §125. Firmware staged from the earlier worktrees into this
worktree's `build/fw-{3.1.3,2.2.1,4.1}/` (Apple files stay under `build/`).

### What really stopped after the first frame

- **[V]** Hardware breakpoints (QEMU gdbstub on a private port, `emu/gdb/rsp.py`)
  on the 7E18 QuartzCore render path, 40 s on the shipped §125 image at the
  home screen: `IOMFBServer::link_callback` (0x31bdc944),
  `Server::render_for_time` (0x31bdfba0), `EAGLServer::render_update`
  (0x31bda440), `Server::render_update` (0x31bdf56c) and
  `CARenderOGLRenderDisplay` (0x31baee14) were each hit 15 times, plus 20
  `IOMFBServer::set_next_update`. CoreAnimation **does** render: every
  update goes `EAGLServer::render_update` → `renderer()` returns NULL →
  `Server::render_update` → `sw_renderer()`, i.e. CA's software compositor,
  which presents with `IOMobileFramebufferSwapBegin/SwapSetLayer/SwapEnd`
  (`IOMFBDisplay::finish_update`, 0x31bdb7a4; stubs resolved from the
  image's indirect symbol table). Vsync notifications are switched on per
  `set_next_update` for three frames (`[this+0xf0] = 3`, decremented in
  `link_callback`), which is why the CLCD frame-IRQ enable is clear on an
  idle home screen.
- **[V]** The picture froze in the **display mirror**: the shim's PICA200
  mirror only starts a frame when `m_dirty` is set, and only a GL present
  (`kernel/shim/pica.c` PRESENT/SCATTER) sets it. On 4.2.1 CA composites
  through our GL engine; on 3.1.3 the software compositor never calls the
  engine, so the mirror copied the first frame and then logged only
  "idle VBlanks (no new CA frame)", with the ARM9 CPU copy paused.
- **[V]** Fix: `scripts/versions.py` gains a per-version `boot_cfg`
  table, applied by `scripts/mkboot.py` only for non-default versions (each
  override replaces exactly one generated line and adds a comment). 3.1.3
  sets `gpu_mirror=0`: the ARM9 copies the CLCD framebuffer every frame.
  `tests/test_versions.py` checks that 4.2.1/4.1 have no override.
  The default build's six hashes are unchanged (`kernel 505f451a…`,
  `devtree 25b1bd23…`, `fakeio e5fecd66…`, `shim e0609de9…`, `gpu 7e696f65…`,
  `boot.cfg 8d4079cf…`).
- **[G]** Why `renderer()` gets no EAGL context (so CA could composite on
  the PICA200 again) is not traced; the software compositor is fast enough
  for the soak below. It is a performance item, not a blocker.

### Soak harness

- **[V]** `emu/soak.py --require-app` makes "the tap did not open an app" a
  failure (and lists such taps in the report), so a frozen home screen can no
  longer pass. Spotlight detection now also requires the frame to differ
  from the home reference: 7E18's first icon row reaches into the top 40
  points (mean ~40 > 35), which made every home frame look like Spotlight
  and sent the recovery onto page 2.

### Result

- **[V]** One-tap check (fresh image, `gpu_mirror=0` edited into the
  card's boot.cfg): Settings opens and renders, the status-bar clock moves
  from 5:58 to 6:00 between frames.
- **[V]** Fresh `make sdimg IOS=3.1.3`, `emu/soak.py --cycles 5 --icount 2
  --require-app --targets "47,314;123,226;199,226;275,226;123,50"` →
  **Result: PASS**: SpringBoard + input after 40 s, five cycles, app diffs
  161.6 / 150.3 / 108.6 / 84.8 / 93.0, home diff ≤ 0.1 after every Home,
  0 restarts, taps that opened no app: none, lock round passed. Settings,
  Notes, World Clock and Calculator rendered (screenshots kept under
  `build/emu-s2/`, not committed); the fifth tap came after the page-swipe
  round and landed on Spotlight's search page (also a rendered, non-home
  frame, left with Home).

## 129. 2.2.1 reaches its home screen, opens apps and passes a soak (2026-11-25)

Fresh `build/sd221.img` images (`scripts/mksd.py --ios 2.2.1`) traced with
hardware breakpoints on QEMU's gdbstub (private port); `exception_triage`
(0xc001b6a0) with the user state read from the current thread
(`[0xc0208d5c] + 0x1cc`, the layout `fleh_dataabt` stores) named every crash.

- **[V]** **Code signing.** 5H11a `vm_fault_enter` calls `cs_invalid_page`
  (0xc01046f6, Thumb) for tainted/unvalidated executable pages; with CS_KILL
  it kills the process. New 2.2.1 kernel patch `cs-invalid-page-allow`
  (`movs r0,#0; bx lr`): patched Apple dylibs keep their stock signature blob
  and load (re-signing still stalls dyld, §116). No `CODESIGNING` line since.
- **[V]** **liblockdown.** The re-log 114 stubs are back, without a re-sign,
  plus `lockdown_checkin`/`lockdown_copy_identity` (as 7E18). Two bugs in the
  old bytes: `lockdown_copy_value` began `ldr ip,[pc,#0x14]`, which loads the
  stub's own `bx lr` word as the table pointer (every caller faulted at
  0x30893c24, far 0xe12fff1e); now `#0x20`, as on 7E18. BrickState pointed at
  the `kCFBooleanFalse` *variable* (0x38241c48); objc_msgSend crashed on it;
  now the object `__kCFBooleanFalse` 0x38237524.
- **[V]** **DataMigrator:** `SBLastSystemVersion = 5H11a` (2.x SpringBoard
  has the same migration gate).
- **[V]** **MBX2D.** With activation fixed SpringBoard starts the render
  server (`CARenderServerStart`) and crashed in `CARenderMBX2DRender` →
  MBX2D.framework (no MBX hardware). SpringBoard's environment now sets
  `CA_ENABLE_MBX2D=0` (and the LK_/OGL variants): software compositor,
  `CADisplayIOMFB::finish_update` presents. `gpu_mirror=0` as on 3.1.3.
- **[V]** **Tools for 2.x.** 5H11a's dyld has no LC_DYLD_INFO/`dyld_stub_binder`,
  and its `pthread_getspecific` uses **r9** as the thread pointer. 2.x builds
  use `-target armv6-apple-ios2.0 -ffixed-r9` plus
  `tools/common/dyld2_glue.S` (a classic `dyld_stub_binding_helper` and the
  `__DATA,__dyld` words dyld fills). Without `-ffixed-r9` the input daemon
  died in dyld at 0x2fe1e724 on its first lazy bind.
- **[V]** **GSEvents on 2.x** have no `context` word: the record is 48 bytes
  (timestamp +0x18, infoSize +0x2c, hand info +0x30, paths +0x54, from
  GraphicsServices' getters and `_GSSendEvent`). `-DINPUT_IOS2` uses that
  layout; with the 4.x layout taps arrived but opened nothing.
- **[V]** **mcud** gets a `-DMCU_OLD` build (3.x and 2.x): no
  IOHIDEventSystemClient, so no brightness/accelerometer, but battery polling,
  the volume slider (AVSystemController) and the NoIdleSleepAssertion work
  (`[mcud] NoIdleSleepAssertion held`, `volume slider 32/63 -> iOS volume 0.51`).
- **[V]** **Battery service off on 2.2.1.** Once the shim registered its
  IOPMPowerSource, the next registry match (CrashHousekeeping) hit
  `IOPanic` in `IORegistryEntry::free` (freed while attached; backtrace
  `getMatchingServices` → `IORegistryIterator::reset` → `IOService::free`).
  `versions.py` `no_battery_ps` → `IOS3DS_KPI_MCU_NO_PMPS` makes `ps_update`
  return at once on 2.2.1 only (4.2.1 shim bytes unchanged).
- **[V]** **Audio relocation reverted.** `i2s-start-no-lrck-sync` exists in
  both kernels (7E18 0xc0565964, 5H11a 0xc0612b9c, state at +0x8c), but with
  it both kernels hung before the root mount (heartbeat timer stopped at
  59 ticks on 2.2.1, 32 md0 requests on 3.1.3). Audio DMA stays as stock
  ("could not start DMA: device is not ready" per stream, no sound).

### Result

- **[V]** Fresh 2.2.1 image, `emu/soak.py --sd build/sd221.img --cycles 5
  --icount 2 --require-app --targets "275,226;199,226;123,226;47,226;275,50"
  --crashlogs` → **Result: PASS**: input after 50 s, Settings, Notes,
  Calculator, Clock and Contacts opened (app diffs 163.4/128.1/84.3/109.8/
  173.6), home diff 0.0 after every Home, 0 restarts, 0 panics, no crash
  reports, lock round passed (screens: `build/emu-s221d/`, not committed).

### Lock/wake on 3.1.3 and 2.2.1, and the ten-cycle soaks

- **[V]** A 10-cycle 3.1.3 soak exposed a hollow lock round: after Lock
  (`[power] iOS screen off`) the Home GSEvent never brought the CLCD back,
  the ARM9 kept showing the last home frame (so the screenshot test passed),
  and every later tap opened nothing. 2.2.1 behaves the same. Not fixed:
  the 3.x/2.x input build (`INPUT_IOS3`) no longer maps SELECT/POWER to the
  Lock button and both overlays set `SBAutoLockTime = -1` (Never). The
  4.2.1 input daemon is byte-identical. **[G]** The wake path (SpringBoard
  undim → AppleM2CLCD enable on these kernels) is the next thing to trace.
- **[V]** Fresh images, `emu/soak.py --cycles 10 --icount 2 --require-app
  --crashlogs` (lock rounds on cycles 4 and 9, the latter 130 s):
  **3.1.3 PASS** (40 s to input, ten apps opened, app diffs 78.8–161.5,
  home diff 0.0, 0 restarts, 0 crashes) and **2.2.1 PASS** (50 s to input,
  ten apps opened, app diffs 54.1–173.6, home diff 0.0, 0 restarts,
  0 crashes). Screenshots: `build/shots/{313,221}-{home,apps}.png` (not
  committed).
- **[V]** `make test` passes. Driver matrix: docs/multi-version.md §6.9.

### Verification after rebasing onto origin/main (82d308c)

- **[V]** `make test` passes. The default 4.2.1 build gives `kernel 505f451a…`,
  `devtree 25b1bd23…`, `fakeio e5fecd66…`, `shim a35631eb…`, `gpu 6bdaf617…`,
  `boot.cfg 8d4079cf…`. The shim/gpu values differ from §125's because main's
  stereo round (§127) changed them; building origin/main's `kpi.h`, `mcu.c`
  and `mkboot.py` in this tree gives the same six hashes, so this branch
  changes no 4.2.1 byte.
- **[V]** Fresh 4.2.1 image, `emu/soak.py --cycles 5 --icount 2` → **Result:
  PASS** (input after 70 s, warm-up 135 s, five app/home cycles, home diff 0.0).
- **[G]** 4.1 (8B117, now staged here) was not re-soaked; its shim path has no
  new flag set.

## 130. Audio on 3.1.3, 2.2.1 and 4.1: the audio IRQ glue's hard-coded 4.2.1 address, mic routes (2026-11-25)

### Why relocating `i2s-start-no-lrck-sync` hung 3.1.3/2.2.1

- **[V]** The patch sites from re-log 129 are right. In 7E18 (0xc0565964) and
  5H11a (0xc0612b9c), the `AppleS5L8900XI2SController` DMA start is
  instruction for instruction the 8C148 function (state at +0x8c instead of
  +0xb0, the same wait loop, enable/disableInterrupt and timer calls).
- **[V]** Reproduced on 3.1.3 with only the patch: the kernel boots further
  than re-log 129 saw this time, and the first UI sound gives
  `[audio] iOS output stream #1 started`, `tx start ch5 ...`, then
  `sleh_abort: prefetch abort in kernel mode: fault_addr=0x80066494`.
- **[V]** Cause: `kernel/shim/audio_glue.S` hard-coded 4.2.1's XNU IRQ
  continuation (`.equ KERNEL_IRQ_CONT, 0x80066494`) and replayed `mrs sp,cpsr`.
  `entry.S` already used `IOS3DS_KPI_KERNEL_IRQ_CONT`/`_SPSR`, but the glue
  that injects the PL080 TC interrupt (return code 2) did not. So the first
  completed DMA buffer on a 0xC0000000 kernel jumped into unmapped memory.
  Before the patch, DMA never started, so the bug never showed.
- **Fix:** the glue includes `kpi.h` and uses the same two KPI macros as
  `entry.S`. 4.1's continuation is also 0x80066494 (`scripts/kpi.py`), so 4.1
  was never affected. **[V]** The six 4.2.1 hashes are unchanged (below).

### Mic input on the old versions

- **[V]** The 3.x build of `ios3ds-tone rec` reported `input available 0`, then
  `AudioQueueNewInput` 'nohw'. This is the same N72 routing wall as 4.2.1
  (docs/audio.md §7), but the 4.2.1 file patch (VirtualAudio @0x287228) was
  dropped on 4.1 (`null`) and absent on 3.x/2.x.
- 4.1's table (`K48 K66 N18 N72 N81 ...`) has N81 at 0x1c9d44: the overlay now
  maps the base patch to 0x1c9d40 (same orig/new bytes).
- 7E18 (`M68 N18 N45 N72 N82 N88`, N72 at 0xfa800) and 5H11a
  (`M68 N45 N72 N82`, N72 at 0x9a358) have no N81, so N72 becomes `X72` and
  N82 (iPhone 3G) becomes `N72`. On 3.1.3 output stays on route "Speaker" and
  input reports `SpeakerAndMicrophone`.
- **[V]** 7E18's input chain does not add 4.2.1's +23.65 dB: with the shim's
  2151/32768 gain, a 1000-amplitude QEMU sine came back at 46.6. New
  `versions.py` key `mic_gain_q15` → `IOS3DS_KPI_MIC_GAIN_Q15` (`audio.h`);
  3.1.3 uses 32768 (unity, so the iOS-side level is about -3 dB). 4.2.1 keeps 2151.

### Test client

- `ios3ds-tone` is now built for 3.x (`build/rootfs-add/ios3/`) and 2.x
  (`ios2/`, 2.0 target, r9, dyld2 glue) with `-DTONE_OLD` (start trace
  lines), and the old overlays install it. As on 4.2.1, no job is shipped.
  `tools/ios3ds-audio/tone/com.ios3ds.tone.old.plist` is the job without the
  ios3ds-diag wrapper, for test images.

### Results (QEMU, plain TCG)

- **[V] 3.1.3** (`build/emu-a2`, `a3`, `a4`): boot UI sounds play: streams
  #1-#4, `[audio] stream stopped: ... TC IRQs 16 (16 injected), bad addr 0`,
  and the CSND model shows 50,011 non-silent frames, peak 8120. The test job
  `ios3ds-tone 2 5 1500 1000 6000 70` → `tone: #1 ... route "Speaker"`,
  `#1 done`, `#2 done`, stream peak 5999 (= amplitude 6000), 34/41 TC IRQs,
  SpringBoard and input up, no panic. Mic: `rec start: input available 1,
  route "SpeakerAndMicrophone"`, `[mic] 1 s: FIFO peak 1000 rms 707.0`,
  `tone: rec 88200 of 88200 frames`, `sine fit: 439.42 Hz` (gain then still
  2151: amplitude 46.6, hence the per-version gain above). The `rx stop` line
  shows 734 underruns without `--icount` (host load); 4.2.1 measured 0 with
  `--icount 2`.
- **[V] 2.2.1** (`build/emu-b1`, `b3`): the I2S patch no longer hangs the
  kernel (SpringBoard and input up, heartbeat runs through 420 s, no panic).
  But no stream ever starts. 2.2.1 plays no boot UI sound, and the test job
  logs `tone: start` and `AudioToolbox loaded` and then nothing: it blocks in
  `AudioSessionInitialize` (the 60 x 2 s retry loop never prints a failure).
  mediaserverd's launchd job exists. **Unfinished [G]:** find out whether
  mediaserverd runs and reaches IOAudio on 5H11a (next: `ps`-style dump or
  a launchd stdout path for mediaserverd). The mic swap @0x9a358 is applied
  but cannot be tested until then.
- **[G] 4.1:** not run this round. Its I2S patch and IRQ path were already
  right (same continuation address). The new VirtualAudio relocation needs one
  `ios3ds-tone rec` run.
- Keyboard click, the test song and Music: **not verified** on 3.x/2.x. The
  click is an ordinary UI sound (same path as the boot sounds that play on
  3.1.3). `scripts/music.py` writes a 4.x `iTunes Library.itlp`, and the
  MusicLibrary signature patch is not relocated for 7E18/5H11a (2.x uses the
  older iTunesDB format), so the song most likely does not appear there.
- Volume buttons: mcud `-DMCU_OLD` → AVSystemController (re-log 129). On
  3.1.3 the shim logs `[audio] iOS volume: amp step 22 -> CSND volume`, so the
  amp-step → CSND volume path works there.

### Hardware checklist (diag=1 log.txt)

1. 3.1.3 boot: a `[audio] iOS output stream #N started` / `stream stopped:
   ... TC IRQs n (n injected), bad addr 0` pair for each UI sound, no
   `sleh_abort`, no `could not start DMA`. You should hear the unlock/tap
   clicks from the speaker.
2. Press the volume buttons: `[audio] iOS volume: amp step S -> CSND volume
   V/32768` changes, and the loudness follows.
3. Plug in headphones: `[audio] headphones in ... iOS route -> headphones`, and
   the sound moves.
4. Voice memo / `ios3ds-tone rec` on 3.1.3: `[audio] mic on: 11901 Hz`,
   `[mic] 1 s: FIFO peak ...` with speech-level peaks, and the iOS peak about
   the same as the FIFO peak (unity gain on 3.1.3). If the iOS peak is about 15x
   the FIFO peak or clips, 7E18 does add gain on hardware: lower
   `mic_gain_q15`.
5. 2.2.1: expect silence (above). Report whether any `stream started` line
   appears at all.

### 4.2.1 unchanged

- **[V]** The default build gives `kernel 505f451a…`, `devtree 25b1bd23…`,
  `fakeio e5fecd66…`, `shim a35631eb…`, `gpu 6bdaf617…`, `boot.cfg 8d4079cf…`.
  These are identical to origin/main built from a git archive. The 4.2.1
  ios3ds-tone binary is byte-identical too (`TONE_OLD` only in the 3.x/2.x
  builds).

### Soaks (fresh images, --icount 2)

- **[V]** 4.2.1 `emu/soak.py --cycles 5` → **Result: PASS**.
- **[V]** 3.1.3 `--cycles 5 --require-app --crashlogs` → **Result: PASS**
  (audio DMA error count 0; previously each UI sound logged
  "could not start DMA").
- **[V]** 2.2.1 `--cycles 5 --require-app --targets "275,226;199,226;123,226;47,226;275,50" --crashlogs`
  → **Result: PASS**.
## 131. iOS 4.1 completeness, and Cydia on 3.1.3/2.2.1 (2026-11-25)

Closes the 4.1 (8B117) open items of re-log 101/111 (docs/multi-version.md
§6.8) and turns Cydia on for the older versions.

### 4.1: the six skipped kernel patches

- **[V]** `clkctl-voltage-quiet` ported: 8B117 literal `0x807040a4`
  (`0x802e3815`), found by its pool neighbours (`function-core_voltage_%ld`
  string, "Bad DPSM Tables" panic string). The AppleARMPlatform lookup the shim
  calls is byte-identical at 8B117 `0x802e3814` (8C148 `0x802e4814`); it has no
  symbol, so `scripts/kpi.py` LITERALS gained `ARM_FUNCTION_LOOKUP` (default =
  the 4.2.1 value, `kernel/shim/kpi.h` fallback) and `quiet.c` uses the macro.
- **[V]** `iortc-wait-mcu` ported: IOKitInitializeTime is the same 0x30-byte
  function at `0x801a75dc`; its waitForService literal `0x801a7608`
  (`0x801b0eb9`) -> `@shim+0x34@`. Boot log: `ios3ds-mcu: 3DS RTC present: not
  waiting for IORTC`.
- **[G] not mine, reported:** `clcd-backlight-log-enable` and
  `clcd-backlight-retry-off` (lock/wake owner) and `gyro-usage`/`gyro-dispatch`
  (sensors owner) remain unported on 4.1.

### 4.1: the six skipped dyld-cache patches

- **[V]** `lockdown_copy_buid_identity`: **unnecessary**. 8B117's liblockdown
  has no such function (the cache has no `_lockdown_copy_buid_identity`
  string; 8C148 has two): `lockdown_copy_value` (0x4fbde4) is followed directly
  by `lockdown_copy_identity` (0x4fbe24). The two other unmapped liblockdown
  entries (copy_value body + table) were already replaced by the 8B117
  stub/table (re-log 101).
- **[V]** MusicLibrary `.cbk` check ported: 8B117 has no `_AOU243378_7_1`;
  the logic is inlined in `MLSSqliteVFSOpen` (0x30570718). The `bne` at
  0x305708c4 (file offset 0x5708c4) that sets the file's `.cbk` path and
  checksum flag for names in AOU243378_5's signed set -> `nop`, so every
  library file opens as a plain sqlite file (same effect as the 8C148 patch).
- VirtualAudio N72->N81 routing: ported by the audio round (re-log 130).
  **[G] not mine, reported:** the CoreMotion gyro capability mask (sensors
  owner) remains unported.

### Cydia/TLSFix on 4.1, and the hfsplus date bug

- **[V]** The base package set (Cydia 1.1.30, iOS >= 2.0) installs as is;
  the "built for 8C148" note was wrong. `overlay-4.1.json` enables Cydia and
  TLSFix, drops the 4.2.1 IconState layout (Cydia lands on page 2) and sets the
  virtual packages to 8B117's values (new recipe key `cydia_pseudo`: firmware
  4.1, Darwin 10.3.1, CoreFoundation 550.38.1; 4.2.1 has none).
- **[V]** First boot with Cydia stalled after launchctl (`launch_msg(): Socket
  is not connected`, EROFS on `/`): `/sbin/fsck*`, `mount*`, `fstyp*`,
  `newfs_hfs` were **0 bytes**. `scripts/cydia.py`'s `hfsplus ls` parser split
  the date on whitespace; 4.1's stock files are dated ` 7/ 5/2010` (space
  padded), so no stock file was seen and firmware-sbin's empty placeholders
  overwrote them (keep_stock never applied). The regex now matches the date.
  4.2.1's rootfs lists the same /sbin and identical dpkg status before/after.
- **[V]** Local repo (re-log 105 harness), fresh 4.1 image,
  `emu/cydia_perf.py --icount 2`: network after 66 s, `apt-get update` 0.83 s,
  download 0.23 s, `dpkg -i` 0.79 s, all rc=0, ALL DONE.
- **[V]** Cydia opens on 4.1 (`emu/tap.py`, page 2, slot 1): Cydia's home tab
  with "Network Error ... Cannot allocate memory" (offline run, no Wi-Fi
  config; same OOM family as re-log 75/112 for TLSFix pages).

### Cydia on 3.1.3 and 2.2.1

- **[V]** Both overlays enable the same package set without TLSFix (its
  Substrate is loaded by ios3ds-applog, which old versions do not install),
  with per-firmware virtual packages (3.1.3: Darwin 10.0.0, CF 478.52; 2.2.1:
  Darwin 9.4.1, CF 478.29). The date fix also matters here (2009 dates).
- **[V]** 3.1.3: Cydia icon on page 2; tapping it opens Cydia (tab bar,
  "Network Error ... Invalid argument" with no network).
- **[V]** 2.2.1: **not feasible with this package set.** Cydia installed and
  its icon appeared (page 1, row 4), but tapping it (`[input] down 123,314`)
  launched nothing in 180 s, with no console line or crash report. The armv6
  slice of Cydia 1.1.30 (and of the apt7/dpkg builds) carries
  `LC_DYLD_INFO_ONLY`, which 5H11a's dyld does not support (re-log 129).
  `overlay-2.2.1.json` keeps `cydia: false`; a 2.x set needs Cydia/APT builds
  linked for iPhoneOS 2.x (classic binding).
- **[G]** No APT refresh test on 3.x/2.x: ios3ds-wifid is not built for them
  (Wi-Fi owner). Hardware: open Cydia, Installed tab lists 34 packages.

### 4.1 driver list vs 4.2.1 (fresh-image soak log)

Touch/buttons, display, GPU (PICA200 arena ready), volume slider, no-sleep,
battery (IOPMPowerSource registered), mcud brightness path, Wi-Fi (cydia_perf
associated and fetched), Music library all as on 4.2.1. Gaps to the other
owners: gyro kernel/CoreMotion patches, CLCD
backlight log/retry patches (above); mic routing landed with re-log 130.

### Results

- **[V]** Fresh 4.1 image, `emu/soak.py --cycles 10 --icount 2 --require-app
  --crashlogs` -> **Result: PASS** (input after 90 s, ten apps opened, app
  diffs 41.0-112.2, home diff <= 5.1, 0 restarts, lock rounds ok, no crash
  reports).
- **[V]** 4.2.1 byte-identical: kernel `505f451a…`, devtree `25b1bd23…`,
  fakeio `e5fecd66…`, shim `a35631eb…`, gpu `6bdaf617…`, boot.cfg `8d4079cf…`
  from this branch and from `git archive origin/main` (2ee022f).
- **[V]** Fresh 3.1.3 image with Cydia, `emu/soak.py --cycles 5 --icount 2
  --require-app --crashlogs --targets "47,314;123,226;199,226;275,226;123,50"`
  -> **Result: PASS** (input after 50 s, five apps opened, 0 restarts, no
  crash reports; the audio DMA lines are the known stock state).
- **[V]** Fresh 4.2.1 image, `emu/soak.py --cycles 5 --icount 2` -> **Result:
  PASS** (input after 90 s, five apps opened, 0 restarts).
- 2.2.1 was not re-soaked: its effective recipe is unchanged (`cydia: false`;
  `cydia_pseudo` and the cydia.py parser only run with Cydia enabled).

## 132. 3.1.3/2.2.1 power: wake after Lock, brightness, the 2.2.1 battery panic, the lid (2026-11-25)

Worktree `oldwake`; firmware staged from `compat16` into this worktree's
`build/fw-{2.2.1,3.1.3,4.1}/`.

### Wake after Lock: the clock controller's performance-state wait

- **[V]** Same cause as 4.2.1's re-log 41: AppleS5L8720XClockController
  sleeps on its command gate after starting a performance-state change until
  a change-done interrupt clears a byte, and the fake clock/power page never
  raises it. Display sleep lowers the performance state, so the CLCD power
  path blocked there and Home never re-enabled the controller. 8C148's
  `s5l-perf-nowait` (byte `+0x38c`) was in the "skipped" list for 7E18/5H11a.
  The loop exists in both, with byte `+0x248` (`ldrb r3,[rX,#0x248]; cmp;
  bne -40` right after `commandSleep`, vtable `+0x90` on 7E18, `+0x98` on
  5H11a): 7E18 `0xc05f30ec` (r5), 5H11a `0xc05e2b40` (r6), both unique.
  Patch: `mov r3,#0; strb r3,[rX,#0x248]; nop`, as on 4.x.
- **[V]** With it: `[input] key 30` → `[input] screen off` → Home →
  `screen on` → the slide on the lock screen → home screen, and every later
  tap opens its app (3.1.3 and 2.2.1 soaks below). The input daemon's
  3.x/2.x build maps SELECT/POWER to Lock again, and both overlays drop
  `SBAutoLockTime = -1` (SpringBoard's default auto-lock, as on 4.2.1).
- **[V]** No "Where is my backlight?" flood on 7E18 after wakes (2 lines
  each for `enable_backlight`/`disable_backlight` in a 10-cycle soak), so
  the 4.x `clcd-backlight-retry-off` is not needed there.

### Brightness on 2.x/3.x

- **[V]** 2.2.1 and 3.1.3 SpringBoard keep `DisplayBrightness` in their own
  in-process `IOHIDEventSystem` (`IOHIDEventSystemCreate/SetProperty`); there
  is no `IOHIDEventSystemClient` to read it from. The Settings slider
  (`GSEventSetBacklightLevel` → SpringBoard) is saved at once as
  `SBBacklightLevel2` in mobile's `com.apple.springboard.plist`.
  `ios3ds-mcud -DMCU_OLD` (root, so not through CFPreferences) reads that file
  when its mtime changes (and every ~2 s anyway), takes the factor from the
  ARM9's screen-off flag (0 after Lock/auto-lock, 1 on wake) and drives the
  shim's LCD backlight through the existing `kern.ios3ds_mcuctl` path. Without
  the key it uses 0.5, SpringBoard's default. Auto-brightness (New 3DS
  camera ALS, `SBEnableALS` read from the same file) runs on top, as on 4.x.
- **[V]** QEMU, 2.2.1 Settings > Brightness: `[mcud] iOS brightness 0.50 x
  factor 1.00 -> 500/1000` at boot; slider to the left → `0.01 ... -> LCD
  backlight 120/1000 (minimum)` (QEMU `lcd-backlight` 90 → 50); to the
  right → `1.00 ... -> 1000/1000` (143); Lock → `factor 0.00 -> 0/1000`,
  wake → `1000/1000`.
- Four new stub-SDK symbols (`_CFDictionaryGetTypeID`, `_CFDictionaryGetValue`,
  `_CFPropertyListCreateFromXMLData`, `_stat`) in `scripts/ios-sdk-symbols.json`.

### 2.2.1 battery: AppleEmbeddedAudio zeroed IOPMrootDomain's retain count

- **[V]** First finding: Darwin 9 objects point at their `__ZTV` symbol
  itself; 7E18/8C148 point 8 bytes in (the constructors store `ZTV` vs
  `ZTV+8`; `IOService` slot of `getProperty(const char*)` is `+0xa4` on
  5H11a vs `+0x9c`). The shim's hand-indexed virtual calls (OSNumber
  `release`, the accelerometer nub's `getProperty("reg")`, the PE/gyro
  calls) therefore hit the wrong slot on 2.2.1 (`release` became
  `getRetainCount`, a leak; `getProperty` a different virtual). New
  `versions.py` `vt_bias` → `IOS3DS_KPI_VT_BIAS` (8 on 5H11a, 0 elsewhere)
  is added to every such offset; the 4.2.1 shim bytes are unchanged.
- **[V]** That was not the panic. QEMU gdbstub (private port), hardware
  breakpoint on `IOPanic` (0xc014c3ec): the message is
  `IOPMrootDomain: attached at free()`. A write watchpoint on the root
  domain's retain count (`+4`, read through `gRootDomain` 0xc0206468) after
  `IOPMrootDomain::batteryPublished` showed `0x00080009 -> 0` written by
  `str r0,[r6,#4]` at 0xc03efb30, called from `IOService::messageClients` ←
  `IOPMPowerStateQueue::checkForWork`: AppleEmbeddedAudio's power interest
  handler stores 0 to `messageArgument+4` (an `IOPowerStateChangeNotification`
  return value) for every message before it checks the type.
  `batteryPublished` → `publishFeature("DisplayDims")` queues
  `kIOPMMessageFeatureChange` with the root domain itself as the argument,
  so the store wiped its retain count; the next registry iterator release
  (CrashHousekeeping, BTServer) freed it.
- **[V]** New 2.2.1 patch `audio-pm-msg-no-store` (0xc03efb28, the store
  becomes `nop`; senders of the sleep messages pre-zero the return value).
  `no_battery_ps` is gone. QEMU: `ios3ds-mcu: battery 100%, charger,
  4200 mV: IOPMPowerSource registered`, no panic, and the status bar shows
  the charging battery; the lock screen shows the big charging battery.

### Lid (shell close/open)

- **[V]** The ARM9 locks on close (POWER tap) unless the shim's backlight
  says the screen is off, and wakes on open (HOME tap) only when it is. On
  3.x/2.x nothing drove the shim's backlight before, so open never woke iOS.
  With mcud's backlight (above): QEMU 2.2.1, `c` → `[mcu] shell closed:
  locking iOS (POWER tap)` → `key 30` → `screen off` → `LCD backlight 0`;
  `c` → `[mcu] shell opened: waking iOS (HOME tap)` → `key 40` →
  `screen on` → `1000/1000`, lock screen on the display.

### 4.1

- **[V]** 8B117 already carries `s5l-perf-nowait`; its mcud/input are the
  4.x builds (unchanged). Fresh `make sdimg IOS=4.1`, `emu/soak.py --cycles 5
  --icount 2 --crashlogs` → **Result: PASS** (input after 90 s, five apps,
  home diff 0.0, lock round passed, 0 restarts/panics/crashes). Lid in QEMU:
  `[mcu] shell closed: locking iOS (POWER tap)` → `key 30` → `LCD backlight
  0/1000`; `shell opened: waking iOS (HOME tap)` → `key 40` → `screen on` →
  `500/1000`. Battery: `IOPMPowerSource registered`.

### Soaks (fresh images, `--icount 2`)

- **[V]** 3.1.3, `--cycles 10 --require-app --crashlogs --targets
  "47,314;123,226;199,226;275,226;123,50"` → **Result: PASS**: input after
  60 s, ten apps opened (diffs 53.1–103.0; cycles 5–8 after the first lock
  and cycle 9 after the 130 s lock), home diff 0.0, lock rounds ended on the
  home screen, 0 restarts/panics/crashes. Log: two `key 30` →
  `[power] iOS screen off` → `LCD backlight 0/1000` → `screen on again` →
  `500/1000`. (An earlier 10-cycle run on an image without the new mcud
  failed one tap at cycle 7: the swipe round left Spotlight up and the tap
  never reached the input daemon (no `[input] down` line); not lock-related.)
- **[V]** 2.2.1, `--sd build/sd221.img --cycles 10 --require-app --crashlogs
  --targets "275,226;199,226;123,226;47,226;275,50"` → **Result: PASS**:
  input after 60 s, ten apps opened (52.8–109.0, cycles 5–9 after locks),
  home diff 0.0, lock rounds fine, 0 restarts/panics/crashes, no crash
  reports, battery registered.
- **[V]** 4.2.1: the fresh default build's six hashes equal origin/main's
  (`kernel 505f451a…`, `devtree 25b1bd23…`, `fakeio e5fecd66…`,
  `shim a35631eb…`, `gpu 6bdaf617…`, `boot.cfg 8d4079cf…`);
  `emu/soak.py --cycles 5 --icount 2` → **Result: PASS** (input after 84 s,
  five cycles, home diff 0.0, lock round passed, 0 restarts/panics/crashes).

### After rebasing onto origin/main (52c663f, old-version audio + 4.1 work)

- **[V]** `make test` passes; the six 4.2.1 hashes are unchanged (above).
  Fresh images, 10-cycle `--require-app --crashlogs` soaks: 3.1.3 **PASS**
  and 2.2.1 **PASS** (ten apps each, lock rounds at cycles 4 and 9, 0
  restarts/panics/crashes; 2.2.1 `IOPMPowerSource registered`).

### Hardware checklist

- 3.1.3/2.2.1: SELECT (or POWER) locks; HOME wakes to the lock screen; the
  slide unlocks; an app opens afterwards. `log.txt` should show
  `[power] iOS screen off` / `screen on again` and `[mcud] iOS brightness
  ... factor 0.00` / `1.00` around each lock.
- Settings > Brightness slider: the LCD dims/brightens
  (`[mcud] iOS brightness X x factor 1.00 -> LCD backlight N/1000`).
- Lid close → screen off and locked; open → lock screen.
- 2.2.1: battery level and the charging plug in the status bar; unplug the
  charger and check `[mcu]`'s flags and the icon follow.

### Not done

- Brightness follows the value SpringBoard saves to its plist (seen after
  each drag in QEMU); whether the LCD follows during a drag was not checked,
  and SpringBoard's live value stays inside its own process.
- 3.1.3's AppleEmbeddedAudio was not checked for the same unconditional
  store; its battery registration never hit it in any soak.

## 133. Wi-Fi on 4.1 and 3.1.3: a shim mailbox address, the 3.x Apple80211 event device and scan-result layout (2026-11-25)

Before: the driver matrix (docs/multi-version.md §6.9) had Wi-Fi "off: built
against 4.2.1 private Apple80211/IOKit APIs" on 3.1.3 and 2.2.1; 4.1 was not
listed. Goal: Settings toggle, scan, WPA2 join, DHCP and Safari on 4.1, 3.1.3
and 2.2.1.

### 4.1 (8B117): nothing to port

- **[V]** 4.1's kernelcache prelinks IOUserEthernet, its IOKit has the
  IOEthernetController API, and its configd WiFiManager imports the same
  IOKit/ioctl surface as 8C148. The 4.1 overlay already keeps the base
  recipe's `ios3ds-wifid`, `ios3ds-a80211` (configd) and `ios3ds-hwaes`
  (securityd). Fresh `make sdimg IOS=4.1`:
  - `emu/wifi_test.py --case ok --icount 2` → **PASS, 20/20** (SDIO, BMI,
    WMI, scan, association, 4-way handshake with the AP's TK/GTK, en0,
    DHCP lease, default route, DNS, HTTP fetch, no panic).
  - `emu/wifi_ui_test.py --keep-ca --psk <letters only>` → **8/8 PASS**
    (Settings > Wi-Fi scans and lists 3 networks, WPA2 join through the
    on-screen keyboard, 4-way handshake, DHCP lease on en0).
  - `emu/safari_test.py --pages 2` → page 2 **loaded in 29 s** (host served
    the HTML and every image, screenshot shows it). Page 1, the cold start,
    got Safari's "Cannot allocate memory" alert: the first-big-page memory
    wall of §104 (its fix is 4.2.1's TLSFix/`/usr/lib` layout), not the
    network path.
- **Test harness:** with `ca_gpu=0` (what wifi_ui_test forces) 4.1 draws a
  black home screen, and its `123` keyboard page does not match the 4.2.1
  coordinates, so the passphrase was typed wrong (`message 2 MIC is wrong`
  in the AP model). New options: `--keep-ca`, `--psk` (the AP model's
  passphrase and the typed one; letters only avoids the 123 page),
  `--wifi-row/--net-row/--join-key`, `--key-pause`.

### 3.1.3 (7E18)

- **Kernel panic, cause:** `ios3ds-wifid`'s first `kern.ios3ds_net` sysctl
  took `kernel abort type 4 … fault_addr=0x808ae000` at pc `0xc0841308`
  (shim). The shim's net code used `NET_VA` = 0x808A0000, 4.2.1's mapping
  of `NET_PA` 0x208A0000 (virt base 0x80000000); Darwin 10 maps physical
  0x20000000 at 0xC0000000. **Fix:** `tools/ios3ds-wifi/shim/net.c` derives
  `NET_VA` from `IOS3DS_KPI_VIRT_BASE`. The 4.2.1 shim is byte-identical
  (same value). The KPI addresses (sysctl_register_oid etc.) were already
  relocated by kpi.py.
- **[V]** After that, with the 3.1.3 overlay installing `ios3ds-wifid`,
  `ios3ds-nettest`, `ios3ds-a80211` (configd `DYLD_INSERT_LIBRARIES`) and
  disabling Apple's `wifiFirmwareLoader`: `emu/wifi_test.py --case ok`
  → **PASS, 20/20**. 7E18 has IOUserEthernet and the IOEthernetController
  API; the 4.2-built daemon runs unchanged.
- **WiFiManager (statically linked Apple80211, symbols present):**
  - `CARD_CAPABILITIES` (GET 12) is also asked with `len 3` (WiFiManager and
    IPConfiguration); the bitmap is now written bytewise.
  - No PF_SYSTEM event socket in 3.x. `Apple80211EventMonitoringInit`
    (WiFiManager 0x1a6cc) does GET 23 (len 1023) for a device name, builds
    `"/dev/%s"`, opens it with `CFReadStreamCreateWithFile` and reads one
    record per callback (0x21bc4): `{u32 event, u32 length, payload}`, the
    read must return exactly 8 + length bytes, events 1–14 filtered by a mask
    that `Apple80211StartMonitoringEvent` enables (SET 66) for 1–10 and 14.
    Without it the Settings list kept spinning: SET 10 went out, the scan
    finished, no GET 11 ever followed. The dylib now answers GET 23 with its
    own name, accepts SET 66, and interposes `CFReadStreamCreateWithFile`
    for that path: WiFiManager gets the read end of a datagram socketpair
    (`CFStreamCreatePairWithSocket`), fed with the same events as the 4.x
    socket (scan done 10, association done 9, link 4, SSID 2, BSSID 3).
  - Scan results are 144 bytes on 3.x (140 on 4.x); the IE length/pointer
    are at 0x88/0x8C (WiFiManager 0x1f174, `ParseRSNIE` callers at
    0x1b034) instead of 0x86/0x88. With 4.x offsets WiFiManager saw no RSN
    IE and tried a WEP join ("key type 2 / 13 bytes"). `fill_result` picks
    the layout by request length.
  - The keychain works on 3.x without `ios3ds-hwaes` (no AppleKeyStore
    wrapping before 4.0).
- **[V]** `emu/wifi_ui_test.py --psk <letters only> --wifi-row 160,96
  --key-pause 2` (Settings' first row is Wi-Fi on 3.x; the software
  compositor drops keys at the default pace) → **8/8 PASS**: scan lists 3
  networks with lock icons, WPA2 join via the keyboard, `Joined`, `Added …
  to list of known networks`, 4-way handshake, DHCP lease, check mark in
  the list. A second boot of the same image auto-joined from the keychain
  ("Auto join scan found …", association done, `en0 configured` 70 s after
  start) and the status bar shows the Wi-Fi bars.
- **[V]** `emu/safari_test.py --pages 3 --settle 240` (overlay now also
  installs `ios3ds-diag` + `com.ios3ds.urltest.plist`, gated by the
  `ios3ds_urltest` boot-arg as on 4.2.1): pages 2 and 3 **loaded in 10 s**
  each (screenshot of page 3 rendered with its images). Page 1, the cold
  start, showed "Cannot Open Page … Invalid argument": same pattern as 4.1
  (first load after Safari's launch fails, every later one works); not
  investigated further.

### 2.2.1 (5H11a): still off

- 5H11a's kernelcache has **no IOUserEthernet** (prelinked bundles:
  IO80211Family, IOSDIOFamily, l2tp/ppp/pptp, audio, flash, …), so
  `ios3ds-wifid` has nothing to create en0 with; there is no tun/utun either.
- Its Wi-Fi manager is a different plugin, `Aeropuerto.bundle`, linked
  against a **dynamic** `Apple80211.framework` (`Apple80211Open/Scan/
  AssociateEmbeddedAndCopyInfo/EventMonitoringInit/…`).
- Plan: a kernel `ifnet` (Darwin 9 KPI `ifnet_allocate/ifnet_attach/
  ifnet_input`, output callback into the netmbox) behind a
  `IOS3DS_KPI_*` flag. The shim's main part has 52 bytes free, so it has to
  live in the GPU part or a new 2.2.1-only area; then a 2.x variant of the
  a80211 bridge that interposes the exported Apple80211 functions instead
  of the ioctls. Not started in this round.

### Hardware checklist (diag=1 log.txt)

- 3.1.3: `[a80211] loaded into configd`, `[a80211] 3.x event stream opened`,
  `[a80211] scan done: N networks` followed by GET 11 (with
  `IOS3DS_WIFI_LOG=2`), `[a80211] associate … (WPA2-PSK, PMK from
  WiFiManager …)`, `[wifid] en0 configured by configd`. Everything below the
  mailbox is the shared ARM9 driver (§103 for the HTC credit refill).
- 4.1: the same lines as 4.2.1 (§52).

### Verification

- **[V]** 4.2.1 hashes of a fresh default build equal origin/main's (built
  from `git archive origin/main`): kernel `505f451a…`, devtree `25b1bd23…`,
  fakeio `e5fecd66…`, shim `a35631eb…`, gpu `6bdaf617…`, boot.cfg
  `8d4079cf…`.
- **[V]** Fresh-image soaks, `--cycles 5 --icount 2` (3.1.3 and 2.2.1 with
  `--require-app` and the §128/§129 targets): **3.1.3 PASS, 2.2.1 PASS,
  4.1 PASS, 4.2.1 PASS**.
- **[V]** 4.2.1 with the changed `ios3ds-a80211.dylib`: `emu/wifi_ui_test.py`
  (default options) **8/8 PASS** (a first run tapped the wrong network while
  Settings was still drawing; the rerun passed). `make test` passes (the
  three new CoreFoundation imports are in `scripts/ios-sdk-symbols.json`).
- **[V]** After rebasing onto origin/main (1add5cf, with the old-version audio,
  power and Cydia rounds): 4.2.1 hashes still equal origin/main's (same six
  values), `make test` passes, 3.1.3 `wifi_test --case ok` **PASS**, fresh
  3.1.3 soak (`--require-app`, §128 targets) **PASS**, fresh 4.2.1 soak
  **PASS**.
## 134. 2.2.1 audio: mediaserverd is alive, a TLS-clobbering test client was the wall (2026-11-25)

### mediaserverd / IOAudio on 5H11a

- **[V]** A scratch launchd job running `/bin/launchctl list` to `/dev/console`
  (as in §117) lists `com.apple.mediaserverd` with pid 14 on a fresh 5H11a
  image. Setting `StandardOutPath`/`StandardErrorPath` to `/dev/console` in its
  plist shows it running, loading VirtualAudio and reaching the emulated
  audio CODEC over I2C (`ios3ds-audio: iic … dev=2c00` from
  `AppleCS42L58Audio::start`): the daemon and the kernel audio driver both run.
- **[V]** SELECT+START thread dump: mediaserverd has 7 live threads, idle in
  `mach_msg`; the tone process dies with signal 10 (no `AudioSessionInitialize`
  result). With the §130 VirtualAudio product-table swap installed,
  mediaserverd logs `RoutingManager::ActivateRoute(): Routing Not Supported`,
  `CreateVirtualAudioDevice(): … Unable to set category`, `cmsmVADRouteChangeListener:
  new route has no ports`, and `AudioQueueServer` reports sample rate 0 /
  `AudioOutputUnitStart` error 560227702. Without the swap none of those lines
  appear (the 5H11a routing database accepts the category), yet the tone still
  died before its first `AudioSessionInitialize` attempt.
- **[V]** GDB (`exception_triage` 0xc001b6a0, saved user state from
  `[0xc0208d5c] + 0x1cc`) named the crash: pc `0x2fe1e724`
  (`dyld:_pthread_getspecific`, `ldr r0,[r0,#0x48]`), lr `0x2fe1eddc`
  (`__gthread_getspecific`), **r9 = 0**. On iOS 2, r9 is the thread/TLS base;
  a scratch build with r9 printed at start, after `dlopen` and just before the
  call had `r9=394346d0` each time, so a callee zeroed it inside the call.
- **[V]** Root cause is in the *test client*, not the audio stack: linking the
  2.x `ios3ds-tone` pulled in
  `__divdi3`/`__udivmoddi4`/`__udivsi3` (the `RATE * ms / 1000` expressions),
  and those compiler-rt helpers use r9 as scratch. Their `mov r9,#0` is visible
  in the 2.x Mach-O. With `-ffixed-r9` on our code but not on compiler-rt, the
  first `AudioSessionInitialize`/`AudioSessionGetProperty` faulted in dyld.
  `TONE_OLD` builds now use a double multiply for the frame count, so no
  compiler-rt division helper is linked (`mov r9,#0` is gone from the binary).
  **[V]** The default 4.2.1 `__text` is instruction-identical to
  `origin/main`'s (all changes are behind `#ifdef TONE_OLD`; the full Mach-O
  differs only in the linker's random UUID between two builds of the same
  source), and `make test` passes.

### Result: output on 2.2.1

- **[V]** Fresh 5H11a image, scratch test job `ios3ds-tone 2 5 1500 1000 6000 5`,
  plain TCG + `--icount 2`: `tone: #1 … route "Speaker"`,
  `ios3ds-audio: tx start ch5`, `[audio] iOS output stream #1 started`,
  `[audio] stream stopped: 145380 words, peak 5999, TC IRQs 5 (5 injected),
  bad addr 0`, then stream #2 with 10/10 TC IRQs (cumulative) and peak 5999.
  `IOS3DS_CSND_WAV` recorded a 9.28 s stereo 44,097 Hz WAV, peak **5999**,
  261,824 non-silent frames. No panic, no `AudioQueue error`, no DMA error.
  The shipped job is still not installed; this one is for test images only.

### The mic patch at 0x9a358 is harmful on 5H11a

- **[V]** §130's `N72\0N82\0` -> `X72\0N72\0` swap at VirtualAudio file offset
  0x9a358 **removes** the N72 product prefix. 5H11a's `GetProductID`
  (`sysctlbyname("hw.model")` + `CFStringHasPrefix` at 0x5da4c/0x5da70/
  0x5da9c/0x5dac8) uses those same strings, so `hw.model=N72AP` becomes an
  unknown product: category activation fails as above. The newer 3.1.3 plug-in
  (AudioDriverPlugIns-186.3) has an `UnknownProduct` routing handler; 2.2.1's
  (141.11) does not. Setting the N72 slot to `N82` instead, and patching the
  product-ID literal at 0x5dcf8 (`n72p` -> `n82p`) while keeping the table,
  were both tried: both still fail to set a category.
- **[V]** With the patch removed, `ios3ds-tone rec` reaches
  `input available 0, route "Speaker"`; forcing `qom-set /machine headphones on`
  gives `[audio] iOS route -> headphones` but still `input available 0`
  (5H11a's N72 database wants a wired *headset* contact, which QEMU's jack
  model does not provide). **[G]** The 2.2.1 mic is therefore off in this
  build: no VirtualAudio byte patch is shipped for 5H11a. It needs either a
  routing-database patch that adds a built-in mic port to N72, or an emulated
  3-contact headset jack; both remain open.

### Other versions

- **[V]** **4.1**: `ios3ds-tone rec` with `IOS3DS_MIC=sine:440:1000`:
  `input available 1, route "SpeakerAndMicrophone"`, `ios3ds-audio: rx start`,
  `rec 88200 of 88200 frames`, sine fit 440.03 Hz / amplitude 1002.5,
  `[mic] 1 s: FIFO peak 1000 … iOS peak 66` (the 4.x +23.65 dB chain with
  2151/32768 gain), `rx stop … underruns=0 resyncs=0`, TC IRQs 20/20.
  The 0x1c9d40 VirtualAudio relocation works.
- **[V]** **3.1.3**: same run: `input available 1`, route
  `SpeakerAndMicrophone`, 88,200/88,200 frames, sine fit 440.03 Hz /
  amplitude 995.4, no clipping. The new `mic_gain_q15=32768` (unity) level is
  correct for 7E18; QEMU peak matches the FIFO peak of 1000.

### Hardware checklist (diag=1 log.txt)

1. 2.2.1 boot with a test tone job: `[audio] iOS output stream #N started`
   paired with `stream stopped: … TC IRQs n (n injected), bad addr 0`; the
   unlock/tap clicks and the tone should be audible from the speaker.
2. 2.2.1 `ios3ds-tone rec`: expect `input available 0`, no `[audio] mic on`
   line and no `rx start`. If a real 3DS with a wired headset gives
   `input available 1`, report the route and whether `ios3ds-audio: rx start`
   appears — that would point at a 3-contact jack route instead of N72's
   missing built-in mic.
3. 4.1/3.1.3: `[mic] 1 s: FIFO peak P, iOS peak Q`; on 4.1 Q should be about
   P/15, on 3.1.3 about P. Loudness on a real console still needs a check.

### Verification after rebasing onto origin/main (1add5cf)

- **[V]** `make test` passes. Fresh default build: `kernel 505f451a…`,
  `devtree 25b1bd23…`, `fakeio e5fecd66…`, `shim a35631eb…`,
  `gpu 6bdaf617…`, `boot.cfg 8d4079cf…` — the six values printed by main
  before this branch (re-log 131/132).
- **[V]** Fresh images, `IOS3DS_QEMU=/tmp/opencode/qemu-compat2/b/qemu-system-arm
  python3 emu/soak.py --cycles 5 --icount 2`:
  - 4.2.1 (`rm -f build/sd.img && make sdimg`): **Result: PASS**.
  - 2.2.1 (`make sdimg IOS=2.2.1`, `--require-app --crashlogs --targets
    "275,226;199,226;123,226;47,226;275,50"`): **Result: PASS**, 0
    audio DMA errors, no app taps that opened nothing.
  - 3.1.3 (`make sdimg IOS=3.1.3`, `--require-app --crashlogs --targets
    "47,314;123,226;199,226;275,226;123,50"`): **Result: PASS**.
  - 4.1 (`make sdimg IOS=4.1`, `--require-app --crashlogs`): **Result: PASS**.
- **[V]** Post-rebase 5H11a test image with the tone job: stream #1 and #2
  started (`tx start ch5`), `stream stopped: … TC IRQs 18 (18 injected)` and
  `36 (36 injected)`, peak 5999 on #2; `IOS3DS_CSND_WAV` recorded 716,355
  frames, peak 13273 (a boot UI sound overlaps the first tone), 264,089
  non-silent frames. A final `ios3ds-tone rec` on the same tree still logs
  `input available 0, route "Speaker"` with no `rx start`.
## 135. Sensors, camera, GPS and GPU on 3.1.3, 2.2.1 and 4.1 (2026-11-25)

Firmware staged from compat16 into this worktree's `build/fw-*`. 7E18 code read
from its dyld shared cache (symbols kept in the 3.x cache) with
`llvm-objdump-18`; all VAs below are 7E18 cache addresses.

### GPU: why CoreAnimation got no GL context on 3.1.3

- **[V]** `EAGLServer::renderer()` (0x31bda370) returns its cached renderer,
  else, while `enable_ogl` (0x382e7d85, set from `CA_ENABLE_OGL`, default 1)
  is on, calls `Display::eagl_context(api)` → `CAEAGLContextCreate` →
  `[[EAGLContext alloc] initWithAPI:api properties:]`. `api` is the third
  constructor argument, which `M2Display::new_server` (0x31bde71c) passes as
  the display kind: 2 (re-log 120 forces the kind to 2 to get an EAGLServer).
- **[V]** 7E18 `-[EAGLSharegroup initWithAPI:require_acceleration:]`
  (0x32f80a98) only tries the MBX engine bundle (ours, `-DGL_IOS3`) when the
  API is 1 (`cmp r8,#1` at 0x32f80bd0). So every frame asked for an ES2
  context that no plugin provides, got nil, and fell back to `sw_renderer()`.
  CA's 3.x OGL renderer is `CA::OGL::GLESContext` only (no GLES2 classes).
- **[V]** Fix: cache offset 0x1bde788 `mov r3,r4` → `mov r3,#1`
  (rootfs/overlay-3.1.3.json, original bytes verified). The log now shows
  `[gl] context created (pid 21)`, `first BindCoreSurface`, `first
  SwapNotification`, `[gl] CA scene check ... 6 of 9216 pixels differ` and
  `[gl] CA compositor: OpenGL ES on the PICA200`. `versions.py` drops the
  3.1.3 `gpu_mirror=0` override (2.2.1 keeps it); `[gpu] display mirror
  ACTIVE`, `mirror: 120 frames in 30 s` while animating.
- **[V]** First soak with it: kernel panic `fault_addr=0x83070000` in the
  shim right after `[gl] pica: surface ... not physically contiguous: the GPU
  writes it through an arena copy` (CA's second surface after Home).
  `PICA_OP_SCATTER`'s `p_map_pa` hard-coded 4.x's linear-map offset
  0x60000000; on Darwin 10 (virt base 0xC0000000) it is 0xA0000000. Now
  `IOS3DS_KPI_VIRT_BASE - 0x20000000`, which is the same constant on 4.2.1
  (gpu.bin hash unchanged).
- **[V]** Performance, `emu/perf.py --runs 3 --icount 2 --open 123,226
  --home-max-stddev 100` (new options: tap an app instead of the Utilities
  folder, and accept the black 3.x wallpaper as home), Notes open/close:
  software CA + ARM9 copy (main): open 4.1 / 6.1 / 6.1 s, close 15.0 / 4.0 /
  4.0 s, boot 88 s. PICA200 CA + mirror: open 46.4 / 10.1 / 14.2 s, close
  46.5 / 20.2 / 20.2 s (first run includes the GPU bring-up), CA frame
  histogram 25 frames/s, 40 ms average while animating. **In QEMU the GPU
  path is slower**: the PICA200 model and the mirror's read-out (~10 ms per
  frame) run on the host CPU, while the software compositor is plain TCG
  code. On hardware the 4.2.1 data (re-log 77ff.) says the GPU path is the
  faster one, but 3.1.3 on hardware was not measured. **[G]** checklist:
  boot 3.1.3 with `gpu_diag=1`, open/close Notes 5×, look for `[gl] CA
  compositor: OpenGL ES on the PICA200`, `[perf] CA:` lines and `[gpu]
  mirror: N frames`; if it is slower than before, set `gpu_mirror=0` and
  `CA_ENABLE_OGL=0` (SpringBoard plist) to get the old path back.
- **2.2.1 GL engine:** 5H11a's OpenGLES.framework *is* the IMG MBX driver
  (`IMGegl*`, `mbx_service`, 214 exports, no engine bundle), and QuartzCore
  links it plus MBX2D directly. A PICA200 engine there means replacing the
  framework with our own EAGL classes and GLES1 exports on the ios3ds GL core
  (2.x ABI, r9, classic binding). Possible, not attempted this round; 2.2.1
  stays software CA + ARM9 copy.

### Accelerometer / gyro

- **[V]** 3.1.3 already worked: 7E18's AppleLIS302DL probes the shim's
  emulated LIS302DL (`ios3ds-mcu: accelerometer read reg 0x0f`, `powered on
  by AppleLIS302DL (CTRL_REG1 0x47)`), SpringBoard's orientation goes
  0→1→4 with `mcu-orient`, and Safari's page turns to landscape (QEMU
  screenshot). The driver matrix said "off" only because the 4.x
  client-side route does not exist; the kernel route is the shipping one.
- **[V]** 4.1: same (`read reg 0x0f`, orientation 0→3, Safari bookmarks in
  landscape). Gyro off on 4.1: the `gyro-usage`/`gyro-dispatch` vtable
  patches were never relocated to 8B117 (`[gyrotest] gyroAvailable 0`).
- **[V]** 2.2.1: no transfer to the accelerometer address ever reaches the
  shim (only the audio codecs at 2b80/2c00), although the node and
  AppleLIS302DL exist in 5H11a. Not traced (next: breakpoint the kext's
  start/probe). Gyro: no CoreMotion before 4.0 on any version → n/a.

### Camera

- **[V]** n/a on 3.1.3 and 2.2.1: no Camera.app, no MediaCapture plugin
  (our `vcd` is a 4.x MediaCapture plugin); 7E18 has Camera.framework and
  Celestial capture code, but no driver interface we could bind without
  writing a whole capture stack. UIImagePickerController reports no camera,
  which is honest for an iPod2,1 of that era. 4.1: the ARM9/kernel side comes
  up (`[cam] ... chip id 2280` ×3); Camera.app not exercised this round.

### GPS

- **[V]** applog's CoreLocation hooks run on 3.x/2.x: instance
  `-locationServicesEnabled` is hooked too, and `CLLocation` is built with the
  2.0 initializer when the 4.2 one is missing. `ios3ds-gpstest` (3.1.3, 2.2.1
  with 2.x builds, and 4.1): `services=1 authorization=-1`, a `delegate:` and
  `location:` 52.520000,13.405000 / 50 m every second.
- **[V]** 2.x builds of applog (dylib), diag, gps and gpstest:
  `tools/common/dyld2_glue.S -DDYLD2_DYLIB` reaches the image header through a
  rebased `__DATA` word (a dylib can slide); executables keep the old glue
  (ios3ds-mcud 2.x binary unchanged).
- **[V]** Not done: stock apps on 3.x/2.x do not get applog. 7E18's
  `_addInternalDebugVariablesToEnvironment` (0x19461) reads `SBGuardMalloc`
  (no `SBGuardMallocAlways` before 4.x) and removes it after one use; even with
  that `removeObjectForKey:` nopped, `diag=1` showed no `[applog]` line from
  MobileMaps, so the insertion was dropped again. `ios3ds-gps set` from
  gpstest fails on 3.x/2.x because there is no `/bin/sh` for `system()`.

### Verification

- 4.2.1 default build hashes (identical to before this branch): kernel
  `505f451a…`, devtree `25b1bd23…`, fakeio `e5fecd66…`, shim `a35631eb…`,
  gpu `6bdaf617…`, boot.cfg `8d4079cf…`.
- 3.1.3 soak with the GPU path (`--cycles 5 --require-app --crashlogs`):
  first run FAIL (the panic above), after the shim fix **PASS** (0 panics,
  every tap opened an app, cycles 1 and 3 needed a second Home).
- Final fresh-image soaks (`--cycles 5 --icount 2`; old versions with
  `--require-app --crashlogs`): **2.2.1 PASS** (0 restarts, every tap opened
  an app, home diff 0.0), **3.1.3 PASS** (0 panics, 0 restarts, every tap
  opened an app; a mediaserverd crash report, as with the audio DMA
  failures), **4.2.1 PASS** (0 restarts, home after every Home).
- **Open on 3.1.3 with the GPU path:** after Home from an app the first frame
  that settles is the home screen with most icons missing/dark (the icon
  fly-in's last frame is not what the mirror shows); a second Home (or any
  touch) redraws it. Every 3.1.3 cycle in the final soak needed that second
  Home; with software CA (re-log 128) the first Home was enough. Next: check
  whether CA's final post-animation frame is presented through the engine
  (`SwapNotification` count) and whether the mirror drops the last
  `m_dirty` (it runs every 2nd VBlank). **[G]** on hardware: log `[gpu] mirror`
  counts after Home and a photo of the home screen 2 s after Home.
- After rebasing onto origin/main (audio/Cydia rounds): `make test` passes,
  the six 4.2.1 hashes above are unchanged (built with and without this
  branch), and a fresh 3.1.3 soak (`--cycles 5 --require-app --crashlogs`)
  **PASS**es: 0 panics, 0 restarts, every tap opened an app, only cycle 2
  needed a second Home.

## 136. 3.1.3's last frame after Home, and making the GPU path opt-in (2026-11-25)

Continues the previous NEXT section. The 3.1.3 PICA200 CoreAnimation path from
137e6cc is kept, but it is **opt-in**: the default image is back on software
CA + the ARM9 screen copy. Firmware staged read-only from the oldsensors
worktree into this worktree's `build/fw-3.1.3`; 2.2.1 was staged by copying
`fw-2.2.1/staging` into this worktree's own `build/`.

### Counting frames after Home

- **[V]** `IOS3DS_CA_SWAP_DIAG=1` (new, off by default) prints every CA
  `SwapNotification` and each present / arena copy / CPU read-back behind it
  as `[gl] [swapdiag]`, next to the ARM9's 30 s `[gpu] mirror:` report. On a
  fresh GPU image (`IOS3DS_GPU313=1 IOS3DS_GPU313_DIAG=1 make sdimg IOS=3.1.3`)
  boot produced swaps #1–6, each with exactly one direct `PICA_OP_PRESENT`
  into surface 0x621000; the mirror built 4 frames in the first 30 s and then
  reported 901 idle VBlanks in the next 30 s (0 busy skips, 0 timeouts).
- **[V]** After the first Home key (`[input] key 40 down/up @14806`) CA still
  issued 18 swaps (#26–#43, presents 36→53, all direct), so the final present
  was not missing and the mirror was not stuck. The frame the soak captured
  after Home was still Settings, not a dark-icon home screen: on this loaded
  host the Home press was swallowed or the GPU close animation had not
  finished inside the harness's settle window; the harness then had to try its
  page-swipe / second-Home recovery (as in the previous section). The
  dark-icon symptom could not be isolated to a single dropped mirror frame
  here.
- **[V]** A late `IOMobileFramebufferSwapSignal` can still switch the CLCD to
  another IOSurface *after* the present marked `m_dirty`: the mirror would then
  copy the old surface, clear `m_dirty`, and see no further present on 3.x.
  This is the remaining display-side race that the fix below closes; the
  QEMU runs cannot confirm that it was the only cause of (a).

### Fix / opt-in

- **[V]** `kernel/shim/pica.c` remembers the CLCD source PA of the last mirror
  frame (new `IOS3DS_KPI_CLCD_SWAP_DIRTY`, set only for 3.1.3 in
  `scripts/versions.py` / `scripts/kpi.py`). On a VBlank with `m_dirty == 0`, a
  changed source PA now marks a frame dirty, so a swap that arrives after the
  present is still mirrored. The code is compiled out on 4.1/4.2.1; the six
  4.2.1 hashes are unchanged (below). GPU part on 3.1.3: 26496/32768 bytes.
- **[V]** `scripts/versions.py` restores 3.1.3's `gpu_mirror=0` boot.cfg
  override; `rootfs/overlay-3.1.3.json` sets `CA_ENABLE_OGL=0` and
  `LK_ENABLE_OGL=0`, and the EAGLServer API patch (0x1bde788) is left out of
  the default recipe. `IOS3DS_GPU313=1 make sdimg IOS=3.1.3` keeps
  `gpu_mirror=1`, sets `CA_ENABLE_OGL=1` and includes the patch.
  `IOS3DS_GPU313_DIAG=1` additionally sets `IOS3DS_CA_SWAP_DIAG=1` in
  SpringBoard's environment. Offline tests check both recipes and boot.cfg.
- **[V]** Why default-off: in QEMU the GPU path is slower (previous section:
  Notes open 10–14 s vs 4–6 s, close ~20 s vs 4 s after the first bring-up
  run). GPS hooks, sensor/camera status and `emu/perf.py` options from the
  branch are unchanged.

### Verification

- **[V]** `make test` passes. Fresh default 4.2.1 build (SHA-256): kernel
  `505f451a44d9da6aae7034d427d5524d7392d9fc70640ec997c374b0b5a70e38`,
  devtree `25b1bd233447ddfc60eb69e30ad4cbd085c1963f86e77052fda969469d8327cc`,
  fakeio `e5fecd6645b3bc44d6510d7af7397c667fdc25da4517e2cd9fabc42fb673570d`,
  shim `a35631eb66b3c2855a1a02e656bb88b77b132dd89f7a3c69a429f9a033a4a3a1`,
  gpu `6bdaf6177b66a3d268d3472958a74c8797ce18c64a1a4556f04806292691f479`,
  boot.cfg `8d4079cfe114e5006a82967dee51b28de666376474fa86ebd8c9a5d4969b5e29`
  (unchanged by this branch). Shim main part 24268/24320 bytes; 3.1.3's GPU
  part is 26496/32768 bytes with the kpi flag compiled in.
- **[V]** Every soak below started from a **fresh `build/sd.img`** (`rm -f`
  then `make sdimg [IOS=...]`), with
  `IOS3DS_QEMU=/tmp/opencode/qemu-compat2/b/qemu-system-arm` and `--icount 2`:

  | image | command | boot | cycles / home | result |
  |---|---|---|---|---|
  | 3.1.3 default | `emu/soak.py --cycles 5 --require-app --crashlogs --targets "47,314;123,226;199,226;275,226;123,50"` | 40 s | 5 apps opened, app diffs 84.8–164.2, home diff 0.03–0.04, **no Home recovery**, 0 restarts | **PASS** |
  | 3.1.3 opt-in GPU | same | 50 s | 5 apps opened, app diffs 84.8–164.1, home diff 0.04 except cycles 1 and 3 (60.9 / 60.2, recovered by second Home / page swipe + Home), 0 restarts | **PASS** |
  | 2.2.1 | same with targets `"275,226;199,226;123,226;47,226;275,50"` | 60 s | 5 apps opened, app diffs 84.4–173.6, home diff 0.0–0.04, no recovery, 0 restarts | **PASS** |
  | 4.2.1 | `emu/soak.py --cycles 5` | 70 s | 5 apps opened, home diff 0.0–0.02, no recovery, 0 restarts | **PASS** |

  The GPU image's `boot.cfg` has `gpu_mirror=1` and the generated recipe has
  `CA_ENABLE_OGL=1`; the default 3.1.3 image has `gpu_mirror=0` and
  `CA_ENABLE_OGL=0`. All five taps opened an app in every run. The two GPU
  recoveries show that (a) is **not fully eliminated** by the CLCD-PA fix under
  a loaded host, which is a second reason (besides (b)) to keep that path off
  by default. The diagnostic run used for the swap counts above was stopped
  after its first cycle; its image was discarded and rebuilt fresh for the
  final soaks.
- **[V]** After rebasing onto the final `origin/main` 1e8c2ea (old-version
  power, Wi-Fi and audio work), `make test` passes, the six 4.2.1 hashes above
  are still identical, and all four fresh-image soaks were re-run on that
  exact tree with the numbers in the table above. A `git archive origin/main`
  check was not possible for the host PICA mirror build without a full
  source-tree userland; instead the committed 4.2.1 values are compared byte
  for byte to the hashes recorded by main's own re-log 132.

### Needs hardware

- **[G]** On a console, first boot the **default** 3.1.3 image: home screen
  after a single Home, Notes opening in ~4–6 s, ARM9 copy (`gpu_mirror=0`) in
  `log.txt`.
- **[G]** For the opt-in image, build with
  `IOS3DS_GPU313=1 IOS3DS_GPU313_DIAG=1 make sdimg IOS=3.1.3`, boot with
  `diag=1` and check for `[gl] CA compositor: OpenGL ES on the PICA200`,
  `[gl] [swapdiag] swap #N: … presents …`, `[gpu] display mirror ACTIVE` and
  `[gpu] mirror: N frames` after opening and closing Notes five times. Take a
  photo 2 s after Home: all icons must be fully drawn without a second Home or
  touch. Compare open/close times with the default image; if the GPU path is
  slower or the last frame is still lost, ship the default (software) image.
- **[G]** The CLCD-source change is only exercised when CA swaps IOSurfaces
  after a present; QEMU's model and the loaded host could not isolate that
  race from a swallowed Home / slow close animation, so the hardware log is
  the deciding evidence for the fix.

## 137. 2.2.1 accelerometer: the I2C probe was there all along (2026-11-26)

Re-log 135 said 5H11a's AppleLIS302DL never sent an I2C transfer. That was a
false negative from a short observation, not a device-tree or probe gap in the
current tree (main 40b5704). No code change was needed; this section records
the evidence and corrects the driver matrix.

- **[V]** Fresh `make sdimg IOS=2.2.1`, boot with
  `IOS3DS_QEMU=/tmp/opencode/qemu-compat2/b/qemu-system-arm python3 emu/run.py
  --sd build/sd.img --time 150 --icount 2` (log kept as
  `build/base221a-run.log`, not committed): the shim sees
  `ios3ds-mcu: accelerometer read reg 0x0f len 1` (WHO_AM_I) and `reg 0x22`
  during kernel start, then, once SpringBoard asks for reports,
  `accelerometer read reg 0x20/0x21/0x22`,
  `accelerometer powered on by AppleLIS302DL (CTRL_REG1 0x40)`, `[mcu]
  accelerometer on` and `accelerometer sample #1: 0 -55 0 counts`. The ARM9
  log has `[mcu] accelerometer on` too. The transfers arrive through the
  patched `AppleARMIICDevice::transfer` and are recognized by
  `nub_is_acc()` (the `getProperty("reg")` vtable slot is
  `0x9c + IOS3DS_KPI_VT_BIAS`, bias 8 on Darwin 9).
- **[V]** Static comparison: the 5H11a device tree (staged
  `devicetree.txt`) has `/arm-io/i2c0/accelerometer` with
  `compatible 'accelerometer,lis331dl|accelerometer,lis302dl'`, `reg 0x1d`,
  `interrupts 0x63 flags 3`, `orientation 5`. `tools/ios3ds-mcu/dt_mcu.py`
  narrows `compatible` to `accelerometer,lis302dl` and verifies address
  `0x1d`; no property is deleted for 2.x. The 5H11a kernelcache has exactly
  one `AppleEmbeddedAccelerometer` personality: class/IOClass
  `AppleLIS302DL`, provider `AppleARMIICDevice`, `IONameMatch
  accelerometer,lis302dl` (read from `__PRELINK/__info` with IDREFs expanded).
  7E18/8B117 use the same match/address, so there is no 2.x bus or address
  difference to fix. `orientation 5` versus 4 only changes the axis-sign
  mapping, not probe or power.
- **[V]** Orientation actually reaches SpringBoard: with
  `--hmp "120=qom-set /machine mcu-orient 1;150=qom-set /machine mcu-orient 2"`
  (log `build/base221orient-run.log`), the samples flip from `0 -55 0` to
  `0 55 0` and mcud logs `[mcud] SpringBoard device orientation 0 -> 4`.
  That is the same notification UIKit/Safari uses on 3.1.3/4.2.1
  (`com.apple.springboard.rawOrientation`), so the 2.2.1 rotation path is
  alive end to end. **[G]** The Safari window itself was not screenshot-tested
  in 2.x QEMU: its home page 1 has no Safari icon and `emu/session.py drag`
  does not change pages with the 2.x GSEvent layout. On hardware: open Safari,
  rotate the console 90°, and check that the page turns landscape; `diag=1`
  `log.txt` should contain `accelerometer read reg 0x0f`,
  `powered on by AppleLIS302DL`, `accelerometer sample #N` and mcud's
  `SpringBoard device orientation 0 -> 4` (or 1/2 for the other poses).
- **[V]** No repository file was changed for this item: `scripts/versions.py`,
  `kernel/dtgen.py`, `tools/ios3ds-mcu/{dt_mcu.py,shim/mcu.c}` and the shim
  binary are unchanged. The 4.2.1 build is therefore byte-identical.

## 138. 2.2.1 microphone: why the VirtualAudio product swap cannot work alone (2026-11-26)

The 5H11a mic route is still **not working**; this section replaces the open
"[G]" in re-log 134 with the binary-level reason and three tested patches.
No VirtualAudio patch is shipped for 2.2.1: all tested variants either break
category activation or leave `input available 0`.

- **[V]** 5H11a's `/Library/Audio/Plug-Ins/HAL/VirtualAudio.plugin/VirtualAudio`
  (AudioDriverPlugIns-141.11, 706,336 bytes) has a four-entry product table at
  file offset `0x9a350`: `M68 N45 N72 N82` (12 CFStrings at `0xa4dbc`…
  `0xa4dec`). `GetProductID` at `0x5da4c` calls `CFStringHasPrefix` for each
  entry and writes the product-ID words `m68p`, `n54p`, `n72p`, `n82p` from
  the literals at `0x5dce0`, `0x5dcec`, `0x5dcf8`, `0x5dd04` into a writable
  buffer; the `RoutingSettingsFactory_Aspen` code then selects a per-model
  routing handler from that ID. There is no N81 slot (the 4.2.1 mic donor) and
  no `UnknownProduct` fallback: renaming the N72 entry leaves
  `hw.model=N72AP` unrecognised, which is the category failure of re-log 134.
- **[V]** Patch A (built and booted): re-log 130's swap at `0x9a358`
  (`N72 N82` -> `X72 N72`) plus an `n82p` -> `n72p` result at `0x5dd04`, so
  `hw.model=N72AP` still yields product ID `n72p`. Result: no
  `Unable to set category` / `Routing Not Supported` in mediaserverd's console
  (stdout was enabled in a scratch image), but `ios3ds-tone rec` still logs
  `input available 0, route "Speaker"` and no `rx start` (logs
  `build/mic221a-run.log`, `build/mic221b-run.log`). The plug-in follows the
  product result back to the **N72** database, which has no built-in-mic port.
- **[V]** Patch B (built and booted): point the N45 slot at N72
  (`0x9a354: N45 -> N72`) and change its ID word at `0x5dcec`
  (`n54p -> n72p`). N45 has a `PlaybackAndRecord_N45` handler and a built-in
  mic; category still activates cleanly, but `input available 0` again
  (`build/mic221c-run.log`). N82 (re-log 134's donor) has no
  `PlaybackAndRecord` handler at all in 141.11 (only Playback/Record/Call/
  Broadcast), so its category is refused.
- **[G]** A real fix needs either (a) a patch to the 5H11a N72 routing
  database that *adds* `kVirtualAudioPortTypeMicrophoneBuiltIn` to the ports
  of `RoutingHandler_PlaybackAndRecord_N72` (the handler at `0x88dbc` looks
  ports up by type at `+0xc`, and the N72 handler's exception strings are at
  `0x9e844`…`0x9e8a0`; the port-construction code still needs to be mapped),
  or (b) headset emulation including the 3-contact microphone / `mikey`
  contact, not just the headphone GPIO bit. Both are beyond a byte swap; no
  such patch is committed. The stock 5H11a N72 database is left in place, as
  re-log 134 concluded.
- **[G]** Hardware checklist (a console with a wired 3-contact headset):
  `diag=1` `log.txt` should show `input available 1` and a route containing
  `MicrophoneWired` from `ios3ds-tone rec`, followed by `[audio] mic on` and
  `ios3ds-audio: rx start`. If it does, a 3-contact jack route (option b) is
  worth implementing; if not, the N72 database needs option (a).
- **[V]** Reverting the experiments leaves the 2.2.1 overlay exactly as on
  main: no `VirtualAudio` entry in `rootfs/overlay-2.2.1.json`. All four
  version soaks and the six 4.2.1 hashes were re-checked afterwards.

### Verification of this round's tree (2026-11-26)

- **[V]** `make test` passes (97 offline tests, GL/Wi-Fi/GPS/input/ALS/
  blkcache harnesses, stub-symbol check). The repository has **no code change**
  in this round: only `docs/re-log.md` and `docs/multi-version.md` are modified.
- **[V]** Fresh default build (`python3 scripts/mkboot.py --out build/verify421`)
  gives `kernel 505f451a44d9da6aae7034d427d5524d7392d9fc70640ec997c374b0b5a70e38`,
  `devtree 25b1bd233447ddfc60eb69e30ad4cbd085c1963f86e77052fda969469d8327cc`,
  `fakeio e5fecd6645b3bc44d6510d7af7397c667fdc25da4517e2cd9fabc42fb673570d`,
  `shim a35631eb66b3c2855a1a02e656bb88b77b132dd89f7a3c69a429f9a033a4a3a1`,
  `gpu 6bdaf6177b66a3d268d3472958a74c8797ce18c64a1a4556f04806292691f479`,
  `boot.cfg 8d4079cfe114e5006a82967dee51b28de666376474fa86ebd8c9a5d4969b5e29`
  — the six values recorded on main (re-log 132/134/135).
- **[V]** Fresh images, `IOS3DS_QEMU=/tmp/opencode/qemu-compat2/b/qemu-system-arm`,
  `--icount 2`, five cycles each, **Result: PASS** in all four soaks:
  - 2.2.1: `--require-app --crashlogs --targets
    "275,226;199,226;123,226;47,226;275,50"`; input after 65 s, five apps
    opened (diffs 52.8–109.0), home diff 0.0 after every Home, 0 panics,
    0 crashes, no audio DMA errors.
  - 3.1.3: `--require-app --crashlogs --targets
    "47,314;123,226;199,226;275,226;123,50"`; 0 panics, 0 crashes, every tap
    opened an app.
  - 4.1: `--require-app --crashlogs`; 0 panics, 0 crashes, every tap opened
    an app.
  - 4.2.1: default image, no extra flags; 0 restarts, home after every Home.
- **[G]** 2.2.1 Safari UI rotation still needs a console test: a scratch
  `ios3ds-diag -urlpoll` job did not launch MobileSafari on 5H11a, and page
  swipes in the 2.x QEMU input build do not reach the second home-screen page,
  so the Safari window could not be captured in landscape here. The kernel
  probe, HID samples and SpringBoard orientation are verified above.
## 139. iOS 4.1: the gyro vtable and CoreMotion capability mask (2026-11-26)

Ports the two kernel and one dyld-cache change that re-log 131 listed as still
missing on 8B117. Everything below was read from this worktree's staged
`build/fw-4.1/staging/` firmware; no Apple code is stored in the repo.

- **[V]** `gyro-usage` / `gyro-dispatch`: the 8C148 `AppleLIS331DLH` vtable at
  0x806200d0 is at 0x805d50dc in 8B117. It has no symbol, so it was relocated
  by its pointer layout (+0x3b0 = 0x805d0970 where 8C148 has 0x8061b970,
  followed by the same 3x3 calibration/getter sequence). The usage slot +0x3b4
  held `mov r0,#3` (0x805d0978) and now points to the existing `mov r0,#9; bx
  lr` at 0x80337eb8 (the 8C148 0x8034433c equivalent, found by its neighbouring
  getters `mov r0,#10..#12`). The dispatch slot +0x3c0 held
  `AppleEmbeddedHIDEventService::dispatchAccelerometerEvent` 0x804fb771 and
  now uses the unchanged shim jump-table slot +0x2c, i.e. `@shim+0x2c@` expands
  to the same 0x8084002c as on 8C148. Both new kernel patches are in
  `kernel/patchsets/4.1.json`; the original words were verified in the cache.
- **[V]** CoreMotion: 8C148's one `0x2012 -> 0x2052` literal (recipe offset
  0x3224dd4) became *two immediate instructions* in 8B117's
  `CLPlatformInfo::getProviders` (symbol VA 0x3053bbcc, cache offsets
  0x53bc3c and 0x53bc4c). The 8B117 function is a 12-case jump table instead
  of 8C148's 16-case one; `getSystemHardware`'s model strings show cases 3/5/7
  are N45/N72/N18, and case 11 (N81) already stores 0x52 — proving 0x40 is the
  gyro bit. Both `mov r2,#0x12` sites are now `mov r2,#0x52` via two
  `extra_file_patches` in `rootfs/overlay-4.1.json`.
- **[V]** Fresh 4.1 image, `diag=1`, `--icount 2`: `[gyrotest] gyroAvailable 64
  deviceMotionAvailable 64 accelerometerAvailable 1`, `gyroActive 1`, and
  after `qom-set gyro-y 30000` the pull rate became `-0.523 0 0 rad/s` (−30 °/s
  in iOS x because boot.cfg `gyro_axes=-y,x,z`), changing at the next 1 s tick
  and holding steady. 64 is 8B117's raw provider bit from
  `-[CMMotionManager isGyroAvailable]` (`and r0,r0,#64` at 0x30554348): truthy,
  where 4.2.1 normalises to 1. `deviceMotion` rate/attitude stayed 0 on 4.1
  with the same setup (4.2.1 follows the gyro); not chased in this round.
- **[G]** QEMU models the ITG-3200 register file, not a soldered chip. On a
  console, boot 4.1 with `diag=1` and confirm `[gyro] ITG-3200 family at I2C
  3:d0`, `[gyrotest] gyroAvailable ...` nonzero and changing `t=N gyro` lines
  after turning the 3DS about its y axis; `gyro=0` must keep the rates at 0.

## 140. iOS 4.1: CLCD backlight log-enable, retry-off not needed (2026-11-26)

- **[V]** 8B117 has the same three `Where is my backlight?` IOLog call sites as
  8C148 (string literal 0x8075f940, referenced three times). Two were already
  nopped (`clcd-backlight-log-set` 0x80758408, `clcd-backlight-log-disable`
  0x80758460); the third, in `AppleM2CLCD::enable_backlight` (0x80758478), was
  not. A diag=1 QEMU boot before the patch printed
  `void AppleM2CLCD::enable_backlight(), Where is my backlight?` on every wake
  from Lock. Added `clcd-backlight-log-enable` at 0x807584b8 (verified `blx r3`
  bytes) to `kernel/patchsets/4.1.json`; after a fresh build, a POWER-then-Home
  lock/wake round printed 0 such lines while `[mcud]` still logged the
  backlight going 500 → 0 → 500.
- **[V]** `clcd-backlight-retry-off` is unnecessary on 8B117: 8C148's
  `safe_enable_backlight` (0x807a95d0) has a `+0xea0` pending flag, a service
  getter and a retry path that the patch short-circuits. The whole 8B117 CLCD
  cluster is simpler: the enable function has no `strb ...,[r4,#0xea0]`
  instruction anywhere in the kernelcache, and each wake produced exactly one
  log line, not a ~3/s retry storm. Only the IOLog was patched.
- **[G]** Hardware: boot 4.1 with `diag=1`, lock/wake five times and check
  `log.txt` contains no `Where is my backlight?` line and that brightness
  returns after Home.

## 141. Fake GPS in stock apps on 3.1.3 and 2.2.1 (2026-11-26)

- **[V]** `LSEnvironment` does not exist in 7E18's SpringBoard or dyld cache,
  and 3.x/2.x have no `SBGuardMallocAlways`. A launchd-wide
  `setenv DYLD_INSERT_LIBRARIES ...` in `/etc/launchd.conf` was tried first:
  launchctl read the file, then no process reached SpringBoard for 200 s
  (kernel idle at 0xc006991c, no crash) — the same family of hang as 4.2.1's
  global Substrate injection (re-log 71). It is not used.
- **[V]** Shipped instead: a real `LC_LOAD_DYLIB` for
  `/usr/lib/ios3ds-applog.dylib` appended into the slack between the end of the
  load commands and the first section of `/Applications/Maps.app/Maps` and
  `/Applications/MobileSafari.app/MobileSafari` (600 / 1176 bytes of slack on
  7E18, enough for the 56-byte command). `scripts/inject_dylib.py` only changes
  `ncmds`/`sizeofcmds` and writes in that slack, so all file offsets stay put;
  unit tests in `tests/test_inject_dylib.py`. `mkrootfs` applies the new
  `inject_dylibs` recipe key from the 3.1.3/2.2.1 overlays only; the 4.2.1 and
  4.1 recipes never enter the loop. AMFI logs `Invalid signature but permitting
  execution` for the changed apps and they launch normally.
- **[V]** 3.1.3 QEMU, `gps=52.52,13.405`, `--icount 2`, a temporary marker
  build of applog (reverted before commit): tapping the Maps icon gave
  `[applog] Maps pid 98 constructor`, `gps_install_hooks: entry`, `GPS hooks
  installed` in `/private/var/tmp/ios3ds-applog-hooks-98.txt`; the console also
  showed `Maps 98 FS_WRITE_DATA SBF /dev/console 13 (seatbelt)`, i.e. the
  sandboxed app's applog constructor really ran. Maps opened to its map view
  and the locate button became active. No map tiles/fix circle can be drawn
  offline in QEMU, so the visible grey map is expected.
- **[V]** 2.2.1 QEMU with the 2.x applog build: `[applog] Maps pid 32` on the
  console after the same tap; Maps opened normally. CoreLocation hooks in a
  process that loads applog were already verified with gpstest (re-log 135).
- **[G]** Hardware checklist: boot 3.1.3/2.2.1 with `gps=52.5200,13.4050` and
  `diag=1`, open Maps and Safari, confirm `[applog] Maps`/`MobileSafari` on
  the console, then check Maps shows the set location when online and Safari's
  `navigator.geolocation` gets the same coordinates. No locationd or `/bin/sh`
  is involved; `ios3ds-gps set` still needs a real shell on these versions.

## 142. Verification of this round (2026-11-26)

- **[V]** `make test`: 101 offline tests including the new
  `tests/test_inject_dylib.py` (append, idempotence, slack limit, bad magic),
  the GL host tests, Wi-Fi/GPS harnesses and the stub-symbol check all pass.
- **[V]** 4.2.1 default build hashes (sha256, unchanged from origin/main and
  re-log 135): kernel `505f451a…`, devtree `25b1bd23…`, fakeio `e5fecd66…`,
  shim `a35631eb…`, gpu `6bdaf617…`, boot.cfg `8d4079cf…`.
- **[V]** Fresh-image soaks with `IOS3DS_QEMU=/tmp/opencode/qemu-compat2/b/qemu-system-arm`,
  `--cycles 5 --icount 2`: **4.2.1 PASS** (default targets), **4.1 PASS**
  (`--require-app --crashlogs`, targets `47,314;123,226;199,226;275,226;123,50`),
  **3.1.3 PASS** (same targets, Maps/Safari injected), **2.2.1 PASS**
  (`--require-app --crashlogs`, targets `275,226;199,226;123,226;47,226;275,50`).
  Each: 0 restarts after boot, 0 panics, 0 stalls, every tap opened an app,
  Home always returned, lock rounds ended on the home screen.
- **[V]** 4.1 CLCD: after the patch, a POWER/Home lock-wake round on the
  `diag=1` image produced 0 `Where is my backlight?` lines (1 before the
  patch) while the MCU still logged backlight 500 → 0 → 500.
- **[V]** 4.1 gyro: `[gyrotest] gyroAvailable 64 deviceMotionAvailable 64
  accelerometerAvailable 1`, gyroActive 1, rate −0.523 rad/s after
  `qom-set gyro-y 30000` (final kernel with the CLCD patch too).
- **[V]** 3.1.3/2.2.1 Maps: applog constructor and GPS hooks verified in Maps
  (3.1.3 via a per-pid marker build that was reverted before the commit;
  2.2.1 via `[applog] Maps pid 32` on the console). The marker change never
  entered any commit; `git diff tools/ios3ds-diag/applog.c` is empty.
- **[V]** After rebasing onto origin/main (c125664, sens221 docs only),
  `make test` exposed a release-packaging check: the new `mkrootfs` import had
  to be added to `scripts/mkrelease.py:SOURCES` so the installer ships
  `scripts/inject_dylib.py`. With that, `make test` passes again and the six
  4.2.1 hashes above are unchanged.

## 143. Safari's first cold page on 4.1 and 3.1.3: -urlpoll opened p1 before Wi-Fi (2026-11-25)

Continues §104/§112 and §133. Task: on 4.1 the first Safari page after a cold
start showed "Cannot allocate memory", on 3.1.3 "Cannot Open Page … Invalid
argument"; every later page loaded. §133 guessed the first-big-page memory
wall of §104/§112. That is not what these runs show.

### Re-log of §104/§112 on the old versions (before the fix)

- **[V]** 4.1 (8B117), fresh `make sdimg IOS=4.1` plus
  `emu/safari_test.py --pages 3 --settle 240 --icount 2 --port 19423`: the SD
  log has `[urltest] no answer from 10.0.2.2:19423 for 40 s: opening its first
  page anyway` and `[urltest] open http://10.0.2.2:19423/p1.html -> 1` at log
  lines 450/455, while `[wifid] en0 configured by configd` only appears at
  line 501. Safari was therefore launched and given its first URL **before
  DHCP**: the failure is a link-not-up error, not a page-size allocation.
- **[V]** The `[mem]` lines with that early Safari (KiB) show the §112 shape
  but no pressure that would explain a first-page-only failure: free
  37,696-38,724, speculative 35,300-35,556, `free_nospec` 2,140-3,424, wired
  30,828-30,980, `memorystatus_level` 61; MobileSafari resident 9,096-9,284
  KiB. No `[applog] … ENOMEM` line was logged: none of the three allocation
  entry points the re-log 112 interposer watches failed.
- **[V]** 3.1.3 (7E18), fresh `make sdimg IOS=3.1.3` plus
  `emu/safari_test.py --pages 1 --settle 0 --icount 2 --port 27113`: the same
  `[urltest] no answer … for 40 s` / `open …/p1.html` pair precedes
  `en0 configured`, and the cold page shows the alert
  "Cannot Open Page — Safari cannot open the page. The error was: 'Operation
  could not be completed. Invalid argument.'" (screenshot
  `build/safari-313-base-invalid-argument.png`, not committed). At that
  baseline 3.1.3's Safari did not load `ios3ds-applog.dylib`, so there was no
  `[applog]` line; the screenshot is the evidence for the error text. Main's
  re-log 139 later injects applog into MobileSafari with `LC_LOAD_DYLIB`, and
  that is part of the merged tree re-verified below. §133's pages 2/3 loaded
  in 10 s each on the same image, after the link was up.
- **[G]** The two error strings are the CFNetwork/OS reported result of that
  early request; they are not two driver bugs. 4.1's TLSFix copy (§104) is
  loaded into Safari, 3.1.3 has no TLSFix at all, yet both fail only when the
  URL is opened before `en0 configured`.

### Root cause and fix

- **[V]** `-urlpoll` in `tools/ios3ds-diag/diag.c` has a fallback for the
  `--nolink` test: 40 s without an answer from the harness's `/next` endpoint
  makes it synthesize `http://host:port/p1.html` and send it through
  `SBSOpenSensitiveURLAndUnlock`. A connect with no link can itself take
  75 s, and Wi-Fi/DHCP on these versions takes longer than 40 s under
  `--icount 2`, so this fired on every cold boot. `urltest` then never opened
  the harness's later p1 again (`last` already held that URL), so the first
  page of every safari_test run was the early, link-less one.
- **[V]** Fix: `ios3ds_urlwait=1` (added to the boot cmdline by
  `emu/safari_test.py` for every linked run) keeps the poll waiting for a
  real URL. The 40 s fallback stays for `--nolink`, which does not set the
  flag. No kernel, shim, rootfs or Wi-Fi driver source changes; `diag`'s
  default behaviour without the new boot-arg is unchanged.
- **[V]** Second harness bug found while re-logging: with `--sd <image>`
  `safari_test.py`'s `wait_log()` read `session.py log`'s default
  `build/sd.img`, not the selected image, so boots on a version-specific
  image never passed its own boot wait. `log_text()` now passes `--sd` too.
- **[V]** `/next` now answers `pending\n` (not a URL) until the harness has a
  URL, so `-urlpoll` keeps polling instead of treating an empty HTTP body as
  an answer. `tests/test_safari_harness.py` covers all three cases offline.

### Verification

- **[V]** 4.1, fresh `make sdimg IOS=4.1`, `emu/safari_test.py --sd
  build/sd41-final.img --pages 3 --settle 240 --icount 2 --port 21307`
  (all fresh-image runs, one QEMU at a time): **3/3 pages loaded**, page 1 in
  46 s, pages 2/3 in 10 s each, no alert; screenshots in
  `build/emu-safari-41-fixed2/` (p1 shows "Page 1" with its image, not an
  alert). `[urltest] open …/p1.html` now appears after `en0 configured`.
  `[mem]` at the first page: free 40,016 KiB / spec 36,576 / `free_nospec`
  3,440 / wired 30,000 / level 61, MobileSafari 15,244 KiB — the same
  low-usable-free shape as §112, but the page loads; memory was not the cause.
- **[V]** 3.1.3, fresh `make sdimg IOS=3.1.3`, `emu/safari_test.py --sd
  build/sd313-final.img --pages 3 --settle 240 --icount 2 --port 29831`:
  **3/3 pages loaded**, page 1 in 15 s, pages 2/3 in 5 s each, no alert;
  `build/emu-safari-313-fixed/p1.png` shows the rendered "Page 1" with its
  images, not the §before "Invalid argument" alert. There are no `[mem]`
  lines on 3.1.3: its overlay does not install `ios3ds-sample`, so the memory
  re-log for that version is the §112-shaped 4.1 figure above plus the
  screenshot; the merged tree's applog injection (re-log 139) gives an
  `[applog]` load-error line if a cold page ever fails again.
- **[V]** 4.2.1, fresh `make sdimg`, `emu/safari_test.py --sd
  build/sd421-final.img --pages 3 --settle 240 --icount 2 --port 31459`:
  **3/3 pages loaded**, first page 66 s, pages 2/3 10/15 s, no alert. Its
  first page is still fine with the new boot-arg, and the six default boot
  file hashes are unchanged (`505f451a…`, `25b1bd23…`, `e5fecd66…`,
  `a35631eb…`, `6bdaf617…`, `8d4079cf…`). `[mem]` at the first page: free
  34,612 / `free_nospec` 1,232 / level 61, MobileSafari 18,340 KiB — even
  lower usable free than 4.1's first page, which rules out a simple free-page
  threshold as the cause of the old versions' alerts.
- **[V]** Wi-Fi still works with the changed `ios3ds-diag`: 4.1 fresh image,
  `emu/wifi_test.py --case ok --icount 2 --time 500 --port 32711` → **PASS,
  20/20** (SDIO/BMI/WMI/scan/4-way/keys/en0/DHCP/route/DNS/HTTP, no panic or
  ARM9 crash). 3.1.3 fresh image, same test with `--port 34177` → **PASS,
  20/20**. Fresh-image soaks: 4.2.1 `rm -f build/sd.img && make sdimg &&
  emu/soak.py --cycles 5 --icount 2` → **Result: PASS** (boot 110 s, five
  cycles, app diffs 50.8-110.1, home diff ≤ 4.6, 0 panics/stalls/md0 errors/
  crashes/jetsam/GPU fallbacks; the one SpringBoard restart is boot-time HID).
  4.1 fresh image, `--cycles 5 --icount 2 --require-app --crashlogs --targets
  "47,314;123,226;199,226;275,226;123,50"` → **Result: PASS** (boot 90 s,
  five cycles, app diffs 41.0-108.7, home diff ≤ 0.6, no crash reports, 0
  panics/stalls/errors/jetsam). 3.1.3 fresh image, same flags/targets →
  **Result: PASS** (input 65 s, five cycles, app diffs 53.1-102.9, home diff
  0.0, no crash reports, 0 panics/stalls/errors/jetsam).

### After rebasing onto origin/main (40b5704)

- **[V]** The six fresh default-build hashes are unchanged
  (`505f451a…`, `25b1bd23…`, `e5fecd66…`, `a35631eb…`, `6bdaf617…`,
  `8d4079cf…`), and `make test` passes (including the five new harness tests).
- **[V]** Fresh images, one QEMU at a time: 4.1 safari_test (`--pages 3
  --settle 60 --port 35731`) **3/3**, page 1 in 46 s; 3.1.3
  (`--port 36217`) **3/3**, page 1 in 15 s. The older-version GPU opt-in on
  main does not affect the default 3.1.3 image used here.
- **[V]** Fresh-image five-cycle soaks, `--icount 2`: **4.2.1 PASS** (boot 80 s),
  **4.1 PASS** (boot 80 s, `--require-app`, §128 targets, no crash reports),
  **3.1.3 PASS** (boot 50 s, same flags); 0 restarts/panics/crashes/jetsam in
  all three, no tap that opened no app.

### Hardware checklist (diag=1 log.txt)

- **[G]** No hardware path changed: `ios3ds_urlwait` is only set by the QEMU
  harness, and a console/user tap does not go through `-urlpoll`. Confirm on a
  console anyway: with `diag=1`, join Wi-Fi, wait for `[wifid] en0 configured
  by configd`, then cold-launch Safari and load a small page. The log must not
  contain `[applog] MobileSafari load error` with POSIX 12/EINVAL for that
  first page. If it does, attach its domain/code plus the `[mem] free_nospec`
  line and the screenshot; that would be a real userland/network bug rather
  than the harness race fixed here.
- **[G]** Repeat the §112 big-page checklist on the same run (the alert there
  is a separate, hardware-only memory report): `[pica] arena 10240 KiB`,
  camera preview/still, `[mem] free_nospec` and any
  `[applog] MobileSafari ENOMEM` line.



## 144. Wi-Fi on 2.2.1: a shim ifnet, Aeropuerto and in-process Apple80211 (2026-11-26)

Before: 2.2.1's driver-matrix row said Wi-Fi "off" (re-log 133): 5H11a has no
IOUserEthernet, and 2.x's Wi-Fi manager is Aeropuerto on a dynamic
Apple80211.framework. Details in docs/wifi.md section 11.

### Kernel interface

- **[V]** No reusable facility: no IOUserEthernet, tun or utun; PPP/L2TP
  would give a point-to-point IP link without DHCP. The 5H11a LC_SYMTAB has
  the full Darwin 9 network KPI (`ifnet_allocate/attach/input/event`,
  `bpfattach`, `bpf_tap_in/out`, `ether_demux/add_proto/del_proto/
  check_multi/frameout`, `mbuf_*`).
- **[V]** `struct ifnet_init_params` checked against 5H11a's
  `ifnet_allocate`: output +0x18, demux +0x1C, add_proto +0x20, del_proto
  +0x24, check_multi +0x28, framer +0x2C, softc +0x30, ioctl +0x34,
  set_bpf_tap +0x38, detach +0x3C, event +0x40, broadcast +0x44/+0x48.
  The first try (ioctl/bpf/detach after output) made `ifnet_attach` return
  ENODEV (it requires add_proto and del_proto); a second allocate with the
  same unique id is EBUSY, so the ifnet is now kept across retries.
- **[V]** `tools/ios3ds-wifi/shim/kif_eth.c` (GPU part via `link.ld`
  `*kif*.o`, built only with versions.py `kernel_ifnet`, KPI addresses from
  `kpi.py KIF_SYMBOLS` with the Thumb bit from `n_desc`) and the sysctl
  `kern.ios3ds_eth`. 2.2.1 shim: main part 24276 of 24320 bytes (+8 for the
  registration call), GPU part 29136 of 32768. The 4.2.1 shim is unchanged.
- **[V]** `ios3ds-wifid -DWIFID_KIF` (2.x flags, `dyld2_glue.S`) uses it.
  configd only configures services from preferences, and en0 has no IOKit
  object, so the overlay writes a `preferences.plist` with a Wi-Fi service on
  en0 (DHCP); the first try lacked `mkdirs` for its directory and xpwn's
  `add` dropped it silently.
- **[V]** `emu/wifi_test.py --case ok` on 2.2.1: **PASS** (SDIO … 4-way, `[kif]
  en0 attached`, DHCP lease in 7 s, default route, DNS, HTTP fetch).

### Settings > Wi-Fi

- **[V]** Aeropuerto matches `IO80211Controller` with
  `IOServiceAddMatchingNotification`, reads `BSD Name` with
  `IORegistryEntrySearchCFProperty`, checks `IOObjectConformsTo(…,
  "IO80211Controller")`. The 2.x a80211 (`-DA80211_IOS2`) hands it
  `IOResources` and answers `en0`.
- **[V]** Settings still said "No Wi-Fi": `AirPortSettings.bundle` (in the
  Preferences process) and SpringBoard call Apple80211 themselves. The dylib
  is now also in SpringBoard's environment (apps inherit it; only configd
  unsets it), and each process that issues an Apple80211 ioctl starts its
  own poller. Settings then showed "Not Connected" and the status bar the
  Wi-Fi icon.
- **[V]** Scan requests from Preferences failed with EPERM: xnu-1228
  `__sysctl` requires root for any write under CTL_KERN. 2.x only:
  `kif_eth.c` registers `debug.ios3ds_wlan` (same handler/buffer,
  `CTLFLAG_ANYBODY`).
- **[V]** Apple80211 2.x keeps a count of outstanding async scans and calls
  back only when it reaches 0 (Settings re-scans before the previous scan
  completes): one SCAN_DONE per request, and the list is re-readable.
- **[V]** The callback then got an empty list: `__addScanResultToList`
  drops results with RSSI below `__asyncScanResultsRSSIThresh`, which
  Settings leaves at 0 (read at runtime with a temporary probe). Scan
  results now carry dBm + 100 on 2.x.
- **[V]** `emu/wifi_ui_test.py --psk <letters> --settings-icon 275,226
  --wifi-row 160,96 --key-pause 2 --keep-ca` → **8/8 PASS**: three
  networks listed with lock icons, WPA2 join with the keyboard, 4-way
  handshake, DHCP; screenshot shows the check mark and the status-bar icon.
  New test options: `--settings-icon`; the interface check accepts the
  2.x/3.x log line.
- **[V]** Safari: with wifi.cfg, a typed `http://10.0.2.2:<port>` (URL
  keyboard driven by taps through `emu/session.py`) loaded the host's page
  (host log shows the request, screenshot shows it rendered). No
  `ios3ds-diag` 2.x build exists on main, so `emu/safari_test.py` is not
  usable on 2.2.1 yet.

### Hardware checklist (diag=1 log.txt)

- `[kif] en0 attached (Darwin 9 ifnet, kern.ios3ds_eth)`, `[wifid] interface
  en0 up (shim ifnet)`, `[wifid] en0 configured by configd`.
- `[a80211] loaded into configd`, `[a80211] Apple80211 user: SpringBoard`,
  `… user: Preferences` after opening Settings > Wi-Fi, `[a80211] scan
  done: N networks`, `[a80211] association done`.
- Signal bars on 2.x use the dBm + 100 values; their scale on hardware is
  unknown [G].

### Verification

- **[V]** 4.2.1 hashes of a fresh default build equal the origin/main values
  (re-log 133): kernel `505f451a…`, devtree `25b1bd23…`, fakeio
  `e5fecd66…`, shim `a35631eb…`, gpu `6bdaf617…`, boot.cfg `8d4079cf…`.
- **[V]** Fresh 2.2.1 soak `--cycles 5 --icount 2 --require-app --targets
  "275,226;199,226;123,226;47,226;275,50"` → **PASS** (with the dylib in
  SpringBoard and every app).
- **[V]** Fresh 4.2.1 soak `--cycles 5 --icount 2` → **PASS**.
- **[V]** After rebasing onto origin/main (origin/main with the gpu313/sens221/gyro41
  rounds; the 2.2.1 overlay Wi-Fi entries were re-applied on main's
  overlay): `make test` passes (test_versions knows the Wi-Fi files,
  `IORegistryEntrySearchCFProperty` added to the stub symbol list),
  2.2.1 `wifi_test --case ok` **PASS**, `wifi_ui_test` **8/8 PASS**, fresh
  2.2.1 soak (`--require-app`, targets above) **PASS**, 4.2.1 hashes
  unchanged (same six values), fresh 4.2.1 soak **PASS**.

## 145. Issue 1 follow-up: home Wi-Fi, cam_test, log archive, Substrate safe mode (2026-11-26)

Tester logs of 2026-11-25 (New 2DS XL, build m1-0.4.0-244).

**Home Wi-Fi.** The home AP (2.4 GHz, channel 13, RSN, 407-byte probe response)
is in the scan, but the join logged `[a80211] associate: key type 2 / 13 bytes
not supported (WEP?)`: WiFiManager took it for WEP and sent the password as a
WEP-104 key. The ARM9 copies a beacon's IEs, in order, into the 76-byte
`wlan_bss_t.ie` and skips what does not fit; a long country IE (one triplet per
channel) and HT caps ahead of RSN pushed the RSN IE out, so iOS saw the privacy
bit without RSN/WPA. Tethering (109 IE bytes) fit. Fix: `scan_record` copies
SSID, rates, DS, RSN, WPA and extended rates first, then the rest
(`[wifi-diag] scan: N needed IE(s) did not fit` if even that overflows).
[V] host test `test_scan_ie_priority` (fails on the old code). [G] hardware join.

**cam_test=1.** The attached logs are cam_test=0 boots, so the cause is not in
them. After the self-test the ARM9 now soft-resets every sensor, power-cycles
the CAM blocks and probes again (`[cam-test] sensors reset ...`), so iOS starts
as after a cam_test=0 boot. [G] unverified.

**Logs.** `log.txt`/`log_prev.txt` keep their names (tools read `log.txt`). At
mount the old `log_prev.txt` is renamed (no copy) to
`logs/YYYY-MM-DD_HH-MM-SS.log`, named from that boot's own `[mcu] ... RTC`
line (`undated` without one); newest 32 kept. [V] QEMU: `[log] older boot's
log kept as sd:/ios3ds/logs/2026-11-25_23-20-05.log`.

**SBSettings / Substrate.** In the tweak session Substrate worked (SBSettings,
Activator, toggles ran in SpringBoard); the "crash" was SELECT+START (requested
panic) while SpringBoard was slow. The next boot found rootfs.hfs dirty;
launchd printed `launch_msg(): Socket is not connected` (absent in good boots)
and never executed anything (no AMFI lines, no daemons); from t=120 s the CPU
spun in launchd's text with no md0 I/O. The tweak debs' maintainer scripts
(sbsettings 6.0.5, libactivator 1.9.13, flipswitch, rocketbootstrap, libhide,
preferenceloader, toggles) do not touch launchd.conf or launchd; they add the
jobs `sbsettingsd`/`rocketd` and use `launchctl load -w` (launchd overrides
db). Suspects: a file damaged by the panic on the unjournaled dirty volume, or
one of those jobs; not proven [G], no QEMU repro yet. Recovery: boot-arg
`ios3ds_safe=1` (boot.cfg `safe_mode=1` or **L** held while the ARM9 loads)
stops ios3ds-applog loading Substrate anywhere; automatic SpringBoard safe start
after two starts in a row under 90 s (docs/cydia.md). A launchd-level hang is
not covered. [V] QEMU: `safe mode (boot.cfg safe_mode=1)` and `[safe] boot-arg
ios3ds_safe=1 ...` from SpringBoard.

### Hardware checklist
- Home network join: no `key type 2`; `[wifi] connect ... (WPA2-PSK/CCMP)`.
- cam_test=1: `[cam-test] sensors reset`, then a picture in Camera.app.
- One dated file per boot in `sd:/ios3ds/logs/`.
- Hold L: `SAFE MODE` on the top screen, `[safe]` lines in log.txt.

### Verification
- [V] No kernel/devtree/fakeio/shim/gpu/mkboot source changed; fresh build:
  kernel 692874e8, devtree 0965a3f2, fakeio b89cfc32, shim 2f36ac60
  (24268 bytes), gpu 5c3b584d, boot.cfg ff838865.
- [V] Fresh 4.2.1 soak `--cycles 5 --icount 2` -> PASS; `make test` passes.

## 146. Cydia on 2.2.1: an iPhone OS 2 package set (Cydia 1.0.3222) (2026-11-26)

Design and user notes: docs/cydia.md "iPhone OS 2.x". Closes the 2.2.1 Cydia
row of the driver matrix (docs/multi-version.md §6.9), which re-log 131 left
off.

### Why the 4.x set was replaced on 2.x

- **[V]** Correction to re-log 131: Cydia 1.1.30's armv6 slice does **not**
  carry `LC_DYLD_INFO(_ONLY)` (`llvm-objdump --private-headers --arch=armv6`:
  LC_SEGMENT, SYMTAB, DYSYMTAB, LOAD_DYLINKER, UUID, VERSION_MIN 2.0,
  UNIXTHREAD, 17 LOAD_DYLIB, CODE_SIGNATURE, ENCRYPTION_INFO,
  FUNCTION_STARTS, DATA_IN_CODE), and neither do apt7-lib 0.7.25.3-16 or dpkg
  1.18.10-12. Why 1.1.30 did not launch on 2.2.1 is still unknown. An
  era-appropriate 1.0.x set was taken instead of chasing it.
- **[V]** apt.saurik.com/debs still serves every old Telesphoreo build (Cydia
  0.9.2284 … 1.1.30, apt 0.6.46, apt7 0.7.20/0.7.25, dpkg 1.13/1.14/1.18).
  Cydia **1.0.3222-73** is the last 1.0.x without `firmware (>= 4.0)` (1.0.3366
  has it); `DTSDKName iphoneos2.0.saurik`, `MinimumOSVersion 2.0`, armv6,
  setuid-root `Cydia_` behind a bash `Cydia` wrapper.
- **[V]** Set = closure of its Depends/Pre-Depends + `essential` + the repo's
  Essential `base`/`debianutils`, newest versions: 29 packages
  (`rootfs/cydia-2.x.json`, size + sha256 pinned). A scratch checker (not
  committed) unpacked every Mach-O: none has LC_DYLD_INFO, and every non-weak
  import resolves (llvm-nm, two-level namespace) in 5H11a's own
  dylibs/frameworks or inside the set. Only misses: lazy `___floatundidf` /
  `___floatundisf` (not in 2.2.1's libgcc_s) in coreutils `ls/dd/df/dir/vdir`,
  tar, find/locate, `sysctl`, `iostat`, `hostinfo`; they are reached only
  when printing 64-bit sizes as floats (e.g. `ls -h`, `df -h`) [G].

### What the build does

- `overlay-2.2.1.json`: `"cydia": "rootfs/cydia-2.x.json"`. 3.1.3/4.x keep
  `rootfs/cydia.json`; the 4.2.1 recipe and manifest are unchanged.
- `scripts/cydia.py`: a manifest `"cache"` (`cydia-2.x`) keeps these debs in
  `build/cydia-2.x`, not the shared 4.x cache; new manifest `"patch"` list
  (`patch_file`, original bytes verified) for byte patches at unpack time.
- **Stash check:** first QEMU launch showed "Reorganizing / Will Automatically
  Close When Done", SpringBoard restarted, and every later tap did the same
  (md0 writes barely moved: the stash never completed). Cydia 1.0.3222's
  `applicationDidFinishLaunching:` (0x179a0..) `readlink()`s `/Applications`,
  `/Library/Ringtones`, `/Library/Wallpaper`, `/usr/include`, `/usr/lib/pam`,
  `/usr/libexec`, `/usr/share` and runs `free.sh` (move to `/var/stash`) when
  one is a real directory; there is no `/.cydia_no_stash` check in 1.0.x. The
  seven `adds r0,#1; bne next` become `b next` (Thumb `00d1` → `00e0`, file
  offsets 0x16a1a … 0x16a86). The page loses its code-signing hash; 2.2.1's
  `cs-invalid-page-allow` kernel patch lets it load as for liblockdown.
- `/var/lib/cydia/metadata.plist` presets `Settings/Role = User` (1.0.x
  otherwise opens on "Who Are You?").
- **No default source.** With `deb http://apt.saurik.com/ tangelo-3.7 main`
  listed: refresh → "Essential Upgrade" (upgrade Cydia Installer to 1.1.30,
  install Cydia Translations + BigBoss Icon Set). Accepting it unpacked 1.1.30
  over 1.0.3222 (`Setting up cydia (1.1.30)`), 1.1.30's postinst added its
  2011 sources, and after dismissing their errors the screen stopped updating
  (heartbeats and input continued, touch target SpringBoard; not debugged). A
  dpkg hold on `cydia` gave "pkgProblemResolver::Resolve generated breaks, this
  may be caused by held packages"; an `/etc/apt/preferences` pin still raised
  the prompt, then "I wasn't able to locate file for the cydia package". So
  no list is written; docs/cydia.md tells users to pick "Ignore (Temporary)".

### QEMU (5H11a, `--icount 2`, qemu-compat2)

- **[V]** `make sdimg IOS=2.2.1`: `[cydia] 29 packages + 6 virtual`, seven
  `patched /Applications/Cydia.app/Cydia_ @0x16a..` lines. Cydia icon page 1,
  row 4, column 2 (iOS point 123,314).
- **[V]** Tap → navigation/tab bars after ~40 s, Home tab. Offline: "Network
  Error / Unable to Load (no Internet connection)". With `session.py --wifi`
  and a wifi.cfg for the ctr-wifi AP: cydia.saurik.com's home page for this
  version, "Attention: This version of Cydia is no longer supported: please
  upgrade to 1.1.x at your earliest convenience." (server text). Sections tab
  lists the installed sections (Administration, Archiving, Development,
  Packaging, Security, System, Terminal Support); Changes is empty (no
  sources). Screenshots: `build/emu-cydia221/` (not committed).
- **[V]** One tap right after the home screen first appeared (t≈60–106 s)
  opened nothing; the same tap ~30 s later opened Cydia. Boot-time
  SpringBoard load, not Cydia (later taps always opened it).

### Hardware checklist

- Cydia icon on page 1; tap opens the Home tab within ~1 min (no
  "Reorganizing", no "Who Are You?"). With Wi-Fi: saurik's "no longer
  supported" home page; offline: "Unable to Load".
- Manage > Packages lists the 29 packages + firmware/cy+ virtuals; Sections
  shows their sections.
- [G] Refresh/install against a user-added source (APT 0.7.25 + dpkg 1.18
  on 5H11a), and whether the Essential Upgrade to 1.1.30 also freezes on
  hardware (pick "Ignore (Temporary)").
- [G] `ls -h` / `df -h` from a shell (the lazy `___floatundidf` import).

### Verification

- **[V]** `make test` passes (new `test_ios2_set`, `test_patch_file`;
  test_versions expects `rootfs/cydia-2.x.json` for 2.2.1).
- **[V]** 4.2.1 default build hashes identical to origin/main before the
  change: kernel `505f451a…`, devtree `25b1bd23…`, fakeio `e5fecd66…`, shim
  `a35631eb…`, gpu `6bdaf617…`, boot.cfg `8d4079cf…`.
- **[V]** Fresh 2.2.1 image, `emu/soak.py --cycles 5 --icount 2
  --require-app --crashlogs --settle 30 --targets
  "123,314;275,226;199,226;123,226;47,226"` → **Result: PASS** (cycle 0 =
  Cydia, app diff 52.2, Cydia's launch screen; 5 apps opened, no Home
  recovery, no crash reports).
- **[V]** Fresh 4.2.1 image, `emu/soak.py --cycles 5 --icount 2` →
  **Result: PASS**.
- **[V]** After `git rebase origin/main` (c24a40d): `make test` passes;
  fresh 2.2.1 soak (same command) → **PASS** (cycle 0 Cydia, app diff 52.2);
  fresh 4.2.1 build has the same six hashes; 4.2.1 soak → **PASS**.
## 147. Audio on 4.1, 3.1.3 and 2.2.1: headphone routing, keyboard clicks and the test song (2026-11-26)
Design: docs/audio.md §4/§10, docs/multi-version.md §6.9, docs/emulator.md
(`emu/audio_check.py`). All runs used the worktree-private QEMU
`/tmp/opencode/qemu-compat2/b/qemu-system-arm`, `--icount 2`, and images built
from firmware staged under this worktree's `build/fw-*`.

### Headphones: already worked, now has an emulator check
- **[V]** On each of 4.1, 3.1.3 and 2.2.1, two plug/unplug cycles through
  `qom-set /machine headphones on|off` (QMP boolean) with Music.app open gave
  `[jack] headphones plugged in|unplugged` from the model, the ARM9's
  `[audio] headphones plugged in: CODEC output -> headphones ... iOS route ->
  headphones` / `... unplugged ...`, `ios3ds-audio: headphones in|out, jack
  interrupt` from the shim, and `[audio] iOS route -> headphones|speaker (jack
  change #N passed to iOS)`. No CODEC or DMA error, no panic. The same DT
  `function-hp_detect` (fake GPIO pin 0x0d00), VIC0 source 12 injection and ARM9
  GPIO2 jack polling are used on every version; no code fix was needed.
- **[V]** `emu/audio_check.py --tests jack` checks the model *and* guest lines
  and fails if either side is missing (3.1.3 run: model 2 in/2 out, guest
  2 in/2 out, 4 iOS route changes).

### Keyboard click: worked, but needed a reliable way to trigger it in QEMU
- **[V]** Typing `a`, `s`, `d` in Spotlight on 4.1 and 3.1.3, and in Notes on
  2.2.1, adds a new `[audio] iOS output stream #N started` and a new
  `[csnd] ch0 play ...` line; the CSND WAV shows the click burst. No change to
  the audio path: the click is an ordinary UI sound on the same DMA/TC IRQ path
  as the boot sounds.
- **[V]** The old input daemon could not resolve an app's purple event port:
  SpringBoard reported `com.apple.mobileipod-AudioPlayer` while the port is
  registered as `com.apple.mobileipod`, so in-app taps went to SpringBoard
  (`[input] no touch port for ...; using SpringBoard`) and Music's playlists
  could not be opened. `tools/ios3ds-input/daemon/input.c` now has a
  `INPUT_PURPLE_FALLBACK` build that retries the id up to the first `-`; it is
  compiled only into the 3.x, 2.x and new 4.1 binaries. The default 4.2.1
  binary is unchanged (`4747378e…` before and after a clean rebuild).

### Test song: three separate blockers
- **[V]** 4.1 (8B117): the sqlite `iTunes Library.itlp` opens after the
  relocated `MLSSqliteVFSOpen` patch (re-log 131); Music.app lists the
  "ios3ds" playlist and the ALAC song. With the purple-port fallback, opening
  the song shows the paused now-playing view; tapping Play starts stream #2
  (`stream stopped: 1478656 words, peak 17910, TC IRQs 47 (47 injected)`), and
  the CSND mix has a 14426 peak in the last 8 s. No audio fix was needed.
- **[V]** 3.1.3 (7E18): `itdbprepserver` created `Locations.itdb.cbk` and
  Music.app exited before showing its library. 7E18 has no
  `_AOU243378_7_1`; its equivalent is `AOU243378_2(MLSSqliteVFSFile*)` at
  dyld-cache offset `0x12a5844` (found from MusicLibrary's LC_SYMTAB inside the
  cache), the sole `bl` target of `MLSSqliteVFSOpen` at `0x12a55a0`. Stubbing
  it to return 0 made `Library.itdb` 0 bytes; returning 1 (`mov r0,#1; bx lr`,
  added to `rootfs/overlay-3.1.3.json`) makes the library open and Music.app
  show "ios3ds Test Song". The ALAC `.m4a` then opened paused and started no
  stream (7E18's software ALAC decoder does not accept our escape frames), so
  the same generated melody is written as PCM `.wav` on 3.1.3
  (`rootfs/overlay-3.1.3.json`: `music_wav_test`). Tapping the song starts
  stream #2; `[csnd]` reports `820203 frames, 24561 non-silent ... peak 24208`
  and the WAV tail peaks at 24375.
- **[V]** 2.2.1 (5H11a): no sqlite `.itlp` library at all; Music.app showed
  "No Music". `scripts/music.py` now writes a binary `iTunes/iTunesDB` (and an
  `iTunes_Control/iTunes/iTunesDB` copy) with the new host helper
  `tools/music-itdb/mkitdb.c` (`make music-itdb`, public libgpod API, no Apple
  data), plus the PCM song. Music.app then opens on its Artists tab with
  "ios3ds"; artist → song starts stream #2 and the now-playing view shows the
  pause icon (playing). The VirtualAudio product-table patch stays disabled on
  5H11a, as before; only the library/song path changed.
- **[G]** The 7E18 ALAC decoder failure is diagnosed only from QEMU (no stream,
  paused UI, WAV works with the same metadata/library). A real 3.1.3 console
  may decode ALAC; the `.wav` is still a valid, lossless version of the same
  test song.

### Files changed (all outside the 4.2.1 default path)
`rootfs/overlay-{4.1,3.1.3,2.2.1}.json`, `scripts/{mkrootfs,music}.py`,
`Makefile`, `tools/music-itdb/{Makefile,mkitdb.c}`,
`tools/ios3ds-input/daemon/{Makefile,input.c}`, `emu/audio_check.py`,
`docs/{audio,emulator,multi-version,re-log}.md`,
`tests/{test_versions,test_music}.py` (the new 3.1.3 cache patch offset and a test that the PCM fallback song equals the ALAC song).

### 4.2.1 unchanged
- **[V]** Fresh `make userland` + `rm -f build/sd.img && make sdimg` after all
  changes: kernel `505f451a44d9da6aae7034d427d5524d7392d9fc70640ec997c374b0b5a70e38`,
  devtree `25b1bd233447ddfc60eb69e30ad4cbd085c1963f86e77052fda969469d8327cc`,
  fakeio `e5fecd6645b3bc44d6510d7af7397c667fdc25da4517e2cd9fabc42fb673570d`,
  shim `a35631eb66b3c2855a1a02e656bb88b77b132dd89f7a3c69a429f9a033a4a3a1`,
  gpu `6bdaf6177b66a3d268d3472958a74c8797ce18c64a1a4556f04806292691f479`,
  boot.cfg `8d4079cfe114e5006a82967dee51b28de666376474fa86ebd8c9a5d4969b5e29`
  — the values printed by main before this round. `make test` passes.

### Hardware checklist (diag=1 log.txt)
1. Plug in headphones during Music playback: `[audio] headphones plugged in:
   CODEC output -> headphones ... iOS route -> headphones`, then
   `[audio] iOS route -> headphones (jack change #N passed to iOS)` and a
   `[audio] iOS headphone volume: CS42L58 .. -> CSND volume ...` line; sound
   must come only from the headphones (the analogue 3DS CODEC path is still
   unconfirmed, docs/audio.md §8).
2. Unplug: the mirrored `unplugged` / `iOS route -> speaker` lines and the
   speaker continuing the song (or the iPod app pausing on
   `OldDeviceUnavailable`).
3. Type on the keyboard: a UI click is audible; in a debug log a new
   `[audio] iOS output stream #N started` accompanies each group of taps.
4. Music.app: "ios3ds" playlist → "ios3ds Test Song"; the 39 s melody plays
   from the speaker and through the headphones when plugged in.

### Emulator checks (`emu/audio_check.py`)
- **[V]** 2.2.1 full check (`build/ac221/report.md`): jack PASS (model 2/2,
  guest 2/2, 4 route changes), click PASS (1 new output stream and 1 CSND
  start), song PASS (1 new stream/start; CSND mix tail peak 20544).
- **[V]** 3.1.3: jack PASS (`build/ac313`, model 2/2, guest 2/2, 4 routes) and
  song PASS (new stream #3, WAV tail peak 22795); the click check passed on a
  second run (`build/ac313c`: 1 stream, 1 CSND start) — the Spotlight page
  swipe is occasionally swallowed under host load.
- **[V]** 4.1: the automated jack check passes (model 2/2, guest 2/1, 3
  routes); the manual session above captured both the click (new stream #2,
  `[csnd] ch0 play`) and the ALAC song (stream #2, peak 17910) on 8B117. The
  automated click/song sequence is timing-sensitive there (Spotlight/Notes
  transitions under a loaded host), so those two lines are manual evidence.
- **[G]** A hardware `diag=1` run is still needed for the analogue CODEC
  output; QEMU stops at the CSND mix.

### Soaks and final verification
- **[V]** Four fresh images were built with `scripts/mksd.py --image ... --ios
  VERSION --staging build/fw-VERSION/staging` and soaked sequentially with
  `IOS3DS_QEMU=/tmp/opencode/qemu-compat2/b/qemu-system-arm`:
  - 4.2.1 (`build/sd.img` from the final default `make sdimg`):
    `python3 emu/soak.py --cycles 5 --icount 2 --out build/soak421-final`
    → **Result: PASS** (input after 81 s, five cycles, 0 panics/crashes/DMA
    errors, home diff 0.0).
  - 4.1 (`build/soak-41.img`): `--sd ... --cycles 5 --icount 2 --require-app
    --crashlogs` → **Result: PASS** (input after 80 s, five app/home cycles,
    no tap that opened nothing, 0 restarts/crashes/DMA errors).
  - 3.1.3 (`build/soak-313.img`): `--sd ... --cycles 5 --icount 2
    --require-app --crashlogs --targets "47,314;123,226;199,226;275,226;123,50"`
    → **Result: PASS** (input after 61 s, 0 restarts/crashes/DMA errors).
  - 2.2.1 (`build/soak-221.img`): `--sd ... --cycles 5 --icount 2
    --require-app --crashlogs --targets "275,226;199,226;123,226;47,226;275,50"`
    → **Result: PASS** (input after 60 s, 0 restarts/crashes/DMA errors).
- **[V]** `make test` passes after all changes (including the new PCM-song
  host test and the relocated 3.1.3 patch-offset expectation).

### After `git rebase origin/main` (76d86c4, 2026-11-26)
- **[V]** `make test` passes; a fresh default `make userland` + `make sdimg`
  still gives the six main hashes: kernel `505f451a…`, devtree `25b1bd23…`,
  fakeio `e5fecd66…`, shim `a35631eb…`, gpu `6bdaf617…`, boot.cfg
  `8d4079cf…`.
- **[V]** Four fresh images (`build/post-{4-1,3-1-3,2-2-1}.img` plus the
  default `build/sd.img`) were soaked again with the same commands as above:
  4.2.1, 4.1, 3.1.3 and 2.2.1 → **Result: PASS** in all four
  (`build/post-soak*/report.md`, 5 cycles each, 0 crashes/DMA errors).
- **[V]** Post-rebase 2.2.1 image (now with main's iPhone OS 2 Cydia set):
  `emu/audio_check.py --tests jack,song` → **PASS** (jack model 2/2 and guest
  2/2 with 4 route changes; one new output stream/CSND start for the song,
  CSND WAV tail peak 22936).

## 148. 3.1.3 GPU path: dark icons after Home are not fixed by a second SwapSignal (2026-11-26)

Continues §136. Symptom: with `IOS3DS_GPU313=1`, the first frame after Home
shows the dimmed icon grid with the app snapshot shrunk in the middle; a
second Home restores it.

- **[V]** Fresh-image soaks without any change (`--cycles 5 --require-app
  --icount 2 --targets "47,314;123,226;199,226;275,226;123,50"`), mirror on:
  2/5, 2/5, 2/5, 2/5 cycles needed a Home recovery (four runs, incl. one with
  the mirror forced to copy every VBlank and one with `gpu_vbl_div=4`).
  So the PICA200 mirror is not just missing a frame: the surface itself holds
  the stale animation frame (CA did not draw its last frame).
- **[V]** GPU CA with `gpu_mirror=0` (ARM9 copy): 0/5, 0/5, 1/5 stuck. The
  symptom is on the CA side; the mirror only changes how often it shows.
- **[V]** Each CA swap after Home had its direct `PICA_OP_PRESENT`, and
  `IOMobileFramebufferSwapSignal` returned 0; the CLCD source PA never
  changed (all frames into one surface, PA 0x18000000).
- **[V]** Tried: a second `IOMobileFramebufferSwapSignal` per CA swap in the
  3.x engine (`#ifdef GL_IOS3` in `tools/ios3ds-gl/iface.c`). Two soaks gave
  0/10, but a third fresh soak still had cycle 0 stuck (home diff 32.9,
  recovered by page swipe + Home; same dark-grid picture). **Not a fix**;
  the change is not committed.
- **[G]** Open: why CA stops before the final frame. Next step: gdbstub
  breakpoints (as in §128) on `IOMFBServer::link_callback`,
  `set_next_update` and `EAGLServer::render_update` across the Home close
  animation, to see whether the vsync source runs out (`[this+0xf0]`
  budget) or CA waits on a swap-completion.
- **[V]** Flag-off 3.1.3 soak 0/5 stuck, PASS; 4.2.1 soak PASS; hashes
  unchanged (kernel 505f451a, devtree 25b1bd23, fakeio e5fecd66, shim
  a35631eb, gpu 6bdaf617, boot.cfg 8d4079cf); `make test` passes.

## 149. GPU (PICA200) CoreAnimation on 2.2.1: an EGL/GLES interposer in SpringBoard (2026-11-26)

2.2.1 drew in software only: 5H11a's OpenGLES.framework *is* the IMG MBX
driver (no engine bundle to replace, §135). This round makes CoreAnimation
composite on the PICA200 there, opt-in at build time like 3.1.3
(`IOS3DS_GPU221=1 make sdimg IOS=2.2.1`). Firmware staged read-only from the
compat16 worktree into this worktree's `build/fw-2.2.1`. 5H11a code read with
`llvm-objdump-18` (QuartzCore keeps its symbols; addresses are 5H11a VAs).

### How 2.x CoreAnimation uses GL

- **[V]** The render server (SpringBoard, `CADisplayH1Server` for the
  AppleM2CLCD display) picks `gles_context` when `CA_ENABLE_OGL=1` and
  `gles_get_config` (0x31e3a0b4) gets an EGL config:
  `eglEnableInternalSurface(1)`, `eglGetDisplay(0)`, `eglInitialize`,
  `eglChooseConfig({EGL_SURFACE_TYPE, EGL_PIXMAP_BIT})`, then
  `eglGetConfigAttrib` for R,G,B,A,DEPTH,STENCIL,SAMPLES,SAMPLE_BUFFERS
  (table at 0x39dcfddc); 8/8/8/8 with the rest 0 is the 'BGRA' config.
  Otherwise it falls back to MBX2D (off: `CA_ENABLE_MBX2D=0`) and then to
  the software compositor.
- **[V]** Render targets are EGL *pixmap* surfaces on CoreSurface buffers
  (`eglCreatePixmapSurface(dpy, cfg, CoreSurfaceBufferRef, 0)`,
  `eglMakeCurrent(dpy, s, s, ctx)`); the display is the CLCD's default layer
  surface. Layer images and offscreen surfaces are sampled with
  `glTexImageCoreSurfaceAPPLE(target, CoreSurfaceBufferRef)` (target 0x84F5).
  The rest is plain GLES 1.1 (44 functions, `nm -u` of QuartzCore).

### The interposer

- **[V]** `tools/ios3ds-gl/ios2/egl2.c` + the GL core (api/draw/raster/
  pica_be/ca/stereo) build as `/usr/lib/ios3ds-gles2.dylib` (2.x target,
  `-ffixed-r9`, `dyld2_glue.S -DDYLD2_DYLIB`). It is appended to SpringBoard's
  `DYLD_INSERT_LIBRARIES` (after the a80211 dylib) and its `__interpose`
  table replaces QuartzCore's EGL/GL imports with the engine's dispatch
  table. Apps are not touched (their EAGL stays the IMG framework). Link
  names come from small source stubs (`ios2/stub_*.c`), no Apple binary.
- **[V]** `eglInitialize` runs the 4.x gate (`pica_ca_gate`: boot.cfg
  ca_gpu/gpu, back-end self-check, CA scene check); a failure leaves CA on
  its software compositor. Pixmap surfaces are drawn in place (32 bpp, GL
  row 0 = last memory row), the PICA200 back end keeps the GPU copy and
  writes it back (direct present into the VRAM CLCD buffer, arena copy for
  scattered CoreSurface memory). Switching the current surface syncs it and
  bumps a per-buffer seed so textures of that buffer re-upload.
- Bring-up walls, in order (each one a fresh-image QEMU run):
  1. **[V]** `glGetString(0x1F04)` (a second, IMG extension list) returned
     NULL → strstr crash. Then `CoreAnimation: unsupported graphics
     hardware; need APPLE_texture_rectangle extension; need
     APPLE_core_surface_texture extension`: both names are now in the
     answer to `GL_EXTENSIONS` and 0x1F04.
  2. **[V]** SIGBUS in MBXConnect from `CADisplayH1::gpu_swap_token`
     (0x31e39718 → `mbx2DSwapNotification`) and `gpu_flush` (`mbx2DFlush`):
     both answered in the dylib (our GPU work is synchronous).
  3. **[V]** SIGSEGV in dyld's lazy binder (0x2fe1e724) on the render thread,
     right after `ca_frame()`: compiler-rt's prebuilt `__udivdi3`/`__umoddi3`
     use r9, which 5H11a reserves as the thread pointer. `ios2/udivdi3.c`
     (built `-ffixed-r9`) replaces them; no other function in the dylib
     touches r9 (objdump).
  4. **[V]** `CADisplayIOMFB::finish_update` passes swap flag 1 ("wait for
     the MBX swap token") to `IOMobileFramebufferSwapSetLayer` after a GL
     frame (0x31e3b85c..0x31e3b940); nothing signals it. Interposed: bit 0
     cleared, as the software path passes 0.
  5. **[V]** Lock never finished (screen never went off, later taps
     ignored). A watchdog thread in the dylib (`IOS3DS_GL_WATCHDOG`, with
     `IOS3DS_GL_WATCHDOG_ABORT=idle:570`) trapped SpringBoard for a crash
     report: render thread in `IOMobileFramebufferSwapWait` from
     `CADisplayIOMFB::set_enabled` (0x31e3ac3c, after SwapEnd), main thread
     waiting for it in `CADisplayServer::set_blanked`. In GL mode the wait
     is answered in the dylib (`[gl] ios2: IOMFB swap wait (token N, flags
     0x2) answered here`). With that the soak passes.
- **[V]** `scripts/crashlogs.py` failed on 5H11a listings (`hfsplus ls`
  prints a time column there): regex fixed. `check-stub-symbols.py` also
  accepts the names our own `build/gl-ios2` stubs provide.
- `gpu_mirror=1` in the GPU image (as 3.1.3). With `gpu_mirror=0` (ARM9
  CPU copy) before fix 5 the screen showed stale animation frames, so the
  mirror is kept.

### Results

- **[V]** Fresh-image soaks on the final tree (rebased on 48a1bc4+),
  `IOS3DS_QEMU=/tmp/opencode/qemu-compat2/b/qemu-system-arm`, `--icount 2`:

  | image | command | boot | cycles / home | result |
  |---|---|---|---|---|
  | 2.2.1 `IOS3DS_GPU221=1` | `soak.py --cycles 5 --require-app --crashlogs --targets "275,226;199,226;123,226;47,226;275,50"` | 62 s | 5 apps opened (diffs 53.7–107.1), home diff 0.0, home settled after 6/22/16/20/20 s, lock round ok, 0 restarts | **PASS** |
  | 2.2.1 default | same | 50 s | 5 apps opened, home diff 0.0, settled after 6 s every cycle, lock round ok, 0 restarts | **PASS** |
  | 4.2.1 | `soak.py --cycles 5` | 70 s | home after every Home, 0 restarts | **PASS** |

  4.2.1 default build hashes are unchanged: kernel `505f451a…`, devtree
  `25b1bd23…`, fakeio `e5fecd66…`, shim `a35631eb…`, gpu `6bdaf617…`,
  boot.cfg `8d4079cf…` (no kernel, shim or boot file is touched; the 2.2.1
  switch only changes the 2.2.1 recipe and boot.cfg). `make test` passes.
  (`build/mkitdb` for the 2.2.1 iTunesDB was copied from another worktree:
  libgpod-dev is not installed here.)
- **[V]** Log of the GPU image: `[gl] CA compositor: OpenGL ES on the
  PICA200 (GPU self-check and scene check passed)`, `[gl] ios2: EGL display
  up for CoreAnimation`, `first pixmap surface 320x480`, `first CA display
  frame (320x480) on the ios3ds PICA200`, `[gl] pica: GPU present check
  passed`, `[gpu] display mirror ACTIVE`; 0 SpringBoard restarts, screenshots
  of home and apps correct (orientation, colours).
- **Frame times (QEMU, `--icount 2`):** the ARM9 mirror reports
  2.8–5.2 fps while animating (`[gpu] mirror: 158 frames in 30 s`, GPU per
  frame upload 3.6–13.8 ms, draw 1.0–10 ms, read-out 2–10 ms; frame avg
  9.5–12 ms). CA's own `[perf] CA:` line needs ≥ 2 frames within a second
  and never printed in QEMU. Home after an app settles in 6–20 s against
  6 s with software CA, i.e. **slower in QEMU**, like 3.1.3 (§135): the
  PICA200 model, the arena copies and the read-out run on the host CPU.
  Hence opt-in.
- Known: `[pica] arena full: 614400 bytes wanted, ~1 MiB free in 73 blocks`
  a few times per run (fragmentation by 600 KiB CA surfaces; the back end
  evicts/falls back per target and the run continues). CoreSurface buffers
  of CA's offscreen surfaces are not physically contiguous (arena copy).

### Needs hardware [G]

- Build `IOS3DS_GPU221=1 IOS3DS_GPU221_DIAG=1 make sdimg IOS=2.2.1`, boot
  with `diag=1`. Look for the lines above, `[gl] CA:` every 60 frames (render
  thread CPU per frame, presents, arena copies), `[perf] CA:` histograms and
  `[gpu] mirror: N frames`. Open/close Notes and Settings 5×, Lock + wake
  (Home, slide): the screen must come back. Compare timings with the
  default image; if slower or broken, ship the default.
- Watch for `[gl] ios2: watchdog: render thread inside ... for N ms` (a stall
  inside the engine; the DIAG image traps after 20 s for a crash report).
- Answering SwapWait at once may tear on a real CLCD (no MBX fence exists
  here); a photo during a scroll shows it.
- The same SwapSetLayer token flag / SwapWait pattern may be behind 3.1.3's
  GPU-path "first frame after Home" issue (§148); not checked there.

## 150. Release installer version choice and user docs for the other iOS versions (2026-11-26)

- **[V]** `scripts/install.py` now accepts only `--ios 4.2.1|4.1|3.1.3|2.2.1`
  (default 4.2.1). An unknown version is refused by an argparse `type` check
  with the supported list in the message; inventory-only entries in
  `scripts/versions.py` (e.g. 3.0, 4.0) are not installer choices.
- **[V]** `verify_ipsw()` hashes the supplied IPSW and compares it with the
  pinned sha1 in `scripts/versions.py` before `fwprep.py` runs. This also
  covers IPSWs over 1 GiB, which the old `fwprep.py` check skipped, and
  `--force` cannot bypass it. A differently named file gets a note, not a
  refusal, because the 3.1.3 (7E18) archive may be renamed locally.
  `check_staging()` rejects a pre-extracted folder whose manifest names a
  different build.
- **[V]** The selected version flows into `fwprep.py --ios` (public keys for
  that build from api.ipsw.me) and `mksd.py --ios` / `mkboot.py`
  (`build_tree()`), i.e. the same patch set, overlay and userland as
  `make sd IOS=VERSION`. The zip's root `install.py` forwards every argument
  by `runpy`, and `mkrelease.py` now also ships `docs/other-versions.md`.
- **[V]** Every non-default version prints a prominent
  `WARNING: iOS X (build) is HIGHLY EXPERIMENTAL ... NEVER on a real 3DS ...
  MAY NOT BOOT ...` notice before any work; 4.2.1 prints nothing extra.
  README.md gained a user-facing section, docs/release.md the zip behaviour,
  docs/INSTALL.md a pointer, and CI smoke checks for version choice and
  rejection. `tests/test_installer.py` adds 9 offline tests (choice, sha1,
  staging, warning, mksd argument, packaging). `make test`: **PASS**
  (119 Python unit tests plus all host tests).
- **[V]** 4.2.1 default build before vs. after this change: kernel
  `505f451a…`, devtree `25b1bd23…`, fakeio `e5fecd66…`, shim `a35631eb…`,
  gpu `6bdaf617…`, boot.cfg `8d4079cf…` — all byte-identical to the values in
  §6.7/re-log 149 and to a fresh build from the unmodified tree. The
  `ios3ds.firm` / `rootfs.hfs` bytes differ only because `git describe --dirty`
  puts the working-tree state and build time in the payload version string;
  no boot file, kernel, shim, GPU or recipe was touched.
- **[V/G]** **Soak not re-run in this session.** A fresh `make sdimg` was
  built, but other agents kept 4–10 QEMU processes running for the whole
  30-minute wait window, so starting another would break the
  one-QEMU-at-a-time rule. **[G]** On a free host, rerun
  `rm -f build/sd.img && make sdimg && IOS3DS_QEMU=/tmp/opencode/qemu-compat2/b/qemu-system-arm python3 emu/soak.py --cycles 5 --icount 2`
  and expect **Result: PASS** (as in §149); no 4.2.1 boot path was changed.
  Older-version images were not rebuilt here either (their IPSWs are not
  staged in this worktree), so §6.9's PASS results stand unmodified.
- **[G]** Hardware: for each newly installable version, install with
  `--ios` and `diag=1`, back up the card, boot on a console and attach
  `sd:/ios3ds/log.txt` with the version and IPSW sha1 to an issue. The
  opt-in GPU paths need the console checklists in re-log 135 and 149.

## 151. 3.1.3 GPU path: the "dark icons after Home" were one slow GPU frame, not a hang (2026-11-26)

Continues §148. Traced with the QEMU gdbstub (hardware breakpoints through
`emu/gdb/rsp.py`) on a fresh `IOS3DS_GPU313=1 make sdimg IOS=3.1.3` image.
7E18 QuartzCore symbols come from the cache's local symbol tables:
`IOMFBServer::link_callback` 0x31bdc944, `set_next_update` 0x31bdcdec,
`Server::render_for_time` 0x31bdfba0, `IOMFBDisplay::finish_update`
0x31bdb7a4, `wait_update` 0x31bdafb8 (calls `IOMobileFramebufferSwapWait`),
stubs `IOMobileFramebufferEnable/DisableVSyncNotifications` 0x31bf596c/0x31bf595c.

- **[V]** The vsync loop works as designed: `set_next_update` arms
  `[this+0xf0] = 3` and enables vsync notifications when it was 0, each
  `link_callback` decrements it and calls `render_for_time`, which calls
  `set_next_update` again while an animation runs. After the last frame of
  the close animation (`t2 = inf`) the count runs out and
  `DisableVSyncNotifications` is called; a later commit re-enables it. No
  lost enable, no missing callback.
- **[V]** `IOMobileFramebufferSwapWait` (hint from the 2.2.1 GL work): no call
  took more than 0.3 s across four Home cycles, so the 2.x "MBX swap token"
  wait is not what happens on 3.1.3.
- **[V]** One `render_for_time` → `finish_update` takes **2–5 s wall time**
  near the end of the close animation (0.5–1.3 s for the others). Sampling
  the stopped CPU during it (gdb interrupt every 0.25 s, frame-pointer
  backtraces): ~90 % of samples are the kernel's `machine_delay_until`
  (0xc006b3ba, spinning on `mach_absolute_time`) called from the shim's GPU
  part (0xc08b2aa0), i.e. `IODelay` polling in `pica_drv` while the PICA200
  runs a command list. User side: `api_Clear` → `pica_clear` →
  `bind_target` → `sync_target` → `d_submit` (engine slide 0x196000). So CA
  is waiting for QEMU's PICA200 model to finish a full-screen frame.
- **[V]** The screen is not stuck: a soak variant that first waits 6 s with
  no input showed the only "stuck" cycle of 10 reach the home screen by
  itself (home diff 32.5 → ≤ 12, `recovered_with: wait (no input)`), and a
  24 s screenshot probe saw 0 of 11 cycles still dark. `emu/soak.py`'s
  `settled()` (two identical shots 2 s apart) took the frozen frame for the
  end of the animation and then sent page swipe + Home, which made it look
  like a second Home was needed. The same happens after an unlock (lock
  rounds 4/9 of a 10-cycle run: a half-drawn frame, then Home on page 1
  opened Spotlight).
- **[V]** Fix (harness, `emu/soak.py`, no product code): `settled()` accepts
  a still frame at once only if it matches the home reference; any other
  frame must stay unchanged 6 s longer. Before any recovery input the Home
  check looks again after 6 s, and a cycle that is home by then is reported
  as `late_settle` ("home screen late but reached without input"), not as a
  failure; the unlock check also looks again before pressing Home (which on
  page 1 opens Spotlight). A screen still wrong after that is handled as
  before. A first 10-cycle run with only the 6 s re-look still had a late
  Home press (cycle 5) and a half-drawn unlock frame (cycle 9) mistaken for
  the end; the `settled()` change removed both.
- **[G]** On hardware the PICA200 runs these lists in milliseconds, so the
  pause should not be visible. Hardware check: with the opt-in GPU image,
  open and close five apps and confirm that one Home always shows the full
  icon grid within ~1 s without touching anything; a `diag=1` log should
  show no `[pica] TIMEOUT`.

### Verification

- **[V]** Fresh images, `IOS3DS_QEMU=/tmp/opencode/qemu-compat2/b/qemu-system-arm`,
  `--icount 2`, targets `47,314;123,226;199,226;275,226;123,50`:

  | image | command | result |
  |---|---|---|
  | 3.1.3 `IOS3DS_GPU313=1` (run 1) | `soak.py --cycles 10 --require-app --crashlogs` | **PASS**, 10/10 home diff ≤ 0.03, 0 recoveries, 0 late, lock rounds 4/9 home |
  | 3.1.3 `IOS3DS_GPU313=1` (run 2) | same | **PASS**, 10/10 home diff ≤ 0.03, 0 recoveries, 0 late, lock rounds 4/9 home |
  | 3.1.3 default | `soak.py --cycles 5 --require-app --crashlogs` | **PASS**, 0 recoveries |
  | 4.2.1 default | `soak.py --cycles 5` | **PASS**, 0 recoveries |

- **[V]** `make test` passes. 4.2.1 fresh build: kernel `505f451a…`, devtree
  `25b1bd23…`, fakeio `e5fecd66…`, shim `a35631eb…`, gpu `6bdaf617…`,
  boot.cfg `8d4079cf…` (unchanged).
- **[G]** Making QEMU's PICA200 frames faster (so the pause also goes away
  in the emulator) is a separate performance item, not done here.

## 152. One-boot hardware test kit: `boot.cfg hwtest=1` (2026-11-26)

Goal: one console boot that checks every driver and writes a clear
PASS/FAIL/SKIP block to `log.txt` (and the screen), so a hardware report
is one file instead of a dozen manual steps. User docs:
[real-hardware.md](real-hardware.md) §6; README switch table.

### Design

- **ARM9:** new `boot.cfg` key `hwtest` (`bootcfg.c`, host-tested in
  `tests/test_bootcfg.py`): `hwtest=1|2` appends the boot-arg
  `ios3ds_hwtest=N` and logs a yellow `[hwtest]` line. Default 0: the
  generated `boot.cfg` is unchanged (hash `8d4079cf…`); no kernel, shim,
  devtree, fakeio or GPU file is touched.
- **Userland:** `tools/ios3ds-diag/hwtest.c` → `/usr/libexec/ios3ds-hwtest`
  (4.x build for 4.2.1/4.1/3.1.3, `-target armv6-apple-ios2.0 -ffixed-r9`
  + `dyld2_glue.S` build for 2.2.1). Job `com.ios3ds.hwtest.plist` runs
  `ios3ds-diag -g ios3ds_hwtest /usr/libexec/ios3ds-hwtest`, so with
  `hwtest=0` it exits at once (same gate as gltest/gpstest).
- It waits for SpringBoard's Mach service plus 30 s (45 s on 2.x/3.x), then
  runs the checks. Reused tools: `ios3ds-tone` (play + `rec`; new env
  `IOS3DS_HWTEST` makes it write to stdout instead of `/dev/console`),
  `ios3ds-gyrotest` (4.x, stopped at its `done:` line), `ios3ds-gltest`,
  `ios3ds-gpstest` (`IOS3DS_HWTEST`: no console redirect, no runtime
  location switch). Read-only mailbox snapshots: `kern.ios3ds_mcu` (battery,
  RTC, sliders, HOME/POWER counters, gyro state, backlight),
  `kern.ios3ds_input` (ring: buttons, pen). Camera: `ios3ds_cam`
  START/FRAME/STOP per sensor. Accelerometer: ios3ds-mcud keeps it off by
  default, so hwtest writes `kern.ios3ds_mcuctl` `{acc=1, backlight=0}`
  for 3 s and back to `acc=0` (only if nobody else had requested it).
  Wi-Fi: en0 DHCP address (configd/WiFiManager scanned and joined the saved
  network) + `NSData dataWithContentsOfURL:` of `https://example.com/`
  (3 tries; `ios3ds_hwtest_url=` overrides).
- Prompts are `CFUserNotificationDisplayAlert/Notice` (SpringBoard shows them
  on 2.x–4.x). Every prompt times out; no answer = SKIP. `hwtest=2` skips
  all prompts. Final on-screen alert: "Hardware test: N PASS, N FAIL,
  N SKIP" with the check names grouped by result.
- Version from `kern.osversion`; CoreMotion (gyro) is iOS 4 only, gltest
  is not on the 3.1.3/2.2.1 rootfs (SKIP with reason), mic on 2.x is SKIP
  (no built-in mic route, re-log 134), HTTPS failure on 2.x/3.x is SKIP.
- `emu/hwtest_check.py --mode 2|1|0`: QEMU harness (ctr-wifi AP, mic model
  `IOS3DS_MIC=sine:440:8000`). Mode 1 plays the person: drags for touch,
  QEMU keys for buttons/HOME/POWER, `qom-set mcu-volume` / `mcu-3d-slider`.
  Mode 0 is the control: home screen must come up, no `[hwtest]` line.

### Results (QEMU)

- **[V]** 4.2.1, `hwtest=2` (`emu/hwtest_check.py --mode 2`): summary after
  59–80 s of checks, **13 PASS, 0 FAIL, 5 SKIP** (gps, headphone, buttons,
  stereo3d, lock/wake need `gps=`/a person). Accel 39 samples in 3 s,
  CoreMotion gyro 209 ARM9 samples, both cameras one frame, gltest pass,
  tone route "Speaker", mic 66150/66150 frames (QEMU sine), en0 10.0.2.15,
  `https://example.com/` 577 bytes, backlight level 90. The on-screen alert
  shows "Hardware test: 13 PASS, 0 FAIL, 5 SKIP" with the grouped names
  (screenshot `build/emu-hwtest/summary-mode2.png`).
- **[V]** 4.2.1, `hwtest=1` + `stereo3d=1` + `gps=52.5200,13.4050`, harness
  playing the person (`--mode 1`): **19 PASS, 0 FAIL, 1 SKIP** (headphone:
  the yes/no alert is left to time out). Touch 78 pen samples, buttons
  `ff3` (all ten wanted), volume slider 0..63, 1 HOME press, 3D slider
  0..255, lock/wake: 2 POWER presses, backlight off then on; gps PASS (the
  separate gpstest job had already moved the location to 40.71,-74.01, now
  reported as "moved at runtime"). Every unanswered prompt timed out to SKIP.
- **[V]** 4.2.1 control, `--mode 0` (no `hwtest=`): home screen up
  (`[mcud] connected to SpringBoard`), **0 `[hwtest]` lines** in 900 s.
- **[V]** 4.1: first run 11 PASS / 2 FAIL — outer camera gave no frame
  within 6 s, and the first HTTPS fetch 12 s after the join failed
  (NSCocoaErrorDomain 256). Fixed in the tool (15 s frame wait, 3 tries
  15 s apart); rerun **13 PASS, 0 FAIL, 5 SKIP**, the fetch succeeded on
  the second try.
- **[V]** 3.1.3: **11 PASS, 0 FAIL, 7 SKIP**: gyro chip PASS without
  CoreMotion, gpu SKIP (no gltest on 7E18), https SKIP (NSCocoaErrorDomain
  256: 3.x TLS), mic 66150 frames, both cameras one frame, join after 20 s.
- **[V]** 2.2.1 (iPhone OS 2 build of the tool): first run 9 PASS / 1 FAIL /
  8 SKIP; the FAIL was cameras (START accepted, no frame from either
  sensor in 15 s). Cameras are n/a on 2.x (no Camera.app/MediaCapture,
  §6.9), so the tool now reports SKIP with the frame-path result.
  Mic SKIP (no route, re-log 134), https SKIP (TLS), gpu SKIP.
  Rerun with that rule: **9 PASS, 0 FAIL, 9 SKIP**, END: PASS.
- **[V]** Soaks, fresh images, `hwtest` off (default `boot.cfg`):
  4.2.1 `soak.py --cycles 5 --icount 2` **PASS** (input after 130 s, 5 apps,
  0 panics/crashes, no SpringBoard restart after the first connect);
  4.1 `--require-app` **PASS** (150 s, 5 apps); 3.1.3 `--require-app
  --targets "47,314;123,226;199,226;275,226;123,50"` **PASS** (80 s);
  2.2.1 `--require-app --targets "275,226;199,226;123,226;47,226;275,50"`
  **PASS** (90 s).
- **[V]** 4.2.1 default build hashes unchanged: kernel `505f451a…`, devtree
  `25b1bd23…`, fakeio `e5fecd66…`, shim `a35631eb…`, gpu `6bdaf617…`,
  boot.cfg `8d4079cf…`. `make test` passes (new `tests/test_hwtest.py`,
  `hwtest` cases in `test_bootcfg.py`, version add lists, stub SDK symbols
  `_kill`, `_pipe`, `_strcat`).

### Findings for other areas

- 2.2.1 camera frame path: `ios3ds_cam` START returns OK but no FRAME
  arrives from either sensor in QEMU (3.1.3/4.x deliver at once). Camera is
  n/a on 2.x anyway; not chased (camera owner).
- 4.1 right after the join, the first `NSData` HTTPS fetch fails with
  NSCocoaErrorDomain 256 and no NSUnderlyingError; 15 s later it works
  (DNS/route settling). Safari users may see the same first-page error.

### Needs hardware [G]

- Boot once with `hwtest=1` (and `stereo3d=1`, `gps=…` if wanted), follow
  the prompts, send `log.txt`. The `[hwtest] ==== SUMMARY` block is the
  report. Unchecked on a console: CFUserNotification prompts on the real
  SpringBoard, the "heard it?" answers (speaker/CODEC/amp, headphones),
  a real mic level (QEMU used a sine), the ITG-3270 through CoreMotion,
  the 3D look, and whether the 4.2.1 summary alert is visible after the
  lock/wake check (in QEMU mode 1 the final screenshot was black: the
  screen had been locked by the test itself; the alert appears on wake).
## 153. Public issue #10: the Old 2DS log is from a stale, corrupt build (2026-11-26)

Dana's four attachments (downloaded to `build/issue-data/attachments/`, treated
as data and never committed) are two identical `log.txt`/`log_prev.txt` pairs.
They answer the "doesn't launch / shows x0000" report without needing new
driver work:

- **[V]** Every log starts `ios3ds m1-0.3.0-62-g6d963b6-20261121.0001`, the
  same stale build as issue #9's chainloader logs (§118/§126). `shim: 23516
  bytes at 20840000 (magic e7bfe619)` is not a shim (`SHIM` = 4d494853), so
  the ARM11 jumped into unrelated data. After Wi-Fi/WMI comes up and
  `B held: jump cancelled`, the log shows `[probe] ARM11 stalled`,
  `[hb] pc x00000000`, all-zero vectors/TTBR and no `[pica] present` or
  SpringBoard line. The "barcode"/`x0000` picture was the loader's stale CLCD
  buffer, not iOS.
- **[V]** Current main already refuses this state: `d01c35f` checks the shim
  header before the jump and prints the reinstall hint; `215ddf5`/`af68575`
  detect and repair older `luma/payloads/*.firm`, log `argv0=` and the
  `payload check:` decisions; `96c0a03` keeps the boot console on screen until
  the CLCD buffer changes after the first iOS frame. `tools/ios3ds-pica`'s
  mirror test covers the wait/first-frame transition, and `make test` passes.
- **[V]** Both new lines are visible on real hardware in issue #1's later New
  2DS XL log (`35435217-log_prev.txt`, build `0.4.0-244`):
  `payload check: 0:/luma/payloads/ios3ds.firm is this build`, then
  `CLCD is on; waiting for the first iOS frame before mirroring its buffer`
  and `first iOS frame in the CLCD buffer: mirroring it now`.
- **[G]** No console retest has been posted. Dana needs matching files (boot
  once from the `.3dsx` launcher, which repairs the chainload payload), then a
  chainload with a fresh `log.txt`. Expected: `shim.bin` 24268 bytes with
  magic `SHIM`, no bad-header line, `CLCD is on; waiting for the first iOS
  frame` followed by `first iOS frame in the CLCD buffer`, then SpringBoard
  with working input. Until then the issue is **partly fixed on main**, not
  confirmed fixed on the reporter's Old 2DS.

## 154. Public issue #6: WPA2+WPA3 popup harness repaired, join run still blocked (2026-11-26)

- **[V]** The product fix is already in main (`8f66c8d`, §84,
  `docs/wifi.md` §10.8): `wlan_ie_wpa2_view()` strips SAE/PSK-SHA256 AKMs and
  MFPC/MFPR from the *iOS copy* of mixed RSN IEs, while the ARM9 keeps the real
  IE and joins with WPA2-PSK. Host tests cover both AKM orders, a following
  vendor IE, plain WPA2, SAE-only, open IEs and a truncated RSN; `make test`
  passes.
- **[V]** The end-to-end harness itself had a bug: `main()` called
  `home_screen(out)` / `shot(out, ...)` without binding `out`, so
  `emu/wifi_popup_test.py` died with `NameError: out` right after boot, before
  any join check. It now binds `out = a.out`; `tests/test_wifi_popup_harness.py`
  checks that binding offline (part of `make test`).
- **[V]** A fresh-image mixed-AP run after the fix reached
  `boot to SpringBoard`, `chip idle without wifi.cfg`, `home screen` and
  `Settings > Wi-Fi scans` (4/4 PASS) before its 3600 s wall-clock limit. The
  SD log then shows `APNetworksController` → `APOtherNetworkSetup` →
  `APOtherNetworkController` and no `[a80211] associate`, i.e. a hardcoded tap
  landed on Settings' "Other Network" row instead of the AP, so the popup path
  was never reached. This is a harness/UI-coordinate problem, not a failed
  WPA2 join; the run's tail is in `build/issue-data/popup-run2.log` (data only).
- **[G]** Still needed: calibrate the popup harness's network-row tap (as
  `emu/wifi_ui_test.py --net-row` allows) on a lightly loaded host, then get a
  PASS through both joins, plus one console join from the "Select a Wi-Fi
  Network" alert on the reporter's Old 3DS.

## 155. Stereo 3D round 4: transitions, `stereo_depth`, and the PDC0 switch that never reached PDC0 (2026-11-26)

Goal: keep per-layer depth through app open/close, the switcher, folders and
page swipes (no flicker, no eye mismatch, flat when the depth is unknown),
add a depth-strength setting, keep `stereo3d=0` byte-identical.

### The real blocker: PDC0 never left 2D
- **[V]** `kernel/shim/pica_stereo.c` addresses PDC0 as `0xBFE00000 + off`,
  but that VA is the GX block's base (PA `0x10400000`); PDC0 is `+0x400`
  (entry.S `PDC0`). Since re-log 90 every 2D/3D switch wrote
  `0x10400024..0x10400098`. QEMU (`xp` on PDC0): `+0x70 = 0x00080341`
  (line doubling, 2D) with the barrier on and the ARM9 logging "both eyes";
  `pdc-3d-mode` read 0x40. Every earlier "3D" QEMU result showed the 2D
  scan-out; re-log 127's hardware checklist would have shown a flat picture.
- **[V]** Fix without touching `gpu.bin` (hash rule): `scripts/mkboot.py`
  writes `gpu3d.bin` = `gpu.bin` with that file's single GX-base literal
  (inside `shim_stereo_poll`) moved to `0xBFE00400` (one byte differs,
  `cmp -l`: offset 10618, 0x00 -> 0x04). The ARM9 loads it instead of
  `gpu.bin` only with `stereo3d=1` (log: `[stereo] stereo3d=1: GPU part
  0:/ios3ds/gpu3d.bin instead of gpu.bin`). After it, QEMU shows PDC0 in AB
  interlace and real per-class disparity (tour below).
- **[G]** On a console this is the first build whose 3D switch writes the
  real PDC0 timing registers (`st_off` 0x24..0x64 with the libn3ds stereo
  preset) and format. Before, it wrote GPU external registers 0x10400024..
  0x10400098 on every switch.

### Transitions (engine, `tools/ios3ds-gl/stereo.c`) [V host test + QEMU]
- Baseline (origin/main firm + engine, tour 1, diag frame log): the idle home
  screen was **wiped flat** by the status-bar spinner's damage updates
  (4..15 draws, too few stamps -> wipe), so `stereo_is_home` failed most of
  the time; folder halves flipped class 2 <-> 3 as they crossed the dock band.
- Damage updates (no full-screen draw, < 16 draws) keep the scene: home ->
  classified by position and stamped (even their first draws), app ->
  dropped. Large layers (>= 1/8 screen) keep a sticky class per texture
  (`Tex.stereo_cls`, 30-frame lifetime). An opaque full-width layer from
  under the status bar to the bottom with < 4 icon draws over it is an app
  window: flat frame. After an app, a frame is home only with the
  wallpaper's texture as background (or same size + >= 12 icon draws).
  Two broader rules were tried and dropped: "opaque >= 85 % of the screen"
  and the app shape without the icon test (both left the home screen flat).
- ARM9 hysteresis (`display.c`): 3D after 2 home probes in a row, held
  through 8 non-home probes; a held flat frame gives identical eyes.
- `boot.cfg stereo_depth=0..100` (default 100, not in the generated file):
  `k' = ceil(k * depth / 100)` on the ARM9 (`stereo_depth_k()`), 0 = flat.

### Evidence
- `emu/stereo_tour.py` (new): boots a `stereo3d=1` image, slider 255, bursts
  of frames through each transition; saves **both eyes** from PDC0
  framebuffer A/B (`pmemsave`), plus the anaglyph, and measures per-band
  disparity and L/R residual. Final run (`build/emu/tour-new5`, not
  committed): every burst `modes 33333333333333`, 0 3D/2D toggles, 0 depth
  sign flips; home: status bar -6, icons 0, dock -6 screen px (= 2k iOS px
  at k=4, 0.75 scale); folder/switcher/app zoom keep icons at 0 and the
  dock/status bar in front; app frames have identical eyes (L/R mean diff
  0.0; before: status bar popped out over the app).
- Eye-mismatch check: 2 of ~140 3D frames had a large residual: one frame
  mid-swipe from Spotlight (`swipe_to_home_12`) shows the left eye one
  scroll step ahead of the right (L/R diff 66 vs 7). Either the capture
  caught the mirror between its two eye renders or the mirror really
  presented eyes from two uploads for one VBlank. [G: watch for a
  one-frame double image during fast swipes on the console]
- Known: Game Center's "Loading" view still gets a small stamped region
  (L/R diff 1.7 vs 6.9 on home); the barrier stays on (with identical or
  nearly identical eyes) in apps whose alpha happens to look stamped to the
  ARM9 probe.
- Host: `IOS3DS_GLTEST_STEREO=1` now also checks damage updates after a home
  and an app frame, the app-window frame and a home snapshot;
  `test_stereo_copy` checks the hysteresis and `stereo_depth=0`;
  `tests/test_stereo.py` checks `stereo_depth_k()`.

### Soaks, tests, cost
- **[V]** Default build after the rebase: `kernel 505f451a`, `devtree
  25b1bd23`, `fakeio e5fecd66`, `shim a35631eb`, `gpu 6bdaf617`, `boot.cfg
  8d4079cf` (unchanged); new card file `gpu3d.bin 4aa214b9`. `make test`
  green.
- **[V]** `stereo3d=0` fresh-image `soak.py --cycles 5 --icount 2`: **PASS**.
- **[V]** `stereo3d=1 --slider3d 255` fresh images: **FAIL** twice. Both
  runs: SpringBoard SIGKILLs Game Center (and Calendar/Photos) a few seconds
  after Home ("exited abnormally with signal 9"), and one tap (Photos once,
  Calendar once) rendered no app within 180 s. Controls on fresh images:
  `stereo_depth=0` (engine stamping on, no 3D) FAIL with the same SIGKILLs
  but every app opened; **origin/main's GL engine** with this firm,
  `stereo3d=1`, `stereo_depth=0`: FAIL with the same SIGKILLs. So the kills
  come with `stereo3d=1` on current main, not from this round's engine
  rules (re-log 127's PASS predates them; not traced). The app that did not
  open appeared only with the real two-eye mirror running, i.e. only now
  that PDC0 really switches; in QEMU the two-eye frame costs ~14 ms vs
  ~10 ms flat (`[stereo] mirror GPU cost`). [G: check app launches with the
  slider up on a console]
- **[V/noisy]** `emu/perf.py --runs 3 --icount 2 --slider3d 255`: one
  `[perf] CA:` window survived in the log (the diag log fills up):
  `30 frames in 1004 ms; min/avg/max 5/33/222; <17:11 17-33:13 33-66:1
  66-100:4 >=100:1`; folder open 14.2/12.4/46.1 s, close 21.0/21.4/4.1 s.
  Host load average was 12-14 (other agents' kernel builds), so this is
  not comparable with re-log 127's 24-26 ms (taken with PDC0 still in 2D,
  i.e. without real two-eye frames). Re-measure on an idle host.

### Hardware checklist (Old 3DS, `stereo3d=1`, `diag=1`, card with `gpu3d.bin`)
1. Log: `[stereo] stereo3d=1: GPU part 0:/ios3ds/gpu3d.bin instead of gpu.bin`.
2. Home, slider up: real depth (dock/status bar in front, wallpaper behind).
   This is the first build that programs PDC0 itself; if the top screen
   goes black/garbled, note it and slide down (PDC0 back to Luma's preset).
3. Swipe to Spotlight and back, open/close Utilities, open/close an app,
   double-Home switcher: no flicker of the barrier, no double image except
   possibly one frame during fast swipes; apps look flat.
4. `stereo_depth=50` then `=0`: half depth, then flat with the LED off.
5. Send `log.txt` with `[stereo] mirror GPU cost`, `[stereo] active path ...
   transitions held in 3D`, `[gl] CA stereo3D transitions: ...`.

### Not done
- One frame of eye mismatch mid-swipe (see Evidence) is not explained.
- In some apps the ARM9 probe still sees a stamped-looking alpha and leaves
  the barrier on (eyes identical or nearly); Game Center's "Loading" view
  keeps a small stamped region.
- The `stereo3d=1` soak failure above (SIGKILL of background apps) predates
  this round and is not traced.

## 153. 2.2.1 camera frames: QEMU wall-clock starvation, not the driver (2026-11-26)

Follow-up to 152's open item: `ios3ds_cam` START worked on 2.2.1 but no
FRAME arrived from either sensor in 15 s, while 3.1.3/4.x delivered at once.
The bare camera service (no Camera.app on 2.x) is the only goal.

### Evidence

- Fresh 2.2.1 image, `emu/hwtest_check.py --mode 2` (the wall-clock QEMU
  default of that time): **9 PASS / 0 FAIL / 9 SKIP**, cameras SKIP with
  "outer no frame in 15 s, inner no frame in 15 s". The log shows the whole
  driver path alive:
  `[cam] buffers: IOKit 3x150 KiB @24964000`, START, ARM9 I2C ids `2280` x3,
  `outer left camera streaming ... into CAM1` and `inner ... into CAM0`,
  sensor registers at 0.2/1/5 s, shim drains (`7911 services`, `649600
  words`), yet `0 frames, 118 FIFO overruns`, `drain gap max 37.5 ms`,
  FIFO parked at the 1280-unit fill cap, `0 blank drops` and both dumps
  skipped. The mailbox handshake is intact (`want`/`ack_seq`/`state=RUNNING`);
  this is not a start, I2C, mailbox or frame-buffer problem.
- The same image with `--icount 2`: **10 PASS / 0 FAIL / 8 SKIP**, cameras
  **PASS** for both sensors; `[cam] ... 2 frames, 0 FIFO overruns`,
  `drain gap max 1.0 ms`, max FIFO fill 320-400 units, no restarts. Nothing
  in `boot/arm9/camera.c`, `boot/common/cam_proto.h` or the 2.x KPI flags
  (`virt_base`, `irq_cont`, `irq_spsr`, `vt_bias`, `kernel_ifnet`) changed
  between the two runs.
- A test with `CAM_STEP_TICKS` halved from 1047 to 500 on 2.2.1 (temporary,
  reverted) doubled the services and the drained words but still gave
  `0 frames`, `117 overruns`, `gap max 10.3 ms`: asking for more wake-ups
  cannot fix a guest that is too slow to service them. 4.2.1's own
  wall-clock run had `gap max 12.5 ms` and still delivered frames because its
  CoreAnimation/GPU path leaves the ARM11 far more idle time.

Cause: QEMU's default virtual clock follows the wall clock while the guest
runs several times slower than a 3DS. 2.2.1 composites in software (the ARM9
copies the screen), so its 1 ms camera drain misses the FIFO's ~5.5 ms window
and every frame dies in an overrun. This is a test-harness timing artifact;
section 5 of [camera.md](camera.md) already prescribes `--icount 2` (real
instruction timing, ~250 MHz) for the camera path.

### Fix

- `emu/hwtest_check.py`: modes 1/2 now boot with `-icount 2` unless `--icount`
  is given (`--icount ""` keeps the old wall-clock timing); mode 0, which has
  no camera check, is unchanged. New `icount_arg()` is covered by
  `tests/test_hwtest.py`.
- `tools/ios3ds-diag/hwtest.c`: `cameras` now **PASS** when both sensors
  return a frame on *every* version, including 2.x, where no Camera.app
  exists to consume them. On 2.x a failed frame path is still SKIP with the
  raw result, not FAIL (no app is missing a picture). The 2.x-only branch is
  compiled with `-DIOS2_HWTEST` in the HW2 Makefile recipe, so the 4.2.1 code
  path and binary are unchanged.
- Docs updated: camera.md §5, real-hardware.md §6, other-versions.md,
  multi-version.md §6.9.

### Verification

- `emu/hwtest_check.py --mode 2` on a fresh 2.2.1 image: **10 PASS, 0 FAIL,
  8 SKIP** (cameras PASS, both frames). Same command on 4.2.1: **13 PASS,
  0 FAIL, 5 SKIP**. Explicit wall-clock runs still reproduce the old
  behaviour on 2.2.1 (0 frames), which is the documented reason for the
  default.
- Fresh-image soaks with `--icount 2`: 2.2.1
  `soak.py --cycles 5 --require-app --targets "275,226;199,226;123,226;47,226;275,50"`
  **PASS** (305 s, five apps opened, 0 failures); 4.2.1
  `soak.py --cycles 5` **PASS** (90 s, 0 failures).
- `make test` passes (six `tests/test_hwtest.py` cases, including the new
  camera-PASS and harness-timing checks). The 4.2.1 tool binary is
  byte-identical (`339f6290…`), and rebuilding 4.2.1 gives the same six
  hashes as before the change: kernel `692874e8…`, devtree `0965a3f2…`,
  fakeio `b89cfc32…`, shim `2f36ac60…`, gpu `5c3b584d…`, boot.cfg
  `ff838865…` (the branch's current baseline; the 152 values predate this
  branch's other merges).

## 156. iOS 4.1: the first HTTPS fetch was a cold trust daemon, not a late DNS (2026-11-26)

Continues 152 (hwtest kit) and 143 (Safari's cold first page). The hwtest kit
reported the first 4.1 fetch after joining Wi-Fi as `NSCocoaErrorDomain 256`
with no `NSUnderlyingError`; the same call 15 s later fetched 577 bytes from
`https://example.com/`. This round finds which part of the stack was actually
late and removes the retry.

### What the first fetch saw

- **[V]** Baseline, fresh `make sdimg IOS=4.1` image, `emu/hwtest_check.py
  --mode 2 --sd build/sd41.img --icount 2` (`build/emu-hwtest-base41/log-mode2.txt`):
  `[wifid] en0 configured by configd`, `[hwtest] wifi.join PASS ... after 14 s`,
  then `[hwtest] wifi.https: NSCocoaErrorDomain 256, retrying in 15 s`; the
  second try passed with 577 bytes. The only lines between the two tries that
  name a newly started service are `AMFI: Invalid signature but permitting
  execution` and `[hwaes] securityd: software IOAESAccelerator` /
  `software AppleKeyStore (key wrap)`: securityd, iOS's on-demand TLS trust
  daemon (`MachServices: com.apple.securityd`, `OnDemand` in its launchd
  plist), was started by that first certificate check.
- **[V]** Instrumented run on the same image (`build/emu-hwtest-diag41/`),
  checking state at the exact moment the old code did its first fetch:
  routing table had a default route via `10.0.2.2`, `getaddrinfo("example.com")`
  resolved and `SCNetworkReachabilityCreateWithName/GetFlags` returned
  `flags 0x2` (Reachable, no ConnectionRequired). So the DHCP address,
  configd's IPMonitor route, mDNSResponder's DNS server and SystemConfiguration
  reachability were **already ready**; merely preflighting them made the first
  fetch succeed in that run. The clock is also fine: the same boot's
  `[mcud] clocks` line reports RTC and `gettimeofday` within 1 s over ~70 s.
- **[G]** CFNetwork's exact wrapper path is not disassembled. The evidence
  points at the cold `securityd`/trust startup (and the few seconds of
  preflight work), not at a missing network service or a bad TLSFix load:
  the hwtest tool has no `ios3ds-applog`/Substrate injection, so TLSFix is not
  in that process at all on either try.

### Fix

- **[V]** `tools/ios3ds-diag/hwtest.c`: after the DHCP address, wait (up to
  30 s) for the IPv4 default route (`sysctl NET_RT_DUMP`, same check as
  `ios3ds-nettest`), a successful `getaddrinfo` for the URL's host, and the
  `bootstrap_look_up(bootstrap_port, "com.apple.securityd", ...)` port, then
  make **one** `NSData dataWithContentsOfURL:options:error:` fetch. The old
  three-try/15 s retry loop and its "retrying" log line are gone. The wait and
  reachability flags are logged for `diag=1` console logs
  (`[hwtest] net ready ...`, `[hwtest] reachability ... flags 0x2`).
- **[V]** `rootfs/overlay-4.1.json` (4.1 only): the generated rootfs sets
  `OnDemand=false` and `RunAtLoad=true` in
  `/System/Library/LaunchDaemons/com.apple.securityd.plist`, so the trust
  daemon is up before the user can join Wi-Fi or tap Safari. The base 4.2.1
  recipe and its plist stay untouched; the hwaes interposer in the same plist
  survives the overlay merge (`tests/test_versions.py`).
- **[V]** `emu/safari_test.py` no longer calls an external page "loaded after
  120 s" unconditionally: it waits the full `--page-timeout` (a cold HTTPS
  page can sit in Loading much longer on the ARM11). The screenshot still has
  to show the page; `tests/test_safari_harness.py` checks the timeout path.

### Verification

- **[V]** 4.1, fresh image with the hwtest fix, `hwtest_check.py --mode 2
  --icount 2`: **13 PASS / 0 FAIL / 5 SKIP**, no retry line, `wifi.https PASS
  577 bytes`. Its log reads `[hwtest] net ready after 0 s: route 10.0.2.2, dns
  ok, securityd up` and `reachability example.com: ok flags 0x2`; the securityd
  hwaes lines arrive during that single fetch. A second fresh image with the
  boot-time securityd overlay (`build/emu-hwtest-final2-41/`) gave the same
  **13 PASS / 0 FAIL / 5 SKIP**, `net ready after 1 s`, 577 bytes.
- **[V]** Safari, 4.1 `--settle 0` (URL opened as soon as DHCP and HID are up),
  `https://example.com/`:
  - before: harness said "loaded in 123 s" but `build/emu-safari-base41/p1.png`
    showed Safari still in **Loading**, with no page text; securityd started
    only near the end of the run.
  - after: `safari_test.py --urls https://example.com/ --settle 0 --page-timeout
    420` printed **1/1 pages loaded without an alert in 772 s**; the harness's
    `p1.png` shows the rendered "Example Domain" page with the HTTPS lock icon
    and its text, no alert/load error. The live screenshot at t=387 s already
    showed the complete page (reload icon, not "Loading"), so the
    extra wait is the harness's conservative timeout, not the page load time.
- **[V]** `safari_test.py --pages 3 --settle 60 --icount 2` on the same 4.1
  image: **3/3 pages loaded without an alert** (page 1 in 205 s, pages 2/3 in
  10 s each), screenshots in `build/emu-safari-41-pages/`.
- **[V]** Fresh-image 4.1 soak, `soak.py --cycles 5 --icount 2 --require-app
  --crashlogs --targets "47,314;123,226;199,226;275,226;123,50"`: **Result:
  PASS** (boot to SpringBoard + input 110 s, five cycles, app diffs 41.0–108.7,
  home diff ≤ 1.4, 0 panics/stalls/md0 errors/crashes/jetsam, no tap that
  opened no app, no CrashReporter reports).
- **[V]** Fresh-image 4.2.1 soak, `soak.py --cycles 5 --icount 2`: **Result:
  PASS** (five cycles, 0 panics/stalls/md0 errors/crashes/jetsam, no tap that
  opened no app). The one SpringBoard restart in each soak report is the
  boot-time HID connect, not a mid-run restart.
- **[V]** 4.2.1 default-build hashes (fresh `make sdimg`): kernel `505f451a…`,
  devtree `25b1bd23…`, fakeio `e5fecd66…`, shim `a35631eb…`, gpu `6bdaf617…`,
  boot.cfg `8d4079cf…` — unchanged. `make test` passes.

### Needs hardware [G]

- Boot 4.1 with `diag=1`, join Wi-Fi and immediately load a small HTTPS page in
  Safari and in another app. The log must show `[hwtest] net ready ...` with
  `securityd up` and no `NSCocoaErrorDomain 256` first-fetch line. None of
  this version has a console run yet; the QEMU securityd start-up is slower
  than a real 3DS under TCG, so a console should only be easier.
## 157. Battery life: opt-in idle telemetry, block gating and a New 3DS idle clock (2026-11-26)

Task: battery life on the main 4.2.1 image. Measure/estimate idle and
screen-off draw from what the emulator and the MCU/battery drivers expose,
gate unused blocks, and lower the ARM11 clock when it is safe. The shipped
default stays byte-identical; every action is behind `boot.cfg`
`power_opt=1` / `idle_clock=1` plus the developer build flag `IOS3DS_IDLE=1`.

### What the sources expose

- **[V]** The MCU battery service is voltage/percent/flags only: `mcubox_t`
  (regs 0x0A–0x0F) is refreshed by `mcu_poll()` (2 s) and registered as an
  `IOPMPowerSource` by the shim (re-log 132). QEMU's MCU holds 4200 mV and
  never discharges, so **no mA can be read from the emulator**. The real
  signals it does expose are duty counters: `[power]` lines and ARM9
  loops/sleeps in `powersave.c`, shim stats `+0x4C/+0x78/+0x8C/+0x98`,
  `gmbox_t.frames`, `mcubox_t.als_state`, `aud_stat_t.state` and
  `netmbox_t.link`. `boot/common/power_model.h` multiplies those counters by
  round-number mA coefficients (30 mA board, 23/37/50 mA ARM11 at
  268/536/804 MHz, 30+120 mA LCD/backlight, 25 mA GPU/camera, 10 mA CSND,
  70/30/15 mA Wi-Fi linked-PS/linked-max/idle) and divides an assumed
  1300/1750 mAh. Host-tested by `tests/test_power_model.py`.
- **[V]** Old 3DS opt-in image (`IOS3DS_IDLE=1 make sdimg CYDIA=0`),
  `emu/soak.py --cycles 5 --icount 2 --require-app --crashlogs`: **Result:
  PASS** (five apps, 0 restarts/panics/crashes, lock round on the home
  screen). `[pwr] screen on: ~181–182 mA modeled (7.1 h / 9.6 h assumed)`;
  `[pwr] GPU clock gated while the screen is off (PDN_GPU_CNT 0001007f -> 0)`
  then `GPU clock restored before the display mirror`. `idle_clock` is a
  logged no-op on an Old model.

### The New 3DS clock switch, and two bugs found in QEMU

- **[V]** New 3DS opt-in (`emu/run.py --new3ds --icount 2`, POWER at 150 s,
  HOME at 200 s): `[pwr] ARM11 clock 536 MHz (idle): PDN_LGR_SOCMODE 3,
  timer prescaler 255, tick shift 0`, `[pica] idle clock: timer prescaler
  191 -> 255, tick shift 1 -> 0`, the reverse on wake, and `[hb] clock`
  ratios 1000/1000, 1000/1000, 999/1000 while idle; a later 20 s lock round
  kept 1000/1000 across both switches. Screen on ~165–232 mA
  (5.6–7.9 h Old / 7.5–10.6 h New assumed), screen off ~122 mA on the first
  sample (CSND still in flight) and ~85–110 mA after the camera/CSND gates
  (10.5–15.2 h Old / 14.2–20.5 h New assumed). With the mirror paused and
  Wi-Fi in `REC_POWER` (a modelled assumption) the clock change alone is
  ~13 mA in the model (804 → 536 MHz).
- **[V]** First attempt used the wrong MPCore register offsets in the GPU
  helper (`PT_COUNT` 0x600 is PT_LOAD, `PT_COUNT` is 0x604): the private timer
  never got its new count, IRQs/s fell from ~300 to ~6 and the 10-cycle soak
  hung. Fixed by using the entry.S offsets. Second attempt left the watchdog
  rate at the old PERIPHCLK (QEMU only re-reads it when its control word is
  written): `[hb] clock` showed 1287/1000 and 1357/1000. The ARM9 now writes
  an `IDLE_REQ_REFRESH` bit after the PDN ack; the shim rewrites WD/PT control
  with the current prescaler and clears the bit, and the ratio is 1000/1000.
  A third bug (reading PDN_SOCMODE 0x10141300 from the shim, whose VA mapping
  does not cover it) caused `fault_addr=0x10141000`; the diagnostic print now
  omits that read. A fourth: the GL-wake hook wrote PDN_GPU_CNT through its
  **physical** address from the ARM11 (kernel abort `fault_addr=0x101411c0`
  in QEMU); `kernel/shim/pica_idle.c` now maps PDN with the same `ml_io_map`
  KPI as `pica.c` before restoring the clock. **[G]** all four are only proven
  in QEMU; the timer reprogramming is the main hardware risk.
- **[V]** Final hardening in the passing images: the ARM9 waits whole
  milliseconds for the shim/PDN handshakes, and accepts `GM_ST_WAIT` as well
  as `GM_ST_ACTIVE` before gating the GPU clock (`ps_update()` clears `want`
  first; the shim changes ACTIVE → WAIT on its next thread poll).
- **[V]** `make test` passes, including the new power-model and power_opt
  boot.cfg key checks. Six fresh default 4.2.1 hashes:
  kernel `505f451a…`, devtree `25b1bd23…`, fakeio `e5fecd66…`, shim
  `a35631eb…`, gpu `6bdaf617…`, boot.cfg `8d4079cf…` — all equal
  origin/main's. The opt-in image leaves the main shim at 24268/24320 bytes
  (the helper lives in gpu.bin, 27216/32768).

### Soaks and expected hours

- **[V]** Default (no keys) fresh image after rebasing onto origin/main 847a0c6:
  `emu/soak.py --cycles 5 --icount 2 --require-app --crashlogs` → **Result:
  PASS** (input after 150 s, five apps, 0 recoveries, 0 restarts/panics/crashes,
  lock round on the home screen). A final, fresh opt-in Old 3DS image with the
  hardened code: **PASS** — five apps, 0 recoveries, 0 restarts/panics/crashes,
  `[pwr] screen on ~181 mA` and one screen-off sample `112 mA` (7.1 h / 11.6 h
  at the assumed 1300 mAh). `idle_clock` is a one-line no-op on an Old model.
- **[V]** Opt-in New 3DS 10-cycle `--require-app --crashlogs --long-lock 20`
  soak on a fresh image: **Result: PASS** — 10/10 apps opened, 0 recoveries,
  Home always returned, the 20 s lock round ended on the home screen, 0
  restarts/panics/crashes. `[pwr] screen off: ~85 mA modeled (15.2 h / 20.5 h
  assumed)` after `[audio] idle: CSND_CNT -> 0`, both `ARM11 clock 536/804`
  switches and both `MPCore control rewritten` lines present; `[hb] clock`
  ratios 999/1000, 1000/1000, 999/1000, 1000/1000. A first run with a
  8 s settle had one tap that opened no app (cycle 7, no failure of any
  driver check); the repeat with the default 15 s settle passed cleanly.
- **[V]** `emu/lockburst.py --rounds 12 --no-console` (rapid POWER/shell
  taps) fails its strict `ARM9 CPU again`/end-brightness checks on the opt-in
  New 3DS image (2 transient copy pauses, dark end screen after a shell-tap
  race). The **same harness on a fresh default image also FAILS** (15 such
  pauses), so it is the harness's tap timing, not the idle policy: the GPU
  copy resumes after every pause and every POWER/shell event is seen. The
  `--require-app` soaks above include lock/wake rounds and pass.
- **[V]** Older-version soaks after the change, fresh images, `--cycles 5
  --icount 2 --require-app --crashlogs` with each version's targets: 4.1
  **PASS**, 3.1.3 **PASS**, 2.2.1 **PASS** (five icons opened each, 0
  recoveries, 0 restarts/panics/crashes). None of those builds gets the opt-in
  keys by default, so their ARM9 path is unchanged.
- **[G]** Expected hours are **model outputs, not measurements**: with the
  shipped default (no telemetry, `powersave=1`) the same counter mix gives
  ~7 h screen-on / ~11 h screen-off on a 1300 mAh Old battery; with the
  opt-in policy, ~7 h screen-on (Wi-Fi at max performance) and ~12–15 h
  screen-off Old / ~16–20 h New (the low end assumes Wi-Fi still linked in
  `REC_POWER`, the top end is the measured QEMU counter mix with no link).
  On the New model the log brackets the clock saving directly: `~123 mA`
  (10.5 h Old / 14.2 h New) at 804 MHz against `~85 mA` (15.2 h / 20.5 h)
  at 536 MHz, with all gates active. A real current meter is required before
  any of these is a claim.

### Needs hardware [G]

1. `IOS3DS_IDLE=1 make sdimg`, boot, open an app, POWER → lock, HOME → wake.
   `log.txt` must show `[pwr] idle policy on`, both `ARM11 clock ...` lines,
   `[pica] idle clock: MPCore control rewritten ...`, and `[hb] clock`
   ratios near 1000/1000 after each switch; the status-bar clock must keep
   moving and the app must still open after the wake.
2. Measure current with a meter in series with the battery: home screen at
   brightness 0.5 (GPU frames moving), 30 s locked with `power_opt=0`, then
   locked with `power_opt=1`; on a New model repeat the locked case with
   `idle_clock=0/1`, with Wi-Fi off / associated, and with Camera.app's ALS
   or Music playing before the lock. Compare each reading with the counters
   and mA estimate on the nearest `[pwr]` line and replace the model's
   coefficients and the assumed 1300/1750 mAh capacities with measured ones.
3. Check the gates do not break users: Camera.app after a lock/wake (ALS
   restore), Music during and after a lock (CSND guard), Wi-Fi reconnect with
   `REC_POWER`, GL drawing while the screen is off (the shim restores
   `PDN_GPU_CNT` on any GPU sysctl). If any fails, set `power_opt=0`.
   Not done: no console with a current meter was available in this worktree.

## 158. stereo3d=1 soak re-check: the round-4 SIGKILL failure does not reproduce on main (2026-11-27)

Goal: find why SpringBoard SIGKILLed apps after Home in the `stereo3d=1`
5-cycle soak (re-log 155) and make that soak pass.

- **[V]** main 5fa2055, no code change. Fresh default build: kernel
  505f451a, devtree 25b1bd23, fakeio e5fecd66, shim a35631eb, gpu 6bdaf617,
  boot.cfg 8d4079cf, gpu3d.bin 4aa214b9. `make test` green.
- **[V]** Flag-on images = the fresh image with `stereo3d=0` changed to
  `stereo3d=1` in `sd:/ios3ds/boot.cfg` (mtools), each one fresh. QEMU:
  the shared `/tmp/opencode/qemu-compat2/b/qemu-system-arm` (has the
  `mcu-3d-slider` property). Every run: `soak.py --cycles 5 --require-app
  --icount 2 --crashlogs`, plus `--slider3d 255` for the flag-on runs:

| run | config | host | boot | result |
|---|---|---|---|---|
| s1 | stereo3d=1, slider 255 | load 3-5 | 70 s | PASS, 0 signals, no CrashReporter reports |
| s2 | stereo3d=1, slider 255 | load 2.5-2.8 | 70 s | PASS, 0 signals |
| s3 | stereo3d=1, slider 255, QEMU + harness pinned to one core shared with two busy loops (about 1/3 speed) | load ~5 | 250 s | PASS, 0 signals (home late twice, recovered without input) |
| s0 | stereo3d=0 | load ~3 | 70 s | PASS |

- **[V]** The flag-on runs really used the two-eye path: `[stereo] switch:
  PDC0 3D, PICA200 two-eye mirror`, the barrier went on and off around the app
  launches, `mirror GPU cost: 3D 72 frames, avg 13021 us; 2D 31 frames, avg
  10224 us`.
- **[V]** Host speed doesn't explain it either: the soak runs `-icount
  shift=2,sleep=off` with `thread=single`, so guest timing (SpringBoard's
  suspend deadlines, ARM9 block service) is set by the instruction count,
  not by wall time. Host load only stretches the harness's wall-clock waits.
  s3 at about 1/3 speed still passed.
- **[G?]** So the cause was not in main's stereo3d=1 path. What differed in
  round 4: that agent's private QEMU build (`build/qemu-hy4`, built from
  its own branch's `emu/qemu`) and its worktree state during the controls.
  Not checked, because building QEMU is outside this task's rules. If the
  failure comes back, record the QEMU binary and its `git describe` in the
  soak report first.
- No fix was needed, so there is no code change; docs/stereo3d.md gets a
  short note. The flag stays experimental until a console test (re-log 155
  checklist).
## 159. Cydia 1.1.30 on 2.2.1: missing UIKit symbols, held by an epoch in dpkg status (2026-11-27)

Continues re-log 146, which left the freeze after the Telesphoreo "Essential
Upgrade" unexplained. Design/user notes: docs/cydia.md "iPhone OS 2.x",
docs/other-versions.md, docs/multi-version.md §6.9.

### Cause: two non-lazy UIKit symbols that 5H11a never exported

- **[V]** Unpacked `cydia_1.1.30_iphoneos-arm.deb` to
  `build/cydia-analysis/Cydia` (fat: armv6 slice at 0x4000, 566928 bytes).
  `/usr/bin/llvm-nm-18 --arch armv6 -u` lists 414 undefined symbols; a scratch
  Mach-O symbol-table reader over the 264 dylibs/frameworks extracted from a
  built 5H11a rootfs (195300 defined symbols) found exactly two without a
  definition: `_UIKeyboardAnimationCurveUserInfoKey` and
  `_UIKeyboardAnimationDurationUserInfoKey`. Every other import resolves, so
  this is not APT/dpkg, Substrate or a new UIKit class.
- **[V]** 5H11a's `UIKit.framework/UIKit` only has
  `UIKeyboardBoundsUserInfoKey`, `UIKeyboardCenterBeginUserInfoKey` and
  `UIKeyboardCenterEndUserInfoKey`: the animation userInfo keys arrived with a
  later UIKit; Cydia 1.1.30 was built against it (its armv6 slice still says
  `LC_VERSION_MIN_IPHONEOS 2.0`, which is why the file loads at all).
- **[V]** The two symbols are indirect entries 285/286. In that slice
  `__nl_symbol_ptr` covers indirect entries 264..316 (53 entries) and
  `__la_symbol_ptr` 317..580: both are **non-lazy**, so dyld binds them before
  `main()`. An upgrade to 1.1.30 therefore leaves a Cydia that cannot launch,
  not a Cydia that fails when the keyboard opens.
- **[V]** QEMU repro on a fresh 2.2.1 image with a temporarily added Telesphoreo
  source (`--icount 2`, ctr-wifi): the Essential Upgrade prompt appeared,
  accepted it; dpkg logged `Setting up cydia (1.1.30) ...`, then
  "Close Cydia (Restart)" and, after tapping the Cydia icon, no UI came back
  (home screen again: `build/emu-session/repro-launch2.png`; full sequence in
  `repro-prompt.png` … `repro-final.png`). **[G]** dyld's own "Symbol not
  found" line was not captured: the guest's `/var/log/syslog` was empty and no
  CrashReporter report was written, so the exact console wording remains
  unverified. The missing non-lazy bindings and the unlaunchable app are [V].
- **[V]** Re-log 146's alternatives were re-checked: `Status: hold` on cydia
  makes apt 0.7's resolver fail ("pkgProblemResolver::Resolve generated
  breaks"), and `/etc/apt/preferences` pinning 1.0.3222 or 1.1.30 alone still
  left "Cydia Installer" in Changes (the repository also offers 1.1.27 and
  Cydia 1.0 draws its upgrade list from the cache, not the pin).

### Fix: an APT epoch in dpkg's status plus a negative pin

- `rootfs/cydia-2.x.json` now has `"status_version": {"cydia":
  "1:1.0.3222-73"}`. `scripts/cydia.py status_entry()` replaces only the
  `Version:` line of that package's generated dpkg status entry: the pinned
  deb's files, md5sums and maintainer scripts are unchanged. APT compares
  `1:1.0.3222-73` (epoch 1) as newer than the repository's `0:1.1.30`, so
  Cydia 1.0 no longer classifies the installer as upgradable.
- `/etc/apt/preferences` pins every version of `cydia` and `cydia-lproj` to
  `Pin-Priority: -1`, a second barrier for direct APT installs. Neither
  mechanism changes 4.x: `status_version` is only in the 2.x manifest, and
  `status_entry()` without an override emits exactly the old status text.
- **[V]** With a Telesphoreo source written into a test image, the fixed
  build opened Cydia straight into the Home tab with **no Essential Upgrade
  prompt** (`build/emu-session/epoch-cydia.png`: only saurik's "no longer
  supported" banner) and Changes listed only "Cydia Translations", not the
  installer (`build/emu-session/epoch-changes.png`). Those two test-only
  source lines were removed again before the final image.

### Verification

- **[V]** `make test` passes, including the new
  `test_status_version` and the extended `test_ios2_set` (epoch, -1 pins, no
  default source).
- **[V]** Fresh 2.2.1 image (`rm -f build/sd.img && make sdimg IOS=2.2.1`),
  `emu/soak.py --cycles 5 --icount 2 --require-app --crashlogs --settle 30
  --targets "123,314;275,226;199,226;123,226;47,226"` → **Result: PASS**
  (five apps opened, no Home recovery, no crash reports). Cycle 0's Cydia
  screenshot is `build/cydia130-proof/cydia-open-221.png` (tab bar + Cydia's
  Home tab, offline "Unable to Load"); the source-added prompt/changes and
  freeze-repro shots are in the same folder.
- **[V]** Six 4.2.1 hashes after a fresh default build: kernel `505f451a…`,
  devtree `25b1bd23…`, fakeio `e5fecd66…`, shim `a35631eb…`, gpu `6bdaf617…`,
  boot.cfg `8d4079cf…` — identical to origin/main.
- **[V]** Fresh 4.2.1 image, `emu/soak.py --cycles 5 --icount 2` → **Result:
  PASS** (0 panics, stalls, crashes or GPU watchdog failures).

### Hardware checklist

- [G] Install a 2.2.1 build, add a Telesphoreo source in Manage > Sources,
  refresh: no Essential Upgrade sheet and no "Cydia Installer" in Changes.
- [G] Manage > Packages shows Cydia Installer `1:1.0.3222-73` and Cydia still
  opens after a reboot. Cydia Translations may upgrade on its own; check that
  Cydia still opens afterwards.
- [G] If someone upgrades Cydia anyway (a manually fetched deb and `dpkg -i`),
  attach `log.txt` and a CrashReporter report: the failure should be dyld's
  two UIKit userInfo keys on 5H11a.


## 160. Issue 1, third report: "Unable to join, but it takes much longer" (2026-11-27)

Log 2026-11-25_23-24-11 (build m1-0.4.0-244-geb90379-20261125.2034, New 2DS XL).

- **[V]** The build predates the home-AP RSN IE fix: eb90379 (2026-11-25 21:34) is
  an ancestor of de015bd ("keep SSID/rates/DS/RSN/WPA IEs when a beacon's IEs
  overflow the scan record", 2026-11-26 06:25), so c24a40d/de015bd are in main
  only (`git describe`: eb90379 is 0.4.0-244, c24a40d is 0.4.0-289).
- **[V]** The log shows the same home-AP bug: probe responses of 407/418 IE
  bytes with RSN, then `[a80211] associate: key type 2 / 13 bytes not supported
  (WEP?)`. The tester then tried a phone hotspot (109-byte IEs, plain RSN).
- **[V]** Hotspot, 3 joins. Joins 1 and 2: association OK, message 1
  (`key info 008a, replay 1`), our message 2 (121 bytes), AP disconnects (reason
  4, code 2), 3 times -> `handshake failed 3 times: wrong password?`, `giving up`
  at +0.6 s. Join 3 (new PMK from WiFiManager): message 1/2 then `EAPOL event 2,
  reply 99 bytes`, `link up`, `en0 configured by configd`. So the 4-way
  handshake code and the message 2 RSN IE are fine; the first two PMKs did not
  match the AP.
- **[V]** The bug found: after giving up the driver set `T.have=0`, state IDLE
  but sent no WMI_DISCONNECT, so the firmware kept auto-reconnecting: 78
  associations, 172 message 1/2 rounds and 94 `disconnected ... idle` over ~90 s
  until the next join. Fix: `ar6k.c` sends WMI_DISCONNECT on give-up. [V] host
  test `test_giveup_disconnect` (fails on the old code). docs/wifi.md 10.10.
- **[V]** 4.2.1 hashes unchanged (kernel 505f451a, devtree 25b1bd23, fakeio
  e5fecd66, shim a35631eb, gpu 6bdaf617; ARM9 only), `make test` green, fresh
  4.2.1 soak (5 cycles, icount 2, fresh image) PASS.
- **[G]** Not provable from this log: whether the home AP now joins. The
  expected lines are `[wifi] connect ... (WPA2-PSK/CCMP)` and no `key type 2`;
  if it fails with a message 1/2 loop, we need the `[wifi-diag] eapol` lines of
  that build (a retype of the password rules out a wrong PMK).
## NEXT. Camera frame dumps in their own sd:/ios3ds/photos/ folder (2026-11-27)

Issue #1: the ARM9 wrote each camera session's diagnostic
`CAM<seq>.RAW` straight into `sd:/ios3ds/`. It now writes
`sd:/ios3ds/photos/CAM<seq>.RAW`: the same 8.3 name (`CAM%08u.RAW`, eight
digits; a sequence past 99,999,999 wraps so the name stays FAT-safe), with
`boot/common/cam_dump.h` shared by the ARM9 writer and the host test.
`camera_init()` creates `photos/` at boot (also when `camera=0`) and renames
old `CAM########.RAW` files found in `sd:/ios3ds/` into it - no copy and never
overwriting a same-named file; case-insensitive spellings are accepted, at
most 64 per boot (the rest migrate on the next boot). The dump writer creates
the folder again in case it was deleted after boot. `tools/ios3ds-camera/analyze_dump.py`,
the camera docs, the driver matrix and the README FAQ now point at the new
path.

### Verification

- **[V]** `make test` passes (132 tests), including the new
  `CameraDumpFolder.test_dump_path_and_legacy_name`: a host C harness built
  from `cam_dump.h` checks the exact path, 8.3 length, sequence wrap, small
  buffer, recognition of legacy names and rejection of lookalikes, plus
  source checks that the ARM9 writer uses the helper, creates the folder and
  renames old dumps.
- **[V]** Fresh default build hashes: kernel `505f451a…`, devtree
  `25b1bd23…`, fakeio `e5fecd66…`, shim `a35631eb…`, gpu `6bdaf617…`,
  boot.cfg `8d4079cf…` - all identical to origin/main; shim, kernel,
  devtree, fakeio and gpu are untouched. With a pinned build version
  (`VERSION=camfolder-hashcheck`, to remove the timestamp from the version
  string), the **ARM9 boot image** changed as expected from
  `arm9.bin` `05a272c7…` to `11d9a808…` and `ios3ds.firm` `04bb1b68…` to
  `5f69b72a…`, because `boot/arm9/camera.c` now owns the folder and migration;
  no other 4.2.1 hash changed.
- **[V]** Fresh 4.2.1 image (`rm -f build/sd.img && make sdimg`),
  `IOS3DS_QEMU=/tmp/opencode/qemu-compat2/b/qemu-system-arm python3 emu/soak.py
  --cycles 5 --icount 2` -> **Result: PASS** (0 panics, stalls, crashes, GPU
  watchdog failures or camera overruns).
- **[V]** Boot-time migration exercised in QEMU: injected a legacy
  `sd:/ios3ds/CAM00000042.RAW` into the image, booted `--icount 2`; log
  `[cam] photos: sd:/ios3ds/photos/ ready, moved 1 old dump(s), kept 0,
  failed 0` and `mdir` showed `CAM00000042.RAW` inside `photos/`, not the root.
  The dump *write* itself is covered by the host path test; the soak does not
  open Camera.app.

### Hardware checklist

- [G] With a card from an older build (root `sd:/ios3ds/CAM*.RAW` present),
  boot and check `log.txt` for `[cam] photos: ... moved N old dump(s), kept N,
  failed N`; the named files must now be in `sd:/ios3ds/photos/` and gone from
  the root. Copy the log into an issue if `failed` is nonzero.
- [G] Open Camera.app, take a photo and close it; `[cam] dumped frames seq ...
  to 0:/ios3ds/photos/CAM....RAW` should name the new location and
  `analyze_dump.py` on that file should still decode both frames.

