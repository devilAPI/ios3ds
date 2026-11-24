#!/usr/bin/env python3
"""End-to-end Wi-Fi test in QEMU (docs/wifi.md section 7).

  emu/wifi_test.py [--time 450] [--icount 2] [--case ok|nofw|nocfg|wrongpsk|open]
                   [--port 8080] [--shots 400] [--out build/emu-wifi]

Puts sd:/ios3ds/wifi.cfg and a *dummy* firmware container (random bytes in
the layout tools/ios3ds-wifi/nwmfw.py writes; the QEMU model never runs
firmware) into build/sd.img, starts a small HTTP server on the host
(reachable from iOS as http://10.0.2.2:<port>/ through slirp), boots with
the ctr-wifi model's access point bridged to "-nic user", then checks the
log for every step: SDIO -> BMI -> HTC/WMI -> scan -> associate -> 4-way ->
keys -> iOS interface -> DHCP lease -> DNS -> HTTP fetch.

The files are removed from the image afterwards (so the image keeps the
"no wifi.cfg" state), unless --keep.

Cases: ok (WPA2, everything must pass), credit (4 MiB bulk fetch through the
HTC credit path), open (open network), nocfg (no wifi.cfg: Wi-Fi must stay
off and nothing may touch the card), nofw (no firmware file: one log line,
Wi-Fi off), wrongpsk (the 4-way handshake must fail at message 2, no link),
tkip (WPA2 with a TKIP group key), wpa1 (WPA v1 vendor IE, TKIP pairwise),
nwm (no wifi-fw.bin: the ARM9 cuts the firmware out of a synthetic,
BLZ-compressed sd:/ios3ds/nwm.code itself, as after the GodMode9 script;
plus the [wifi-diag] lines), badnwm (an nwm.code that is not NWM: one error
line, Wi-Fi off, card untouched).
"""
import argparse
import http.server
import os
import random
import re
import subprocess
import sys
import threading

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools", "ios3ds-wifi"))
sys.path.insert(0, os.path.join(ROOT, "tools", "ios3ds-wifi", "tests"))
import nwmfw  # noqa: E402
import test_nwmfw  # noqa: E402  (synthetic NWM .code: random bytes, no Nintendo code)

SSID = "ios3ds-qemu"
PSK = "ios3ds-test-passphrase"
TOKEN = "ios3ds-wifi-test-%08x" % random.getrandbits(32)
PART = 2048 * 512
hits = []
HTTPS = []


def mtools(args, sd, check=True):
    return subprocess.run(args[:1] + ["-i", "%s@@%d" % (sd, PART)] + args[1:],
                          capture_output=True, check=check)


def dummy_fw(path):
    rnd = random.Random(1)
    parts = {"stub_data": bytes(rnd.getrandbits(8) for _ in range(0x38)),
             "stub_code": bytes(rnd.getrandbits(8) for _ in range(0x316)),
             "database": bytes(rnd.getrandbits(8) for _ in range(0x1E8)),
             "main": bytes(rnd.getrandbits(8) for _ in range(0x9A00))}
    with open(path, "wb") as f:
        f.write(nwmfw.pack(parts))


def dummy_nwm(path):
    """a synthetic NWM .code with GBATEK's newer literal pool, BLZ-compressed
    like the real ExeFS .code (GodMode9's extrcode writes it decompressed;
    the ARM9 handles both)"""
    class Few(random.Random):               # compressible, like ARM code
        def getrandbits(self, k):
            return (0x00, 0x01, 0xE5, 0x9F)[int(self.random() * 4)]
    code, _ = test_nwmfw.make_code("newer", Few(7))
    with open(path, "wb") as f:
        f.write(test_nwmfw.blz_compress(code, 0x200))


BULK_SIZE = 4 * 1024 * 1024         # several MB through the HTC credit path
BULK_BLOCK = bytes(i % 251 for i in range(251 * 256))  # exact multiple of the pattern


class Handler(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        hits.append(self.path)
        if self.path == "/bulk":
            self.send_response(200)
            self.send_header("Content-Type", "application/octet-stream")
            self.send_header("Content-Length", str(BULK_SIZE))
            self.end_headers()
            left = BULK_SIZE
            while left > 0:
                n = min(left, len(BULK_BLOCK))
                self.wfile.write(BULK_BLOCK[:n])
                left -= n
            return
        body = ("%s path=%s\n" % (TOKEN, self.path) + "".join(u + "\n" for u in HTTPS)).encode()
        self.send_response(200)
        self.send_header("Content-Type", "text/plain")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, fmt, *args):
        print("[http] %s %s" % (self.address_string(), fmt % args))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--case", default="ok",
                    choices=("ok", "open", "nocfg", "nofw", "wrongpsk", "nofifo", "hidden", "tkip", "mixed", "wpa3", "rekey",
                             "credit", "wpa1", "settings", "nwm", "badnwm"))
    ap.add_argument("--time", type=float, default=450)
    ap.add_argument("--icount", default="")
    ap.add_argument("--port", type=int, default=8080)
    ap.add_argument("--sd", default=os.path.join(ROOT, "build", "sd.img"))
    ap.add_argument("--out", default=os.path.join(ROOT, "build", "emu-wifi"))
    ap.add_argument("--shots", default="")
    ap.add_argument("--hmp", default="", help="HMP commands at guest seconds, passed to emu/run.py")
    ap.add_argument("--dns", default="example.com", help="name iOS resolves in the self-test")
    ap.add_argument("--debug", default="1", help="ctr-wifi model log level (0-3)")
    ap.add_argument("--keep", action="store_true", help="leave wifi.cfg / wifi-fw.bin on the image")
    ap.add_argument("--https", action="append", default=[], help="https:// URL iOS fetches via CFNetwork")
    ap.add_argument("--expect-https", choices=("ok", "any"), default="any")
    a = ap.parse_args()
    if a.case == "credit" and a.time == 450:
        a.time = 900                # a 4 MiB transfer takes a while under TCG
    HTTPS.extend(a.https)
    os.makedirs(a.out, exist_ok=True)

    cfg = os.path.join(a.out, "wifi.cfg")
    fw = os.path.join(a.out, "wifi-fw.bin")
    nwm = os.path.join(a.out, "nwm.code")
    for f in ("wifi.cfg", "wifi-fw.bin", "nwm.code"):
        mtools(["mdel", "::/ios3ds/" + f], a.sd, check=False)
    if a.case == "settings":                 # no wifi.cfg: iOS Settings drive the chip
        dummy_fw(fw)
        mtools(["mcopy", "-o", fw, "::/ios3ds/wifi-fw.bin"], a.sd)
    elif a.case != "nocfg":
        lines = ["ssid=" + SSID, "verbose=1",
                 "nettest=http://10.0.2.2:%d/ios3ds-nettest %s%s" %
                 (a.port, a.dns, " bulk" if a.case == "credit" else "")]
        if a.case == "open":
            lines.append("security=open")
        else:
            lines.append("psk=" + (PSK if a.case != "wrongpsk" else "not-the-passphrase"))
        with open(cfg, "w") as f:
            f.write("\n".join(lines) + "\n")
        mtools(["mcopy", "-o", cfg, "::/ios3ds/wifi.cfg"], a.sd)
        if a.case == "nwm":
            dummy_nwm(nwm)
            mtools(["mcopy", "-o", nwm, "::/ios3ds/nwm.code"], a.sd)
        elif a.case == "badnwm":
            with open(nwm, "wb") as f:
                f.write(bytes(random.Random(3).getrandbits(8) for _ in range(300000)))
            mtools(["mcopy", "-o", nwm, "::/ios3ds/nwm.code"], a.sd)
        elif a.case != "nofw":
            dummy_fw(fw)
            mtools(["mcopy", "-o", fw, "::/ios3ds/wifi-fw.bin"], a.sd)

    srv = http.server.ThreadingHTTPServer(("127.0.0.1", a.port), Handler)
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    print("[wifi-test] case %s, HTTP on 127.0.0.1:%d (iOS: 10.0.2.2:%d), token %s"
          % (a.case, a.port, a.port, TOKEN))

    extra = ["-nic", "user,model=ctr-wifi",
             "-global", "ctr-wifi.ssid=" + SSID,
             "-global", "ctr-wifi.psk=" + PSK,
             "-global", "ctr-wifi.debug=" + a.debug]
    if a.case == "open":
        extra += ["-global", "ctr-wifi.security=open"]
    if a.case == "nofifo":
        extra += ["-global", "ctr-wifi.fifo32=off"]
    if a.case == "hidden":
        extra += ["-global", "ctr-wifi.hidden=on"]
    if a.case == "tkip":
        extra += ["-global", "ctr-wifi.group=tkip"]
    if a.case == "wpa1":
        extra += ["-global", "ctr-wifi.security=wpa1"]
    if a.case == "mixed":                   # FRITZ!Box "WPA2 + WPA3" (hardware run 22)
        extra += ["-global", "ctr-wifi.security=wpa2-wpa3"]
    if a.case == "wpa3":
        extra += ["-global", "ctr-wifi.security=wpa3"]
    if a.case == "rekey":                   # group key rekeys (hardware run 23: reason 16)
        extra += ["-global", "ctr-wifi.rekey=30"]
    cmd = [sys.executable, os.path.join(ROOT, "emu", "run.py"), "--time", str(a.time),
           "--sd", a.sd, "--out", a.out, "--extra", " ".join(extra)]
    if a.icount:
        cmd += ["--icount", a.icount]
    if a.shots:
        cmd += ["--shots", a.shots]
    if a.hmp:
        cmd += ["--hmp", a.hmp]
    r = subprocess.run(cmd, capture_output=True, text=True)
    out = r.stdout + r.stderr
    with open(os.path.join(a.out, "run.txt"), "w") as f:
        f.write(out)
    srv.shutdown()
    if not a.keep:
        for f in ("wifi.cfg", "wifi-fw.bin", "nwm.code"):
            mtools(["mdel", "::/ios3ds/" + f], a.sd, check=False)

    def has(pat):
        return re.search(pat, out, re.M) is not None

    checks = {
        "ok": [
            ("SDIO up, AR6014", r"\[wifi\] SDIO up, chip id 0d000001"),
            ("FIFO check", r"\[wifi\] TMIO2 32-bit FIFO works"),
            ("HTC control not charged", r"\[wifi-diag\] .*htc: \d+ endpoint-0 control messages \(CMD53\), 0 data credits charged"),
            ("BMI ROM version", r"\[wifi\] BMI ROM version 2300006f"),
            ("model: firmware running", r"ctr-wifi: .*firmware running"),
            ("WMI ready", r"\[wifi\] WMI ready, MAC"),
            ("scan found the AP", r"ctr-wifi: .*BSSINFO"),
            ("associated", r"ctr-wifi: .*associated with"),
            ("4-way: message 4 OK", r"ctr-wifi: .*message 4 OK"),
            ("pairwise key = AP TK", r"pairwise.*equals the AP's TK"),
            ("group key = AP GTK", r"group.*equals the AP's GTK"),
            ("ARM9 link up", r"\[wifi\] link up"),
            ("uplink traffic", r"ctr-wifi: .*first frame from the station"),
            ("iOS interface", r"\[wifid\] interface is en\d+"),
            ("DHCP lease", r"\[nettest\] .*address 10\.0\.2\.\d+"),
            ("default route", r"\[nettest\] default route via 10\.0\.2\.2"),
            ("DNS", r"\[nettest\] dns " + re.escape(a.dns) + r" -> \d+\.\d+\.\d+\.\d+"),
            ("self-test passed", r"\[nettest\] all steps passed"),
            ("HTTP fetch", r"\[nettest\] http .*" + TOKEN),
        ],
        "open": [
            ("associated (open)", r"ctr-wifi: .*associated with"),
            ("ARM9 link up", r"\[wifi\] link up"),
            ("DHCP lease", r"\[nettest\] .*address 10\.0\.2\.\d+"),
            ("HTTP fetch", r"\[nettest\] http .*" + TOKEN),
        ],
        "nofifo": [
            ("FIFO check fails", r"\[wifi\] TMIO2 32-bit FIFO check failed"),
            ("endpoint 0 not charged", r"\[wifi-diag\] .*htc: \d+ endpoint-0 control messages \(CMD52\), 0 data credits charged"),
            ("CMD52 path reaches WMI", r"\[wifi\] CMD52 mailbox path: HTC and WMI came up without the 32-bit FIFO"),
            ("model: firmware running", r"ctr-wifi: .*firmware running"),
            ("4-way: message 4 OK", r"ctr-wifi: .*message 4 OK"),
            ("ARM9 link up", r"\[wifi\] link up"),
        ],
        "hidden": [
            ("probe response found", r"ctr-wifi: .*BSSINFO.*probe response"),
            ("ARM9 link up", r"\[wifi\] link up"),
        ],
        "mixed": [
            ("mixed network detected", r"\[wifi\] .* is a WPA2 \+ WPA3 network: joining with WPA2-PSK"),
            ("4-way: message 4 OK", r"4-way: message 4 OK"),
            ("ARM9 link up", r"\[wifi\] link up"),
        ],
        "credit": [
            ("HTC credits used", r"\[wifi-diag\] .*htc: credits so far: used [1-9][0-9]*"),
            ("credit reports applied", r"\[wifi-diag\] .*htc: credit report: \+[1-9]"),
            ("model returned credits", r"ctr-wifi: .*HTC credits: consumed [1-9][0-9]*, returned [1-9]"),
            ("4 MiB bulk transfer", r"\[nettest\] bulk \d+ bytes \(%d body, 0 bad\).*PASS" % BULK_SIZE),
            ("HTTP fetch", r"\[nettest\] http .*" + TOKEN),
            ("self-test passed", r"\[nettest\] all steps passed"),
        ],
        "wpa1": [
            ("WPA v1 selected", r"\[wifi\] connect .*WPA-PSK/TKIP"),
            ("WPA vendor IE used", r"WPA v1 vendor IE"),
            ("pairwise TKIP key", r"pairwise.*equals the AP's TK"),
            ("group TKIP key", r"group.*equals the AP's GTK"),
            ("4-way: message 4 OK", r"4-way: message 4 OK"),
            ("ARM9 link up", r"\[wifi\] link up"),
            ("self-test passed", r"\[nettest\] all steps passed"),
        ],
        "rekey": [
            ("ARM9 link up", r"\[wifi\] link up"),
            ("rekey 1 answered", r"ctr-wifi: .*group rekey: message 2 OK \(rekey 1\)"),
            ("rekey 2 answered", r"ctr-wifi: .*group rekey: message 2 OK \(rekey 2\)"),
            ("new group key installed", r"\[wifi\] group key renewed by the AP"),
        ],
        "wpa3": [
            ("WPA3-only refused once", r"\[wifi\] .* is WPA3-only"),
        ],
        "tkip": [
            ("TKIP group seen", r"\[wifi\] connect .*TKIP group"),
            ("group key = AP GTK", r"group.*equals the AP's GTK"),
            ("ARM9 link up", r"\[wifi\] link up"),
        ],
        "settings": [
            ("chip idle without wifi.cfg", r"\[wifi\] idle: no network configured"),
            ("a80211 loaded in configd", r"\[a80211\] loaded"),
        ],
        "nwm": [
            ("firmware from nwm.code", r"\[wifi\] firmware from 0:/ios3ds/nwm\.code: \d+ bytes \(BLZ-compressed\), main type4"),
            ("diag: pool + sizes", r"\[wifi-diag\] .*fw: newer pool at \.code\+[0-9a-f]+, text base 100000"),
            ("diag: power readback", r"\[wifi-diag\] .*power: CFG11_WIFICNT \S+ -> 01"),
            ("diag: CMD5/OCR", r"\[wifi-diag\] .*sdio: CMD5 x\d+: .*ready 1"),
            ("diag: CIS Atheros", r"\[wifi-diag\] .*sdio: CIS manufacturer 0271"),
            ("diag: BMI version", r"\[wifi-diag\] .*bmi: target version 2300006f \(AR6014 ROM\)"),
            ("diag: main uploaded", r"\[wifi-diag\] .*fw: part 4 main \(BMI_LZ_DATA\): 42475 bytes"),
            ("diag: board data", r"\[wifi-diag\] .*boot: hi_board_data_initialized = 1"),
            ("diag: HTC services", r"\[wifi-diag\] .*htc: service 0100 -> status 0, endpoint 1"),
            ("diag: WMI ready", r"\[wifi-diag\] .*wmi: WMI_READY"),
            ("diag: channel list", r"\[wifi-diag\] .*wmi: channel list: 11 channels, 2\.4 GHz mask 0ffe"),
            ("diag: bring-up summary", r"\[wifi-diag\] .*summary: bring-up OK"),
            ("diag: scan result", r"\[wifi-diag\] .*scan: \"" + SSID + r"\" .* MHz \(ch \d+\) rssi"),
            ("diag: scan complete", r"\[wifi-diag\] .*scan: SCAN_COMPLETE after \d+ ms"),
            ("model: firmware running", r"ctr-wifi: .*firmware running"),
            ("ARM9 link up", r"\[wifi\] link up"),
            ("DHCP lease", r"\[nettest\] .*address 10\.0\.2\.\d+"),
            ("HTTP fetch", r"\[nettest\] http .*" + TOKEN),
        ],
        "badnwm": [
            ("not NWM logged", r"\[wifi\] 0:/ios3ds/nwm\.code \(300000 bytes\): no NWM firmware pool"),
        ],
        "nocfg": [
            ("Wi-Fi off", r"\[wifi\] 0:/ios3ds/wifi-fw\.bin: not found .*Wi-Fi off"),
        ],
        "nofw": [
            ("firmware missing logged", r"\[wifi\] .*wifi-fw\.bin: not found"),
            ("Wi-Fi off", r"\[wifi\] backend ar6014 not available: networking off"),
        ],
        "wrongpsk": [
            ("message 2 rejected", r"ctr-wifi: .*message 2 MIC is wrong"),
            ("deauth after retries", r"ctr-wifi: .*deauthenticating"),
            ("psk hint after 3 tries", r"\[wifi\] the WPA2 handshake failed 3 times"),
        ],
    }[a.case]
    forbidden = {
        "nocfg": [("card untouched", r"ctr-wifi: .*card powered")],
        "nofw": [("card untouched", r"ctr-wifi: .*card powered")],
        "badnwm": [("card untouched", r"ctr-wifi: .*card powered")],
        "wrongpsk": [("no link", r"\[wifi\] link up")],
        "rekey": [("no group key timeout", r"reason 16")],
        "credit": [("no mailbox overflow", r"ctr-wifi: .*(Mbox Full|TX overflow)"),
                   ("no lost bulk bytes", r"\[nettest\] bulk .*bad [1-9]")],
    }.get(a.case, [])
    forbidden += [("no kernel panic", r"panic\(cpu"), ("no ARM9 crash", r"ARM9 (data abort|exception)")]
    ok = True
    for name, pat in checks:
        good = has(pat)
        ok &= good
        print("[wifi-test] %-26s %s" % (name, "PASS" if good else "FAIL"))
    for name, pat in forbidden:
        good = not has(pat)
        ok &= good
        print("[wifi-test] %-26s %s" % (name, "PASS" if good else "FAIL"))
    if a.case == "nwm":
        for ln in out.splitlines():
            if "[wifi-diag]" in ln and ("monitor:" in ln or "summary:" in ln):
                print("[wifi-test] " + ln.strip())
    if a.case in ("ok", "open", "nwm"):
        print("[wifi-test] host HTTP requests: %s" % (hits or "none"))
    for u in a.https:
        m = re.search(r"\[nettest\] https " + re.escape(u) + r" -> (.*)", out)
        res = m.group(1).strip() if m else "no result"
        good = res.startswith(("2", "3"))
        if a.expect_https == "ok":
            ok &= good
        print("[wifi-test] https %-40s %s" % (u, res))
    print("[wifi-test] %s (log: %s)" % ("PASS" if ok else "FAIL", os.path.join(a.out, "run.txt")))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
