// USB descriptors: CDC-ACM (interfaces 0,1) + Mass Storage (interface 2)
#include "tusb.h"
#include "pico/unique_id.h"

#define USB_VID   0x1209   // pid.codes
#define USB_PID   0x0001   // pid.codes test PID (private / non-distributed use)
#define USB_BCD   0x0200

enum { ITF_NUM_CDC = 0, ITF_NUM_CDC_DATA, ITF_NUM_MSC, ITF_NUM_TOTAL };

#define EPNUM_CDC_NOTIF   0x81
#define EPNUM_CDC_OUT     0x02
#define EPNUM_CDC_IN      0x82
#define EPNUM_MSC_OUT     0x03
#define EPNUM_MSC_IN      0x83

#define CONFIG_TOTAL_LEN  (TUD_CONFIG_DESC_LEN + TUD_CDC_DESC_LEN + TUD_MSC_DESC_LEN)

static const tusb_desc_device_t desc_device = {
    .bLength = sizeof(tusb_desc_device_t),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = USB_BCD,
    .bDeviceClass = TUSB_CLASS_MISC,          // IAD composite
    .bDeviceSubClass = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor = USB_VID,
    .idProduct = USB_PID,
    .bcdDevice = 0x0100,
    .iManufacturer = 1,
    .iProduct = 2,
    .iSerialNumber = 3,
    .bNumConfigurations = 1,
};

const uint8_t *tud_descriptor_device_cb(void) { return (const uint8_t *)&desc_device; }

static const uint8_t desc_configuration[] = {
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, CONFIG_TOTAL_LEN, 0x00, 250),
    TUD_CDC_DESCRIPTOR(ITF_NUM_CDC, 4, EPNUM_CDC_NOTIF, 8, EPNUM_CDC_OUT, EPNUM_CDC_IN, 64),
    TUD_MSC_DESCRIPTOR(ITF_NUM_MSC, 5, EPNUM_MSC_OUT, EPNUM_MSC_IN, 64),
};

const uint8_t *tud_descriptor_configuration_cb(uint8_t index) {
    (void)index;
    return desc_configuration;
}

static const char *string_desc[] = {
    (const char[]){0x09, 0x04},   // 0: English
    "SC26",                       // 1: Manufacturer
    "SC26 GIF Player",            // 2: Product
    NULL,                         // 3: Serial (board unique ID)
    "SC26 Console",               // 4: CDC
    "SC26 GIFS",                  // 5: MSC
};

static uint16_t desc_str[33];

const uint16_t *tud_descriptor_string_cb(uint8_t index, uint16_t langid) {
    (void)langid;
    char serial[2 * PICO_UNIQUE_BOARD_ID_SIZE_BYTES + 1];
    const char *str;
    uint8_t n;
    if (index == 0) {
        desc_str[1] = 0x0409;
        n = 1;
    } else {
        if (index >= sizeof(string_desc) / sizeof(string_desc[0])) return NULL;
        if (index == 3) { pico_get_unique_board_id_string(serial, sizeof serial); str = serial; }
        else str = string_desc[index];
        n = (uint8_t)strlen(str);
        if (n > 32) n = 32;
        for (uint8_t i = 0; i < n; i++) desc_str[1 + i] = (uint8_t)str[i];
    }
    desc_str[0] = (uint16_t)((TUSB_DESC_STRING << 8) | (2 * n + 2));
    return desc_str;
}
