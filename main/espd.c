/*
 * espd main: boot coordinator + audio loop.
 * Subsystems are in separate modules (espd_usb.c, espd_gpio_io.c,
 * espd_config_file.c, espd_storage.c, espd_io.c).
 */

#include "espd.h"
#include "espd_audio.h"
#include "espd_pd_io.h"
#include "espd_usb.h"
#include "espd_sync_io.h"
#include "espd_config_file.h"
#include "espd_gpio_peripherals.h"
#include "espd_dev.h"
#include "espd_storage.h"
#include "espd_i2c.h"
#if CONFIG_ESPD_WIFI_AP_SYNC
#include "espd_dev_wifi.h"
#include "wifi.h"
#endif
#include "bsp/bsp_io.h"
#include "../pd/src/m_pd.h"
#if CONFIG_ESPD_USE_USB_OTG
#include <tinyusb.h>
#include <tinyusb_cdc_acm.h>
#endif
#if CONFIG_ESPD_USE_USB_MIDI_HOST
#include "espd_usb_host_midi.h"
#endif

#include <ctype.h>
#include <esp_event.h>
#include <esp_heap_caps.h>
#include <esp_netif.h>
#include <esp_pthread.h>
#include <esp_timer.h>
#include <math.h>
#include <nvs.h>
#include <nvs_flash.h>
#include <stdarg.h>

static const char *TAG = "ESPD";
bool g_espd_pd_running = false;

int espd_main_pd_loaded_from_store;
const char *espd_main_pd_loaded_dir;

#ifdef ESPD_USE_WIFI
int espd_wifi_net_enabled = 1;
char espd_wifi_ssid[33];
char espd_wifi_password[65];

static void espd_wifi_config_defaults(void)
{
    if (espd_wifi_ssid[0] == '\0') {
        snprintf(espd_wifi_ssid, sizeof(espd_wifi_ssid), "%s", CONFIG_ESP_WIFI_SSID);
        snprintf(espd_wifi_password, sizeof(espd_wifi_password), "%s", CONFIG_ESP_WIFI_PASSWORD);
    }
}

static int espd_wifi_config_txt_allows_sta(void)
{
    if (!espd_storage_config_path())
        return 0;
    return g_espd_cfg.wifi_have_ssid;
}
#endif /* ESPD_USE_WIFI */

/* ─── NVS ─── */

static void espd_nvs_flash_init(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
}

/* ─── Audio engine ─── */

extern void pdmain_tick(void);
void pdmain_init(void);

static espd_audio_t *s_audio;
#define BLKSIZE 64
DRAM_ATTR float soundin[IOCHANS * BLKSIZE], soundout[IOCHANS * BLKSIZE];
DRAM_ATTR short poodle[IOCHANS * BLKSIZE];

static inline short espd_soundout_to_short(float x)
{
    x = (x > 1.f) ? 1.f : ((x < -1.f) ? -1.f : x);
    short y = (short)lrintf(x * 32768.f);
    return (y > 32767) ? 32767 : ((y < -32768) ? -32768 : y);
}

static inline float espd_soundin_from_i16(int16_t s)
{
    return (float)s * (1.f / 32768.f);
}

void senddacs(void)
{
    int i, j;
    esp_err_t err;

    if (!s_audio)
        return;

    for (i = j = 0; i < BLKSIZE; i++, j += IOCHANS)
    {
        poodle[j] = espd_soundout_to_short(soundout[i]);
        soundout[i] = 0.f;
#if IOCHANS > 1
        poodle[j + 1] = espd_soundout_to_short(soundout[i + BLKSIZE]);
        soundout[i + BLKSIZE] = 0.f;
#endif
    }

    err = espd_audio_write(s_audio, poodle, (size_t)(IOCHANS * BLKSIZE));
    if (err != ESP_OK)
        ESP_LOGE(TAG, "audio write failed: %s", esp_err_to_name(err));

#ifdef ESPD_USE_ADC
    err = espd_audio_read(s_audio, poodle, (size_t)(IOCHANS * BLKSIZE));
    if (err != ESP_OK && err != ESP_ERR_NOT_SUPPORTED)
        ESP_LOGE(TAG, "audio read failed: %s", esp_err_to_name(err));

    for (i = j = 0; i < BLKSIZE; i++, j += IOCHANS)
    {
        soundin[i] = espd_soundin_from_i16(poodle[j]);
    #if IOCHANS > 1
        soundin[i + BLKSIZE] = espd_soundin_from_i16(poodle[j + 1]);
    #endif
    }
#endif /* ESPD_USE_ADC */
}

static void espd_initdacs(void)
{
    esp_err_t e = espd_audio_init(&s_audio);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "audio init failed: %s — no sound output", esp_err_to_name(e));
        s_audio = NULL;
        return;
    }
    memset(soundout, 0, sizeof(soundout));
    memset(soundin, 0, sizeof(soundin));
}

/* ─── Pd host messaging ─── */

void *getbytes(size_t nbytes);
void freebytes(void *x, size_t nbytes);
void *resizebytes(void *x, size_t oldsize, size_t newsize);
static char *pd_bt_buf;
static int pd_bt_size;
static SemaphoreHandle_t pd_bt_mutex;

void pd_fromhost(char *data, size_t size)
{
    if (!pd_bt_buf)
        pd_bt_buf = getbytes(0);
    if (!pd_bt_mutex)
        pd_bt_mutex = xSemaphoreCreateMutex();
    while (xSemaphoreTake(pd_bt_mutex, 1) != pdTRUE)
        ;
    pd_bt_buf = (char *)resizebytes(pd_bt_buf, pd_bt_size, pd_bt_size+size);
    memcpy(pd_bt_buf + pd_bt_size, data, size);
    pd_bt_size += size;
    xSemaphoreGive(pd_bt_mutex);
}

void pd_pollhost(void)
{
    int lastchar;
    if (!pd_bt_mutex)
        pd_bt_mutex = xSemaphoreCreateMutex();
    if (xSemaphoreTake(pd_bt_mutex, 0) != pdTRUE)
        return;

    lastchar = pd_bt_size-1;
    while (lastchar >= 0 && isspace((int)(pd_bt_buf[lastchar])))
        lastchar--;

    if (lastchar >= 3 && pd_bt_buf[lastchar] == ';' &&
        pd_bt_buf[lastchar-1] != '\\')
    {
        pd_sendmsg(pd_bt_buf, pd_bt_size);
        pd_bt_buf = (char *)resizebytes(pd_bt_buf, pd_bt_size, 0);
        pd_bt_size = 0;
    }
    xSemaphoreGive(pd_bt_mutex);
}

void pdmain_print(const char *s)
{
    if (!s || !*s)
        return;

#if CONFIG_ESPD_DEV_SERIAL_SYNC || CONFIG_ESPD_DEV_CDC_SYNC || CONFIG_ESPD_WIFI_AP_SYNC
    espd_sync_print(s);
#else
    printf("%s", s);
#endif

#if defined(ESPD_USE_WIFI) && ESPD_ENABLE_LEGACY_WIFI_TRANSPORT
    if (espd_wifi_net_enabled && wifi_ipaddr[0] != '\0') {
        char y[81];
        strncpy(y, s, 79);
        y[79] = '\0';
        strcat(y, ";");
        net_sendudp(y, strlen(y), CONFIG_ESP_WIFI_SENDPORT);
        net_sendtcp(y, strlen(y));
    }
#endif
}

void trymem(int foo);

/* ─── cputime ─── */

static unsigned int cputime;

void espd_cputime_reset(void)
{
    cputime = 0;
}

unsigned int espd_cputime_get(void)
{
    return cputime;
}

/* ─── app_main ─── */

void app_main(void)
{
    esp_log_level_set("*", ESP_LOG_WARN);
    esp_log_level_set("ESPD", ESP_LOG_INFO);
    esp_log_level_set("espd_usb", ESP_LOG_INFO);
    esp_log_level_set("espd_config", ESP_LOG_INFO);
    esp_log_level_set("espd_pd_io", ESP_LOG_INFO);
    esp_log_level_set("espd_gpio_peripherals", ESP_LOG_INFO);
    esp_log_level_set("espd_storage", ESP_LOG_INFO);
    esp_log_level_set("espd_dev", ESP_LOG_INFO);
    esp_log_level_set("espd_audio", ESP_LOG_INFO);
    esp_log_level_set("espd_audio_dac", ESP_LOG_INFO);
    esp_log_level_set("sdmmc_sd", ESP_LOG_NONE);
    esp_log_level_set("sdmmc_common", ESP_LOG_NONE);
    esp_log_level_set("vfs_fat_sdmmc", ESP_LOG_NONE);

    espd_nvs_flash_init();

    /* lwIP + default event loop — needed unconditionally because Pd's netsend/
     * netreceive can call socket() at any time, even on non-WiFi builds. */
    esp_netif_init();
    esp_event_loop_create_default();


    /* Register a full-flash /storage partition at runtime, so one firmware uses
     * whatever flash the chip has (no per-board flash size). Must run before the
     * storage partition is first looked up below. */
    (void)espd_usb_register_dynamic_storage();

    /* /storage for config.txt before USB takes over the flash partition. */
    esp_err_t mnt = espd_usb_mount_flash_early_vfs();
    if (mnt != ESP_OK)
        ESP_LOGW(TAG, "/storage on early VFS unavailable");

#if CONFIG_ESP_WIFI_REMOTE_ENABLED
    /* Deferred esp_hosted init (see wifi.c wrap): needs nvs, netif, event loop. */
    wifi_ensure_hosted();
#endif

#ifdef ESPD_USE_SDCARD
    espd_storage_mount_sdcard();
#endif
    espd_storage_init();
    espd_config_load();

#ifdef ESPD_USE_WIFI
    espd_wifi_config_defaults();
#if CONFIG_ESPD_WIFI_AP_SYNC
    {
        const char *ap_ssid = g_espd_cfg.wifi_ap_have_ssid ? g_espd_cfg.wifi_ap_ssid : NULL;
        wifi_ap_configure(ap_ssid, g_espd_cfg.wifi_ap_password);
        wifi_prepare_phy();
        if (g_espd_cfg.wifi_have_ssid) {
            espd_wifi_net_enabled = 1;
            wifi_start_apsta();
        } else {
            espd_wifi_net_enabled = 0;
            wifi_start_ap();
        }
        {
            int ap_min = g_espd_cfg.wifi_sync_ap_minutes;
            if (ap_min < 0)
                ap_min = 30;
            wifi_ap_boot_window_start(ap_min);
        }
    }
#else
    if (!espd_wifi_config_txt_allows_sta())
        espd_wifi_net_enabled = 0;
    if (espd_wifi_net_enabled)
        wifi_prepare_phy();
#endif
#endif

#ifdef ESPD_USE_WIFI
#if !CONFIG_ESPD_WIFI_AP_SYNC
    if (espd_wifi_net_enabled)
        wifi_start_sta();
#endif
#endif

#if CONFIG_ESPD_DEV_SERIAL_SYNC
    espd_dev_init();
#endif
#if CONFIG_ESPD_DEV_SERIAL_SYNC || CONFIG_ESPD_DEV_CDC_SYNC || CONFIG_ESPD_WIFI_AP_SYNC
    esp_log_set_vprintf(espd_sync_log);
#endif
#if CONFIG_ESPD_WIFI_AP_SYNC
    espd_dev_wifi_init();
#endif

#if CONFIG_ESP_MAIN_TASK_STACK_SIZE < 16384
    ESP_LOGW(TAG,
        "Main task stack is %d bytes — too small for Pd (use >= 32768, "
        "65536 for FFT-heavy patches). menuconfig → Component config → "
        "ESP System Settings → Main task stack size",
        CONFIG_ESP_MAIN_TASK_STACK_SIZE);
#endif
#if CONFIG_SPIRAM
    heap_caps_malloc_extmem_enable(16384);
#endif

    esp_pthread_cfg_t pth_cfg = esp_pthread_get_default_config();
    pth_cfg.stack_size = 8192;
    pth_cfg.prio = 5;
    pth_cfg.pin_to_core = 0;
#if CONFIG_SPIRAM
    /* Pd's pthreads (e.g. [pd~]/clone) take stacks from PSRAM where available, to
     * spare scarce internal RAM. On a no-PSRAM chip keep the default internal
     * stack — requesting MALLOC_CAP_SPIRAM there would simply fail to allocate. */
    pth_cfg.stack_alloc_caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
#endif
    if (esp_pthread_set_cfg(&pth_cfg) != ESP_OK)
        ESP_LOGW(TAG, "esp_pthread_set_cfg failed; using IDF defaults");

    bsp_led_init();
    bsp_button_set_handler(espd_din_changed);
    bsp_button_init();

#ifdef ESPD_USE_WIFI
    if (espd_wifi_net_enabled) {
        bool sta_connected = wifi_wait_sta(pdMS_TO_TICKS(2000));
#if ESPD_ENABLE_LEGACY_WIFI_TRANSPORT
        /* Only stand up the legacy TCP/UDP transport once STA is actually
         * connected. Starting it with no network (e.g. the fallback SSID not
         * found) wastes the receiver task stacks + socket buffers — on the tiny
         * classic-ESP32 RAM that pushed Pd into an out-of-memory panic at boot. */
        if (sta_connected) {
            net_init();
            net_hello();
        } else {
            ESP_LOGI(TAG, "WiFi not connected — skipping legacy net transport");
        }
#endif
    }
#endif

#if CONFIG_ESPD_USE_USB_OTG
    /* OTG: TinyUSB + MSC mount before touching I2S (codec init is below, after
     * this block). Flash/USB teardown must finish before bsp_audio_init(). */
    bool usb_host_mode = false;
#if CONFIG_ESPD_USE_USB_MIDI_HOST
    usb_host_mode = (g_espd_cfg.usb_midi_mode == ESPD_USB_MIDI_HOST);
#else
    if (g_espd_cfg.usb_midi_mode == ESPD_USB_MIDI_HOST) {
        ESP_LOGW(TAG, "config.txt usb_midi_role=host, but USB-MIDI host not "
            "compiled — falling back to device mode");
    }
#endif
    if (usb_host_mode) {
#if CONFIG_ESPD_USE_USB_MIDI_HOST
        ESP_LOGI(TAG, "USB MIDI: host. Serial monitor unavailable while hosting.");
        if (espd_usb_host_midi_start() != ESP_OK)
            ESP_LOGE(TAG, "USB host: start failed");
#endif
    } else {
        const char *midi_name = (g_espd_cfg.usb_midi_mode == ESPD_USB_MIDI_DEVICE)
            ? "device (CDC+MSC+MIDI)" : "off (CDC+MSC only)";
        ESP_LOGI(TAG, "USB MIDI: %s", midi_name);
        if (!espd_usb_start_after_wifi())
            ESP_LOGE(TAG, "USB: boot init failed");
    }

#if CONFIG_ESPD_USE_USB_MSC
#if CONFIG_ESPD_DEV_CDC_SYNC
#ifdef ESPD_USE_SDCARD
    if (espd_storage_sdcard_ready()) {
        (void)espd_usb_expose_msc_to_host();
        ESP_LOGI(TAG, "SD card is the store: internal flash exposed to host, Pd runs");
    } else
#endif
    {
        /* Flash store: first-boot USB drive on OTG CDC. Drive mode only on cold
         * power-on with a host; button reset and dev-sync RESET skip it. */
        if (!usb_host_mode && espd_usb_msc_storage_present()) {
            if (esp_reset_reason() == ESP_RST_POWERON
                && espd_usb_wait_for_host(pdMS_TO_TICKS(2000))) {
                espd_usb_drive_mode_wait();
            } else if (esp_reset_reason() != ESP_RST_POWERON) {
                ESP_LOGI(TAG, "warm reset — skipping USB drive mode, starting Pd");
            }
            if (espd_usb_msc_disable_and_remount_vfs() == ESP_OK)
                espd_storage_resolve_paths();
        }
    }
#else /* MSC without OTG CDC dev-sync: no first-boot drive gate */
    if (!usb_host_mode && espd_usb_msc_storage_present()) {
        if (espd_usb_msc_disable_and_remount_vfs() == ESP_OK)
            espd_storage_resolve_paths();
    } else if (usb_host_mode) {
        ESP_LOGI(TAG, "USB host mode: keeping early /storage VFS (no MSC teardown)");
    }
#endif
#endif /* CONFIG_ESPD_USE_USB_MSC */

#endif /* CONFIG_ESPD_USE_USB_OTG */

    /* Codec (I2S + BSP I2C) before Pd. On OTG kits, USB/MSC teardown above
     * already finished; patch objects (espd_i2c, etc.) see a live bus. */
    espd_initdacs();
#ifdef ESPD_USE_I2C
    espd_i2c_init();
#endif
    pdmain_init();

    ESP_LOGI(TAG, "codec=%s", s_audio ? "ok" : "FAILED");

#if CONFIG_ESPD_USE_USB_MSC && CONFIG_ESPD_DEV_CDC_SYNC
#ifdef ESPD_USE_SDCARD
    /* SD is the patch store: internal flash may stay exposed to the host. */
    if (espd_storage_sdcard_ready()
        && espd_usb_msc_storage_present()
#if CONFIG_ESPD_USE_USB_MIDI
        && g_espd_cfg.usb_midi_mode != ESPD_USB_MIDI_DEVICE
#endif
    ) {
        esp_err_t exp = espd_usb_expose_msc_to_host();
        if (exp == ESP_OK)
            ESP_LOGI(TAG, "USB: /storage exposed to host");
        else
            ESP_LOGW(TAG, "USB: host MSC expose: %s", esp_err_to_name(exp));
    }
#endif
#endif

    espd_aout_init();
    espd_dout_init();
    espd_pd_io_bind_leds();
    espd_din_gpio_init();
    espd_din_log_map();
    espd_ain_init();
    espd_touch_init();

    ESP_LOGI(TAG, "entering audio block loop");

    UBaseType_t was = uxTaskPriorityGet(NULL);
    UBaseType_t want = 19;
    if (want > (UBaseType_t)(configMAX_PRIORITIES - 2))
        want = (UBaseType_t)(configMAX_PRIORITIES - 2);
    vTaskPrioritySet(NULL, want);
    ESP_LOGI(TAG, "audio loop priority set to %u (was %u)",
        (unsigned)want, (unsigned)was);

    g_espd_pd_running = true;

    while (1)
    {
        uint64_t t0 = (uint64_t)esp_timer_get_time();
        if (!espd_dev_sync_poll()) {
            pd_pollhost();
            espd_pd_io_poll();
            pdmain_tick();
        }
        cputime += (unsigned int)((uint64_t)esp_timer_get_time() - t0);
        senddacs();
    }
}

static void espd_print_memdiag(void)
{
    char msg[192];
    size_t int_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    size_t int_largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    size_t all_free = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    size_t all_largest = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
#if CONFIG_SPIRAM
    size_t psram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    size_t psram_largest = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
    snprintf(msg, sizeof(msg),
        "mem: int_free=%u int_largest=%u all_free=%u all_largest=%u psram_free=%u psram_largest=%u\n",
        (unsigned)int_free, (unsigned)int_largest,
        (unsigned)all_free, (unsigned)all_largest,
        (unsigned)psram_free, (unsigned)psram_largest);
#else
    snprintf(msg, sizeof(msg),
        "mem: int_free=%u int_largest=%u all_free=%u all_largest=%u\n",
        (unsigned)int_free, (unsigned)int_largest,
        (unsigned)all_free, (unsigned)all_largest);
#endif
    pdmain_print(msg);
}

void glob_mem(void *dummy)
{
    (void)dummy;
    espd_print_memdiag();
}
