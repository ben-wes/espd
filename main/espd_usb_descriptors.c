/*
 * Custom composite USB descriptor: CDC (serial monitor) [+ MSC (flash drive)]
 * + MIDI (native Pd MIDI). Built by hand because esp_tinyusb's auto descriptor
 * generator does not handle the USB MIDI class. The CDC interface is always
 * present (serial monitoring in parallel with MIDI); MSC is included only when
 * CONFIG_ESPD_USE_USB_MSC is set.
 */

#include "espd_usb_descriptors.h"
#include "espd_config_file.h"

#if CONFIG_ESPD_USE_USB_OTG \
    && (CONFIG_ESPD_USE_USB_MIDI || CONFIG_ESPD_DEV_CDC_SYNC)

#include "soc/soc_caps.h"
#include "tusb.h"

/* HS configuration descriptor is required on P4 (and other multi-port HS PHYs). */
#if (SOC_USB_OTG_PERIPH_NUM > 1)
#define ESPD_USB_HAS_HS_DESC 1
#elif defined(CONFIG_IDF_TARGET_ESP32S31)
#define ESPD_USB_HAS_HS_DESC 1
#else
#define ESPD_USB_HAS_HS_DESC 0
#endif

#if CONFIG_ESPD_USE_USB_MSC
#define ESPD_USB_HAS_MSC 1
#else
#define ESPD_USB_HAS_MSC 0
#endif

#define ESPD_USB_FS_EP_SIZE  64
#if ESPD_USB_HAS_HS_DESC
#define ESPD_USB_HS_EP_SIZE  512
#endif

/* ─── String descriptor table ─── */
enum {
    STRID_LANGID = 0,
    STRID_MANUFACTURER,
    STRID_PRODUCT,
    STRID_SERIAL,
    STRID_CDC,
    STRID_MSC,
    STRID_MIDI,
};

static const char *s_str_desc[] = {
    (const char[]){0x09, 0x04},
    "espd",
    "ESPD",
    "000000000001",
    "ESPD serial",
    "ESPD storage",
    "ESPD MIDI",
};

static const tusb_desc_device_t s_device_desc = {
    .bLength            = sizeof(tusb_desc_device_t),
    .bDescriptorType    = TUSB_DESC_DEVICE,
    .bcdUSB             = 0x0200,
    .bDeviceClass       = TUSB_CLASS_MISC,
    .bDeviceSubClass    = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol    = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0    = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor           = 0x303A,
    .idProduct          = 0x4020,
    .bcdDevice          = 0x0100,
    .iManufacturer      = STRID_MANUFACTURER,
    .iProduct           = STRID_PRODUCT,
    .iSerialNumber      = STRID_SERIAL,
    .bNumConfigurations = 0x01,
};

static void espd_usb_apply_descriptor_common(tinyusb_config_t *cfg)
{
    cfg->descriptor.device       = &s_device_desc;
    cfg->descriptor.qualifier    = NULL;
    cfg->descriptor.string       = s_str_desc;
    cfg->descriptor.string_count = sizeof(s_str_desc) / sizeof(s_str_desc[0]);
}

#if CONFIG_ESPD_DEV_CDC_SYNC
/* CDC serial only — dev sync without MSC (PUT over CDC, no USB drive). */
enum {
    ITF_CDC_ONLY = 0,
    ITF_CDC_ONLY_DATA,
    ITF_CDC_ONLY_TOTAL,
};

#define EPNUM_CDC_ONLY_NOTIF   0x81
#define EPNUM_CDC_ONLY_OUT     0x02
#define EPNUM_CDC_ONLY_IN      0x82

#define ESPD_USB_CDC_ONLY_CONFIG_TOTAL_LEN \
    (TUD_CONFIG_DESC_LEN + TUD_CDC_DESC_LEN)

static const uint8_t s_fs_config_cdc_only[] = {
    TUD_CONFIG_DESCRIPTOR(1, ITF_CDC_ONLY_TOTAL, 0,
        ESPD_USB_CDC_ONLY_CONFIG_TOTAL_LEN, 0x00, 100),

    TUD_CDC_DESCRIPTOR(ITF_CDC_ONLY, STRID_CDC, EPNUM_CDC_ONLY_NOTIF, 8,
                       EPNUM_CDC_ONLY_OUT, EPNUM_CDC_ONLY_IN, ESPD_USB_FS_EP_SIZE),
};

#if ESPD_USB_HAS_HS_DESC
static const uint8_t s_hs_config_cdc_only[] = {
    TUD_CONFIG_DESCRIPTOR(1, ITF_CDC_ONLY_TOTAL, 0,
        ESPD_USB_CDC_ONLY_CONFIG_TOTAL_LEN, 0x00, 100),

    TUD_CDC_DESCRIPTOR(ITF_CDC_ONLY, STRID_CDC, EPNUM_CDC_ONLY_NOTIF, 8,
                       EPNUM_CDC_ONLY_OUT, EPNUM_CDC_ONLY_IN, ESPD_USB_HS_EP_SIZE),
};
#endif

void espd_usb_apply_cdc_sync_descriptor(tinyusb_config_t *cfg)
{
    if (!cfg)
        return;
    espd_usb_apply_descriptor_common(cfg);
    cfg->descriptor.full_speed_config = s_fs_config_cdc_only;
#if ESPD_USB_HAS_HS_DESC
    cfg->descriptor.high_speed_config = s_hs_config_cdc_only;
#endif
}
#endif /* CONFIG_ESPD_DEV_CDC_SYNC */

#if CONFIG_ESPD_DEV_CDC_SYNC && CONFIG_ESPD_USE_USB_MSC
enum {
    ITF_CDC_MSC = 0,
    ITF_CDC_MSC_DATA,
    ITF_CDC_MSC_LUN,
    ITF_CDC_MSC_TOTAL,
};

#define EPNUM_CDC_MSC_NOTIF   0x81
#define EPNUM_CDC_MSC_OUT     0x02
#define EPNUM_CDC_MSC_IN      0x82
#define EPNUM_CDC_MSC_MSC_OUT 0x03
#define EPNUM_CDC_MSC_MSC_IN  0x83

#define ESPD_USB_CDC_MSC_CONFIG_TOTAL_LEN \
    (TUD_CONFIG_DESC_LEN + TUD_CDC_DESC_LEN + TUD_MSC_DESC_LEN)

static const uint8_t s_fs_config_cdc_msc[] = {
    TUD_CONFIG_DESCRIPTOR(1, ITF_CDC_MSC_TOTAL, 0,
        ESPD_USB_CDC_MSC_CONFIG_TOTAL_LEN, 0x00, 100),

    TUD_CDC_DESCRIPTOR(ITF_CDC_MSC, STRID_CDC, EPNUM_CDC_MSC_NOTIF, 8,
                       EPNUM_CDC_MSC_OUT, EPNUM_CDC_MSC_IN, ESPD_USB_FS_EP_SIZE),

    TUD_MSC_DESCRIPTOR(ITF_CDC_MSC_LUN, STRID_MSC, EPNUM_CDC_MSC_MSC_OUT,
                       EPNUM_CDC_MSC_MSC_IN, ESPD_USB_FS_EP_SIZE),
};

#if ESPD_USB_HAS_HS_DESC
static const uint8_t s_hs_config_cdc_msc[] = {
    TUD_CONFIG_DESCRIPTOR(1, ITF_CDC_MSC_TOTAL, 0,
        ESPD_USB_CDC_MSC_CONFIG_TOTAL_LEN, 0x00, 100),

    TUD_CDC_DESCRIPTOR(ITF_CDC_MSC, STRID_CDC, EPNUM_CDC_MSC_NOTIF, 8,
                       EPNUM_CDC_MSC_OUT, EPNUM_CDC_MSC_IN, ESPD_USB_HS_EP_SIZE),

    TUD_MSC_DESCRIPTOR(ITF_CDC_MSC_LUN, STRID_MSC, EPNUM_CDC_MSC_MSC_OUT,
                       EPNUM_CDC_MSC_MSC_IN, ESPD_USB_HS_EP_SIZE),
};
#endif

void espd_usb_apply_cdc_msc_descriptor(tinyusb_config_t *cfg)
{
    if (!cfg)
        return;
    espd_usb_apply_descriptor_common(cfg);
    cfg->descriptor.full_speed_config = s_fs_config_cdc_msc;
#if ESPD_USB_HAS_HS_DESC
    cfg->descriptor.high_speed_config = s_hs_config_cdc_msc;
#endif
}
#endif /* CONFIG_ESPD_DEV_CDC_SYNC && CONFIG_ESPD_USE_USB_MSC */

#if CONFIG_ESPD_USE_USB_MIDI
enum {
    ITF_NUM_CDC = 0,
    ITF_NUM_CDC_DATA,
#if ESPD_USB_HAS_MSC
    ITF_NUM_MSC,
#endif
    ITF_NUM_MIDI,
    ITF_NUM_MIDI_STREAMING,
    ITF_NUM_TOTAL,
};

enum {
    ITF_NOMSC_CDC = 0,
    ITF_NOMSC_CDC_DATA,
    ITF_NOMSC_MIDI,
    ITF_NOMSC_MIDI_STREAMING,
    ITF_NOMSC_TOTAL,
};

#define EPNUM_NOMSC_CDC_NOTIF   0x81
#define EPNUM_NOMSC_CDC_OUT     0x02
#define EPNUM_NOMSC_CDC_IN      0x82
#define EPNUM_NOMSC_MIDI_OUT    0x03
#define EPNUM_NOMSC_MIDI_IN     0x83

#define EPNUM_CDC_NOTIF   0x81
#define EPNUM_CDC_OUT     0x02
#define EPNUM_CDC_IN      0x82
#if ESPD_USB_HAS_MSC
#define EPNUM_MSC_OUT     0x03
#define EPNUM_MSC_IN      0x83
#define EPNUM_MIDI_OUT    0x04
#define EPNUM_MIDI_IN     0x84
#else
#define EPNUM_MIDI_OUT    0x03
#define EPNUM_MIDI_IN     0x83
#endif

#define ESPD_USB_IN_EP_USED   (3 + ESPD_USB_HAS_MSC)
_Static_assert(ESPD_USB_IN_EP_USED <= 5,
    "USB composite (CDC+MSC+MIDI) exceeds the ESP32-S3 IN-endpoint budget; "
    "disable MSC (ESPD_USE_USB_MSC) or a class to free an IN endpoint.");

#define ESPD_USB_CONFIG_TOTAL_LEN \
    (TUD_CONFIG_DESC_LEN + TUD_CDC_DESC_LEN \
     + (ESPD_USB_HAS_MSC ? TUD_MSC_DESC_LEN : 0) \
     + TUD_MIDI_DESC_LEN)

#define ESPD_USB_NOMSC_CONFIG_TOTAL_LEN \
    (TUD_CONFIG_DESC_LEN + TUD_CDC_DESC_LEN + TUD_MIDI_DESC_LEN)

static const uint8_t s_fs_config_msc[] = {
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, ESPD_USB_CONFIG_TOTAL_LEN, 0x00, 100),

    TUD_CDC_DESCRIPTOR(ITF_NUM_CDC, STRID_CDC, EPNUM_CDC_NOTIF, 8,
                       EPNUM_CDC_OUT, EPNUM_CDC_IN, ESPD_USB_FS_EP_SIZE),

#if ESPD_USB_HAS_MSC
    TUD_MSC_DESCRIPTOR(ITF_NUM_MSC, STRID_MSC, EPNUM_MSC_OUT, EPNUM_MSC_IN,
                       ESPD_USB_FS_EP_SIZE),
#endif

    TUD_MIDI_DESCRIPTOR(ITF_NUM_MIDI, STRID_MIDI, EPNUM_MIDI_OUT, EPNUM_MIDI_IN,
                        ESPD_USB_FS_EP_SIZE),
};

#if ESPD_USB_HAS_MSC
static const uint8_t s_fs_config_nomsc[] = {
    TUD_CONFIG_DESCRIPTOR(1, ITF_NOMSC_TOTAL, 0, ESPD_USB_NOMSC_CONFIG_TOTAL_LEN, 0x00, 100),

    TUD_CDC_DESCRIPTOR(ITF_NOMSC_CDC, STRID_CDC, EPNUM_NOMSC_CDC_NOTIF, 8,
                       EPNUM_NOMSC_CDC_OUT, EPNUM_NOMSC_CDC_IN, ESPD_USB_FS_EP_SIZE),

    TUD_MIDI_DESCRIPTOR(ITF_NOMSC_MIDI, STRID_MIDI, EPNUM_NOMSC_MIDI_OUT, EPNUM_NOMSC_MIDI_IN,
                        ESPD_USB_FS_EP_SIZE),
};
#endif

#if ESPD_USB_HAS_HS_DESC
static const uint8_t s_hs_config_msc[] = {
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, ESPD_USB_CONFIG_TOTAL_LEN, 0x00, 100),

    TUD_CDC_DESCRIPTOR(ITF_NUM_CDC, STRID_CDC, EPNUM_CDC_NOTIF, 8,
                       EPNUM_CDC_OUT, EPNUM_CDC_IN, ESPD_USB_HS_EP_SIZE),

#if ESPD_USB_HAS_MSC
    TUD_MSC_DESCRIPTOR(ITF_NUM_MSC, STRID_MSC, EPNUM_MSC_OUT, EPNUM_MSC_IN,
                       ESPD_USB_HS_EP_SIZE),
#endif

    TUD_MIDI_DESCRIPTOR(ITF_NUM_MIDI, STRID_MIDI, EPNUM_MIDI_OUT, EPNUM_MIDI_IN,
                        ESPD_USB_HS_EP_SIZE),
};

#if ESPD_USB_HAS_MSC
static const uint8_t s_hs_config_nomsc[] = {
    TUD_CONFIG_DESCRIPTOR(1, ITF_NOMSC_TOTAL, 0, ESPD_USB_NOMSC_CONFIG_TOTAL_LEN, 0x00, 100),

    TUD_CDC_DESCRIPTOR(ITF_NOMSC_CDC, STRID_CDC, EPNUM_NOMSC_CDC_NOTIF, 8,
                       EPNUM_NOMSC_CDC_OUT, EPNUM_NOMSC_CDC_IN, ESPD_USB_HS_EP_SIZE),

    TUD_MIDI_DESCRIPTOR(ITF_NOMSC_MIDI, STRID_MIDI, EPNUM_NOMSC_MIDI_OUT, EPNUM_NOMSC_MIDI_IN,
                        ESPD_USB_HS_EP_SIZE),
};
#endif
#endif /* ESPD_USB_HAS_HS_DESC */

void espd_usb_apply_midi_descriptor(tinyusb_config_t *cfg)
{
    if (!cfg)
        return;
    espd_usb_apply_descriptor_common(cfg);
#if ESPD_USB_HAS_MSC
    if (g_espd_cfg.usb_midi_mode == ESPD_USB_MIDI_DEVICE) {
        cfg->descriptor.full_speed_config = s_fs_config_nomsc;
#if ESPD_USB_HAS_HS_DESC
        cfg->descriptor.high_speed_config = s_hs_config_nomsc;
#endif
        return;
    }
#endif
    cfg->descriptor.full_speed_config = s_fs_config_msc;
#if ESPD_USB_HAS_HS_DESC
    cfg->descriptor.high_speed_config = s_hs_config_msc;
#endif
}
#endif /* CONFIG_ESPD_USE_USB_MIDI */

#endif /* CONFIG_ESPD_USE_USB_OTG && (MIDI || CDC_SYNC) */
