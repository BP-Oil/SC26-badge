#!/usr/bin/env python3
"""Turn the two boot-splash astronauts into skeletons (Halloween edition).

  python3 tools/make_skeletons.py boot/char.svg boot/Astro-3.svg boot/

Writes boot/skel_float.png and boot/skel_thumbs.png (700 px tall, RGBA). Feed them to
tools/make_boot_assets.py in place of the SVGs; it accepts PNGs as well.

Each astronaut keeps its helmet and silhouette. The suit is darkened like an X-ray, a
skeleton is drawn over it following the pose (landmarks below, in 700 px render
coordinates), the visor shows a skull with glowing eye sockets, and the gloves become
bony hands.
"""
import io, math, subprocess, sys
from PIL import Image, ImageChops, ImageDraw, ImageFilter

H = 700
SS = 3                                   # supersampling for smooth bone edges
BONE = (238, 230, 206)
BONE_SHADE = (176, 166, 140)
INK = (14, 14, 20)
EYE = (120, 255, 90)                     # sickly green glow in the sockets

# ---- landmarks (x, y) in the 700 px render --------------------------------------------
POSES = {
    "float": dict(
        visor=(305, 100, 128, 108, -12),          # cx, cy, w, h, tilt (deg)
        helmet=(300, 105, 190, 175),              # region kept un-darkened (cx, cy, w, h)
        neck=(262, 178), pelvis=(212, 340), tilt=-28,
        arms=[[(170, 152), (102, 198), (44, 252)], [(352, 218), (402, 292), (440, 362)]],
        hands=[((44, 252), (18, 282), 1.0), ((440, 362), (458, 402), 1.1)],
        legs=[[(180, 352), (78, 398), (82, 470)], [(232, 360), (196, 488), (128, 602)]],
        feet=[((82, 470), (70, 492)), ((128, 602), (96, 660))],
        thumbs_up=None),
    "thumbs": dict(
        visor=(125, 86, 116, 100, -6),
        helmet=(125, 85, 160, 160),
        neck=(140, 168), pelvis=(172, 330), tilt=-8,
        arms=[[(84, 172), (58, 268), (36, 330)], [(226, 128), (258, 190), (204, 176)]],
        hands=[((36, 330), (24, 360), 1.0)],
        legs=[[(136, 352), (108, 470), (114, 568)], [(210, 356), (268, 472), (308, 580)]],
        feet=[((114, 568), (100, 602)), ((308, 580), (326, 650))],
        thumbs_up=((188, 170), 1.15)),            # fist centre, scale
}


def render(path):
    if path.lower().endswith(".png"):
        im = Image.open(path).convert("RGBA")
        im = im.resize((round(im.width * H / im.height), H), Image.LANCZOS)
    else:
        png = subprocess.run(["rsvg-convert", "-f", "png", "-h", str(H), "--keep-aspect-ratio", path],
                             capture_output=True, check=True).stdout
        im = Image.open(io.BytesIO(png)).convert("RGBA")
    return im


def P(p):
    return (p[0] * SS, p[1] * SS)


def bone(d, a, b, w, ink=True):
    """A long bone from a to b: shaft plus knobbly ends."""
    a, b = P(a), P(b)
    w *= SS
    if ink:
        d.line([a, b], fill=INK, width=int(w + 4 * SS))
        for p in (a, b):
            for s in (-1, 1):
                dx, dy = b[0] - a[0], b[1] - a[1]
                L = math.hypot(dx, dy) or 1
                nx, ny = -dy / L * w * 0.45 * s, dx / L * w * 0.45 * s
                r = w * 0.62 + 2 * SS
                d.ellipse([p[0] + nx - r, p[1] + ny - r, p[0] + nx + r, p[1] + ny + r], fill=INK)
    d.line([a, b], fill=BONE, width=int(w))
    for p in (a, b):
        for s in (-1, 1):
            dx, dy = b[0] - a[0], b[1] - a[1]
            L = math.hypot(dx, dy) or 1
            nx, ny = -dy / L * w * 0.45 * s, dx / L * w * 0.45 * s
            r = w * 0.62
            d.ellipse([p[0] + nx - r, p[1] + ny - r, p[0] + nx + r, p[1] + ny + r], fill=BONE)
    # a thin shade line down one side of the shaft
    dx, dy = b[0] - a[0], b[1] - a[1]
    L = math.hypot(dx, dy) or 1
    ox, oy = -dy / L * w * 0.28, dx / L * w * 0.28
    d.line([(a[0] + ox, a[1] + oy), (b[0] + ox, b[1] + oy)], fill=BONE_SHADE, width=max(1, int(w * 0.18)))


def rot(p, c, ang):
    s, co = math.sin(ang), math.cos(ang)
    x, y = p[0] - c[0], p[1] - c[1]
    return (c[0] + x * co - y * s, c[1] + x * s + y * co)


def spine_and_ribs(d, neck, pelvis):
    n = 9
    pts = [(neck[0] + (pelvis[0] - neck[0]) * i / (n - 1), neck[1] + (pelvis[1] - neck[1]) * i / (n - 1)) for i in range(n)]
    for p in pts:                                        # vertebrae
        r = 7 * SS
        q = P(p)
        d.ellipse([q[0] - r - 2 * SS, q[1] - r * 0.8 - 2 * SS, q[0] + r + 2 * SS, q[1] + r * 0.8 + 2 * SS], fill=INK)
        d.ellipse([q[0] - r, q[1] - r * 0.8, q[0] + r, q[1] + r * 0.8], fill=BONE)
    ang = math.atan2(pelvis[1] - neck[1], pelvis[0] - neck[0]) - math.pi / 2
    L = math.hypot(pelvis[0] - neck[0], pelvis[1] - neck[1])
    # ribs: arcs around the upper 55 % of the spine, drawn in the spine's frame
    for i in range(5):
        t = 0.14 + i * 0.1
        cy = t * L
        half = 44 - i * 2.5
        for side in (-1, 1):
            arc = []
            for k in range(13):
                u = k / 12
                x = side * half * math.sin(u * math.pi * 0.62)
                y = cy + 16 * u * u - 2
                arc.append(rot((neck[0] + x, neck[1] + y), neck, ang))
            q = [P(p) for p in arc]
            d.line(q, fill=INK, width=int(12 * SS), joint="curve")
            d.line(q, fill=BONE, width=int(8 * SS), joint="curve")
    # collarbones
    for side in (-1, 1):
        a = rot((neck[0], neck[1] + 6), neck, ang)
        b = rot((neck[0] + side * 52, neck[1] + 2), neck, ang)
        bone(d, a, b, 7)
    # pelvis: one wide hip bowl with the two small openings low down, sacrum in the middle
    pc = pelvis
    bowl = []
    for k in range(25):
        u = k / 24 * math.pi
        x = -34 * math.cos(u)
        y = -16 * math.sin(u) * 0.55 + 18 * math.sin(u) ** 3 - 4
        bowl.append(rot((pc[0] + x, pc[1] + y), pc, ang))
    bowl += [rot((pc[0] + 30, pc[1] - 14), pc, ang), rot((pc[0] - 30, pc[1] - 14), pc, ang)]
    q = [P(p) for p in bowl]
    d.polygon(q, fill=BONE, outline=INK, width=int(3 * SS))
    for side in (-1, 1):
        c = P(rot((pc[0] + side * 15, pc[1] + 6), pc, ang))
        rx, ry = 6 * SS, 5 * SS
        d.ellipse([c[0] - rx, c[1] - ry, c[0] + rx, c[1] + ry], fill=INK)
    c = P(rot((pc[0], pc[1] - 6), pc, ang))
    d.ellipse([c[0] - 6 * SS, c[1] - 9 * SS, c[0] + 6 * SS, c[1] + 9 * SS], fill=BONE_SHADE, outline=INK, width=int(2 * SS))


def bony_hand(d, wrist, tip, scale):
    """Open skeletal hand: palm bones fanning out to four fingers + thumb."""
    ang = math.atan2(tip[1] - wrist[1], tip[0] - wrist[0])
    L = math.hypot(tip[0] - wrist[0], tip[1] - wrist[1]) * scale
    for f, spread in enumerate((-0.55, -0.18, 0.18, 0.5)):
        a1 = ang + spread * 0.55
        knuckle = (wrist[0] + math.cos(a1) * L * 0.55, wrist[1] + math.sin(a1) * L * 0.55)
        a2 = ang + spread * 0.75 + 0.25
        tipp = (knuckle[0] + math.cos(a2) * L * 0.6, knuckle[1] + math.sin(a2) * L * 0.6)
        bone(d, wrist, knuckle, 4)
        bone(d, knuckle, tipp, 3.5)
    a = ang - 1.15
    th = (wrist[0] + math.cos(a) * L * 0.62, wrist[1] + math.sin(a) * L * 0.62)
    bone(d, wrist, th, 4)


def thumbs_up_hand(d, c, s):
    """Skeleton fist with the thumb bone pointing up."""
    for i in range(4):                                   # curled finger knuckles
        y = c[1] - 10 * s + i * 9 * s
        bone(d, (c[0] - 4 * s, y), (c[0] + 14 * s, y + 2 * s), 6 * s)
    bone(d, (c[0] - 8 * s, c[1] - 12 * s), (c[0] - 14 * s, c[1] - 36 * s), 7 * s)   # thumb
    bone(d, (c[0] - 14 * s, c[1] - 36 * s), (c[0] - 16 * s, c[1] - 54 * s), 6 * s)


def skull(size, glow_eyes=True):
    """A skull facing the viewer, w x h, RGBA."""
    w, h = size
    im = Image.new("RGBA", (w * SS, h * SS), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    W, Hh = w * SS, h * SS
    # cranium + cheekbones + jaw
    d.ellipse([W * 0.08, 0, W * 0.92, Hh * 0.74], fill=BONE)
    d.rounded_rectangle([W * 0.24, Hh * 0.5, W * 0.76, Hh * 0.97], radius=W * 0.12, fill=BONE)
    d.ellipse([W * 0.12, Hh * 0.42, W * 0.36, Hh * 0.66], fill=BONE)
    d.ellipse([W * 0.64, Hh * 0.42, W * 0.88, Hh * 0.66], fill=BONE)
    # shading on one side
    sh = Image.new("L", im.size, 0)
    ImageDraw.Draw(sh).ellipse([W * 0.55, Hh * 0.05, W * 1.1, Hh * 0.9], fill=110)
    shade = Image.new("RGBA", im.size, BONE_SHADE + (255,))
    shade.putalpha(ImageChops.multiply(sh, im.getchannel("A")))
    im.alpha_composite(shade)
    d = ImageDraw.Draw(im)
    # eye sockets
    for x0 in (0.18, 0.56):
        d.ellipse([W * x0, Hh * 0.3, W * (x0 + 0.26), Hh * 0.56], fill=INK)
    # nose
    d.polygon([(W * 0.5, Hh * 0.55), (W * 0.43, Hh * 0.68), (W * 0.57, Hh * 0.68)], fill=INK)
    # teeth
    d.rectangle([W * 0.3, Hh * 0.76, W * 0.7, Hh * 0.88], fill=INK)
    for i in range(6):
        x = W * (0.31 + i * 0.065)
        d.rectangle([x, Hh * 0.765, x + W * 0.05, Hh * 0.82], fill=BONE)
        d.rectangle([x, Hh * 0.83, x + W * 0.05, Hh * 0.875], fill=BONE)
    im = im.resize((w, h), Image.LANCZOS)
    if glow_eyes:
        g = Image.new("RGBA", (w, h), (0, 0, 0, 0))
        gd = ImageDraw.Draw(g)
        for x0 in (0.24, 0.62):
            cx, cy = w * (x0 + 0.07), h * 0.44
            gd.ellipse([cx - w * 0.05, cy - h * 0.045, cx + w * 0.05, cy + h * 0.045], fill=EYE + (255,))
        halo = g.filter(ImageFilter.GaussianBlur(w * 0.06))
        im.alpha_composite(halo)
        im.alpha_composite(halo)
        im.alpha_composite(g)
    return im


def skeletonise(src, pose):
    base = render(src)
    w, h = base.size
    alpha = base.getchannel("A")

    # 1. darken the suit (everything outside the helmet) towards an X-ray navy
    keep = Image.new("L", base.size, 0)
    hx, hy, hw, hh = pose["helmet"]
    ImageDraw.Draw(keep).ellipse([hx - hw / 2, hy - hh / 2, hx + hw / 2, hy + hh / 2], fill=255)
    keep = keep.filter(ImageFilter.GaussianBlur(6))
    dark = Image.blend(base.convert("RGB"), Image.new("RGB", base.size, (20, 26, 44)), 0.72)
    rgb = Image.composite(base.convert("RGB"), dark, keep)
    out = rgb.convert("RGBA")
    out.putalpha(alpha)

    # 2. the skeleton, supersampled, clipped to the suit silhouette
    layer = Image.new("RGBA", (w * SS, h * SS), (0, 0, 0, 0))
    d = ImageDraw.Draw(layer)
    for chain in pose["legs"]:
        for a, b in zip(chain, chain[1:]):
            bone(d, a, b, 13)
    for a, b in pose["feet"]:
        bone(d, a, b, 10)
    spine_and_ribs(d, pose["neck"], pose["pelvis"])
    for chain in pose["arms"]:
        for a, b in zip(chain, chain[1:]):
            bone(d, a, b, 10)
    for wrist, tip, s in pose["hands"]:
        bony_hand(d, wrist, tip, s)
    if pose["thumbs_up"]:
        thumbs_up_hand(d, *pose["thumbs_up"])
    layer = layer.resize((w, h), Image.LANCZOS)
    # hands and thumbs may stick out past the gloves a little; everything else stays inside
    sil = alpha.filter(ImageFilter.MaxFilter(9))
    layer.putalpha(ImageChops.multiply(layer.getchannel("A"), sil))
    out.alpha_composite(layer)

    # 3. skull in the visor, under the glass: dim it a touch, then put the reflections back
    vx, vy, vw, vh, tilt = pose["visor"]
    sk = skull((int(vw * 0.72), int(vh * 0.92))).rotate(-tilt, resample=Image.BICUBIC, expand=True)
    sk_dim = Image.blend(Image.new("RGBA", sk.size, (0, 0, 0, 0)), sk, 0.92)
    out.alpha_composite(sk_dim, (int(vx - sk.width / 2), int(vy - sk.height / 2 + vh * 0.04)))
    # glass reflections: the bright, low-saturation pixels of the original visor
    vis = Image.new("L", base.size, 0)
    ImageDraw.Draw(vis).ellipse([vx - vw / 2, vy - vh / 2, vx + vw / 2, vy + vh / 2], fill=255)
    lum = base.convert("L")
    hi = lum.point(lambda v: 255 if v > 150 else 0)
    refl = ImageChops.multiply(hi, vis).filter(ImageFilter.GaussianBlur(1))
    glare = base.copy()
    glare.putalpha(refl.point(lambda v: v * 3 // 4))
    out.alpha_composite(glare)
    return out


def main():
    float_src, thumbs_src, out_dir = sys.argv[1:4]
    for name, src in (("float", float_src), ("thumbs", thumbs_src)):
        im = skeletonise(src, POSES[name])
        path = f"{out_dir.rstrip('/')}/skel_{name}.png"
        im.save(path)
        print("wrote", path, im.size)


if __name__ == "__main__":
    main()
