# GPU: OpenGL ES 1.1 on ios3ds

iOS 4.2.1 on the iPod touch 2G draws GL through the MBX Lite GPU. The 3DS has a
PICA200 instead, and Apple's `MBXGLEngine` can't drive it. `tools/ios3ds-gl` is a
replacement engine that plugs into the same slot inside `OpenGLES.framework`.
It draws with a CPU rasterizer by default. A second back end renders on the
PICA200 itself ([pica200.md](pica200.md)); it is opt-in until it has been
verified on real hardware.

Apps, and later QuartzCore, keep using Apple's `OpenGLES.framework` unmodified.
They call `gl*` and `EAGLContext` as usual. Only the engine behind those calls is
ours.

## 1. How OpenGLES.framework finds an engine (8C148)

`_eagl_init` (cache VA 0x34ff870c) runs once, from the first
`-[EAGLSharegroup initWithAPI:]`:

1. It tries `OpenGLES.framework/GLEngine.bundle/GLEngine`, the SGX/GLI engine.
   That bundle doesn't exist on the iPod, so the call fails.
2. `IOServiceGetMatchingServices(IOServiceMatching("AppleMBXDevice"))` must
   return at least one service. Otherwise EAGL has no engine, and
   `initWithAPI:` returns nil.
3. `CFBundleCopyResourceURL(com.apple.opengles, "MBXGLEngine", "bundle")` and
   `CFBundleCreate` run. Then `CFBundleGetDataPointerForName("GLESGetEGLInterface")`
   is called, and the result is saved in `_mbx_dispatch` (0x3e7d84c8).

Then:
- `-[EAGLSharegroup initWithAPI:]` uses the MBX engine only for `api == 1`
  (ES1). It calls `CreateSharegroup(&S[0x44])` and sets the engine id `S+4 = 2`.
- `-[EAGLContext initWithAPI:properties:]` calls
  `CreateGC(sg, X+0x10, X+0xD34, X+0xC)`. Here `X` is the context's private block:
  - +0xC holds the engine GC;
  - +0x10 and +0xD34 are two 841-slot `__GLIFunctionDispatchRec` tables. The second
    starts at 0x10 + 841·4.

Each exported `gl*` is a trampoline:
`mrc p15,0,rT,c13,c0,3; ldr rX,[rT,#0x78]` (the current context's X), then
`r0 = [X,#0xC]`, `fn = [X,#0x10+4·slot]`, followed by a tail call with the GL
arguments shifted up by one. `tools/ios3ds-gl/genslots.py` reads each export's
slot from your own dyld cache and writes `gl_slots.h`. That file holds only names
and numbers.

### What ios3ds changes (rootfs/recipe.json)

| change | why |
|---|---|
| file patch, cache file offset 0x4ff9458 (OpenGLES `__cstring`, VA 0x34ff9458): `"AppleMBXDevice\0"` → `"IOResources\0\0\0\0"` | Step 2 now matches the always-registered `IOResources` nub. No kernel or device-tree change is needed. Only OpenGLES's copy of the string changes; the copies in MBX2D and MBXGLEngine are untouched. |
| write `/System/Library/Caches/com.apple.dyld/enable-dylibs-to-override-cache` | `MBXGLEngine` is in the shared cache. With this switch, dyld (`sDylibsOverrideCache`) loads the on-disk file at the same path instead. The rootfs keeps no other cached dylibs on disk (only 12 symlinks into frameworks that exist only in the cache), so nothing else changes. |
| file patch, cache file offset 0x46de6a8 (QuartzCore `CA::WindowServer::M2Display::M2Display` literal pool, VA 0x346de6a8): `'L565'` → `'BGRA'` | The display constructor probes `eagl_context(2)`, then `eagl_context(1)`; on success it stores the API at `+0x17c` and the display surface format at `+0x180`: `'BGRA'` for API 2, `'L565'` (16 bpp) for API 1. With this patch an ES1 compositor renders the 32 bpp surfaces the CLCD scans out and the screen mirror reads. (Until 2026-11-18 a patch at 0x46de620 skipped the ES1 probe instead, keeping CA's software compositor; §7.) |
| add `OpenGLES.framework/MBXGLEngine.bundle/MBXGLEngine` (0755) | Our engine, ad-hoc signed. The bundle's stock `Info.plist` already names `MBXGLEngine` as its executable. |
| add `/usr/libexec/ios3ds-gltest` and `com.ios3ds.gltest.plist` | The test program (§5). |

A kernel-side alternative to the string patch is to publish an IOService of
class `AppleMBXDevice` from the shim, or to keep the `/arm-io/mbx` device-tree
node with a stub driver. Neither is needed while userland is the only consumer.

## 2. The engine interface

`GLESGetEGLInterface()` returns a table of 11 function pointers. The 4.2.1 table
is the 3.x table of nine plus Set/GetProperty. Every caller was checked in
8C148's OpenGLES; the stock MBXGLEngine's `_GLESBindView`, `_GLESPresentView` and
`_GLESCreateBuffer` confirmed the drawable protocol.

| off | entry | called from | ios3ds |
|---|---|---|---|
| 0x00 | `CreateSharegroup(void **sg)` → ≠0 ok | `-[EAGLSharegroup initWithAPI:]` | `gl_share_new` |
| 0x04 | `DestroySharegroup(sg)` | `-[EAGLSharegroup dealloc]` | refcounted release |
| 0x08 | `CreateGC(sg, front, back, &gc)` → ≠0 ok | `-[EAGLContext initWithAPI:properties:]` | new GC; fills both tables |
| 0x0C | `DestroyGC(gc)` | `-[EAGLContext dealloc]` | |
| 0x10 | `BindCoreSurface(gc, target, IOSurfaceRef)` | `attachImage:toCoreSurface:invertedRender:` (OpenGLES allows `GL_RENDERBUFFER` only with invertedRender=YES, and 0x84F5 only with NO) | renderbuffer: draw straight into the surface. 0x84F5 or `GL_TEXTURE_2D`: the bound texture samples the surface. 32 bpp `'BGRA'` surfaces only. |
| 0x14 | `BindView(gc, drawable, ifmt, retained)` | `renderbufferStorage:fromDrawable:` (`[layer nativeWindow]`; ifmt `GL_RGBA8_OES`/`GL_RGB565_OES`) | see below |
| 0x18 | `FinishTexture` | never called | returns 1 |
| 0x1C | `PresentView(gc)` | `presentRenderbuffer:` | `drawable->present(d, 1)` |
| 0x20 | `SwapNotification(gc, fb_id, transaction, layer)` | `swapNotification:forTransaction:onLayer:` (CA's GL renderer) | `IOMobileFramebufferSwapSignal(main display, transaction, layer)`. 4.2.1's `IOMobileFramebufferGetID` is no longer a port, so a 3.x-style signal by id would never complete CA's swaps. |
| 0x24 | `SetProperty(gc, pname, const void *)` | `setParameter:to:`, profiler (pname 100) | ignored |
| 0x28 | `GetProperty(gc, pname, void *)` | `getParameter:to:`, profiler | writes 0 |

**QuartzCore drawable (`[CAEAGLLayer nativeWindow]`).** This is a block of function
pointers, each called with the drawable as its first argument:

| off | call | stock engine use |
|---|---|---|
| +0x04 | `bind(d, fourcc, CallbackBlock *)` → ≠0 ok | BindView: fourcc `'BGRA'` (0x42475241) for RGBA8/RGB8, `'L565'` for RGB565 |
| +0x08 | `unbind(d)` | rebinding, or failure |
| +0x0C | `dequeue(d)` → IOSurfaceRef | before the first draw of each frame (`_ViewTextureBeginIfNeeded`) |
| +0x10 | `present(d, 1)` → ≠0 ok | PresentView, only if a frame was begun |

`CallbackBlock {ctx, create(ctx, IOSurfaceRef) → 1 ok, destroy(ctx, IOSurfaceRef)}`
is how CA announces each buffer it allocates for the layer. ios3ds always asks for
`'BGRA'`, because the rasterizer draws 0xAARRGGBB words. RGB565/RGB8
renderbuffers keep alpha at 1 instead (`RT.opaque`). The drawable dequeues at
bind time, so `GL_RENDERBUFFER_WIDTH/HEIGHT` are valid straight after
`renderbufferStorage:fromDrawable:`. GL row 0 is the surface's last row. With a
retained backing, the last presented frame is copied into a newly dequeued
buffer.

## 3. Engine layout (`tools/ios3ds-gl`)

| file | role |
|---|---|
| `api.c` | The ES 1.1 state machine: 177 entry points by slot (`gl_install_dispatch`), objects (textures, buffers, renderbuffers, framebuffers, fences), matrices, `glGet*`, pixel transfer. |
| `draw.c` | Front end: vertex fetch (all array types, VBOs, indices), modelview/projection and texture matrices, 8 lights, colour material, fog, primitive assembly. Points and lines become quads. `glDrawTex*OES` is handled here too. Its output is **clip-space triangles** plus the GC state. |
| `pica_be.c` | The PICA200 `Backend` ([pica200.md](pica200.md)): command lists, GPU-side render targets and textures, read-back. |
| `raster.c` | The software `Backend`: clipping, viewport, face culling, perspective-correct spans, 2 texture units (nearest/linear, repeat/clamp/mirror; full `GL_COMBINE`), alpha test, depth test (16-bit), blending (all factors and equations), colour mask, scissor, fog, polygon offset. |
| `iface.c` | EAGL glue: the 11-entry table, IOSurface-backed renderbuffers and textures, the CA drawable protocol. IOSurface and IOMobileFramebuffer are resolved with `dlsym`. |
| `thunks.S` | 841 12-byte stubs. Each unimplemented slot logs `[gl] unimplemented GL call: slot N` to the console once, then returns 0. |
| `genslots.py` → `gl_slots.h` | Maps API names to slot numbers, read from your dyld cache. |
| `test/` | `gltest` (§5) and the host harness. |

**API coverage.** All 44 `gl*` functions QuartzCore imports are implemented
(checked against QuartzCore's import list). So is the rest of ES 1.1 common
profile, plus `OES_framebuffer_object`, `OES_draw_texture`, `OES_point_size_array`, `OES_mapbuffer`, blend equation/func separate,
`APPLE_fence`, the `APPLE` multisample entry points (single-sampled) and
`EXT_discard_framebuffer` (a no-op). `OES_compressed_paletted_texture` and
`IMG_texture_compression_pvrtc` (all four 2bpp/4bpp RGB/RGBA formats) are
decoded to RGBA words at upload (`pvrtc.c`); 2bpp uses an averaged
low-frequency signal rather than the PowerVR reference interpolation.
`OES_matrix_palette` is implemented: `glCurrentPaletteMatrixOES`,
`glLoadPaletteFromModelViewMatrixOES`, `glMatrixIndexPointerOES` and
`glWeightPointerOES` feed a per-vertex 1..4 matrix blend in object space, in
addition to the modelview matrix. `glGenerateMipmapOES` builds a box-filtered
level chain, but sampling still uses level 0. ES 2.0-only entry points
(shaders, programs, vertex attributes) set `GL_INVALID_OPERATION` instead of
silently returning 0; `EAGLContext initWithAPI:2` is refused before our
engine is reached, so a game's ES2 probe fails and its ES1 fallback works.

**Not done:**
- mipmap sampling: minification uses level 0 even after `glGenerateMipmapOES`;
- stencil: the state is kept, but there is no stencil buffer;
- user clip planes (the state is kept, but nothing is clipped);
- logic op, multisampling, smooth points and lines;
- bit-exact PVRTC 2bpp low-frequency interpolation;
- ES 2.0 (deliberately: ES1 games must keep running).

## 4. Back ends

The rasterizer sits behind `struct Backend` (`engine.h`):

```c
typedef struct Backend {
    const char *name;
    void (*clear)(GC *, RT *, GLbitfield mask);             /* honours scissor and masks */
    void (*triangles)(GC *, RT *, const Vtx *v, int n);     /* n/3 clip-space triangles */
    void (*tex_changed)(GC *, Tex *);                        /* optional: texture image changed */
    void (*tex_deleted)(Tex *);                              /* optional */
    void (*finish)(GC *, RT *);                              /* optional: make rt->color current */
} Backend;
```

- `Vtx` carries the lit colour, texture coordinates after the texture matrix,
  and the fog factor.
- Every per-fragment setting is in the GC: blend, alpha and depth test, texenv
  and combiners, scissor, cull, colour mask, and polygon offset.
- `RT` names the target: its colour words, a 16-bit depth buffer, the pitch, and
  `flip` (GL row 0 is the last memory row).
- `Tex.be_priv`/`be_seq` let a back end cache its own copy of a texture.
- `gl_be_forget(mem)` tells back ends that renderbuffer or texture storage is
  about to be freed.

There are two:

| back end | file | `GL_RENDERER` |
|---|---|---|
| CPU rasterizer (default, and the fallback) | `raster.c` | `ios3ds software rasterizer` |
| PICA200 GPU | `pica_be.c` + `tools/ios3ds-pica` | `ios3ds PICA200` |

`gl_gc_new` asks `gl_pick_backend()`: with `IOS3DS_GL=pica` (or `auto`) in the
process environment and a usable GPU the context draws on the PICA200,
otherwise on the CPU. The GPU back end, the kernel service behind it and the
software GPU model used to test it are described in [pica200.md](pica200.md).

## 5. Test: `ios3ds-gltest`

The test is `tools/ios3ds-gl/test/gltest.c`. It uses only the public API: an
`EAGLContext` through the ObjC runtime, plus `gl*` from OpenGLES.framework.

It also checks the ES 1.1 compatibility paths a 3/4-era game probes:
hand-encoded PVRTC 4bpp/2bpp images, the compressed-format and extension
lists, `OES_matrix_palette` index/weight arrays, `glGenerateMipmapOES`, and a
clean `GL_INVALID_OPERATION` for an ES2-only shader call. The extra checks
run after the two frame grabs, so the compared frames stay the same. On iOS
it also asks EAGL for an ES2 context and verifies the request is refused
before continuing on the ES1 context.

1. It renders the test scene into an offscreen FBO (RGBA8 colour + DEPTH16). The
   scene has:
   - a smooth triangle;
   - a GL_LINEAR checkerboard texture;
   - `glDrawTexiOES`;
   - a 50 % blended quad;
   - a scissored clear;
   - a lit, culled, depth-tested cube in a frustum.
2. It reads back 10 probe pixels and prints `ok`/`FAIL` for each, then
   `PASS`/`FAIL`, to the console.
   It then repeats this on a second context created with `IOS3DS_GL=pica` and
   compares the two frames pixel by pixel (skipped, with a log line, when the
   GPU is not usable).
3. After `delay` seconds it binds the main display's framebuffer IOSurface
   (`IOMobileFramebufferGetLayerDefaultSurface`) as a renderbuffer with
   `attachImage:toCoreSurface:invertedRender:YES`. For `seconds` it then draws the
   animated scene straight onto the screen, which the ARM9 mirrors to the top
   LCD.

The launch daemon runs it with `360 60`: the probe test runs at boot, and the on-screen demo starts at about 425 s guest time, after the home screen is up. To keep only the boot self-test, set both arguments to `0`.

To build and run:

```
make -C tools/ios3ds-gl               # engine bundle + gltest into build/rootfs-add
make -C tools/ios3ds-gl host-test     # same core + scene on the build host (offscreen, PPM)
make -C boot && python3 scripts/mksd.py --image build/sd.img
python3 emu/run.py --time 450 --shots 415,449     # home screen, then the GL scene
mtype -i build/sd.img@@1048576 ::/ios3ds/log.txt | grep -a 'gl'
```

## 6. Status

Verified in QEMU (`-M nintendo3ds`, 450 s boot, gltest args `360 60`):
- `[gl] context created (software rasterizer) (pid 8)`: OpenGLES loads our bundle from disk past the cache copy, and `CreateGC` fills the dispatch tables.
- `gltest: PASS (0 failed)`: the 10 probes and `glGetError` match the host build value for value. This covers smooth shading, a LINEAR texture, DrawTex, blending, scissor, and depth/lighting/culling.
- `[gl] first BindCoreSurface`, then `drawing into the display surface 320x480`: from about 425 s the top LCD shows the animated scene, rotated 90° like everything else on the portrait display. It runs at ~13 fps including a 50 ms sleep per frame, i.e. about 25 ms per 320x480 frame in the emulator.
- In the same boot, the 415 s screenshot shows the SpringBoard home screen as before. With the QuartzCore probe patch, SpringBoard no longer creates a GL context. Without the patch the screen stayed black (§1).
- No `unimplemented GL call` was logged.
- `make host-test` gives the same pixels on the build host.

Screenshots aren't committed (iOS artwork); rerun the commands above to reproduce them.

**Verified only by RE, not exercised yet:** `BindView`/`PresentView` (the CAEAGLLayer path of a UIKit GL app). Texture-from-IOSurface and `SwapNotification` now run in CA's compositor (§7).

**Next:**
- CA compositor without the per-frame read-back of the display surface (§7);
- a UIKit GL app through CAEAGLLayer;
- PVRTC decode and mipmaps;
- the PICA200 back end on real hardware ([pica200.md](pica200.md) §8).

## 7. CoreAnimation's compositor on the engine (and on the PICA200)

SpringBoard is the render server: QuartzCore's `CA::WindowServer` composites
every app's layers into the display surface. Without an EAGL engine it does
that in software on the ARM11; with one it uses `CA::OGL::GLESContext`, i.e.
OpenGL ES 1.1 through our engine, and with the PICA back end the per-pixel
work (blending, texture sampling, transforms) moves to the GPU.

### How CA drives the engine (RE of 8C148's QuartzCore)

- **Display probe** (`M2Display::M2Display`, 0x346de5b8): `eagl_context(2)`,
  then `eagl_context(1)` (= `-[EAGLContext initWithAPI:]`, i.e. our
  `CreateSharegroup`/`CreateGC`). The first that succeeds selects GL
  compositing; API 1 picks the surface format `'L565'`, patched to `'BGRA'`.
- **Display surface as render target** (`GLESContext::make_buffer_current`,
  0x3470d274): `glGenTextures`, `glBindTexture(0x84F5, …)` (0x84F5 =
  GL_TEXTURE_RECTANGLE), NEAREST/CLAMP parameters, then
  `-[EAGLContext attachImage:toCoreSurface:invertedRender:NO]` with target
  0x84F5 = our `BindCoreSurface` (the texture's pixels are the IOSurface),
  then an FBO with that texture as `GL_COLOR_ATTACHMENT0`.
- **GL usage**: the 44 imported `gl*` functions (§3), all implemented. Per
  call site (disassembly of the whole `__text`, imports resolved through
  `__picsymbolstub4` + the indirect symbol table): textures bound with a
  per-image target (2D or 0x84F5), `glTexEnvfv(GL_TEXTURE_ENV_COLOR)` and
  `GL_COMBINE` tables, `glTexImage2D`/`SubImage2D` uploads,
  `glPixelStorei(GL_UNPACK_CLIENT_STORAGE_APPLE)` (only with
  APPLE_client_storage, not advertised), `glGetString(0x8A1E)` after
  `GL_EXTENSIONS` (an Apple-private second list), `glGetIntegerv(0x84F8)`
  (max rectangle texture size), and `glFlush` with FBO 0 bound before the swap.
- **Present**: `SwapNotification` → `IOMobileFramebufferSwapSignal`.

### What the engine needed

| first run's symptom | fix |
|---|---|
| `glBindTexture(0x84F5)` → GL_INVALID_ENUM | 0x84F5 accepted wherever GL_TEXTURE_2D is (bind, parameters, images, enable); a texture last bound as 0x84F5 takes texel coordinates (host test: same pixels as 2D with 0..1) |
| `[gl] Get: unknown pname 0x84f8` | max rectangle size = 1024 |
| `GL error 0x500` in `GetString` → `CoreAnimation: rendering error 500` | `glGetString(0x8A1E)` returns "" |
| screen black except for one icon | `glFlush` did nothing and `glFinish` only synced the *bound* framebuffer; CA flushes with FBO 0 bound. Now `glFlush`/`glFinish`/`SwapNotification` write every GPU target back (`Backend.flush_all`) |
| photo grids empty (Settings > Wallpaper, re-log 62): `[gl] IOSurface pixel format 'L555' not supported` | PhotoLibrary's album rows are IOSurface CGImages in `'L555'` (x1R5G5B5, MusicLibrary's thumbnail formats); `BindCoreSurface` on a texture target converts `'L555'`/`'L565'` surfaces to a 32 bpp copy at bind (CA binds again for a new texture). Render targets stay 32 bpp only |

`IOS3DS_GL_TRACE=1` (set for SpringBoard) logs the first GL error of every
call site as `[gl] trace: GL error 0x… at engine+0x…` (look it up with `nm`).
`IOS3DS_CA_LOG_FRAMES=N` changes the `[gl] CA:` line's interval from 300 CA
swaps to N (test hook, e.g. to see the cost before and after a scattered
surface in a short QEMU run). `IOS3DS_CA_SWAP_DIAG=1` logs every CA swap and
the present / arena copy / read-back that wrote its surface (`[gl] [swapdiag]`),
so a last frame lost after Home can be counted against the mirror's frame count
(re-log NEXT). Both are off by default and add a line per frame when enabled.

### Switching it on, and falling back

SpringBoard's environment (rootfs/recipe.json) has `CA_ENABLE_OGL=1` and
`IOS3DS_CA=1`. In a process with `IOS3DS_CA`, the first `CreateGC` decides
once (`tools/ios3ds-gl/ca.c`, `pica_ca_gate`):
1. boot.cfg `ca_gpu` (and `gpu`) through the GPU service (`PICA_OP_CONFIG`);
2. the PICA200 back end's bring-up and self-check;
3. a CA-like scene (clear, premultiplied textured quad, rotated translucent
   fill) drawn on the CPU rasterizer and on the GPU and compared (≤ 2 % of
   pixels may differ by more than 24, polygon edges).

If any step fails the context is refused: QuartzCore's probe fails and CA
composites in software exactly as before (one `[gl] CA compositor: software
(…)` line). CA on our *CPU* rasterizer is never used, it would be slower than
CA's own software path. A GPU failure later in the run turns the back end
into the CPU rasterizer for the rest of the run (`[gl] pica: … GPU rendering
is OFF`): the screen keeps working, slower. Kill switch: boot.cfg `ca_gpu=0`
(or `gpu=0`).

### Results (QEMU, re-log 46)

- Home screen, Spotlight + keyboard, page swipes, and app launches and exits
  (`emu/soak.py`: PASS) render through the PICA200 model; screenshots against the software
  compositor at the same moments: 2181 of 192000 pixels differ, by at most 1
  (rounding), on still frames; animation frames differ where the two runs
  are at different animation times.
- Cost: see re-log 46 (with the read-back) and 51 (without).

### Hardware run 15: depth, arena, NPOT repeat (re-log 55)

- **GPU memory is bounded by eviction.** Texture copies are dropped least
  recently used when the arena is full and re-uploaded on their next use;
  render targets no longer keep a linear staging buffer. If even that cannot
  satisfy an allocation, the process writes its GPU images back and draws on
  our CPU rasterizer from then on (slower, never black; one line). The
  `[gl] CA:` line reports held memory, free arena and evictions.
- **Depth** is stored reversed (citro3d's convention); a probe at bring-up
  picks the convention that matches the CPU rasterizer (docs/pica200.md §16).
- **The scene check** now includes NPOT layers (a repeated 2D texture and a
  rectangle texture), with a 1 % limit.

### Presenting without the read-back (re-log 51)

Until re-log 51 every CA frame was rendered into an arena target and then
copied back: PPF tiled→linear into arena scratch, `READ` into the IOSurface,
and a per-pixel byte rotation (RGBA8 in the PICA200's byte order A,B,G,R →
iOS BGRA) on the ARM11, ~616 KiB per frame. Now the final frame never passes
through the CPU:

1. **Which targets.** `gl_get_rt` marks a render target `surface` when its
   pixels are an IOSurface (a texture bound with `BindCoreSurface`, or a
   renderbuffer backed by a drawable). If such a target has a multiple-of-8
   width and height, pitch = width and 8-byte alignment, the back end gives it
   a power-of-two GPU colour buffer (run 13: NPOT textures sample garbage)
   plus a w×h "swizzle" buffer.
2. **Swizzle pass.** At sync time (`glFlush`/`glFinish`/`SwapNotification`)
   the back end draws one quad into the swizzle buffer, sampling the target's
   colour buffer as a texture (nearest, clamp, texcoords 0..w/pw, 0..h/ph).
   Three TEV stages build the output: `tex.a·(1,0,0)`, `+ tex.r·(0,1,0)`,
   `+ tex.g·(0,0,1)`, alpha = `tex.b` (or the constant 255 for an opaque
   target). Read back in the PICA200's byte order this is B,G,R,A in memory,
   i.e. iOS BGRA, with no CPU work.
3. **Present.** The engine pre-writes one word per destination page (see
   below), then asks the shim (`PICA_OP_PRESENT`, protocol 3) to PPF-transfer
   the swizzle buffer (tiled RGBA8 → linear RGBA8, row order flipped unless
   the target is flipped) straight to the IOSurface.
4. **Shim checks** (`kernel/shim/pica.c`, `present`): size = w·h·4, 8-byte
   aligned, plain RGBA8 → RGBA8 transfer (no texture copy, no tiled output,
   no crop). Then it walks the *caller's* page tables (TTBR0/TTBCR, L1 section
   or coarse table, L2 small/large page) for every page of the range: each
   must be mapped user read/write (AP = 3, APX = 0), the pages must be
   physically contiguous, and the whole range must lie in FCRAM or VRAM. So
   the GPU writes only memory the caller could write itself. Then the range's
   D-cache lines are cleaned and invalidated by VA (`c7,c14,1`; the caller's
   VAs are live because the sysctl runs in its context) and the transfer runs.
   A refusal is logged once with the descriptors and CP15 state.
5. **Present check.** Until one non-uniform frame has matched, every present
   is followed by the old CPU read-back of the same frame into a scratch
   buffer and a comparison with what the GPU wrote (±2 per channel for TEV
   rounding): the swizzle pass and the transfer are not verified on hardware,
   and the CA scene check only covers the render target, not the presented
   surface. A mismatch logs `[gl] pica: GPU present check FAILED: …`, puts the
   read-back into the surface for that frame and turns presenting off for the
   process; success logs `GPU present check passed` once. Host test: a third
   gltest run with presents that swap R and B (`IOS3DS_HOST_PRESENT=swap`)
   must hit the check and still PASS.
6. **Scattered surfaces and the fallback.** The shim refuses a destination
   that is not one physically contiguous range (`PICA_ERR_NOT_CONTIG`); CA
   makes such a 320x480 surface when an app returns to the home screen. For
   that surface (only) the engine keeps the swizzle pass and runs the same
   PPF transfer into a linear arena buffer, then asks the shim
   (`PICA_OP_SCATTER`) to copy the buffer **page by page** into the scattered
   pages. The shim walks the caller's page tables again (each page must be
   user-writable and in FCRAM), maps it through XNU's linear map, cleans and
   invalidates the caller's lines for that page, copies 4 KiB with the CPU
   (no pixel work) and writes the new lines back. Every other surface keeps
   its direct `PRESENT` (re-log 78/79). Any other `PRESENT` failure still
   stops presenting for the rest of the process, with the CPU read-back;
   then one read-back staging buffer stays allocated (re-log 75).
   `IOS3DS_HOST_PRESENT=0` (host shim) and `IOS3DS_GL_SCATTER=1` (PRESENT's
   test flag, refused for FCRAM destinations by the real shim too, so the
   QEMU display in VRAM keeps its direct present) force the scattered path
   in tests; `IOS3DS_GL_NO_PRESENT=1` forces the read-back;
   `IOS3DS_GL_PRESENT_ALL=1` presents every eligible target (host test).

**XNU's modified-bit emulation.** The first attempt was refused: `not
user-writable (… L1 257ca001 L2 18000823 … TTBCR 2 DACR 00000001)`. L2
0x18000823 is a small page at PA 0x18000000 with AP = 2 (user read-only):
XNU's ARM pmap maps writable pages read-only until the first write, takes the
permission fault, sets the page's modified bit and only then maps it AP = 3.
CA had not written the surface since it was mapped (the GPU had). The engine
now writes each destination page once (`pg[k] = pg[k]`, one word per 4 KiB
and the last word) right before `PRESENT`; the walk then sees AP = 3.

**What CA's display surface is.** The log of the first successful present:
`user VA 0x00325000 = PA 0x18000000 (VRAM, 600 KiB)` — the IOSurface CA
composites into is AppleM2CLCD's scan-out framebuffer at the start of `/vram`,
physically contiguous. The GPU writes the picture the LCD path (ARM9 copy or
GPU mirror) reads, so a frame costs one P3D quad and one PPF transfer.

Results (QEMU, re-log 51): CA frames have 0 read-backs; render thread CPU per
frame under `--icount 2` dropped from 19–27 ms to about 8 ms; screenshots
match the software compositor except for the status-bar clock and moments
where the runs are at different animation times.

## 8. MBX2D: the messages in every hardware log

The New 2DS XL log (and every earlier run) has:

```
Iterator had no matching services
Failed to open connection to MBX
Failed to initialized MBX2D driver (1).
```

The dyld cache was searched with `hfsplus cat` on `build/staging/rootfs.hfs`
and the image table at file offset 0xa8 (362 `dyld_cache_image_info` entries,
32 bytes each, `pathFileOffset` at +24; VA = file offset + 0x30000000). This
attributes the messages precisely:

| string | file offset / VA | image |
|---|---|---|
| `Iterator had no matching services`, `AppleMBXDevice`, `Failed to open connection to MBX` | 0x770c68 / 0x30770c68 and nearby | `/System/Library/PrivateFrameworks/MBX2D.framework/MBX2D` (base 0x3076a000) |
| `Failed to initialized MBX2D driver (%d).`, `CA_ENABLE_MBX2D`, `LK_ENABLE_MBX2D`, `CA_ENABLE_OGL`, `MBX2D_PAGE_FLIP` | 0x47bfe0 / 0x347bfe0 and nearby | `/System/Library/Frameworks/QuartzCore.framework/QuartzCore` (base 0x346dc000) |

So MBX2D itself prints the first two when its `IOServiceMatching("AppleMBXDevice")`
returns nothing — the same missing service OpenGLES sees before our one-file
string patch (§1) — and **QuartzCore** prints the third when its
`CA::RenderMBX2D` 2D blit renderer fails to initialise. That renderer is the
accelerated blit path CoreAnimation can pick for layer contents, images and
video frames; it is not the GL compositor, which uses our engine instead.
MBX2D's own strings mention `YUV420 texturing ... mbx3DQuadCopy*` and surface
mapping, i.e. CoreVideo/IOSurface-backed video frames are a likely user,
besides QuartzCore's layer blits.
On hardware the failure is only logged and SpringBoard carries on, but a game
that reaches the MBX2D path can treat the failed driver as fatal.

Fix, in two parts (both in `rootfs/recipe.json`):
1. SpringBoard's `EnvironmentVariables` set `CA_ENABLE_MBX2D=0` and
   `LK_ENABLE_MBX2D=0`, so a QuartzCore version that honours them skips the
   probe. `CA_ENABLE_OGL=1`, `MBX2D_PAGE_FLIP=0` and `IOS3DS_CA=1` are
   unchanged.
2. A QEMU boot with those variables set still printed both lines: this
   8C148 QuartzCore attempts the probe regardless (the GL compositor is
   selected by our own `IOS3DS_CA` gate, not by `CA_ENABLE_OGL`). Rather than
   fake an `AppleMBXDevice` user client the
   cache's two format strings are patched to empty
   (`0x7707cc` in MBX2D, `0x47bb470` in QuartzCore, verified against the
   bytes from the user's own cache). The init call still fails and
   QuartzCore still falls back to the working GL/software renderer; only
   the alarming console lines disappear. A future iOS build with a real
   MBX2D user client does not need the patches.

`tests/test_mbx2d.py` checks the recipe. Only the native MBX2D framework's
own strings were read from the dyld cache; no Apple binary or private file
is shipped.
