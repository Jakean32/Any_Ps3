#!/usr/bin/env python3
"""Any_Ps3 asset pipeline: generate all logo-derived assets for Android + Windows."""
import os, json
from PIL import Image, ImageDraw, ImageFilter, ImageOps

SRC = "/home/z/my-project/upload/file_00000000ee0881f58712cd8f547df02d.png"
OUT = "/home/z/my-project/assets"
ANDROID_RES = "/home/z/my-project/android/res"
WINDOWS_DIR = "/home/z/my-project/windows"
for d in (OUT, ANDROID_RES, WINDOWS_DIR):
    os.makedirs(d, exist_ok=True)

logo = Image.open(SRC).convert("RGBA")
W, H = logo.size  # 1254x1254

# ---------- palette extraction ----------
small = logo.convert("RGB").resize((64, 64))
warm, sky_px, dark_px = [], [], []
for y in range(64):
    for x in range(64):
        r, g, b = small.getpixel((x, y))
        if r > 140 and g > 95 and b < 115 and r > b + 45:   # temple gold
            warm.append((r, g, b))
        if b > r + 25 and b > 90:                            # sky blue
            sky_px.append((r, g, b))
        if (r * 299 + g * 587 + b * 114) // 1000 < 60:       # dark stone
            dark_px.append((r, g, b))

def avg(px):
    n = len(px)
    return tuple(sum(c[i] for c in px) // n for i in range(3)) if n else (128, 128, 128)

gold = avg(warm); sky = avg(sky_px); stone = avg(dark_px)
# accent: slightly saturated gold for dark theme contrast
accent = tuple(min(255, int(c * 1.05)) for c in gold)
palette = {
    "accent_gold": "#%02x%02x%02x" % accent,
    "gold_raw": "#%02x%02x%02x" % gold,
    "sky_blue": "#%02x%02x%02x" % sky,
    "deep_sky": "#%02x%02x%02x" % (sky[0] // 2, sky[1] // 2, sky[2] // 2),
    "dark_stone": "#%02x%02x%02x" % stone,
    "bg_dark": "#12151a",
    "surface": "#1b1f26",
    "surface_light": "#232833",
    "text_primary": "#e8e6e1",
    "text_secondary": "#a8a29a",
}
with open(os.path.join(OUT, "palette.json"), "w") as f:
    json.dump(palette, f, indent=2)
print("palette:", palette)

def rrect_mask(size, radius_ratio):
    """Rounded-rect alpha mask."""
    m = Image.new("L", (size, size), 0)
    d = ImageDraw.Draw(m)
    r = int(size * radius_ratio)
    d.rounded_rectangle([0, 0, size - 1, size - 1], radius=r, fill=255)
    return m

def circle_mask(size):
    m = Image.new("L", (size, size), 0)
    d = ImageDraw.Draw(m)
    d.ellipse([0, 0, size - 1, size - 1], fill=255)
    return m.filter(ImageFilter.GaussianBlur(0.5))

def crop_center(im, cw, ch):
    w, h = im.size
    l, t = (w - cw) // 2, (h - ch) // 2
    return im.crop((l, t, l + cw, t + ch))

# ---------- adaptive icon foreground (full-bleed 108dp grid) ----------
DENS = {"mdpi": 1, "hdpi": 1.5, "xhdpi": 2, "xxhdpi": 3, "xxxhdpi": 4}
for dpi, mult in DENS.items():
    px = int(108 * mult)
    d = os.path.join(ANDROID_RES, "mipmap-" + dpi)
    os.makedirs(d, exist_ok=True)
    fg = logo.resize((px, px), Image.LANCZOS)
    fg.save(os.path.join(d, "ic_launcher_foreground.png"))
    # background: solid sky-blue drawable via color; still ship a 1px? no, use color resource.

# monochrome adaptive layer: silhouette (non-sky -> white)
mono = Image.new("RGBA", (W, H), (0, 0, 0, 0))
mp = mono.load(); lp = logo.convert("RGB").load()
for y in range(0, H, 2):
    for x in range(0, W, 2):
        r, g, b = lp[x, y]
        if not (b > r + 25 and b > 90):
            # slightly enlarge alpha to keep 2px grid solid
            for dy in (0, 1):
                for dx in (0, 1):
                    if x + dx < W and y + dy < H:
                        mp[x + dx, y + dy] = (255, 255, 255, 255)
mono = mono.resize((256, 256), Image.LANCZOS)
for dpi, mult in DENS.items():
    px = int(108 * mult)
    d = os.path.join(ANDROID_RES, "mipmap-" + dpi)
    mono.resize((px, px), Image.LANCZOS).save(os.path.join(d, "ic_launcher_monochrome.png"))

# ---------- legacy launcher icons (pre-masked circle, blue ring) ----------
for dpi, mult in DENS.items():
    px = int(48 * mult)
    d = os.path.join(ANDROID_RES, "mipmap-" + dpi)
    ring_w = max(2, px // 18)
    ring = Image.new("RGBA", (px, px), (0, 0, 0, 0))
    dr = ImageDraw.Draw(ring)
    dr.ellipse([0, 0, px - 1, px - 1], fill=palette["deep_sky_rgb"] if "deep_sky_rgb" in palette else (1, 74, 142, 255))
    inner = crop_center(logo, W - 4, W - 4).resize((px - ring_w * 2, px - ring_w * 2), Image.LANCZOS)
    inner.putalpha(circle_mask(px - ring_w * 2))
    ring.paste(inner, (ring_w, ring_w), inner)
    ring.save(os.path.join(d, "ic_launcher.png"))

# ---------- full logo for splash / wizard ----------
logo512 = logo.copy()
logo512.putalpha(rrect_mask(W, 0.045))
logo512 = logo512.resize((512, 512), Image.LANCZOS)
os.makedirs(os.path.join(ANDROID_RES, "drawable-nodpi"), exist_ok=True)
logo512.save(os.path.join(ANDROID_RES, "drawable-nodpi", "logo_full.png"))
logo512.save(os.path.join(OUT, "logo_full.png"))
logo512.resize((192, 192), Image.LANCZOS).save(os.path.join(ANDROID_RES, "drawable-nodpi", "logo_small.png"))

# banner variant (wide, for wizard header): crop sky+temple band
band = crop_center(logo, W, int(H * 0.62)).resize((1024, 636), Image.LANCZOS)
band.putalpha(rrect_mask(0) if False else Image.new("L", (1024, 636), 255))
band.save(os.path.join(ANDROID_RES, "drawable-nodpi", "logo_banner.png"))
band.save(os.path.join(OUT, "logo_banner.png"))

# ---------- cover placeholder (16:9 tile) ----------
cp = crop_center(logo, W, int(W * 9 / 16)).resize((640, 360), Image.LANCZOS)
cp = ImageOps.d_brightness(cp, 0.55) if hasattr(ImageOps, "d_brightness") else cp
cp = Image.eval(cp, lambda v: int(v * 0.55))
d = ImageDraw.Draw(cp)
d.text((24, 300), "Any_Ps3", fill=(232, 230, 225))
cp.save(os.path.join(ANDROID_RES, "drawable-nodpi", "cover_placeholder.png"))
cp.save(os.path.join(OUT, "cover_placeholder.png"))

# ---------- Windows .ico (multi-size, rounded square) ----------
sizes = [(16, 16), (24, 24), (32, 32), (48, 48), (64, 64), (128, 128), (256, 256)]
ico_base = logo.copy()
ico_base.putalpha(rrect_mask(W, 0.09))
ico_base.save(os.path.join(WINDOWS_DIR, "anyps3.ico"), format="ICO", sizes=sizes)
ico_base.resize((256, 256), Image.LANCZOS).save(os.path.join(OUT, "icon_256.png"))
# verify
import struct as _s
_d = open(os.path.join(WINDOWS_DIR, "anyps3.ico"), "rb").read()
_cnt = _s.unpack_from("<H", _d, 4)[0]
print("ico frames:", _cnt)
assert _cnt == len(sizes), "ICO must contain all frame sizes"
print("assets written to", OUT, "and", ANDROID_RES)
