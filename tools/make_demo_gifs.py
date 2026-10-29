#!/usr/bin/env python3
"""Generate original, theme-inspired demo GIFs for the SC26 round 360x360 display.

All artwork is procedurally generated here (no film footage). Content is kept inside
the 360 px circle. Usage:  python3 make_demo_gifs.py <out_dir>
"""
import math, os, random, sys
import numpy as np
from PIL import Image, ImageDraw, ImageFont, ImageFilter

W = H = 360
CX = CY = 180
OUT = sys.argv[1] if len(sys.argv) > 1 else "gifs"
os.makedirs(OUT, exist_ok=True)
MONO = "/usr/share/fonts/truetype/dejavu/DejaVuSansMono-Bold.ttf"
SANS = "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf"

yy, xx = np.mgrid[0:H, 0:W]
RADIUS = np.hypot(xx - CX + 0.5, yy - CY + 0.5)
CIRCLE = RADIUS <= 180


def vignette(img, strength=0.55):
    a = np.asarray(img).astype(np.float32)
    v = 1.0 - strength * np.clip((RADIUS - 110) / 70.0, 0, 1) ** 2
    a *= v[..., None]
    a[~CIRCLE] = 0
    return Image.fromarray(a.clip(0, 255).astype(np.uint8))


def save(frames, name, ms, colors=64):
    pal = [f.convert("P", palette=Image.ADAPTIVE, colors=colors, dither=Image.Dither.NONE) for f in frames]
    path = os.path.join(OUT, name)
    pal[0].save(path, save_all=True, append_images=pal[1:], duration=ms, loop=0, optimize=True, disposal=1)
    print(f"{name:24s} {len(frames):3d} frames  {os.path.getsize(path)/1024:7.1f} KB")


# 1 -- green code rain ---------------------------------------------------------
def code_rain():
    rnd = random.Random(7)
    font = ImageFont.truetype(MONO, 16)
    glyphs = list("0123456789ABCDEFXZ:=+*<>|$#%")
    cw, ch = 14, 18
    cols, rows = W // cw, H // ch + 1
    N = 40
    # pre-render glyph tiles (mirrored for an alien look)
    tiles = {}
    for g in glyphs:
        t = Image.new("L", (cw, ch), 0)
        ImageDraw.Draw(t).text((1, -1), g, font=font, fill=255)
        tiles[g] = t.transpose(Image.FLIP_LEFT_RIGHT)
    grid = [[rnd.choice(glyphs) for _ in range(rows)] for _ in range(cols)]
    drops = []
    for c in range(cols):
        for _ in range(2):
            drops.append(dict(c=c, phase=rnd.random(), speed=rnd.choice([1, 2]), tail=rnd.randint(6, 14)))
    frames = []
    for f in range(N):
        inten = np.zeros((rows, cols), np.float32)
        head = np.zeros((rows, cols), bool)
        for d in drops:
            span = rows + d["tail"]
            pos = int((d["phase"] * span + f * d["speed"] * span / N)) % span
            for k in range(d["tail"]):
                r = pos - k
                if 0 <= r < rows:
                    inten[r, d["c"]] = max(inten[r, d["c"]], 1.0 - k / d["tail"])
                    if k == 0:
                        head[r, d["c"]] = True
        if f % 3 == 0:  # glyphs mutate
            for _ in range(25):
                grid[rnd.randrange(cols)][rnd.randrange(rows)] = rnd.choice(glyphs)
        img = Image.new("RGB", (W, H), (0, 0, 0))
        for c in range(cols):
            for r in range(rows):
                v = inten[r, c]
                if v <= 0.02:
                    continue
                col = (200, 255, 200) if head[r, c] else (0, int(70 + 185 * v), int(25 * v))
                img.paste(col, (c * cw, r * ch - 6), tiles[grid[c][r]])
        img = Image.blend(img, img.filter(ImageFilter.GaussianBlur(3)), 0.35)
        frames.append(vignette(img, 0.4))
    save(frames, "code_rain.gif", 70, colors=48)


# 2 -- hacker terminal ---------------------------------------------------------
def terminal():
    font = ImageFont.truetype(MONO, 17)
    big = ImageFont.truetype(MONO, 26)
    script = [
        "> connect 10.13.37.1:443",
        "  handshake ............ OK",
        "> scan --deep subnet/24",
        "  [######----] 61%",
        "  [##########] 100%",
        "> crack vault.key",
        "  trying 0x3F9A...",
        "  trying 0xC0DE...",
        "  KEY FOUND",
        "> open /core",
    ]
    frames, typed = [], []
    lines = [(s, i) for i, s in enumerate(script)]
    seq = []
    for s, _ in lines:
        for k in range(0, len(s) + 1, 3):
            seq.append(("type", s[:k]))
        seq.append(("line", s))
    seq += [("granted", i) for i in range(10)]
    for step in seq:
        img = Image.new("RGB", (W, H), (0, 6, 2))
        d = ImageDraw.Draw(img)
        y = 70
        shown = typed[-9:]
        for s in shown:
            d.text((40, y), s, font=font, fill=(60, 255, 120))
            y += 22
        if step[0] == "type":
            d.text((40, y), step[1] + "_", font=font, fill=(150, 255, 170))
        elif step[0] == "line":
            typed.append(step[1])
        else:
            on = step[1] % 2 == 0
            d.rectangle((58, 150, 302, 210), fill=(0, 40, 10) if on else (0, 10, 4), outline=(60, 255, 120), width=3)
            tw = d.textlength("ACCESS GRANTED", font=big)
            d.text(((W - tw) / 2, 164), "ACCESS GRANTED", font=big, fill=(160, 255, 180) if on else (40, 160, 70))
        a = np.asarray(img).astype(np.float32)
        a[::3] *= 0.6  # scanlines
        frames.append(vignette(Image.fromarray(a.astype(np.uint8)), 0.6))
    save(frames, "hacker_terminal.gif", 90, colors=32)


# 3 -- neon cyberspace grid ----------------------------------------------------
def neon_grid():
    N = 30
    frames = []
    for f in range(N):
        img = Image.new("RGB", (W, H), (8, 0, 20))
        d = ImageDraw.Draw(img)
        horizon = 175
        for i in range(horizon):  # sky gradient
            t = i / horizon
            d.line([(0, i), (W, i)], fill=(int(10 + 60 * t), 0, int(40 + 50 * t)))
        for r, c in ((70, (255, 60, 140)), (60, (255, 120, 80)), (50, (255, 210, 90))):
            d.ellipse((CX - r, horizon - 30 - r, CX + r, horizon - 30 + r), fill=c)
        for k in range(6):  # sun slats
            yk = horizon - 40 + k * 8
            d.rectangle((CX - 80, yk, CX + 80, yk + 2 + k // 2), fill=(10 + 10 * k, 0, 45 + 8 * k))
        d.rectangle((0, horizon, W, H), fill=(5, 0, 18))
        for i in range(-12, 13):  # converging lines
            d.line([(CX + i * 6, horizon), (CX + i * 70, H)], fill=(0, 220, 255), width=2)
        off = f / N
        for j in range(14):  # horizontal lines, perspective-spaced
            z = (j + 1 - off)
            y = horizon + int(185 / z) if z > 0.05 else H
            if horizon < y < H:
                d.line([(0, y), (W, y)], fill=(255, 40, 200), width=2)
        img = Image.blend(img, img.filter(ImageFilter.GaussianBlur(4)), 0.4)
        frames.append(vignette(img, 0.5))
    save(frames, "neon_grid.gif", 50, colors=64)


# 4 -- vault dial heist --------------------------------------------------------
def vault():
    N = 48
    font = ImageFont.truetype(SANS, 13)
    frames = []
    targets = [0, 300, 60, 210, 210]
    for f in range(N):
        seg = min(f // 12, 3)
        t = (f % 12) / 11
        a0, a1 = targets[seg], targets[seg + 1]
        ang = a0 + (a1 - a0) * (0.5 - 0.5 * math.cos(math.pi * t)) if f < 36 else 210
        opened = max(0, f - 36) / 11
        img = Image.new("RGB", (W, H), (20, 18, 16))
        d = ImageDraw.Draw(img)
        d.ellipse((20, 20, 340, 340), fill=(70, 72, 78), outline=(150, 150, 160), width=6)
        for k in range(12):  # bolts
            a = k * math.pi / 6
            bx, by = CX + 145 * math.cos(a), CY + 145 * math.sin(a)
            d.ellipse((bx - 7, by - 7, bx + 7, by + 7), fill=(170, 170, 180))
        # dial
        R = 95
        d.ellipse((CX - R, CY - R, CX + R, CY + R), fill=(35, 35, 40), outline=(210, 180, 90), width=5)
        for k in range(0, 360, 10):
            a = math.radians(k + ang - 90)
            r1 = R - (14 if k % 30 == 0 else 8)
            d.line([(CX + r1 * math.cos(a), CY + r1 * math.sin(a)), (CX + (R - 3) * math.cos(a), CY + (R - 3) * math.sin(a))],
                   fill=(230, 200, 110), width=2)
            if k % 30 == 0:
                tx, ty = CX + (R - 28) * math.cos(a), CY + (R - 28) * math.sin(a)
                s = str(k // 3)
                d.text((tx - d.textlength(s, font=font) / 2, ty - 8), s, font=font, fill=(240, 220, 150))
        d.polygon([(CX, CY - R - 14), (CX - 9, CY - R - 28), (CX + 9, CY - R - 28)], fill=(255, 70, 50))
        d.ellipse((CX - 22, CY - 22, CX + 22, CY + 22), fill=(120, 100, 50))
        # unlock: gold glow spreading from the center
        if opened > 0:
            a = np.asarray(img).astype(np.float32)
            glow = np.clip(1 - RADIUS / (40 + 160 * opened), 0, 1)[..., None]
            a = a * (1 - 0.8 * glow) + np.array([255, 205, 70]) * glow * 0.9
            img = Image.fromarray(a.clip(0, 255).astype(np.uint8))
            d = ImageDraw.Draw(img)
            if opened > 0.5:
                tw = d.textlength("OPEN", font=ImageFont.truetype(SANS, 34))
                d.text(((W - tw) / 2, CY - 20), "OPEN", font=ImageFont.truetype(SANS, 34), fill=(60, 40, 0))
        frames.append(vignette(img, 0.35))
    save(frames, "vault_heist.gif", 60, colors=64)


# 5 -- striped vortex ----------------------------------------------------------
def vortex():
    N = 24
    ang = np.arctan2(yy - CY, xx - CX)
    logr = np.log(np.maximum(RADIUS, 1))
    frames = []
    for f in range(N):
        ph = 2 * math.pi * f / N
        v = np.sin(6 * ang + 5 * logr - ph)
        stripes = (v > 0).astype(np.float32)
        # soft edge + slight green tint in the dark bands
        img = np.zeros((H, W, 3), np.float32)
        img[..., 0] = 245 * stripes + 10 * (1 - stripes)
        img[..., 1] = 245 * stripes + 30 * (1 - stripes)
        img[..., 2] = 235 * stripes + 12 * (1 - stripes)
        frames.append(vignette(Image.fromarray(img.astype(np.uint8)), 0.6))
    save(frames, "stripe_vortex.gif", 60, colors=16)


# 6 -- island searchlight ------------------------------------------------------
def beacon():
    N = 36
    rnd = random.Random(3)
    stars = [(rnd.randrange(W), rnd.randrange(150), rnd.random()) for _ in range(70)]
    frames = []
    for f in range(N):
        img = Image.new("RGB", (W, H), (4, 8, 22))
        d = ImageDraw.Draw(img)
        for i in range(200):
            d.line([(0, i), (W, i)], fill=(4, 8 + i // 20, 22 + i // 6))
        for x, y, b in stars:
            tw = 0.5 + 0.5 * math.sin(f / N * 2 * math.pi * 2 + b * 9)
            c = int(100 + 155 * tw * b)
            d.point((x, y), fill=(c, c, c))
        d.ellipse((250, 50, 290, 90), fill=(235, 235, 215))
        # sea with moving waves
        for i in range(200, H):
            d.line([(0, i), (W, i)], fill=(2, 12, 30))
        for k in range(10):
            y = 210 + k * 15
            for x in range(-40, W, 40):
                xo = x + (f * 40 / N * (1 + k % 2)) % 40
                d.arc((xo, y, xo + 30, y + 8), 200, 340, fill=(30, 70, 110), width=1)
        # island + tower
        d.polygon([(70, 205), (120, 170), (210, 160), (300, 205)], fill=(16, 18, 22))
        d.rectangle((160, 120, 176, 165), fill=(40, 42, 50))
        d.rectangle((156, 110, 180, 122), fill=(60, 60, 70))
        # rotating beam
        a = 2 * math.pi * f / N
        reach = 300
        beam = Image.new("L", (W, H), 0)
        bd = ImageDraw.Draw(beam)
        spread = 0.12 * (0.6 + 0.4 * abs(math.cos(a)))
        dx = math.cos(a)
        tip = (168 + reach * dx, 115 - 30 * math.sin(a))
        bd.polygon([(168, 116), (168 + reach * math.cos(a - spread), 115 - 30 * math.sin(a - spread)),
                    (168 + reach * math.cos(a + spread), 115 - 30 * math.sin(a + spread))], fill=int(235 * (0.55 + 0.45 * abs(dx))))
        beam = beam.filter(ImageFilter.GaussianBlur(4))
        img = Image.composite(Image.new("RGB", (W, H), (255, 245, 200)), img, beam)
        d = ImageDraw.Draw(img)
        d.ellipse((161, 108, 175, 122), fill=(255, 250, 220))
        frames.append(vignette(img, 0.45))
    save(frames, "island_beacon.gif", 70, colors=64)


if __name__ == "__main__":
    code_rain(); terminal(); neon_grid(); vault(); vortex(); beacon()
