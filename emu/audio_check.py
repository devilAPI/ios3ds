#!/usr/bin/env python3
"""Audio checks for the older iOS versions in QEMU (headphones, keyboard click, song).

  python3 emu/audio_check.py --sd build/sd-4-1.img --ios 4.1 \
      --tests jack,click,song --out build/audio-41 --icount 2

Boots one image, opens Spotlight and Music, toggles the emulated headphone
jack, types on the on-screen keyboard and (where the Music UI accepts touches)
starts the test song. It checks the guest's sd:/ios3ds/log.txt and the QEMU
CSND/jack output, writes screenshots, a WAV of the mix and report.{md,json}.
Exit status 1 if a selected check failed. Each run needs a fresh SD image.

These are development checks, not shipped in images. 4.2.1 is unaffected:
the script is new and does not change the default build.
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

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
import viewer  # noqa: E402
from session import key_xy  # noqa: E402

PART = 2048 * 512

# iOS points in the 320x480 portrait screen. The dock/Music icon and the
# Spotlight gesture are SpringBoard-owned, so they work on every version even
# when an application's own purple port cannot be resolved in QEMU.
UI = {
    "4.2.1": {"music": (47, 440), "notes": (47, 273), "playlist": (60, 179),
              "song": (90, 75), "play": (213, 401), "artist": None,
              "kb_rows": (291, 347, 400), "spotlight": True},
    "4.1": {"music": (47, 440), "notes": (47, 273), "playlist": (60, 179),
            "song": (160, 127), "play": (213, 401), "artist": None,
            "kb_rows": (291, 347, 400), "spotlight": True},
    "3.1.3": {"music": (47, 440), "notes": (160, 273), "playlist": (60, 179),
              "song": (90, 75), "play": (213, 401), "artist": None,
              "kb_rows": (304, 360, 413), "spotlight": True},
    # 2.2.1 starts Music on the Artists tab; tap the artist, then the song.
    "2.2.1": {"music": (47, 440), "notes": (199, 226), "playlist": None,
              "song": (120, 83), "play": None, "artist": (60, 83),
              "kb_rows": (296, 347, 395), "spotlight": False},
}
DEFAULT_UI = dict(UI["4.2.1"], play=(213, 401))


def read_sd_text(sd, path="/ios3ds/log.txt"):
    r = subprocess.run(["mtype", "-i", "%s@@%d" % (sd, PART), "::" + path],
                       capture_output=True, text=True, errors="replace")
    return r.stdout


def count(text, pattern):
    return len(re.findall(pattern, text, re.M))


def first(text, pattern):
    m = re.search(pattern, text, re.M)
    return m.group(0)[:220] if m else None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--sd", default=os.path.join(ROOT, "build", "sd.img"))
    ap.add_argument("--ios", default="4.2.1")
    ap.add_argument("--out", default=os.path.join(ROOT, "build", "emu-audio"))
    ap.add_argument("--tests", default="jack,click,song")
    ap.add_argument("--icount", default="2")
    ap.add_argument("--qemu", default=os.environ.get("IOS3DS_QEMU", ""))
    ap.add_argument("--boot-timeout", type=float, default=1200)
    ap.add_argument("--settle", type=float, default=12.0)
    a = ap.parse_args()
    tests = [t.strip() for t in a.tests.split(",") if t.strip()]
    ui = UI.get(a.ios, DEFAULT_UI)
    os.makedirs(a.out, exist_ok=True)
    if a.qemu:
        os.environ["IOS3DS_QEMU"] = a.qemu
    import viewer as v
    v.QEMU = a.qemu or v.QEMU
    libdir = os.path.dirname(v.QEMU)
    if os.path.exists(os.path.join(libdir, "libslirp.so.0")):
        os.environ["LD_LIBRARY_PATH"] = libdir + os.pathsep + os.environ.get("LD_LIBRARY_PATH", "")
    wav = os.path.join(a.out, "mix.wav")
    os.environ["IOS3DS_CSND_WAV"] = wav

    # QMP's AF_UNIX path must be <108 bytes; use the short approved temp dir
    # instead of a (long, worktree-qualified) --out directory.
    tmp = tempfile.mkdtemp(prefix="ios3ds-audio-", dir="/tmp/opencode")
    # Start with an empty guest log: a previous QEMU run's "connected" line
    # would otherwise make wait_boot() return before this boot is ready.
    empty = os.path.join(tmp, "empty.log")
    open(empty, "wb").close()
    subprocess.run(["mcopy", "-o", "-i", "%s@@%d" % (a.sd, PART), empty,
                    "::/ios3ds/log.txt"], check=True, capture_output=True)
    sock = os.path.join(tmp, "qmp.sock")
    qout = open(os.path.join(a.out, "qemu.txt"), "w")
    cmd = [v.QEMU, "-M", "nintendo3ds", "-accel",
           "tcg,thread=single" if a.icount else "tcg,thread=multi",
           "-kernel", os.path.join(ROOT, "build", "boot", "ios3ds.firm"),
           "-drive", "file=%s,if=sd,format=raw" % a.sd, "-serial", "none",
           "-display", "none", "-qmp", "unix:%s,server,nowait" % sock]
    if a.icount:
        cmd += ["-icount", "shift=%s,sleep=off" % a.icount]
    proc = subprocess.Popen(cmd, stdout=qout, stderr=subprocess.STDOUT)
    q = v.QMP(sock)
    t0 = time.time()

    def log(msg):
        print("[audio %6.0fs] %s" % (time.time() - t0, msg), flush=True)

    def guest():
        return read_sd_text(a.sd)

    def qemu():
        with open(os.path.join(a.out, "qemu.txt"), errors="replace") as f:
            return f.read()

    def shot(name):
        f = os.path.join(tmp, "s.ppm")
        q.cmd("screendump", filename=f)
        img = Image.open(f).crop((20, 0, 380, 240)).rotate(-90, expand=True)
        img.save(os.path.join(a.out, name + ".png"))
        return img

    def tap(x, y, hold=0.18):
        dx, dy = v.ios_to_display(x / 320.0, y / 480.0)
        q.pointer(dx, dy, True)
        time.sleep(hold)
        q.pointer(dx, dy, False)

    def drag(x0, y0, x1, y1, secs=0.7):
        dx, dy = v.ios_to_display(x0 / 320.0, y0 / 480.0)
        q.pointer(dx, dy, True)
        time.sleep(0.15)
        for i in range(1, 11):
            time.sleep(secs / 10)
            f = i / 10.0
            dx, dy = v.ios_to_display((x0 + (x1 - x0) * f) / 320.0,
                                      (y0 + (y1 - y0) * f) / 480.0)
            q.pointer(dx, dy, None)
        time.sleep(0.05)
        q.pointer(dx, dy, False)

    def key(qcode, hold=0.2):
        q.key(qcode, True)
        time.sleep(hold)
        q.key(qcode, False)

    def type_text(text):
        rows = ui["kb_rows"]
        for ch in text:
            _, xy = key_xy(ch)
            row = min(range(3), key=lambda i: abs(xy[1] - (291, 347, 400)[i]))
            tap(xy[0], rows[row], hold=0.18)
            time.sleep(1.5)

    def wait(pattern, timeout, source=guest):
        end = time.time() + timeout
        while time.time() < end:
            if re.search(pattern, source(), re.M):
                return True
            if proc.poll() is not None:
                return False
            time.sleep(3)
        return False

    home_ref = None

    def home():
        # One Home is enough from an app; four presses land in Spotlight on
        # iOS 4 and make the keyboard check start from an arbitrary page.
        nonlocal home_ref
        key("ret")
        time.sleep(3)
        img = shot("home")
        for _ in range(30):
            time.sleep(4)
            img = shot("home")
            bright = ImageStat.Stat(img.convert("L")).mean[0]
            if bright > 10:
                home_ref = img.convert("L")
                return img
        return img

    def ensure_home():
        """Clear Spotlight (query + keyboard) and get back to the icon grid."""
        for attempt in range(5):
            img = shot("ensure_home_%d" % attempt).convert("L")
            if home_ref is not None and ImageStat.Stat(ImageChops.difference(
                    img, home_ref)).mean[0] <= 12:
                return True
            tap(301, 30)               # query's X, harmless when not searching
            time.sleep(2)
            drag(280, 240, 40, 240)
            time.sleep(2)
            key("ret")
            time.sleep(4)
        return False

    results = {"ios": a.ios, "tests": {}, "boot_seconds": None}
    booted = wait(r"connected to (SpringBoard|the SpringBoard's|SpringBoard's Purple)", a.boot_timeout)
    if booted:
        results["boot_seconds"] = round(time.time() - t0, 1)
        log("SpringBoard + input up after %.0f s" % results["boot_seconds"])
        home()

    base = guest()
    base_q = qemu()

    if "jack" in tests:
        r = {"lines": []}
        tap(*ui["music"])
        time.sleep(a.settle)
        shot("jack_music")
        b_in = count(base, r"\[audio\] headphones plugged in")
        b_out = count(base, r"\[audio\] headphones unplugged")
        b_route = count(base, r"\[audio\] iOS route ->")
        b_m_in = count(base_q, r"\[jack\] headphones plugged in")
        b_m_out = count(base_q, r"\[jack\] headphones unplugged")
        for _ in range(2):
            q.cmd("qom-set", path="/machine", property="headphones", value=True)
            time.sleep(5)
            q.cmd("qom-set", path="/machine", property="headphones", value=False)
            time.sleep(5)
        text, qt = guest(), qemu()
        r.update({
            "model_in": count(qt, r"\[jack\] headphones plugged in") - b_m_in,
            "model_out": count(qt, r"\[jack\] headphones unplugged") - b_m_out,
            "guest_in": count(text, r"\[audio\] headphones plugged in") - b_in,
            "guest_out": count(text, r"\[audio\] headphones unplugged") - b_out,
            "routes": count(text, r"\[audio\] iOS route ->") - b_route,
            "shim": count(text, r"ios3ds-audio: headphones (in|out), jack interrupt"),
        })
        r["lines"] = [l for l in text.splitlines() if re.search(
            r"headphones plugged in|headphones unplugged|iOS route ->|ios3ds-audio: headphones", l)][-12:]
        r["ok"] = (r["model_in"] >= 1 and r["model_out"] >= 1 and
                   r["guest_in"] >= 1 and r["guest_out"] >= 1 and r["routes"] >= 2)
        log("jack: model %d/%d, guest %d/%d, routes %d -> %s" %
            (r["model_in"], r["model_out"], r["guest_in"], r["guest_out"], r["routes"],
             "PASS" if r["ok"] else "FAIL"))
        results["tests"]["jack"] = r
        home()

    if "click" in tests:
        r = {"lines": []}
        if ui["spotlight"]:
            # Spotlight is a SpringBoard keyboard; retry the page swipe until
            # the search field appears (short touches are sometimes dropped
            # on a loaded --icount guest).
            for attempt in range(6):
                drag(40, 240, 280, 240, 0.8)
                time.sleep(6)
                img = shot("click_keyboard").convert("L")
                if ImageStat.Stat(img.crop((0, 0, 240, 30))).mean[0] > 35:
                    break
            tap(160, 40)                   # focus the search field
            time.sleep(5)
        else:
            tap(*ui["notes"])
            time.sleep(12)
        shot("click_keyboard")
        text = guest()
        b_stream = count(text, r"\[audio\] iOS output stream #")
        b_play = count(qemu(), r"\[csnd\] ch0 play")
        type_text("asd")
        time.sleep(6)
        shot("click_typed")
        text, qt = guest(), qemu()
        r.update({
            "streams": count(text, r"\[audio\] iOS output stream #") - b_stream,
            "csnd_starts": count(qt, r"\[csnd\] ch0 play") - b_play,
        })
        r["lines"] = [l for l in text.splitlines()[-200:] if re.search(
            r"iOS output stream|stream stopped|iOS volume|headphones", l)][-8:]
        # A UI click is short enough that the CSND channel can still be the
        # boot one; the new output stream is the primary evidence.
        r["ok"] = booted and r["streams"] >= 1
        log("keyboard click: %d stream(s), %d CSND start(s) -> %s" %
            (r["streams"], r["csnd_starts"], "PASS" if r["ok"] else "FAIL"))
        results["tests"]["click"] = r
        home()

    if "song" in tests:
        r = {"lines": []}
        if not ensure_home():
            log("could not return to the home screen before the song test")
        tap(*ui["music"])
        wait(r"Updating Library", 60)  # informational; may be too fast to catch
        time.sleep(a.settle)
        shot("song_library")
        text = guest()
        b_stream = count(text, r"\[audio\] iOS output stream #")
        b_play = count(qemu(), r"\[csnd\] ch0 play")
        # 4.x shows the visible playlist; 2.x opens on the Artists tab. The
        # per-version input daemon fallback resolves the app's purple port.
        if ui["artist"]:
            tap(*ui["artist"])
            time.sleep(5)
            shot("song_artist")
        elif ui["playlist"]:
            tap(*ui["playlist"])
            time.sleep(5)
            shot("song_playlist")
        tap(*ui["song"])
        time.sleep(12)
        shot("song_playing")
        text = guest()
        if (count(text, r"\[audio\] iOS output stream #") == b_stream
                and ui.get("play")):
            # 8B117's now-playing view opens paused; the Play control is the
            # last row at (160,301) in the 240x360 upright screenshot.
            tap(*ui["play"])
            time.sleep(12)
            shot("song_after_play")
        text, qt = guest(), qemu()
        r.update({
            "streams": count(text, r"\[audio\] iOS output stream #") - b_stream,
            "csnd_starts": count(qt, r"\[csnd\] ch0 play") - b_play,
            "csnd_stats": [l for l in qt.splitlines() if l.startswith("[csnd] ") and "non-silent" in l][-4:],
        })
        r["lines"] = [l for l in text.splitlines()[-200:] if re.search(
            r"iOS output stream|stream stopped", l)][-8:]
        r["ok"] = r["streams"] >= 1 and r["csnd_starts"] >= 1
        log("song: %d stream(s), %d CSND start(s) -> %s" %
            (r["streams"], r["csnd_starts"], "PASS" if r["ok"] else "FAIL"))
        results["tests"]["song"] = r

    try:
        q.cmd("quit")
        proc.wait(timeout=30)
    except Exception:
        proc.kill()
    qout.close()
    results["guest_log"] = os.path.join(a.out, "guest.log")
    with open(results["guest_log"], "w") as f:
        f.write(guest())
    results["wav"] = wav if os.path.exists(wav) else None
    if results["wav"]:
        try:
            import wave
            w = wave.open(wav, "rb")
            rate = w.getframerate()
            w.setpos(max(0, w.getnframes() - 8 * rate))
            data = w.readframes(w.getnframes() - max(0, w.getnframes() - 8 * rate))
            samples = __import__("array").array("h")
            samples.frombytes(data[:len(data) // 2 * 2])
            results["wav_tail_peak"] = max((abs(x) for x in samples), default=0)
            results["wav_frames"] = w.getnframes()
        except Exception as e:
            results["wav_tail_peak"] = None
            results["wav_error"] = str(e)
    results["failed"] = not booted or any(not r["ok"] for r in results["tests"].values())
    with open(os.path.join(a.out, "report.json"), "w") as f:
        json.dump(results, f, indent=1)
    lines = ["# Audio check report", "", "- iOS: %s" % a.ios,
             "- boot to input: %s" % (results["boot_seconds"] or "**not reached**")]
    if results.get("wav_tail_peak") is not None:
        lines.append("- CSND mix: %d frames, last 8 s peak %d" %
                     (results.get("wav_frames", 0), results["wav_tail_peak"]))
    for name, r in results["tests"].items():
        lines.append("- %s: **%s**" % (name, "PASS" if r["ok"] else "FAIL"))
        for k in ("model_in", "model_out", "guest_in", "guest_out", "routes", "streams", "csnd_starts"):
            if k in r:
                lines.append("  - %s: %d" % (k, r[k]))
        for l in r["lines"][-5:]:
            lines.append("  - `%s`" % l)
    lines.append("")
    lines.append("**Result: %s**" % ("FAIL" if results["failed"] else "PASS"))
    with open(os.path.join(a.out, "report.md"), "w") as f:
        f.write("\n".join(lines) + "\n")
    print("\n".join(lines))
    return 1 if results["failed"] else 0


if __name__ == "__main__":
    sys.exit(main())
