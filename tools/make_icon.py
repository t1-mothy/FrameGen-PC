"""Renders src/framegen.ico (pure Python, no packages needed).

Same motif as the Mac app's icon: three frames sliding right, the in-between (generated) one
highlighted with a play symbol, plus motion streaks. Run: python tools/make_icon.py
"""
import math
import os
import struct
import zlib

OUT = os.path.join(os.path.dirname(__file__), "..", "src", "framegen.ico")


def sd_round_rect(px, py, x0, y0, x1, y1, r):
    cx, cy = (x0 + x1) / 2, (y0 + y1) / 2
    hx, hy = (x1 - x0) / 2 - r, (y1 - y0) / 2 - r
    qx, qy = abs(px - cx) - hx, abs(py - cy) - hy
    out = math.hypot(max(qx, 0), max(qy, 0))
    return out + min(max(qx, qy), 0) - r


def sd_segment(px, py, ax, ay, bx, by):
    pax, pay, bax, bay = px - ax, py - ay, bx - ax, by - ay
    h = max(0, min(1, (pax * bax + pay * bay) / (bax * bax + bay * bay)))
    return math.hypot(pax - bax * h, pay - bay * h)


def in_triangle(px, py, a, b, c):
    def s(p1, p2):
        return (px - p2[0]) * (p1[1] - p2[1]) - (p1[0] - p2[0]) * (py - p2[1])
    d1, d2, d3 = s(a, b), s(b, c), s(c, a)
    neg = d1 < 0 or d2 < 0 or d3 < 0
    pos = d1 > 0 or d2 > 0 or d3 > 0
    return not (neg and pos)


def over(dst, rgb, a):
    r, g, b, da = dst
    oa = a + da * (1 - a)
    if oa <= 0:
        return (0, 0, 0, 0)
    return tuple((rgb[i] * a + dst[i] * da * (1 - a)) / oa for i in range(3)) + (oa,)


def render(size):
    s = size / 1024.0
    px_world = 1 / s  # one output pixel in icon units
    dim = (0.45, 0.50, 0.75, 0.55)
    glow = (0.20, 0.95, 0.65, 1.0)
    frames = [(205, dim, False), (489, dim, False), (347, glow, True)]
    rows = []
    for j in range(size):
        row = bytearray()
        for i in range(size):
            x, y = (i + 0.5) * px_world, (j + 0.5) * px_world
            col = (0.0, 0.0, 0.0, 0.0)
            d = sd_round_rect(x, y, 40, 40, 984, 984, 180)
            cov = max(0.0, min(1.0, 0.5 - d / px_world))
            if cov > 0:
                t = max(0.0, min(1.0, (x + y) / 2048))
                bg = (0.07 + 0.09 * t, 0.08 + 0.02 * t, 0.16 + 0.16 * t)
                col = over(col, bg, cov)
                for fx, c, is_glow in frames:
                    x0, y0, x1, y1 = fx, 387, fx + 330, 637
                    dd = sd_round_rect(x, y, x0, y0, x1, y1, 34)
                    if is_glow and dd > 0:
                        col = over(col, c[:3], 0.55 * math.exp(-dd / 28) * cov)
                    fill_cov = max(0.0, min(1.0, 0.5 - dd / px_world))
                    if fill_cov > 0:
                        col = over(col, (0.08, 0.09, 0.18), (0.9 if is_glow else 0.6) * fill_cov * cov)
                    sw = 22 if is_glow else 16
                    stroke_cov = max(0.0, min(1.0, 0.5 - (abs(dd) - sw / 2) / px_world))
                    if stroke_cov > 0:
                        col = over(col, c[:3], c[3] * stroke_cov * cov)
                    if is_glow:
                        cx, cy = fx + 165 + 8, 512
                        tri = ((cx - 40, cy - 52), (cx + 52, cy), (cx - 40, cy + 52))
                        hits = 0
                        for sy in (0.25, 0.75):
                            for sx in (0.25, 0.75):
                                if in_triangle((i + sx) * px_world, (j + sy) * px_world, *tri):
                                    hits += 1
                        if hits:
                            col = over(col, c[:3], hits / 4 * cov)
                for k, (yy, w, x_end) in enumerate(((724, 14, 724), (756, 10, 604))):
                    ds = sd_segment(x, y, 300, yy, x_end, yy) - w / 2
                    sc = max(0.0, min(1.0, 0.5 - ds / px_world))
                    if sc > 0:
                        col = over(col, (0.2, 0.95, 0.65), 0.5 * sc * cov)
            row += bytes(int(round(max(0, min(1, v)) * 255)) for v in col)
        rows.append(bytes([0]) + bytes(row))
    raw = b"".join(rows)

    def chunk(tag, data):
        return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)

    png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", size, size, 8, 6, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b"")
    return png


def main():
    sizes = [16, 24, 32, 48, 64, 128, 256]
    images = [render(sz) for sz in sizes]
    header = struct.pack("<HHH", 0, 1, len(sizes))
    offset = 6 + 16 * len(sizes)
    entries, data = b"", b""
    for sz, img in zip(sizes, images):
        entries += struct.pack("<BBBBHHII", sz % 256, sz % 256, 0, 0, 1, 32, len(img), offset + len(data))
        data += img
    with open(OUT, "wb") as f:
        f.write(header + entries + data)
    print("wrote", os.path.abspath(OUT))


if __name__ == "__main__":
    main()
