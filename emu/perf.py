#!/usr/bin/env python3
"""Folder open/close and Safari-launch timing benchmark (QEMU, --icount 2).

The repeatable "before/after" number for the Old 3DS UI slowness: how many
wall seconds SpringBoard's CoreAnimation takes to open and close the
Utilities folder, and to cold-launch Safari, on the Old 3DS model, together
with the [perf] CA frame-time histogram lines the engine prints while an
animation runs (the same lines a hardware log shows).

  python3 emu/perf.py --runs 3 --icount 2
  python3 emu/perf.py --safari --icount 2      # add a cold Safari launch

Boots build/sd.img, waits for a stable home screen (and the sbwarm folder
warm-up), then per run: tap the Utilities folder (iOS point 120,237), wait
for the folder to open (the screen stops changing and differs from the home
reference), press Home, wait until the home screen is back. The open/close
times are the seconds from the input until the screen settles. A Safari run
taps the dock icon (199,440) and waits for a rendered, non-launch-screen
frame. Writes build/emu/perf/report.md + report.json and the log.

For a full page load over the Wi-Fi model (HTML + images served from the
host) use emu/safari_test.py, which reports the page-load time and memory.
"""
import argparse
import fcntl
import json
import os
import re
import subprocess
import sys
import tempfile
import time

from PIL import Image, ImageStat, ImageChops

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import viewer  # noqa: E402

ROOT = viewer.ROOT

UTILITIES = (120, 237)     # re-log 77: the Utilities folder, row 3 column 2
SAFARI_DOCK = (199, 440)   # handoff.md: Safari in the dock


def read_log(sd):
    r = subprocess.run(["mtype", "-i", "%s@@%d" % (sd, 2048 * 512), "::/ios3ds/log.txt"],
                       capture_output=True, text=True, errors="replace")
    return r.stdout


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--sd", default=os.path.join(ROOT, "build", "sd.img"))
    ap.add_argument("--qemu", default="")
    ap.add_argument("--out", default=os.path.join(ROOT, "build", "emu", "perf"))
    ap.add_argument("--runs", type=int, default=3)
    ap.add_argument("--icount", default="")
    ap.add_argument("--safari", action="store_true")
    ap.add_argument("--boot-timeout", type=float, default=1200)
    ap.add_argument("--slider3d", type=int, default=-1,
                    help="set the 3D slider (QOM mcu-3d-slider, e.g. 255) after boot, for stereo3d=1 images")
    ap.add_argument("--settle-limit", type=float, default=45,
                    help="seconds an open/close has to settle before it counts as failed")
    a = ap.parse_args()
    if a.qemu:
        viewer.QEMU = a.qemu
    libdir = os.path.dirname(viewer.QEMU)
    if os.path.exists(os.path.join(libdir, "libslirp.so.0")):
        os.environ["LD_LIBRARY_PATH"] = libdir + os.pathsep + os.environ.get("LD_LIBRARY_PATH", "")
    os.makedirs(a.out, exist_ok=True)
    tmp = tempfile.mkdtemp(prefix="ios3ds-perf-")
    sock = os.path.join(tmp, "qmp.sock")
    accel = "tcg,thread=single" if a.icount else "tcg,thread=multi"
    cmd = [viewer.QEMU, "-M", ("nintendo3ds,new3ds=on" if os.environ.get("IOS3DS_NEW3DS") == "1" else "nintendo3ds"),
           "-accel", accel, "-kernel", os.path.join(ROOT, "build", "boot", "ios3ds.firm"),
           "-drive", "file=%s,if=sd,format=raw" % a.sd, "-serial", "none",
           "-display", "none", "-qmp", "unix:%s,server,nowait" % sock]
    if a.icount:
        cmd += ["-icount", "shift=%s,sleep=off" % a.icount, "-rtc", "clock=vm"]
    proc = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    q = viewer.QMP(sock)
    t0 = time.time()
    log = lambda m: print("[perf %6.0fs] %s" % (time.time() - t0, m), flush=True)  # noqa: E731

    def shot(name):
        f = os.path.join(tmp, "s.ppm")
        q.cmd("screendump", filename=f)
        im = Image.open(f).crop((20, 0, 380, 240)).rotate(-90, expand=True)
        im.save(os.path.join(a.out, name + ".png"))
        return im

    def grey(im):
        return im.convert("L")

    def mean(im):
        return ImageStat.Stat(grey(im)).mean[0]

    def diff(a, b):
        return ImageStat.Stat(ImageChops.difference(grey(a), grey(b))).mean[0]

    def tap(x, y, hold=0.15):
        dx, dy = viewer.ios_to_display(x / 320.0, y / 480.0)
        q.pointer(dx, dy, True)
        time.sleep(hold)
        q.pointer(dx, dy, False)

    def key(qcode, hold=0.2):
        q.key(qcode, True)
        time.sleep(hold)
        q.key(qcode, False)

    def wait_for(name, want_open, limit):
        """Poll until the screen is stable AND (matches home) or (differs from
        home), depending on want_open. Returns (image, seconds since start)."""
        start = time.time()
        prev = None
        while time.time() - start < limit:
            time.sleep(2)
            im = shot(name)
            if prev is not None and diff(im, prev) < 1.0:
                d = diff(im, home_ref)
                if want_open and d > 12:
                    return im, time.time() - start
                if not want_open and d <= 12:
                    return im, time.time() - start
            prev = im
        return prev, time.time() - start

    # 1. boot to a stable, lit home screen (the same loop as emu/soak.py)
    booted = None
    while time.time() - t0 < a.boot_timeout:
        time.sleep(10)
        if "[input] connected to SpringBoard" in read_log(a.sd):
            booted = time.time() - t0
            break
        if proc.poll() is not None:
            break
    home_ref = None
    if booted is not None:
        log("SpringBoard + input up after %.0f s" % booted)
        if a.slider3d >= 0:
            # as emu/soak.py: set after boot (a machine reset can clear an early write)
            q.cmd("qom-set", path="/machine", property="mcu-3d-slider", value=a.slider3d)
            log("3D slider set to %d" % a.slider3d)
        prev = None
        for _ in range(60):
            time.sleep(5)
            im = shot("home_ref")
            # The boot console is also lit and stable for a while after
            # [sbwarm]; its greyscale stddev is ~92 against ~54 for the home
            # screen. Taking it as the reference made every folder close look
            # like a timeout (2026-11-24).
            stable_home = (prev is not None and mean(im) > 20 and
                           ImageStat.Stat(grey(im)).stddev[0] < 70 and diff(im, prev) < 1.0)
            if stable_home:
                home_ref = im
                break
            prev = im
    results = {"boot_seconds": booted, "runs": [], "safari": []}
    if home_ref is None:
        log("no stable home screen; aborting (boot %s)" % booted)
    else:
        # wait for the folder warm-up so every measurement is the steady state
        while time.time() - t0 < 120 and "sbwarm" not in read_log(a.sd):
            time.sleep(5)

        for i in range(a.runs):
            r = {"run": i}
            log("run %d: opening the Utilities folder" % i)
            tap(*UTILITIES)
            _, r["open_s"] = wait_for("r%02d_open" % i, True, a.settle_limit)
            r["open_settled"] = diff(shot("r%02d_open_final" % i), home_ref) > 12
            log("run %d: folder open in %.1f s" % (i, r["open_s"]))
            key("ret")                       # Home closes the folder
            _, r["close_s"] = wait_for("r%02d_close" % i, False, a.settle_limit)
            r["close_settled"] = diff(shot("r%02d_close_final" % i), home_ref) <= 12
            log("run %d: folder close in %.1f s" % (i, r["close_s"]))
            # dismiss Spotlight if Home on page 1 opened it (re-log 100)
            if not r["close_settled"]:
                for _ in range(3):
                    tap(301, 30)
                    time.sleep(2)
                    key("ret")
                    _, t = wait_for("r%02d_unspotlight" % i, False, 20)
                    if diff(shot("r%02d_unspotlight" % i), home_ref) <= 12:
                        break
            results["runs"].append(r)

        if a.safari:
            log("cold-launching Safari")
            tap(*SAFARI_DOCK)
            start = time.time()
            prev = None
            safari_s = None
            while time.time() - start < a.settle_limit:
                time.sleep(2)
                im = shot("safari")
                g = grey(im)
                m, sd = ImageStat.Stat(g).mean[0], ImageStat.Stat(g).stddev[0]
                if m > 90 and m < 170 and sd < 8:
                    prev = None                 # launch image / blank
                    continue
                if prev is not None and diff(im, prev) < 1.0 and diff(im, home_ref) > 12:
                    safari_s = time.time() - start
                    break
                prev = im
            results["safari"] = [{"launch_s": safari_s}]
            log("Safari cold launch in %.1f s" % (safari_s or -1))

    try:
        q.cmd("quit")
        proc.wait(timeout=30)
    except Exception:
        proc.kill()
    text = read_log(a.sd)
    with open(os.path.join(a.out, "log.txt"), "w") as f:
        f.write(text)
    perf = [l for l in text.splitlines() if l.startswith("[perf] ")]
    ca = [l for l in text.splitlines() if "render thread CPU" in l]
    results["perf_lines"] = perf
    results["ca_lines"] = ca
    with open(os.path.join(a.out, "report.json"), "w") as f:
        json.dump(results, f, indent=1)
    opens = [r["open_s"] for r in results["runs"]]
    closes = [r["close_s"] for r in results["runs"]]
    lines = ["# Perf report", "",
             "- boot to SpringBoard + input: %s" % ("%.0f s" % booted if booted else "**not reached**"),
             "- folder open (s): %s" % (", ".join("%.1f" % v for v in opens) if opens else "n/a"),
             "- folder close (s): %s" % (", ".join("%.1f" % v for v in closes) if closes else "n/a"),
             "- Safari cold launch (s): %s" % ("%.1f" % results["safari"][0]["launch_s"] if results["safari"] and results["safari"][0]["launch_s"] else "n/a"),
             ""]
    if perf:
        lines += ["## [perf] CA frame-time histogram (while animating)", ""]
        for l in perf:
            lines.append("- `%s`" % l)
        lines.append("")
    if ca:
        lines += ["## [gl] CA render-thread cost", ""]
        for l in ca:
            lines.append("- `%s`" % l)
        lines.append("")
    with open(os.path.join(a.out, "report.md"), "w") as f:
        f.write("\n".join(lines) + "\n")
    print("\n".join(lines))
    return 0 if booted and home_ref else 1


if __name__ == "__main__":
    sys.exit(main())
