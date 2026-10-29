// SC26 board pin map, recovered from the original "Wayfarer" firmware (v0.1, pico-sdk 2.2.0)
// by boot emulation + disassembly. See docs/pin-map.md for evidence per pin.
#pragma once

// ---- Display: ST77916, 360x360, QSPI (1-1-4), driven by PIO -----------------
#define LCD_WIDTH        360
#define LCD_HEIGHT       360
#define PIN_LCD_SCK      10   // PIO side-set clock
#define PIN_LCD_D0       11   // QSPI data D0..D3 = GP11..GP14 (consecutive)
#define PIN_LCD_CS       15   // SIO, active low
#define PIN_LCD_RST       9   // SIO, active low reset
#define PIN_LCD_TE        8   // input from panel (tearing effect), pulled up
#define PIN_LCD_BL        7   // PWM slice 3 channel B, ~10 kHz, 0..100 %

// ---- Inputs (active low, internal pull-ups) -------------------------------
#define PIN_BTN_UP       23
#define PIN_BTN_DOWN     21
#define PIN_BTN_LEFT     22
#define PIN_BTN_RIGHT    19
#define PIN_BTN_CENTER   20   // joystick press (filled-circle icon in the original self-test)
#define PIN_BTN_B        16
#define PIN_BTN_A        17
#define PIN_OFFLINE_STRAP 29  // read once at boot by the original: low = "offline mode"

// ---- LEDs ------------------------------------------------------------------
#define PIN_WS2812       24   // 9x WS2812-class RGB LEDs, GRB order, 800 kHz (PIO0)
#define WS2812_COUNT      9
#define PIN_STATUS_LED   18   // single LED, original blinks it at 1 Hz via PIO0

// ---- Other buses -----------------------------------------------------------
#define PIN_UART0_TX      0   // "docking" serial link, 9600 8N1
#define PIN_UART0_RX      1
#define PIN_I2C0_SDA      4   // 400 kHz, 24Cxx-style EEPROM at 0x50 (holds game save/unlock data)
#define PIN_I2C0_SCL      5
#define EEPROM_I2C_ADDR   0x50
