#!/usr/bin/env python3
"""Stereo 3D transition tour (QEMU): both eyes through the home-screen
animations (docs/stereo3d.md section 9).

Boots a stereo3d=1 image, sets the 3D slider, then walks through the
SpringBoard transitions the stereo classifier has to survive: page swipes,
the Utilities folder open/close, an app open/close zoom and the iOS 4 app
switcher (double Home). During every transition it grabs frames in a burst
and saves BOTH eye images separately (read straight out of PDC0's
framebuffer A = left and B = right with pmemsave, not the anaglyph), plus
the anaglyph screendump.

For every 3D frame it measures the horizontal disparity between the eyes in
three bands of the iOS frame (status bar, icon grid, dock) and the residual
after the best shift; a large residual means the eyes show different
pictures (eye mismatch / tearing), a band whose disparity changes sign from
one frame to the next is depth flicker. Flat frames (PDC0 in 2D) must have
the barrier off. Writes build/emu/stereo_tour/report.md + report.json.

  python3 emu/stereo_tour.py --icount 2 --slider3d 255
"""
import argparse
import json
import os
import re
import subprocess
import sys
import tempfile
import time

from PIL import Image, ImageChops, ImageStat

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import viewer  # noqa: E402

ROOT = viewer.ROOT
UTILITIES = (120, 237)
APP = (47, 50)              # first icon on page 1


def read_log(sd):
    r = subprocess.run(["mtype", "-i", "%s@@%d" % (sd, 2048 * 512), "::/ios3ds/log.txt"],
                       capture_output=True, text=True, errors="replace")
    return r.stdout


def fb_to_image(raw, fmt, stride):
    """PDC0 framebuffer (column-major, 240 rows per column) -> 400x240 RGB,
    the same mapping as the QEMU model's screen update."""
    bpp = {0: 4, 1: 3}.get(fmt, 2)
    im = Image.new("RGB", (400, 240))
    px = im.load()
    for x in range(400):
        base = x * stride
        for y in range(240):
            o = base + (239 - y) * bpp
            if fmt == 0:
                px[x, y] = (raw[o + 3], raw[o + 2], raw[o + 1])
            elif fmt == 1:
                px[x, y] = (raw[o + 2], raw[o + 1], raw[o])
            else:
                v = raw[o] | raw[o + 1] << 8
                px[x, y] = ((v >> 11) << 3, ((v >> 5) & 63) << 2, (v & 31) << 3)
    return im


def to_ios(im):
    return im.crop((20, 0, 380, 240)).rotate(-90, expand=True)     # 240x360, iOS orientation


BANDS = {"status": (0, 14), "icons": (30, 255), "dock": (300, 355)}


def disparity(left, right, band, maxs=8):
    """best horizontal shift s (right(x) ~ left(x - s)) in one band and the
    residual mean abs difference after it"""
    y0, y1 = BANDS[band]
    l = left.convert("L").crop((maxs, y0, 240 - maxs, y1))
    best = None
    for s in range(-maxs, maxs + 1):
        r = right.convert("L").crop((maxs + s, y0, 240 - maxs + s, y1))
        d = ImageStat.Stat(ImageChops.difference(l, r)).mean[0]
        if best is None or d < best[1] - 0.05:
            best = (s, d)
    return best


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--sd", default=os.path.join(ROOT, "build", "sd.img"))
    ap.add_argument("--qemu", default="")
    ap.add_argument("--firm", default=os.path.join(ROOT, "build", "boot", "ios3ds.firm"))
    ap.add_argument("--out", default=os.path.join(ROOT, "build", "emu", "stereo_tour"))
    ap.add_argument("--icount", default="2")
    ap.add_argument("--slider3d", type=int, default=255)
    ap.add_argument("--burst", type=int, default=12, help="frames grabbed per transition")
    ap.add_argument("--boot-timeout", type=float, default=1200)
    ap.add_argument("--only", default="", help="comma list of transition labels to run (default: all)")
    ap.add_argument("--flush", type=float, default=90, help="idle seconds before quitting, so the log reaches the card")
    a = ap.parse_args()
    if a.qemu:
        viewer.QEMU = a.qemu
    libdir = os.path.dirname(viewer.QEMU)
    if os.path.exists(os.path.join(libdir, "libslirp.so.0")):
        os.environ["LD_LIBRARY_PATH"] = libdir + os.pathsep + os.environ.get("LD_LIBRARY_PATH", "")
    os.makedirs(a.out, exist_ok=True)
    tmp = tempfile.mkdtemp(prefix="ios3ds-st-", dir=os.path.join(ROOT, "build"))
    sock = os.path.join(tmp, "qmp.sock")
    accel = "tcg,thread=single" if a.icount else "tcg,thread=multi"
    cmd = [viewer.QEMU, "-M", "nintendo3ds", "-accel", accel,
           "-kernel", a.firm,
           "-drive", "file=%s,if=sd,format=raw" % a.sd, "-serial", "none",
           "-display", "none", "-qmp", "unix:%s,server,nowait" % sock]
    if a.icount:
        cmd += ["-icount", "shift=%s,sleep=off" % a.icount, "-rtc", "clock=vm"]
    proc = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    q = viewer.QMP(sock)
    t0 = time.time()
    log = lambda m: print("[tour %6.0fs] %s" % (time.time() - t0, m), flush=True)  # noqa: E731

    def hmp(line):
        return q.cmd("human-monitor-command", **{"command-line": line}).get("return", "")

    def rd32(pa):
        out = hmp("xp /1wx 0x%x" % pa)
        m = re.search(r":\s*(0x[0-9a-fA-F]+)", out)
        return int(m.group(1), 16) if m else 0

    def qom(prop):
        r = q.cmd("qom-get", path="/machine", property=prop)
        return r.get("return", 0)

    def grab(name):
        """both eyes + the anaglyph; returns a record"""
        q.cmd("stop")
        try:
            # screendump first: the model refreshes pdc-3d-mode in its
            # screen update, which -display none only runs on a dump
            f = os.path.join(tmp, "s.ppm")
            q.cmd("screendump", filename=f)
            ana = to_ios(Image.open(f).convert("RGB"))
            mode3d = (qom("pdc-3d-mode") & 0x70) == 0x20 and bool(qom("lcd-3d-barrier") & 1)
            barrier = qom("lcd-3d-barrier")
            fmt = rd32(0x10400470) & 7
            sel = (rd32(0x10400478) >> 4) & 1
            stride = rd32(0x10400490) or 720
            fa = rd32(0x10400468 + 4 * sel)
            fbb = rd32(0x10400494 + 4 * sel)
            eyes = []
            for tag, pa in (("L", fa), ("R", fbb)):
                f = os.path.join(tmp, tag + ".bin")
                q.cmd("pmemsave", val=pa, size=stride * 400, filename=f)
                with open(f, "rb") as fh:
                    eyes.append(to_ios(fb_to_image(fh.read(), fmt, stride)))
        finally:
            q.cmd("cont")
        rec = {"name": name, "pdc3d": mode3d, "barrier": barrier}
        if not mode3d:
            eyes[1] = eyes[0]           # in 2D the panel shows A only
        eyes[0].save(os.path.join(a.out, name + "_L.png"))
        eyes[1].save(os.path.join(a.out, name + "_R.png"))
        ana.save(os.path.join(a.out, name + "_ana.png"))
        if mode3d:
            for b in BANDS:
                rec[b] = disparity(eyes[0], eyes[1], b)
        return rec

    def tap(x, y, hold=0.15):
        dx, dy = viewer.ios_to_display(x / 320.0, y / 480.0)
        q.pointer(dx, dy, True)
        time.sleep(hold)
        q.pointer(dx, dy, False)

    def drag(x0, y0, x1, y1, secs=0.4):
        dx, dy = viewer.ios_to_display(x0 / 320.0, y0 / 480.0)
        q.pointer(dx, dy, True)
        time.sleep(0.15)
        for i in range(1, 11):
            time.sleep(secs / 10)
            f = i / 10.0
            dx, dy = viewer.ios_to_display((x0 + (x1 - x0) * f) / 320.0, (y0 + (y1 - y0) * f) / 480.0)
            q.pointer(dx, dy, None)
        time.sleep(0.05)
        q.pointer(dx, dy, False)

    def key(qcode, hold=0.2):
        q.key(qcode, True)
        time.sleep(hold)
        q.key(qcode, False)

    booted = None
    while time.time() - t0 < a.boot_timeout:
        time.sleep(10)
        if "[input] connected to SpringBoard" in read_log(a.sd):
            booted = time.time() - t0
            break
        if proc.poll() is not None:
            break
    results = {"boot_seconds": booted, "transitions": []}
    if booted:
        log("SpringBoard + input up after %.0f s" % booted)
        q.cmd("qom-set", path="/machine", property="mcu-3d-slider", value=a.slider3d)
        while time.time() - t0 < booted + 400 and "sbwarm" not in read_log(a.sd):
            time.sleep(5)
        # SpringBoard sometimes comes up on (or races into) Spotlight with
        # its keyboard (the known iOS 4 home/Spotlight race): a swipe to the
        # left leaves Spotlight for page 1 and only rubber-bands on page 1
        drag(280, 240, 40, 240)
        time.sleep(60)

        def burst(label, action, after=None, gap=1.5):
            if a.only and label not in a.only.split(","):
                return
            recs = [grab("%s_00_before" % label)]
            action()
            for i in range(1, a.burst + 1):
                time.sleep(gap)
                recs.append(grab("%s_%02d" % (label, i)))
            time.sleep(20)
            recs.append(grab("%s_%02d_settled" % (label, a.burst + 1)))
            if after:
                after()
                time.sleep(8)
            results["transitions"].append({"label": label, "frames": recs})
            log("%s: %s" % (label, " ".join(
                ("3D[%s]" % ",".join("%+d" % r[b][0] for b in BANDS)) if r["pdc3d"] else "2D" for r in recs)))

        burst("home", lambda: None, gap=1.0)
        # 4.2.1's stock layout has one icon page: a swipe to the right
        # rubber-bands, a swipe to the left pages over to Spotlight and back
        burst("swipe_bounce", lambda: drag(280, 240, 40, 240), gap=0.8)
        burst("swipe_to_spotlight", lambda: drag(40, 240, 280, 240), gap=0.8)
        burst("swipe_to_home", lambda: drag(280, 240, 40, 240), gap=0.8)
        burst("folder_open", lambda: tap(*UTILITIES))
        burst("folder_close", lambda: key("ret"))
        burst("app_open", lambda: tap(*APP))
        burst("app_close", lambda: key("ret"))
        burst("switcher_open", lambda: (key("ret", 0.1), time.sleep(0.15), key("ret", 0.1)))
        burst("switcher_close", lambda: key("ret"))
        time.sleep(a.flush)
    try:
        q.cmd("quit")
        proc.wait(timeout=30)
    except Exception:
        proc.kill()
    text = read_log(a.sd)
    with open(os.path.join(a.out, "log.txt"), "w") as f:
        f.write(text)

    # analysis: mismatch = residual after the best shift well above the flat
    # residual; flicker = a band's disparity sign flips between 3D frames
    lines = ["# Stereo transition tour", "", "- boot: %s" % (booted and "%.0f s" % booted), ""]
    bad = 0
    for t in results["transitions"]:
        fr = t["frames"]
        flips = 0
        mism = 0
        for b in BANDS:
            prev = None
            for r in fr:
                if not r["pdc3d"]:
                    prev = None
                    continue
                s, res = r[b]
                if res > 12:
                    mism += 1
                if prev is not None and s * prev < 0:
                    flips += 1
                prev = s if s else prev
        modes = "".join("3" if r["pdc3d"] else "2" for r in fr)
        toggles = sum(1 for i in range(1, len(modes)) if modes[i] != modes[i - 1])
        t["flips"], t["mismatch"], t["toggles"] = flips, mism, toggles
        bad += flips + mism
        lines.append("- %s: modes %s, 3D/2D toggles %d, depth sign flips %d, eye-mismatch bands %d" %
                     (t["label"], modes, toggles, flips, mism))
        for r in fr:
            if r["pdc3d"]:
                lines.append("  - %s: %s" % (r["name"], ", ".join(
                    "%s %+d px (res %.1f)" % (b, r[b][0], r[b][1]) for b in BANDS)))
    lines += ["", "- [stereo]/[gl] stereo lines:", ""]
    lines += ["  - `%s`" % l for l in text.splitlines() if "stereo" in l.lower()][-40:]
    with open(os.path.join(a.out, "report.json"), "w") as f:
        json.dump(results, f, indent=1)
    with open(os.path.join(a.out, "report.md"), "w") as f:
        f.write("\n".join(lines) + "\n")
    print("\n".join(lines[:60]))
    return 0 if booted and not bad else 1


if __name__ == "__main__":
    sys.exit(main())
