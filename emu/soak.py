#!/usr/bin/env python3
"""Stability soak test: boot iOS in QEMU, then cycle through the home screen
for a long time (open every icon, Home, page swipes, lock/unlock) and check
the SD log for trouble afterwards.

  python3 emu/soak.py --cycles 40                # ~35 min after boot
  python3 emu/soak.py --cycles 10 --icount 2     # hardware timing (slower)

Writes build/emu/soak/report.md + report.json, one screenshot per cycle
(upright iOS view) and the final log. Exit status 1 if any check failed.
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

from PIL import ImageFilter, Image, ImageChops, ImageStat

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import viewer  # noqa: E402

ROOT = viewer.ROOT

# iOS points (320x480 portrait): 4x4 icon grid + 4 dock slots on page 1
GRID = [(47 + 76 * c, 50 + 88 * r) for r in range(4) for c in range(4)]
DOCK = [(47 + 76 * c, 440) for c in range(4)]
TARGETS = GRID + DOCK

# log patterns: (name, regex, is_failure)
CHECKS = [
    ("kernel panic", r"panic\(cpu|Debugger message: Fatal Exception|undefined kernel instruction", True),
    ("ARM11 stall dump", r"\[probe\] ARM11 stalled", True),
    ("md0 I/O error", r"\[blk\] .* failed", True),
    ("log write error", r"\[log\] write error", True),
    ("md0 errors in heartbeat", r"\[hb\] t=.* (?!0 err)\d+ err$", True),
    ("process crash (signal)", r"exited abnormally|Segmentation fault|Bus error|Abort trap", True),
    ("jetsam / low memory", r"jetsam|memorystatus|low memory|Received memory warning", False),
    ("SpringBoard restart", r"\[input\] connected to SpringBoard", False),
    ("GPU mirror failure", r"\[gpu\] display mirror( FAILED|: self-test (FAILED|aborted))", False),
    ("GPU timeout", r"\[pica\] TIMEOUT|\[gpu\] display mirror.*did not finish", True),
    ("GPU present check failed", r"GPU present check FAILED", True),
    ("GPU fell back to the CPU", r"GPU rendering is OFF|reading surfaces back through the CPU from now on", False),
    ("camera init failed", r"\[cam\].*init failed|\[cam\].*did not answer", False),
    ("camera FIFO overruns", r"\[cam\].*FIFO overruns [1-9]", False),
    ("audio DMA error", r"could not start DMA|no CODEC", False),
    ("watchdog / hang", r"watchdog (timeout|reset|fired)|hang detected", True),
]


def read_log(sd):
    r = subprocess.run(["mtype", "-i", "%s@@%d" % (sd, 2048 * 512), "::/ios3ds/log.txt"],
                       capture_output=True, text=True, errors="replace")
    return r.stdout


def soak_slot(slots=2):
    """Hold one of `slots` machine-wide soak slots (flock, released when the
    process exits). Many parallel soaks overload the host: --icount guests then
    boot in 8 minutes instead of 1 and the screenshot checks fail. Waits for a
    free slot; IOS3DS_SOAK_SLOTS overrides the count, 0 disables the lock."""
    slots = int(os.environ.get("IOS3DS_SOAK_SLOTS", slots))
    if slots <= 0:
        return None
    d = os.path.join(os.path.expanduser("~"), ".cache", "ios3ds-soak")
    os.makedirs(d, exist_ok=True)
    t = time.time()
    while True:
        for n in range(slots):
            f = open(os.path.join(d, "slot%d.lock" % n), "w")
            try:
                fcntl.flock(f, fcntl.LOCK_EX | fcntl.LOCK_NB)
                if time.time() - t > 1:
                    print("[soak] got soak slot %d after %.0f s" % (n, time.time() - t), flush=True)
                return f
            except OSError:
                f.close()
        if time.time() - t < 1:
            print("[soak] all %d soak slots busy (other worktrees); waiting" % slots, flush=True)
        time.sleep(10)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--sd", default=os.path.join(ROOT, "build", "sd.img"))
    ap.add_argument("--qemu", default="", help="QEMU binary to use (default: env IOS3DS_QEMU or the shared build/qemu)")
    ap.add_argument("--out", default=os.path.join(ROOT, "build", "emu", "soak"))
    ap.add_argument("--cycles", type=int, default=40)
    ap.add_argument("--boot-timeout", type=float, default=1200)
    ap.add_argument("--settle", type=float, default=15, help="seconds an app gets after its tap")
    ap.add_argument("--new3ds", action="store_true", help="model a New 3DS (-M nintendo3ds,new3ds=on; env IOS3DS_NEW3DS=1 does the same, docs/n3ds.md)")
    ap.add_argument("--icount", default="")
    ap.add_argument("--slider3d", type=int, default=-1,
                    help="set the 3D slider (MCU reg 0x08) at boot, e.g. 255 for stereo3d=1")
    ap.add_argument("--long-lock", type=float, default=130, help="seconds locked on every 10th cycle")
    ap.add_argument("--targets", default="", help="x,y;x,y;... iOS points to tap instead of the icon grid + dock")
    ap.add_argument("--settles", default="", help="comma list of per-cycle settle seconds (cycled)")
    ap.add_argument("--unlock-first", action="store_true", help="press Home and slide to unlock before taking the home-screen reference (use when the image boots to a lock screen)")
    ap.add_argument("--no-extras", action="store_true", help="skip the page swipes and lock rounds (for a one-tap-per-icon app survey)")
    ap.add_argument("--require-app", action="store_true",
                    help="count a tap that did not open an app as a failure (older iOS versions: "
                         "a home screen that never changes must not pass, re-log 125)")
    ap.add_argument("--crashlogs", action="store_true", help="run scripts/crashlogs.py on the SD image afterwards and copy CrashReporter reports into --out")
    a = ap.parse_args()
    if a.new3ds:
        os.environ["IOS3DS_NEW3DS"] = "1"
    if a.qemu:
        viewer.QEMU = a.qemu
    # a worktree-private QEMU build bundles libslirp next to the binary
    libdir = os.path.dirname(viewer.QEMU)
    if os.path.exists(os.path.join(libdir, "libslirp.so.0")):
        os.environ["LD_LIBRARY_PATH"] = libdir + os.pathsep + os.environ.get("LD_LIBRARY_PATH", "")
    os.makedirs(a.out, exist_ok=True)
    targets = [tuple(float(v) for v in p.split(",")) for p in a.targets.split(";") if p] or TARGETS
    settles = [float(v) for v in a.settles.split(",") if v] or [a.settle]
    tmp = tempfile.mkdtemp(prefix="ios3ds-soak-")
    sock = os.path.join(tmp, "qmp.sock")
    accel = "tcg,thread=single" if a.icount else "tcg,thread=multi"
    cmd = [viewer.QEMU, "-M", ("nintendo3ds,new3ds=on" if os.environ.get("IOS3DS_NEW3DS") == "1" else "nintendo3ds"), "-accel", accel,
           "-kernel", os.path.join(ROOT, "build", "boot", "ios3ds.firm"),
           "-drive", "file=%s,if=sd,format=raw" % a.sd, "-serial", "none",
           "-display", "none", "-qmp", "unix:%s,server,nowait" % sock]
    if a.icount:
        cmd += ["-icount", "shift=%s,sleep=off" % a.icount]
    slot = soak_slot()  # noqa: F841 (held until exit)
    proc = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    q = viewer.QMP(sock)
    if a.slider3d >= 0:
        q.cmd("qom-set", path="/machine", property="mcu-3d-slider", value=a.slider3d)
    t0 = time.time()
    log = lambda m: print("[soak %6.0fs] %s" % (time.time() - t0, m), flush=True)  # noqa: E731

    def shot(name):
        f = os.path.join(tmp, "s.ppm")
        q.cmd("screendump", filename=f)
        img = Image.open(f).crop((20, 0, 380, 240)).rotate(-90, expand=True)
        img.save(os.path.join(a.out, name + ".png"))
        return img

    def settled(name, first=4.0, limit=30.0):
        """screenshot once two shots 2 s apart (after `first` s) are the same
        and lit: a still-black screen (backlight fading in after an unlock)
        would otherwise pass here and fail the unlock comparison"""
        t_start = time.time()
        time.sleep(first)
        prev = shot(name)
        bright = ImageStat.Stat(prev.convert("L")).mean[0]
        while time.time() - t_start < limit:
            time.sleep(2)
            img = shot(name)
            b = ImageStat.Stat(img.convert("L")).mean[0]
            if b > 10 and bright > 10 and ImageStat.Stat(ImageChops.difference(
                    img.convert("L"), prev.convert("L"))).mean[0] < 1.0:
                # Not the home screen: one slow frame can hold an animation
                # still for 2-5 s (3.1.3's GPU compositor in QEMU, re-log
                # NEXT). Such a frame must stay unchanged 6 s longer.
                ref = home_ref_box[0]
                if ref is None or ImageStat.Stat(ImageChops.difference(
                        img.convert("L"), ref.convert("L"))).mean[0] <= 12:
                    return img, time.time() - t_start
                still = True
                for _ in range(3):
                    time.sleep(2)
                    nxt = shot(name)
                    if ImageStat.Stat(ImageChops.difference(
                            nxt.convert("L"), img.convert("L"))).mean[0] >= 1.0:
                        still = False
                        img = nxt
                        break
                if still:
                    return img, time.time() - t_start
                b = ImageStat.Stat(img.convert("L")).mean[0]
            prev, bright = img, b
        return prev, time.time() - t_start

    def is_bootlog(img):
        """The M1 boot console is text on black (green or white, depending on
        the log line); on a loaded host it can stay lit long after SpringBoard's
        input daemon connected, so the 'stable and bright' reference test can
        accept it and every later home screenshot fails with a ~70 diff.
        The console is monochrome: unlike the home screen's coloured icons it
        has almost no saturated pixels. (re-log 111/114 and NEXT.)"""
        rgb = img.convert("RGB")
        rm, gm, bm = (ImageStat.Stat(ch).mean[0] for ch in rgb.split())
        if gm > 2.0 * max(rm, bm) + 2 and (rm + gm + bm) / 3.0 < 90:
            return True
        # Only ask the saturation question of console-*textured* frames. A
        # lit home screen is dim on this host (RGB means ~44/44/44) but its
        # greyscale stddev is ~54; text on black is ~92. Without this gate a
        # dim, low-saturation home frame would be called a console, the
        # reference would never be accepted and every cycle would time out
        # waiting for an app (seen as a >15 min silent soak after re-log 113).
        grey = img.convert("L")
        stat = ImageStat.Stat(grey)
        if stat.stddev[0] < 70:
            return False
        # Sample the top screen: the home screen's icon grid has thousands of
        # saturated pixels, the console's text has a few (measured 1.7k vs
        # 17k over both screens on a 4.1 image). 1/4 sampling keeps this cheap
        # even when it is called on every reference attempt.
        top = rgb.crop((0, 0, min(320, rgb.width), min(240, rgb.height)))
        px = list(top.getdata())[::4]
        saturated = sum(1 for r, g, b in px
                        if max(r, g, b) - min(r, g, b) > 25 and r + g + b > 120)
        return saturated < 1200

    def is_spotlight(img):
        """Spotlight's light search field fills the top 40 points; the home
        screen's status bar is dark (measured on both GPU and CPU copies).
        On 3.1.3 the first icon row reaches into that band (mean ~40), so a
        frame that still matches the home reference is never Spotlight."""
        if ImageStat.Stat(img.convert("L").crop((0, 0, 320, 40))).mean[0] <= 35:
            return False
        ref = home_ref_box[0]
        return ref is None or ImageStat.Stat(ImageChops.difference(
            img.convert("L"), ref.convert("L"))).mean[0] > 12

    def dismiss_spotlight(name):
        """Leave Spotlight, including the keyboard and a typed query. Home on
        page 1 opens Spotlight, and a tap landing during a transition can type
        an app name into it (re-log 100): a page swipe + Home is the usual way
        out, but a query needs its X (top right) tapped first, and on a loaded
        --icount 2 guest one Home can be swallowed. Return the settled image
        and the number of attempts."""
        img = None
        for attempt in range(3):
            if attempt:
                tap(301, 30)                # clear a query that keeps the keyboard up
                time.sleep(2)
            drag(280, 240, 40, 240)
            time.sleep(3)
            key("ret")
            time.sleep(4)
            img, _ = settled(name)
            if not is_spotlight(img):
                break
        return img, attempt + 1

    def tap(x, y, hold=0.15):
        dx, dy = viewer.ios_to_display(x / 320.0, y / 480.0)
        q.pointer(dx, dy, True)
        time.sleep(hold)
        q.pointer(dx, dy, False)

    def drag(x0, y0, x1, y1, secs=0.4):
        dx, dy = viewer.ios_to_display(x0 / 320.0, y0 / 480.0)
        q.pointer(dx, dy, True)
        # hold the start a moment: with --icount (sleep=off) an idle guest's
        # clock jumps ahead, and a 0.4 s wall-clock swipe was first sampled
        # 50 points in (re-log 77: "down 88,240" for a swipe from 40)
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

    home_ref_box = [None]                   # is_spotlight() reads the current reference

    # 1. boot: wait for the input daemon to reach SpringBoard
    booted = None
    while time.time() - t0 < a.boot_timeout:
        time.sleep(10)
        if "[input] connected to SpringBoard" in read_log(a.sd):
            booted = time.time() - t0
            break
        if proc.poll() is not None:
            break
    results = {"boot_seconds": booted, "cycles": [], "checks": {}}
    if booted is None:
        log("boot did not reach SpringBoard in %.0f s" % a.boot_timeout)
    else:
        if a.slider3d >= 0:
            # QEMU resets the machine shortly after QMP connects, which can
            # clear an early qom-set of mcu-3d-slider. Re-apply it now that the
            # guest is up; the ARM9 polls MCU reg 0x08 every 100 ms.
            q.cmd("qom-set", path="/machine", property="mcu-3d-slider", value=a.slider3d)
            log("3D slider set to %d after boot" % a.slider3d)
        log("SpringBoard + input up after %.0f s; waiting for a stable home screen" % booted)
        if a.unlock_first:
            # Some images come up locked (or with Spotlight left open): wake,
            # slide to unlock, retry a swallowed slide, then return from any
            # Spotlight page with a page swipe + Home (re-log 63/78).
            key("ret")
            time.sleep(3)
            for attempt in range(3):
                drag(40, 420, 300, 420, 0.8)
                time.sleep(5)
                if ImageStat.Stat(shot("unlock_first").convert("L")).mean[0] > 10:
                    break
            drag(280, 240, 40, 240)
            time.sleep(3)
            key("ret")
            time.sleep(4)
            log("unlock-first: %d slide attempt(s)" % (attempt + 1))
        # SpringBoard's icon warm-up marker is the earliest point at which the
        # screen can be the home screen. Re-log 113: a slow host could leave
        # the boot console lit and stable past the input daemon's connect, so
        # an un-gated reference test took *that* as home and reported every
        # later cycle as "stuck". Keep the console-colour test below as a
        # second line of defence (re-log 114: white console text), but never
        # skip the warm-up wait when applog is installed. A firmware without
        # the marker falls through after the timeout instead of hanging.
        warm_wait = time.time()
        while time.time() - warm_wait < 300 and "sbwarm" not in read_log(a.sd):
            time.sleep(5)
        if "sbwarm" in read_log(a.sd):
            log("SpringBoard icon warm-up ready after %.0f s" % (time.time() - t0))
        else:
            log("no [sbwarm] marker within 300 s; relying on the image tests")
        # reference: not black (the screen copy may be switching to the GPU,
        # soak 3) and unchanged across two shots 5 s apart.  On a loaded host
        # with --icount 2 SpringBoard's first lit frame came ~220 s after the
        # input daemon connected: allow 300 s (a black reference makes every
        # later diff meaningless and the unlock checks fail).
        prev, home_ref = None, None
        for _ in range(60):
            time.sleep(5)
            img = shot("home_ref")
            bright = ImageStat.Stat(img.convert("L")).mean[0]
            if (prev is not None and bright > 20 and not is_bootlog(img) and
                    ImageStat.Stat(ImageChops.difference(
                        img.convert("L"), prev.convert("L"))).mean[0] < 1.0):
                home_ref = img
                log("home reference after %.0f s: mean %.1f, stddev %.1f" %
                    (time.time() - t0, bright, ImageStat.Stat(img.convert("L")).stddev[0]))
                break
            prev = img
        dark_ref = False
        if home_ref is None:
            # A busy host can keep the first frame black for minutes. Comparing
            # later, lit screenshots with a black reference reports every cycle
            # as "not home"; re-acquire once the display comes up instead.
            log("no stable, non-black home screen within 300 s; re-acquiring later")
            home_ref, dark_ref = prev, True
        for i in range(a.cycles):
            home_ref_box[0] = None if dark_ref else home_ref
            x, y = targets[i % len(targets)]
            c = {"cycle": i, "tap": [x, y]}
            tap(x, y)
            c["settle"] = settles[i % len(settles)]
            # Wait for the app to *render*, not for a fixed time: on a loaded
            # --icount 2 host a tap can take far longer than the settle, and
            # the app's uniform gray launch image differs from the home screen
            # enough to look like a finished frame. Home pressed on a launch
            # image is ignored, which then made the whole cycle "stuck".
            start = time.time()
            app, prev = None, None
            while time.time() - start < 180:
                time.sleep(4)
                img = shot("c%03d_app" % i)
                g = img.convert("L")
                mean, sd = ImageStat.Stat(g).mean[0], ImageStat.Stat(g).stddev[0]
                if mean > 90 and mean < 170 and sd < 8:
                    prev = None             # a launch image / blank screen
                    continue
                differs = ImageStat.Stat(ImageChops.difference(
                    g, home_ref.convert("L"))).mean[0] > 12
                if differs and prev is not None and ImageStat.Stat(ImageChops.difference(
                        g, prev.convert("L"))).mean[0] < 1.0:
                    app = img
                    break
                prev = img
            if app is None:
                app = shot("c%03d_app" % i)
                log("cycle %d: no rendered app within 180 s" % i)
            c["app_wait"] = round(time.time() - start, 1)
            app_diff = ImageStat.Stat(ImageChops.difference(
                app.convert("L"), home_ref.convert("L"))).mean[0]
            if app_diff > 12:
                key("ret")                      # Home (START)
            else:
                c["no_app"] = True
                log("cycle %d: the tap did not open an app" % i)
            # wait until the screen stops changing (up to 30 s) instead of a
            # fixed 6 s: with --icount the guest runs slower when the host is
            # busy (other emulators), and a fixed wait screenshots the close
            # animation halfway, then the recovery's second Home opens Spotlight
            home, t_wait = settled("c%03d_home" % i)
            c["home_wait"] = round(t_wait, 1)
            if ((dark_ref or is_bootlog(home_ref)) and
                    ImageStat.Stat(home.convert("L")).mean[0] > 20 and
                    not is_bootlog(home)):
                # the display finally came up: this shot is just after Home,
                # so it is a valid home reference from now on
                home_ref, dark_ref = home.copy(), False
                c["home_reference_reacquired"] = True
                log("cycle %d: re-acquired a lit home screen reference" % i)
            # Home on the first page opens Spotlight on iOS 4; it is not a
            # stuck app. Dismiss it before measuring (re-log 63/86).
            if is_spotlight(home):
                home, n = dismiss_spotlight("c%03d_unspotlight" % i)
                c["spotlight_dismissed"] = True
                c["spotlight_attempts"] = n
            diff = ImageStat.Stat(ImageChops.difference(home.convert("L"), home_ref.convert("L"))).mean[0]
            c["home_diff"] = round(diff, 2)
            c["app_diff"] = round(app_diff, 2)
            # not home: Home on page 1 opens Spotlight (a second Home returns);
            # a Home press seen as a hold shows the "Edit Home Screen" alert
            # (needs Dismiss). Recover, but record what it took.
            def dist():
                # wait for the screen to settle: a fixed sleep was too short on
                # a busy host (Spotlight still sliding away when compared)
                img, _ = settled("c%03d_recover" % i, first=2.0)
                return ImageStat.Stat(ImageChops.difference(
                    img.convert("L"), home_ref.convert("L"))).mean[0]
            if diff > 12:
                # Look again before touching anything: one slow frame can
                # hold a close animation still for >2 s (3.1.3's GPU
                # compositor in QEMU, re-log 151), which settled() takes
                # for the end. Only a screen that is still wrong after
                # this wait needs (and is counted as) a recovery.
                time.sleep(6)
                late = dist()
                if late <= 12:
                    c["late_settle"] = round(diff, 2)
                    c["home_diff"] = round(late, 2)
                    diff = late
                    log("cycle %d: home screen arrived late (diff %.1f at the first look), no input needed" %
                        (i, c["late_settle"]))
            if diff > 12:
                c["recovered_with"] = "stuck"
                for step, act in (("page swipe + Home", lambda: (drag(280, 240, 40, 240),
                                                                 time.sleep(3), key("ret"))),
                                  ("clear query + Home", lambda: (tap(301, 30), time.sleep(2),
                                                                  drag(280, 240, 40, 240),
                                                                  time.sleep(3), key("ret"))),
                                  ("second Home", lambda: key("ret")),
                                  ("Dismiss", lambda: tap(160, 333)),
                                  # On a slow --icount 2 host a cycle can take
                                  # longer than iOS' auto-lock: Home wakes the
                                  # lock screen, the slide returns to the app
                                  # or the home screen.
                                  ("Home + unlock", lambda: (key("ret"), time.sleep(2),
                                                             drag(40, 420, 300, 420, 0.8))),
                                  ("Dismiss + Home", lambda: (tap(160, 333), time.sleep(3), key("ret")))):
                    act()
                    time.sleep(6)
                    if dist() <= 12:
                        c["recovered_with"] = step
                        break
            if not a.no_extras and i % 4 == 3:  # page swipes there and back
                drag(280, 240, 40, 240)
                # the second swipe must start on page 2: on a busy host a fixed
                # 3 s ran it while page 1 was still showing, which opened
                # Spotlight (soak FAIL 'stuck' with the keyboard up)
                settled("c%03d_swipe" % i)
                drag(40, 240, 280, 240)
                # until the page has stopped moving: a tap on a scrolling page
                # only stops it, and with the CA frames presented on the GPU
                # QEMU's PICA200 model makes a scroll take longer than 4 s of
                # wall time (re-log 77)
                settled("c%03d_swipe" % i)
            if not a.no_extras and i % 5 == 4:  # lock, wake, slide to unlock
                key("backspace")
                # every 10th cycle a long lock: iOS used to put the whole
                # system to sleep ~1 min after a lock with no way to wake it
                c["lock_seconds"] = a.long_lock if i % 10 == 9 else 4
                time.sleep(c["lock_seconds"])
                key("ret")
                time.sleep(5)
                # The first slide after a wake can be swallowed while the
                # input daemon is still polling at 10 Hz (re-log 75/78): the
                # screen stays black not because iOS hung, but because the
                # gesture never reached SpringBoard. Retry a missed slide
                # before measuring the unlock screen; wake again first if the
                # screen went dark (auto-lock on a long cycle).
                for attempt in range(5):
                    if ImageStat.Stat(shot("c%03d_unlock" % i).convert("L")).mean[0] < 5:
                        key("ret")
                        time.sleep(3)
                    drag(40, 420, 300, 420, 0.8)
                    settled("c%03d_unlock" % i, limit=45)  # the icons fly in after an unlock
                    u0 = shot("c%03d_unlock" % i).convert("L")
                    if ImageStat.Stat(u0).mean[0] > 5:
                        break
                c["unlock_attempts"] = attempt + 1
                # iOS unlocks to whatever was frontmost. If a previous Home
                # did not leave the app, comparing the app with the home
                # screen would report a false lock failure: press Home and
                # compare the home screen (a slide that never unlocked still
                # differs; the lock screen ignores Home).
                if ImageStat.Stat(u0).mean[0] > 5 and ImageStat.Stat(ImageChops.difference(
                        u0, home_ref.convert("L"))).mean[0] > 12:
                    # a slow frame can hold the unlock animation still for
                    # >2 s (see the Home check above): look again first, or
                    # the Home below lands on page 1 and opens Spotlight
                    time.sleep(6)
                    u0, _ = settled("c%03d_unlock" % i)
                    u0 = u0.convert("L")
                if ImageStat.Stat(u0).mean[0] > 5 and ImageStat.Stat(ImageChops.difference(
                        u0, home_ref.convert("L"))).mean[0] > 12:
                    key("ret")
                    time.sleep(2)
                    u0, _ = settled("c%03d_unlock_home" % i)
                    # Home on page 1 opens Spotlight on iOS 4 (re-log 100):
                    # the single Home above can leave the keyboard up after a
                    # wake, which looks exactly like a failed unlock to the
                    # comparison below. Dismiss it the same way as in the
                    # normal cycle path.
                    if is_spotlight(u0):
                        u0, n = dismiss_spotlight("c%03d_unspotlight_unlock" % i)
                        c["unlock_spotlight_attempts"] = n
                        c["unlock_spotlight_dismissed"] = True
                    u0 = u0.convert("L")
                # iOS fades the screen in after an unlock (and the emulated
                # backlight dims it): compare at the reference's brightness so
                # only content differences count (re-log 63)
                # blur: the GPU and the ARM9 CPU screen copy scale slightly
                # differently (after a wake either may be showing)
                u = u0.filter(ImageFilter.GaussianBlur(2))
                ref = home_ref.convert("L").filter(ImageFilter.GaussianBlur(2))
                mu, mr = ImageStat.Stat(u).mean[0], ImageStat.Stat(ref).mean[0]
                if mu > 5:
                    u = u.point(lambda v, k=mr / mu: min(255, int(v * k + 0.5)))
                c["after_unlock_diff"] = round(ImageStat.Stat(ImageChops.difference(u, ref)).mean[0], 2)
            log("cycle %d tap (%d,%d): app diff %.1f, home diff %.1f (home settled after %.0f s)" % (i, x, y, c["app_diff"], diff, c["home_wait"]))
            results["cycles"].append(c)
            if proc.poll() is not None:
                log("QEMU exited early")
                break
    try:
        q.cmd("quit")
        proc.wait(timeout=30)
    except Exception:
        proc.kill()
    text = read_log(a.sd)
    with open(os.path.join(a.out, "log.txt"), "w") as f:
        f.write(text)
    if a.crashlogs:
        r = subprocess.run([sys.executable, os.path.join(ROOT, "scripts", "crashlogs.py"),
                            a.sd, os.path.join(a.out, "crashlogs")], capture_output=True,
                           text=True, errors="replace")
        log("crashlogs: %s" % (r.stdout.strip() or r.stderr.strip()))

    failed = booted is None
    for name, rx, is_fail in CHECKS:
        hits = [l for l in text.splitlines() if re.search(rx, l)]
        results["checks"][name] = {"count": len(hits), "first": hits[0][:200] if hits else None,
                                   "failure": is_fail}
        if is_fail and hits:
            failed = True
    # The first "[input] connected" is the boot connection, not a restart
    # (re-log 63). A second one means SpringBoard or the input daemon died.
    sb_restarts = results["checks"]["SpringBoard restart"]["count"] - 1
    results["springboard_restarts"] = max(0, sb_restarts)
    if results["springboard_restarts"]:
        failed = True
    hb = [l for l in text.splitlines() if l.startswith("[hb] t=")]
    results["last_heartbeat"] = hb[-1] if hb else None
    # home screen not restored after Home = stuck app or broken compositor
    rec = {}
    for c in results["cycles"]:
        if "recovered_with" in c:
            rec.setdefault(c["recovered_with"], []).append(c["cycle"])
    results["recoveries"] = rec
    results["late_settles"] = [c["cycle"] for c in results["cycles"] if "late_settle" in c]
    stuck = rec.get("stuck", [])
    if stuck or len(rec.get("Dismiss + Home", [])) > 1:
        failed = True
    # a lock round must end on the home screen (after a long lock too)
    bad_unlock = [c["cycle"] for c in results["cycles"] if c.get("after_unlock_diff", 0) > 20]
    results["unlock_not_home"] = bad_unlock
    if bad_unlock:
        failed = True
    no_app = [c["cycle"] for c in results["cycles"] if c.get("no_app")]
    results["no_app"] = no_app
    if a.require_app and (no_app or not results["cycles"]):
        failed = True
    results["failed"] = failed
    with open(os.path.join(a.out, "report.json"), "w") as f:
        json.dump(results, f, indent=1)
    lines = ["# Soak report", "", "- boot to SpringBoard + input: %s" %
             ("%.0f s" % booted if booted else "**not reached**"),
             "- cycles run: %d" % len(results["cycles"]),
             "- SpringBoard/input daemon restarts after boot: %d" % results["springboard_restarts"],
             "- not home after Home (cycle lists by what it took to recover): %s" % (rec or "never"),
             "- home screen late but reached without input (cycles): %s" % (results["late_settles"] or "none"),
             "- taps that opened no app: %s%s" % (no_app or "none",
                                                  " (failure: --require-app)" if a.require_app and no_app else ""),
             "- lock rounds not ending on the home screen (cycles 9, 19, ... lock for %g s): %s" % (a.long_lock, bad_unlock or "none"),
             "- last heartbeat: `%s`" % results["last_heartbeat"], "", "| check | count | first |", "|---|---|---|"]
    for name, r in results["checks"].items():
        lines.append("| %s%s | %d | %s |" % (name, " (failure)" if r["failure"] else "", r["count"],
                                             ("`%s`" % r["first"]) if r["first"] else ""))
    lines += ["", "**Result: %s**" % ("FAIL" if failed else "PASS")]
    with open(os.path.join(a.out, "report.md"), "w") as f:
        f.write("\n".join(lines) + "\n")
    print("\n".join(lines))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
