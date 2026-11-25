#!/usr/bin/env python3
"""Dump the CLCD scan-out page and every iOS framebuffer slot from QEMU.

    python3 emu/surfaces.py --sd build/sd.img --time 120 --out build/surfaces

Boots an SD image headlessly, takes a screenshot of the 3DS LCDs, saves the
fake S5L8720 CLCD register page and /vram through QMP `pmemsave`, and renders
each 320x480x4 BGRA slot of /vram as an upright PNG. This answers the usual
"is the picture I want on a different surface than the one the ARM9 copies?"
question without changing the guest. All output goes under --out (build/).
"""
import argparse
import json
import os
import socket
import struct
import subprocess
import sys
import tempfile
import time

from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import viewer  # noqa: E402

ROOT = viewer.ROOT
W, H, FRAME = 320, 480, 320 * 480 * 4
CLCD_PA = 0x1FFBA000       # boot.cfg display_regs; window-1 base at +0x24
VRAM_PA = 0x18000000
VRAM_LEN = 0x300000


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--sd", default=os.path.join(ROOT, "build", "sd.img"))
    ap.add_argument("--firm", default=os.path.join(ROOT, "build", "boot", "ios3ds.firm"))
    ap.add_argument("--time", type=float, default=120, help="guest seconds to wait before the dump")
    ap.add_argument("--out", default=os.path.join(ROOT, "build", "surfaces"))
    ap.add_argument("--icount", default="2")
    ap.add_argument("--clcd", default=hex(CLCD_PA))
    ap.add_argument("--vram", default=hex(VRAM_PA))
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    tmp = tempfile.mkdtemp(prefix="ios3ds-surfaces-", dir=a.out)
    mon = os.path.join(tmp, "qmp.sock")
    cmd = [viewer.QEMU, "-M", "nintendo3ds", "-accel", "tcg,thread=single",
           "-kernel", a.firm, "-display", "none", "-serial", "none",
           "-qmp", "unix:%s,server,nowait" % mon,
           "-drive", "file=%s,if=sd,format=raw" % a.sd]
    if a.icount:
        cmd += ["-icount", "shift=%s,sleep=off" % a.icount, "-rtc", "clock=vm"]
    print("[surfaces] " + " ".join(cmd), flush=True)
    proc = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.STDOUT)
    q = viewer.QMP(mon)
    time.sleep(a.time)
    q.cmd("screendump", filename=os.path.join(a.out, "lcd.png"))
    clcd = os.path.join(a.out, "clcd.bin")
    vram = os.path.join(a.out, "vram.bin")
    q.cmd("pmemsave", val=int(a.clcd, 0), size=0x1000, filename=clcd)
    q.cmd("pmemsave", val=int(a.vram, 0), size=VRAM_LEN, filename=vram)
    q.cmd("quit")
    proc.wait(timeout=20)

    regs = open(clcd, "rb").read()
    u32 = lambda o: struct.unpack_from("<I", regs, o)[0]
    print("CLCD: on=%#x base=%#x stride=%#x wh=%#x" %
          (u32(0), u32(0x24), u32(0x28), u32(0x30)))
    data = open(vram, "rb").read()
    for off in range(0, len(data) - FRAME + 1, FRAME):
        px = bytearray(data[off:off + FRAME])
        px[0::4], px[2::4] = px[2::4], px[0::4]     # BGRA -> RGBA
        path = os.path.join(a.out, "vram_%06x.png" % (int(a.vram, 0) + off))
        Image.frombytes("RGBA", (W, H), bytes(px)).convert("RGB").save(path)
        print("wrote %s" % path)


if __name__ == "__main__":
    main()
