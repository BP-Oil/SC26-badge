#!/usr/bin/env python3
"""gifprep.py - fit any animated GIF to the SC26 round 360x360 display.

    python3 gifprep.py input.gif [output.gif] [--mode circle|square|fill] [--max-fps 25] [--colors 128]

  circle  (default) whole picture visible inside the round screen
  square  fill the 360x360 square (corners fall outside the circle)
  fill    fill the circle completely, cropping the edges

Why: the player can scale on the fly, but pre-sizing gives sharper results (high quality
resampling), smaller files and faster decoding (fewer pixels per frame).
Needs: pip3 install pillow
"""
import argparse, math, os, sys
from PIL import Image, ImageSequence

SIZE = 360


def fit_size(w, h, mode):
    if mode == "square":
        s = min(SIZE / w, SIZE / h)
    elif mode == "fill":
        s = SIZE / min(w, h)
    else:
        s = SIZE / math.hypot(w, h)
    return max(1, round(w * s)), max(1, round(h * s))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("input")
    ap.add_argument("output", nargs="?")
    ap.add_argument("--mode", choices=["circle", "square", "fill"], default="circle")
    ap.add_argument("--max-fps", type=float, default=25.0, help="drop frames above this rate (default 25)")
    ap.add_argument("--colors", type=int, default=128, help="palette size per frame (default 128)")
    a = ap.parse_args()
    out = a.output or os.path.splitext(a.input)[0] + "_sc26.gif"

    src = Image.open(a.input)
    w, h = src.size
    nw, nh = fit_size(w, h, a.mode)
    min_ms = 1000.0 / a.max_fps
    frames, durations, pending = [], [], 0.0
    for fr in ImageSequence.Iterator(src):
        ms = fr.info.get("duration", 100) or 100
        if ms < 20:
            ms = 100                      # same rule browsers and the player use
        rgba = fr.convert("RGBA").resize((nw, nh), Image.LANCZOS)
        canvas = Image.new("RGB", (SIZE, SIZE), (0, 0, 0))
        canvas.paste(rgba, ((SIZE - nw) // 2, (SIZE - nh) // 2), rgba)
        if frames and pending + ms < min_ms:   # merge frames that come too fast
            pending += ms
            durations[-1] += ms
            continue
        pending = 0.0
        frames.append(canvas)
        durations.append(ms)

    pal = [f.quantize(colors=a.colors, method=Image.Quantize.MEDIANCUT, dither=Image.Dither.FLOYDSTEINBERG) for f in frames]
    pal[0].save(out, save_all=True, append_images=pal[1:], duration=[int(d) for d in durations],
                loop=0, optimize=True, disposal=1)
    kb = os.path.getsize(out) / 1024
    fps = len(frames) / (sum(durations) / 1000.0)
    print(f"{a.input}: {w}x{h}, {src.n_frames} frames -> {out}: {nw}x{nh} on 360x360, "
          f"{len(frames)} frames, {fps:.1f} fps avg, {kb:.0f} KB")
    if kb > 4096:
        print("note: over 4 MB - try --max-fps 15 or --colors 64 to shrink it")


if __name__ == "__main__":
    sys.exit(main())
