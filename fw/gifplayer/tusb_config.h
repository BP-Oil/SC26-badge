// TinyUSB configuration: composite CDC (log console + 1200-baud BOOTSEL trigger) + MSC (GIF drive)
#pragma once

#ifndef CFG_TUSB_MCU
#error CFG_TUSB_MCU must be defined by the SDK
#endif

#define CFG_TUSB_RHPORT0_MODE   (OPT_MODE_DEVICE | OPT_MODE_FULL_SPEED)
#define CFG_TUSB_OS             OPT_OS_PICO
#define CFG_TUSB_MEM_ALIGN      __attribute__((aligned(4)))

#define CFG_TUD_ENDPOINT0_SIZE  64

#define CFG_TUD_CDC             1
#define CFG_TUD_MSC             1
#define CFG_TUD_HID             0
#define CFG_TUD_MIDI            0
#define CFG_TUD_VENDOR          0

#define CFG_TUD_CDC_RX_BUFSIZE  256
#define CFG_TUD_CDC_TX_BUFSIZE  1024
#define CFG_TUD_CDC_EP_BUFSIZE  64

// One flash sector per MSC transfer chunk -> fewer, larger write callbacks
#define CFG_TUD_MSC_EP_BUFSIZE  4096

// Host role, used only by the USB LINK "PLAYER 1" mode (port 0 is restarted as a host; the
// other badge shows up as a USB serial device). The normal boot still starts as a device.
#define CFG_TUH_ENABLED                   1
#define CFG_TUH_MAX_SPEED                 OPT_MODE_FULL_SPEED
#define CFG_TUH_DEVICE_MAX                1
#define CFG_TUH_HUB                       0
#define CFG_TUH_CDC                       1
#define CFG_TUH_ENUMERATION_BUFSIZE       256
#define CFG_TUH_CDC_LINE_CONTROL_ON_ENUM  0x03   // DTR | RTS. Must be a plain number: TinyUSB tests it with #if
#define CFG_TUH_CDC_RX_BUFSIZE            256
#define CFG_TUH_CDC_TX_BUFSIZE            256
