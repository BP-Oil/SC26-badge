#!/usr/bin/env python3
"""Second batch of original demo GIFs for the round 360x360 SC26 screen:
office humour, a 60-second countdown, and meme-style loops. All procedurally drawn here.
Usage: python3 make_demo_gifs2.py <out_dir>
"""
import math, sys
import numpy as np
from PIL import Image, ImageDraw, ImageFont, ImageFilter
import make_demo_gifs as base

OUT = sys.argv[1] if len(sys.argv) > 1 else "gifs"
base.OUT = OUT
W = H = 360; CX = CY = 180
SANS = base.SANS; MONO = base.MONO
vignette, save = base.vignette, base.save


def font(path, size): return ImageFont.truetype(path, size)


def centered(d, y, text, f, fill):
    d.text(((W - d.textlength(text, font=f)) / 2, y), text, font=f, fill=fill)


# 1 -- office: stapler set in a wobbling gelatin block ----------------------------
def office_gelatin():
    N = 30
    frames = []
    for f in range(N):
        t = f / N * 2 * math.pi
        img = Image.new("RGB", (W, H), (58, 46, 38))
        d = ImageDraw.Draw(img)
        d.rectangle((0, 238, W, H), fill=(120, 86, 56))                    # desk
        for k in range(0, W, 24): d.line([(k, 238), (k + 40, H)], fill=(108, 76, 48))
        # sticky note
        d.polygon([(214, 62), (282, 68), (276, 132), (210, 126)], fill=(255, 236, 120))
        d.text((219, 86), "MONDAY", font=font(SANS, 13), fill=(90, 70, 20))
        # gelatin block: sheared, squashed with the wobble
        wob = math.sin(t) * 10 * (0.6 + 0.4 * math.cos(2 * t))
        squash = 1 + 0.05 * math.cos(2 * t)
        bx0, bx1, by1 = 95, 265, 240
        by0 = by1 - int(120 * squash)
        layer = Image.new("RGBA", (W, H), (0, 0, 0, 0))
        ld = ImageDraw.Draw(layer)
        top_shift = wob
        ld.polygon([(bx0 + top_shift, by0), (bx1 + top_shift, by0), (bx1, by1), (bx0, by1)], fill=(255, 205, 40, 170))
        ld.polygon([(bx0 + top_shift, by0), (bx1 + top_shift, by0), (bx1 - 12 + top_shift, by0 - 16),
                    (bx0 + 12 + top_shift, by0 - 16)], fill=(255, 230, 110, 190))
        # stapler inside (moves with ~half the wobble)
        sx = 125 + wob * 0.5; sy = 185
        ld.rounded_rectangle((sx, sy, sx + 110, sy + 22), 8, fill=(60, 60, 66, 230))        # base
        ld.rounded_rectangle((sx + 4, sy - 22, sx + 106, sy - 2), 9, fill=(170, 30, 36, 235))   # top
        ld.ellipse((sx + 92, sy - 18, sx + 104, sy - 6), fill=(90, 90, 96, 235))
        img = Image.alpha_composite(img.convert("RGBA"), layer).convert("RGB")
        d = ImageDraw.Draw(img)
        d.line([(bx0 + 18 + top_shift, by0 + 10), (bx0 + 10, by1 - 12)], fill=(255, 250, 220), width=4)  # gloss
        centered(d, 266, "DO NOT DISTURB", font(SANS, 18), (255, 240, 200))
        frames.append(vignette(img, 0.4))
    save(frames, "office_gelatin.gif", 60, colors=64)


# 2 -- sixty-second countdown speedometer -----------------------------------------
def sixty_seconds():
    frames = []
    big = font(SANS, 64); small = font(SANS, 15)
    for f in range(0, 61):
        sec = 60 - f
        img = Image.new("RGB", (W, H), (8, 8, 12))
        d = ImageDraw.Draw(img)
        # speed streaks
        for k in range(26):
            y = 30 + (k * 53) % 300
            x = (k * 97 + f * 38) % (W + 160) - 160
            d.line([(x, y), (x + 70 + (k % 4) * 25, y)], fill=(180, 30 + (k % 3) * 30, 20), width=2)
        # gauge
        d.arc((40, 40, 320, 320), 135, 405, fill=(80, 80, 90), width=14)
        frac = (f / 60) ** 0.8
        d.arc((40, 40, 320, 320), 135, 135 + 270 * frac, fill=(255, 70 + int(120 * (1 - frac)), 30), width=14)
        for k in range(0, 13):
            a = math.radians(135 + k * 22.5)
            d.line([(CX + 118 * math.cos(a), CY + 118 * math.sin(a)), (CX + 132 * math.cos(a), CY + 132 * math.sin(a))],
                   fill=(220, 220, 230), width=3)
        a = math.radians(135 + 270 * frac)
        d.line([(CX, CY), (CX + 110 * math.cos(a), CY + 110 * math.sin(a))], fill=(255, 255, 255), width=5)
        d.ellipse((CX - 12, CY - 12, CX + 12, CY + 12), fill=(200, 30, 30))
        centered(d, 205, f"{sec:02d}", big, (255, 230, 120) if sec > 10 else (255, 80, 60))
        centered(d, 278, "SECONDS LEFT", small, (200, 200, 210))
        frames.append(vignette(img, 0.35))
    for _ in range(6):   # flash at zero
        frames.append(frames[-1] if len(frames) % 2 else Image.new("RGB", (W, H), (120, 10, 10)))
    save(frames, "sixty_seconds.gif", 70, colors=64)


# 3 -- meme: bouncing logo on a screen with no corners -----------------------------
def no_corners():
    frames, N = [], 60
    x, y, vx, vy = 120.0, 140.0, 5.3, 4.1
    hue = 0
    cols = [(255, 80, 80), (80, 220, 255), (255, 220, 60), (120, 255, 120), (255, 120, 255), (255, 160, 60)]
    f_logo = font(SANS, 30); f_txt = font(SANS, 17)
    flash = 0
    for f in range(N):
        x += vx; y += vy
        dx, dy = x - CX, y - CY
        r = math.hypot(dx, dy)
        if r > 128:                                   # reflect off the circular wall
            nx, ny = dx / r, dy / r
            dot = vx * nx + vy * ny
            vx -= 2 * dot * nx; vy -= 2 * dot * ny
            x, y = CX + nx * 127, CY + ny * 127
            hue = (hue + 1) % len(cols); flash = 6
        img = Image.new("RGB", (W, H), (0, 0, 0))
        d = ImageDraw.Draw(img)
        d.rounded_rectangle((x - 55, y - 24, x + 55, y + 24), 10, outline=cols[hue], width=4)
        d.text((x - d.textlength("SC26", font=f_logo) / 2, y - 18), "SC26", font=f_logo, fill=cols[hue])
        if flash:
            centered(d, 300, "STILL NO CORNERS", f_txt, (255, 255, 255))
            flash -= 1
        frames.append(vignette(img, 0.2))
    save(frames, "meme_no_corners.gif", 40, colors=32)


# 4 -- meme: loading stuck at 99% ---------------------------------------------------
def loading_99():
    frames = []
    f_big = font(SANS, 46); f_small = font(SANS, 16)
    pct = [min(99, int(99 * (1 - (1 - k / 22) ** 3))) for k in range(23)] + [99] * 25
    msgs = ["almost there", "almost there.", "almost there..", "any second now", "trust the process"]
    for k, p in enumerate(pct):
        img = Image.new("RGB", (W, H), (14, 16, 24))
        d = ImageDraw.Draw(img)
        d.rounded_rectangle((60, 170, 300, 196), 12, outline=(140, 150, 170), width=3)
        d.rounded_rectangle((64, 174, 64 + int(232 * p / 100), 192), 9, fill=(80, 200, 255))
        centered(d, 100, f"{p}%", f_big, (240, 240, 250))
        if k >= 23:
            centered(d, 222, msgs[(k - 23) // 5 % len(msgs)], f_small, (170, 175, 190))
            if (k // 3) % 2:
                d.ellipse((CX - 5, 260, CX + 5, 270), fill=(80, 200, 255))
        frames.append(vignette(img, 0.3))
    save(frames, "meme_loading_99.gif", 90, colors=32)


# 5 -- meme: "typing..." bubble that never sends -----------------------------------
def typing():
    frames = []
    f_s = font(SANS, 15)
    for k in range(36):
        img = Image.new("RGB", (W, H), (236, 238, 244))
        d = ImageDraw.Draw(img)
        d.rounded_rectangle((70, 110, 250, 160), 24, fill=(60, 140, 255))
        d.text((92, 125), "u up?", font=font(SANS, 20), fill=(255, 255, 255))
        d.rounded_rectangle((110, 190, 220, 240), 24, fill=(210, 212, 220))
        for j in range(3):
            ph = (k / 12 * 2 * math.pi) - j * 0.9
            yb = 215 - max(0, math.sin(ph)) * 8
            d.ellipse((132 + j * 26 - 7, yb - 7, 132 + j * 26 + 7, yb + 7), fill=(120, 122, 132))
        centered(d, 262, "typing...", f_s, (130, 132, 140))
        frames.append(vignette(img, 0.25))
    save(frames, "meme_typing.gif", 60, colors=32)


if __name__ == "__main__":
    office_gelatin(); sixty_seconds(); no_corners(); loading_99(); typing()
