#!/usr/bin/env python3
"""Render the SC26 power-up sequence (BIOS POST + astronaut splash) as a looping GIF for the
GIF player. Uses the same sprites and timeline as fw/gifplayer/boot_splash.c and the badge's
own 10x19 bitmap font for the BIOS text.

  python3 make_boot_gif.py <char.svg> <astro.svg> <out.gif> [--fps 15]
"""
import argparse, math, os, re, subprocess, sys
import numpy as np
from PIL import Image

sys.path.insert(0, os.path.dirname(__file__))
import make_boot_assets as A

W = H = 360
FONT_H_PATH = os.path.join(os.path.dirname(__file__), "..", "fw", "common", "font.h")


def load_font():
    rows = re.findall(r"\{((?:0x[0-9A-Fa-f]{4},?){19})\}", open(FONT_H_PATH).read())
    return [[int(v, 16) for v in r.split(",") if v] for r in rows]   # 95 glyphs x 19 rows


FONT = load_font()
GRAY = np.array([200, 200, 200], np.float32)


def text(fb, x, y, s, col=GRAY):
    for ch in s:
        c = ord(ch)
        g = FONT[c - 32] if 32 <= c <= 126 else FONT[ord("?") - 32]
        for r, bits in enumerate(g):
            for k in range(10):
                if bits & (0x8000 >> k) and 0 <= y + r < H and 0 <= x + k < W:
                    fb[y + r, x + k] = col
        x += 10


def bios_frames():
    """[(frame, duration_ms)] reproducing bios_begin() + the drive lines + cursor blink."""
    X, Y0, DY = 58, 78, 19
    lines, out = [], []

    def frame(cursor=True, mem=None):
        fb = np.zeros((H, W, 3), np.float32)
        for i, s in enumerate(lines):
            if s:
                text(fb, X, Y0 + i * DY, s)
        if mem is not None:
            text(fb, X, Y0 + len(lines) * DY, mem)
        elif cursor:
            y = Y0 + len(lines) * DY + 2
            fb[y:y + 15, X:X + 9] = GRAY
        return fb

    out.append((np.zeros((H, W, 3), np.float32), 300))
    for s, ms in [("SC26 v2.0 by BP  (c) 2026", 140), ("", 0), ("CPU : RP2350 @ 150 MHz", 140)]:
        lines.append(s)
        out.append((frame(), max(ms, 40)))
    for kb in range(0, 521, 40):
        out.append((frame(mem="MEM : %3d KB" % kb), 40))
    lines.append("MEM : 520 KB  OK")
    for s, ms in [("ROM : 16 MB QSPI FLASH", 160), ("", 0), ("Detecting drives ...", 300),
                  ("  SC26 GIFS   6.4 MB", 120), ("  1 GIF  3 GAMES", 120), ("Booting SC26 OS ...", 0)]:
        lines.append(s)
        out.append((frame(), max(ms, 40)))
    for k in range(4):                                   # cursor blink
        out.append((frame(cursor=k % 2 == 1), 250))
    return out


def splash_frames(sprites, fps, seconds=6.0, fade=0.35):
    Asp, Bsp, TITLE, VER = sprites
    stars = A.stars_ref()
    out, n = [], int((seconds + fade) * fps)
    for f in range(n):
        t = f / fps
        fb = np.zeros((H, W, 3), np.float32)
        sfade = A.clamp01(t / 0.8)
        for (x, y, br, ph, sp, big) in stars:
            x = (x - 4 * t) % 360
            v = br * (0.65 + 0.35 * math.sin(ph + t * sp)) * sfade
            if big:
                fb[int(y):int(y) + 2, int(x):int(x) + 2] = v
            else:
                fb[int(y), int(x)] = v
        if 3.2 <= t < 3.75:
            p = (t - 3.2) / 0.55
            hx, hy = 330 - 230 * p, 60 + 90 * p
            for k in range(28):
                q = 1 - k / 28
                xx, yy = int(hx + k * 2.3), int(hy - k * 0.9)
                if 0 <= xx < W and 0 <= yy < H:
                    fb[yy, xx] = np.maximum(fb[yy, xx], 255 * q * (1 - p * 0.5))
        e = A.ease_out((t - 0.2) / 1.6)
        ax = -70 + (118 + 3 * math.sin(t * 2 * math.pi / 5.3) + 70) * e
        ay = 40 + (148 + 5 * math.sin(t * 2 * math.pi / 3.1) - 40) * e
        A.draw_sprite_ref(fb, Asp, ax, ay, -50 * (1 - e) + 7 * math.sin(t * 2 * math.pi / 4.6), 1.0,
                          A.clamp01((t - 0.2) / 0.4))
        e2 = A.ease_back((t - 0.7) / 1.4)
        by = 430 + (158 - 430) * e2 + 4 * math.sin((t - 0.7) * 2 * math.pi / 2.7) * A.clamp01(t - 2.1)
        pop = 1 + 0.035 * math.exp(-((t - 2.25) / 0.12) ** 2)
        A.draw_sprite_ref(fb, Bsp, 242, by, 2.5 * math.sin(t * 2 * math.pi / 3.3), pop, A.clamp01((t - 0.7) / 0.3))
        tf = A.clamp01((t - 1.6) / 0.8)
        if tf > 0:
            A.draw_sprite_ref(fb, TITLE, 180, 276 - 8 * A.ease_out(tf), 0, 1.0, tf)
            A.draw_sprite_ref(fb, VER, 180, 330, 0, 1.0, tf)
        # loader bar (same geometry as boot_splash.c draw_bar)
        vis = A.clamp01((t - 0.9) / 0.3)
        if vis > 0:
            g = 150 * vis
            fb[298, 129:232] = g; fb[311, 129:232] = g
            fb[301:309, 126] = g; fb[301:309, 234] = g
            for (dx, dy) in ((1, 2), (2, 1), (1, 1)):
                for (x, y) in ((126 + dx, 298 + dy), (234 - dx, 298 + dy), (126 + dx, 311 - dy), (234 - dx, 311 - dy)):
                    fb[y, x] = g
            if t >= 1.0:
                span = 108 + 33
                pos = ((t - 1.0) * 62) % span - 33
                for k in range(3):
                    xa = int(128 + pos + k * 11); xb = xa + 8
                    xa, xb = max(xa, 129), min(xb, 232)
                    for y in range(301, 309):
                        s = (y - 301) / 7
                        if xb > xa:
                            fb[y, xa:xb] = np.array([110 - 80 * s, 170 - 90 * s, 255 - 60 * s]) * vis
        if t > seconds:                                   # end: fade like the backlight does
            fb *= 1 - A.clamp01((t - seconds) / fade)
        out.append((fb, int(round(1000 / fps))))
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("char_svg"); ap.add_argument("astro_svg"); ap.add_argument("out")
    ap.add_argument("--fps", type=float, default=15)
    ap.add_argument("--lossy", type=int, default=40)
    a = ap.parse_args()
    sprites = [A.quantise(im) for im in (A.add_glow(A.render_svg(a.char_svg, 166)), A.add_glow(A.render_svg(a.astro_svg, 170)),
                                          A.text_sprite(), A.small_text_sprite("v2.0   by BP"))]
    frames = bios_frames() + splash_frames(sprites, a.fps) + [(np.zeros((H, W, 3), np.float32), 600)]
    imgs = [Image.fromarray(fb.clip(0, 255).astype(np.uint8)) for fb, _ in frames]
    durs = [d for _, d in frames]
    # one shared palette taken from the busiest frames keeps colours stable between frames
    sample = Image.new("RGB", (W * 4, H))
    for i, k in enumerate((-60, -40, -25, -10)):
        sample.paste(imgs[k], (i * W, 0))
    pal_img = sample.quantize(colors=255, method=Image.Quantize.MEDIANCUT)
    q = [im.quantize(palette=pal_img, dither=Image.Dither.NONE) for im in imgs]
    tmp = a.out + ".raw.gif"
    q[0].save(tmp, save_all=True, append_images=q[1:], duration=durs, loop=0, optimize=False, disposal=1)
    subprocess.run(["gifsicle", "-O3", f"--lossy={a.lossy}", "--no-comments", "--no-extensions", "-o", a.out, tmp], check=True)
    os.remove(tmp)
    print(f"{a.out}: {len(q)} frames, {os.path.getsize(a.out):,} bytes, {sum(durs) / 1000:.1f} s")


if __name__ == "__main__":
    main()
