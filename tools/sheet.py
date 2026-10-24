#!/usr/bin/env python3
"""Tile PGM images (8 or 16 bit) into one upscaled PNG for viewing.
  sheet.py OUT.png SCALE IMG.pgm...   (16-bit images are min/max stretched)"""
import struct, sys, zlib

def read_pgm(path):
    d = open(path, 'rb').read()
    # Header: magic, width, height, maxval, then exactly one whitespace byte.
    fields, pos = [], 0
    while len(fields) < 4:
        while d[pos:pos + 1].isspace():
            pos += 1
        end = pos
        while not d[end:end + 1].isspace():
            end += 1
        fields.append(d[pos:end])
        pos = end
    w, h, mx = int(fields[1]), int(fields[2]), int(fields[3])
    px = d[pos + 1:]
    if mx > 255:
        v = [px[2*i] << 8 | px[2*i+1] for i in range(w*h)]
        lo, hi = min(v), max(v) or 1
        v = [(x - lo) * 255 // max(hi - lo, 1) for x in v]
    else:
        v = list(px[:w*h])
    return w, h, v

def write_png(path, w, h, rows):
    raw = b''.join(b'\0' + bytes(r) for r in rows)
    chunk = lambda t, d: struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d))
    open(path, 'wb').write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 0, 0, 0, 0))
                           + chunk(b'IDAT', zlib.compress(raw)) + chunk(b'IEND', b''))

out, scale, files = sys.argv[1], int(sys.argv[2]), sys.argv[3:]
imgs = [read_pgm(f) for f in files]
cols = min(len(imgs), 8); gap = 4
cw = max(i[0] for i in imgs) * scale; ch = max(i[1] for i in imgs) * scale
nrows = (len(imgs) + cols - 1) // cols
W, H = cols * (cw + gap), nrows * (ch + gap)
canvas = [[60] * W for _ in range(H)]
for n, (w, h, v) in enumerate(imgs):
    ox, oy = (n % cols) * (cw + gap), (n // cols) * (ch + gap)
    for y in range(h * scale):
        for x in range(w * scale):
            canvas[oy + y][ox + x] = v[(y // scale) * w + x // scale]
write_png(out, W, H, canvas)
