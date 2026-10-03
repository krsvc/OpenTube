#!/usr/bin/env python3
"""OpenTube r14: build romfs/gfx/font/latin_extended_a_font.t3x (Ő ő Ű ű) from the app's own bundled glyphs.

The external font has no Latin Extended-A block, so Hungarian ő / ű drew as <?>. The four glyphs are composed from the
bundled Latin-1 block (same font, same 20 px cell, same ETC1A4 format): the letter body of Ö / ö / Ü / ü and the acute
of Ó / ó / Ú / ú, doubled side by side (the double acute). No external font is used.

Usage: python3 scripts/make_hungarian_glyphs.py [out.t3x] [png_dir]   (Pillow + devkitPro tex3ds; nothing is installed)
Also importable: decode_block() / decode_t3x() are used by the host asset test.
"""
import os
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

from PIL import Image

ROOT = Path(__file__).resolve().parents[1]
FONT_DIR = ROOT / "romfs/gfx/font"
TEX3DS = os.environ.get("TEX3DS", os.path.join(os.environ.get("DEVKITPRO", "/opt/devkitpro"), "tools/bin/tex3ds"))
# (new glyph, letter body from, acute from), in code point order = the order of font_samples.txt
GLYPHS = [("Ő", "Ö", "Ó"), ("ő", "ö", "ó"), ("Ű", "Ü", "Ú"),
          ("ű", "ü", "ú")]
SHIFT = 2  # each copy of the acute moves this many px left / right


def lz11(d):
    assert d[0] == 0x11, "not LZ11"
    size, p = d[1] | d[2] << 8 | d[3] << 16, 4
    if size == 0:
        size, p = struct.unpack_from("<I", d, 4)[0], 8
    out = bytearray()
    while len(out) < size:
        flags = d[p]
        p += 1
        for i in range(8):
            if len(out) >= size:
                break
            if flags & (0x80 >> i):
                b, ind = d[p], d[p] >> 4
                if ind == 0:
                    n, disp, p = ((b & 15) << 4 | d[p + 1] >> 4) + 0x11, ((d[p + 1] & 15) << 8 | d[p + 2]) + 1, p + 3
                elif ind == 1:
                    n = ((b & 15) << 12 | d[p + 1] << 4 | d[p + 2] >> 4) + 0x111
                    disp, p = ((d[p + 2] & 15) << 8 | d[p + 3]) + 1, p + 4
                else:
                    n, disp, p = ind + 1, ((b & 15) << 8 | d[p + 1]) + 1, p + 2
                for _ in range(n):
                    out.append(out[-disp])
            else:
                out.append(d[p])
                p += 1
    return bytes(out)


def decode_t3x(path):
    """-> (format, width, height, [(w, h, left, top, right, bottom)], alpha image) for ETC1A4 (13) textures."""
    d = Path(path).read_bytes()
    n, wh, fmt, _mip = struct.unpack_from("<HBBB", d, 0)
    w, h = 8 << (wh & 7), 8 << ((wh >> 3) & 7)
    subs = [struct.unpack_from("<HHHHHH", d, 5 + 12 * i) for i in range(n)]
    data = lz11(d[5 + 12 * n:])
    assert fmt == 13, "expected ETC1A4"
    img = Image.new("L", (w, h))
    px, p = img.load(), 0
    for ty in range(h // 8):
        for tx in range(w // 8):
            for bx, by in ((0, 0), (1, 0), (0, 1), (1, 1)):  # 4x4 blocks in Z order inside an 8x8 tile
                a = struct.unpack_from("<Q", data, p)[0]
                p += 16  # 8 bytes alpha + 8 bytes ETC1 color
                for x in range(4):
                    for y in range(4):
                        px[tx * 8 + bx * 4 + x, ty * 8 + by * 4 + y] = ((a >> (4 * (x * 4 + y))) & 15) * 17
    return fmt, w, h, subs, img


def decode_block(path):
    """-> list of glyph alpha images in subtexture order."""
    _fmt, w, h, subs, img = decode_t3x(path)
    res = []
    for sw, sh, left, top, _right, _bottom in subs:
        x0, y0 = round(left * w / 1024), h - round(top * h / 1024)
        res.append(img.crop((x0, y0, x0 + sw, y0 + sh)))
    return res


def latin1_glyph(glyphs, ch):
    return glyphs[ord(ch) - 0xA0]  # the Latin-1 block holds U+00A0..U+00FF in order


def compose(body, acute):
    w, h = body.size
    assert acute.size == (w, h)
    a, b = acute.load(), body.load()
    top = next(y for y in range(h) if all(abs(a[x, yy] - b[x, yy]) <= 17 for yy in range(y, h) for x in range(w)))
    out = Image.new("L", (w, h))
    o = out.load()
    for y in range(h):
        for x in range(w):
            if y >= top:
                o[x, y] = b[x, y]
            else:
                left = a[x + SHIFT, y] if x + SHIFT < w else 0
                right = a[x - SHIFT, y] if x - SHIFT >= 0 else 0
                o[x, y] = max(left, right)
    return out, top


def main():
    out = Path(sys.argv[1]) if len(sys.argv) > 1 else FONT_DIR / "latin_extended_a_font.t3x"
    latin1 = decode_block(FONT_DIR / "latin_1_supplement_font.t3x")
    # one row of glyphs, 1 px apart, never rotated (like the bundled blocks); tex3ds encodes the ETC1A4 data and the
    # subtexture table is written here (tex3ds --atlas would rotate some glyphs to pack them)
    tex_w, tex_h, x = 64, 32, 0
    atlas = Image.new("RGBA", (tex_w, tex_h), (255, 255, 255, 0))
    subs = []
    for ch, body_ch, acute_ch in GLYPHS:
        alpha, top = compose(latin1_glyph(latin1, body_ch), latin1_glyph(latin1, acute_ch))
        w, h = alpha.size
        glyph = Image.new("RGBA", (w, h), (255, 255, 255, 0))
        glyph.putalpha(alpha)
        atlas.paste(glyph, (x, 0))
        # left, top, right, bottom in 1/1024 of the texture, v measured from the bottom (tex3ds / citro2d layout)
        subs.append((w, h, x * 1024 // tex_w, (tex_h - 0) * 1024 // tex_h, (x + w) * 1024 // tex_w,
                     (tex_h - h) * 1024 // tex_h))
        print("U+%04X from %s + double %s: %dx%d at x %d, accent rows < %d" % (ord(ch), body_ch, acute_ch, w, h, x, top))
        x += w + 1
    assert x <= tex_w
    with tempfile.TemporaryDirectory() as tmp:
        png = Path(sys.argv[2]) if len(sys.argv) > 2 else Path(tmp) / "atlas.png"
        atlas.save(png)
        single = Path(tmp) / "single.t3x"
        subprocess.run([TEX3DS, "-f", "etc1a4", "-z", "auto", "-o", str(single), str(png)], check=True)
        d = single.read_bytes()
        n, wh, fmt, mip = struct.unpack_from("<HBBB", d, 0)
        assert n == 1 and fmt == 13 and (8 << (wh & 7), 8 << ((wh >> 3) & 7)) == (tex_w, tex_h)
        header = struct.pack("<HBBB", len(subs), wh, fmt, mip) + b"".join(struct.pack("<HHHHHH", *e) for e in subs)
        out.write_bytes(header + d[5 + 12:])
    print("wrote", out)


if __name__ == "__main__":
    main()
