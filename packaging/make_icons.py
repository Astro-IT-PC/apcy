#!/usr/bin/env python3
"""Renders the apcy logo and writes every icon the build needs.

Pure Python, no dependencies: shapes are drawn with signed distance fields so
edges are anti-aliased at any size. Outputs (relative to the repo root):

  resources/logo.png        64 px  - drawn in the app header
  resources/logo-256.png   256 px  - window icon (Windows/Linux) and Linux desktop icon
  packaging/apcy.ico                - Windows executable icon (PNG-compressed entries)
  packaging/apcy.icns               - macOS bundle icon (macOS only, needs iconutil)

Run:  python3 packaging/make_icons.py
"""
import math
import os
import shutil
import struct
import subprocess
import sys
import zlib

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def clamp(v, lo, hi):
    return lo if v < lo else hi if v > hi else v


def lerp(a, b, t):
    return a + (b - a) * t


def sd_round_rect(px, py, cx, cy, hw, hh, r):
    qx = abs(px - cx) - hw + r
    qy = abs(py - cy) - hh + r
    return min(max(qx, qy), 0.0) + math.hypot(max(qx, 0.0), max(qy, 0.0)) - r


def sd_segment(px, py, ax, ay, bx, by):
    pax, pay = px - ax, py - ay
    bax, bay = bx - ax, by - ay
    h = clamp((pax * bax + pay * bay) / (bax * bax + bay * bay), 0.0, 1.0)
    return math.hypot(pax - bax * h, pay - bay * h)


def coverage(d, aa):
    """0..1 coverage of a shape from its signed distance and an AA width."""
    return clamp(0.5 - d / aa, 0.0, 1.0)


def mix(col, target, a):
    return [c + (t - c) * a for c, t in zip(col, target)]


def render(size):
    """Returns raw RGBA rows for the logo at `size` pixels."""
    aa = 1.0 / size  # one pixel in normalized units
    out = bytearray()
    for j in range(size):
        y = (j + 0.5) / size
        out.append(0)  # PNG filter byte: none
        for i in range(size):
            x = (i + 0.5) / size

            # --- background: rounded square, dark navy gradient + violet glow
            d_bg = sd_round_rect(x, y, 0.5, 0.5, 0.5, 0.5, 0.225)
            a_bg = coverage(d_bg, aa)
            if a_bg <= 0.0:
                out += b"\x00\x00\x00\x00"
                continue
            t = clamp((x + y) * 0.5, 0.0, 1.0)
            col = [lerp(0x26, 0x10, t), lerp(0x2a, 0x12, t), lerp(0x3c, 0x1c, t)]
            gd = math.hypot((x - 0.5) * 1.1, y - 0.64)
            glow = max(0.0, 1.0 - gd / 0.6) ** 2 * 0.5
            col = mix(col, (0x7c, 0x5c, 0xff), glow)

            # --- inbox tray: U shape (outer rounded rect minus the opening)
            d_out = sd_round_rect(x, y, 0.5, 0.66, 0.30, 0.14, 0.075)
            d_in = sd_round_rect(x, y, 0.5, 0.575, 0.22, 0.145, 0.045)
            d_tray = max(d_out, -d_in)
            a = coverage(d_tray, aa)
            if a > 0.0:
                tt = clamp((y - 0.5) / 0.3, 0.0, 1.0)
                tray = (lerp(0xa4, 0x6a, tt), lerp(0x88, 0x3d, tt), lerp(0xff, 0xf0, tt))
                col = mix(col, tray, a)

            # --- checkmark rising out of the tray, with a soft glow
            d_chk = min(sd_segment(x, y, 0.355, 0.44, 0.47, 0.565),
                        sd_segment(x, y, 0.47, 0.565, 0.715, 0.245))
            col = mix(col, (0xd8, 0xcc, 0xff), coverage(d_chk - 0.10, 0.08) * 0.35)
            col = mix(col, (0xf9, 0xf7, 0xff), coverage(d_chk - 0.068, aa))

            out += bytes((int(round(clamp(col[0], 0, 255))), int(round(clamp(col[1], 0, 255))),
                          int(round(clamp(col[2], 0, 255))), int(round(255 * a_bg))))
    return bytes(out)


def png_chunk(tag, data):
    body = tag + data
    return struct.pack(">I", len(data)) + body + struct.pack(">I", zlib.crc32(body) & 0xFFFFFFFF)


def encode_png(size, raw_rows):
    ihdr = struct.pack(">IIBBBBB", size, size, 8, 6, 0, 0, 0)
    return (b"\x89PNG\r\n\x1a\n" + png_chunk(b"IHDR", ihdr) +
            png_chunk(b"IDAT", zlib.compress(raw_rows, 9)) + png_chunk(b"IEND", b""))


def write_ico(path, pngs):
    """pngs: dict size -> PNG bytes. Modern ICO files may embed PNG data directly."""
    sizes = sorted(pngs)
    header = struct.pack("<HHH", 0, 1, len(sizes))
    offset = len(header) + 16 * len(sizes)
    entries, blobs = b"", b""
    for s in sizes:
        data = pngs[s]
        dim = 0 if s >= 256 else s
        entries += struct.pack("<BBBBHHII", dim, dim, 0, 0, 1, 32, len(data), offset + len(blobs))
        blobs += data
    with open(path, "wb") as f:
        f.write(header + entries + blobs)


def main():
    needed = sorted({16, 24, 32, 48, 64, 128, 256, 512, 1024})
    pngs = {}
    for s in needed:
        sys.stderr.write(f"rendering {s}px...\n")
        pngs[s] = encode_png(s, render(s))

    res = os.path.join(ROOT, "resources")
    pkg = os.path.join(ROOT, "packaging")
    with open(os.path.join(res, "logo.png"), "wb") as f:
        f.write(pngs[64])
    with open(os.path.join(res, "logo-256.png"), "wb") as f:
        f.write(pngs[256])
    write_ico(os.path.join(pkg, "apcy.ico"), {s: pngs[s] for s in (16, 24, 32, 48, 64, 128, 256)})

    if shutil.which("iconutil"):
        iconset = os.path.join(pkg, "apcy.iconset")
        os.makedirs(iconset, exist_ok=True)
        for name, s in (("icon_16x16", 16), ("icon_16x16@2x", 32), ("icon_32x32", 32), ("icon_32x32@2x", 64),
                        ("icon_128x128", 128), ("icon_128x128@2x", 256), ("icon_256x256", 256),
                        ("icon_256x256@2x", 512), ("icon_512x512", 512), ("icon_512x512@2x", 1024)):
            with open(os.path.join(iconset, name + ".png"), "wb") as f:
                f.write(pngs[s])
        subprocess.run(["iconutil", "-c", "icns", iconset, "-o", os.path.join(pkg, "apcy.icns")], check=True)
        shutil.rmtree(iconset)
    else:
        sys.stderr.write("iconutil not available: apcy.icns not regenerated\n")
    sys.stderr.write("done\n")


if __name__ == "__main__":
    main()
