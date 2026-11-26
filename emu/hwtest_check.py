#!/usr/bin/env python3
"""QEMU run of the one-boot hardware test kit (boot.cfg hwtest=, docs/real-hardware.md).

  emu/hwtest_check.py [--mode 2] [--sd build/sd.img] [--icount 2] [--timeout 1500]
  emu/hwtest_check.py --mode 0          control: hwtest off, no [hwtest] line may appear

Sets hwtest=<mode> in sd:/ios3ds/boot.cfg, adds the ctr-wifi model's access
point (wifi.cfg + dummy firmware, as emu/wifi_test.py does) so the Wi-Fi join
has a network, boots with emu/session.py and waits for "[hwtest] ==== END".
With --mode 1 it also answers the on-screen prompts it can (taps the first
button). Prints the summary block, saves a screenshot of the on-screen
summary to <out>/summary.png, and restores boot.cfg and the Wi-Fi files.
Exit 0 when the summary appeared (mode 1/2) or no [hwtest] line appeared
within --timeout (mode 0).
"""
import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "emu"))
import wifi_test  # noqa: E402


def sess(sd, *args, check=True):
    return subprocess.run([sys.executable, os.path.join(ROOT, "emu", "session.py"), "--sd", sd] + list(args),
                          capture_output=True, text=True, check=check)


def act(sd, prompt):
    print("[hwtest-check] answering prompt %r" % prompt)
    time.sleep(2)
    if prompt == "touch":
        for _ in range(3):
            sess(sd, "drag", "60", "240", "260", "250", "0.6", check=False)
            sess(sd, "drag", "260", "250", "60", "240", "0.6", check=False)
    elif prompt == "buttons":
        for k in ("a", "s", "x", "y", "q", "w", "up", "down", "left", "right"):
            sess(sd, "key", k, check=False)
    elif prompt == "volume":
        for v in ("0", "63", "32"):
            sess(sd, "set", "mcu-volume", v, check=False)
            time.sleep(1)
    elif prompt == "3D":
        for v in ("255", "0"):
            sess(sd, "set", "mcu-3d-slider", v, check=False)
            time.sleep(4)
    elif prompt == "HOME":
        sess(sd, "key", "h", check=False)
    elif prompt == "lock":
        time.sleep(3)
        sess(sd, "key", "p", check=False)
        time.sleep(8)
        sess(sd, "key", "p", check=False)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--mode", type=int, default=2, choices=(0, 1, 2))
    ap.add_argument("--sd", default=os.path.join(ROOT, "build", "sd.img"))
    ap.add_argument("--icount", default="")
    ap.add_argument("--timeout", type=float, default=1500)
    ap.add_argument("--out", default=os.path.join(ROOT, "build", "emu-hwtest"))
    ap.add_argument("--extra-cfg", default="", help="more boot.cfg lines, ';'-separated (e.g. 'stereo3d=1;gps=52.52,13.40')")
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    # the QEMU mic model is silent unless told otherwise (emu/qemu/hw/arm/ctr_csnd.c)
    os.environ.setdefault("IOS3DS_MIC", "sine:440:8000")
    tmp = tempfile.mkdtemp(dir=a.out)
    saved = os.path.join(tmp, "boot.cfg")
    wifi_test.mtools(["mcopy", "-o", "::/ios3ds/boot.cfg", saved], a.sd)
    cfg = open(saved).read()
    cfg = re.sub(r"(?m)^hwtest=.*\n?", "", cfg)
    if not cfg.endswith("\n"):
        cfg += "\n"
    if a.mode:
        cfg += "hwtest=%d\n" % a.mode
    for l in filter(None, a.extra_cfg.split(";")):
        cfg += l.strip() + "\n"
    with open(os.path.join(tmp, "b.cfg"), "w") as f:
        f.write(cfg)
    wifi_test.mtools(["mcopy", "-o", os.path.join(tmp, "b.cfg"), "::/ios3ds/boot.cfg"], a.sd)
    with open(os.path.join(tmp, "wifi.cfg"), "w") as f:
        f.write("ssid=%s\npsk=%s\n" % (wifi_test.SSID, wifi_test.PSK))
    wifi_test.dummy_fw(os.path.join(tmp, "wifi-fw.bin"))
    for f in ("wifi.cfg", "wifi-fw.bin"):
        wifi_test.mtools(["mcopy", "-o", os.path.join(tmp, f), "::/ios3ds/" + f], a.sd)
    wifi_test.mtools(["mdel", "::/ios3ds/log.txt"], a.sd, check=False)

    extra = "-nic user,model=ctr-wifi -global ctr-wifi.ssid=%s -global ctr-wifi.psk=%s" % (wifi_test.SSID, wifi_test.PSK)
    start = ["start", "--extra", extra] + (["--icount", a.icount] if a.icount else [])
    print(sess(a.sd, *start).stdout.strip())
    t0 = time.time()
    ok = False
    seen_prompts = 0
    text = ""
    try:
        while time.time() - t0 < a.timeout:
            time.sleep(10)
            text = sess(a.sd, "log", r"\[hwtest\]", check=False).stdout
            if a.mode == 1:
                # play the person for the prompts the harness can answer with
                # QEMU input; the yes/no alerts are left to time out (SKIP)
                for pr in re.findall(r"\[hwtest\] prompt: Hardware test: (\w+)", text)[seen_prompts:]:
                    seen_prompts += 1
                    act(a.sd, pr)
            if "==== END" in text:
                ok = True
                time.sleep(15)                  # the summary alert
                sess(a.sd, "shot", "summary", check=False)
                break
            if a.mode == 0 and "SpringBoard" in sess(a.sd, "log", "ios3ds-input", check=False).stdout \
                    and time.time() - t0 > 600:
                break
    finally:
        sess(a.sd, "stop", check=False)
        time.sleep(3)
        log = subprocess.run(["mtype", "-i", "%s@@%d" % (a.sd, 2048 * 512), "::/ios3ds/log.txt"],
                             capture_output=True).stdout.decode(errors="replace")
        with open(os.path.join(a.out, "log-mode%d.txt" % a.mode), "w") as f:
            f.write(log)
        wifi_test.mtools(["mcopy", "-o", saved, "::/ios3ds/boot.cfg"], a.sd)
        for f in ("wifi.cfg", "wifi-fw.bin"):
            wifi_test.mtools(["mdel", "::/ios3ds/" + f], a.sd, check=False)
        shutil.rmtree(tmp, ignore_errors=True)
    hw = [l for l in log.splitlines() if "[hwtest]" in l]
    print("\n".join(l for l in hw if "| " in l or "====" in l) or "(no summary)")
    shot = os.path.join(ROOT, "build", "emu-session", "summary.png")
    if os.path.exists(shot):
        os.replace(shot, os.path.join(a.out, "summary-mode%d.png" % a.mode))
    if a.mode == 0:
        good = not hw
        print("Result: %s (hwtest=0: %d [hwtest] lines)" % ("PASS" if good else "FAIL", len(hw)))
        return 0 if good else 1
    print("Result: %s" % ("PASS" if ok else "FAIL (no summary within %d s)" % a.timeout))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
