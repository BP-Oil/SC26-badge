#!/usr/bin/env python3
"""Third batch of original demo GIFs for the round 360x360 SC26 screen: retro computing.
All artwork is procedurally drawn here; no logos, characters or footage from real products/films.
Usage: python3 make_demo_gifs3.py <out_dir>
"""
import math, random, sys
from PIL import Image, ImageDraw, ImageFont, ImageFilter
import make_demo_gifs as base

OUT = sys.argv[1] if len(sys.argv) > 1 else "gifs"
base.OUT = OUT
W = H = 360; CX = CY = 180
vignette, save = base.vignette, base.save


def F(path, size): return ImageFont.truetype(path, size)
MONO, SANS = base.MONO, base.SANS


def centered(d, y, text, f, fill):
    d.text(((W - d.textlength(text, font=f)) / 2, y), text, font=f, fill=fill)


# 1 -- retro boot: BIOS POST then a fictional "SC26 OS" splash ---------------------------
def retro_boot():
    frames = []
    mono = F(MONO, 14)
    post = ["SC26 BIOS v1.0  (c) 2026", "", "CPU : RP2350 @ 150 MHz", "MEM : {} KB", "",
            "Detecting drives ...", "  SC26 GIFS   12 MB", "Booting from flash ..."]
    for f in range(34):
        img = Image.new("RGB", (W, H), (0, 0, 0))
        d = ImageDraw.Draw(img)
        lines = post[:min(len(post), 1 + f // 3)]
        for i, s in enumerate(lines):
            if "{}" in s:
                s = s.format(min(520, f * 24))
            d.text((58, 78 + i * 19), s, font=mono, fill=(200, 200, 200))
        if f % 4 < 2:
            d.rectangle((58, 78 + len(lines) * 19, 66, 78 + len(lines) * 19 + 14), fill=(200, 200, 200))
        frames.append(vignette(img, 0.3))
    big, small = F(SANS, 40), F(SANS, 14)
    for f in range(44):
        img = Image.new("RGB", (W, H))
        d = ImageDraw.Draw(img)
        for y in range(H):
            t = y / H
            d.line([(0, y), (W, y)], fill=(int(20 + 30 * t), int(10 + 20 * t), int(60 + 90 * t)))
        # original mark: ring with an orbiting dot
        d.ellipse((CX - 40, 70, CX + 40, 150), outline=(255, 200, 60), width=8)
        a = f / 44 * 2 * math.pi * 2
        ox, oy = CX + 40 * math.cos(a), 110 + 40 * math.sin(a)
        d.ellipse((ox - 9, oy - 9, ox + 9, oy + 9), fill=(80, 230, 255))
        centered(d, 162, "SC26 OS", big, (255, 255, 255))
        centered(d, 210, "starting up ...", small, (190, 200, 230))
        # chunky segmented progress bar
        d.rectangle((90, 250, 270, 270), outline=(200, 210, 240), width=2)
        for k in range(12):
            if (k - f) % 12 < 4:
                d.rectangle((95 + k * 14.6, 254, 95 + k * 14.6 + 11, 266), fill=(80, 230, 255))
        frames.append(vignette(img, 0.35))
    save(frames, "retro_boot.gif", 90, colors=64)


# 2 -- bouncing-logo screensaver that really hits the corner -----------------------------
def corner_bounce():
    N, S = 120, 250                      # square play-field inscribed in the round screen
    x0, y0 = CX - S // 2, CY - S // 2
    bw, bh = 76, 38
    lx, ly = S - bw, S - bh
    cols = [(255, 80, 80), (80, 220, 255), (255, 220, 60), (120, 255, 120), (255, 120, 255), (255, 160, 60)]
    f_logo, f_big = F(SANS, 26), F(SANS, 30)

    def tri(t, length):                  # triangle wave in [0, length] with t traversals
        t = t % 2
        return t * length if t <= 1 else (2 - t) * length

    frames, prev_dir, col = [], None, 0
    rnd = random.Random(5)
    confetti = []
    for f in range(N):
        tx, ty = 4 * f / N, 6 * f / N     # x: 4 traversals, y: 6 -> corners at f = 0 and N/2
        x, y = tri(tx, lx), tri(ty, ly)
        dirs = (int(tx) % 2, int(ty) % 2)
        if prev_dir is not None and dirs != prev_dir:
            col = (col + 1) % len(cols)
        prev_dir = dirs
        corner = f % (N // 2) < 10
        img = Image.new("RGB", (W, H), (0, 0, 0))
        d = ImageDraw.Draw(img)
        d.rectangle((x0 - 2, y0 - 2, x0 + S + 1, y0 + S + 1), outline=(40, 40, 50), width=2)
        bx, by = x0 + x, y0 + y
        d.rounded_rectangle((bx, by, bx + bw, by + bh), 9, fill=cols[col])
        d.text((bx + (bw - d.textlength("SC26", font=f_logo)) / 2, by + 5), "SC26", font=f_logo, fill=(0, 0, 0))
        if f % (N // 2) == 0:
            confetti = [(bx + bw / 2, by + bh / 2, rnd.uniform(-7, 7), rnd.uniform(-9, 3), rnd.choice(cols)) for _ in range(40)]
        if corner:
            k = f % (N // 2)
            for (cx, cy, vx, vy, c) in confetti:
                px, py = cx + vx * k, cy + vy * k + 0.6 * k * k
                d.rectangle((px, py, px + 5, py + 5), fill=c)
            centered(d, 156, "CORNER!", f_big, (255, 255, 255))
        frames.append(img)
    save(frames, "corner_bounce.gif", 40, colors=32)


# 3 -- short circuit: lightning hits a circuit board, an original little robot wakes up ---
def short_circuit():
    rnd = random.Random(11)
    frames = []
    traces = [[(rnd.randrange(60, 300), rnd.randrange(60, 300))] for _ in range(14)]
    for tr in traces:
        for _ in range(4):
            px, py = tr[-1]
            if rnd.random() < 0.5: tr.append((min(320, max(40, px + rnd.choice([-60, 60]))), py))
            else: tr.append((px, min(320, max(40, py + rnd.choice([-60, 60])))))
    sparks = []
    for f in range(60):
        img = Image.new("RGB", (W, H), (6, 30, 16))
        d = ImageDraw.Draw(img)
        glow = max(0.0, 1 - abs(f - 14) / 16)
        for tr in traces:
            d.line(tr, fill=(int(30 + 200 * glow), int(120 + 120 * glow), int(40 + 60 * glow)), width=3)
            for p in tr[1:-1]:
                d.ellipse((p[0] - 4, p[1] - 4, p[0] + 4, p[1] + 4), fill=(200, 170, 60))
        if 8 <= f <= 14:                                        # lightning strike
            pts, x = [(CX + 30, 0)], CX + 30
            for y in range(20, 175, 20):
                x += rnd.randint(-22, 22); pts.append((x, y))
            d.line(pts, fill=(255, 255, 255), width=5)
            d.line(pts, fill=(170, 200, 255), width=2)
            if f in (8, 10):
                img = Image.blend(img, Image.new("RGB", (W, H), (255, 255, 255)), 0.55)
                d = ImageDraw.Draw(img)
            sparks += [[x, 175, rnd.uniform(-6, 6), rnd.uniform(-8, 1), 12] for _ in range(10)]
        for s in sparks:
            if s[4] > 0:
                d.line([(s[0], s[1]), (s[0] - s[2], s[1] - s[3])], fill=(255, 230, 120), width=2)
                s[0] += s[2]; s[1] += s[3]; s[3] += 0.7; s[4] -= 1
        if f >= 22:                                            # original robot head wakes up
            t = min(1, (f - 22) / 8)
            hx, hy = CX, 205
            d.rounded_rectangle((hx - 62, hy - 48, hx + 62, hy + 48), 18, fill=(150, 160, 175), outline=(90, 95, 110), width=4)
            d.line([(hx, hy - 48), (hx, hy - 78)], fill=(90, 95, 110), width=4)
            blink = (f // 4) % 2
            d.ellipse((hx - 9, hy - 90, hx + 9, hy - 72), fill=(255, 60, 60) if blink else (110, 30, 30))
            er = 26
            d.ellipse((hx - er, hy - 30, hx + er, hy + 22), fill=(20, 24, 30))
            lum = int(255 * t) if not (36 <= f <= 38) else 20      # one blink
            d.ellipse((hx - 14, hy - 18, hx + 14, hy + 10), fill=(int(40 * t), lum, int(255 * t)))
            for k in range(5):
                on = (k + f) % 5 < 3 and t >= 1
                d.rectangle((hx - 40 + k * 17, hy + 30, hx - 30 + k * 17, hy + 38), fill=(80, 255, 140) if on else (40, 60, 50))
            if f >= 40:
                centered(d, 290, "SYSTEM ONLINE", F(SANS, 18), (120, 255, 160))
        frames.append(vignette(img, 0.4))
    save(frames, "short_circuit.gif", 70, colors=64)


# 4 -- the computer plays tic-tac-toe against itself: always a draw -----------------------
def tictactoe():
    games = [[4, 0, 8, 2, 1, 7, 6, 3, 5], [0, 4, 1, 7, 8, 2, 6, 3, 5], [2, 4, 6, 3, 5, 8, 0, 1, 7]]
    f_xo, f_t = F(SANS, 44), F(MONO, 18)
    frames = []
    g0 = 120
    cell = 66
    for gi, g in enumerate(games):
        for n in range(len(g) + 1):
            for _ in range(2):
                img = Image.new("RGB", (W, H), (0, 14, 6))
                d = ImageDraw.Draw(img)
                for k in (1, 2):
                    d.line([(CX - 99 + k * cell, g0 - 40), (CX - 99 + k * cell, g0 + 158)], fill=(60, 255, 120), width=3)
                    d.line([(CX - 99, g0 - 40 + k * cell), (CX + 99, g0 - 40 + k * cell)], fill=(60, 255, 120), width=3)
                for i, c in enumerate(g[:n]):
                    sx = CX - 99 + (c % 3) * cell + 18; sy = g0 - 40 + (c // 3) * cell + 8
                    d.text((sx, sy), "X" if i % 2 == 0 else "O", font=f_xo, fill=(150, 255, 170))
                centered(d, 62, f"GAME {gi + 1}", f_t, (60, 200, 100))
                if n == len(g):
                    centered(d, 290, "DRAW", f_t, (200, 255, 210))
                a = img.copy()
                frames.append(vignette(a, 0.5))
    for k in range(10):
        img = Image.new("RGB", (W, H), (0, 14, 6))
        d = ImageDraw.Draw(img)
        centered(d, 150, "NO WINNER", F(SANS, 36), (150, 255, 170) if k % 2 == 0 else (60, 160, 90))
        frames.append(vignette(img, 0.5))
    save(frames, "tictactoe_ai.gif", 160, colors=16)


# 5 -- neon light trails racing on a grid ---------------------------------------------------
def light_trails():
    frames = []
    step = 6
    a = dict(p=[(100, 260)], d=(0, -1), c=(60, 230, 255))
    b = dict(p=[(260, 100)], d=(0, 1), c=(255, 150, 40))
    turns_a = {14: (1, 0), 30: (0, -1), 44: (1, 0), 58: (0, 1)}
    turns_b = {12: (-1, 0), 26: (0, 1), 40: (-1, 0), 54: (0, -1)}
    crash = None
    for f in range(76):
        for bike, turns in ((a, turns_a), (b, turns_b)):
            if crash is None:
                if f in turns: bike["d"] = turns[f]
                x, y = bike["p"][-1]
                bike["p"].append((x + bike["d"][0] * step, y + bike["d"][1] * step))
        hb = b["p"][-1]
        if crash is None and (hb in b["p"][:-1] or hb in a["p"] or math.hypot(hb[0] - CX, hb[1] - CY) > 150):
            crash = (hb, f)
        img = Image.new("RGB", (W, H), (2, 4, 14))
        d = ImageDraw.Draw(img)
        for k in range(0, W, 30):
            d.line([(k, 0), (k, H)], fill=(10, 30, 60)); d.line([(0, k), (W, k)], fill=(10, 30, 60))
        for bike in (a, b):
            d.line(bike["p"], fill=bike["c"], width=5)
            hx, hy = bike["p"][-1]
            d.ellipse((hx - 6, hy - 6, hx + 6, hy + 6), fill=(255, 255, 255))
        if crash:
            (cx, cy), cf = crash
            r = (f - cf) * 7
            d.ellipse((cx - r, cy - r, cx + r, cy + r), outline=(255, 180, 60), width=4)
            if f - cf > 3:
                centered(d, 286, "CYAN WINS", F(SANS, 22), (60, 230, 255))
        img = Image.blend(img, img.filter(ImageFilter.GaussianBlur(3)), 0.45)
        frames.append(vignette(img, 0.45))
        if crash and f - crash[1] > 14:
            break
    save(frames, "light_trails.gif", 55, colors=48)


# 6 -- office printer: paper jam, forever -----------------------------------------------------
def paper_jam():
    frames = []
    for f in range(32):
        shake = (2 if f % 2 else -2) if (f // 8) % 2 else 0
        img = Image.new("RGB", (W, H), (60, 64, 72))
        d = ImageDraw.Draw(img)
        ox = shake
        d.rounded_rectangle((60 + ox, 150, 300 + ox, 260), 14, fill=(205, 205, 200), outline=(120, 120, 120), width=3)
        d.rectangle((90 + ox, 130, 270 + ox, 152), fill=(170, 170, 165))
        # crumpled paper sticking out
        pts = [(110 + ox, 132), (250 + ox, 132), (262 + ox, 96 + (f % 4) * 2), (220 + ox, 104), (196 + ox, 80),
               (160 + ox, 100), (128 + ox, 84), (100 + ox, 104)]
        d.polygon(pts, fill=(250, 250, 245), outline=(180, 180, 180))
        d.line([(160 + ox, 100), (176 + ox, 128)], fill=(200, 200, 200), width=2)
        # control panel
        d.rectangle((200 + ox, 196, 284 + ox, 222), fill=(40, 70, 50))
        if f % 6 < 4:
            d.text((206 + ox, 201), "PAPER JAM", font=F(MONO, 11), fill=(140, 255, 160))
        d.ellipse((90 + ox, 200, 106 + ox, 216), fill=(255, 40, 40) if f % 4 < 2 else (90, 20, 20))
        d.rectangle((80 + ox, 240, 280 + ox, 250), fill=(150, 150, 150))
        centered(d, 290, "ERROR 0x0BAD", F(MONO, 16), (255, 210, 120))
        frames.append(vignette(img, 0.4))
    save(frames, "paper_jam.gif", 80, colors=32)


if __name__ == "__main__":
    retro_boot(); corner_bounce(); short_circuit(); tictactoe(); light_trails(); paper_jam()
