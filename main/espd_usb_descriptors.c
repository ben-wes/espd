/*
 * Custom composite USB descriptor: CDC (serial monitor) [+ MSC (flash drive)]
 * + MIDI (native Pd MIDI). Built by hand because esp_tinyusb's auto descriptor
 * generator does not handle the USB MIDI class. The CDC interface is always
 * present (serial monitoring in parallel with MIDI); MSC is included only when
 * CONFIG_ESPD_USE_USB_MSC is set.
 */

#include "espd_usb_descriptors.h"
#include "espd_config_file.h"

#if CONFIG_ESPD_USE_USB_OTG && CONFIG_ESPD_USE_USB_MIDI

#include "soc/soc_caps.h"
#include "tusb.h"

/* HS configuration descriptor is required on P4 (and other multi-port HS PHYs).
 * Use soc_caps so this file always builds HS config on those chips. */
#if (SOC_USB_OTG_PERIPH_NUM > 1)
#define ESPD_USB_HAS_HS_DESC 1
#elif defined(CONFIG_IDF_TARGET_ESP32S31)
#define ESPD_USB_HAS_HS_DESC 1
#else
#define ESPD_USB_HAS_HS_DESC 0
#endif

/* ─── MSC presence as a 0/1 token usable in arithmetic/static_assert ─── */
#if CONFIG_ESPD_USE_USB_MSC
#define ESPD_USB_HAS_MSC 1
#else
#define ESPD_USB_HAS_MSC 0
#endif

/* ─── Interface numbering: CDC (2 itf) [+ MSC] + MIDI (2 itf) ─── */
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

/* CDC+MIDI only (no MSC) — used when usb_midi_role=device so the host cannot
 * seize the flash LUN over SCSI while CDC dev sync is active. */
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

/* ─── Endpoint addresses (full speed). IN endpoints consume the scarce
 * ESP32-S3 IN-endpoint budget; keep them contiguous and assert the total. ─── */
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

/* IN endpoints used: CDC notif + CDC data + MIDI (+ MSC). ESP32-S3 FS budget
 * is 5 IN endpoints besides EP0; fail the build early if we overflow it. */
#define ESPD_USB_IN_EP_USED   (3 + ESPD_USB_HAS_MSC)
_Static_assert(ESPD_USB_IN_EP_USED <= 5,
    "USB composite (CDC+MSC+MIDI) exceeds the ESP32-S3 IN-endpoint budget; "
    "disable MSC (ESPD_USE_USB_MSC) or a class to free an IN endpoint.");

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
    (const char[]){0x09, 0x04}, /* 0: supported language English (0x0409) */
    "espd",                     /* 1: Manufacturer */
    "ESPD",                     /* 2: Product (host MIDI port name base) */
    "000000000001",             /* 3: Serial */
    "ESPD serial",              /* 4: CDC interface */
    "ESPD storage",             /* 5: MSC interface */
    "ESPD MIDI",                /* 6: MIDI interface (port name in Pd) */
};

/* ─── Device descriptor (composite → IAD/misc class, required by CDC) ─── */
static const tusb_desc_device_t s_device_desc = {
    .bLength            = sizeof(tusb_desc_device_t),
    .bDescriptorType    = TUSB_DESC_DEVICE,
    .bcdUSB             = 0x0200,
    .bDeviceClass       = TUSB_CLASS_MISC,
    .bDeviceSubClass    = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol    = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0    = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor           = 0x303A,   /* Espressif */
    .idProduct          = 0x4020,   /* espd composite w/ MIDI (unchanged on S3) */
    .bcdDevice          = 0x0100,
    .iManufacturer      = STRID_MANUFACTURER,
    .iProduct           = STRID_PRODUCT,
    .iSerialNumber      = STRID_SERIAL,
    .bNumConfigurations = 0x01,
};

/* ─── Endpoint max packet sizes (FS vs HS) ─── */
#define ESPD_USB_FS_EP_SIZE  64
#if ESPD_USB_HAS_HS_DESC
#define ESPD_USB_HS_EP_SIZE  512
#endif

/* ─── Configuration descriptor ─── */
#define ESPD_USB_CONFIG_TOTAL_LEN \
    (TUD_CONFIG_DESC_LEN + TUD_CDC_DESC_LEN \
     + (ESPD_USB_HAS_MSC ? TUD_MSC_DESC_LEN : 0) \
     + TUD_MIDI_DESC_LEN)

#define ESPD_USB_NOMSC_CONFIG_TOTAL_LEN \
    (TUD_CONFIG_DESC_LEN + TUD_CDC_DESC_LEN + TUD_MIDI_DESC_LEN)

static const uint8_t s_fs_config_msc[] = {
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, ESPD_USB_CONFIG_TOTAL_LEN, 0x00, 100),

    /* CDC: serial monitor (Pd print / ESP_LOG) */
    TUD_CDC_DESCRIPTOR(ITF_NUM_CDC, STRID_CDC, EPNUM_CDC_NOTIF, 8,
                       EPNUM_CDC_OUT, EPNUM_CDC_IN, ESPD_USB_FS_EP_SIZE),

#if ESPD_USB_HAS_MSC
    /* MSC: internal-flash USB drive */
    TUD_MSC_DESCRIPTOR(ITF_NUM_MSC, STRID_MSC, EPNUM_MSC_OUT, EPNUM_MSC_IN,
                       ESPD_USB_FS_EP_SIZE),
#endif

    /* MIDI: native Pd MIDI in/out */
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
/* P4 (and other HS ports): host negotiates high speed and uses this descriptor.
 * Without it, esp_tinyusb falls back to the auto-generated HS config (CDC+MSC
 * only — no MIDI class). */
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
    cfg->descriptor.device            = &s_device_desc;
    cfg->descriptor.qualifier         = NULL;
    cfg->descriptor.string            = s_str_desc;
    cfg->descriptor.string_count      = sizeof(s_str_desc) / sizeof(s_str_desc[0]);
#if ESPD_USB_HAS_MSC
    if (g_espd_cfg.usb_midi_mode == ESPD_USB_MIDI_DEVICE) {
        cfg->descriptor.full_speed_config = s_fs_config_nomsc;
#if ESPD_USB_HAS_HS_DESC
        cfg->descriptor.high_speed_config = s_hs_config_nomsc;
#endif
    } else
#endif
    {
        cfg->descriptor.full_speed_config = s_fs_config_msc;
#if ESPD_USB_HAS_HS_DESC
        cfg->descriptor.high_speed_config = s_hs_config_msc;
#endif
    }
}

#endif /* CONFIG_ESPD_USE_USB_OTG && CONFIG_ESPD_USE_USB_MIDI */
