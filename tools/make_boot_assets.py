#!/usr/bin/env python3
"""Build the SC26 boot-splash assets from the badge artwork.

  python3 make_boot_assets.py <char.svg> <astro.svg> <out_header.h> [preview.gif]

Each sprite is rendered from SVG, given a soft space-blue glow, quantised to 255 colours
(index 0 = transparent) and written as a C array with a 1-pixel transparent border so the
firmware's bilinear sampler never needs bounds checks. Palettes are stored premultiplied
as two lanes (0x00RR00BB, 0x00AA00GG) for fast blending. The optional preview GIF renders
the same timeline with the same maths the firmware uses (boot_splash.c).
"""
import io, math, random, sys
import numpy as np
import cairosvg
from PIL import Image, ImageDraw, ImageFont, ImageFilter

W = H = 360
SANS_B = "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf"
SANS_BO = "/usr/share/fonts/truetype/dejavu/DejaVuSans-BoldOblique.ttf"
SANS = "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"
ORANGE = (249, 110, 42)          # from the artwork's palette


def render_svg(path, height):
    png = cairosvg.svg2png(url=path, output_height=height)
    im = Image.open(io.BytesIO(png)).convert("RGBA")
    return im.crop(im.getbbox())


def add_glow(im, radius=5, color=(40, 90, 170), strength=0.55):
    pad = radius * 2
    big = Image.new("RGBA", (im.width + 2 * pad, im.height + 2 * pad), (0, 0, 0, 0))
    big.alpha_composite(im, (pad, pad))
    a = big.getchannel("A").filter(ImageFilter.GaussianBlur(radius))
    a = a.point(lambda v: int(min(255, v * strength * 1.6)))
    glow = Image.new("RGBA", big.size, color + (0,))
    glow.putalpha(a)
    out = Image.new("RGBA", big.size, (0, 0, 0, 0))
    out.alpha_composite(glow)
    out.alpha_composite(big)
    return out.crop(out.getbbox())


def text_sprite():
    big, sup = ImageFont.truetype(SANS_B, 40), ImageFont.truetype(SANS_BO, 17)
    im = Image.new("RGBA", (200, 60), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    d.text((4, 4), "SC26", font=big, fill=(255, 255, 255, 255))
    x = 4 + d.textlength("SC26", font=big) + 3
    d.text((x, 6), "OS", font=sup, fill=ORANGE + (255,))
    # gentle top-to-bottom shade on the white letters (keeps the orange superscript as is)
    px = np.array(im).astype(np.float32)
    shade = np.linspace(1.0, 0.78, im.height)[:, None]
    white = (px[..., 0] > 200) & (px[..., 1] > 200)
    for c in range(3):
        px[..., c] = np.where(white, px[..., c] * shade, px[..., c])
    im = Image.fromarray(px.clip(0, 255).astype(np.uint8), "RGBA")
    return im.crop(im.getbbox())


def small_text_sprite(s, size=12, color=(120, 128, 145)):
    f = ImageFont.truetype(SANS, size)
    im = Image.new("RGBA", (220, 24), (0, 0, 0, 0))
    ImageDraw.Draw(im).text((2, 2), s, font=f, fill=color + (255,))
    return im.crop(im.getbbox())


def quantise(im):
    """-> (w, h, indices[h][w] uint8 with 1px border, palette list of premultiplied RGBA)"""
    q = im.quantize(colors=255, method=Image.Quantize.FASTOCTREE, dither=Image.Dither.NONE)
    pal = q.getpalette(rawmode="RGBA")[: 256 * 4]
    idx = np.array(q, dtype=np.int32)
    alpha = np.array(im.getchannel("A"))
    n = len(pal) // 4
    entries = [tuple(pal[i * 4:(i + 1) * 4]) for i in range(n)]
    # shift by one so 0 is transparent; drop near-transparent pixels
    idx = idx + 1
    idx[alpha < 6] = 0
    out = np.zeros((im.height + 2, im.width + 2), dtype=np.uint8)
    out[1:-1, 1:-1] = idx
    prem = [(0, 0, 0, 0)]
    for (r, g, b, a) in entries[:255]:
        prem.append((r * a // 255, g * a // 255, b * a // 255, a))
    while len(prem) < 256:
        prem.append((0, 0, 0, 0))
    return out, prem


def c_sprite(name, idx, prem):
    h, w = idx.shape
    lines = [f"// {name}: {w - 2}x{h - 2} (+1 px transparent border)"]
    lines.append(f"static const uint8_t {name}_px[{w * h}] = {{")
    flat = idx.flatten()
    for i in range(0, len(flat), 32):
        lines.append("  " + ",".join(str(int(v)) for v in flat[i:i + 32]) + ",")
    lines.append("};")
    rb = [(r << 16) | b for (r, g, b, a) in prem]
    ag = [(a << 16) | g for (r, g, b, a) in prem]
    for lane, vals in (("rb", rb), ("ag", ag)):
        lines.append(f"static const uint32_t {name}_{lane}[256] = {{")
        for i in range(0, 256, 8):
            lines.append("  " + ",".join(f"0x{v:08x}" for v in vals[i:i + 8]) + ",")
        lines.append("};")
    lines.append(f"static const boot_sprite_t {name} = {{ {w}, {h}, {name}_px, {name}_rb, {name}_ag }};\n")
    return "\n".join(lines)


# ----------------------------------------------------------------------------- preview
def draw_sprite_ref(fb, spr, cx, cy, ang_deg, scale, fade):
    """numpy version of boot_splash.c draw_sprite(): bilinear, premultiplied, over fb (float RGB)."""
    idx, prem = spr
    prem = np.array(prem, dtype=np.float32)
    h, w = idx.shape
    a = math.radians(ang_deg)
    ca, sa = math.cos(a) / scale, math.sin(a) / scale
    ys, xs = np.mgrid[0:H, 0:W].astype(np.float32)
    dx, dy = xs + 0.5 - cx, ys + 0.5 - cy
    u = dx * ca + dy * sa + w / 2 - 0.5
    v = -dx * sa + dy * ca + h / 2 - 0.5
    iu, iv = np.floor(u).astype(np.int32), np.floor(v).astype(np.int32)
    ok = (iu >= 0) & (iv >= 0) & (iu < w - 1) & (iv < h - 1)
    fu, fv = (u - iu)[ok], (v - iv)[ok]
    iu, iv = iu[ok], iv[ok]
    c = (prem[idx[iv, iu]] * ((1 - fu) * (1 - fv))[:, None] + prem[idx[iv, iu + 1]] * (fu * (1 - fv))[:, None]
         + prem[idx[iv + 1, iu]] * ((1 - fu) * fv)[:, None] + prem[idx[iv + 1, iu + 1]] * (fu * fv)[:, None]) * fade
    sub = fb[ok]
    fb[ok] = c[:, :3] + sub * (1 - c[:, 3:4] / 255.0)


def ease_out(x): x = min(max(x, 0), 1); return 1 - (1 - x) ** 3
def ease_back(x):
    x = min(max(x, 0), 1); c1 = 1.4; c3 = c1 + 1
    return 1 + c3 * (x - 1) ** 3 + c1 * (x - 1) ** 2
def clamp01(x): return min(max(x, 0.0), 1.0)


def stars_ref(n=90, seed=26):
    r = random.Random(seed)
    out = []
    while len(out) < n:
        x, y = r.uniform(0, 360), r.uniform(0, 360)
        if (x - 180) ** 2 + (y - 180) ** 2 < 178 ** 2:
            out.append((x, y, r.randint(70, 255), r.uniform(0, 6.28), r.uniform(1.5, 4.0), r.random() < 0.12))
    return out


def preview(sprites, out_path, T=6.0, fps=25):
    A, B, TITLE, VER = sprites
    stars = stars_ref()
    frames = []
    for f in range(int(T * fps)):
        t = f / fps
        fb = np.zeros((H, W, 3), np.float32)
        sfade = clamp01(t / 0.8)
        for (x, y, br, ph, sp, big) in stars:
            x = (x - 4 * t) % 360
            tw = 0.65 + 0.35 * math.sin(ph + t * sp)
            v = br * tw * sfade
            if big:
                fb[int(y):int(y) + 2, int(x):int(x) + 2] = v
            else:
                fb[int(y), int(x)] = v
        # shooting star
        if 3.2 <= t < 3.75:
            p = (t - 3.2) / 0.55
            hx, hy = 330 - 230 * p, 60 + 90 * p
            for k in range(28):
                q = 1 - k / 28
                xx, yy = int(hx + k * 2.3), int(hy - k * 0.9)
                if 0 <= xx < W and 0 <= yy < H:
                    fb[yy, xx] = np.maximum(fb[yy, xx], 255 * q * (1 - p * 0.5))
        # floating astronaut
        e = ease_out((t - 0.2) / 1.6)
        ax = -70 + (118 + 3 * math.sin(t * 2 * math.pi / 5.3) + 70) * e
        ay = 40 + (148 + 5 * math.sin(t * 2 * math.pi / 3.1) - 40) * e
        aang = -50 * (1 - e) + 7 * math.sin(t * 2 * math.pi / 4.6)
        draw_sprite_ref(fb, A, ax, ay, aang, 1.0, clamp01((t - 0.2) / 0.4))
        # thumbs-up astronaut
        e2 = ease_back((t - 0.7) / 1.4)
        by = 430 + (158 - 430) * e2 + 4 * math.sin((t - 0.7) * 2 * math.pi / 2.7) * clamp01(t - 2.1)
        bang = 2.5 * math.sin(t * 2 * math.pi / 3.3)
        pop = 1 + 0.035 * math.exp(-((t - 2.25) / 0.12) ** 2)
        draw_sprite_ref(fb, B, 242, by, bang, pop, clamp01((t - 0.7) / 0.3))
        # title + version
        tf = clamp01((t - 1.6) / 0.8)
        if tf > 0:
            draw_sprite_ref(fb, TITLE, 180, 268 - 8 * ease_out(tf) + 8, 0, 1.0, tf)
            draw_sprite_ref(fb, VER, 180, 330, 0, 1.0, tf)
        # loader bar
        bf = clamp01((t - 0.9) / 0.3)
        if bf > 0:
            img = Image.fromarray(fb.clip(0, 255).astype(np.uint8))
            d = ImageDraw.Draw(img)
            g = int(150 * bf)
            d.rounded_rectangle((126, 298, 234, 311), 4, outline=(g, g, g), width=1)
            span = 108 + 3 * 11
            pos = ((t - 1.0) * 62) % span - 3 * 11 if t >= 1.0 else -40
            for k in range(3):
                x0 = 128 + pos + k * 11
                for yy in range(301, 309):
                    s = (yy - 301) / 7
                    col = (int(110 - 80 * s), int(170 - 90 * s), int(255 - 60 * s))
                    xa, xb = max(x0, 129), min(x0 + 8, 231)
                    if xb > xa:
                        d.line((xa, yy, xb, yy), fill=tuple(int(c * bf) for c in col))
            fb = np.array(img).astype(np.float32)
        img = Image.fromarray(fb.clip(0, 255).astype(np.uint8))
        mask = Image.new("L", (W, H), 0)
        ImageDraw.Draw(mask).ellipse((0, 0, W - 1, H - 1), fill=255)
        bg = Image.new("RGB", (W, H), (25, 25, 25))
        bg.paste(img, (0, 0), mask)
        frames.append(bg)
    frames[0].save(out_path, save_all=True, append_images=frames[1:], duration=int(1000 / fps), loop=0)
    return frames


def main():
    char_svg, astro_svg, out_h = sys.argv[1:4]
    prev = sys.argv[4] if len(sys.argv) > 4 else None
    A_img = add_glow(render_svg(char_svg, 166))
    B_img = add_glow(render_svg(astro_svg, 170))
    T_img = text_sprite()
    V_img = small_text_sprite("v2.1   by BP")
    sprites = [quantise(im) for im in (A_img, B_img, T_img, V_img)]
    names = ["spr_float", "spr_thumbs", "spr_title", "spr_version"]
    hdr = ["// Generated by tools/make_boot_assets.py from the badge artwork - do not edit.",
           "#pragma once", "#include <stdint.h>",
           "typedef struct { uint16_t w, h; const uint8_t *px; const uint32_t *rb, *ag; } boot_sprite_t;", ""]
    total = 0
    for n, (idx, prem) in zip(names, sprites):
        hdr.append(c_sprite(n, idx, prem))
        total += idx.size + 2048
        print(f"{n}: {idx.shape[1] - 2}x{idx.shape[0] - 2}")
    open(out_h, "w").write("\n".join(hdr))
    print(f"wrote {out_h} (~{total // 1024} KB of flash)")
    if prev:
        preview(sprites, prev)
        print("preview:", prev)


if __name__ == "__main__":
    main()
