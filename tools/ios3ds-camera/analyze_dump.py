#!/usr/bin/env python3
"""Analyse an ios3ds camera frame dump (build 44+).

The ARM9 monitor writes sd:/ios3ds/photos/CAM<seq>.RAW at every camera stop:
the shim's two newest complete YUV422 320x240 frames, straight out of its
buffers (docs/camera.md section 4.1). Older builds put the dumps in
sd:/ios3ds/; on boot ios3ds moves them into photos/, so both paths are valid
input here.

    CAMD u32 header (little-endian): magic 0x44554D43, version 1, width,
    height, cam, port, seq of frame 1, seq of frame 2; then frame 1 (newest),
    frame 2. No padding anywhere.

    python3 tools/ios3ds-camera/analyze_dump.py sd/ios3ds/photos/CAM00001077.RAW [--dir out]

Writes <out>/frame1.yuyv.bmp, frame1.uyvy.bmp, frame2.*.bmp (YUV byte order
both ways: which one looks right settles the colour question), frame2-on-1.png
no - a BMP mosaic, and prints:

  - the mean absolute brightness difference between the frames at zero shift
    (0 = the picture never moved: the "one frame" symptom),
  - the best global (dx, dy) by luma SAD over +-32 px: a drift between two
    neighbouring frames = the word alignment slips (lost FIFO words), the
    signature of the "wiggling around" preview,
  - per-line wrap spots (packets lost mid-frame) as a shear hint.
"""
import argparse
import os
import struct
import sys

SHIFT = 32


def yuv_planes(frame: bytes, w: int, h: int, order: str):
    """Return (luma[y][x], u, v) for byte order 'yuyv' or 'uyvy'."""
    y = [[0] * w for _ in range(h)]
    u = [[0] * (w // 2) for _ in range(h)]
    v = [[0] * (w // 2) for _ in range(h)]
    for row in range(h):
        base = row * w * 2
        for cx in range(w // 2):
            b = frame[base + cx * 4:base + cx * 4 + 4]
            if order == "yuyv":
                y0, u0, y1, v0 = b[0], b[1], b[2], b[3]
            else:
                u0, y0, v0, y1 = b[0], b[1], b[2], b[3]
            y[row][cx * 2] = y0
            y[row][cx * 2 + 1] = y1
            u[row][cx] = u0
            v[row][cx] = v0
    return y, u, v


def bmp_gray(path: str, plane, w: int, h: int):
    with open(path, "wb") as f:
        stride = (w * 3 + 3) & ~3
        f.write(struct.pack("<2sIHHII", b"BM", 14 + 40 + stride * h, 0, 0, 14 + 40, 40))
        f.write(struct.pack("<IiiHHIIiiII", 40, w, h, 1, 24, 0, stride * h, 2835, 2835, 0, 0))
        for row in range(h - 1, -1, -1):
            f.write(b"".join(struct.pack("<3B", plane[row][x], plane[row][x], plane[row][x]) for x in range(w)))
            f.write(b"\0" * (stride - w * 3))


def bmp_yuv(path: str, y, u, v, w: int, h: int):
    def clamp(n):
        return max(0, min(255, n))

    with open(path, "wb") as f:
        stride = (w * 3 + 3) & ~3
        f.write(struct.pack("<2sIHHII", b"BM", 14 + 40 + stride * h, 0, 0, 14 + 40, 40))
        f.write(struct.pack("<IiiHHIIiiII", 40, w, h, 1, 24, 0, stride * h, 2835, 2835, 0, 0))
        for row in range(h - 1, -1, -1):
            out = bytearray()
            for x in range(w):
                c = x // 2
                yy, uu, vv = y[row][x] - 16, u[row][c] - 128, v[row][c] - 128
                r = clamp((298 * yy + 409 * vv + 128) >> 8)
                g = clamp((298 * yy - 100 * uu - 208 * vv + 128) >> 8)
                b = clamp((298 * yy + 516 * uu + 128) >> 8)
                out += bytes((b, g, r))
            f.write(out)
            f.write(b"\0" * (stride - w * 3))


def sad_shift(a, b, w: int, h: int):
    """Best (dx, dy, cost) aligning plane b onto a (b shifted by +dx,+dy)."""
    best = None
    for dy in range(-SHIFT, SHIFT + 1):
        for dx in range(-SHIFT, SHIFT + 1, 2):       # YUV words shift in pairs
            total = n = 0
            for row in range(4, h - 4, 2):
                ra = row
                rb = row + dy
                if not 0 <= rb < h:
                    continue
                for x in range(4, w - 4, 4):
                    xb = x + dx
                    if not 0 <= xb < w:
                        continue
                    total += abs(a[ra][x] - b[rb][xb])
                    n += 1
            if n and (best is None or total * best[3] < best[2] * n):
                best = (dx, dy, total, n)
    if not best:
        return 0, 0, 0
    return best[0], best[1], best[2] / best[3]


def mean_diff(a, b, w, h):
    total = n = 0
    for row in range(0, h, 2):
        for x in range(0, w, 2):
            total += abs(a[row][x] - b[row][x])
            n += 1
    return total / n


def row_score(a, b, row_a, row_b, dx, w):
    """SAD of one row pair, used for the per-line shear hint."""
    total = n = 0
    for x in range(8, w - 8, 4):
        xb = x + dx
        if 0 <= xb < w:
            total += abs(a[row_a][x] - b[row_b][xb])
            n += 1
    return total / max(n, 1)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("dump")
    ap.add_argument("--dir", default=None)
    args = ap.parse_args()
    data = open(args.dump, "rb").read()
    m, ver, w, h, cam, port, seq0, seq1 = struct.unpack_from("<8I", data, 0)
    if m != 0x44554D43:
        sys.exit("not a CAMD dump (magic %08x)" % m)
    print("cam %u port %u, frames seq %u (newest) and %u, %ux%u YUV422" % (cam, port, seq0, seq1, w, h))
    fbytes = w * h * 2
    if len(data) != 32 + 2 * fbytes:
        sys.exit("size mismatch: %u bytes, expected %u" % (len(data), 32 + 2 * fbytes))
    outdir = args.dir or os.path.splitext(args.dump)[0] + ".out"
    os.makedirs(outdir, exist_ok=True)
    for i, frame in enumerate((data[32:32 + fbytes], data[32 + fbytes:32 + 2 * fbytes]), 1):
        for order in ("yuyv", "uyvy"):
            y, u, v = yuv_planes(frame, w, h, order)
            bmp_yuv(os.path.join(outdir, "frame%d.%s.bmp" % (i, order)), y, u, v, w, h)
    y1, _, _ = yuv_planes(data[32:32 + fbytes], w, h, "yuyv")
    y2, _, _ = yuv_planes(data[32 + fbytes:32 + 2 * fbytes], w, h, "yuyv")
    print("mean |luma diff| frame1 vs frame2: %.2f (0 = identical picture)" % mean_diff(y1, y2, w, h))
    dx, dy, cost = sad_shift(y1, y2, w, h)
    print("best alignment of frame2 onto frame1: dx %+d, dy %+d (luma SAD %.2f vs %.2f at 0,0)"
          % (dx, dy, cost, mean_diff(y1, y2, w, h)))
    # shear hint: rows whose best dx differs from the global one by more than 2
    # shear hint: rows whose best dx (at the global dy) differs from it by more than 2
    odd = []
    for row in range(h):
        rb = row + dy
        if not 0 <= rb < h:
            continue
        bestdx = min(range(-SHIFT, SHIFT + 1, 2), key=lambda d: row_score(y1, y2, row, rb, d, w))
        if abs(bestdx - dx) > 2:
            odd.append((row, bestdx))
    if odd:
        print("rows with a different best dx (mid-frame word loss): " +
              " ".join("%u:%+d" % o for o in odd[:12]) + (" ..." if len(odd) > 12 else ""))
    print("wrote BMPs to", outdir)
    print("verdict help: dx/dy ~ 0 and diff ~ 0 = frozen content; nonzero constant drift = alignment slips")
    print("frame-to-frame; rows disagreeing with the global dx = words lost mid-frame")


if __name__ == "__main__":
    main()
