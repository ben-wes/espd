#ifndef ESPD_H
#define ESPD_H

#include "espd_config.h"
#include <stdbool.h>

/* task priorities */
#define  PRIORITY_WIFI 2

    /* if WIFI is enabled we need to define the WIFI name and password,
    the peer machine, and the send and receive ports.  This can be done
    using main/locale.h (copy from locale.h.example) or by defining
    CONFIG_ESP_WIFI_SSID, etc., in sdkconfig / compile flags. */
#if defined(ESPD_USE_WIFI)
#include "sdkconfig.h"
#endif
#if defined(CONFIG_LOCALE_FILE)
#include CONFIG_LOCALE_FILE
#else
#if defined(ESPD_USE_WIFI) && !defined(CONFIG_ESP_WIFI_SSID)
#include "locale.h"
#endif
#endif
#if defined(ESPD_USE_WIFI)
#ifndef CONFIG_ESP_WIFI_SSID
#define CONFIG_ESP_WIFI_SSID "espd"
#endif
#ifndef CONFIG_ESP_WIFI_PASSWORD
#define CONFIG_ESP_WIFI_PASSWORD ""
#endif
#ifndef CONFIG_ESP_WIFI_SENDPORT
#define CONFIG_ESP_WIFI_SENDPORT 4498
#endif
#ifndef CONFIG_ESP_WIFI_LISTENPORT
#define CONFIG_ESP_WIFI_LISTENPORT 4498
#endif
#ifndef CONFIG_ESP_WIFI_SENDADDR
#define CONFIG_ESP_WIFI_SENDADDR "192.168.4.255"
#endif
#endif

#include <sys/types.h>
void pdmain_print(const char *s);
void pd_sendmsg(char *buf, int bufsize);
void pd_fromhost(char *data, size_t size);
void espd_control_io_init(void);

#if CONFIG_ESPD_USE_USB_OTG && CONFIG_ESPD_DEV_CDC_SYNC
/** Serialized TinyUSB CDC TX (protocol replies + esp_log). */
void espd_usb_cdc_write(const void *data, size_t len);
#endif

#ifdef ESPD_USE_WIFI
#include "freertos/FreeRTOS.h"
void espd_netif_ensure_init(void); /* wifi.c - idempotent lwIP/event-loop init */
void wifi_prepare_phy(void);       /* wifi.c - esp_wifi_init only (before USB OTG) */
void wifi_start_sta(void);         /* wifi.c - set STA config + start (after config.txt) */
bool wifi_wait_sta(TickType_t ticks); /* pdMS_TO_TICKS(ms); 0 = poll once */
void net_init( void);   /* init */
void net_hello( void);  /* send initial TCP packet when connected */
void net_alive( void);  /* send keep-alive packet if needed */
void net_sendudp(void *msg, int len, int port); /* send whatev */
void net_sendtcp(void *msg, int len);
extern char wifi_ipaddr[];
extern char espd_wifi_ssid[33];
extern char espd_wifi_password[65];
#endif

#ifndef PIN_BIT_CLOCK
#define PIN_BIT_CLOCK 13
#define PIN_WORD_SELECT 33
#define PIN_DATA_OUT 32
#define PIN_DATA_IN 35
#endif

#ifndef ESPD_AIN_NUM_CHANNELS
#define ESPD_AIN_NUM_CHANNELS 1
#endif
#ifndef ESPD_AIN_PIN_0
#define ESPD_AIN_PIN_0 (-1)
#endif
#ifndef ESPD_AIN_PIN_1
#define ESPD_AIN_PIN_1 (-1)
#endif
#ifndef ESPD_AIN_PIN_2
#define ESPD_AIN_PIN_2 (-1)
#endif
#ifndef ESPD_AIN_PIN_3
#define ESPD_AIN_PIN_3 (-1)
#endif
#ifndef ESPD_AIN_PIN_4
#define ESPD_AIN_PIN_4 (-1)
#endif
#ifndef ESPD_AIN_PIN_5
#define ESPD_AIN_PIN_5 (-1)
#endif
#ifndef ESPD_AIN_PIN_6
#define ESPD_AIN_PIN_6 (-1)
#endif
#ifndef ESPD_AIN_PIN_7
#define ESPD_AIN_PIN_7 (-1)
#endif
#ifndef ESPD_AIN_DEADBAND
#define ESPD_AIN_DEADBAND 32
#endif
/* Consumer-side throttle applied when draining the ADC producer queue on the
 * audio thread. 1 = forward every changed sample; N>1 = every N audio blocks.
 * With the producer task doing the actual reads, this is now a secondary knob;
 * ESPD_AIN_TASK_PERIOD_MS is the primary rate limiter. */
#ifndef ESPD_AIN_REPORT_EVERY_N_BLOCKS
#define ESPD_AIN_REPORT_EVERY_N_BLOCKS 1
#endif
/* Dedicated ADC-sampling task: pinned to the core opposite Pd's audio loop so
 * adc_oneshot_read() blocking time never eats the audio block budget.
 * Shorter period = more conversion attempts per second (more temporal data);
 * which raw changes become Pd messages is still governed by ESPD_AIN_DEADBAND
 * (noise gate), not by the period. */
#ifndef ESPD_AIN_TASK_PERIOD_MS
#define ESPD_AIN_TASK_PERIOD_MS 5
#endif
#ifndef ESPD_AIN_TASK_PRIO
#define ESPD_AIN_TASK_PRIO 2
#endif
#ifndef ESPD_AIN_TASK_CORE
#define ESPD_AIN_TASK_CORE 0
#endif
#ifndef ESPD_TOUCH_NUM_CHANNELS
#define ESPD_TOUCH_NUM_CHANNELS 1
#endif
#ifndef ESPD_TOUCH_PIN_0
#define ESPD_TOUCH_PIN_0 (-1)
#endif
#ifndef ESPD_TOUCH_PIN_1
#define ESPD_TOUCH_PIN_1 (-1)
#endif
#ifndef ESPD_TOUCH_PIN_2
#define ESPD_TOUCH_PIN_2 (-1)
#endif
#ifndef ESPD_TOUCH_PIN_3
#define ESPD_TOUCH_PIN_3 (-1)
#endif
#ifndef ESPD_TOUCH_PIN_4
#define ESPD_TOUCH_PIN_4 (-1)
#endif
#ifndef ESPD_TOUCH_PIN_5
#define ESPD_TOUCH_PIN_5 (-1)
#endif
#ifndef ESPD_TOUCH_PIN_6
#define ESPD_TOUCH_PIN_6 (-1)
#endif
#ifndef ESPD_TOUCH_PIN_7
#define ESPD_TOUCH_PIN_7 (-1)
#endif
/* Dedicated touch-sampling task cadence. */
#ifndef ESPD_TOUCH_TASK_PERIOD_MS
#define ESPD_TOUCH_TASK_PERIOD_MS 5
#endif
#ifndef ESPD_TOUCH_TASK_PRIO
#define ESPD_TOUCH_TASK_PRIO 2
#endif
#ifndef ESPD_TOUCH_TASK_CORE
#define ESPD_TOUCH_TASK_CORE 0
#endif
#ifndef ESPD_TOUCH_REPORT_EVERY_N_BLOCKS
#define ESPD_TOUCH_REPORT_EVERY_N_BLOCKS 1
#endif
/* Global cap on WS2812 channel brightness (0..255). Lower values reduce peak
 * LED current draw and the supply-rail dips that couple into the audio codec;
 * the receiver scales incoming r/g/b by (cap/255) before writing the strip. */
#ifndef ESPD_LED_MAX_BRIGHTNESS
#define ESPD_LED_MAX_BRIGHTNESS 255
#endif

/* Analog outputs (PWM-backed): [s espd/aout/0]..[s espd/aout/N] expect normalized 0..1 floats. */
#ifndef ESPD_AOUT_NUM_CHANNELS
#define ESPD_AOUT_NUM_CHANNELS 0
#endif
#ifndef ESPD_AOUT_PIN_0
#define ESPD_AOUT_PIN_0 (-1)
#endif
#ifndef ESPD_AOUT_PIN_1
#define ESPD_AOUT_PIN_1 (-1)
#endif
#ifndef ESPD_AOUT_PIN_2
#define ESPD_AOUT_PIN_2 (-1)
#endif
#ifndef ESPD_AOUT_PIN_3
#define ESPD_AOUT_PIN_3 (-1)
#endif
#ifndef ESPD_AOUT_PWM_FREQ_HZ
#define ESPD_AOUT_PWM_FREQ_HZ 20000
#endif
#ifndef ESPD_AOUT_PWM_FALLBACK_FREQ_HZ
#define ESPD_AOUT_PWM_FALLBACK_FREQ_HZ 10000
#endif

/* GPIO digital inputs (config.txt din_pins=); indices follow BSP button count. */
#ifndef ESPD_DIN_GPIO_ACTIVE_LOW
#define ESPD_DIN_GPIO_ACTIVE_LOW 1
#endif
#ifndef ESPD_DIN_GPIO_TASK_PERIOD_MS
#define ESPD_DIN_GPIO_TASK_PERIOD_MS 5
#endif
#ifndef ESPD_DIN_GPIO_TASK_PRIO
#define ESPD_DIN_GPIO_TASK_PRIO 2
#endif
#ifndef ESPD_DIN_GPIO_TASK_CORE
#define ESPD_DIN_GPIO_TASK_CORE 0
#endif

/* 1 = start legacy espd TCP/UDP transport tasks; 0 = WiFi only for Pd objects. */
#ifndef ESPD_ENABLE_LEGACY_WIFI_TRANSPORT
#define ESPD_ENABLE_LEGACY_WIFI_TRANSPORT 1
#endif
/** VFS path where the SD card is mounted (ESP-ADF LyraT default: /sdcard). */
#ifndef ESPD_SDCARD_MOUNT
#define ESPD_SDCARD_MOUNT "/sdcard"
#endif
/** SD card patch store when the card is mounted (see pdmain_init). */
#define ESPD_SDCARD_MAIN_PD_PATH ESPD_SDCARD_MOUNT "/main.pd"
#define ESPD_SDCARD_CONFIG_PATH ESPD_SDCARD_MOUNT "/config.txt"
#if CONFIG_ESPD_USE_USB_MSC
/** VFS path where USB MSC storage is mounted (flash "storage" partition). */
#ifndef ESPD_STORAGE_MOUNT
#define ESPD_STORAGE_MOUNT "/storage"
#endif
#define ESPD_STORAGE_MAIN_PD_PATH ESPD_STORAGE_MOUNT "/main.pd"
#define ESPD_STORAGE_CONFIG_PATH ESPD_STORAGE_MOUNT "/config.txt"
#endif
/* config.txt optional keys (key=value, # comment). Active store: mounted SD,
 * else mounted internal flash (/storage) — not mixed by file presence.
 * WiFi (when ESPD_USE_WIFI): STA starts when config.txt has a non-empty wifi_ssid=
 *   (wifi_password optional). Missing config.txt → with ESPD_USE_SDCARD, STA stays
 *   off; otherwise Kconfig/locale defaults apply.
 *   wifi_ssid=myap
 *   wifi_password=secret
 * Analog in (when ESPD_USE_AIN compiled in): espd/ain starts only if config.txt
 *   has ain_pins= with at least one GPIO. Off when there is no config.txt,
 *   no ain_pins= key, or ain_pins= is empty.
 *   ain_pins=3,4,5,6,7 — GPIOs for espd/ain/0.. in order (max 8)
 *   ain_pins=      — empty list: analog off
 *   (Implementation uses ESP32 ADC unit 1; not the same as adc~ audio input.)
 *   ain_task_period_ms=1 — producer wake interval in ms (1..500); lower =
 *       more samples in time (more ADC-task CPU). Does not loosen filtering.
 *   ain_deadband=N — raw delta to treat as a new value (1..2047); noise gate
 *       only — smaller = more sensitive / more messages when the input moves.
 *   ain_report_every_n_blocks=1 — audio thread forwards at most every N
 *       blocks; keep 1 for lowest latency to Pd when the producer has updates.
 * Touch (when ESPD_USE_TOUCH compiled in): espd/touch starts only if config.txt
 *   has touch_pins= with at least one touch-capable GPIO. No config.txt, no
 *   touch_pins key, or empty list → no touch task / no polling.
 *   touch_pins=4,5,6      — GPIOs for espd/touch/0.. in order (ESP32-S3: 1–14)
 *   touch_pins=            — empty list: touch off
 *   touch_task_period_ms=5 — producer wake interval in ms (1..500)
 *   touch_report_every_n_blocks=1 — audio thread forwards at most every N blocks
 * PWM out (when ESPD_USE_AOUT compiled in): espd/aout starts only if config.txt
 *   has aout_pins= with at least one GPIO. No aout_pins key or empty list →
 *   no LEDC setup (patch floats are ignored). No background polling.
 *   aout_pins=8,9 — GPIOs for espd/aout/0.. in order (max 4)
 *   aout_pwm_freq_hz=20000 — LEDC frequency (optional; default 20000)
 * Digital in (when ESPD_USE_DIN compiled in): GPIO channels append after BSP
 *   buttons. espd/din/0..(N-1) are board digital ins (e.g. via an I2C IO
 *   expander on a BSP kit); din_pins= adds espd/din/N.. at indices
 *   bsp_button_count()+i. Boot log lists
 *   the full map. No din_pins= key → no extra GPIO polling (BSP din still works).
 *   din_pins=4,5 — GPIOs for extra espd/din channels (max 8)
 *   din_active_low=1 — treat low level as pressed (default 1)
 *   din_task_period_ms=5 — GPIO poll interval in ms (1..500)
 * Digital out (when ESPD_USE_DOUT compiled in): espd/dout starts only if config.txt
 *   has dout_pins= with at least one GPIO. Float >= 0.5 → high, else low. No
 *   background polling.
 *   dout_pins=8,9 — GPIOs for espd/dout/0.. in order (max 8)
 * Audio (codec and generic I2S backends):
 *   audio_sample_rate=48000 — Hz (8000..192000; default CONFIG_ESPD_AUDIO_SAMPLE_RATE).
 *     Single source of truth: same value flows into both the audio backend
 *     (I2S / codec) and Pd (sys_getsr). If the codec / BSP cannot deliver this
 *     rate, audio init fails (better than a silent pitch shift).
 *   audio_dma_desc_num=3 — number of DMA buffers (2..16; IDF default 6)
 *   audio_dma_frame_num=64 — frames per buffer (8..1024; IDF default 240)
 *   Output latency ≈ desc_num * frame_num / sample_rate seconds.
 *   IDF defaults (6×240, ~30 ms) are conservative; 3×64 (~4 ms) is a good
 *   low-latency target for typical audio kits. Pd block size is 64 samples;
 *   64 frames/buffer matches one block (low latency).
 *   Larger frame_num (e.g. 128 or 240) adds headroom for FFT-heavy patches.
 */
/* [pdcontrol] message "ip" → list of four float octets 0..255, or symbol "noip" when unavailable. */
void espd_pdcontrol_sync_cwd(void);
#include "espd_storage.h"

void pdmain_reload_patch(void);
void pdmain_reload_patch_from(const char *dir);

/* USB dev-sync MSC takeover mode:
 * active => keep MSC hidden from host and use /storage for CDC dev sync. */
bool espd_usb_msc_sync_mode_active(void);
void espd_usb_msc_sync_mode_set(bool active);
#if CONFIG_ESPD_USE_USB_OTG && CONFIG_ESPD_USE_USB_MSC
/** MSC storage instance exists (internal flash partition wired for USB). */
bool espd_usb_msc_storage_present(void);
/** True when internal flash FAT is owned by the USB host, not the app. */
bool espd_usb_msc_host_mounted(void);
/** Hand /storage to the USB host (mass-storage volume). */
esp_err_t espd_usb_expose_msc_to_host(void);
/** Take /storage back for app I/O (dev PUT/RELOAD); no-op if already on APP. */
esp_err_t espd_usb_ensure_msc_app_mount(void);
#endif
extern int espd_main_pd_loaded_from_store;
/** If a local main.pd was opened, which directory it was loaded from (e.g. /sdcard or /storage). */
extern const char *espd_main_pd_loaded_dir;
#ifdef ESPD_USE_WIFI
extern int espd_wifi_net_enabled;
/** True after net_init() has finished (TCP patch link up). Safe gate for net_send*. */
int espd_net_send_ready(void);
#endif

/** Zero [cputime] accumulated counter. */
void espd_cputime_reset(void);
/** Summed wall microseconds per loop (polls + pdmain_tick + net_alive; not
 *  senddacs). Not true CPU time — cheap esp_timer path; see espd.c. */
unsigned int espd_cputime_get(void);

#endif /* ESPD_H */
