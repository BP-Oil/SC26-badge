// TinyUSB callbacks: mass storage backed by flash_disk, CDC as a printf console,
// and "1200-baud touch" -> reboot into the RP2350 BOOTSEL bootloader.
#include <string.h>
#include "tusb.h"
#include "pico/stdlib.h"
#include "pico/stdio/driver.h"
#include "pico/bootrom.h"
#include "flash_disk.h"
#include "usb_glue.h"

static volatile bool ejected;
static volatile bool want_bootsel;

// ---------------------------------------------------------------- MSC
void tud_msc_inquiry_cb(uint8_t lun, uint8_t vendor_id[8], uint8_t product_id[16], uint8_t product_rev[4]) {
    (void)lun;
    memcpy(vendor_id, "SC26    ", 8);
    memcpy(product_id, "GIF Storage     ", 16);
    memcpy(product_rev, "1.0 ", 4);
}

bool tud_msc_test_unit_ready_cb(uint8_t lun) {
    (void)lun;
    if (ejected) { tud_msc_set_sense(lun, SCSI_SENSE_NOT_READY, 0x3A, 0x00); return false; }
    return true;
}

void tud_msc_capacity_cb(uint8_t lun, uint32_t *block_count, uint16_t *block_size) {
    (void)lun;
    *block_count = DISK_BLOCK_COUNT;
    *block_size = DISK_BLOCK_SIZE;
}

bool tud_msc_start_stop_cb(uint8_t lun, uint8_t power_condition, bool start, bool load_eject) {
    (void)lun; (void)power_condition;
    if (load_eject) {
        flashdisk_flush();
        ejected = !start;
    }
    return true;
}

int32_t tud_msc_read10_cb(uint8_t lun, uint32_t lba, uint32_t offset, void *buffer, uint32_t bufsize) {
    (void)lun;
    if (offset % DISK_BLOCK_SIZE || bufsize % DISK_BLOCK_SIZE) return -1;
    flashdisk_read(lba + offset / DISK_BLOCK_SIZE, buffer, bufsize / DISK_BLOCK_SIZE);
    return (int32_t)bufsize;
}

int32_t tud_msc_write10_cb(uint8_t lun, uint32_t lba, uint32_t offset, uint8_t *buffer, uint32_t bufsize) {
    (void)lun;
    if (offset % DISK_BLOCK_SIZE || bufsize % DISK_BLOCK_SIZE) return -1;
    flashdisk_write(lba + offset / DISK_BLOCK_SIZE, buffer, bufsize / DISK_BLOCK_SIZE);
    return (int32_t)bufsize;
}

bool tud_msc_is_writable_cb(uint8_t lun) { (void)lun; return true; }

int32_t tud_msc_scsi_cb(uint8_t lun, uint8_t const scsi_cmd[16], void *buffer, uint16_t bufsize) {
    (void)buffer; (void)bufsize;
    switch (scsi_cmd[0]) {
        case 0x35:                       // SYNCHRONIZE CACHE (10)
            flashdisk_flush();
            return 0;
        default:
            tud_msc_set_sense(lun, SCSI_SENSE_ILLEGAL_REQUEST, 0x20, 0x00);
            return -1;
    }
}

bool usb_msc_ejected(void) { return ejected; }

// ---------------------------------------------------------------- CDC console
void tud_cdc_line_coding_cb(uint8_t itf, cdc_line_coding_t const *coding) {
    (void)itf;
    if (coding->bit_rate == 1200) want_bootsel = true;   // Arduino-style "1200 baud touch"
}

bool usb_bootsel_requested(void) { return want_bootsel; }

static void cdc_out_chars(const char *buf, int len) {
    if (!tud_cdc_connected()) return;
    absolute_time_t until = make_timeout_time_ms(20);
    while (len > 0 && !time_reached(until)) {
        uint32_t n = tud_cdc_write_available();
        if (n == 0) { tud_task(); continue; }
        if (n > (uint32_t)len) n = (uint32_t)len;
        tud_cdc_write(buf, n);
        buf += n; len -= (int)n;
    }
    tud_cdc_write_flush();
}

static stdio_driver_t cdc_stdio = {
    .out_chars = cdc_out_chars,
#if PICO_STDIO_ENABLE_CRLF_SUPPORT
    .crlf_enabled = true,
#endif
};

void usb_glue_init(void) {
    tusb_init();
    // After a UF2 flash the boot ROM restarts us so quickly that macOS may not notice the
    // device ever left, and never mounts the drive again. Drop off the bus for a moment so
    // the computer sees a clean unplug / plug.
    tud_disconnect();
    sleep_ms(300);
    tud_connect();
    stdio_set_driver_enabled(&cdc_stdio, true);
}

void usb_console(bool on) { stdio_set_driver_enabled(&cdc_stdio, on); }

void usb_reboot_to_bootsel(void) {
    flashdisk_flush();
    reset_usb_boot(0, 0);
}
