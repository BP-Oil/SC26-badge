#!/usr/bin/env python3
"""Build the downloadable SC26 OS release: install/update UF2s, README, licences, source, zip.

  python3 tools/make_release.py --out release --version 2.1 \\
      --os fw/build/sc26_gif.bin --orig wayfarer.bin \\
      --doom p3/rp2040-doom/build_sc26/src/doom_tiny_nost.bin \\
      --restore original_wayfarer_restore.uf2 --boot-gif out/sc26_boot.gif

Public builds carry no game data. For a private build of games you own, add --whd doom.whd
(DOOM data) and --rom game.nes (repeatable; goes into GAMES/), and optionally --src.

The SC26 GIFS drive comes pre-loaded: the full install writes a drive image made from
tools/sc26_blank_drive.img (SC26 OS's own fresh format) plus the boot GIF and any --rom games.
SYSINFO.TXT is left out so each badge writes its own install check on first boot.
Needs mtools (mcopy/mdel/mdir).
"""
import argparse, os, shutil, subprocess, sys, zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
FS_SIZE = 0x680000
ENV = dict(os.environ, MTOOLS_SKIP_CHECK="1", MTOOLS_NO_VFAT="0")


def mt(*args):
    subprocess.run(list(args), check=True, env=ENV, stdout=subprocess.DEVNULL)


def build_drive(img, boot_gif, roms):
    blank = open(os.path.join(HERE, "sc26_blank_drive.img"), "rb").read()
    open(img, "wb").write(blank + b"\0" * (FS_SIZE - len(blank)))
    subprocess.run(["mdel", "-i", img, "::/SYSINFO.TXT"], env=ENV, stderr=subprocess.DEVNULL)
    if boot_gif:
        mt("mcopy", "-D", "o", "-i", img, boot_gif, "::/sc26_boot.gif")
    for r in roms:
        mt("mcopy", "-D", "o", "-i", img, r, "::/GAMES/" + os.path.basename(r))
    listing = subprocess.run(["mdir", "-i", img, "-/", "::/"], env=ENV, capture_output=True, text=True, check=True).stdout
    return listing


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", required=True)
    ap.add_argument("--version", default="2.1")
    for k in ("os", "orig", "doom", "whd", "restore", "src", "boot-gif"):
        ap.add_argument("--" + k)
    ap.add_argument("--rom", action="append", default=[])
    ap.add_argument("--help-png", default=None)
    ap.add_argument("--readme", default=os.path.join(ROOT, "docs", "PUBLIC_README.md"))
    ap.add_argument("--name", default=None, help="package name (default SC26-OS-v<version>)")
    a = ap.parse_args()

    name = a.name or f"SC26-OS-v{a.version}"
    out = os.path.join(a.out, name)
    shutil.rmtree(out, ignore_errors=True)
    os.makedirs(os.path.join(out, "LICENSES"))
    work = os.path.join(a.out, "_work")
    os.makedirs(work, exist_ok=True)

    drive = os.path.join(work, "drive.img")
    print(build_drive(drive, a.boot_gif, a.rom))

    uf2 = [sys.executable, os.path.join(HERE, "make_combined_uf2.py")]
    full = uf2 + [os.path.join(out, "sc26_full_install.uf2"), "--os", a.os, "--fs", drive]
    for k in ("orig", "doom", "whd"):
        if getattr(a, k):
            full += ["--" + k, getattr(a, k)]
    subprocess.run(full, check=True)
    subprocess.run(uf2 + [os.path.join(out, "sc26_os_update.uf2"), "--os", a.os], check=True)
    if a.restore:
        shutil.copy(a.restore, os.path.join(out, "original_firmware_restore.uf2"))
    if a.src:
        shutil.copy(a.src, os.path.join(out, "sc26-src.tar.gz"))
    if a.help_png:
        shutil.copy(a.help_png, os.path.join(out, "help_pages.png"))

    readme = open(a.readme).read()
    readme = readme.replace("{VERSION}", a.version).replace("](docs/images/help_pages.png)", "](help_pages.png)")
    open(os.path.join(out, "README.md"), "w").write(readme)
    lic = {
        "SC26-OS_GPL-3.0.txt": "fw/lib/infones/LICENSE.pico-infones",
        "rp2040-doom_GPL-2.0.md": "p3/rp2040-doom/COPYING.md",
        "AnimatedGIF_Apache-2.0.txt": "fw/lib/AnimatedGIF/LICENSE",
        "pico-sdk_BSD-3-Clause.txt": "pico-sdk/LICENSE.TXT",
        "TinyUSB_MIT.txt": "pico-sdk/lib/tinyusb/LICENSE",
    }
    for dst, src in lic.items():
        shutil.copy(os.path.join(ROOT, src), os.path.join(out, "LICENSES", dst))
    shutil.copy(os.path.join(ROOT, "fw/lib/infones/LICENSE.pico-infones"), os.path.join(out, "LICENSE"))
    ff = open(os.path.join(ROOT, "fw/gifplayer/fatfs/ff.h")).read()
    ff = ff[:ff.index("*/") + 2]                       # the licence comment at the top of ff.h
    open(os.path.join(out, "LICENSES", "FatFs.txt"), "w").write(ff + "\n")

    z = os.path.join(a.out, name + ".zip")
    with zipfile.ZipFile(z, "w", zipfile.ZIP_DEFLATED) as zf:
        for dp, _, fns in os.walk(out):
            for fn in sorted(fns):
                p = os.path.join(dp, fn)
                zf.write(p, os.path.relpath(p, a.out))
    print(f"{z}: {os.path.getsize(z):,} bytes")


if __name__ == "__main__":
    main()
