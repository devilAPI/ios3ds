# Camera: the 3DS cameras in iOS 4.2.1

Status: **hardware run 2026-11-24 failed before any sensor work: CAM_OP_START
returned status 3 (`CAM_ERR_NO_MEM`) and CAM_OP_FRAME status 6
(`CAM_ERR_NOT_RUNNING`), so no camera request ever reached the ARM9 (section 7.1).
Frame buffers now come from the already-wired GPU arena; the next card run must
confirm it.** In QEMU Camera.app shows a live preview and takes photos through the whole
path (QEMU camera model → ARM9 sensor set-up → ARM11 FIFO drain → mediaserverd
plug-in → Celestial → Camera.app → Camera Roll). Video recording is not
offered (section 8).

```
 Camera.app / UIImagePickerController / AVFoundation (in the app)
      | FigRecorderRemote (Mach IPC "com.apple.coremedia.recorder")
 mediaserverd: Celestial FigRecorder -> FigLiveSource -> FigCaptureDevice/Stream
      |   figCreateVideoCaptureDevice loads /System/Library/MediaCapture/vcd.mediacapture
      |   (CFPreference "vcd" of com.apple.coremedia = true) before any Apple camera
 vcd.mediacapture (tools/ios3ds-camera/vcd, ours)          YUV422 -> BGRA, CMSampleBuffers
      | sysctl ios3ds_cam (CAM_OP_START / FRAME / STOP)       in CMBufferQueues
 kernel shim, GPU part (kernel/shim/cam.c)                   3 frame buffers, IOMallocContiguous
      | every 1 ms while a camera runs (audio service wake-ups): drain the CAM FIFO window
      | camera mailbox (shim + 0x7E00)
 ARM9 monitor (boot/arm9/camera.c): CFG11_CAMERA_CNT clock, Aptina MT9V113 set-up over I2C
 3DS: inner camera (I2C 0x10161000 dev 0x7A) and outer left camera (I2C 0x10144000 dev 0x78)
      -> CAM0 0x10120000 / CAM1 0x10121000, FIFO windows 0x10320000 / 0x10321000 (ARM11 only)
```

QEMU evidence (`--icount 2`, the camera model's pattern): live preview of the
outer camera, a photo, switch to the inner camera (two marker blocks), a photo,
Photos shows "Camera Roll (2)"; 144 frames per 10 s (14.4 fps), 0 FIFO
overruns, ARM9 sensor bring-up 5 ms per camera. Screenshots and the `[cam]`
log: `build/camera-evidence/` (not committed).

iOS sees two cameras: the 3DS **outer left** camera is the "back" camera
(`PortTypeBack`), the **inner** camera the "front" one (`PortTypeFront`, the
switch button in Camera.app). The outer right camera is kept in standby (it
shares CAM0's parallel bus with the inner one).

## 1. What iOS 4.2.1 needs (RE, re-log section 65)

| Fact | Where |
|---|---|
| Camera.app (`/Applications/Camera.app`, in the iPod2,1 rootfs) is tagged `SBAppTags = (still-camera)`; SpringBoard hides apps whose tags are not capabilities of the device | Camera.app Info.plist |
| The device capabilities come from `SpringBoard.app/N72AP.plist` `capabilities` (no camera keys on the iPod touch 2G); the iPod touch 4G's N81AP.plist has `still-camera`, `front-facing-camera`, `video-camera`, `hd-video-capture` | both rootfs images |
| Capture runs in **mediaserverd**: the app talks to `FigRecorderRemote`; the recorder, live source and capture devices are in Celestial inside mediaserverd (runs as `mobile`) | Celestial symbols, mediaserverd launchd plist |
| `figCreateVideoCaptureDevice` (Celestial 0x32e01520): first, if `CFPreferencesGetAppBooleanValue("vcd", "com.apple.coremedia")`, `dlopen("/System/Library/MediaCapture/vcd.mediacapture")` + `dlsym("FigVirtualCameraDeviceCreate")` (`FigLoadAspenCaptureDevice` 0x32e0133c); then IOKit `AppleH4CamIn`/`H3`/`H2`/`H1` → `H4ISP`...`H1ISP.mediacapture`. A loaded virtual device wins; create = `(allocator, NULL options, &device)` | disassembly |
| The capture device / stream interface is Apple's FigBaseObject: vtable `{0, base class, derived class}`, base class `{version 1, storage size, equal, invalidate, finalize, copyDebugDescription, copyProperty, setProperty}`, stream class `{1, start, stop}`; created with `FigDerivedObjectCreate(alloc, vtable, FigCapture{Device,Stream}GetClassID(), &obj)` | H3ISP.mediacapture (in the shared cache) 0x3e07e5b8.. |
| Device properties used: `StreamArray`, `Clock` (host clock), `SupportedPropertiesDictionary` | H3ISP device CopyProperty 0x3038c52c, Celestial |
| Stream properties used: `PortType` (`PortTypeBack`/`PortTypeFront`), `SupportedFormatsArray` (dicts of FormatDescription, VideoMaxFrameRate, VideoScaleFactor, VideoIsBinned), `FormatDescription`, `FormatIndex`, `Min/MaximumFrameRate`, `BufferQueue` and `StillImageBufferQueue` (CMBufferQueues **created by the plug-in**, Celestial installs triggers on them), `StillImageCaptureNow` (the next frame also goes to the still queue), `BufferAllocator`; notifications `StreamStarted`/`StreamStopped` on `FigCaptureDeviceGetNotificationCenter()` | H3ISP stream CopyProperty 0x30385a54, Celestial `livesource_updateSourceVideoQueues` 0x32e250e0 |
| Frames: `CMSampleBufferCreateForImageBuffer` of IOSurface-backed CVPixelBuffers (`kIOSurfaceIsGlobal`), PTS = host time in ns | H3ISP `MyH3ISPFrameReceivedProc` 0x3038afd0 |
| Four constants are private to Celestial (bound into Apple's plug-ins by the cache, not visible to `dlsym`): `PortType`, `PortTypeBack`, `PortTypeFront`, `FrameDroppedFromQueue`; they are compared with `CFEqual`, so equal CFStrings work | cache symbol tables |
| Modes and formats come from `Celestial.framework/<model>/AVCapture.plist`; N72's only has audio recording. Schema taken from the iPod touch 4G (N81) file: `AVCaptureMode_PhotoCapture`, `AVCaptureMode_FrontFacingPhotoCapture` with `LiveSourceOptions` `{VideoPort, Sensor, Capture, Preview (Width, Height, PixelFormatType), Min/MaxFrameRate, SensorOrientation, PreviewVideoMirror, FastSwitchCompanion}` | N81 rootfs (public firmware, looked at only) |
| CoreAnimation hands 4:2:0 IOSurfaces of a video layer to an AppleM2CLCD overlay window (`CADisplayIOMFB swap_queue`), which needs physically contiguous memory and is not shown by ios3ds: 420f previews were black (`AppleM2CLCD, swap_queue error: e00002c8`). BGRA previews are composited like any other layer | QEMU |
| Photos: `livesource_workerThread` → `FigAspenCreateJPEGFromSbuf` → no AppleJPEG service on N72 → `softwareJPEGEncode` (ImageIO `CGImageDestination`) | Celestial 0x32e25d40, 0x32e03b28 |

## 2. Choice of bridge

Options considered:

1. **A fake IOKit camera service** (`AppleH3CamIn` user client) under Apple's
   H3ISP.mediacapture: H3ISP drives an ISP with firmware upload, I2C, 3A and a
   frame receiver through dozens of user-client selectors. Emulating that in
   the shim is far more code than the camera itself, and none of it matches
   3DS hardware.
2. **Interposing CoreMedia/Celestial in mediaserverd** (like the Apple80211
   interposer): the capture device is created inside Celestial from a
   function table, not through calls an interposer could catch cleanly.
3. **Celestial's own virtual-camera slot** (`vcd.mediacapture`, chosen):
   Apple left a plug-in hook that is loaded before any hardware camera when one
   preference is set. The plug-in implements the same FigCaptureDevice
   interface as Apple's plug-ins, so everything above it (Celestial's live
   source, preview, still pipeline, JPEG, AVFoundation, UIImagePickerController,
   Camera.app) is Apple's unmodified code. No byte patch is needed anywhere.

Kernel/hardware side: the CAM data FIFO is ARM11-only, so the ARM11 must move
the data. The 3DS's own driver uses CDMA (peripheral 2/3); ios3ds drains the
FIFO with the CPU from the shim's interrupt path, every 1 ms while a camera
runs, the way the microphone FIFO is drained (docs/audio.md section 7). That
needs no DMA microcode and the QEMU model stays simple; the cost is bounded by
the frame size (section 6). The I2C buses and the camera clock are the ARM9's
(it already owns I2C for the MCU), so the sensor set-up is done there.

## 3. Pieces

| Piece | File |
|---|---|
| Plug-in `FigVirtualCameraDeviceCreate`: device with two streams, frame thread per running stream (sysctl → YUYV → BGRA IOSurface pool buffer → CMSampleBuffer → queues), stills, pattern mode (`ios3dsPattern` preference) | `tools/ios3ds-camera/vcd/vcd.c` → `/System/Library/MediaCapture/vcd.mediacapture` |
| Capabilities `still-camera`, `front-facing-camera` | `rootfs/recipe.json` `plist_merge` (new: nested-dictionary merge in `scripts/mkrootfs.py`) |
| Modes/formats: photo back + front (each the other's `FastSwitchCompanion`), 320x240 `BGRA`, 15 fps, `SensorOrientation 90`, no preview mirroring | `rootfs/files/camera/AVCapture.plist` → `Celestial.framework/N72/AVCapture.plist` (our own file; it replaces N72's audio-only one and keeps its audio mode) |
| `vcd = true` | `rootfs/files/camera/com.apple.coremedia.plist` → `/var/mobile/Library/Preferences/` |
| Shared contracts: sysctl requests, mailbox, register map | `boot/common/cam_proto.h` |
| Test pattern + YUV conversions (plug-in, QEMU model, host test) | `boot/common/cam_pattern.h` |
| Shim: sysctl `ios3ds_cam`, frame buffers (GPU arena first, `IOMallocContiguous` fallback), FIFO drain, overrun recovery | `kernel/shim/cam.c` (GPU part, `link.ld` `*cam*.o`), hooks in `gpu_glue.c`, `audio.c`; arena block `kernel/shim/pica.c` |
| ARM9: camera clock, I2C (16-bit register index added to `i2c.c`), Aptina set-up, standby | `boot/arm9/camera.c` |
| boot.cfg `camera=1` (default) / `camera=0` | `scripts/mkboot.py`, `boot/arm9/main.c` |
| QEMU model | `emu/qemu/hw/arm/ctr_cam.c` |
| Host test | `tools/ios3ds-camera/tests/test_cam.c`, `tests/test_camera.py` |

### 3.1 Sensor set-up (ARM9)
First the CAM blocks, in the order the 3DS CAMERA sysmodule uses
(re-log 74): CAM0's CAM_CNT bit 4 (the reset of both blocks; it does not
exist on CAM1) held low while the camera clock (CFG11_CAMERA_CNT bit 0, the
module's `pdn:c` call) comes on, released with the clock running, clock off;
then on both blocks transfer off, overrun cleared, 256-byte transfer unit,
trimming off, CAM_CNT bits 8-9 = 2, bit 10 on, bit 11 off, bit 14 on; then
the clock on for good. Then the sensor:
GBATEK's DSi Aptina code list (reset, wakeup, PLL as on the DSi, YUV output,
sizes, AE window, sequencer refresh; the cam module's base script is the
same list plus 1 ms/10 ms waits, MCU 0xA111 = 0x0A and flicker-detection MCU
values, which are written too) with output size A = 320x240, then
"activate" (standby off, parallel output on). The read mode is GBATEK's
(0x24 for the 0x7A camera, 0x25 = x-flip for the others); which of them
gives a true or a mirrored picture is unverified (section 7). Each wait gives up after
100 ms; a camera that does not answer or does not come up reports
`CAM_ST_FAILED` and iOS shows a black preview. Switching cameras puts the
previous sensor into standby first.

### 3.2 Data (shim)
- START maps the CAM registers (0x10100000 section, shared with the audio
  code) and the FIFO windows (0x10300000 → VA 0xBFB00000), allocates three
  150 KiB frame buffers once, and posts the request to the ARM9.
- Once the ARM9 reports the sensor streaming, the interrupt path sets
  the set-up variant's CAM_CNT with bit 15 (variant 0 = the cam module's
  0xC610; bit 4 must stay set on CAM0, stopping clears bit 15 only; the transfer
  starts at a frame start) and from then on, every 1 ms, reads `CAM_STAT`'s
  fill count and copies that many words from the window into the frame being
  filled. Window reads always continue at the next address (a word is only
  consumed when the address changes).
- An overrun (stat bit 15) costs the frame: transfer off, overrun cleared,
  transfer on (next frame start), the partial frame dropped.
- FRAME copies the newest complete frame out (`copyout`), never one the
  interrupt is writing.
- The worst interval between two drain services (in shim ticks) is tracked and
  published per session (`gap_max`, the stats line prints `drain gap max`).
  The FIFO holds about 5.5 ms of the stream: anything beyond that loses
  words **even when the overrun bit stays clear** - run 29 saw a FIFO parked
  at the fill cap 0x500 with CAM_STAT bit 15 still 0 (section 7).
- **Frame alignment** (run 30's dump: one frame shifted by 16 pixels, its
  right edge wrapped to the left): the shim cannot see the sensor's VSYNC,
  but the vertical blank shows as two or more services in a row with an
  empty FIFO (a line arrives every ~0.25 ms, the blank lasts milliseconds).
  A frame counts only if its 38400th word is the last before such a blank:
  once a blank has been seen, a complete frame waits for it and is published
  there if no word came in between; a partial frame at a blank, or words
  after a complete one, mean words were lost or doubled, and that frame is
  dropped (`resyncs`, printed as "frames dropped at the blank" in the 10 s
  line). The next frame starts aligned. A pause counts as the blank only
  within 256 words of a frame boundary (QEMU pauses mid-frame). Host test: a lossy FIFO model (8
  words vanish every 3rd frame, 4 come twice every 5th) delivers only whole
  frames.
- STOP cleans all three frame buffers; the ARM9 then copies the two newest
  complete frames out of them (section 4.1).

### 3.3 Status numbers in `[cam] stream N: ... status X`
These are the **sysctl request status** (`CAM_ERR_*` in `boot/common/cam_proto.h`),
not the ARM9's mailbox `CAM_ST_*` state, even where the numbers coincide:

| Line | Status | Meaning |
|---|---|---|
| `start camera ... -> status 3` | `CAM_ERR_NO_MEM` | the shim could not get the 3 x 150 KiB frame buffers; it returned before sending the mailbox request, so the ARM9 never logs a start and the camera clock/I2C/Aptina paths are untouched |
| `frame request failed, status 6` | `CAM_ERR_NOT_RUNNING` | FRAME arrived with no successful START in this boot (`C.running == 0`): a consequence of the failed START, not a separate sensor fault |
| `kernel state 3` in `[cam] device: ...` | `CAM_ST_FAILED` | different scale: the **ARM9** gave up on I2C/sensor init; it always logs `[cam] ... init failed (step N)` then. This did not happen in run 2026-11-24. |

`CAM_ERR_AGAIN` (4) is normal while waiting for the first frame. A status 3 at
START also explains `0 frames` and `monitor at stop: 0 CAM_STAT samples`: nothing
was ever started.

## 4. Preview, photos, orientation
- Preview: the plug-in delivers BGRA frames (YUYV converted in mediaserverd),
  which CoreAnimation composites like any layer. 4:2:0 frames go to a CLCD
  overlay window instead and stay black on ios3ds (section 1).
- **The 1-2 s loop (run 30):** the frames come from a CVPixelBuffer pool, so
  the same IOSurfaces are written again and again. CA binds each surface to
  a texture once and keeps it (on Apple's GPU the texture *is* the surface
  memory); ios3ds' PICA200 back end (`tools/ios3ds-gl/pica_be.c`) keeps a
  tiled copy in GPU memory and re-uploaded it only when GL said the image
  changed. Run 30's `[gl] CA` line: 0.0 texture uploads per frame while the
  camera ran at 15 fps, so the preview cycled through the pool's first
  frames. Now a surface-backed texture is re-uploaded when the IOSurface's
  seed (`IOSurfaceGetSeed`, bumped by every write unlock) or a 64-texel sample
  of it changed. Video and any other producer of recycled surfaces benefit
  too.
- Review image: Camera.app only saves a photo after Celestial has made its
  review image. Celestial uses the sample's `ScaledPreviewPixelBuffer`
  attachment as it is when there is one; otherwise it scales the frame to
  512x384 with VTPixelTransferSession, which on this device refuses to scale
  up (-12905) and the photo is silently dropped. The plug-in attaches each
  frame's own pixel buffer.
- Photos: every still request is logged (`still requested (frame N)`, then
  `still image #n` or why not). Run 30 had none: the request never reached
  the plug-in (re-log 76); QEMU takes photos upright and face down.
- Photos: on `StillImageCaptureNow` the next frame goes to the still queue at
  640x480, interpolated from the 320x240 frame (`cam_bgra_upscale2x`);
  Celestial JPEG-encodes it in software (no AppleJPEG service on N72) and
  Camera.app saves it into the Camera Roll (`/var/mobile/Media/DCIM`). The
  detail is that of the 320x240 sensor output.
- `SensorOrientation 90`: the 3DS sensor delivers landscape frames; iOS's
  portrait is the console turned 90°, like an iPhone's landscape sensor.
- Front/back switch: Celestial creates the device with the mode's stream and
  its `FastSwitchCompanion` stream and swaps them on the switch button
  (`livesource_swapVideoStreams`). The front preview is **not mirrored**:
  with `PreviewVideoMirror` Celestial needs a mirroring preview scaler
  (`livesource_setupVideoScalerForPreview`), which fails without the hardware
  scaler (-12914), and the switch ended in `PreviewFailed` (re-log 65).
  Without a companion Camera.app's switch button does nothing.

### 4.1 Frame dumps (build 44+)
Run 29's open question is word-level: the preview in Camera.app shows one
"first frame" that wiggles, while every counter says the data flows
(section 7). The pixel truth decides between the suspects, so at every camera
stop the ARM9 copies the shim's two newest complete 320x240 YUV422 frames
straight out of its buffers to `sd:/ios3ds/CAM<seq>.RAW` (one file per camera
session, ~300 KiB; `[cam] dumped frames seq ...` in the log):

```
header (u32 LE): 0x44554D43 ('CAMD'), 1, 320, 240, cam, port, seq1, seq2
frame 1 (newest), frame 2 - each 320*240*2 bytes, YUV422, no padding
```

On the PC: `python3 tools/ios3ds-camera/analyze_dump.py CAM00001234.RAW`
decodes both frames to BMPs (in both YUYV and UYVY byte orders, which settles
the colour question by eye) and prints

- the mean luma difference at zero shift: ~0 = the picture never changes (the
  kernel delivered the same scene),
- the best (dx, dy) alignment between the two frames: a constant nonzero
  drift between neighbouring frames = the 38400-word frame window slips
  through the stream (words lost), the signature of a picture that wanders,
- rows whose best dx disagrees with the global one: words lost mid-frame
  (horizontal shear).

The shim's `drain gap max` (worst service interval, ms) and the ARM9's
independent `[cam] monitor` line (its own `CAM_STAT` sampling (each monitor-loop turn, so never starved by XNU): max
fill, near-full count) say whether the drain was ever starved long enough to
lose words. Together they tell lost-words (FIFO full behind masked
interrupts; the fix is a CDMA drain, section 6) apart from a display-side
problem, without guessing.

## 5. QEMU
`emu/qemu/hw/arm/ctr_cam.c`: three MT9V113 register models on I2C (answer only
while CFG11_CAMERA_CNT bit 0 is set), the two CAM blocks, their FIFO windows
(pop on address change, 0x500 units, overrun), and a moving test pattern at 15
fps on the virtual clock (`cam_pattern.h`: scrolling colour bars, a bouncing
square, one black block for the outer camera and two for the inner one, and
a grey 64x48 frame-counter patch in the middle, Y = 40 + frame mod 160).
`tools/ios3ds-camera/tests/preview_counter.py` reads that patch off
screenshots of a running session for 10 s: fresh frames make it climb, a
preview looping over old frames steps back (FAIL), a frozen one stays put.
`emu/session.py set mcu-orient 5` lays the console face down (4 = face up).
`IOS3DS_CAM_LOG=1` logs register writes and transfers.

QEMU's default timing (virtual clock = wall clock while the guest runs about
3x slower than a 3DS) overruns the FIFO on every frame: use `--icount 2`
(real-hardware instruction timing), where the 1 ms drain keeps up with 0
overruns. With iOS 2.2.1's software compositor the guest is slower still, so
`emu/hwtest_check.py` uses `--icount 2` by default for its camera checks;
`--icount ""` restores the old wall-clock timing (re-log 153). The plug-in's
pattern mode (`ios3dsPattern` = true in
com.apple.coremedia's preferences) bypasses the kernel path entirely.

## 6. Cost and limits
- 320x240 YUV422 is 150 KiB per frame: at 15 fps 288,000 FIFO words a second
  through the ARM11, a 150 KiB `copyout` and a YUYV → BGRA conversion per
  frame in mediaserverd.
- The FIFO holds about 5.5 ms of 320x240 video (run 29: 10 KiB against
  ~1.8 MB/s of sensor data). While XNU masks interrupts longer
  than that (md0 transfers do, docs/architecture.md) frames are lost (counted
  as overruns, logged when the camera stops).
- Photos are 320x240. VGA would need 4x the FIFO bandwidth; with the CPU drain
  that is only reasonable at a few frames per second (or with CDMA).

## 7. Hardware: first run, diagnostics, what the log shows
**Run 23 (Old 3DS, re-log 68):** all three sensors answer (`chip id 2280`),
the outer left one comes up in ~135 ms, but `0 frames, 0 FIFO overruns`
every 10 s: no word ever reached the CAM1 FIFO. A third start timed out in
the sequencer refresh (`init failed (step 5)`). The shim's FIFO drain itself
is not in question yet: without overruns the FIFO never filled at all.

**From the CAMERA sysmodule (re-log 74):** what run 25 lacked is the block
reset with the clock running (CAM0 bit 4 had been set by whatever ran
before, with the clock off), bit 14 on both blocks, a transfer unit of whole
lines, and a stop that leaves bit 4 alone (the shim wrote CAM_CNT = 0, which
holds both blocks in reset). All four are now as the module does them;
variant 0 is the module's set-up (0xC610, 2560-byte unit). The next log
should show `after the cam module's block reset/init` with CAM0 `4610`,
CAM1 `4600`, then data in variant 0.

**Run 25 (re-log 70):** with these diagnostics: the sensor reports streaming
(SEQ_STATE 3, parallel output on, awake), yet neither block ever sees sync or
data in any variant; CFG11 bit 1 does not stick; CAM_CNT bit 4 sticks on CAM0
but not on CAM1. Now: variants with the DSi's CAM_CNT init bits (8-10) and a
**boot self-test** (`cam_test=1` in boot.cfg, before XNU; until build 60 it ran with `diag=1`): every sensor x every block set-up
(and two sensor-side variants), one `[cam-test]` line each, `<-- DATA` where
anything arrives. Issue #1 follow-up (re-log 145): after the self-test the
ARM9 now soft-resets every sensor, power-cycles the CAM blocks and probes
again, so iOS starts as after a `cam_test=0` boot. The reset has no console
confirmation yet; on a console look for `[cam-test] sensors reset ...` and
check Camera.app (see [real-hardware.md](real-hardware.md) §5).

**Run 29 (build 43, first hardware data):** the build-43 fixes work. The
self-test: **all 36 set-ups `<-- DATA`** (each sensor on its mapped block -
outer left on CAM1, inner and outer right on CAM0). At runtime every session
streams: ~**12 frames/s** (450 FIFO words/ms, words = frames x 38400 exactly),
**0 FIFO overruns**, the plug-in delivers to Celestial and Celestial dequeues
everything (0 queue drops). But Camera.app's preview shows one "first frame"
whose content wiggles around - and one 5-s diag caught the CAM1 FIFO **parked
at the fill cap (1280 units) with the overrun bit still clear**, so a full
FIFO does not necessarily flag anything: drain starvation silently loses
words, which would displace the frame window per incident exactly that way.
So the capture stays officially "artifacts" (README) until the build-44 frame
dumps (section 4.1) say whether the delivered frames drift.

What the code does about it (all logged):
- **Both CAM blocks are watched** until data appears: if the left camera's
  data arrives on CAM0 instead of CAM1, the shim moves there
  (`CAM0`/`CAM1` in the stats line).
- **Set-up variants**, 2 s each, cycled until data appears
  (`CAM_VAR_CNT_LIST`/`CAM_VAR_CLR_LIST` in `boot/common/cam_proto.h`): the
  cam module's 0xC610 with +6 = 9 (4 lines of 320 pixels) first, then
  0x8010; 0x8610 (the DSi's init bits 8-9 = 2, 10 = 1); 0x8E10 (+ bit 11);
  0x8610 with +6 = 0x22 (GBATEK suspects +6 is the DSi's CAM_MCNT, "on" =
  0x22); 0xC610 + DRQ with +6 = 0x22.
- `CFG11_CAMERA_CNT` = 3, read back (run 25: only bit 0 sticks).
- Sensor waits 500 ms instead of 100 ms; the cameras are probed at boot (iOS
  saw "present 0x0" until the first start).

`[cam-diag]` lines in `sd:/ios3ds/log.txt`, in this order:
1. `[cam-diag] boot: CFG11_CAMERA_CNT .. CAM0 cnt .. stat .. +6 .. CAM1 ...`
   and the three `chip id` lines at boot.
2. `[cam-diag] CFG11_CAMERA_CNT xx -> yy`: whether bit 1 sticks.
3. After `streaming ... into CAM1`: the sensor registers (`STANDBY` bit 14 = in
   standby, should be 0; `RESET_MISC` bit 9 = parallel output, should be 1;
   `SEQ_STATE`: 3 = preview/streaming on the MT9V113 family; `OUT_W_A`/`OUT_H_A`
   = 0140/00f0).
4. At 0.2 s, 1 s, 5 s: both CAM blocks again (CAM_CNT as the shim set it), and
   per block over 50 ms: how often CAM_CNT bits 0/1 changed (GBATEK: they
   toggle during a transfer = the block sees sync from the sensor) and how
   often the FIFO fill changed.
5. Every 10 s: `shim: N services, M with data, stat ..., drain gap max X ms,
   CAM_CNT ..., variant V, CAMp[, still probing: no data yet], W words`.
   `services` = 0 means the interrupt-time drain never ran for the camera
   (a shim problem, not the hardware's). `drain gap max` > ~5 ms (the FIFO's
   worth of stream) means words were lost even if `overruns` stayed 0.
6. `[cam] monitor 10 s / at stop`: the ARM9's own `CAM_STAT` sampling (each monitor-loop turn, so it cannot be starved by XNU) -
   max FIFO fill and near-full count. This observer cannot be starved by
   XNU's interrupt masking, unlike the shim's.
7. At stop: `[cam] dumped frames seq ... to ios3ds/CAM....RAW` (section 4.1).

How to read it:
- Bits 0/1 toggle and the fill changes on one block → data arrives; the
  variant/port in the stats line is the working set-up (make it the default).
- No toggles on either block, sensor reports streaming (`STANDBY` 0, parallel
  bit set) → the CAM block is not clocked or not enabled: look at
  `CFG11_CAMERA_CNT` and the CAM_CNT readback (bits that did not stick).
- Sensor in standby / `SEQ_STATE` not 3 → the DSi code list does not bring the
  3DS sensor to streaming; the sensor side needs the official sequence.

Other unverified points: the colour byte order (GBATEK: Y0 U Y1 V), the
picture's orientation/mirroring (`SensorOrientation`, read mode 0x2717/0x272D
bit 0 = x-flip), and whether a 1 ms CPU drain keeps up on the console (many
overruns would mean CDMA is needed). `camera=0` in boot.cfg keeps the kernel
away from the cameras (the plug-in then reports no device).

### 7.1 Hardware run 2026-11-24 (Old 3DS): out of frame-buffer memory
Evidence: `issue-reports/run-2026-11-24.txt` (git-excluded, quoted here only in
short excerpts). The card identified itself as
`m1-0.4.0-21-g5800f1e-dirty-20261122.1622`, i.e. build 21 after the 0.4.0 tag
that was confirmed working on a New 2DS XL - **not** the `edf6444` sources in
this worktree. That matters for the history bisect but not for the failure:
the camera sources are byte-for-byte identical from 0.4.0 (`6d963b6`) through
`5800f1e`; what changed between the last working Old-3DS run 29 (build 43) and
this run was the surrounding memory footprint (TLSFix/Substrate, Music library,
GPS daemon, larger rootfs and GPU arena churn), not the camera driver.

- The boot `[cam-diag]` lines are identical to run 29's: `CFG11_CAMERA_CNT 00 ->
  01`, CAM0 `cnt 00004610`, CAM1 `cnt 00004600`, and all three chips answer
  `chip id 2280`. `cameras present 0x3` in the plug-in's INFO call confirms the
  ARM9 probe. So the CAM clock, CAM0 bit-4 reset, I2C buses and Aptina IDs are
  fine on this console; none of those paths was reached again at START.
- The plug-in logs `start camera outer -> status 3` then
  `frame request failed, status 6`, stop after `0 frames`, and
  `[cam] monitor at stop: 0 CAM_STAT samples`. There is no `[cam] start outer
  camera ...`, no `[cam] iOS wants ...` and no `[cam-diag]` start dump. Those
  lines only exist after `op_start()` gets past allocation: status 3 is
  `CAM_ERR_NO_MEM` from `IOMallocContiguous(CAM_FRAME_BYTES, 32, &pa)`, before
  the mailbox request. Run 29's log has `[cam] start outer camera ...` followed
  by `status 0`. This is the exact boundary of the regression.
- On the Old 3DS XNU has 128 MiB; the GPU arena already wires 10 MiB now (12 MiB
  before the memory pass, re-log 112; the camera's 450 KiB block is still
  reserved inside it) and the wired/contiguous heap is much tighter than on the
  New 2DS XL's 256 MiB. A new
  3 x 150 KiB *physically contiguous, wired* allocation at Camera.app start can
  fail there even though the boot-time arena allocation succeeded. QEMU's
  `--icount 2` model did not reproduce the failure (IOMallocContiguous succeeds
  there), so the fix had to be proven with host tests and diagnostics.

**Fix** (`kernel/shim/cam.c`, `kernel/shim/pica.c`, `tools/ios3ds-camera/vcd/vcd.c`):
at GPU bring-up (once, before GL requests can race the driver's block table)
reserve one 450 KiB block for the three frames from a full-size arena
(`pica_drv_alloc` under private owner `0xFFFFFFFE`, so a GL client's `free_all`
cannot reclaim it; arenas under 6 MiB are left to the display mirror). The
shim derives the frame VAs from the arena's VA/PA at START and falls back to
`IOMallocContiguous` when no block was reserved (`gpu=0`, GPU bring-up failed,
or a small arena). On failure, give back the frames already
allocated (the old code leaked them, making every retry worse), log
`[cam] NO_MEM ...`, and let the plug-in retry START four times at 200 ms. The
arena block is kept across camera sessions, like the IOKit buffers before it.

**Host proof:** `tools/ios3ds-camera/tests/test_cam.c` models both allocation
sources: arena first (0 IOKit calls), IOKit fallback when the arena is off, a
forced mid-sequence failure frees the first frame and a retry succeeds;
`tools/ios3ds-pica/tests/test_pica.c` reserves 450 KiB under the private owner
and checks that GL churn/free-all does not overlap it. Both run in `make test`.

**QEMU proof** (`--icount 2`, Old model, fresh image): `[cam] reserved 3 x 150
KiB in the GPU arena at PA <PA>`, `[cam] buffers: arena 3x150 KiB
@<PA>`, `[cam] start outer (... src 1 ...)`,
`stream 0: start camera outer -> status 0`, frames delivered and 0 overruns;
`emu/stress.py --cycles 0 --camera-cycles 2 --icount 2` saw 2 starts/3 stops,
and `emu/soak.py --cycles 5 --icount 2` PASS. The stress harness's camera tap
coordinate was corrected from (123,314) to (123,360) for the current IconState;
with earlier app cycles the first touch can still land in Spotlight under
`--icount 2` (a QEMU input-timing flake, unrelated to the camera).

**Next card run must show** (with `diag=1`):
1. `[cam] buffers: arena 3x150 KiB @<PA>` soon after Camera.app opens, and
   `[cam] start outer (... src 1 ...)` / `start camera outer -> status 0`.
   If instead `src 2` appears, the GPU arena was unavailable - check the
   `[pica] arena ...` line; if `[cam] NO_MEM ...` appears, both sources failed.
2. `[cam] iOS wants the outer left camera`, the start `[cam-diag]` block dumps,
   `outer left camera streaming ...`, and frames/overruns in the 10 s line.
3. A live preview and a saved photo in Camera Roll. If statuses 3/6 still
   appear, the new `NO_MEM` line names which allocator failed; do **not** read
   it as a sensor/I2C fault. The boot probe alone cannot distinguish them.

### 7.2 Old vs New: paths checked, and what is different
| Path | Old 3DS (this report / run 29) | New 2DS XL (0.4.0) | Verdict |
|---|---|---|---|
| CAM clock | `CFG11_CAMERA_CNT` 00 -> 01 (bit 1 does not stick) | same register sequence | not the failure |
| CAM block reset/init | CAM0 `4610`, CAM1 `4600` before the sensor set-up | same | not the failure |
| I2C buses | outer left `0x10144000/0x78`, inner `0x10161000/0x7A`, outer right `0x10161000/0x78`; all answer `2280` | same | not the failure |
| Aptina init | run 29 streamed 12 fps with the same code | confirmed working | unchanged since 0.4.0 |
| Contiguous frame buffers | 128 MiB XNU, arena already wired (12 MiB then, 10 MiB now); failed at START | 256 MiB (`n3ds_ram=1`), succeeded | **the difference**; now reserved from the arena on both |

## 8. Not done
- **Video recording**: Camera.app's video switch needs `video-camera` and an
  H.264 encoder; the iPod touch 2G has none (no `VideoEncoders` for H.264, no
  hardware), so the capability stays off.
- Focus, exposure, white balance and flash requests are accepted and ignored
  (the sensors run their own auto exposure/white balance).
- Mirrored front preview (section 4; the sensor's own x-flip could do it).
- The third (outer right) camera and 3D photos.
- VGA stills (section 6).
