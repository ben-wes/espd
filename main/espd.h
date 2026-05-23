#ifndef ESPD_H
#define ESPD_H

#include "espd_config.h"

/* task priorities */
#define  PRIORITY_WIFI 2

    /* if WIFI is enabled we need to define the WIFI name and password,
    the peer machine, and the send and receive ports.  This can be done
    using main/locale.h (copy from locale.h.example) or by defining
    CONFIG_ESP_WIFI_SSID, etc., in sdkconfig / compile flags. */
#if defined(PD_USE_WIFI)
#include "sdkconfig.h"
#endif
#if defined(CONFIG_LOCALE_FILE)
#include CONFIG_LOCALE_FILE
#else
#if defined(PD_USE_WIFI) && !defined(CONFIG_ESP_WIFI_SSID)
#include "locale.h"
#endif
#endif
#if defined(PD_USE_WIFI)
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
void pd_sendmsg(char *buf, int bufsize);
void pd_fromhost(char *data, size_t size);
void espd_control_io_init(void);

#ifdef PD_USE_BLUETOOTH
void pd_bt_poll( void);
void bt_init( void);
void pd_bt_writeback(unsigned char *s, int length);
#endif

#ifdef PD_USE_WIFI
void espd_netif_ensure_init(void); /* wifi.c - idempotent lwIP/event-loop init */
void wifi_init(void);   /* wifi.c - manage 802.11 connection */
void net_init( void);   /* init */
void net_hello( void);  /* send initial TCP packet when connected */
void net_alive( void);  /* send keep-alive packet if needed */
void net_sendudp(void *msg, int len, int port); /* send whatev */
void net_sendtcp(void *msg, int len);
extern char wifi_ipaddr[];
extern char espd_wifi_ssid[33];
extern char espd_wifi_password[65];
extern int espd_log_broadcast_port; /* UDP port for Pd log/error broadcast; 0 disables */
#endif

#ifndef PIN_BIT_CLOCK
#define PIN_BIT_CLOCK 13
#define PIN_WORD_SELECT 33
#define PIN_DATA_OUT 32
#define PIN_DATA_IN 35
#endif

#ifndef ESPD_ANALOG_NUM_CHANNELS
#define ESPD_ANALOG_NUM_CHANNELS 1
#endif
#ifndef ESPD_ANALOG_PIN_0
#define ESPD_ANALOG_PIN_0 (-1)
#endif
#ifndef ESPD_ANALOG_PIN_1
#define ESPD_ANALOG_PIN_1 (-1)
#endif
#ifndef ESPD_ANALOG_PIN_2
#define ESPD_ANALOG_PIN_2 (-1)
#endif
#ifndef ESPD_ANALOG_PIN_3
#define ESPD_ANALOG_PIN_3 (-1)
#endif
#ifndef ESPD_ANALOG_PIN_4
#define ESPD_ANALOG_PIN_4 (-1)
#endif
#ifndef ESPD_ANALOG_PIN_5
#define ESPD_ANALOG_PIN_5 (-1)
#endif
#ifndef ESPD_ANALOG_PIN_6
#define ESPD_ANALOG_PIN_6 (-1)
#endif
#ifndef ESPD_ANALOG_PIN_7
#define ESPD_ANALOG_PIN_7 (-1)
#endif
#ifndef ESPD_ANALOG_DEADBAND
#define ESPD_ANALOG_DEADBAND 32
#endif
/* Consumer-side throttle applied when draining the ADC producer queue on the
 * audio thread. 1 = forward every changed sample; N>1 = every N audio blocks.
 * With the producer task doing the actual reads, this is now a secondary knob;
 * ESPD_ANALOG_TASK_PERIOD_MS is the primary rate limiter. */
#ifndef ESPD_ANALOG_REPORT_EVERY_N_BLOCKS
#define ESPD_ANALOG_REPORT_EVERY_N_BLOCKS 1
#endif
/* Dedicated ADC-sampling task: pinned to the core opposite Pd's audio loop so
 * adc_oneshot_read() blocking time never eats the audio block budget.
 * Shorter period = more conversion attempts per second (more temporal data);
 * which raw changes become Pd messages is still governed by ESPD_ANALOG_DEADBAND
 * (noise gate), not by the period. */
#ifndef ESPD_ANALOG_TASK_PERIOD_MS
#define ESPD_ANALOG_TASK_PERIOD_MS 5
#endif
#ifndef ESPD_ANALOG_TASK_PRIO
#define ESPD_ANALOG_TASK_PRIO 2
#endif
#ifndef ESPD_ANALOG_TASK_CORE
#define ESPD_ANALOG_TASK_CORE 0
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

#ifndef ESPD_PATCH_STORE_MOUNT
#define ESPD_PATCH_STORE_MOUNT "/espd_pd"
#endif
#ifndef ESPD_PATCH_SPIFFS_PARTITION_LABEL
#define ESPD_PATCH_SPIFFS_PARTITION_LABEL "pdstore"
#endif
#ifndef ESPD_SKIP_WIFI_WHEN_MAIN_PD_ON_DISK
#define ESPD_SKIP_WIFI_WHEN_MAIN_PD_ON_DISK 0
#endif
/* 1 = start legacy espd TCP/UDP transport tasks; 0 = WiFi only for Pd objects. */
#ifndef ESPD_ENABLE_LEGACY_WIFI_TRANSPORT
#define ESPD_ENABLE_LEGACY_WIFI_TRANSPORT 1
#endif
/** VFS path where the SD card is mounted (ESP-ADF LyraT default: /sdcard). */
#ifndef ESPD_SDCARD_MOUNT
#define ESPD_SDCARD_MOUNT "/sdcard"
#endif
/** If present on the mounted SD card, loaded before SPIFFS (see pdmain_init). */
#define ESPD_SDCARD_MAIN_PD_PATH ESPD_SDCARD_MOUNT "/main.pd"
#define ESPD_SDCARD_CONFIG_PATH ESPD_SDCARD_MOUNT "/config.txt"
/** Internal flash (SPIFFS) fallback when SD has no main.pd / config.txt. */
#define ESPD_PATCH_STORE_CONFIG_PATH ESPD_PATCH_STORE_MOUNT "/config.txt"
#ifdef PD_USE_USB_MSC
/** VFS path where USB MSC storage is mounted. */
#ifndef ESPD_STORAGE_MOUNT
#define ESPD_STORAGE_MOUNT "/storage"
#endif
/** If present on USB MSC storage, loaded after SD and before SPIFFS (see pdmain_init). */
#define ESPD_STORAGE_MAIN_PD_PATH ESPD_STORAGE_MOUNT "/main.pd"
#define ESPD_STORAGE_CONFIG_PATH ESPD_STORAGE_MOUNT "/config.txt"
#endif
/* config.txt optional keys (key=value, # comment). Search order: SD card,
 * then SPIFFS at ESPD_PATCH_STORE_MOUNT, then USB MSC (if enabled).
 * WiFi (when PD_USE_WIFI): STA starts only if config.txt exists and wifi_ssid=
 *   has a non-empty value (wifi_password optional). Missing file → with
 *   PD_USE_SDCARD, STA stays off; otherwise Kconfig/locale defaults apply.
 *   log_broadcast_port=9001
 *     >0 enables UDP broadcast of Pd print/error output to this port
 *     (when WiFi/net is active); 0 or missing keeps default log routing.
 *   wifi_ssid=myap
 *   wifi_password=secret
 * Analog (when PD_USE_ANALOG0): ADC / espd/ain starts only if analog_pins=
 *   lists at least one GPIO (ADC1-capable). Missing file, no analog_pins key,
 *   or analog_pins= with an empty list → analog stays off (Kconfig defaults).
 *   analog_pins=3,4,5,6,7 — GPIOs for espd/ain/0.. in order (max 8, ADC1 pins)
 *   analog_pins=      — empty list: do not start analog
 *   analog_task_period_ms=1 — producer wake interval in ms (1..500); lower =
 *       more samples in time (more ADC-task CPU). Does not loosen filtering.
 *   analog_deadband=N — raw delta to treat as a new value (1..2047); noise gate
 *       only — smaller = more sensitive / more messages when the input moves.
 *   analog_report_every_n_blocks=1 — audio thread forwards at most every N
 *       blocks; keep 1 for lowest latency to Pd when the producer has updates.
 * Touch (when PD_USE_TOUCH0): espd/touch/N from ESP32 touch sensor GPIOs.
 *   Enable in menuconfig (ESPD_PD_USE_TOUCH0). From config.txt (SD or SPIFFS):
 *   touch_pins= lists touch-capable GPIOs (max 8). Empty list → touch off.
 *   touch_pins=4,5,6      — GPIOs for espd/touch/0.. in order (ESP32-S3: 1–14)
 *   touch_pins=           — empty list: do not start touch
 *   touch_task_period_ms=5 — producer wake interval in ms (1..500)
 *   touch_report_every_n_blocks=1 — audio thread forwards at most every N blocks
 *   Without config.txt, use menuconfig → Capacitive touch (espd/touch) defaults.
 * Audio I2S DMA (codec and generic I2S backends):
 *   audio_dma_desc_num=3 — number of DMA buffers (2..16; IDF default 6)
 *   audio_dma_frame_num=64 — frames per buffer (8..1024; IDF default 240)
 *   Output latency ≈ desc_num * frame_num / sample_rate seconds.
 *   Stock Waveshare firmware uses 3×64 (~4 ms). IDF defaults are 6×240 (~30 ms).
 *   Pd block size is 64 samples; 64 frames/buffer matches one block (low latency).
 *   Larger frame_num (e.g. 128 or 240) adds headroom for FFT-heavy patches.
 */
/* [pdcontrol] message "ip" → list of four float octets 0..255, or symbol "noip" when unavailable. */
#define ESPD_MAIN_PD_PATH ESPD_PATCH_STORE_MOUNT "/main.pd"

#include "espd_storage.h"

extern int espd_main_pd_loaded_from_store;
/** If a local main.pd was opened, which directory it was loaded from (e.g. /sdcard or /espd_pd). */
extern const char *espd_main_pd_loaded_dir;
#ifdef PD_USE_WIFI
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
