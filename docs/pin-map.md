# SC26 RP2350 board: pin map

Recovered from the original firmware ("Wayfarer" v0.1, pico-sdk 2.2.0, built Aug 16 2026). The firmware was dumped over BOOTSEL (16 MB flash, 1.5 MB used, sha256 `b14bf0a2…cb22f`). The evidence comes from Unicorn boot emulation, which logged every register write, plus targeted disassembly.

| GPIO | Function | Details | Evidence |
|---|---|---|---|
| GP0 | UART0 TX | "Docking" serial link, 9600 8N1 | UART0 IBRD/FBRD = 976/36; `$STATION`/`$GAME` protocol strings |
| GP1 | UART0 RX | ″ | ″ |
| GP4 | I2C0 SDA | 400 kHz, pull-up, EEPROM @ 0x50 | I2C0 target 0x50; "EEPROM FAILED!" strings |
| GP5 | I2C0 SCL | ″ | ″ |
| GP7 | LCD backlight | PWM slice 3B, 10 kHz, level = brightness % × 10 | PWM CC written from a 0–100 value |
| GP8 | LCD TE (input) | Pull-up, set in the display init | Configured as input inside the LCD init routine |
| GP9 | LCD RESET | Active low | Toggled 1→0→1 right before the init sequence |
| GP10 | LCD QSPI SCK | PIO1 side-set | PIO1 PINCTRL: side-set base 10 |
| GP11–GP14 | LCD QSPI D0–D3 | PIO1 `out pins, 4` | PIO1 PINCTRL: OUT base 11, count 4 |
| GP15 | LCD CS | Active low | Toggled around each of the 195 init transactions |
| GP16 | Button **B** | Active low, pull-up | Button table + Konami-code detector + self-test icon |
| GP17 | Button **A** | ″ | ″ ("HOLD A TO RESET EEPROM" checks this bit) |
| GP18 | Status LED | Original blinks it at 1 Hz via PIO0 SM0 | pico-examples `blink.pio` + "Blinking pin %d at %d Hz" |
| GP19 | Joystick **Right** | Active low, pull-up | Button table / Konami / icon |
| GP20 | Joystick **Center** (press) | ″ | Button table / filled-circle icon |
| GP21 | Joystick **Down** | ″ | ″ |
| GP22 | Joystick **Left** | ″ | ″ |
| GP23 | Joystick **Up** | ″ | ″ |
| GP24 | WS2812 RGB LEDs ×9 | PIO0 SM1, 800 kHz, GRB | ws2812 PIO program, 27-byte (9×3) LED buffer |
| GP29 | "Offline mode" strap | Pull-up, read once at boot (low = offline) | `main()` reads bit 29 → "Offline mode enabled" |

The 16-pin ribbon connector carries the display signals: SCK, D0–D3, CS, RST, TE, backlight, plus power and ground.

## Display

- **Controller:** Sitronix **ST77916**. Identified from its vendor init sequence (`F0 28`, `F2 28`, `73 F0`, `7C D1`…).
- **Resolution:** 360×360 (`CASET`/`RASET` 0–359). 16-bit RGB565 (`3A 55`), inversion on (`21`). Often a round 1.8″ panel. **To confirm:** round or square?
- **Bus:** QSPI 1-1-4 via a 2-instruction PIO program.
  - Register write: `02 00 <cmd> 00` + params, all on 1 line.
  - Pixel write: `32 00 2C 00` on 1 line, then RGB565 big-endian on 4 lines, sent by DMA.
- **Speed:** The original ran the PIO at sysclk/1 (75 MHz SCK). The new firmware uses /2 (37.5 MHz), which is about 14 ms per full frame.
- **Rotation (MADCTL `36`):** 0° = `00`, 90° = `60`, 180° = `C0`, 270° = `A0`.

## LEDs

- **9× WS2812-class RGB** on GP24 (24-bit GRB, T1/T2/T3 = 3/3/4 cycles at 8 MHz).
- **1× single-color status LED** on GP18.

## Inputs

There are 7 buttons: a 5-way joystick (up/down/left/right/center) plus A and B. All are active low with internal pull-ups. The original polls them every 10 ms through a repeating timer.

## Other

- 16 MB QSPI flash.
- USB CDC serial: this is the "COM5" port on Windows (`/dev/cu.usbmodem*` on macOS).
- Pressing **B** on the original's boot screen appears to open its self-test (it shows button states).
- Holding **A** on that self-test screen resets the EEPROM data. **Don't do that** unless you mean to.

## Hardware notes for later phases

1. **RAM:** A full 360×360×16-bit framebuffer is 253 KB, about half of the RP2350's 520 KB SRAM. GIF playback and Doom will need to stream rows, use 8-bit palettized buffers, or render at a lower resolution and scale up.
2. **Round panel (if confirmed):** Corners are invisible. Doom's 320×200 frame should be scaled to about 288×180 to fit inside the circle.
3. **Buttons for NES:** It needs Up/Down/Left/Right/A/B/Start/Select (8) and the board has 7. Plan: Center = Start, and Center + B = Select (or a long press).
4. **Buttons for Doom:** It needs fire/use/strafe/run/weapon/map/menu, so it'll use combos (for example A = fire, B = use, Center = menu, B + direction = strafe).
5. **EEPROM (0x50):** It holds the original game's save/unlock state. New firmware leaves it untouched.
