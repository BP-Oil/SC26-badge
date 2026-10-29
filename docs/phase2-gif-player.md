# SC26 GIF Player (Phase 2)

Firmware `sc26_gif.uf2`. It plays every `.gif` in the top level of the board's **SC26 GIFS** USB drive on the round 360×360 screen. The 9-LED ring either replays the original firmware's animation or follows the picture.

## Controls

| Button | Action |
|---|---|
| Left / Right | Previous / next GIF (it also auto-advances after ~20 s, at the end of a loop) |
| Up / Down | Screen brightness (10–100 %) |
| Center | Zoom: **Auto** → Fit circle → Fit square → Fill |
| A | Pause / resume (shows the current fps) |
| B (tap) | LED mode: **Original** → Ambient → Off |
| B (hold 1.5 s) | Mirror the LED ring direction (use this if ambient colors appear on the wrong side) |
| A + B (hold 2 s) | Reboot into BOOTSEL to update firmware, no unplugging needed |

Auto zoom shows square GIFs 1:1 and fits other shapes entirely inside the circle.

## Loading GIFs

1. Plug the board in. **SC26 GIFS** appears in Finder.
2. Drag `.gif` files onto it. The screen shows **RECEIVING** while files copy. About 1.5 s after the copy finishes, the newest GIF starts playing.
3. Eject the drive before unplugging.

GIFs live in their own 12 MB area of flash, so **firmware updates don't erase them**.

### Limits

| Item | Limit / notes |
|---|---|
| Resolution | Up to **480 px wide** (decoder limit). Best is **360×360** with the content centered, because the screen is round. |
| Files | 64 GIFs in the top-level folder. Subfolders and hidden files are ignored. |
| Space | About 12 MB in total. |
| Frame rate | About **20–25 fps** for full-screen 360×360 frames (4.2–4.8 M instructions per frame at 150 MHz, measured in the emulator). Smaller or partly static frames play faster. |
| Frame delay | 0–10 ms delays play at 100 ms, the same rule browsers use. |
| GIF features | Global and local palettes, transparency, interlacing, and disposal methods 1 and 2 are supported. Disposal 3 ("restore previous") is treated as 1. |
| macOS | `._*` resource-fork files are ignored. Files you delete go to `.Trashes` on the drive until you empty the Trash. |
| Metadata | Large metadata blocks (C2PA, XMP, long comments) are handled correctly (see the AnimatedGIF patch below). |

### gifprep.py: fit any GIF to the round screen

```bash
pip3 install --user pillow
python3 gifprep.py downloaded.gif                 # -> downloaded_sc26.gif, whole picture inside the circle
python3 gifprep.py downloaded.gif --mode fill     # fill the circle, crop the edges
python3 gifprep.py big.gif --max-fps 15 --colors 64   # shrink a large GIF
```

It resizes with high-quality resampling onto a 360×360 canvas, merges frames faster than `--max-fps`, and re-quantizes the colors. The output plays 1:1 in Auto zoom, so it decodes fastest.

## LEDs

- **Original:** an exact replica of the stock firmware's behavior, decoded from its code at `0x10010670`.
  - A 5-step sweep down the ring with masks `0x080, 0x140, 0x021, 0x012, 0x00C`, one step every 170 ms, in color (16, 0, 16).
  - The status LED on GP18 blinks at 1 Hz.
  - The original sped the sweep up by 5 ms per unlocked game achievement; there are no achievements here.
- **Ambient:** each LED is mapped to its angle around the ring. LED 7 is at the top, as the original sweep shows.
  - For each LED, the player samples 9 points near the edge of the picture in that direction.
  - The average is saturation-boosted, gamma-corrected and smoothed at 50 Hz.
  - Brightness is 45 %.

## Updating firmware

Use any one of these, then drop a `.uf2` file onto the **RP2350** drive:

- Hold **A + B** for 2 s.
- Open the serial port at 1200 baud: `stty -f /dev/cu.usbmodem* 1200`.
- Hold BOOT while plugging the board in.

The USB serial console (`cat /dev/cu.usbmodem*`) logs each file played, its size, and whether it's memory-mapped or streamed.

## Building

```bash
# toolchain: arm-none-eabi-gcc 13+, cmake, ninja, pico-sdk 2.x, picotool
export PICO_SDK_PATH=/path/to/pico-sdk
cmake -S fw -B fw/build -G Ninja
ninja -C fw/build sc26_gif        # -> fw/build/sc26_gif.uf2   (sc26_probe for the Phase 1 test app)
```

On macOS: `brew install cmake ninja picotool` plus `brew install --cask gcc-arm-embedded`. Then clone pico-sdk with `--recurse-submodules`.

## How it works

| Part | Implementation |
|---|---|
| Display driver | Custom, not LVGL: `common/lcd.c`. PIO1 runs a 2-instruction QSPI program (`common/qspi_lcd.pio`) at 75 MHz SCK, the same as the original. DMA streams the RGB565 framebuffer, and it waits for TE (GP8) before each frame to reduce tearing. The init table is the original firmware's 195 commands, extracted verbatim by emulation. |
| Framebuffer | 360×360×16-bit = 253 KB of SRAM. Only rows that changed are sent to the screen. |
| Decoder | AnimatedGIF (Apache-2.0) in RAW mode, one line per callback. `gif_player.c` scales lines into the framebuffer using a nearest-neighbor lookup table, with a 1:1 fast path. |
| Memory-mapped playback | If FatFs reports a file as contiguous (the usual case on this drive), the GIF is decoded straight from XIP flash with no copying. Otherwise it streams through FatFs. |
| USB | TinyUSB composite: CDC serial console + MSC drive. VID:PID 1209:0001 (pid.codes test ID). |
| Storage | `common/flash_disk.c`: a 512-byte block device at flash offset 4 MB (12 MB). A 4 KB write-back sector cache is flushed when a different sector is touched, after 300 ms idle, on SCSI SYNC, or on eject. FatFs R0.16 formats it as FAT12 (label "SC26 GIFS") on first boot. |
| RAM use | About 316 KB static, out of 520 KB. |

### Patches to AnimatedGIF (marked `SC26 patch` in `gif.inl`)

1. The NEON include was keyed on `__ARM_ARCH >= 7`. It now checks `__ARM_NEON`, since Cortex-M33 has no NEON.
2. **Large extension blocks:** after skipping a block bigger than the 4 KB read buffer, the parser could read the next headers from stale buffer data. `GIFRefill()` now tops the buffer up at each block boundary. Before the fix, GIFs carrying a 5.8 KB C2PA manifest lost their first frame and ended with `GIF_EARLY_EOF`. After it, they decode identically to clean copies.

The application code also sets `pLineBufAligned` after `GIF_begin()`. The C API never initializes it, so without this the decoder writes every line to address 0, which is the read-only boot ROM on the RP2350.

## Known limitations

- There's no double buffering (not enough RAM), so very fast full-screen motion can show slight tearing.
- Playback pauses briefly whenever the computer writes to the drive. macOS also writes housekeeping files just after mounting.
- Copying is limited by flash erase speed, roughly 100 KB/s, so a 1 MB GIF takes about 10 s.
