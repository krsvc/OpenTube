#!/usr/bin/env python3
"""Generate the OpenTube r13 home-menu icon (48x48) and 2D banner (256x128) for the diagnostic build.

Original mark in the approved modern style (design/opentube-modern-r13): the logo tile (accent pill-rect with the
accent-ink play notch) on the Slate top-ground with a soft Slate ambient glow, wordmark "OpenTube".
No reference thumbnail or sample artwork is used. The wordmark is rasterised with Urbanist (SIL Open Font License,
design/opentube-modern-r13/concepts/assets/fonts) at weight 800 when Pillow can load it; the font file itself is NOT
packaged (only the rendered pixels are). Outputs go to resource/opentube/ (new paths); resource/icon.png and
resource/banner.cgfx stay untouched for rollback.

Usage: python3 scripts/make_opentube_branding.py [out_dir]   (Pillow required; nothing is installed)
"""
import sys
from pathlib import Path

from PIL import Image, ImageDraw, ImageFilter, ImageFont

ROOT = Path(__file__).resolve().parents[1]
FONT = ROOT / "design/opentube-modern-r13/concepts/assets/fonts/Urbanist-VariableFont_wght.ttf"

# Slate tokens (concepts/modern/index.html [data-theme="slate"])
TOP = (0x13, 0x1B, 0x28)
BG = (0x19, 0x21, 0x2F)
ACC = (0x5C, 0x9E, 0xFF)
ACC_INK = (0x06, 0x14, 0x31)
TX = (0xEE, 0xF3, 0xFB)
AMB = (0x3F, 0x73, 0xC4)


def font(size):
    try:
        f = ImageFont.truetype(str(FONT), size)
        try:
            f.set_variation_by_axes([800])
        except Exception:
            pass
        return f, "Urbanist 800"
    except Exception:
        return ImageFont.load_default(), "Pillow default"


def logo_tile(draw, x, y, h):
    """16x11 accent tile with a 3.5 radius and the play notch (reference .logo), scaled to height h."""
    w = h * 16 / 11
    draw.rounded_rectangle([x, y, x + w, y + h], radius=h * 3.5 / 11, fill=ACC)
    nx, ny, nh = x + w * 6.2 / 16, y + h * 2.5 / 11, h * 6 / 11
    draw.polygon([(nx, ny), (nx, ny + nh), (nx + w * 5 / 16, ny + nh / 2)], fill=ACC_INK)
    return w


def glow(size, center, radius, color, alpha):
    layer = Image.new("RGBA", size, (0, 0, 0, 0))
    d = ImageDraw.Draw(layer)
    cx, cy = center
    d.ellipse([cx - radius, cy - radius * 0.8, cx + radius, cy + radius * 0.8], fill=color + (alpha,))
    return layer.filter(ImageFilter.GaussianBlur(radius / 2.2))


def make_icon(out):
    s = 4  # supersample
    img = Image.new("RGBA", (48 * s, 48 * s), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    d.rounded_rectangle([0, 0, 48 * s - 1, 48 * s - 1], radius=10 * s, fill=TOP + (255,))
    img.alpha_composite(glow(img.size, (24 * s, 26 * s), 20 * s, AMB, 150))
    d = ImageDraw.Draw(img)
    h = 22 * s
    w = h * 16 / 11
    logo_tile(d, (48 * s - w) / 2, (48 * s - h) / 2, h)
    img = img.resize((48, 48), Image.LANCZOS)
    img.save(out)


def make_banner(out):
    s = 4
    W, H = 256 * s, 128 * s
    img = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    # 2D banner card on the Slate ground with the ambient glow behind the mark
    d.rounded_rectangle([6 * s, 22 * s, 250 * s, 106 * s], radius=16 * s, fill=TOP + (255,))
    img.alpha_composite(glow(img.size, (80 * s, 64 * s), 70 * s, AMB, 130))
    mask = Image.new("L", (W, H), 0)
    ImageDraw.Draw(mask).rounded_rectangle([6 * s, 22 * s, 250 * s, 106 * s], radius=16 * s, fill=255)
    clipped = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    clipped.paste(img, (0, 0), mask)
    img = clipped
    d = ImageDraw.Draw(img)
    f, used = font(34 * s)
    word = "OpenTube"
    tb = d.textbbox((0, 0), word, font=f)
    tw, th = tb[2] - tb[0], tb[3] - tb[1]
    h = 30 * s
    w = h * 16 / 11
    total = w + 12 * s + tw
    x = (W - total) / 2
    logo_tile(d, x, (H - h) / 2, h)
    d.text((x + w + 12 * s - tb[0], (H - th) / 2 - tb[1]), word, font=f, fill=TX)
    img = img.resize((256, 128), Image.LANCZOS)
    img.save(out)
    return used


def main():
    out_dir = Path(sys.argv[1]) if len(sys.argv) > 1 else ROOT / "resource/opentube"
    out_dir.mkdir(parents=True, exist_ok=True)
    make_icon(out_dir / "icon.png")
    used = make_banner(out_dir / "banner.png")
    print("wrote", out_dir / "icon.png", out_dir / "banner.png", "(wordmark font:", used + ")")


if __name__ == "__main__":
    main()
