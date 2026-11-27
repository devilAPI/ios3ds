# PICA200: GPU-accelerated OpenGL ES 1.1

`tools/ios3ds-gl` (see [gpu.md](gpu.md)) draws through a `struct Backend`.
This document describes the second back end, which renders on the 3DS's
PICA200 instead of the CPU, and everything underneath it.

**Status.** The whole path runs and is pixel-compared against the software
rasterizer, but only against a *software model* of the GPU (on the build host
and inside our QEMU machine). It has not run on a real 3DS yet. For that
reason the software rasterizer is still the default; the GPU back end is
selected per GL context with `IOS3DS_GL=pica` (§6). §8 is the checklist for
the first run on hardware.

## 1. Layers

```
app / QuartzCore ── gl* ──> OpenGLES.framework ──> MBXGLEngine (tools/ios3ds-gl)
  api.c, draw.c     GL state, transform + lighting on the CPU, clip-space triangles
  pica_be.c         Backend "ios3ds PICA200": batches draws into command lists
  tools/ios3ds-pica/src
    pica_cmd.c        command-list encoder
    pica_state.c      register encoders: target, viewport, depth/blend/alpha, textures, TEV, vertex layout
    pica_ff.c         fixed function: vertex shader + uniforms, glTexEnv -> TEV, GL enum mapping
    pica_tex.c        texel conversion and 8x8 Morton tiling
    pica_dev_ios.c    transport: sysctl ios3ds_gpu
──────────────────────────────── kernel ────────────────────────────────
  kernel/shim/pica.c  the sysctl node; GPU bring-up; arena; copyin/copyout; cache maintenance
  pica_drv.c          driver core: GX registers, allocator, PSC fill, PPF transfer, P3D submit
──────────────────────────────── hardware ──────────────────────────────
  PICA200 GX block at PA 0x10400000        (QEMU and host tests: tools/ios3ds-pica/sim/picasim.c)
```

`pica_drv.c` is freestanding C behind a small platform table. The shim links
it against real registers; the host tests link the very same file against
`picasim` (`pica_dev_host.c`). `pica_be.c` is identical in both builds too.

## 2. How userland reaches the GPU

The PICA200 only understands physical addresses, and iOS userland has no way
to get physically contiguous memory or map device registers. Two designs were
considered:

1. **Map** the GX registers and an arena into every GL process and let the
   engine drive the GPU itself (what the earlier draft of `tools/ios3ds-pica`
   assumed). Fast, but it needs `IOMemoryDescriptor` C++ calls from the shim, a
   cross-process lock and allocator in shared memory, and any GL process could
   make the GPU write anywhere in RAM.
2. **A service in the shim** (chosen): userland sends requests, the shim owns
   the registers and the arena and copies data in and out.

The service is the top-level sysctl node `ios3ds_gpu`, registered by the shim
like `kern.ios3ds_input`. A request is *one extra MIB component*: the address
of a `pica_req_t` in the caller (`tools/ios3ds-pica/include/pica_proto.h`).
`sysctl_root` hands a `CTLTYPE_NODE` handler the unconsumed part of the name,
so the handler gets that address, `copyin`s the request, runs it and
`copyout`s the result. Nothing is passed as a sysctl "new value", so only
sysctl *read* permission is needed. Reading the node plainly
(`sysctl ios3ds_gpu`) returns a `pica_info_t` without side effects.

The node is not under `kern.`: with `kern.ios3ds_gpu` the first QEMU run
could not reach the service (the engine logged "no service" and fell back to
the software rasterizer, as designed). The errno was not captured in that run.
The likely cause, from the xnu-1504 source: `__sysctl` sends every `CTL_KERN`
name through the old `kern_sysctl()` first, which returns `ENOTDIR` for a name
with more than one component after `kern` unless that component is on a fixed
list. A top-level node goes straight to the OID tree, and works.

| op | effect |
|---|---|
| `INFO` | bring-up on first use; version, state, `PDN_GPU_CNT`, arena address/size/free, counters |
| `ALLOC` / `FREE` / `FREE_ALL` | arena blocks (128-byte granules), owned by the calling pid |
| `WRITE` / `READ` | copy between a user buffer and arena memory (+ cache clean / invalidate) |
| `SUBMIT` | copy a command list into the driver's staging buffer (optionally vertex data first), run it, wait for the end marker |
| `FILL` | constant fill of two arena blocks (CPU copy; PSC cannot write FCRAM, §13) |
| `TRANSFER` | PPF display transfer |
| `CONFIG` | boot.cfg switches for userland (`ca_gpu`), no bring-up |
| `PRESENT` | PPF display transfer from an arena buffer straight into a *user* buffer, after a page-table walk proves the caller may write it (§15) |
| `SCATTER` | for a destination refused as `PICA_ERR_NOT_CONTIG`: copy an arena buffer page by page into the caller's pages through XNU's linear map, with the same walk and cache maintenance per page (§15) |

One request runs at a time (a flag taken with interrupts masked; waiters
`IOSleep`). The same flag keeps GL requests and the display mirror's frames
apart (§9). boot.cfg `gpu=0` makes bring-up refuse before touching any
register; every request then answers `PICA_ERR_GPU_OFF` and the engine uses
the CPU. Blocks of processes that died are reclaimed when the arena runs
full (`proc_find`). Completion is **polled**: `PSC_FILLn_CNT` bit 1, `PPF_CNT`
bit 8, and bit 31 (P3D) of the GX interrupt status register at +0x34, every
20 µs, for at most 2 s. A timeout is logged with the registers involved and
returned as `PICA_ERR_TIMEOUT`; the engine then turns the GPU path off for the
process and continues on the software rasterizer.

The price of design 2 is one copy per texture upload, per vertex batch and
per frame read-back. Interrupt-driven waits (P3D is GIC 45; `shim_irq` in
`entry.S` already has the trampoline) and mapping the arena into clients are
the obvious next optimisations; neither changes the protocol's shape.

Not done: validating the *contents* of command lists. A list can point the
GPU's render target at any physical address. ios3ds already runs with code
signing off, so this is not a new hole, but it should be closed (scan for the
address registers) before anything untrusted may use GL.

### GPU memory: the arena

- VRAM (0x18000000, 6 MiB) is taken: `/vram` gives iOS the first 3 MiB for the
  CLCD framebuffer, Luma's LCD framebuffers (which the ARM9 draws) start at
  0x18300000.
- So the arena is FCRAM: `IOMallocContiguous(10 MiB on an Old 3DS, 12 MiB on a
  New model, page aligned, &pa)` at the first request (falling back to 8, 4 and
  2 MiB; on a New 3DS with 256 MiB it must lie below 0x28000000, docs/n3ds.md
  section 6). The Old model's 10 MiB still holds the camera's 450 KiB frame
  reservation (re-log 108) and gives apps 2 MiB more than the old default;
  hardware verification of that size is still outstanding. It is ordinary wired, cached
  kernel memory; the GPU sees it by physical address. `CFG11_GPUPROT`
  (0x10140140) is written 0 so GPU DMA may reach all of FCRAM.
- Right after allocating, the shim cleans and invalidates the whole D-cache
  once (`c7,c14,0`): lines the pages' previous owner (or a zeroing) left dirty
  would otherwise be evicted over GPU output at some random later time.
- Cache: after the CPU writes arena memory the shim cleans those D-cache
  lines (`c7,c10,1`), and before it reads memory the GPU wrote it invalidates
  them (`c7,c6,1`). Blocks are multiples of 128 bytes, so a line never holds
  two owners' data.
- The first 128 KiB are the driver's command-list staging buffer.
- Placement (`pica_drv.c`, re-log 75 and NEXT): blocks of 256 KiB-2 MiB
  (render targets, depth buffers, read-back staging, the mirror's texture)
  come first-fit from the top of the **medium** pool; smaller ones (textures)
  use page-granular first fit from its bottom (best fit was tried and made
  QEMU scatter small blocks and log retries). The top
  `PICA_DRV_HUGE_RESERVE` (6 MiB in a 12 MiB
  arena, 2 MiB in 8-11 MiB) is two-sided: blocks >= 2 MiB come down from its
  top, small blocks fall back into its bottom and 256 KiB-2 MiB blocks into
  its top only when the medium pool is full, and neither may take the top
  `PICA_DRV_HUGE_KEEP` (4 MiB on New models).
  A 4 MiB texture can then be placed even with small textures
  and 1 MiB targets filling the medium pool (hardware log 2026-11-25).
  The block table holds 224 entries (160 filled up on
  a New 2DS XL, issue 1); free space is the gaps between live blocks, so
  adjacent freed blocks coalesce implicitly. When a block does not fit, the
  GL engine evicts the textures highest in the arena first; LRU order is kept
  for small requests.

### Power and clock

`PDN_GPU_CNT` (0x10141200) must read 0x1007F: bit 16 clock enable, bits 0-6
"not in reset" for the register block, PSC, geometry, rasterizer, PPF and the
two PDC blocks (libn3ds `pdn.h`). Luma leaves it like that whenever it
initialised the screens, which ios3ds requires anyway. The shim checks it at
bring-up and logs the value; if it differs it enables the clock and releases
the resets (as libn3ds `PDN_controlGpu` does) and says so. It never *resets*
the block: the display controllers in it are live.

`pica_drv_init` then does the part of libn3ds `hardwareReset()` that concerns
us: PSC and PPF control registers to 0, `IRQ_ACK`, `IRQ_CMP = 0x12345678`,
`IRQ_MASK`, `IRQ_AUTOSTOP = 1`, `START_DRAW_FUNC0 = 1` (configuration mode;
"if not set the first command list will hang the GPU"), and one throw-away
16-byte texture copy (libn3ds: PPF glitches if the first operation after a
reset is a real one).

## 3. What the back end does (`tools/ios3ds-gl/pica_be.c`)

**Vertices.** The front end still transforms and lights on the CPU, so the
GPU gets clip-space `Vtx` records. `struct Vtx` *is* the vertex buffer format:
four float attributes (position 4, colour 4, texcoord0 2, texcoord1 2) and 4
bytes of padding, stride 52. The vertex shader (`shader/ff.vsh`, assembled by
`picasm.py`) multiplies the position by a "clip matrix" and the texture
coordinates by two texture matrices. Here the clip matrix only
- maps GL's clip z (−w…w) to the GPU's (−w…0), and
- applies a viewport correction when the GL viewport does not fit the GPU's
  register (negative origin, larger than the target): the GPU viewport then
  becomes the whole target and the scissor box does the clipping.

The texture matrices scale [0,1] to the used corner of a power-of-two
texture. Only non-indexed triangle lists are drawn (`DRAWARRAYS`); citro3d's
register sequence is used.

**Render targets.** The GPU renders into 8×8-tiled buffers; iOS surfaces are
linear BGRA. Per render target (keyed by `rt->color`) the back end keeps a
tiled RGBA8 colour buffer and a linear staging buffer in the arena, padded to
multiples of 16 and at least 64×64 (smaller display transfers are unreliable
on hardware, libn3ds). Depth is D24S8, GPU-only, keyed by `rt->depth`.
- *CPU → GPU* (first draw after the CPU copy changed, unless the draw is a
  full clear): rows are written to the staging buffer and a display transfer
  with "output tiled" converts them.
- *GPU → CPU* (`finish`: `glFinish`/`glFlush`, `glReadPixels`, present, or
  switching to another target): display transfer tiled → linear, `READ`,
  then one CPU pass that rotates each word (GPU RGBA8 word is
  `R<<24|G<<16|B<<8|A`, the engine's is `A<<24|R<<16|G<<8|B`) and maps rows.
  The GPU stores GL row 0 last, which is also how `flip` targets (IOSurfaces)
  are laid out.
- Only one target is ever ahead of its CPU copy. `gl_be_forget()` (called
  where the engine frees renderbuffer or texture storage) drops GPU copies of
  memory that is going away.

**Clears.** Every clear is a quad at the clear depth with depth function
ALWAYS. (A full clear used to be a PSC memory fill; hardware run 13 showed
the fill engine writes nothing into FCRAM, and the arena is FCRAM, §13.) A
clear of all colour channels over the whole target skips the upload of the
old picture. New depth buffers are initialised to "far" by a CPU write.
Depth is stored reversed, as citro3d does (§16).

**Textures.** Every texture is uploaded as tiled RGBA8, padded to a power of
two (edge texels replicated), when its `be_seq` changes. An axis that is not
a power of two and wraps with `GL_REPEAT` is stretched to the power-of-two
size instead (nearest), so it repeats with its own period (§16). GPU copies
are evicted least-recently-used when the arena is full (§16).

**State.** All register state of a draw is collected in one struct and sent
when it differs from the last one in the list: viewport, scissor, cull mode,
depth test/func/mask/range, polygon offset (constant term only), alpha test,
blend equation and factors, colour mask, two texture units (filter, wrap),
and the TEV stages. `glTexEnv` maps to one TEV stage per unit: `MODULATE`,
`REPLACE`, `DECAL`, `BLEND`, `ADD` depending on the texture's base format,
and all of `GL_COMBINE` (sources, operands, functions, scale) directly.

**Batching.** Draws accumulate in one command list (≤ 6144 vertices, 96 KiB)
until a flush: `finish`, target change, texture upload, full clear, or a full
buffer. Each list is self-contained (shader, base state, target), so several
GL processes can share the GPU without tracking each other.

**Self-check.** `pica_backend_init` (for `IOS3DS_GL=pica` and `auto`) first
sends a 64x64 pattern through the whole path the frames depend on: WRITE,
PPF linear→tiled, PPF tiled→linear, READ, then a constant-colour P3D quad
into the arena and back (it was a PSC fill until run 13). Any
wrong word or failed operation is logged (`[gl] pica: GPU self-check: …: not
using the GPU`) and the context gets the software rasterizer.

**Fallback.** If the GPU is unavailable, a target is larger than 1024 or a
GPU operation fails, the same draw goes to the software rasterizer, and one
`[gl] pica: …` line says why. A full arena first evicts textures; if that is
not enough, the process writes its GPU images back and draws on the CPU from
then on (§16).

Not done yet: fog (drawn without, logged once), stencil, mipmaps, the slope
term of polygon offset, NPOT textures with REPEAT wrap, flat shading,
textures or targets above 1024. T&L stays on the CPU.

## 4. Register facts and where they come from

Nothing here was measured on hardware yet. Sources: libn3ds
(`include/arm11/drivers/gx.h`, `gpu_regs.h`, `pdn.h`, `source/arm11/drivers/gfx.c`),
libctru/citro3d register names and write sequences, and the Citra/Azahar
software renderer (`src/video_core/renderer_software/sw_rasterizer.cpp`,
`sw_clipper.cpp`, `sw_framebuffer.cpp`, `sw_blitter.cpp`, `pica/pica_core.cpp`)
for semantics. No code was copied from any of them; picaGL was not used.

| fact | source |
|---|---|
| command word pair: value, header (reg 0-15, byte mask 16-19, extra count 20-27, bit 31 consecutive) | Citra `ProcessCmdList` |
| list ends at a `FINALIZE` (0x010) write with a byte equal to `IRQ_CMP`; sets GX stat bit 31 | Citra `HandleSpecialReg`, libn3ds `GX_processCommandList` |
| submit: `CMDBUF_SIZE0 = bytes>>3`, `CMDBUF_ADDR0 = pa>>3`, `CMDBUF_JUMP0 = 1` | libn3ds |
| clip volume −w ≤ x,y ≤ w, −w ≤ z ≤ 0; depth = z/w · scale + offset | Citra clipper / rasterizer |
| cull mode 2 keeps counter-clockwise (positive area, y up), 1 keeps clockwise | Citra `ProcessTriangle` |
| colour/depth buffers and textures are 8×8 Morton tiles; the first stored row is the top (GL) row | Citra `DrawPixel`, `LookupTexture` |
| RGBA8 pixel bytes are A,B,G,R | Citra `EncodeRGBA8` |
| colour buffer formats number RGB5A1 = 2, RGB565 = 3; PPF formats RGB565 = 2, RGB5A1 = 3 | Citra `regs_framebuffer.h` vs `PixelFormat` |
| display transfer flags: bit 0 flip, bit 1 linear → tiled, bit 5 raw, bits 8-10 / 12-14 formats | libn3ds `gx.h`, Citra `sw_blitter.cpp` |
| PSC fill: start>>3, end>>3, value, control (bit 0 start, bit 1 done, bits 8-9 width) | libn3ds, 3dbrew |
| TEV stage 0 has no "previous" | Citra `WriteTevConfig`; we always name the vertex colour explicitly |
| vertex colours are \|c\| saturated before interpolation | Citra `output_vertex.cpp` |

## 5. The software GPU model (`tools/ios3ds-pica/sim/picasim.c`)

QEMU has no PICA200, and porting Citra's C++ core into a C board file was more
work than writing the subset we emit. `picasim` (about 900 lines of C,
GPL-2.0-or-later so it can be linked into QEMU) models the GX block at
register level: PSC fill, PPF display transfer and texture copy, the P3D
register file, command-list decoding, vertex arrays and fixed attributes, the
vertex shader (arithmetic opcodes), clipping, strips/fans/lists, three 2D
texture units in every uncompressed format, six TEV stages with the combiner
buffer, alpha test, depth test, blending, logic ops, write masks and scissor.
It follows Citra's software renderer operation by operation (fill rule, 12.4
coordinates, integer blend maths); two deliberate differences: bilinear
filtering when the magnification filter bit is set (Citra's software path is
nearest only), and bounds checks on every buffer.

Whatever it does not implement (geometry shaders, shader flow control,
fragment lighting, fog, stencil, shadow/cube/proc textures, ETC1, mipmaps,
immediate mode) is counted and logged, never silently ignored.

It is used in two places:
- **host**: `make -C tools/ios3ds-pica test` (encoders, tiling, driver core
  and model end to end) and `make -C tools/ios3ds-gl host-test` (gltest on
  both back ends, pixel-compared);
- **QEMU**: `emu/qemu/hw/arm/nintendo3ds.c` maps it at 0x10400000 (below the
  PDC registers), lets it reach FCRAM and VRAM, drives GIC lines 40/41/44/45
  from its status bits, and reports `PDN_GPU_CNT = 0x1007F`.

**What this does and does not prove.** The model was written from the same
references as the encoders, by the same hand. It catches wrong logic in the
back end (state tracking, batching, target/texture management, orientation,
the protocol, the kernel service) and any encoder/decoder disagreement. It
cannot catch a misreading of the references that both sides share, hardware
behaviour the references do not describe (timing, the cache maintenance,
power-up), or registers that must be written and are not. Those are for §8.

## 6. Using it

```
make -C tools/ios3ds-pica test        # unit tests + driver core against the model
make -C tools/ios3ds-gl host-test     # gltest: software vs PICA200 (model), compared
emu/build-qemu.sh                     # QEMU with the GX model (rebuild needed once)
make -C tools/ios3ds-gl && make -C boot && python3 scripts/mksd.py --image build/sd.img
python3 emu/run.py --time 450 --shots 449
mtype -i build/sd.img@@1048576 ::/ios3ds/log.txt | grep -a 'pica\|gltest'
```

`IOS3DS_GL` in a process's environment picks the back end for GL contexts
created afterwards: `pica` or `auto` = GPU if it comes up, anything else or
unset = software. `ios3ds-gltest` renders its scene on a software context,
then creates a second context with `IOS3DS_GL=pica`, renders again, runs the
same probes and compares the two frames pixel by pixel. A second scene, only
compared between the back ends, covers the less common paths: a viewport
hanging over the target, an NPOT texture, two texture units, alpha test,
`GL_COMBINE`, colour and depth masks, a masked scissored clear and
render-to-texture. If a demo time is
given (plist arguments `delay seconds`), the on-screen animation runs on the
GPU context.

Console lines to expect (QEMU, and hopefully hardware):

```
[pica] bring-up: PDN_GPU_CNT 0x0001007f, GPUPROT 0x00000000, GX clock ..., GX stat ...
[pica] arena 8192 KiB at VA 0x... = PA 0x2...; driver ready (state 0)
[gl] pica: GPU up (self-check passed): arena 8192 KiB at PA 0x2... (8064 KiB free), PDN_GPU_CNT 0x1007f, PICA200
[gl] context 0x... uses the ios3ds PICA200
gltest: GL_VERSION "OpenGL ES-CM 1.1 ios3ds", GL_RENDERER "ios3ds PICA200"
gltest: ... ok   (10 probes)
gltest: scene 1, pica vs software: N of 153600 pixels differ at all, M by more than 24 (max ..) ok
gltest: scene 2, pica vs software: ... ok
gltest: PICA200 PASS (0 failed)
```

## 7. Verification so far

Commands and output are in `re-log.md`, entry 27.
- **Host** (`make -C tools/ios3ds-pica test`, `make -C tools/ios3ds-gl host-test`):
  328 unit checks pass. gltest passes all 10 probes on both back ends. Of
  153,600 pixels, 10 differ by more than 24 between the two (triangle edges,
  fill rule); 27,584 differ at all, by rounding (the GPU path interpolates and
  blends in 8-bit fixed point). In scene 2, 320 pixels differ by
  more than 24 (texel boundaries of a nearest-filtered texture).
- **QEMU** (`-M nintendo3ds`, stock iOS 4.2.1 userland): the shim brings the
  service up (`PDN_GPU_CNT 0x0001007f`, 8 MiB arena from
  `IOMallocContiguous`), `ios3ds-gltest` gets `GL_RENDERER "ios3ds PICA200"`,
  all 10 probes pass, the frame comparison gives the same numbers as on the
  host, and the animated scene drawn through the emulated GPU appears on the
  top LCD. The model logged no unsupported operation. The home screen and the
  software context are unaffected.
- **Display mirror** (§9, re-log entry 28): host test byte-identical to the
  ARM9's display.c in all three modes; QEMU plain and `--icount 2` boots to
  the home screen with the mirror on, self-test exact, spot checks 0/512.
- **Real hardware**: not run.

## 8. First run on a real 3DS: what to look at

1. Boot as usual; `log.txt` on the SD card (or the top-screen console) has
   the `[pica]` and `gltest:` lines. gltest runs at boot with `diag=1` in
   boot.cfg (docs/boot-perf.md §3); nothing else uses
   the GPU, so a failure cannot take the home screen down.
2. `[pica] bring-up:` shows `PDN_GPU_CNT`. Expected 0x0001007F. Anything else
   means Luma did not leave the GPU running; the shim tries to start it and
   logs that.
3. `[pica] TIMEOUT waiting for …` names the engine that did not finish
   (first PPF texture copy, P3D, PSC, PPF) and dumps the status registers.
   - P3D timeout on the first list: suspect the configuration-mode bit, the
     list address (GPUPROT, FCRAM reachability) or the end marker.
   - PPF/PSC timeout: suspect the "done" bit positions (they are from
     libn3ds/3dbrew, not measured).
4. `gltest: PICA200 … FAIL` with all probes wrong but no timeout: suspect the
   cache maintenance (stale lines), byte order of the read-back, or row order.
   The probe lines print the RGB they read; compare with the software lines
   just above. A frame that is correct but mirrored vertically means the
   transfer's row order differs from Citra's.
5. Individual probes wrong: texture probe → tiling/filter/texture cache;
   blend probe → blend register; cube probe → depth mapping or culling
   direction; scissored clear → scissor register.
6. To see it on screen, set the gltest plist arguments to e.g. `60 30`.

Then: make `auto` the default, move waits to the P3D interrupt, map the arena
into clients (or at least present with a PPF transfer straight into the
framebuffer), and move T&L into the vertex shader.

## 9. The GPU display mirror

Until now the ARM9 copied iOS's framebuffer to the LCDs itself
(`boot/arm9/display.c`): 320x480 BGRA, rotated and scaled to 360x240 on the
top screen and 2/3 scale on the bottom one. The ARM9 also serves the root
disk, and the ARM11 kernel spins with IRQs off until each block request is
served, so every millisecond of copying is a millisecond of stalled iOS (the
copy is paced down to 2 fps while md0 is busy for exactly that reason). The
mirror moves the copy to the PICA200, driven by the ARM11 shim. The contract
is `boot/common/gpu_mirror.h`.

### What the GPU does per frame

```
PPF  iOS framebuffer (VRAM 0x18000000, linear BGRA)  -> upload 320x480 RGBA8, tiled    0x18480000
PPF  texture copy: 60 tile rows of 10240 bytes       -> texture 512x512 RGBA8 in the FCRAM arena,
     (output gap 6144 bytes: 512-wide tile rows)        rows 0..479 used (power of two: §13)
P3D  gpumirror.bin list: two textured quads          -> RT top 240x400 RGBA8, tiled    0x18516000
                                                     -> RT bottom 240x320 RGBA8, tiled 0x18573C00
PPF  RT top + bottom (one transfer when Luma's bottom framebuffer follows the top one,
     as it does: 0x18300000 / 0x18346500)            -> LCD framebuffers, RGBA8 -> BGR8
     ... once per framebuffer set (Luma has two, the CPU copy wrote both too)
```

Five GPU operations per frame in "both" mode (4 + 1 per set in the others).
With `stereo3d=1` and the 3D slider up on a stamped home frame the P3D list is
the v4 two-eye one instead: one baseline quad plus three alpha-tested passes
per eye, with the class stamp read through texture unit 1 and the colour
sample through unit 0 at that class's eye offset (docs/stereo3d.md §5). The
left eye goes into the usual top render target, the right eye into the upload
buffer (free after the texture copy); two extra PPF read-outs write the two
LCD framebuffer sets the PDC0 stereo preset points at. Flat frames use the
original list, so slider-down is the ordinary path. Seven operations per
frame in "both" mode while in 3D.

**Why this is a plain scaling.** The 3DS framebuffers are column-major:
framebuffer row j is screen column j, and its 240 pixels run from the bottom
of the screen up. In those terms the CPU copy's "rotation" disappears; both
screens are axis-aligned scalings of the iOS picture (`gm_map()` in the
header is the CPU copy's integer maths as one function):

| screen | framebuffer pixel (i, j) | iOS pixel |
|---|---|---|
| top 240x400 | j 20..379 (else black) | x = ⌊4i/3⌋, y = ⌊4(j−20)/3⌋ |
| bottom 240x320 | i 14..226 (else black) | x = ⌊(3i−40)/2⌋, y = ⌊3j/2⌋ |

**Exactness.** The quads are nearest-sampled with texture coordinates chosen
so that the GPU's `⌊s·320⌋`, `⌊t·480⌋` at each pixel centre equals the CPU
copy's integer division: iOS x = 4/3·q − 1/2 at column q (and so on), which
gives fractional parts 1/6, 1/2, 5/6 at pixel centres, i.e. at least 1/6
texel away from any rounding edge; float24 texture coordinates are good to
~1e-5. The quads' edges lie on pixel boundaries, so coverage is exact too;
the margins are never drawn (both render targets are black-filled once).

**Colour.** The texture holds iOS's bytes B,G,R,A, which the GPU reads as
RGBA8 (bytes A,B,G,R): sample.a = B, .b = G, .g = R. Three TEV stages
rebuild the colour: `g·(1,0,0)`, `+ b·(0,1,0)`, `+ a·(0,0,1)` (multiplications
by 0 or 255 and sums of disjoint channels: exact). The read-out transfer
drops alpha and writes B,G,R bytes, the LCD format.

**The command lists are built at build time** (`tools/ios3ds-pica/mirror/
genmirror.c`) with the same encoders as the GL back end, at fixed physical
addresses: the shim needs no floating point and no encoder. Two lists, one
per sampling hypothesis (below).

### Who does what

- **ARM9** (`boot/arm9/gpumirror.c`): before XNU starts it fills the mailbox
  `gmbox_t` at shim+0x7F00 (boot.cfg switches, Luma's framebuffer addresses,
  the screen mode). When AppleM2CLCD starts driving the display it sets
  `want`. It skips its CPU copy only while the shim's `frames` counter
  advances (within 1 s) and the shim has acknowledged the current mode;
  otherwise it copies as before. A screen-mode change (X) bumps `seq` and
  waits up to 100 ms for the shim to acknowledge it, so no GPU frame for the
  old mode overwrites the console redrawn on the screen given back.
- **Shim, thread context** (`shim_gpu_thread_poll` in `kernel/shim/pica.c`,
  called from the input daemon's sysctl, which polls every 16 ms, and from
  the GL service): once `want` is set it brings the GPU up (the GL service's
  bring-up), checks gpu.bin and the framebuffer layout, runs the self-test
  and switches the mirror on.
- **Shim, IRQ context** (`shim_gpu_irq`, called by `entry.S` on GIC 42 VBlank
  and GIC 29 timer interrupts, on the audio glue's private IRQ stack): every
  `gpu_vbl_div`-th VBlank it starts a frame if the GPU is free, and on every
  interrupt it starts the next operation of a frame in flight whose current
  operation has finished (`pica_mirror.c` `pm_poll`). It never waits for the
  GPU (unless boot.cfg `gpu_spin_us` asks for it). No GPU interrupt is
  enabled: completion is polled, from interrupts that already exist.
- **Arbitration with GL**: `busy` = 1 while a GL request owns the GPU, 2 while
  a mirror frame is in flight. A VBlank that finds GL busy skips its frame
  (`busy_skips`); a GL request that finds a frame in flight sleeps (`IOSleep`)
  until the interrupts have finished it. Mirror lists are self-contained, as
  GL's are, so neither depends on the other's GPU state.

### Self-test and fallbacks

Before the first frame, `pm_selftest` mirrors a synthetic 320x480 picture
(unique colour per pixel, alpha not 0xFF) from the FCRAM arena into an arena
buffer framed by 4 KiB sentinels, and compares **every byte** with `gm_map`.
QEMU's GPU model was written from the same references as the encoders, so
three hardware facts are left open and tried in turn until one variant is
exact (`GM_VAR_*`):

| bit | hypothesis |
|---|---|
| 1 | nearest sampling rounds instead of truncating: list [1], texcoords −½ texel |
| 2 | texture rows are stored the other way round: upload with the PPF flip flag |
| 4 | render-target rows the other way round: per-screen read-outs with the flip flag |

Fallbacks to the ARM9 CPU copy, each logged:
- boot.cfg `gpu=0` (nothing touches the GPU, GL included) or `gpu_mirror=0`;
- no gpu.bin, GPU bring-up failure, framebuffers overlapping the mirror's VRAM;
- self-test: no exact variant, sentinel overwritten, or an operation timing out;
- at run time: an operation not finishing within 1 s (it was 200 ms until soak 4, re-log 47) → mirror off for good;
- iOS framebuffer not 320x480 32 bpp in /vram → frames skipped;
- GL holding the GPU for over a second → the ARM9 copies until frames resume.

### Memory

| range | use |
|---|---|
| shim+0x7F00 (PA 0x20847F00), 256 B | `gmbox_t` mailbox: lines 0-1 ARM9, lines 2-4 ARM11 |
| PA 0x208B0000, 32 KiB | the shim's GPU part (pica.c, pica_drv.c, pica_mirror.c), from gpu.bin |
| PA 0x208B8000, ≤ 32 KiB | gpumirror blob: v4 command lists (flat + two-eye), vertex arrays, slider shift table, texture edge-padding table and diagnostics, from gpu.bin (32336 bytes) |
| VRAM 0x18480000-0x185BEC00 | upload buffer and the two render targets |
| FCRAM arena, 1 MiB | the 512x512 texture (allocated at mirror start-up; its address is patched into both lists) |

The shim's GPU code moved out of the main shim because the main shim must end
below +0x5F00 (probe snapshot, mailboxes). `link.ld` places every object named
`*pica*.o` at 0x808B0000; `scripts/mkboot.py` writes them (bss included as
zeros) plus the blob to `gpu.bin`, loaded by `load=0:/ios3ds/gpu.bin,0x208b0000`.
The main shim calls into it only when the part's header magic is present
(`kernel/shim/gpu_glue.c`), so a missing gpu.bin means no GPU, nothing worse.

### boot.cfg

```
gpu=1            # 0: the kernel never touches the GPU (GL: CPU rasterizer, screen: ARM9)
gpu_mirror=1     # 0: screen copy stays on the ARM9
gpu_vbl_div=2    # a GPU frame at most every 2nd VBlank (30 fps)
gpu_spin_us=0    # IRQ-time busy-wait for the GPU per step
```

## 10. Real-hardware review (what QEMU cannot show)

QEMU has no caches, its GPU finishes every operation inside the register
write that starts it, and its model shares our reading of the references.
This is what was checked or changed for the first hardware run, and how a
log will show it.

| topic | what the code does | unverified assumption | log line that confirms / denies |
|---|---|---|---|
| D-cache, arena | after `IOMallocContiguous` the whole D-cache is cleaned and invalidated once (`c7,c14,0`): dirty lines of the pages' previous owner could otherwise be evicted over GPU output later. Then: clean after every CPU write (WRITE, command lists, self-test buffers), invalidate before the GPU writes and before the CPU reads (READ, TRANSFER/FILL destinations, self-test output) | Old 3DS has no outer cache, so L1 maintenance by MVA reaches memory | `[gl] pica: GPU self-check …` (round trip through PPF + cache), `[gpu] mirror self-test variant …: 0 of 518400 bytes differ` |
| D-cache, mirror | texture and render targets are in VRAM, never touched by the CPU; the blob is written by the ARM9 before XNU runs (never in ARM11 caches); the iOS framebuffer is read exactly as the ARM9 reads it | iOS writes /vram so that DRAM is current (the ARM9 copy works on hardware, so it is) | `[gpu] spot check (GPU copy): 0 of N sampled LCD pixels differ` on a still screen |
| addresses | GL: arena physical addresses only (`IOMallocContiguous` returns the PA; XNU's physBase is FCRAM's real 0x20000000). Mirror: fixed VRAM PAs and the blob's PA (0x208B8000); the source is the PA AppleM2CLCD programs into the CLCD window register | GPUPROT = 0 opens all of FCRAM to the GPU | first P3D list / PPF in the self-test finishes (no `did not finish`) |
| registers | the GX block is reached through `entry.S`'s 1 MiB device section at 0xBFE00000 (the one the VBlank code uses; not mapped twice); PDN and CFG11 through `ml_io_map` | — | `[pica] bring-up: PDN_GPU_CNT 0x0001007f, GPUPROT …` |
| power/clock | Luma's state is kept: `PDN_GPU_CNT` must read 0x1007F (else clock on + resets released, logged); the block is never reset (the LCD controllers are in it); PSC/PPF control registers are zeroed, P3D gets IRQ_CMP/AUTOSTOP/config mode, one throw-away texture copy | Luma leaves PSC/PPF idle | `[pica] bring-up:` value; `[pica] TIMEOUT waiting for the first PPF texture copy` would deny |
| completion | polled: PPF_CNT bit 8, PSC_FILLn bit 1, GX +0x34 bit 31 (P3D, after IRQ_ACK). No GIC line is enabled beyond main's 29 and 42; the mirror is stepped from those two | the bit positions (libn3ds/3dbrew) | self-test `GPU a/b/c ticks (upload/draw/read-out)` non-zero and sane; `did not finish` names the stage otherwise |
| VRAM | mirror: 0x18480000-0x185BEC00 (above Luma's second framebuffer set, below 6 MiB); the shim refuses framebuffers inside it or inside /vram | Luma's framebuffer addresses (checked at run time) | `[gpu] display mirror: LCD framebuffers top 0x18300000/0x18400000 bottom 0x18346500/0x18446500 (2 sets)` |
| texture | NPOT 320x480 RGBA8 texture, nearest, clamp | the PICA200 samples NPOT textures (Citra handles any size; libctru's docs ask for POT) | **denied by run 13** (§13): now a 512x512 texture |
| sampling | truncation (Citra) tried first, rounding second | — | `variant 0 (as modelled)` vs `variant 1 (sampling rounds)` |
| row order | texture and render-target rows as Citra stores them tried first, flips after | — | the variant number (bits 2 and 4) |
| present | `PRESENT` walks the caller's TTBR0 tables (L1 section/coarse, L2 small/large; AP = 3 and APX = 0 required), checks contiguity and FCRAM/VRAM, cleans+invalidates the range by VA, then PPFs into the PA | the ARMv6 descriptor layout as in the ARM ARM; that the CLCD scans out what PPF writes to /vram without further maintenance; no outer cache | `[pica] present: GPU writes the caller's surface directly: user VA … = PA 0x18000000 (VRAM, 600 KiB)`; a refusal prints `[pica] present: destination … not usable: …` with the descriptors, and the engine falls back to the read-back; `[gl] pica: GPU present check passed` / `FAILED` compares the first non-uniform presented frame with the CPU read-back |
| GPU hang | every mirror operation has a 1 s limit (200 ms until soak 4); a timeout switches the mirror off for good, the ARM9 notices within 1 s | — | `[gpu] display mirror FAILED (GPU timeout: operation N of a frame did not finish, register …)` or, in the self-test, `self-test aborted, GPU operation N did not finish` + `[gpu] screen copy: ARM9 CPU again` |

## 11. First hardware run of `agent/gpu`: what to look at

Everything below is in `sd:/ios3ds/log.txt`. Nothing GPU-related can stop
the boot: the mirror only starts after the home screen's display is on, the
GL test runs on its own context, and every failure falls back to the CPU.

1. `[gpu] PICA200: gpu=1 gpu_mirror=1 (every 2. VBlank, spin 0 us); mailbox at 20847f00`
   (ARM9, before XNU): boot.cfg was read.
2. `[pica] bring-up: PDN_GPU_CNT 0x0001007f, GPUPROT …, GX clock …, GX stat …`:
   Luma's power state (§2). Anything but 0x1007F is followed by what the shim
   did about it. `GX clock` read 0 on hardware in run12: Luma never sets it
   (§12); the next line says what the shim set.
3. `[gpu] display mirror: LCD framebuffers top 0x18300000/0x18400000 bottom
   0x18346500/0x18446500 (2 sets)`: Luma's layout as the mirror expects it.
   Otherwise `… is not where the mirror can write it` and the CPU copy stays.
4. `[gpu] mirror self-test variant N (…): B of 518400 bytes differ, guard
   intact; GPU a/b/c ticks (upload/draw/read-out)`: one line per variant tried.
   - variant 0 with 0 bytes: the model's assumptions hold. Ticks are
     ~0.955 µs: these are the **real** GPU costs of one frame's stages.
   - a later variant passing tells which assumption was wrong (§9 table).
   - all fail: the first-difference line (screen, framebuffer row, pixel,
     expected vs got byte) is the starting point. All-zero output: P3D drew
     nothing (sampling/texture setup). Shifted by one pixel: sampling. Garbage
     everywhere: NPOT texture or tiling. Right picture, wrong colours: TEV or
     RGBA8 byte order.
   - `GPU operation k of n (…) did not finish`: completion bit or a hung
     engine (§4 sources); the stage is named.
   - `guard OVERWRITTEN`: the read-out's dimensions are misread; mirror off.
5. `[gpu] display mirror ACTIVE …` then `[gpu] screen copy: PICA200 (ARM9 CPU copy paused)`.
6. Every 30 s: `[gpu] mirror: F frames in 30 s (x fps); GPU per frame: upload …
   us, draw … us, read-out … us; frame avg … us (incl. IRQ latency); ARM11 IRQ
   time … us/frame …` and `[gpu] spot check (GPU copy): 0 of 512 sampled LCD
   pixels differ` (on a still screen; animation can make a few differ).
   Compare with the `[monitor] CPU screen copy: … ms each (ARM9)` lines from
   before the mirror started, and with md0 throughput in the `[hb]` lines.
7. `[gpu] screen copy: ARM9 CPU again (GPU frames stopped …)`: GL held the
   GPU for over a second (fine, it comes back) or the mirror failed
   (`display mirror FAILED (GPU timeout)`; the register value is in the
   mailbox, `first_bad[3]`).
8. GL: `[gl] pica: GPU up (self-check passed)` or `[gl] pica: GPU self-check:
   …: not using the GPU`; then the gltest lines as in §8, including
   `gltest: PICA200 offscreen frame took N ms` next to the software one.
9. X still cycles the screen modes; the console must come back cleanly on the
   screen given back (the shim acknowledges each mode change first).
10. With `stereo3d=1`: slider down must give the same `[stereo] active path:
    flat (GPU mirror)` and frame times as the flag-off run. Slider up on the
    home screen gives `[stereo] switch: PDC0 3D, PICA200 two-eye mirror`,
    `[stereo] 3D LED on` and `[monitor] stereo 3D: both eyes ... barrier on`;
    app frames and slider-down give the matching 2D/barrier-off lines. A
    `[stereo] switch FAILED` means the shim never acked: the mirror stays flat
    rather than showing a broken pair. See docs/stereo3d.md §8.

If anything goes wrong: `gpu_mirror=0` in `sd:/ios3ds/boot.cfg` brings back
the ARM9 copy exactly as on main; `gpu=0` keeps the kernel away from the GPU
altogether (GL then uses the CPU rasterizer). Commenting out the
`load=0:/ios3ds/gpu.bin,…` line has the same effect as `gpu=0` (the shim
then finds no GPU part); do not just delete the file: a `load=` entry whose
file is missing stops the boot, as for any `load=` entry.

## 12. Hardware run 12: what failed, and the staged diagnostics

First hardware run of the GPU paths (`m1-21f26fc`, docs/hwlogs/run12.txt).
Every fallback worked (ARM9 copy, CPU GL); the GPU did not:

```
[pica] bring-up: PDN_GPU_CNT 0x0001007f, GPUPROT 0x00000000, GX clock 0x00000000, GX stat 0x0c000000
[gpu] mirror self-test variant 0 (as modelled): 451182 of 518400 bytes differ, guard intact; GPU 635/635/579 ticks
[gpu]   first difference at byte 14400 (top screen, fb row 20, pixel 0): expected 0x00, got 0x80   (0x40 with the read-out flip)
[gl] pica: GPU self-check: memory fill left 4096 of 4096 words wrong (word 0: 0x5a5aa5a5): not using the GPU
```

What the lines say:
- **PPF works**, FCRAM to FCRAM, in both directions (the GL self-check's
  transfer round trip passed before the fill step), and so do the shim's
  cache maintenance and the addresses (PA >> 3).
- **The PSC fill into FCRAM did nothing**: after the fill the transfer
  copied the *old* tiled pattern back (`0x5a5aa5a5` is word 0 of it). The
  done bit still came on.
- **The mirror's read-out ran** (the black margins of the output are right:
  the first 14400 bytes match), but the render targets held something else
  than the picture. Every stage took ~0.6 ms.
- **`GX clock` (GX +0x004) reads 0.** Luma's screen init
  (`arm11/source/main.c initScreens`) writes PDN_GPU_CNT and the PDC
  registers only. libn3ds `hardwareReset()` and GSP write 0x70100 there
  ("each bit seems to be a different clock setting"), clear the VRAM bank
  disable bits in +0x030 and set the DMA priorities at +0x050/+0x054. Our
  bring-up did none of that. Leading hypothesis: the fill engine and P3D
  need those clocks; the PPF does not.

**Changes for the next run.** Bring-up now does the rest of libn3ds'
`hardwareReset()` without the PDN reset (the LCDs stay live): GX clock
0x70100 if it reads 0, VRAM banks on, DMA priorities 0x22221200 / 0xFF2
(boot.cfg `gpu_clk=0` skips this). Around it, `tools/ios3ds-pica/src/
pica_diag.c` runs staged tests and logs `[gpu-diag]` lines (boot.cfg
`gpu_diag=0` skips them):

| step | what | tells |
|---|---|---|
| regs `as found` | GX +0x00..0x58, PSC, PPF, P3D IRQ registers, and the GPU's memory-traffic counters at +0x70..+0xBC (libn3ds `GxRegs`: external / VRAM A / VRAM B reads and writes, vertex, texture, colour, depth, PPF, fill0/1) | Luma's state |
| fill `as found` | PSC fill of 4 KiB in VRAM (0x185C0000) and in FCRAM (arena), before any setup | does the fill engine work without the clock setup; counters say whether it wrote anything at all |
| setup | the clock / VRAM / priority writes, logged old → new | |
| regs + fill `after setup` | the same again | the clock hypothesis, directly |
| PPF | 64x64 FCRAM → VRAM tiled → FCRAM | the known-good path, now with counters |
| P3D solid → VRAM | constant-colour quad into a 64x64 VRAM target | command list, shader, rasteriser, colour write; counters vtx / col |
| P3D solid → FCRAM | the same list with its target patched into FCRAM | can P3D render to FCRAM (the GL back end's targets live there) |
| texture 64x64 / 48x40 | a texture written by the CPU into VRAM, drawn 1:1 with REPLACE | texture fetch, tiling, and NPOT textures (the mirror uses 320x480) |
| summary | one line, ok/FAIL per step | |

If the mirror self-test still fails, the shim also dumps the first texture
tile and one tile of the top render target next to what they should hold
(`[gpu-diag] mirror texture tile 0 / mirror top target`); the host test
checks those expectations against a passing self-test.

How to read the outcomes:
- fill FAIL → ok across the setup, P3D ok: the clock was it; the mirror and
  GL self-checks should pass in the same run.
- fill VRAM ok but FCRAM FAIL after setup: the fill engine cannot reach
  FCRAM. GL clears would have to go through P3D quads or VRAM targets.
- P3D solid VRAM FAIL with `vtx +0`: the list never fetched vertices
  (list/attribute setup); `col-wr +0` with vertices read: nothing reached
  the colour buffer (shader output map, viewport, TEV, colour buffer setup).
- solid ok, texture FAIL: texture unit (address, format, tiling); POT ok and
  NPOT FAIL: the GPU cannot sample a non-power-of-two texture at all. The
  diagnostic's NPOT step now checks the *workaround* (§13), not a raw NPOT
  texture: a padded 64x64 texture with scaled texcoords, as the GL back end
  uploads it.

## 13. Hardware run 13: the diagnostics' answer, and what changed

Run 13 (`eab9618` = §12's build, docs/hwlogs/run13.txt):

```
[pica] GX setup as libn3ds hardwareReset: clock 0x00000000 -> 0x00070100, … dma prio 0x1111ef00/0x00000112 -> 0x22221200/0x00000ff2
[gpu-diag] summary: fill VRAM ok->ok, fill FCRAM FAIL->FAIL (as found -> after setup); PPF ok; P3D solid VRAM ok, FCRAM ok; texture POT ok, NPOT FAIL
```

- **The GX clock was not the cause.** Fills into VRAM worked before and
  after the setup.
- **PSC fills do not write FCRAM.** Done bit set after 1 tick, `fill0 +256`
  (the engine ran), `ext-wr +0` (no external-memory write), 0 of 1024 words.
  Into VRAM: `vramB-wr +266`, all words. Taken as a hardware rule.
  The only PSC fill left in the GL path was already replaced by P3D quads in
  run 13 (clears, depth init, self-check); the mirror's black margins are
  VRAM fills, which work. `PICA_OP_FILL`, the remaining API that could have
  filled the FCRAM arena, now writes the constant with the CPU (`pica_drv_fill`)
  and cleans the cache lines, so a caller can never silently get an unfilled
  buffer. The raw PSC fill stays only in `[gpu-diag]`, to report the hardware
  rule. Host test: `test_pica` checks 16/24/32-bit fills, both slots and a
  rejected out-of-arena range; `test_mirror` still expects the diagnostic's
  FCRAM bit to fail.
- **P3D works**, into VRAM and FCRAM (`col-wr +1088` / `ext-wr +2176`), with
  a 64x64 texture (`tex +1024`). Command lists, the fixed-function shader,
  vertex fetch, TEV and colour writes are right as encoded.
- **NPOT textures are broken**: the 48x40 texture drawn 1:1 gave 0 of 1920
  texels (`tex +256`: the unit fetched far less than the 1920-texel image).
  The mirror's 320x480 texture is NPOT, which explains its failure.
- **The texture tile dump** (run13 lines 288-291) is not a second bug: the
  dump ran after the last self-test variant (7), which uploads with the PPF
  flip flag. Decoded with the self-test pattern, texture word i of tile 0 is
  iOS pixel (x, 479 - y) for its tile position (x, y) - exactly a flipped
  upload of a correct linear→tiled transfer (`(0,479) (1,479) (0,478) …`).
  The target tile then holds iOS pixels (128..130, 300) everywhere: NPOT
  sampling. The dump now re-runs variant 0 before dumping, and shows the
  upload buffer, the texture and the target.

**Model.** picasim now does what the hardware did: a fill outside VRAM sets
its done bit and writes nothing, and a non-power-of-two texture samples
noise (each said once in the log, not counted as a model error). With that,
the old mirror and the old GL self-check failed in the host tests and in
QEMU exactly as on hardware (re-log 30), and the model reproduces run 13's
diagnostic summary bit for bit (`test_mirror`).

**NPOT diagnostic check.** Since the hardware rule is confirmed, the second
texture step no longer asks the GPU for a raw 48x40 texture. `genmirror.c`
now builds its vertex array for a 64x64 texture with texcoords scaled by
48/64 and 40/64; `pica_diag.c` writes the 48x40 image into the used corner
of the padded texture (zero elsewhere) and compares the 1920 drawn texels.
This is the same padding/coordinate path `pica_be.c use_texture()` uses for
NPOT GL textures, so `PD_TEX_NPOT` now means "the engine's NPOT workaround
renders on the GPU" and should read **ok** on hardware. A failure there
points at tiling, scaled texcoords or the nearest-sampling fill rule; a
raw NPOT texture is still expected to sample noise (the quirk stays in the
model). `test_mirror` checks all diagnostic bits except the FCRAM fill.

**Mirror.** The texture is 512x512 (power of two) in the FCRAM arena. The
upload still goes linear → tiled into VRAM (320 wide), then a PPF texture
copy moves the 60 tile rows (10240 bytes each) into the 512-wide texture
(16384 bytes per tile row: output gap 6144 bytes). Texture coordinates: iOS
x / 512 and (512 − iOS y) / 512, same 1/6-texel margins. Host test: byte-
identical to display.c in all modes, also with a GPU that finishes late.

**GL.** No more PSC fills: clears are quads, the depth buffer starts "far"
through a CPU write, and the self-check draws a constant-colour quad into the
arena instead of filling it.

**Unverified on hardware:** the texture copy with an output gap (libn3ds'
`GX_textureCopy` documents width and gap in 16-byte units; only a 16-byte
copy without gaps has run so far, in `pica_drv_init`) and texture fetches
from FCRAM (citro3d allocates textures in FCRAM by default, so very likely).
The mirror self-test covers both: a wrong copy or fetch fails it.

## 14. The CPU copy, faster

On hardware the ARM9's copy took 313-433 ms per frame (run 12/13), and the
GPU mirror has not passed its self-test on hardware yet, so the CPU copy is
what users see. `boot/arm9/display.c` now:
- reads every needed iOS row whole with 16-byte `LDM` bursts into ARM9 RAM
  (the old code did one 32-bit VRAM read per used pixel);
- packs each output column (240 BGR8 pixels) into 180 words in ARM9 RAM
  (4 pixels = 3 words) and stores it with 16-byte `STM` bursts (the old code
  wrote three single bytes per pixel);
- writes only the framebuffer set each LCD shows. The shim reads the PDC
  select registers (PDC0/PDC1 +0x78; the ARM9 cannot reach the GPU block)
  on every VBlank and publishes them in the mailbox (`gmbox_t.pdc_swap`);
  until then, or without gpu.bin, both sets are written as before. The old
  code wrote set 0 and then copied it to set 1 by reading VRAM back.

VRAM traffic per "both" frame: before, about 160k word reads + 490k byte
writes + 122k word reads + 122k word writes (the second set), all single
accesses; now about 217k word reads + 122k word writes, all in 4-word
bursts. Results stay byte-identical (host test: CPU copy vs GPU model vs
`gm_map`, all modes, and the shown-set-only case). QEMU cannot show bus
costs; its instruction-clocked timing is in re-log 30.

## 15. Presenting into the caller's memory (`PICA_OP_PRESENT`)

For CoreAnimation on the GPU (docs/gpu.md §7) the engine needs the final
frame in the display IOSurface. Copying it through the arena (`TRANSFER` to
scratch, `READ`, byte rotation on the CPU) cost ~616 KiB of CPU copying per
frame. `PRESENT` (protocol 3) lets the GPU write the caller's buffer itself:

- arguments: source PA (an arena block, tiled RGBA8), dimensions, the
  destination's *user* VA and size, PPF flags; only a plain RGBA8→RGBA8
  tiled→linear transfer (optionally row-flipped) is accepted;
- the shim walks the page tables the MMU uses for that VA (TTBR0, TTBCR.N;
  L1 section or coarse table; L2 large/small page) for every 4 KiB step and
  requires user read/write access (AP = 3, APX = 0) — so the GPU writes only
  what the caller could write — physical contiguity, and a range inside FCRAM
  or VRAM; a refusal returns `PICA_ERR_FAULT` / `PICA_ERR_NOT_CONTIG` and logs
  the descriptors and TTBR0/TTBCR/DACR/SCTLR once;
- D-cache lines of the range are cleaned and invalidated by VA before the
  transfer (the CPU may have dirty lines from earlier software frames).

XNU's pmap maps writable pages read-only (AP = 2) until their first write
(modified-bit emulation), so the engine touches every destination page just
before the request. Residual risk, documented rather than solved: another
thread of the same process could unmap the buffer between the walk and the
transfer; the GPU would then write a page the process no longer owns. The
display surface is wired for the life of the window server, and command lists
are not validated either (§2), so this is not a new class of hole.

In QEMU the display IOSurface turned out to be AppleM2CLCD's framebuffer at PA
0x18000000 (VRAM, contiguous, 600 KiB). On hardware the same `[pica] present:`
line answers it; any failure falls back to the read-back.

When the walk finds several physical runs, `PRESENT` refuses with
`PICA_ERR_NOT_CONTIG` (CA's second 320x480 surface after an app returns to the
home screen, hardware runs 21/25/29 and the New 2DS XL's run 1). The engine
then runs the same PPF transfer into a linear arena buffer and calls
`PICA_OP_SCATTER`: the shim repeats the walk for each 4 KiB page, requires
user write access and FCRAM (VRAM has no linear map), drops the caller's
lines for that page, copies it through `pa + 0x60000000` (XNU's linear map)
and cleans the new lines. No pixel is touched and only that one surface takes
the detour; other refusals and GPU errors still switch the whole process to
the CPU read-back. Protocol version 4 adds this op, plus a test flag
(`PICA_PRESENT_TEST_SCATTER`, bit 31 of PRESENT's flags): the shim then
refuses FCRAM destinations, so `IOS3DS_GL_SCATTER=1` drives the detour in
QEMU without disturbing the VRAM display's direct present.

## 16. Hardware run 15: depth, the arena, NPOT repeat

Run 15 (`f45dc07`, `ca_gpu=1`, build/hwlogs/run15.txt) composited CA on the
GPU and showed three faults.

**Depth tests passed nothing.** gltest's cube was drawn only with
`GL_DEPTH_TEST` off (`cube variant no depth test -> drawn`, every variant
with depth -> `NOT drawn`), and scene 1 differed from the CPU in 12070 pixels
(max 230). The back end stored GL's depth directly (`DEPTHMAP` scale
far−near, offset far; `glClear` = a quad at z/w = 0, i.e. depth exactly 1.0)
and tested `GL_LESS`. The model now stores a fragment depth of 1.0 as 0 (the
conversion to 24-bit fixed point wrapping, `picasim.c fragment`); with the
old convention the host gltest then fails with **12070 pixels, max 229** in
scene 1 — run 15's numbers. That is the working explanation; the depth probe
below confirms or refutes it on hardware.

Fix (`pica_raster_t.depth_conv`, `pica_state.c`): citro3d's convention,
which every hardware homebrew uses (`C3D_DepthMap(true, -1, 0)` with
`GPU_GREATER`): stored = K·(1 − d), K = 1 − 2⁻¹⁰, so far is 0 and no value
reaches 1.0; `DEPTHMAP_SCALE` = −K·(far − near), `DEPTHMAP_OFFSET` =
K·(1 − far − bias); the test functions are mirrored (LESS↔GREATER,
LEQUAL↔GEQUAL); new depth buffers start at 0. GL semantics are unchanged.

**Depth probe** (`pica_be.c depth_probe`, `ca.c depth_scene`): at bring-up
every process draws a 96×96 scene with a depth buffer (`glClear` of depth,
`GL_LESS` with a hidden and a visible quad, `GL_GREATER`) on the CPU and on
the GPU under each convention and logs, e.g. on the model:

```
[gl] pica: depth probe, GPU vs CPU rasterizer (pixels off by more than 24 of 9216): reversed (citro3d) 0 ok, standard 0 ok, unscaled (run 15) 4608 FAIL: using reversed (citro3d)
```

Reversed is used if it matches, else "standard" (K·d); if neither does,
depth-tested drawing goes to the CPU (`IOS3DS_GL_ZCONV=0/1/2` forces one).
On hardware, `unscaled FAIL` with the others `ok` confirms the wrap.

**Arena exhaustion.** After a few minutes CA wanted a 1 MiB texture with
829 KiB free in 78 blocks: `[gl] pica: GPU arena full (texture): sampled as
opaque black`, then 128 KiB requests failing forever. Not a leak (deleted
textures and renderbuffers were freed; exited processes' blocks are reclaimed
when full): CA keeps a texture for every layer image it shows, many padded to
512×512 (1 MiB), and every render target held a second, linear staging buffer
of its full size (1 MiB for the 320×480 display) that is used only during a
transfer. 8 MiB minus the mirror's 1 MiB texture does not hold all of that.
Fixes:
- every texture with GPU memory is on a list with the draw that last
  sampled it; when an allocation fails the least recently used ones are
  evicted (the engine keeps every image, the next use uploads it again)
  until it fits; textures of the draw being built stay;
- the linear staging buffer is allocated only for the upload / read-back
  transfer and freed right after;
- if memory cannot be had even then, the process writes every GPU image back
  and draws on the CPU rasterizer from then on, with one line
  (`[gl] pica: GPU arena full (…) …: this process draws on the CPU from now
  on`); nothing is drawn black any more (the "black" path is left only for
  textures larger than 1024);
- the shim's `[pica] arena full` line is rate-limited (first 4, then every
  256th, with a count): failures are now routine;
- CA's 30 s line adds `GPU memory: N KiB held (T textures, K KiB), arena F
  KiB free, E evictions`.

Host check: gltest draws 24 textures of 512×512 (24 MiB through a 16 MiB
model arena), each twice: 0 of 48 draws wrong, 39 evictions, GPU still on.

**NPOT textures with `GL_REPEAT`** were padded to a power of two and their
coordinates scaled by w/pw, so they repeated with the padding (edge texels)
instead of their own period (the likely source of the stripe garbage in
Settings' grouped table views on run 15's photos). Such an
axis is now stretched to the power-of-two size on upload. CA's scene check
gained a 20×12 2D texture repeated 2.5×3.5 times and a 20×12 rectangle
(0x84F5) texture, and its limit is 1 % of pixels (run 15: 0, the model ~6).
With the old code the check fails (750 pixels); now 6. All GPU textures were
already power-of-two in memory (rectangle and `BindCoreSurface` textures
included); the model's NPOT noise applies to any texture whose size registers
are not powers of two, so the GL path would show it.

## 17. Hardware run 17: the mirror and the display going off

- The shim mirrors nothing while iOS' CLCD is disabled (display off). The ARM9 now treats that as "nothing to copy", not as "GPU frames stopped". After a wake it gives the GPU 1 s before it copies with the CPU (`gpumirror.c gm_live`).
- Mirror vs GL: a VBlank that finds a GL request on the GPU counts a busy skip and marks the frame pending. GL requests then wait (`take`) until the next interrupt that finds the GPU free has started that frame, so GL work back to back cannot starve the screen copy. The flag is dropped whenever the mirror is not active, so GL never waits on a stopped mirror.
- Run 17's depth probe: `reversed (citro3d) 0 ok, standard 4608 FAIL, unscaled (run 15) 4608 FAIL`. Reversed depth (§16) is confirmed on hardware. The scaled standard convention fails too, which the picasim model (§16) does not reproduce yet.

