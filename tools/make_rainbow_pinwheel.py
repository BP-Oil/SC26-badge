#!/usr/bin/env python3
"""Rainbow pinwheel for the SC26 round screen (replaces the black-and-white stripe vortex).

  python3 make_rainbow_pinwheel.py out.gif [--frames 24] [--ms 60]

Eight curved paper-style blades, each a rainbow colour with a lighter fold, spinning one full
turn per loop around a white pin. The rim colours sweep round the badge's LED rocket in
ambient mode.
"""
import argparse, colorsys, math
import numpy as np
from PIL import Image

W = H = 360
SS = 2                       # supersampling for smooth blade edges


def frame(rot, n_blades=8):
    s = W * SS
    yy, xx = np.mgrid[0:s, 0:s].astype(np.float32)
    cx = cy = s / 2
    dx, dy = (xx + 0.5 - cx) / SS, (yy + 0.5 - cy) / SS
    r = np.hypot(dx, dy)
    ang = (np.arctan2(dy, dx) - rot) % (2 * np.pi)
    # blades curve with radius (a gentle spiral), like a paper pinwheel
    twist = ang + 0.010 * r
    pos = (twist / (2 * np.pi) * n_blades) % n_blades
    blade = np.floor(pos).astype(np.int32)
    frac = pos - blade
    img = np.zeros((s, s, 3), np.float32)
    for b in range(n_blades):
        rgb = np.array(colorsys.hsv_to_rgb(b / n_blades, 0.85, 1.0)) * 255
        light = rgb + (255 - rgb) * 0.35                 # the folded half catches the light
        m = blade == b
        lit = m & (frac < 0.45)
        img[m] = rgb * 0.92
        img[lit] = light
    # thin dark seams between blades
    seam = (frac < 0.03) | (frac > 0.97)
    img[seam] *= 0.35
    # white hub with a grey rim and a red pin (flat colours keep the GIF small and clean)
    img[r < 24] = (90, 90, 100)
    img[r < 21] = (245, 245, 250)
    pin = r < 7
    img[pin] = (200, 40, 60)
    img[r > 179] = 0
    im = Image.fromarray(img.clip(0, 255).astype(np.uint8))
    return im.resize((W, H), Image.LANCZOS)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("out")
    ap.add_argument("--frames", type=int, default=24)
    ap.add_argument("--ms", type=int, default=60)
    a = ap.parse_args()
    frames = [frame(2 * math.pi * f / a.frames) for f in range(a.frames)]
    # one shared palette keeps colours steady from frame to frame
    sample = Image.new("RGB", (W * 3, H))
    for i, k in enumerate((0, a.frames // 3, 2 * a.frames // 3)):
        sample.paste(frames[k], (i * W, 0))
    pal = sample.quantize(colors=96, method=Image.Quantize.MEDIANCUT)
    q = [f.quantize(palette=pal, dither=Image.Dither.NONE) for f in frames]
    q[0].save(a.out, save_all=True, append_images=q[1:], duration=a.ms, loop=0, optimize=False, disposal=1)


if __name__ == "__main__":
    main()
