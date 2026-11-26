# Running ios3ds on a real Old 3DS (without risking the existing setup)

**Goal:** test on your own console with boot9strap and Luma3DS, while leaving everything that is already installed (Luma, boot9strap, homebrew, NAND) untouched and bootable.

## 1. Why this can't brick the console

| Risk | Status | How it is ensured / checked |
|---|---|---|
| Writing NAND (eMMC) | **never** | `boot/arm9/sdmmc.c` has no eMMC code at all (removed when imported); the only device ever selected is `devicenumber = 0` (SD, `REG_SDPORTSEL` port 0). XNU has no driver for the 3DS eMMC, and the TMIO controller at 0x10006000 is only reachable from the ARM9. |
| Modifying FIRM0/FIRM1, boot9strap, Luma | **never** | Nothing writes to NAND (see above). ios3ds is just a payload file on SD that Luma chainloads into RAM. |
| Persistent chip state (MCU, PMIC, CODEC) | none written | No I2C/MCU writes anywhere. The touch driver configures CODEC registers over SPI; those reset at power-off. |
| SD card damage | possible, limited | The ARM9 writes only `sd:/ios3ds/log.txt`, `log_prev.txt`, `logs/*.log` and `rootfs.hfs` (iOS's disk image), through FatFs. A power loss during a write can at worst corrupt that FAT volume, as with any homebrew. Mitigation: back up the SD card (or use a spare card). |
| Hang / black screen | recoverable | Hold POWER for ~3 s to start a clean shutdown (if iOS still responds), or ~10 s to force the console off. ios3ds runs entirely from RAM, so the next normal boot is your usual Luma + HOME Menu. A hard cut can leave the HFS+ root image unclean; see §3. |
| Battery / heat | low | Same as any long-running homebrew: no clock or voltage changes. |

There is no software path back into NAND. The worst realistic outcome is a corrupted SD card, which you restore from your backup.

## 2. Before the first run (one time)

1. **Back up the SD card:** copy all of it to your PC.
2. **Back up the NAND, if you don't have a backup yet.** If you followed the 3ds.hacks.guide "Finalizing Setup" page, you already have one in `sd:/gm9/out/` (`<date>_<serial>_sysnand_###.bin` plus `essential.exefs`). Otherwise use GodMode9's NAND backup option (`[S:] SYSNAND VIRTUAL` → `nand.bin` → A → *NAND image options* → *Backup SysNAND*). Copy the backup to your PC. ios3ds never needs them; they are your safety net for anything else.
3. **Check Luma's version:** Luma3DS 10+ with boot9strap. The chainloader is the menu you get by holding START during boot.
4. Build: `make sd`, then copy the **contents** of `build/sd/` to the SD root:
   - `luma/payloads/ios3ds.firm` (≈200 KB);
   - `ios3ds/` (kernel, device tree, shim, `boot.cfg`, `rootfs.hfs` ≈1 GB).

   Nothing existing is overwritten except an older `ios3ds/`.
5. Optional quick-launch: rename the payload to `x_ios3ds.firm`. Luma then boots it directly when **X** is held at power-on; all other boots are unchanged.

## 3. Test stages

Every stage writes to the screen and to `sd:/ios3ds/log.txt`. The previous run is kept as `log_prev.txt`, older runs as `logs/<date>_<time>.log` (newest 32). After each stage, power off and send `log.txt`.

| Stage | How | Expected | If it fails |
|---|---|---|---|
| 0: monitor only | Boot ios3ds, hold **B** | Bottom screen: version, "SD ok", "ARM11 ok"; the monitor stays in its menu | Screen/SD/ARM11 bring-up problem: the log says which step |
| 1: kernel start | Boot without B | XNU's first lines on the top screen ("Darwin Kernel Version 10.4.0…") | Look at the last log line: device tree, boot_args, MMU |
| 2: root disk | – | `BSD root: md0`, launchd, daemons in the log | Block server: TMIO timing on real SD |
| 3: SpringBoard | – | Home screen on the top screen (turn the console 90° clockwise) | `[smp]` thread dumps in the log (boot.cfg `diag=1`, [boot-perf.md](boot-perf.md) §3); SELECT+START for a panic dump |
| 4: input | START = Home, SELECT = lock, L/R = volume, touch on the bottom screen | Spotlight opens on Home; touches move or tap | Log lines `[input] …` and `tsc_ok` |

To get back: hold POWER ~3 s for a clean shutdown (iOS unmounts `rootfs.hfs`, then the MCU switches the console off), or ~10 s if iOS is stuck. Power on normally afterwards; the log's `[blk] rootfs.hfs:` line says whether the image was cleanly unmounted at the last boot.

## 4. Launching from the Homebrew Launcher (`.3dsx`)

**Why iOS can't run inside a `.3dsx`:** a `.3dsx` is a user-mode program under Nintendo's kernel. XNU must own the CPU (MMU, vectors, interrupts), and ios3ds needs the ARM9 as its I/O processor. The ARM9 runs Process9, in ARM9-only memory the ARM11 can't map, and the old ARM11→ARM9 exploits (firmlaunch-hax, safehax) are patched on current firmware.

**Why a homebrew app can't launch a `.firm` directly:** the userland firmlaunch (pm `LaunchFirm` → `svcKernelSetState(0, …)`) is hooked by Luma's `patchFirmlaunches`. The hook always reloads the file Luma itself booted from (`launchedPath`) and skips the chainloader menu (Luma3DS `arm9/source/patches.c`, `main.c` FIRMLAUNCH branch).

**What we do: `tools/ios3ds-launcher` → `sd:/3ds/ios3ds.3dsx`.** It is a launcher. It only renames files on the SD card, then reboots (once, before that, it copies the Wi-Fi firmware: if `ios3ds/nwm.code` is missing it reads NWM's `.code` read-only through Horizon's FS and writes that file; [wifi.md](wifi.md) §3.1):

1. create `ios3ds/swap.flg`;
2. rename `boot.firm` (Luma) → `ios3ds/luma.firm`;
3. rename `ios3ds/ios3ds.firm` → `boot.firm`;
4. reboot (`PTMSYSM_RebootAsync`, fallback `APT_HardwareResetAsync`).

boot9strap then boots ios3ds directly. Our FIRM header sets boot9strap's screen-init flag, so the screens come up as they do under Luma.

**Luma comes back first:** the monitor's **first action after mounting the SD card** is the reverse swap (`boot/arm9/swap.c`). The next power-on is Luma again.

**Why an interrupted swap still boots:**
- Renames only, no copies, so no half-written `boot.firm` can exist.
- Between the two renames there is no `sd:/boot.firm`, and boot9strap falls back to `ctrnand:/boot.firm`, which the 3ds.hacks.guide installs. If you skipped that step, the fix is copying `ios3ds/luma.firm` back to `boot.firm` on a PC.
- After the swap, ios3ds boots and restores Luma.
- If anything is left over, the launcher sees `ios3ds/luma.firm` on its next start and offers **Y: restore Luma**.

**To return to the HOME Menu:** hold POWER ~3 s for a clean shutdown, then power on; that boots Luma as usual. The ~10 s hardware cut remains as a last resort when iOS is unresponsive.

**Tested:**
- QEMU: the monitor restores a simulated swap (`launcher swap: restored, next boot is Luma`); iOS still boots with long-filename support enabled in FatFs.
- Azahar 2126.2: the `.3dsx` runs, swaps on A, and restores on Y. Azahar can't boot a FIRM, so its "reboot" just restarts the app.

**Not tested on real hardware yet.**

Building the launcher needs devkitARM + libctru: `source ../rockbox-3ds/scripts/env.sh && make -C tools/ios3ds-launcher`, then `make sd` adds `3ds/ios3ds.3dsx`.
