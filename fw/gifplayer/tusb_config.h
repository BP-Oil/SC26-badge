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
