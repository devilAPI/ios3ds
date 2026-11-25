#!/usr/bin/env python3
"""Settings > Wi-Fi end to end in QEMU, driven through the touch screen.

  emu/wifi_ui_test.py [--sd build/sd.img] [--out build/emu-wifi-ui] [--keep-cfg] [--nwm]

--nwm: a synthetic nwm.code (as the GodMode9 script leaves it) instead of
wifi-fw.bin, i.e. exactly the files a console has after docs/wifi.md
section 3; the ARM9 then cuts the firmware out at boot.

Puts a dummy wifi-fw.bin on the SD image and removes wifi.cfg (the ARM9
driver then brings the chip up idle and waits for iOS, docs/wifi.md section
10), boots with the ctr-wifi access point (plus its decoys NeighborNet and
CoffeeShop), opens Settings > Wi-Fi, checks that the scan lists all three
networks, taps ios3ds-qemu, types the passphrase on the on-screen keyboard,
taps Join and waits for the association, the DHCP lease and the status-bar
Wi-Fi icon. Screenshots of every step go to --out.

boot.cfg gets ca_gpu=0 for the run (CoreAnimation in software, the setting
the card uses): the GPU compositor's artifacts would make the screenshots
hard to read; they are not what this test is about. wifi.cfg, wifi-fw.bin and
boot.cfg are restored afterwards. Uses emu/session.py, so build/emu-session/
must not be in use.
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

SESSION = [sys.executable, os.path.join(HERE, "session.py")]
SDIR = os.path.join(ROOT, "build", "emu-session")
PSK = "ios3ds-test-passphrase"
# iOS points (320x480) on the screens this test passes through
SETTINGS_ICON = (40, 315)       # home screen page 1, row 4
WIFI_ROW = (160, 140)           # Settings: second row
NET_ROW = (93, 229)             # Wi-Fi Networks: second network (sorted: CoffeeShop, ios3ds-qemu, NeighborNet)
JOIN_KEY = (287, 457)           # password keyboard: Join


def sess(*args, check=True):
    r = subprocess.run(SESSION + list(args), capture_output=True, text=True)
    if check and r.returncode:
        raise SystemExit("session %s failed: %s" % (args[0], r.stderr.strip()))
    return r.stdout


def log_text():
    return sess("log")


def wait_log(pattern, timeout, since=0):
    t0 = time.time()
    while time.time() - t0 < timeout:
        m = re.search(pattern, log_text()[since:])
        if m:
            return m
        time.sleep(3)
    return None


def shot(out, name):
    sess("shot", name)
    src = os.path.join(SDIR, name + ".png")
    dst = os.path.join(out, name + ".png")
    shutil.copy(src, dst)
    return Image.open(dst).convert("L")


def diff(a, b):
    return ImageStat.Stat(ImageChops.difference(a, b)).mean[0]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--sd", default=os.path.join(ROOT, "build", "sd.img"))
    ap.add_argument("--out", default=os.path.join(ROOT, "build", "emu-wifi-ui"))
    ap.add_argument("--boot-timeout", type=float, default=400)
    ap.add_argument("--nwm", action="store_true", help="nwm.code instead of wifi-fw.bin")
    ap.add_argument("--keep-ca", action="store_true",
                    help="leave boot.cfg's ca_gpu alone (4.1: software CA draws a black home screen)")
    ap.add_argument("--psk", default=PSK,
                    help="access point passphrase typed in Settings (letters only avoids the "
                         "123 keyboard page, whose layout differs between iOS versions)")
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    tmp = tempfile.mkdtemp(prefix="ios3ds-wifi-ui-")
    cfg_saved = os.path.join(tmp, "boot.cfg")
    wifi_test.mtools(["mcopy", "-o", "::/ios3ds/boot.cfg", cfg_saved], a.sd)
    for f in ("wifi.cfg", "wifi-fw.bin", "nwm.code"):
        wifi_test.mtools(["mdel", "::/ios3ds/" + f], a.sd, check=False)
    if a.nwm:
        fw = os.path.join(tmp, "nwm.code")
        wifi_test.dummy_nwm(fw)
        wifi_test.mtools(["mcopy", "-o", fw, "::/ios3ds/nwm.code"], a.sd)
    else:
        fw = os.path.join(tmp, "wifi-fw.bin")
        wifi_test.dummy_fw(fw)
        wifi_test.mtools(["mcopy", "-o", fw, "::/ios3ds/wifi-fw.bin"], a.sd)
    cfg = os.path.join(tmp, "boot-ca0.cfg")
    with open(cfg_saved) as f, open(cfg, "w") as g:
        t = f.read()
        g.write(t if a.keep_ca else re.sub(r"(?m)^ca_gpu=1$", "ca_gpu=0", t))
    wifi_test.mtools(["mcopy", "-o", cfg, "::/ios3ds/boot.cfg"], a.sd)

    checks = []

    def check(name, ok, detail=""):
        checks.append((name, bool(ok)))
        print("[wifi-ui] %-34s %s %s" % (name, "PASS" if ok else "FAIL", detail))
        return ok

    t0 = time.time()
    try:
        if a.psk != PSK:
            sess("start", "--wifi", "--sd", a.sd, "--extra=-global ctr-wifi.psk=" + a.psk)
        else:
            sess("start", "--wifi", "--sd", a.sd)
        if not check("boot to SpringBoard", wait_log(r"\[input\] connected to SpringBoard", a.boot_timeout)):
            return 1
        check("chip idle without wifi.cfg", wait_log(r"\[wifi\] idle: no network configured", 30))
        check("WiFiManager got the interface",
              wait_log(r"\[a80211\] handing en\d to an IO80211Interface client", 120))
        # a stable home screen (as emu/soak.py)
        prev, home = None, None
        for _ in range(30):
            time.sleep(5)
            img = shot(a.out, "00-home")
            if prev is not None and ImageStat.Stat(img).mean[0] > 20 and diff(img, prev) < 1.0:
                home = img
                break
            prev = img
        home = home or prev
        # Settings > Wi-Fi; Home first (wakes a blanked screen, leaves page 2 / Spotlight)
        n0 = len(re.findall(r"scan done", log_text()))
        listed = False
        for attempt in range(4):
            sess("key", "h")
            time.sleep(4)
            if diff(shot(a.out, "01-before-settings"), home) > 15:
                sess("key", "h")            # Spotlight: back to the icons
                time.sleep(4)
            sess("tap", *map(str, SETTINGS_ICON))
            prev = None
            for _ in range(40):                     # Settings is slow to launch in QEMU:
                time.sleep(3)                       # wait until it is up and drawn
                img = shot(a.out, "02-settings")
                if diff(img, home) > 15 and prev is not None and diff(img, prev) < 1.0:
                    break
                prev = img
            time.sleep(15)                          # the launch image is stable too
            sess("tap", *map(str, WIFI_ROW))
            t1 = time.time()
            while time.time() - t1 < 45:
                time.sleep(3)
                if len(re.findall(r"scan done", log_text())) > n0:
                    listed = True
                    break
            if listed:
                break
            print("[wifi-ui] no scan after opening Settings (attempt %d), retrying" % (attempt + 1))
        if not check("Settings > Wi-Fi scans", listed):
            return 1
        time.sleep(8)
        shot(a.out, "03-wifi-list")
        text = log_text()
        m = re.findall(r"\[a80211\] scan done: (\d+) networks", text)
        check("scan lists 3 networks", m and int(m[-1]) >= 3, "(%s)" % (m[-1] if m else "none"))
        # join with the on-screen keyboard
        n_assoc = len(re.findall(r"association (done|failed)", text))
        sess("tap", *map(str, NET_ROW))
        time.sleep(15)
        shot(a.out, "04-password-prompt")
        first = True
        for chunk in re.findall(r"[a-z]+|[^a-z]+", a.psk):
            if first:
                sess("type", chunk, "--page", "abc")
                first = False
            else:
                sess("type", chunk)
            time.sleep(4)
        time.sleep(6)
        shot(a.out, "05-password-typed")
        sess("tap", *map(str, JOIN_KEY))
        t1 = time.time()
        res = None
        while time.time() - t1 < 90 and res is None:
            time.sleep(3)
            r = re.findall(r"\[a80211\] association (done|failed)", log_text())
            if len(r) > n_assoc:
                res = r[-1]
        check("association via Settings", res == "done", "(%s)" % res)
        check("WPA2 4-way handshake, link up", wait_log(r"\[wifi\] link up: ios3ds-qemu", 30))
        check("DHCP lease on en0", wait_log(r"\[wifid\] en\d configured by configd", 90))
        time.sleep(30)                      # Settings redraws slowly in QEMU
        shot(a.out, "06-joined")
        sess("key", "h")
        time.sleep(8)
        shot(a.out, "07-home-wifi-bars")
        return 0 if all(ok for _, ok in checks) else 1
    finally:
        sess("stop", check=False)
        time.sleep(3)
        with open(os.path.join(a.out, "log.txt"), "w") as f:
            f.write(log_text())
        wifi_test.mtools(["mcopy", "-o", cfg_saved, "::/ios3ds/boot.cfg"], a.sd)
        wifi_test.mtools(["mdel", "::/ios3ds/wifi-fw.bin"], a.sd, check=False)
        wifi_test.mtools(["mdel", "::/ios3ds/nwm.code"], a.sd, check=False)
        shutil.rmtree(tmp, ignore_errors=True)
        print("[wifi-ui] %d/%d checks passed in %.0f s; screenshots and log in %s"
              % (sum(ok for _, ok in checks), len(checks), time.time() - t0, a.out))


if __name__ == "__main__":
    sys.exit(main())
