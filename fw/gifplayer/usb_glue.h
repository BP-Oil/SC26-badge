#pragma once
#include <stdbool.h>

void usb_glue_init(void);          // TinyUSB + printf -> USB serial
bool usb_msc_ejected(void);        // host ejected the drive
bool usb_bootsel_requested(void);  // host opened the serial port at 1200 baud
void usb_reboot_to_bootsel(void);  // flush pending writes, enter RP2350 USB bootloader
void usb_console(bool on);         // printf -> USB serial on/off (off while the port is a game link)
