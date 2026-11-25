# Experimental stereoscopic 3D home screen

Status: **hardware path implemented, not yet confirmed on a console**. Off by
default (`stereo3d=0`). Enabled with `stereo3d=1` in `sd:/ios3ds/boot.cfg`;
the 3D slider must be above the bottom notch (MCU register 0x08 >= 16).
The bottom screen and the touch map are unaffected. The GPU display mirror
stays on: with the slider down (or a flat frame) the PICA200 copies the same
one-eye picture as with the flag off, and with the slider up its two-eye
command lists build the eyes. The ARM9 two-eye copy is only the fallback if
the mirror stops (docs/re-log.md 109).

## 1. Idea

The 3DS top LCD has a parallax barrier: with the barrier on it shows two
images, one per eye, and the viewer sees depth. iOS's home screen is drawn by
SpringBoard through CoreAnimation, which composites every layer with our GL
engine (`tools/ios3ds-gl`, docs/gpu.md). That engine stamps the depth class
of the topmost layer into the frame's alpha byte while it composites:

- wallpaper furthest back,
- the icon grid at the convergence plane,
- dock and status bar in front of it,
- folders and alerts in front of those.

The GPU mirror's two-eye command list reads that class map when it samples
the frame: one baseline pass and three alpha-tested class passes per eye
shift the colour sample by its class offset. The 3D slider scales the shift
(1..4 pixels); slider down or a frame that is not the home screen gives both
eyes the same image and the barrier is off. The alpha byte is masked away
before the pixels reach the LCD: the screen always gets opaque BGR8. An ARM9
copy with the same class/delta rules takes over when the mirror cannot run.

## 2. Why the alpha channel

The composed frame is 32 bpp BGRA in window 1 (`/vram`, 0x18000000). Core
Animation's own colours, blending and coverage are left untouched in pass 1.
Pass 2 writes only the alpha channel of the same pixels with the layer's
depth class (a second, alpha-only pass over the same geometry, colour mask A
only, blending off, no texture). The ARM9 screen copy reads that byte back.

The alpha values are chosen (0x00/0x33/0x66/0x99/0xCC) well away from the
premultiplied alphas a software compositor would leave behind (0x00
transparent, 0xFF opaque), so a frame without our stamps never looks like a
class map: `stereo_alphas_stereo()` rejects it. The ARM9 samples a 32x32 grid
of the frame's alpha bytes on every copy; one unstamped byte is enough to
fall back to the normal, flat copy.

## 3. Depth classes and offsets

Shared by the GL engine, the ARM9 copy, and the host tests in
`boot/common/stereo.h`:

- `STC_FLAT` (0): both eyes identical,
- `STC_BG` (1): wallpaper, furthest back,
- `STC_ICONS` (2): icon grid, convergence plane,
- `STC_FRONT` (3): dock, status bar,
- `STC_ALERT` (4): folders, alerts, in front.

`stereo_eye_delta(cls, k, right)` gives the horizontal sampling offset in iOS
pixels for one eye: `out(x) = in(x - delta)`. Class 2 stays put; class 1
moves `-k` (left) / `+k` (right); class 3 moves `+k` / `-k`; class 4 moves
`+2k` / `-2k`. `k = stereo_shift_k(slider)` is 1..4 for slider 16..255 and 0
below that. The frame is rotated on the LCD, so this horizontal iOS offset
becomes a vertical parallax shift on the 3DS panel.

## 4. GL engine stamping (`tools/ios3ds-gl/stereo.c`)

Only the CoreAnimation render server (`IOS3DS_CA=1`) ever stamps, and only
when the boot-arg `ios3ds_stereo=1` is present; otherwise the flag is ignored
and `gltest`/apps are unaffected. Each draw into the display target is
classified by `stereo_draw()`:

- the first full-screen draw is the background, even when its texture has
  translucent texels (SpringBoard's wallpaper does); when the wallpaper is
  drawn as many small layers instead, the first few small draws establish it;
- a later full-screen opaque draw covers everything: the frame is flat (apps,
  the lock screen), nothing is stamped and the frame is wiped;
- a later full-screen translucent draw is a backdrop: it keeps the classes
  below it, and content drawn after it (a folder or alert) becomes `STC_ALERT`;
- everything else is classified by position: the status-bar band or the dock
  band => `STC_FRONT`, otherwise the icon grid => `STC_ICONS`.

A frame keeps its class map only if `stereo_frame_home()` holds: phase home or
alert, a background seen, and at least `STEREO_MIN_ICONS` (4) icon draws.
Frames that are not the home screen are wiped flat at the end of the frame
(one full-screen quad setting alpha to `SA_FLAT` everywhere), so the screen
copy sees a uniform frame and gives both eyes the same image.

Pass 1 is CoreAnimation's own draw, untouched. The stamp is **deferred and
batched** (re-log NEXT): `gl_be_triangles()` runs pass 1 and, when the draw
is classified, records its triangles in the target's window space with the
class as vertex alpha. At the end of the frame (`stereo_end_frame()`, from
`glFlush`/the CA swap) a home frame gets *one* alpha-only draw with every
stamp of the frame in submission order (a new segment only where CA changed
the scissor box, so a partial update stamps exactly what it drew); a frame
that is not the home screen gets only the wipe and its stamps are dropped.
Before, every classified CA draw was followed by its own alpha-only draw:
twice the GPU state changes and command-list words per frame, and app frames
paid for stamps that the wipe then erased. Draws with an alpha test are
still not stamped (their discarded fragments would be). A target freed in
mid-frame drops its pending stamps (`stereo_forget()` from `pica_forget()`).

The final CA frame goes straight from the render target into the display
IOSurface through the GPU (`PICA_OP_PRESENT`, docs/gpu.md §7), **with
`stereo3d=1` too**. The swizzle pass writes the target's alpha into the
surface's alpha byte (output R = sample alpha, and the GPU word R<<24 is the
surface's A), so the class map survives it; `gltest`'s
`alpha_stamp_check` proves it on the PICA200 model for the direct present
and the arena-copy (scattered surface) path, and the
`IOS3DS_GLTEST_STEREO=1` host run checks a SpringBoard-like frame end to end
(wallpaper 0x33, icons 0x66, dock/status bar 0x99, RGB untouched, an app
frame wiped to 0). Rounds 106-113 used the CPU read-back for the stereo
display target instead: a 600 KiB copy per CA frame on the ARM11
(re-log 78 measured 31-37 ms per frame for read-back against 13.5-16 ms for
the GPU present on hardware). That detour is gone.

## 5. Real hardware: the two-eye mirror and the display registers

**GPU mirror** (`tools/ios3ds-pica/mirror/genmirror.c`,
`tools/ios3ds-pica/src/pica_mirror.c`): `gpumirror.bin` v4 carries two extra
command lists (one per sampling variant). Each eye draws a baseline quad
(all pixels, no shift) and three more quads alpha-tested against the stamp
read through texture unit 1: BG (0x33), FRONT (0x99) and ALERT (0xCC).
Texture unit 0 carries the colour sample at the eye offset from §3; the
shim patches its texture matrix per pass from a precomputed table for the
slider's k, so no floating point runs in the kernel. The left eye renders
into `GM_RT_TOP_PA`, the right eye into the upload buffer (free after the
texture copy); both are transferred to the two LCD framebuffer sets. The
bottom screen keeps the ordinary flat quad. The unused texture columns
320..327 are filled from x=319 after every upload (`pad_edge()`), so a
shifted sample at the iOS edge clamps exactly like the ARM9 copy. The
alpha stamp is sampled but never written to the LCD.

**ARM9 fallback** (`boot/arm9/display.c`, `blit_top_stereo()`): when the GPU
mirror stops, one source row is read per screen column as in the normal fast
copy. For each output pixel the alpha byte at that source position picks the
depth class; the left eye samples `x - delta(cls,k,left)`, the right eye
`x + delta(cls,k,right)`, clamped to the source width. Both samples are
packed as BGR8 (low 24 bits only) into `top_fb[0]` (left eye) and `top_fb[1]`
(right eye). The bottom copy and the touch map are the ordinary
`blit_bottom()`, unchanged.

The two-eye copy caches one signature per screen column, taken from the
source row that feeds it (all 320 pixels, alpha stamp included). A column
whose row has not changed keeps the eye pixels already in the LCD buffers;
only changed columns are rebuilt, at most 120 per copy so a frame that
changed everywhere is finished over three copies instead of spending one
very long turn on the ARM9. The class lookup is a 256-byte table, the eye
offsets are looked up per class instead of computed per pixel, and the
source row is copied with eight pixels of padding on each side so sampling
never needs a clamp. The cache is dropped when the slider, screen mode,
framebuffer set or `stereo3d` flag changes. `stereo3d=0` never enters this
code; its copy is still the original byte-for-byte path.

**PDC0 2D/3D switch** (`kernel/shim/pica_stereo.c`, ARM11 GPU part): the PDC
registers are ARM11-only, so the ARM9 publishes the requested mode in the
display mailbox (`gmbox_t.stereo_on/stereo_seq`, `boot/common/gpu_mirror.h`)
and the shim applies the preset on the next VBlank. Per 3dbrew
[GPU/External Registers](https://www.3dbrew.org/wiki/GPU/External_Registers):

- PDC0 (top screen) is at PA `0x10400400`;
- `+0x68/+0x6C` = framebuffer A (left eye) set 0/1, `+0x94/+0x98` =
  framebuffer B (right eye) set 0/1: both pairs are pointed at the two LCD
  framebuffer sets the ARM9 just wrote;
- `+0x70` bits 5-4 = 2 selects AB interlace (one scanline from A, the next
  from B) and bit 6 must be clear: this is the 3D mode GSP uses. The shim
  saves Luma's 2D preset on the first switch and restores it on the way back
  (bit 6 set, no interlace).

**Parallax barrier** (`boot/arm9/display.c`, ARM9 side). Per 3dbrew
[LCD Registers](https://www.3dbrew.org/wiki/LCD_Registers):

- parallax barrier enable at PA `0x10202000`: `0x00010001` on, `0` off;
- parallax barrier PWM at PA `0x10202004`: `0x0A390A39` before enabling
  (the value libn3ds' `LCD_setParallaxBarrier` writes);
- the 3D LED is MCU register `0x2C`: the ARM9 writes 1/0 with the barrier,
  because the MCU firmware only lights it when Horizon enables the barrier
  through GSP (we write the registers directly).

The ARM9 enables the barrier only **after** the shim acknowledged the PDC
switch and one GPU frame was built in 3D mode (`gmbox_t.st_frames`); on the
CPU fallback it waits for the two-eye copy instead. Any later flat frame
(app, slider down, boot flag off) switches PDC0 back to 2D and clears the
barrier register again. No CONFIG11/SPI write is needed: the barrier itself
is the register above (3dbrew also notes that Horizon's GSP turns it on when
it programs AB interlace; we are baremetal, so we write it directly).

**`stereo3d=0`** never calls any of this code: `display_blit()` executes the
same two copy calls as before, writing the same bytes to the same
framebuffers. `tools/ios3ds-pica/tests/test_stereo_copy.c` and
`test_mirror.c` check that byte-for-byte (including the flag-off case and
the slider-down fallback).

## 6. Cost

With the flag off the normal path is unchanged: `stereo_prepare()` returns -1
immediately and no stamp is recorded. With the flag on:

- **CA render thread (ARM11):** one extra alpha-only draw per home frame
  (the batch) or per app frame (the wipe); the display target is written
  by the GPU present exactly as with the flag off. No CPU read-back.
  `[gl] CA stereo3D: N frames, N home (stamped), N flat (wiped), N stamped
  draws in N batched alpha draws (N vertices), N wipes; display written back
  by the GPU present` every report period, next to the usual
  `[gl] CA: ... per frame ...` line (its `CPU read-backs` column must stay
  at ~0.1 with stereo on now; before it was ~1 per frame).
- **GPU mirror:** slider down or a flat frame use the flat list (the same
  cost as `stereo3d=0`). A stamped home frame with the slider up uses the
  two-eye list: per eye a baseline quad and three alpha-tested class quads
  (BG, FRONT, ALERT), plus one clear of the right eye's target - about five
  times the flat list's draw stage. Damage tracking (re-log 113) means this
  is paid only for frames CA actually changed: an idle home screen in 3D
  costs no GPU time at all. Every 30 s the ARM9 logs
  `[stereo] mirror GPU cost: 3D N frames, avg X us (draw Y us); 2D N frames,
  avg X us (draw Y us) in N s` from per-mode counters the shim keeps in the
  mailbox (`st_n/st_sum/st_sum_3d`, `fl_*`), so a hardware log shows the
  real two-eye cost next to the flat one.
- **ARM9:** nothing per frame while the mirror runs (`[stereo] active path:
  PICA200 two-eye mirror`). If the mirror falls back, the ARM9 two-eye copy
  is cached per screen column (§5): only rows that changed are rebuilt, at
  most 120 columns per copy, and every 30 s the log adds
  `[monitor] stereo eye columns: N rebuilt, N reused`.

Measured numbers are in docs/re-log.md, sections 90, 109 and NEXT.

## 7. QEMU

QEMU models the hardware registers, not the old shortcut: the anaglyph comes
from PDC0's framebuffer A (left) and B (right), the format bits at `+0x70`
and the barrier register at `0x10202000` (plus QOM properties `pdc-3d-mode`
and `lcd-3d-barrier`, readable with `qom-get /machine ...`). The arm9 gets an
alias of the LCD register page so its barrier writes reach the model. With
the barrier off or PDC0 in 2D mode the ordinary top-screen copy is shown.

The QEMU binary in `build/qemu` is shared between worktrees and predates the
`mcu-3d-slider` QOM property; this branch built a private one under `build/`
(`QEMU_DIR=$PWD/build/qemu-hy4 emu/build-qemu.sh`; the configure script needs
a one-line quoting fix for the HDD path with spaces) and selects it with
`IOS3DS_QEMU=$PWD/build/qemu-hy4/b/qemu-system-arm`:

```
# flag on + slider up: anaglyph home screen; tap an app: no offset
IOS3DS_QEMU=$PWD/build/qemu-hy4/b/qemu-system-arm \
  python3 emu/run.py --time 240 --shots 239 --slider3d 255
# slider down / up again at runtime: clean hand-over both ways
... --hmp '180=qom-set /machine mcu-3d-slider 255;225=qom-set /machine mcu-3d-slider 0;255=qom-set /machine mcu-3d-slider 255'
# flag off: ordinary screen
```

`emu/soak.py --slider3d 255` sets the slider for flag-on soaks (it re-applies
the QOM property after boot, because QEMU's machine reset can clear an early
write). The bottom screen and the touch mapping are unchanged.

## 8. What still needs a console

Everything above is implemented and exercised in QEMU, but **the 3D effect
has not been seen on a real 3DS**. To test:

1. Add `stereo3d=1` to `sd:/ios3ds/boot.cfg` (and keep `diag=1` if you want
   the per-frame classifier lines). Boot as usual.
2. On the home screen, push the 3D slider up and take a **photo of the top
   screen from straight in front**, with the slider at maximum and at the
   bottom notch (control photo). Compare: wallpaper/dock should separate in
   depth in the first, the second should look like 2D. Open an app with the
   slider still up and take a third photo: it must look flat.
3. Capture `sd:/ios3ds/log.txt` (and `log_prev.txt` if a previous run is
   relevant). The lines to look for are
   `[stereo] stereo3d=1: GPU display mirror stays on ...`,
   `[stereo] MCU 3D slider 8 -> N (raw reg 0x08, 3D ON at >= 16)` for every
   slider movement, `[stereo] switch: ... PICA200 two-eye mirror ...` on each
   mode change, `[stereo] 3D LED on/off`,
   `[monitor] stereo 3D: both eyes ... parallax barrier on (slider ..., shift ... px)`
   and, after leaving the home screen or sliding down,
   `[monitor] stereo 3D: top screen back to 2D, parallax barrier off`.
   Every 30 s `[stereo] active path: ...` reports the current path and how
   many 3D/2D GPU frames were built. An ARM9 fallback says `ARM9 two-eye
   copy`; any `[monitor] stereo 3D: no shim ...` means the GPU part was not
   loaded (check `gpu=1`): the copy stays in 2D rather than showing a broken
   image.
4. Report the console model (Old/New, 2DS?) and whether the top LCD really
   shows depth. A 2DS has no barrier, so a flat picture there is expected.

Until that photo and log arrive, treat this as experimental; the README row
stays 🧪.

## Soak status at merge (2026-11-24)

- `stereo3d=0` (default): fresh-image 5-cycle `--icount 2` soak PASS (boot
  161 s), `14.7 ms` per ARM9 copy in the 30-second monitor line.
- `stereo3d=1`, slider 255, private QEMU build: fresh-image 5-cycle
  `--icount 2` soak PASS (boot 90 s). Both eyes and the barrier come up
  (`[monitor] stereo 3D: both eyes ..., parallax barrier on`), the flag-off
  copy stays byte-for-byte the same, and there are no panic/stall/md0/crash
  failures. The two-eye ARM9 copy takes 8.2-13.0 ms per blit in QEMU (before:
  120-186 ms), because only changed source rows are rebuilt. Some cycles
  still need a repeated Home or a Spotlight dismissal; that is the known iOS 4
  home/Spotlight race in the harness, not a display stall (the harness now
  retries the dismissal and clears a typed query before giving up). On a
  console the ARM9 runs in parallel with iOS, so QEMU is not a hardware
  prediction. Keep the flag experimental until a console test (section 8) says
  how it feels.

### GPU-built eyes, fresh soaks (2026-11-24)

The rework above moves the two-eye work into the mirror lists. All three
configurations then got a **fresh** `build/sd.img`, a private-QEMU
`emu/soak.py --cycles 5 --icount 2` run and `Result: PASS`, with 0
panic/stall/md0/crash/GPU-timeout findings; flag-on images had `stereo3d=1`
in `boot.cfg`, slider 255 was set with `--slider3d 255`:

| boot.cfg / slider | active path | GPU frame avg (30 s line) | CPU copy before mirror |
|---|---|---|---|
| `stereo3d=0` | flat mirror | 306 frames/30 s, 13.2 ms | 47 copies, 14.3 ms each |
| `stereo3d=1`, 0 | flat mirror (`k=0`) | 327 frames/30 s, 13.7 ms | 43 copies, 14.3 ms each |
| `stereo3d=1`, 255 | PICA200 two-eye mirror (`k=4`) | 312 frames/30 s, 14.3 ms | 48 copies, 14.8 ms each |

Slider-down cost is now the same as flag-off (both use the same flat list),
and slider-up costs about 1 ms per frame more in the instruction-clocked
emulator, instead of the old 120-186 ms ARM9 two-eye copy (§106). A runtime
toggle run (`--hmp` slider 0→255→0→255) logged a PDC0 3D switch, barrier on,
then PDC0 2D and barrier off, then 3D again; flat screenshots had single
icons and 3D ones the two shifted eye images. A flat app frame with the
slider up leaves the barrier off and the flat GPU mirror running; the
soak's 3D run logged 30 such on/off pairs across app launches. These are
emulator timings, not hardware predictions; section 8 still applies.
