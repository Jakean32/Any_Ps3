#!/usr/bin/env python3
"""Any_Ps3 — Android launcher icon generator (v0.6.0 redesign).

Design brief: the user asked for a REAL app icon — simple, clean, standing
out from the competition. Brand identity = the "3" (from the user's chosen
logo: a stone "3" + mountain temple, gold/sky palette).

  * Foreground : one bold "3" in a gold gradient with a soft bevel and a
                 warm glow, sized to the adaptive-icon safe zone (66/108).
  * Background : deep night-blue vertical gradient with a faint diagonal
                 light sweep (premium, calm, high contrast vs the gold).
  * Monochrome : the same "3" silhouette in white — Android 13 themed
                 icons tint it with the user's wallpaper color.
  * Legacy     : circle-masked composite for older launchers.

No Sony assets, no copyrighted content — a plain typographic "3".
Outputs into hosts/android/app/src/main/res/mipmap-* plus a 512px store icon.
"""
import os
import numpy as np
from PIL import Image, ImageDraw, ImageFilter, ImageFont

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
RES = os.path.join(ROOT, "hosts", "android", "app", "src", "main", "res")

FONT_CANDIDATES = [
    "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",
    "/usr/share/fonts/truetype/liberation/LiberationSans-Bold.ttf",
    "/usr/share/fonts/truetype/english/Carlito-Bold.ttf",
]
FONT = next(p for p in FONT_CANDIDATES if os.path.exists(p))

S = 1080  # master canvas (10x the 108dp adaptive spec)
DENSITIES = {"mdpi": 108, "hdpi": 162, "xhdpi": 216, "xxhdpi": 324, "xxxhdpi": 432}
LEGACY = {"mdpi": 48, "hdpi": 72, "xhdpi": 96, "xxhdpi": 144, "xxxhdpi": 192}


def v_gradient(size, top, bottom):
    out = Image.new("RGB", (size, size))
    d = ImageDraw.Draw(out)
    for y in range(size):
        t = y / (size - 1)
        c = tuple(int(top[i] + (bottom[i] - top[i]) * t) for i in range(3))
        d.line([(0, y), (size, y)], fill=c)
    return out


def make_background(size=S):
    """Rich night-blue gradient + diagonal light sweep + corner vignette.
    numpy math (no inverted paste-mask traps)."""
    ys = np.linspace(0.0, 1.0, size)
    top = np.array([40.0, 72.0, 122.0])
    bot = np.array([15.0, 23.0, 41.0])
    grad = top[None, None, :] * (1.0 - ys[:, None, None]) + \
        bot[None, None, :] * ys[:, None, None]
    grad = np.repeat(grad, size, axis=1)                      # (size,size,3)
    xs = np.linspace(0.0, 1.0, size)
    sweep = np.clip(1.0 - np.abs(xs - 0.62) / 0.5, 0.0, 1.0)[None, :] * 0.16
    light = np.array([120.0, 160.0, 220.0])
    grad = np.clip(grad + sweep[:, :, None] * light[None, None, :], 0, 255)
    xx = np.linspace(-1.0, 1.0, size)[None, :]
    yy = np.linspace(-1.0, 1.0, size)[:, None]
    r = np.sqrt(xx * xx + yy * yy) / np.sqrt(2.0)              # 0 center -> 1 corners
    vig = np.clip((r - 0.62) / 0.38, 0.0, 1.0) * 0.45          # corners only
    grad = grad * (1.0 - vig[:, :, None])
    return Image.fromarray(grad.astype(np.uint8), "RGB")


def glyph_mask(size=S, frac=0.56):
    """Anti-aliased '3' mask, centered, slightly raised."""
    font = ImageFont.truetype(FONT, int(size * frac))
    img = Image.new("L", (size, size), 0)
    d = ImageDraw.Draw(img)
    bbox = d.textbbox((0, 0), "3", font=font)
    w, h = bbox[2] - bbox[0], bbox[3] - bbox[1]
    x = (size - w) // 2 - bbox[0]
    y = (size - h) // 2 - bbox[1] - int(size * 0.022)
    d.text((x, y), "3", font=font, fill=255)
    return img


def mountain_silhouette(size=S):
    """Filled distant-mountain silhouette, anchored at the lower safe zone.
    Used BEHIND the gold '3' (background layer of the foreground drawable)."""
    img = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    base = int(size * 0.87)
    # two overlapping ridges for depth (back ridge lighter, front darker)
    back = [
        (int(size * 0.18), base),
        (int(size * 0.34), int(size * 0.60)),
        (int(size * 0.47), int(size * 0.72)),
        (int(size * 0.60), int(size * 0.56)),   # main back peak
        (int(size * 0.78), int(size * 0.70)),
        (int(size * 0.90), base),
        (int(size * 0.90), base + 1), (int(size * 0.18), base + 1),
    ]
    front = [
        (int(size * 0.12), base),
        (int(size * 0.28), int(size * 0.685)),
        (int(size * 0.43), int(size * 0.775)),
        (int(size * 0.58), int(size * 0.665)),  # front peak
        (int(size * 0.74), int(size * 0.76)),
        (int(size * 0.88), base),
        (int(size * 0.88), base + 1), (int(size * 0.12), base + 1),
    ]
    d.polygon(back, fill=(38, 58, 88, 235))
    d.polygon(front, fill=(26, 41, 63, 245))
    # snow caps on the two back peaks
    for (px, py) in [(int(size * 0.34), int(size * 0.60)), (int(size * 0.60), int(size * 0.56))]:
        d.polygon([
            (px - int(size * 0.030), py + int(size * 0.030)),
            (px, py),
            (px + int(size * 0.030), py + int(size * 0.030)),
        ], fill=(150, 178, 210, 160))
    return img


def make_foreground(size=S, mask=None):
    """Gold gradient '3' floating above a mountain silhouette (brand mark)."""
    mask = mask if mask is not None else glyph_mask(size)
    out = mountain_silhouette(size)
    # warm radial glow behind the glyph
    glow = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    gd = ImageDraw.Draw(glow)
    cx = cy = size // 2
    r = int(size * 0.36)
    gd.ellipse([cx - r, cy - r, cx + r, cy + r], fill=(226, 178, 96, 110))
    glow = glow.filter(ImageFilter.GaussianBlur(size * 0.09))
    out.alpha_composite(glow)

    # gold vertical gradient fill through the glyph mask
    gold = v_gradient(size, (246, 216, 150), (188, 134, 58)).convert("RGBA")
    glyph = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    glyph.paste(gold, (0, 0), mask)

    # drop shadow behind the glyph (onto the mountains — adds depth)
    off = max(3, size // 140)
    shadow = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    shadow.paste(Image.new("RGBA", (size, size), (0, 0, 0, 150)), (off * 2, off * 2), mask)
    shadow = shadow.filter(ImageFilter.GaussianBlur(size * 0.010))

    out.alpha_composite(shadow)
    out.alpha_composite(glyph)

    # crisp top-left highlight: a rim-colored band whose alpha comes from
    # (shifted mask minus mask) — pasted as a real layer, never zeroing alpha
    shifted = Image.new("L", (size, size), 0)
    shifted.paste(mask, (-off, -off))
    edge_alpha = Image.composite(shifted, Image.new("L", (size, size), 0), mask)
    rim_layer = Image.new("RGBA", (size, size), (255, 244, 214, 255))
    rim_band = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    rim_band.paste(rim_layer, (0, 0), edge_alpha)
    out.alpha_composite(rim_band)
    return out


def make_monochrome(size=S, mask=None):
    """White silhouette ('3' + mountains) for Android 13 themed icons."""
    mask = mask if mask is not None else glyph_mask(size)
    img = mountain_silhouette(size).copy()
    # unify to pure white with the silhouette's alpha
    px = img.load()
    for y in range(img.height):
        for x in range(img.width):
            r, g, b, a = px[x, y]
            if a > 0:
                px[x, y] = (255, 255, 255, a)
    white = Image.new("RGBA", (size, size), (255, 255, 255, 255))
    img.paste(white, (0, 0), mask)
    return img


def circle_mask(size):
    m = Image.new("L", (size, size), 0)
    ImageDraw.Draw(m).ellipse([0, 0, size - 1, size - 1], fill=255)
    return m


def main():
    mask = glyph_mask()
    bg = make_background()
    fg = make_foreground(mask=mask)
    mono = make_monochrome(mask=mask)

    # adaptive layers per density
    for dpi, px in DENSITIES.items():
        d = os.path.join(RES, f"mipmap-{dpi}")
        os.makedirs(d, exist_ok=True)
        bg.resize((px, px), Image.LANCZOS).save(os.path.join(d, "ic_launcher_bg.png"))
        fg.resize((px, px), Image.LANCZOS).save(os.path.join(d, "ic_launcher_fg.png"))
        mono.resize((px, px), Image.LANCZOS).save(os.path.join(d, "ic_launcher_mono.png"))
        # legacy circle icon: background + glyph, corners transparent
        legacy = bg.convert("RGBA")
        legacy.alpha_composite(fg)
        legacy.putalpha(circle_mask(S))
        legacy.resize((px, px), Image.LANCZOS).save(os.path.join(d, "ic_launcher.png"))

    # store / marketing icon (512, rounded square)
    store = bg.convert("RGBA")
    store.alpha_composite(fg)
    r = int(S * 0.18)
    square = Image.new("L", (S, S), 0)
    ImageDraw.Draw(square).rounded_rectangle([0, 0, S - 1, S - 1], radius=r, fill=255)
    store.putalpha(square)
    dl = os.path.join(ROOT, "download")
    os.makedirs(dl, exist_ok=True)
    store.resize((512, 512), Image.LANCZOS).save(os.path.join(dl, "Any_Ps3-icon-512.png"))

    print("font:", FONT)
    print("icons written to", RES, "+ download/Any_Ps3-icon-512.png")


if __name__ == "__main__":
    main()
