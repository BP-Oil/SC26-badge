# SC26 OS v2.1: boot screen, games, DOOM, 2 PLAYER

## What's on the badge (16 MB flash)

| Offset | Size | Contents |
|---|---|---|
| `0x000000` | 1 MB | **SC26 OS**: boot screen, GIF player, menu, NES emulator. This is what boots at power-on. |
| `0x100000` | 1.5 MB | **Original "Wayfarer" firmware**, unchanged, started from the menu |
| `0x280000` | 508 KB | NES ROM cache (the game you pick is copied here and runs in place) |
| `0x2FF000` | 4 KB | Settings (last GIF, STAY, brightness, zoom, LED mode) |
| `0x300000` | 512 KB | **DOOM** engine (rp2040-doom, ported to this badge) |
| `0x380000` | 6 MB | DOOM game data (`DOOM.WAD` converted to WHD) + save games at the top |
| `0x980000` | 6.5 MB | **SC26 GIFS** USB drive: GIFs, `GAMES/` folder |

`SYSINFO.TXT` on the drive lists a CRC32 of each slot. It is written when the drive is freshly formatted, so you can check an install from the computer.

## Boot screen

At power-up:

1. **BIOS POST.** The LED ring flashes once like a PC self-test. The header reads **SC26 v2.1 by BP  (c) 2026**. Then live system info scrolls up: CPU clock, a memory count-up, flash size, the drive size, and how many GIFs and games were found.
2. **Splash** (~6 s). Your two astronauts drift in over a twinkling starfield: one tumbles in from the top-left, the other rises from below and gives a thumbs-up. Then the **SC26 OS** title fades in, with **v2.1 by BP** under it, above a blue marching-blocks loader bar, which is mirrored as a comet running around the LED ring. A shooting star crosses at about 3 s.
3. The GIF player starts, with a 5 s hint at the bottom: **HOLD CENTER: MENU/HELP**. **Any button skips** the BIOS or the splash.

The splash is drawn live every frame: the sprites are rotated and scaled with bilinear filtering and alpha-blended, not played back from a video. The artwork lives in `tools/make_boot_assets.py` → `fw/gifplayer/boot_assets.h` (~60 KB). To change it, rerun:

```
python3 tools/make_boot_assets.py char.svg Astro-3.svg fw/gifplayer/boot_assets.h boot_preview.gif
```

## Menu

**Hold CENTER for 1 s** in the GIF player to open it.

- **GIF PLAYER**
- **HOW TO USE**: on-screen help for every button (see below)
- **One entry per `.nes` file** in `GAMES/`
- **DOOM**
- **ORIGINAL FIRMWARE**
- **FIRMWARE UPDATE MODE**: the board becomes the RP2350 drive

Up/Down to move, A or Right to start, B to go back.

### HOW TO USE (on-screen help)

Nine pages, drawn to fit the round screen. **Left/Right** (or Up/Down) turn the pages and **A** goes to the next one. **B** or **CENTER** goes back to the menu.

1. **HOW TO USE**: the joystick and buttons, tap vs hold
2. **GIF PLAYER (joystick)**: Left/Right previous/next, Up/Down brightness, CENTER zoom, hold CENTER for the menu
3. **GIF PLAYER (A and B)**: A pause, hold A stay on this GIF, B LED mode, hold B swap LED sides, hold A+B update mode
4. **MENU**
5. **NES GAMES**
6. **DOOM**: moving, fire, use, menu, quit
7. **DOOM (hold CENTER and press)**: strafe, next weapon, map, run on/off
8. **ADD GIFS**: copying GIFs from a computer
9. **UPDATE**: firmware update steps

Button names sit in dark "chips". Amber **hold** means keep the button pressed. Source: `fw/gifplayer/help.c`. `tools/preview_help.sh <dir>` renders every page on a PC and checks it stays inside the circle.

**Fixed with this:** leaving the menu with B used to also change the LED mode when B was released (and leaving with A paused the GIF). The button that leaves the menu is now ignored until it is released. Also, the first CENTER press after leaving the menu no longer gets lost.

## NES games

**Adding games:** drop iNES `.nes` files (up to 504 KB) into **GAMES** on the SC26 GIFS drive. Only use ROMs you own. Games appear in the menu straight away. The first launch of a game copies it into flash (a progress bar shows); later launches start at once.

**Emulator:** InfoNES (GPL-3.0) in RAM. It supports 130+ mappers: 0–11, 13, 15–19, 21–26, 32–34, 40–51, 57, 58, 60–62, 64–80, 82, 83, 85–97, 100, 101, 105, 107–110, 112–119, 122, 133–135, 140, 151, 160, 180–183, 185, 187–189, 191, 193, 194, 200–202, 222, 225–236, 240–246, 248, 249, 251, 252, 255. That covers most US releases (NROM, MMC1, UxROM, CNROM, MMC3, MMC5, AxROM…). Contra (mapper 2) works. The picture is 256×232 at 1:1, centred.

| Button | NES |
|---|---|
| Joystick | D-pad |
| A / B | A / B |
| Tap CENTER | START |
| CENTER + B (or CENTER + DOWN) | SELECT |
| Hold CENTER 1.5 s | back to the menu |

**LEDs:** the ring follows the colours at the edge of the game picture (ambient) unless the LED mode is OFF. There is no sound, because the badge has no speaker.

## DOOM

**Data:** your `DOOM.WAD` (11 MB, registered v1.9) is converted to a 5.7 MB WHD file. Music and sound effects stay in it but aren't played, because there's no speaker. No other WAD is needed. To use a different IWAD, convert it with `whd_gen <wad> out.whd -no-super-tiny` and rebuild the install UF2.

| Button | In game | In menus |
|---|---|---|
| Up / Down | forward / back | move |
| Left / Right | turn | adjust / move |
| A | **fire** | select (Enter), "Y" in yes/no prompts |
| B | **use / open doors** | back, "N" in yes/no prompts |
| Tap CENTER | menu (Esc) | close menu |
| CENTER + Left / Right | strafe | — |
| CENTER + Up | next weapon (skips ones without ammo) | — |
| CENTER + Down | automap | — |
| CENTER + B | toggle always-run (on by default) | — |
| Hold CENTER alone 2 s | **quit to SC26 OS** | quit to SC26 OS |

- **Quit Game** in the DOOM menu also returns to SC26 OS, and so does any crash (automatic reboot).
- **Saves** go to flash at the top of the DOOM slot. There are up to 8 slots and they survive power-off and OS updates.
- **LEDs:** the ring takes the colours of the 3D view in each direction. It flashes red when you take damage, gold on pickups and green with the radiation suit. It respects the OS LED mode (OFF stays off), ring direction and screen brightness.
- **Display:** the 320×200 picture is shown 1:1, centred slightly high so the status bar stays inside the circle. The very ends of the status bar touch the edge of the round screen.
- **Under the hood:**
  - CPU at 270 MHz (1.30 V), as upstream rp2040-doom does on the RP2350, with flash at 90 MHz.
  - Core 1 streams finished frames to the panel at 60 Hz, line by line, alongside its rendering work.
  - Source changes are in `rp2040-doom-sc26.patch` (GPL-2.0).

## Release package

`tools/make_release.py` builds the downloadable `SC26-OS-v2.1.zip` (see the docstring for the full command). It contains:

- the full install, the OS update and the stock-firmware restore UF2s
- `README.md` (from `docs/PUBLIC_README.md`, or another file given with `--readme`)
- the help-page picture
- `LICENSES/`

The full install comes with a **pre-loaded SC26 GIFS drive**. In the public build that's `sc26_boot.gif` plus an empty GAMES folder. `--rom` adds `.nes` files to GAMES and `--whd` adds DOOM game data, for a private build of games you own.

The drive image starts from `tools/sc26_blank_drive.img`, which is SC26 OS's own fresh format (the first 36 KB; the rest is empty). `SYSINFO.TXT` is left out, so each badge writes its own install check on first boot. `make_combined_uf2.py --fs` writes only the boot sector, the FATs, the root directory and the clusters in use (~1.5 MB), so the UF2 stays small.

The public build has no DOOM game data, so SC26 OS only lists **DOOM** in the menu once a WHD file is found at `0x380000` (it starts with `IWHD`/`PWHD`). People add their own with `make_combined_uf2.py doom_data.uf2 --whd doom.whd` (see the README).

## Updating / recovery

| File | What it does |
|---|---|
| `sc26_os_update.uf2` | **OS only**: keeps GIFs, games, saves and settings. Use this for normal firmware updates. |
| `sc26_full_install.uf2` | **Everything**: OS + original firmware + DOOM + DOOM data. **Erases the SC26 GIFS drive** (DOOM saves survive). |
| `original_wayfarer_restore.uf2` | Puts the stock firmware back at the start of flash. |

To get the RP2350 drive, use any of these:

- Menu → **FIRMWARE UPDATE MODE**
- Hold **A + B** for 2 s
- Hold the BOOTSEL button while plugging in

Then drag the UF2 onto it.

## Benchmarks

| What | Numbers |
|---|---|
| GIF player | 360×360 full-frame GIFs ~20–25 fps (4.2–4.8 M instructions/frame at 150 MHz). Panel write 6.9 ms/frame at 75 MHz QSPI. |
| Boot splash | ~0.1 M instructions/frame for the starfield alone, ~1.8 M with both astronauts rotating (bilinear + alpha) → ~25–30 fps with tear-free (TE-synced) updates |
| NES (Contra) | see `NES_BENCH` below |
| DOOM | Runs on the badge (confirmed 2026-10-28); frame rate not measured yet. Panel output is ~3.8 ms of DMA per 60 Hz refresh, lines built on core 1. |

## DOOM port notes (bugs found on hardware)

1. **Float panic at startup.** This build has no float library, but the LED and LCD PIO setup used float clock dividers. Fixed with integer dividers.
2. **Hard assert installing the panel interrupt.** Fixed by writing the vector table entry directly.
3. **Freeze when a level starts.** The status-bar/HUD overlays write a few pixels past the 320-pixel line, as the VGA buffers allowed. Without slack, that overwrote the frame-tick state right after the line buffers. Fixed with 64 B / 256 B of slack around each line buffer.
4. **NVIC enables inherited from SC26 OS** (USB, DMA_IRQ_1, timer) survive the chain boot. They are cleared at startup.

**Freeze/crash reports:** DOOM leaves its status in the watchdog scratch registers, plus a register dump in RAM at 0x20078000. SC26 OS appends a line to `DOOMLOG.TXT` on the drive after the reboot. A core-0 probe reboots after 4 s without input polling, and grabs core 1's state via a doorbell NMI. The hardware watchdog (15 s) is the backstop.

## Rocket LED layout

The 9 LEDs follow the SAINTCON rocket outline, not a circle: LED7 is the nose; 6/8 sit beside the screen; 5/0 and 4/1 go down the body; 3/2 are at the fins (8, 0, 1, 2 on the LEFT, confirmed from a photo; that is the default). In ambient mode each LED samples the picture edge at a matching angle: nose -90 deg, then down the right side at -28, 0, 26, 53 deg, mirrored on the left. So the picture's colours run from the nose down to the fins. Hold B (1.5 s) to mirror the sides if they look swapped; the screen shows LED SIDES NORMAL or SWAPPED. DOOM uses the same mapping.

GIFs up to 1024 px wide are accepted (they are scaled to fit).
