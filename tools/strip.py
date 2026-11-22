"""Dev aid: side-by-side crops of PNGs. python tools/strip.py out.png x0 y0 x1 y1 a.png b.png ..."""
import struct, sys, zlib

def readpng(p):
    d = open(p, 'rb').read(); i = 8; idat = b''
    while i < len(d):
        n = struct.unpack('>I', d[i:i+4])[0]; t = d[i+4:i+8]; c = d[i+8:i+8+n]
        if t == b'IHDR': w, h = struct.unpack('>II', c[:8]); ct = c[9]
        if t == b'IDAT': idat += c
        i += 12 + n
    raw = zlib.decompress(idat); bpp = {6: 4, 2: 3, 0: 1}[ct]; stride = w * bpp
    out = bytearray(h * stride); prev = bytearray(stride)
    for y in range(h):
        f = raw[y*(stride+1)]; line = bytearray(raw[y*(stride+1)+1:(y+1)*(stride+1)])
        for x in range(stride):
            a = line[x-bpp] if x >= bpp else 0; b = prev[x]; c = prev[x-bpp] if x >= bpp else 0
            if f == 1: line[x] = (line[x] + a) & 255
            elif f == 2: line[x] = (line[x] + b) & 255
            elif f == 3: line[x] = (line[x] + (a + b) // 2) & 255
            elif f == 4:
                pa, pb, pc = abs(b - c), abs(a - c), abs(a + b - 2 * c)
                line[x] = (line[x] + (a if pa <= pb and pa <= pc else (b if pb <= pc else c))) & 255
        out[y*stride:(y+1)*stride] = line; prev = line
    return w, h, bpp, out

out, x0, y0, x1, y1 = sys.argv[1], *map(int, sys.argv[2:6])
imgs = [readpng(p) for p in sys.argv[6:]]
rows = b''
for y in range(y0, y1):
    row = b''
    for w, h, bpp, px in imgs:
        for x in range(x0, x1):
            o = (y * w + x) * bpp
            row += bytes(px[o:o+3]) + b'\xff' if bpp >= 3 else bytes([px[o]] * 3) + b'\xff'
        row += b'\xff\x00\xff\xff' * 3
    rows += b'\x00' + row
W = (x1 - x0 + 3) * len(imgs)
ch = lambda t, d: struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d) & 0xffffffff)
open(out, 'wb').write(b'\x89PNG\r\n\x1a\n' + ch(b'IHDR', struct.pack('>IIBBBBB', W, y1 - y0, 8, 6, 0, 0, 0)) + ch(b'IDAT', zlib.compress(rows)) + ch(b'IEND', b''))
