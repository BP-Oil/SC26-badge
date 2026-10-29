#!/usr/bin/env python3
"""Build one UF2 that installs the whole SC26 flash map (see fw/common/flash_layout.h).

  python3 make_combined_uf2.py out.uf2 --os sc26_gif.bin --orig wayfarer.bin \
          --doom doom_tiny_nost.bin --whd doom.whd [--wipe-fs | --fs drive.img]

Every part is written at its absolute flash offset. --wipe-fs erases the first sector of
the USB-drive area so SC26 OS formats a fresh drive on its next boot (needed whenever the
drive moves). --fs writes a ready-made SC26 GIFS drive image instead (a FAT volume made by
SC26 OS's own formatter, e.g. by tools/make_release.py); trailing empty space is left out, since
the FAT already marks it free. Blocks are emitted in address order with consistent numbering.
"""
import argparse, struct

FAMILY_RP2350_ARM_S = 0xE48BFF59
XIP = 0x10000000
LAYOUT = {            # name: (offset, max size)
    "os":   (0x000000, 0x100000),
    "orig": (0x100000, 0x180000),
    "doom": (0x300000, 0x080000),
    "whd":  (0x380000, 0x600000),
}
FS_OFFSET = 0x980000
FS_SIZE = 0x680000


def blocks(addr, data):
    for i in range(0, len(data), 256):
        chunk = data[i:i + 256]
        yield addr + i, chunk + b"\x00" * (256 - len(chunk))


def fat_used_end(img):
    """Bytes of a FAT12/16 image that matter: boot sector, FATs, root directory and every
    cluster up to the highest one in use (the rest is free space)."""
    bps, spc = struct.unpack_from("<H", img, 11)[0], img[13]
    rsvd, nfats, root_ents, tot16 = struct.unpack_from("<H", img, 14)[0], img[16], *struct.unpack_from("<HH", img, 17)
    fatsz, tot32 = struct.unpack_from("<H", img, 22)[0], struct.unpack_from("<I", img, 32)[0]
    assert fatsz, "FAT32 not supported"
    root_secs = (root_ents * 32 + bps - 1) // bps
    data0 = (rsvd + nfats * fatsz + root_secs) * bps
    nclus = ((tot16 or tot32) * bps - data0) // (spc * bps)
    fat = img[rsvd * bps:(rsvd + fatsz) * bps]
    def entry(n):
        if nclus < 4085:                                   # FAT12
            v = struct.unpack_from("<H", fat, n * 3 // 2)[0]
            return v >> 4 if n & 1 else v & 0xFFF
        return struct.unpack_from("<H", fat, n * 2)[0]
    last = max([n for n in range(2, nclus + 2) if entry(n)], default=1)
    end = data0 + (last - 1) * spc * bps
    return (end + 4095) // 4096 * 4096


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("out")
    for k in LAYOUT:
        ap.add_argument("--" + k)
    ap.add_argument("--wipe-fs", action="store_true")
    ap.add_argument("--fs", help="drive image to place at the SC26 GIFS area")
    a = ap.parse_args()
    parts = []
    for k, (off, mx) in LAYOUT.items():
        p = getattr(a, k)
        if not p:
            continue
        d = open(p, "rb").read()
        assert len(d) <= mx, f"{k}: {len(d)} bytes > slot {mx}"
        parts.append((off, d, k))
    if a.wipe_fs and a.fs:
        ap.error("--wipe-fs and --fs are exclusive")
    if a.wipe_fs:
        parts.append((FS_OFFSET, b"\xff" * 256, "fs-wipe"))
    if a.fs:
        img = open(a.fs, "rb").read()
        assert len(img) <= FS_SIZE, f"drive image {len(img)} > {FS_SIZE}"
        end = fat_used_end(img)
        parts.append((FS_OFFSET, img[:end], "fs"))
    parts.sort()
    for (o1, d1, k1), (o2, d2, k2) in zip(parts, parts[1:]):
        assert o1 + len(d1) <= o2, f"{k1} overlaps {k2}"
    allb = [b for off, d, _ in parts for b in blocks(XIP + off, d)]
    with open(a.out, "wb") as f:
        # RP2350-E10 workaround, same as picotool --abs-block: a leading "ignore me" block in the
        # absolute family at the end of flash (flags: family + extension tags, RP2_IGNORE_BLOCK tag)
        f.write(struct.pack("<IIIIIIII", 0x0A324655, 0x9E5D5157, 0x0000A000, 0x10FFFF00, 256, 0, 2, 0xE48BFF57)
                + b"\xef" * 256 + struct.pack("<I", 0x9957E304) + b"\x00" * (476 - 260) + struct.pack("<I", 0x0AB16F30))
        for n, (addr, payload) in enumerate(allb):
            f.write(struct.pack("<IIIIIIII", 0x0A324655, 0x9E5D5157, 0x00002000, addr, 256, n, len(allb),
                                FAMILY_RP2350_ARM_S) + payload + b"\x00" * (476 - 256) + struct.pack("<I", 0x0AB16F30))
    for off, d, k in parts:
        print(f"  {k:8s} 0x{off:06x} .. 0x{off + len(d):06x}  ({len(d):,} bytes)")
    print(f"{a.out}: {len(allb)} blocks, {len(allb) * 512 / 1e6:.1f} MB")


if __name__ == "__main__":
    main()
