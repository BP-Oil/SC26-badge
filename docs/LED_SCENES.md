# LED Scenes for SC26 OS

Adds a **SCENES** LED mode to SC26 OS: 36 built-in animations for the badge's 9 ring LEDs, plus
your own scenes written as text files on the badge's USB drive (no rebuild needed). Scenes only
change the ring LEDs; the minibadge clock keeps the stock 1 Hz blink in every mode.

## Installing

1. Put the badge in update mode: hold **A + B** for 2 s (or Menu → FIRMWARE UPDATE MODE).
2. Drag `sc26_os_update_led_scenes.uf2` onto the **RP2350** drive.

This is an OS-only update. Your GIFs, games, DOOM, saves and settings stay put. On first boot it
creates a `LEDS` folder on the **SC26 GIFS** drive with two example scenes.

## Using it

- **B (tap)** in the GIF player now cycles: original → ambient → **scenes** → off.
- **Menu → LED SCENES** opens the scene page. The ring runs the scene live, and the screen shows
  the same nine LEDs laid out like the rocket.

| Button | LED SCENES page |
|---|---|
| UP / DOWN | choose a row: SCENE, SPEED, BRIGHT |
| LEFT / RIGHT | change it |
| A or CENTER | save and go back to the GIFs |
| B | save and go back to the menu |

Leaving the page either way saves your choice to flash immediately and switches the LEDs to
SCENES mode, so it survives a power-off. Custom scenes are marked with `*`.

Scenes are drawn by their own 50 Hz timer, so they stay smooth even while a large GIF is
decoding.

## Games: the same LED mode everywhere

Your LED mode (original, ambient, scenes or off) now carries into the games:

- **NES games** keep your mode; SC26 OS no longer switches to ambient for them.
- **DOOM** needs `doom_engine_led_modes.uf2` installed once (drag it onto the RP2350 drive in
  update mode; it only replaces the DOOM engine, not your WAD data or saves). It shows the
  original sweep, ambient, off, or your scene. DOOM has no floating point, so when you start
  it SC26 OS records about 8.5 s of the running scene (a file scene: exactly one cycle) as a
  seamless loop, and DOOM plays that back.
- The **original badge firmware** (Menu → ORIGINAL FIRMWARE) is the SAINTCON team's own
  program and keeps its own LED behaviour.

## Built-in scenes

**Space:** RAINBOW, COMET, THRUSTERS (engine fire at the fins, nav beacon on the nose), FIRE,
PLASMA, BREATHE, SCANNER, HYPERSPACE, STARFIELD, RED ALERT, HEARTBEAT, AURORA, CREW SWEEP (the
stock badge's sweep, cycling the five crew colours), ION STORM, PULSAR, SOLAR FLARE, ORBIT,
DOCKING LIGHTS (red/green nav lights, white nose strobe, red fin beacon).

**Space horror:**

- **MOTION TRACKER**: a green ping sweeps the hull and the contact blip gets a row closer each
  time, faster and faster, until it reaches the nose and everything strobes red.
- **HULL BREACH**: spinning amber warning beacons, a red klaxon pulse, lights dropping out.
- **DERELICT**: the last cold panel lights on a dead ship, humming, sputtering, some gone dark.
- **POSSESSED**: a slow red breathing, then without warning the ring convulses in purple and green.
- **PUMPKIN NEBULA**: swirling gas in Halloween orange and purple.
- **GHOST SHIP**: pale green wisps drifting up the hull and fading at the nose.
- **BLOOD MOON**: a deep red glow with an eclipse shadow creeping across it.
- **INFECTION**: something green spreads up from the engines LED by LED, then a white purge flash.
- **SHADOW EYES**: darkness, then a pair of red or yellow eyes opens on the hull, blinks, and is gone.

**Slow Halloween ambience** (calm, always moving, good for leaving on):

- **CANDLELIGHT**: every LED its own candle, swaying and shimmering, now and then guttering low.
- **JACK-O-LANTERN**: a candlelit face: green stem on the nose, eyes that slowly squint, glowing
  cheeks and a jagged mouth at the fins.
- **LURKING EYES**: in a dim violet dark, pairs of amber, green or violet eyes slowly open, glance
  about, blink lazily and sink back.
- **CAULDRON**: a purple glow at the base with green bubbles rising up the sides and popping.
- **GRAVEYARD MIST**: cold violet fog rolling slowly, with a pale green spirit drifting through.
- **EMBERS**: deep red coals breathing slowly; now and then one flares up.
- **WILL-O-WISP**: one soft green-white light wandering the hull, fading in and out.
- **CANDY CORN**: yellow, orange and white bands rolling slowly down the rocket.
- **SPIDER DROP**: a spider lowers itself from the nose on a silk thread, sways, and climbs back up.

## Writing your own scenes (no compiling)

Make a `.TXT` file in the `LEDS` folder of the SC26 GIFS drive. The file name becomes the scene
name (`WARP_CORE.TXT` → `WARP CORE`). After the copy finishes, open LED SCENES and pick it. If
the scene you're editing is the one running, saving the file reloads it automatically.

```
# comment
step 150        # default time per frame, ms (applies to frames after it)
fade 80         # cross-fade into each frame, ms (0 = hard cut)

# one frame per line: colours, then optional extras
FF0000 - - - -          # 5 colours: nose + right side top->fin, mirrored to the left
#00FFFF @300            # 1 colour: all LEDs. @300 = 300 ms for this frame
FF0000 FF8000 FFFF00 00FF00 00FFFF 0000FF 8000FF FF00FF FFFFFF   # 9 colours: every LED
```

**Colours:** `RRGGBB`, `#RRGGBB`, short `RGB` / `#RGB`, or `-` for off.

**LED order:**

- 9 colours run clockwise from the nose: nose, right side top → right fin, left fin → left side top.
- 5 colours are nose + right side (top → fin), mirrored onto the left.
- 1 colour fills every LED.

"Right" means as you look at the badge. If your scene comes out mirrored, hold **B** in the GIF
player to swap LED sides. That setting applies to scenes too.

**Limits:** 256 frames per file, 24 scene files, lines up to 199 characters. The scene speed
setting scales all timings. Brightness and gamma are applied for you, so pick colours as they'd
look on a screen.

If a file has a mistake, the LED SCENES page shows the line, e.g. `LINE 7: 4 COLOURS`, and keeps
the previous scene running.

`example_scenes/HAUNTED.TXT` is the same Haunted example, if you ever need a fresh copy.

## Adding a built-in scene in C

Built-in scenes live in `fw/gifplayer/scenes.c`. Each is one function:

```c
static void sc_myscene(float t, float dt, float c[NLED][3]) {
    for (int k = 0; k < NLED; k++)              // k = ring position, 0 = nose, clockwise
        hsv(c[k], t * 0.1f + py[k] * 0.3f, 1, 1);   // colour 0..255 per channel
}
```

Then add `{ "MY SCENE", sc_myscene }` to the `builtins[]` table.

- `t` is scene time in seconds, already scaled by the speed setting. `dt` is the time step.
- `px[k]` and `py[k]` give each LED's position: x is -1 (left) to 1 (right), y is -1 (nose) to 0.85 (fins).
- `row[k]` is 0 at the nose through 4 at the fins.
- Helpers: `hsv()`, `heat()` (black → red → yellow → white), `noise1()` (smooth noise), `frand()`.

Build exactly as in the main README (`make sc26_gif`), then package the update with:

```
python3 tools/make_combined_uf2.py sc26_os_update.uf2 --os fw/build/sc26_gif.bin
```

## What changed in the source

- `fw/gifplayer/scenes.c/.h`: new. Scene engine, rocket geometry, built-ins, file loader, and
  the loop recorder used for DOOM.
- `fw/gifplayer/ambient.c/.h`: the `LEDMODE_SCENE` mode (added after OFF, so saved settings
  keep their meaning), drawn by a 50 Hz timer.
- `fw/gifplayer/main.c`: the LED SCENES menu page and preview, the B-button order, boot-time
  restore, a rescan after files are copied, NES games keeping the LED mode, and recording the
  scene loop before DOOM starts.
- `fw/gifplayer/settings.c/.h`: BP's original settings record is kept exactly as it was (DOOM
  reads it), and a separate scenes record follows it in the same sector. Settings from BP's
  version and from earlier builds of this mod are read, not reset.
- `fw/gifplayer/help.c`: one help line updated.
- `p3/rp2040-doom/src/pico/sc26_doom.c` and `docs/rp2040-doom-sc26.patch`: DOOM's LEDs follow
  all four modes; the scene loop is played back with integer maths only.

To build the DOOM engine, follow BP's README (rp2040-doom at `29a453c` + the patch + the
`p3/rp2040-doom/src/pico/sc26_doom.[ch]` files), then:

```
python3 tools/make_combined_uf2.py doom_engine.uf2 --doom path/to/doom_tiny_nost.bin
```
