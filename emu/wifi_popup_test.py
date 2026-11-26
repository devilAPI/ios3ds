#!/usr/bin/env python3
"""WPA2+WPA3 mixed networks from both iOS join paths, driven through the UI.

  python3 emu/wifi_popup_test.py [--case mixed|wpa2] [--out build/emu-wifi-popup]

Puts a dummy wifi-fw.bin on the SD image, removes wifi.cfg and boots with the
ctr-wifi access point. With --case mixed the AP announces the FRITZ!Box-style
RSN IE (AKM PSK + SAE, MFPC); wpa2 is a plain WPA2 AP. The same network is
joined twice:

  1. Settings > Wi-Fi > ios3ds-qemu (password typed on the iOS keyboard);
  2. after "Forget this Network", the SpringBoard "Select a Wi-Fi Network"
     alert, raised by opening Mail with no known network in range.

Both must reach [wifi] link up and [wifid] en0 configured by configd. The
IEs iOS sees are checked too: the SAE AKM must never reach WiFiManager, while
the ARM9 still logs its mixed-mode classification and joins with WPA2-PSK.
Screenshots of every step go to --out. Uses emu/session.py, so
build/emu-session/ must not be in use. boot.cfg, wifi-fw.bin and nwm.code are
restored afterwards.
"""
import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time

from PIL import Image, ImageChops, ImageStat

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(ROOT, "tools", "ios3ds-wifi"))
import wifi_test  # noqa: E402  (dummy firmware, mtools helpers)
import viewer     # noqa: E402  (QMP touch injection)
from session import key_xy, MODE_KEY, tap as qmp_tap  # noqa: E402

SESS = [sys.executable, os.path.join(HERE, "session.py")]
DIR = os.path.join(ROOT, "build", "emu-session")
SSID = "ios3ds-qemu"
PSK = "ios3ds-test-passphrase"


def sess(*args, check=True):
    r = subprocess.run(SESS + list(args), capture_output=True, text=True)
    if check and r.returncode:
        raise SystemExit("session %s failed: %s" % (args[0], r.stderr.strip()))
    return r.stdout


def log_text():
    return sess("log")


def wait_log(pat, timeout, since=0):
    t0 = time.time()
    while time.time() - t0 < timeout:
        m = re.search(pat, log_text()[since:])
        if m:
            return m
        time.sleep(3)
    return None


def shot(out, name):
    sess("shot", name)
    src = os.path.join(DIR, name + ".png")
    dst = os.path.join(out, name + ".png")
    shutil.copy(src, dst)
    return Image.open(dst).convert("L")


def diff(a, b):
    return ImageStat.Stat(ImageChops.difference(a, b)).mean[0]


def last_down():
    lines = [l for l in sess("log", r"\[input\] down").splitlines() if l.startswith("[input] down")]
    m = re.search(r"down (\d+),(\d+)", lines[-1]) if lines else None
    return (int(m.group(1)), int(m.group(2))) if m else None


def touch(q, x, y, hold=0.6, verify=True):
    """A tap with a long hold; the slow QEMU sometimes drops short ones."""
    want = (round(x), round(y))
    for _ in range(4 if verify else 1):
        qmp_tap(q, x, y, hold)
        time.sleep(0.8)
        got = last_down()
        if not verify or (got and abs(got[0] - want[0]) <= 2 and abs(got[1] - want[1]) <= 2):
            return True
    return False


def type_keyboard(text, page="abc"):
    q = viewer.QMP(os.path.join(DIR, "qmp.sock"))
    for ch in text:
        want, xy = key_xy(ch)
        if want and want != page:
            if not touch(q, *MODE_KEY, hold=0.8):
                return False
            page = want
        if not touch(q, *xy, hold=0.8):
            return False
    return True


def home_screen(out):
    """Press HOME until the icon grid is back and stable (Music auto-launches
    at boot on images with a test song, the first HOME can open Spotlight)."""
    for _ in range(6):
        sess("key", "h")
        time.sleep(8)
        prev = None
        for _ in range(6):
            img = shot(out, "home")
            mean = ImageStat.Stat(img).mean[0]
            if prev is not None and 15 < mean < 60 and diff(img, prev) < 1.0:  # icon grid, not a bright launch image
                return True
            prev = img
            time.sleep(5)
    return False


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--case", choices=("mixed", "wpa2"), default="mixed")
    ap.add_argument("--sd", default=os.path.join(ROOT, "build", "sd.img"))
    ap.add_argument("--out", default=os.path.join(ROOT, "build", "emu-wifi-popup"))
    ap.add_argument("--boot-timeout", type=float, default=900)
    a = ap.parse_args()
    out = a.out                 # rest of main() uses this for shots and logs
    os.makedirs(out, exist_ok=True)
    tmp = tempfile.mkdtemp(prefix="ios3ds-wifi-popup-")
    saved = os.path.join(tmp, "boot.cfg")
    wifi_test.mtools(["mcopy", "-o", "::/ios3ds/boot.cfg", saved], a.sd)
    for f in ("wifi.cfg", "nwm.code", "wifi-fw.bin"):
        wifi_test.mtools(["mdel", "::/ios3ds/" + f], a.sd, check=False)
    wifi_test.dummy_fw(os.path.join(tmp, "wifi-fw.bin"))
    wifi_test.mtools(["mcopy", "-o", os.path.join(tmp, "wifi-fw.bin"), "::/ios3ds/wifi-fw.bin"], a.sd)
    with open(saved) as f:
        cfg = f.read()
    cfg = re.sub(r"(?m)^ca_gpu=1$", "ca_gpu=0", cfg)
    if "ios3ds_applog" not in cfg:
        cfg = re.sub(r"(?m)^(cmdline=.*)$", lambda m: m.group(1) + " ios3ds_applog=1", cfg)
    with open(os.path.join(tmp, "b.cfg"), "w") as f:
        f.write(cfg)
    wifi_test.mtools(["mcopy", "-o", os.path.join(tmp, "b.cfg"), "::/ios3ds/boot.cfg"], a.sd)

    checks = []

    def check(name, ok, detail=""):
        checks.append((name, bool(ok)))
        print("[popup] %-36s %s %s" % (name, "PASS" if ok else "FAIL", detail), flush=True)
        return ok

    extra = "-global ctr-wifi.debug=1"
    if a.case == "mixed":
        extra += " -global ctr-wifi.security=wpa2-wpa3"
    t0 = time.time()
    try:
        sess("start", "--wifi", "--sd", a.sd, "--extra", extra)
        if not check("boot to SpringBoard", wait_log(r"\[input\] connected to SpringBoard", a.boot_timeout)):
            return 1
        check("chip idle without wifi.cfg", wait_log(r"\[wifi\] idle: no network configured", 60))
        if not check("home screen", home_screen(out)):
            return 1

        # ---- path 1: Settings > Wi-Fi ---------------------------------------
        listed = False
        for attempt in range(4):
            sess("key", "h")
            time.sleep(4)
            touch(viewer.QMP(os.path.join(DIR, "qmp.sock")), 40, 315, 0.15, False)
            time.sleep(30)
            n0 = len(re.findall(r"\[a80211\] scan done", log_text()))
            touch(viewer.QMP(os.path.join(DIR, "qmp.sock")), 160, 140, 0.15, False)
            t1 = time.time()
            while time.time() - t1 < 60:
                time.sleep(3)
                if len(re.findall(r"\[a80211\] scan done", log_text())) > n0:
                    listed = True
                    break
            if listed:
                break
            print("[popup] no scan after opening Settings (attempt %d), retrying" % (attempt + 1))
        if not check("Settings > Wi-Fi scans", listed):
            return 1
        shot(out, "01-wifi-list")
        q = viewer.QMP(os.path.join(DIR, "qmp.sock"))
        n_assoc = len(re.findall(r"\[a80211\] associate", log_text()))
        touch(q, 93, 229, 0.15, False)          # ios3ds-qemu
        time.sleep(12)
        shot(out, "02-settings-password")
        if not check("Settings typed the passphrase", type_keyboard(PSK)):
            return 1
        shot(out, "03-settings-typed")
        touch(q, 287, 457, 0.8)                 # Join
        t1 = time.time()
        while time.time() - t1 < 120 and len(re.findall(r"\[a80211\] associate", log_text())) <= n_assoc:
            time.sleep(3)
        text = log_text()
        check("Settings: associate reaches wifid",
              re.search(r"\[a80211\] associate \"%s\" \(WPA2-PSK" % SSID, text) is not None)
        check("Settings: link up", wait_log(r"\[wifi\] link up: " + SSID, 120))
        check("Settings: DHCP on en0", wait_log(r"\[wifid\] en\d configured by configd", 120))
        if a.case == "mixed":
            check("ARM9 falls back to WPA2-PSK",
                  re.search(r"\[wifi\] \".*\" is a WPA2 \+ WPA3 network", log_text()) is not None)
        time.sleep(30)
        shot(out, "04-settings-joined")

        # ---- forget the network ---------------------------------------------
        q = viewer.QMP(os.path.join(DIR, "qmp.sock"))
        touch(q, 290, 228, 0.15, False)         # blue disclosure arrow
        time.sleep(20)
        shot(out, "05-network-details")
        touch(q, 160, 97, 0.15, False)          # Forget this Network
        time.sleep(12)
        shot(out, "06-forget-confirmation")
        touch(q, 160, 367, 0.15, False)         # red Forget button
        check("network forgotten", wait_log(r"\[wifi\] iOS: leave " + SSID, 60))
        if not home_screen(out):
            return 1

        # ---- path 2: the SpringBoard Ask-to-Join popup ----------------------
        # Mail asks for the network as soon as it opens; no known network is
        # left in range, so SpringBoard shows "Select a Wi-Fi Network".
        touch(viewer.QMP(os.path.join(DIR, "qmp.sock")), 100, 445, 0.15, False)
        check("Ask-to-Join popup", wait_log(r"\[applog\] SpringBoard alert shown, title \"Select a Wi-Fi Network\"", 300))
        shot(out, "07-ask-to-join")
        touch(viewer.QMP(os.path.join(DIR, "qmp.sock")), 100, 242, 0.15, False)
        check("popup password prompt",
              wait_log(r"\[applog\] SpringBoard alert setTitle: \"Enter password for", 120))
        shot(out, "08-popup-password")
        n_assoc = len(re.findall(r"\[a80211\] associate", log_text()))
        if not check("popup typed the passphrase", type_keyboard(PSK)):
            return 1
        shot(out, "09-popup-typed")
        since = len(log_text())
        touch(viewer.QMP(os.path.join(DIR, "qmp.sock")), 210, 174, 0.8)   # Join
        t1 = time.time()
        while time.time() - t1 < 120 and len(re.findall(r"\[a80211\] associate", log_text()[since:])) == 0:
            time.sleep(3)
        text = log_text()[since:]
        check("popup: associate reaches wifid",
              re.search(r"\[a80211\] associate \"%s\" \(WPA2-PSK" % SSID, text) is not None)
        check("popup: link up", wait_log(r"\[wifi\] link up: " + SSID, 180, since))
        check("popup: association done", wait_log(r"\[a80211\] association done \(result 1\)", 180, since))
        # configd keeps the lease from path 1 on the same interface, so the
        # "configured by configd" line need not repeat on the second join.
        check("popup: IP address on en0", re.search(r"\[wifid\] en\d configured by configd", log_text()) is not None)
        check("no handshake failure", re.search(r"the WPA2 handshake failed|giving up on", text) is None)
        time.sleep(30)
        shot(out, "10-popup-joined")
        return 0 if all(ok for _, ok in checks) else 1
    finally:
        sess("stop", check=False)
        time.sleep(3)
        with open(os.path.join(a.out, "log.txt"), "w") as f:
            f.write(log_text())
        wifi_test.mtools(["mcopy", "-o", saved, "::/ios3ds/boot.cfg"], a.sd)
        wifi_test.mtools(["mdel", "::/ios3ds/wifi-fw.bin"], a.sd, check=False)
        shutil.rmtree(tmp, ignore_errors=True)
        print("[popup] %d/%d checks passed in %.0f s; screenshots and log in %s"
              % (sum(ok for _, ok in checks), len(checks), time.time() - t0, a.out), flush=True)


if __name__ == "__main__":
    sys.exit(main())
