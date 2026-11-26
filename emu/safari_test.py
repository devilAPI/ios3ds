#!/usr/bin/env python3
"""Safari page loads over the QEMU Wi-Fi model, with memory accounting.

  emu/safari_test.py [--pages 8] [--icount 2] [--new3ds] [--out build/emu-safari]

Puts wifi.cfg + a dummy firmware (as emu/wifi_test.py) on build/sd.img and
adds "ios3ds_mem=1 ios3ds_applog=1 ios3ds_urltest=1" to boot.cfg's cmdline:
  - ios3ds_mem: the [mem] report every 20 s (ios3ds-sample -mem: vm_stat,
    jetsam level, mbufs, kernel zones, resident size per process);
  - ios3ds_applog: every app alert is logged ("[applog] MobileSafari alert ...");
  - ios3ds_urltest: ios3ds-diag -urlpoll asks http://10.0.2.2:8080/next every
    2 s and opens a new URL in Safari through SpringBoardServices.
The host serves pages with images and a little JavaScript. Page 1 cold-starts
Safari (the "first page load" of the hardware reports); the rest load into the
running Safari. A page counts as loaded when the host served its HTML and
every image; any Safari alert (e.g. "Cannot allocate memory") fails it.
/next answers "pending" (not a URL) until the harness has a URL for Safari.
Every normal run boots with ios3ds_urlwait=1: without it ios3ds-diag -urlpoll
opens page 1 by itself 40 s after boot, often before Wi-Fi/DHCP is up, and
Safari shows ENOMEM (4.1) or EINVAL (3.1.3) (re-log 143). --nolink leaves
the flag off so the no-network alert is still exercised.
Prints the [mem] lines before Safari, after the first page and at the end.
boot.cfg is restored and the Wi-Fi files are removed afterwards.
"""
import argparse
import http.server
import io
import os
import random
import re
import shutil
import subprocess
import sys
import tempfile
import threading
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "emu"))
import wifi_test  # noqa: E402

NIMG = 6
state = {"next": "", "hits": [], "www": None}
SD = None                       # --sd path: session.py log must read the same image
lock = threading.Lock()


def image(i):
    from PIL import Image
    rnd = random.Random(i)
    im = Image.new("RGB", (320, 240), tuple(rnd.randrange(256) for _ in range(3)))
    for _ in range(3000):
        im.putpixel((rnd.randrange(320), rnd.randrange(240)), tuple(rnd.randrange(256) for _ in range(3)))
    b = io.BytesIO()
    im.save(b, "JPEG", quality=85)
    return b.getvalue()


IMAGES = {}


def page(n):
    paras = "".join("<p>Page %d, paragraph %d. %s</p>" % (n, k, "The quick brown fox jumps over the lazy dog. " * 12)
                    for k in range(40))
    imgs = "".join('<img src="/img%d.jpg?p=%d" width="300" height="225">' % (k, n) for k in range(NIMG))
    js = ("<script>var a=[];for(var i=0;i<5000;i++)a.push({i:i,s:'item '+i});"
          "document.title='page %d ('+a.length+')';</script>" % n)
    return ("<html><head><meta name='viewport' content='width=device-width'><title>p%d</title>%s</head>"
            "<body><h1>Page %d</h1>%s%s</body></html>" % (n, js, n, imgs, paras)).encode()


class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def do_GET(self):
        path = self.path
        local = None
        if state["www"] and path != "/next":
            import urllib.parse
            local = os.path.join(state["www"], os.path.basename(urllib.parse.unquote(path)))
            if not os.path.isfile(local):
                local = os.path.join(state["www"], os.path.basename(urllib.parse.unquote(path.split("?")[0])))
        if path == "/next":
            # ios3ds-diag -urlpoll reads one URL per /next answer and opens it.
            # An empty body for 40 s is its "no network, open the first page
            # anyway" case (used by --nolink): on a normal run that fallback
            # fires while Wi-Fi/DHCP is still coming up on old versions, so
            # Safari cold-loads p1 before there is a link. Keep the poll alive
            # with a token that is not a URL until the harness really has one.
            body = state["next"].encode() if state["next"] or state["nolink"] else b"pending\n"
            ctype = "text/plain"
        elif local and os.path.isfile(local):
            with open(local, "rb") as f:
                body = f.read()
            ext = os.path.splitext(local.split("?")[0])[1].lower()
            ctype = {".html": "text/html", ".css": "text/css", ".js": "text/javascript", ".png": "image/png",
                     ".jpg": "image/jpeg", ".jpeg": "image/jpeg", ".svg": "image/svg+xml"}.get(ext, "text/plain")
            if "load.php" in local:
                ctype = "text/css" if "only=styles" in local or local.endswith(".css") else "text/javascript"
        elif re.match(r"/p\d+\.html", path):
            body, ctype = page(int(re.match(r"/p(\d+)", path).group(1))), "text/html"
        elif re.match(r"/img\d+\.jpg", path):
            i = int(re.match(r"/img(\d+)", path).group(1))
            body, ctype = IMAGES.setdefault(i, image(i)), "image/jpeg"
        else:
            body, ctype = b"not found", "text/plain"
        if path != "/next":
            with lock:
                state["hits"].append((time.time(), path))
        self.send_response(200 if body != b"not found" else 404)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, fmt, *args):
        pass


def boot_extra(nolink, port):
    extra = " ios3ds_mem=1 ios3ds_applog=1 ios3ds_urltest=1"
    if not nolink:                      # never open p1 before Wi-Fi/DHCP is up
        extra += " ios3ds_urlwait=1"
    if port != 8080:                    # ios3ds_urlport: 8080 is often taken on the host
        extra += " ios3ds_urlport=%d" % port
    return extra


def sess(*args, check=True):
    return subprocess.run([sys.executable, os.path.join(ROOT, "emu", "session.py")] + list(args),
                          capture_output=True, text=True, check=check)


def log_text():
    # session.py's default is build/sd.img; with --sd pointing at a version's
    # image its log lines would otherwise be invisible to wait_log(), and the
    # harness would sit in the boot wait until boot-timeout.
    return sess("log", "--sd", SD).stdout if SD else sess("log").stdout


def wait_log(pattern, timeout):
    t = time.time()
    while time.time() - t < timeout:
        if re.search(pattern, log_text(), re.M):
            return True
        time.sleep(5)
    return False


def last_mem(text):
    lines = [l for l in text.splitlines() if re.match(r"\[mem \d+\] (KiB|\d+ procs|zones)", l)]
    if not lines:
        return []
    seq = re.match(r"\[mem (\d+)\]", lines[-1]).group(1)
    return [l for l in lines if l.startswith("[mem %s]" % seq)]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--pages", type=int, default=8)
    ap.add_argument("--icount", default="")
    ap.add_argument("--new3ds", action="store_true")
    ap.add_argument("--sd", default=os.path.join(ROOT, "build", "sd.img"))
    ap.add_argument("--out", default=os.path.join(ROOT, "build", "emu-safari"))
    ap.add_argument("--port", type=int, default=8080)
    ap.add_argument("--nolink", action="store_true",
                    help="wrong Wi-Fi passphrase: no network; page 1 is opened anyway after 40 s, its alert is reported")
    ap.add_argument("--www", help="serve this directory too (e.g. pages saved with wget -p -k -E -nd)")
    ap.add_argument("--urls", default="",
                    help="comma-separated pages of --www (or absolute http(s) URLs) to load instead of the generated ones")
    ap.add_argument("--mem-size", help="boot.cfg mem_size override (less RAM for XNU), e.g. 0x06000000")
    ap.add_argument("--host", default="10.0.2.2",
                    help="name in the URLs, e.g. 10.0.2.2.nip.io to make Safari resolve it (slirp's DNS asks the host's)")
    ap.add_argument("--boot-timeout", type=float, default=900)
    ap.add_argument("--page-timeout", type=float, default=300)
    ap.add_argument("--settle", type=float, default=60,
                    help="seconds to wait after boot before opening page 1 (0 = as early as the harness can)")
    a = ap.parse_args()
    global SD
    SD = a.sd
    os.makedirs(a.out, exist_ok=True)
    state["www"] = a.www
    state["nolink"] = a.nolink
    urls = [u for u in a.urls.split(",") if u]
    if urls:
        a.pages = len(urls)
    if a.new3ds:
        os.environ["IOS3DS_NEW3DS"] = "1"
    tmp = tempfile.mkdtemp(prefix="ios3ds-safari-")
    saved = os.path.join(tmp, "boot.cfg")
    wifi_test.mtools(["mcopy", "-o", "::/ios3ds/boot.cfg", saved], a.sd)
    with open(saved) as f:
        cfg = f.read()
    extra = boot_extra(a.nolink, a.port)
    cfg = re.sub(r"(?m)^(cmdline=.*)$", lambda m: m.group(1) + extra, cfg)
    if a.mem_size:
        cfg = re.sub(r"(?m)^mem_size=.*$", "mem_size=" + a.mem_size, cfg)
    with open(os.path.join(tmp, "b.cfg"), "w") as f:
        f.write(cfg)
    wifi_test.mtools(["mcopy", "-o", os.path.join(tmp, "b.cfg"), "::/ios3ds/boot.cfg"], a.sd)
    with open(os.path.join(tmp, "wifi.cfg"), "w") as f:
        f.write("ssid=%s\npsk=%s\n" % (wifi_test.SSID, "not-the-passphrase" if a.nolink else wifi_test.PSK))
    wifi_test.mtools(["mcopy", "-o", os.path.join(tmp, "wifi.cfg"), "::/ios3ds/wifi.cfg"], a.sd)
    wifi_test.dummy_fw(os.path.join(tmp, "wifi-fw.bin"))
    wifi_test.mtools(["mcopy", "-o", os.path.join(tmp, "wifi-fw.bin"), "::/ios3ds/wifi-fw.bin"], a.sd)

    srv = http.server.ThreadingHTTPServer(("127.0.0.1", a.port), Handler)
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    results, mem = [], {}
    t0 = time.time()
    try:
        args = ["start", "--wifi", "--sd", a.sd]
        if a.icount:
            args += ["--icount", a.icount]
        sess(*args)
        if not a.nolink and not wait_log(r"\[wifid\] en\d configured by configd", a.boot_timeout) or \
                not wait_log(r"\[input\] connected to SpringBoard", a.boot_timeout):
            print("[safari] boot or DHCP did not finish")
            return 1
        time.sleep(a.settle)                # let the boot settle (launch-time daemons exit)
        if a.nolink:                        # ios3ds-diag -urlpoll opens p1 by itself after 40 s
            ok = wait_log(r"\[applog\] MobileSafari alert", a.page_timeout * 3)
            text = log_text()
            alerts = re.findall(r"\[applog\] MobileSafari (?:alert|load error).*", text)
            for l in alerts:
                print("[safari] " + l[:400])
            sess("shot", "nolink", check=False)
            bad = [l for l in alerts if "allocate memory" in l]
            print("[safari] no network: %s" % ("no alert" if not ok else "ENOMEM alert" if bad else "alert without ENOMEM"))
            return 1 if bad or not ok else 0
        wait_log(r"^\[mem \d+\] \d+ procs", 60)
        mem["boot"] = last_mem(log_text())
        n_err = 0
        for n in range(1, a.pages + 1):
            n_alert = len(re.findall(r"\[applog\] MobileSafari alert", log_text()))
            with lock:
                start = len(state["hits"])
            name = urls[n - 1] if urls else "p%d.html" % n
            url = name if name.startswith(("http://", "https://")) else "http://%s:%d/%s" % (a.host, a.port, name)
            state["next"] = url
            t1 = time.time()
            ok, why = False, "timeout"
            while time.time() - t1 < a.page_timeout:
                time.sleep(5)
                with lock:
                    paths = [p for _, p in state["hits"][start:]]
                text = log_text()
                alerts = re.findall(r"\[applog\] MobileSafari alert .*", text)[n_alert:]
                if alerts:
                    ok, why = False, "; ".join(alerts)
                    break
                if url == name:             # a page on the internet (slirp): no alert for 120 s
                    if time.time() - t1 > 120:
                        ok, why = True, "(no alert in 120 s)"
                        break
                elif urls:                  # a saved page: its HTML, then 60 s without a new request
                    with lock:
                        last = state["hits"][-1][0] if len(state["hits"]) > start else 0
                    if "/" + name in paths and time.time() - last > 60:
                        ok, why = True, ""
                        break
                elif "/p%d.html" % n in paths and all(any(p.startswith("/img%d.jpg?p=%d" % (k, n)) for p in paths)
                                                       for k in range(NIMG)):
                    ok, why = True, ""
                    break
            dt = time.time() - t1
            time.sleep(10)                  # layout and paint after the last image
            sess("shot", "p%d" % n, check=False)
            shutil.copy(os.path.join(ROOT, "build", "emu-session", "p%d.png" % n), a.out)
            text = log_text()
            alerts = re.findall(r"\[applog\] MobileSafari alert .*", text)[n_alert:]
            if ok and alerts:
                ok, why = False, "; ".join(alerts)
            errs = re.findall(r"\[applog\] MobileSafari load error .*", text)
            for e in errs[n_err:]:
                print("[safari]   " + e[:400], flush=True)
            n_err = len(errs)
            results.append((n, ok, dt, why))
            print("[safari] page %d: %s in %.0f s %s" % (n, "loaded" if ok else "FAILED", dt, why), flush=True)
            if n == 1:
                time.sleep(25)
                mem["first page"] = last_mem(log_text())
            if not ok and "alert" in why:      # dismiss the alert (OK button)
                sess("tap", "160", "313", check=False)
                time.sleep(5)
        time.sleep(25)
        mem["end"] = last_mem(log_text())
    finally:
        sess("stop", check=False)
        time.sleep(3)
        srv.shutdown()
        text = log_text()
        with open(os.path.join(a.out, "log.txt"), "w") as f:
            f.write(text)
        wifi_test.mtools(["mcopy", "-o", saved, "::/ios3ds/boot.cfg"], a.sd)
        for f in ("wifi.cfg", "wifi-fw.bin"):
            wifi_test.mtools(["mdel", "::/ios3ds/" + f], a.sd, check=False)
        shutil.rmtree(tmp, ignore_errors=True)
    for k, v in mem.items():
        print("[safari] memory %s:" % k)
        for l in v:
            print("    " + l[:900])
    good = sum(1 for r in results if r[1])
    print("[safari] %d/%d pages loaded without an alert in %.0f s; log and screenshots in %s"
          % (good, a.pages, time.time() - t0, a.out))
    return 0 if good == a.pages else 1


if __name__ == "__main__":
    sys.exit(main())
