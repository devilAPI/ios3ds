# Handoff: 3DS cameras in iOS (state 2026-11-20 late, build 44 committed & deployed)

**Task for the next session:** run 30 (the user boots build 44 on the console
and uses Camera.app, see §4) will produce `build/hwlogs/run30.txt` plus one
`CAM<seq>.RAW` frame dump per camera session. **Review them and decide the
"wiggle" diagnosis** (§5). Everything below says what was done, where things
are, and how to read the results. Status of everything at handover:
`673bb5b` on `main` = build 44, pushed to GitHub; the same build's payload is
**already copied onto the SD card** in the reader PC (`devilapi@192.168.178.83:/run/media/devilapi/3ds/ios3ds/`, all files 2026-11-20 23:04), so the
user can boot it without any further copying.

## 1. What happened this session (the short version)

The user reported (run 29, build 43): a picture appears in Camera.app, but it
looks like **only the first captured frame, wiggling around**. I analyzed
`build/hwlogs/run29.txt` (committed as `docs/hwlogs/run29.txt`), found the
whole pipeline healthy on hardware, and built the diagnostics that decide the
wiggle with pixels instead of guessing (build 44, this commit):

1. **Run 29 findings** (all in `docs/camera.md` §7 and §1 below):
   - Boot self-test: **36/36 set-ups `<-- DATA`** (both CAM blocks, all three
     sensors, on the mapped ports).
   - Runtime: **~12 fps of real sensor data, every session** (7 sessions in
     the boot; kernel words = frames × 38400 exactly, 0 FIFO overruns),
     vcd delivered to Celestial, Celestial dequeued everything (0 drops).
   - Smoking gun for one suspect: the 5-s diag caught the CAM1 FIFO **parked
     at the fill cap 1280 units with the CAM_STAT overrun bit still clear** →
     a full FIFO does not raise the overrun bit on this hardware → drain
     starvation (XNU IRQ-mask windows; the GPU mirror alone measured 29 ms IRQ
     latency in run 29) would silently discard words, displacing the
     38400-word frame window per incident = a wiggling picture.
   - 15 fps would be ~576 words/ms; the observed ~450 words/ms is either the
     sensor's true rate or starved - until the dumps decide.
   - "0 stills" in run 29: the shutter tap never reached vcd as
     `StillImageCaptureNow`; ask the user to take one photo in run 30.
2. **Build 44 diagnostics** (committed, details in `docs/camera.md` §3.2/§4.1/§7):
   - Mailbox (`boot/common/cam_proto.h`, still exactly 128 bytes): `buf_pa[3]`,
     `newest`/`newest_seq`, `snew`/`snew_seq` (two newest complete frames),
     `gap_max` (worst drain-service interval in shim ticks) in the former pad.
   - Kernel shim (`kernel/shim/cam.c`): publishes those; tracks the drain gap;
     `op_stop` cleans all 3 frame buffers and **publishes before `send_want`**
     so the ARM9 sees final values; fixed a real bug where a camera *restart*
     wrote an uninitialised `pa` into `C.buf_pa[]` (IOMallocContiguous only
     fills it when it allocates), so from session 2 on the ARM9 saw pa = 0.
   - ARM9 (`boot/arm9/camera.c`): at every stop dumps the shim's two newest
     complete frames **straight out of its buffers** to `sd:/ios3ds/photos/CAM<seq>.RAW`
     (`[cam] dump check: ...` line states the guard values, skipped-dump lines
     say why); an independent `[cam] monitor` CAM_STAT watcher the XNU cannot
     starve (per monitor-loop sample: max fill, near-full count); `drain gap
     max x.y ms` added to the 10 s `[cam]` line.
   - `tools/ios3ds-camera/analyze_dump.py` (new, stdlib-only, self-tested):
     decodes a dump to BMPs in **both YUYV and UYVY byte orders** (look at
     both to settle the colour order by eye), prints mean luma diff, the best
     (dx, dy) inter-frame alignment, and rows whose best dx disagrees
     (mid-frame word loss / shear).
   - vcd (`tools/ios3ds-camera/vcd/vcd.c`): stop line is now
     `stopped after N frames this run (M total, ...)` (old counts were
     cumulative per stream across fast switches and misled the log reader).
   - Docs updated: `docs/camera.md` (§3.2, new §4.1 "Frame dumps", §6 FIFO
     timing corrected to 5.5 ms, §7 run-29 section + new diag line list),
     `README.md` cameras row now ⚠️ "data flows, preview wiggles", this file.
3. **QEMU validation** (each full boot ≈ 10-15 min wall with `--icount 2`):
   - Live preview confirmed in QEMU (screenshots `build/emu-camtest/camrun*/tap_*.png`,
     the model's pattern moves).
   - Dump path exercised end to end over several runs; found and fixed two
     real bugs this way (publish ordering; the uninitialised pa above).
     Verified states so far: dump fires at stop 1 with correct content
     (307232 bytes, header + 2 frames, `analyze_dump.py` reads it); stop-2
     skips are explained by the pa bug and fixed in the last iteration.
   - Final run (camrun5, the pa fix): **all three sessions dumped**
     (`dump check` + `dumped frames` at every stop: seq 11/10, 15/14,
     169/168; three 307232-byte RAWs on the SD image; buffer PAs stay valid
     across restarts, no skip lines). The card carries this exact build.
     Reproduce with:
     `python3 emu/tap.py --icount 2 --time 520 --tap 392:127,314 --key 505:h --out build/emu-camtest/camrun6`
     (Camera.app's icon is at iOS points (127,314) - row 3, second column).
     Note: `make sdimg` (re)builds `build/sd.img` and wipes the old emu run's
     files from it - pull the log first.
4. **Deployed build 44** to the SD card over scp (see top). Commit `673bb5b`
   pushed to GitHub main. Also on request: the unused `good first issue`
   label was deleted from the GitHub repo (issues untouched).

## 2. Formats (what the next run produces)

- `sd:/ios3ds/log.txt` → the user calls this **run 30** in
  `build/hwlogs/run30.txt`. The boot rotates the previous boot's log to
  `sd:/ios3ds/log_prev.txt`.
- `sd:/ios3ds/photos/CAM<seq>.RAW`, ~300 KiB each, one per camera session with ≥2
  frames: u32×8 header `0x44554D43 ('CAMD'), 1, 320, 240, cam, port,
  seq1, seq2` then frame 1 (newest) and frame 2, each 320×240×2 YUY422, no
  padding. Copy to `build/hwlogs/` next to the log (`scp devilapi@192.168.178.83:/run/media/devilapi/3ds/ios3ds/'photos/CAM*.RAW' build/hwlogs/`).
- New log lines to expect (documented in `docs/camera.md` §7):
  `[cam] dump check: newest seq ..., idx .../..., pa .../...`,
  `[cam] dumped frames seq A (newest) and B to 0:/ios3ds/photos/CAM....RAW`,
  `[cam] monitor 10 s / at stop: N CAM_STAT samples, max FIFO fill F units,
  near-full C`, and `drain gap max X.Y ms` inside the 10 s stats line.

## 3. Question the next session answers (the decision tree)

Run `python3 tools/ios3ds-camera/analyze_dump.py` on each RAW (script prints
its own verdict help; also renders `frame1/2.yuyv.bmp` + `.uyvy.bmp`) and read
the log's gap/monitor lines together:

| Evidence | Verdict | Next step |
|---|---|---|
| `drain gap max` > ~5 ms and/or `near-full` > 0, **and** dumps drift (dx/dy ≠ 0 between the two frames, shear rows) | words lost behind masked interrupts (FIFO overflow silently, see run 29's full-cap-without-overrun-bit) | CDMA drain (Nintendo's own driver uses CDMA; `docs/camera.md` §2/§6) - or reduce IRQ-mask windows |
| gaps small, monitor quiet, but dumps **still drift** (same dx every frame) | the true sensor frame is not exactly 38400 words (blanking reaching the FIFO) | CAM block set-up: transfer unit (+6), trimming bit 13, SOFS/EOFS; compare against the private `cam.code` module copy (rules below) |
| dump frames correct/stable, diff ~ 0 across seconds of live use | sensors delivered a frozen scene: sensor-side (AE/sequencer) or display-side problem | look at vcd's BGRA path and Celestial/pica (run 29 had a pica CPU read-back fallback + `arena full` around the camera); try `ios3dsPattern` preference to separate display from data |
| YUYV vs UYVY BMPs | one looks right | fix `cam_yuyv_to_bgra` byte order if needed (`boot/common/cam_pattern.h`), note in `docs/camera.md` §7 |

Also: 12 fps vs 15 - if the dumps are clean and the gap lines quiet, ~12 fps
is simply the sensor's rate with our PLL config; not a bug per se.

## 4. Run 30 procedure (tell the user; card already has build 44)

Boot the console, open Camera.app, hold ~20-30 s on the back camera, switch
to the front for ~20 s, take **one photo** (tests the still/review-image path
on hardware for the first time), close with Home. Then hand the card back and:

    scp devilapi@192.168.178.83:/run/media/devilapi/3ds/ios3ds/log.txt build/hwlogs/run30.txt
    scp 'devilapi@192.168.178.83:/run/media/devilapi/3ds/ios3ds/photos/CAM*.RAW' build/hwlogs/

## 5. Odds and ends to know

- The shim's counters (`frames/words/services`) are **global since boot**;
  the 10 s line prints deltas since the last stat line, the 0.2/1/5-s lines
  print absolute values - that mismatch misled me in run 29's numbers at
  first (details in the scratchpad-era analysis; keep it in mind).
- Two mediaserverd processes appeared per Camera.app launch (device re-created
  inside each), plus Celestial restarts the stream after the first ~5 s;
  the same happens in QEMU, so it is iOS-side behaviour, not hardware.
- FatFs on the ARM9 has LFN enabled, so `CAM00000169.RAW` is a legal name.
- The kernel shim lives in two images: `shim.bin` and `gpu.bin` (the cam.o
  part!) - **`make firm` does NOT rebuild them, run `make shim` before
  `make sdimg`**, or you test a stale shim like I did twice.
- QEMU tap coordinates: Camera.app icon at iOS points (127,314); Settings
  (47,314), Notes (47,226), Safari dock (199,440). The `good first issue`
  label is gone from the repo (user request); issues #1, #6, #7 open.
- Nintendo code rules (unchanged): `~/.local/share/ios3ds-private/cam.code`
  (also `sd:/ios3ds/cam.code`) stays private - never commit or quote it;
  only derived facts in our own words; disassembly base 0x00100000, Thumb,
  CAM0 virtual 0x1EC20000 / CAM1 0x1EC21000.
- `handoff` numbering: this file supersedes the previous one entirely.
