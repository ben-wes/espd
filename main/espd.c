/*

   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

#include "espd.h"
#include "../pd/src/m_pd.h"
#include <string.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#ifdef PD_LYRAT
#include "board.h"
#endif /* PD_LYRAT */
#ifdef OBSOLETEAPI
#include "driver/i2s.h"
#else /* OBSOLETEAPI */
#include "driver/i2s_std.h"
#include "driver/gpio.h"
#endif /* OBSOLETEAPI */

#include "esp_log.h"
#include "esp_err.h"
#include "esp_pthread.h"
#include "esp_task_wdt.h"
#include "esp_heap_caps.h"
#ifdef ESPD_BOARD_WAVESHARE_S3
#include "boards/waveshare_s3/waveshare_s3_audio.h"
#include "boards/waveshare_s3/waveshare_s3_buttons.h"
#include "boards/waveshare_s3/waveshare_s3_leds.h"
#include "boards/waveshare_s3/waveshare_s3_sdcard.h"
#endif
#include "nvs.h"
#include "nvs_flash.h"
#include "esp_timer.h"
#include <stdlib.h>
#ifdef PD_USE_USB_MSC
#include "esp_partition.h"
#include "esp_vfs_fat.h"
#include "wear_levelling.h"
#include "tinyusb.h"
#include "tusb_msc_storage.h"
#include "tinyusb_default_config.h"
#endif
#ifdef PD_USE_ANALOG0
#include "esp_adc/adc_oneshot.h"
#include <stdatomic.h>
#endif
static const char *TAG = "ESPD";

#ifdef PD_USE_USB_MSC
static wl_handle_t wl_handle = WL_INVALID_HANDLE;
#endif

#if ESPD_AOUT_NUM_CHANNELS > 0
#include "driver/ledc.h"
#endif
#ifdef PD_USE_CONSOLE
#include "driver/uart.h"
#include "esp_console.h"
#endif

#if ESPD_AOUT_NUM_CHANNELS > 0
#define ESPD_AOUT_MAX_CHANNELS 4
#define ESPD_AOUT_PWM_RES LEDC_TIMER_12_BIT
#define ESPD_AOUT_PWM_MAX_DUTY ((1u << 12) - 1u)

typedef struct _espd_aout_receiver
{
    t_pd x_pd;
    int idx;
} t_espd_aout_receiver;

static t_class *espd_aout_receiver_class;
static int pd_aout_pins[ESPD_AOUT_MAX_CHANNELS] = {
    ESPD_AOUT_PIN_0, ESPD_AOUT_PIN_1, ESPD_AOUT_PIN_2, ESPD_AOUT_PIN_3
};
static int pd_aout_active[ESPD_AOUT_MAX_CHANNELS];
static t_espd_aout_receiver pd_aout_receivers[ESPD_AOUT_MAX_CHANNELS];

static void pd_aout_set_value(int idx, t_float f)
{
    uint32_t duty;
    if (idx < 0 || idx >= ESPD_AOUT_MAX_CHANNELS || !pd_aout_active[idx])
        return;
    if (f < 0.f)
        f = 0.f;
    else if (f > 1.f)
        f = 1.f;
    duty = (uint32_t)(f * (t_float)ESPD_AOUT_PWM_MAX_DUTY + 0.5f);
    ledc_set_duty(LEDC_LOW_SPEED_MODE, (ledc_channel_t)idx, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, (ledc_channel_t)idx);
}

static void espd_aout_receiver_float(t_espd_aout_receiver *x, t_floatarg f)
{
    pd_aout_set_value(x->idx, (t_float)f);
}

static void pd_aout_init(void)
{
    ledc_timer_config_t timer_cfg = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = ESPD_AOUT_PWM_RES,
        .timer_num = LEDC_TIMER_0,
        .freq_hz = ESPD_AOUT_PWM_FREQ_HZ,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    int i;
    int nchan = ESPD_AOUT_NUM_CHANNELS;
    int enabled = 0;

    if (nchan < 0)
        nchan = 0;
    if (nchan > ESPD_AOUT_MAX_CHANNELS)
        nchan = ESPD_AOUT_MAX_CHANNELS;

    for (i = 0; i < ESPD_AOUT_MAX_CHANNELS; i++)
        pd_aout_active[i] = 0;

    if (nchan == 0)
        return;

    if (ledc_timer_config(&timer_cfg) != ESP_OK)
    {
        timer_cfg.freq_hz = ESPD_AOUT_PWM_FALLBACK_FREQ_HZ;
        if (ledc_timer_config(&timer_cfg) != ESP_OK)
        {
            ESP_LOGE(TAG, "aout: LEDC timer init failed (freq=%d then %d)",
                     ESPD_AOUT_PWM_FREQ_HZ, ESPD_AOUT_PWM_FALLBACK_FREQ_HZ);
            return;
        }
        ESP_LOGW(TAG, "aout: %d Hz unavailable at %d-bit PWM; using %d Hz",
                 ESPD_AOUT_PWM_FREQ_HZ, 12, ESPD_AOUT_PWM_FALLBACK_FREQ_HZ);
    }

    for (i = 0; i < nchan; i++)
    {
        int pin = pd_aout_pins[i];
        if (pin < 0)
            continue;
        ledc_channel_config_t ch_cfg = {
            .gpio_num = pin,
            .speed_mode = LEDC_LOW_SPEED_MODE,
            .channel = (ledc_channel_t)i,
            .intr_type = LEDC_INTR_DISABLE,
            .timer_sel = LEDC_TIMER_0,
            .duty = 0,
            .hpoint = 0,
        };
        if (ledc_channel_config(&ch_cfg) != ESP_OK)
        {
            ESP_LOGW(TAG, "aout%d setup failed on GPIO%d", i, pin);
            continue;
        }
        pd_aout_active[i] = 1;
        enabled++;
        ESP_LOGI(TAG, "analog aout%d enabled on GPIO%d", i, pin);
    }

    if (!enabled)
    {
        ESP_LOGW(TAG, "aout enabled but no valid channels configured");
        return;
    }

    if (!espd_aout_receiver_class)
    {
        espd_aout_receiver_class = class_new(gensym("_espd_aout_receiver"),
            0, 0, sizeof(t_espd_aout_receiver), CLASS_PD, 0);
        class_addfloat(espd_aout_receiver_class, (t_method)espd_aout_receiver_float);
    }

    for (i = 0; i < nchan; i++)
    {
        char name[16];
        if (!pd_aout_active[i])
            continue;
        pd_aout_receivers[i].x_pd = espd_aout_receiver_class;
        pd_aout_receivers[i].idx = i;
        snprintf(name, sizeof(name), "espd/aout/%d", i);
        pd_bind((t_pd *)&pd_aout_receivers[i], gensym(name));
    }
}
#endif

#ifdef ESPD_BOARD_WAVESHARE_S3
/*
 * Pd glue for the WS2812 ring on the Waveshare S3 board.
 *
 *   [r espd/led]      list r g b           → fill all LEDs
 *                     list idx r g b       → set one LED
 *                     symbol "clear"/"off" → all off
 *   [r espd/led/N]    list r g b           → set LED N
 *
 * RGB values are 0..255 (clamped). After every message the strip is refreshed
 * so the change is visible immediately.
 */
#define ESPD_LED_MAX_RECEIVERS 16

typedef struct _espd_led_receiver
{
    t_pd x_pd;
    int idx;       /* -1 = master "espd/led", >=0 = per-LED "espd/led/N" */
} t_espd_led_receiver;

static t_class *espd_led_receiver_class;
static t_espd_led_receiver pd_led_master;
static t_espd_led_receiver pd_led_per[ESPD_LED_MAX_RECEIVERS];
static int pd_led_bound;

static int espd_led_clamp(t_float f)
{
    int v = (int)(f + 0.5f);
    if (v < 0) v = 0;
    else if (v > 255) v = 255;
#if ESPD_LED_MAX_BRIGHTNESS < 255
    /* Scale down proportionally so colors keep their hue while peak current
     * drawn by the WS2812 ring is reduced (cuts supply-rail coupling into
     * the audio codec). */
    v = (v * ESPD_LED_MAX_BRIGHTNESS) / 255;
    if (v > ESPD_LED_MAX_BRIGHTNESS) v = ESPD_LED_MAX_BRIGHTNESS;
#endif
    return v;
}

static void espd_led_receiver_list(t_espd_led_receiver *x,
    t_symbol *s, int argc, t_atom *argv)
{
    (void)s;
    if (x->idx >= 0) {
        /* Per-LED receiver: expect 3 floats r g b. */
        if (argc < 3) return;
        uint8_t r = espd_led_clamp(atom_getfloat(argv));
        uint8_t g = espd_led_clamp(atom_getfloat(argv + 1));
        uint8_t b = espd_led_clamp(atom_getfloat(argv + 2));
        espd_waveshare_s3_leds_set(x->idx, r, g, b);
        espd_waveshare_s3_leds_mark_dirty();
        return;
    }
    /* Master receiver. */
    if (argc == 3) {
        uint8_t r = espd_led_clamp(atom_getfloat(argv));
        uint8_t g = espd_led_clamp(atom_getfloat(argv + 1));
        uint8_t b = espd_led_clamp(atom_getfloat(argv + 2));
        espd_waveshare_s3_leds_fill(r, g, b);
        espd_waveshare_s3_leds_mark_dirty();
    } else if (argc >= 4) {
        int idx = (int)atom_getfloat(argv);
        uint8_t r = espd_led_clamp(atom_getfloat(argv + 1));
        uint8_t g = espd_led_clamp(atom_getfloat(argv + 2));
        uint8_t b = espd_led_clamp(atom_getfloat(argv + 3));
        espd_waveshare_s3_leds_set(idx, r, g, b);
        espd_waveshare_s3_leds_mark_dirty();
    }
}

static void espd_led_receiver_clear(t_espd_led_receiver *x)
{
    (void)x;
    espd_waveshare_s3_leds_clear();
}

static void pd_led_init(void)
{
    int i;
    int count = ESPD_WAVESHARE_LED_COUNT;
    if (count > ESPD_LED_MAX_RECEIVERS) count = ESPD_LED_MAX_RECEIVERS;
    if (pd_led_bound) return;

    if (!espd_led_receiver_class) {
        espd_led_receiver_class = class_new(gensym("_espd_led_receiver"),
            0, 0, sizeof(t_espd_led_receiver), CLASS_PD, 0);
        class_addlist(espd_led_receiver_class, (t_method)espd_led_receiver_list);
        class_addmethod(espd_led_receiver_class,
            (t_method)espd_led_receiver_clear, gensym("clear"), 0);
        class_addmethod(espd_led_receiver_class,
            (t_method)espd_led_receiver_clear, gensym("off"), 0);
    }

    /* Master "espd/led". */
    pd_led_master.x_pd = espd_led_receiver_class;
    pd_led_master.idx = -1;
    pd_bind((t_pd *)&pd_led_master, gensym("espd/led"));

    /* Per-LED "espd/led/N". */
    for (i = 0; i < count; i++) {
        char name[16];
        pd_led_per[i].x_pd = espd_led_receiver_class;
        pd_led_per[i].idx = i;
        snprintf(name, sizeof(name), "espd/led/%d", i);
        pd_bind((t_pd *)&pd_led_per[i], gensym(name));
    }
    pd_led_bound = 1;
}
/* Strong override of the weak stub in waveshare_s3_buttons.c: forward each
 * debounced press/release as a float to [r espd/din/<idx>]. */
void espd_button_state_changed(int idx, int pressed)
{
    char name[16];
    t_symbol *sym;
    t_pd *dest;
    snprintf(name, sizeof(name), "espd/din/%d", idx);
    sym = gensym(name);
    dest = sym ? sym->s_thing : NULL;
    if (dest)
        pd_float(dest, (t_float)(pressed ? 1 : 0));
}
#endif /* ESPD_BOARD_WAVESHARE_S3 */

#ifdef PD_USE_SDCARD
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <sys/stat.h>
static char *espd_cfg_trim(char *s)
{
    char *e;
    while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n')
        s++;
    e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '\n'))
        *--e = '\0';
    return s;
}
#endif
int espd_main_pd_loaded_from_store;
const char *espd_main_pd_loaded_dir;
#ifdef PD_USE_WIFI
int espd_wifi_net_enabled = 1;
static int espd_wifi_started;
char espd_wifi_ssid[33];
char espd_wifi_password[65];
int espd_wifi_force_enable = 0;

static void espd_wifi_config_defaults(void)
{
    snprintf(espd_wifi_ssid, sizeof(espd_wifi_ssid), "%s", CONFIG_ESP_WIFI_SSID);
    snprintf(espd_wifi_password, sizeof(espd_wifi_password), "%s", CONFIG_ESP_WIFI_PASSWORD);
    espd_wifi_force_enable = 0;
}

#ifdef PD_USE_SDCARD
static void espd_wifi_try_load_sdcard_config(void)
{
    FILE *f = fopen(ESPD_SDCARD_CONFIG_PATH, "r");
    char line[256];
    int ssid_from_file = 0;
    if (!f)
        return;
    while (fgets(line, sizeof(line), f))
    {
        char *eq;
        char *k;
        char *v;
        char *comment = strchr(line, '#');
        if (comment)
            *comment = '\0';
        k = espd_cfg_trim(line);
        if (*k == '\0')
            continue;
        eq = strchr(k, '=');
        if (!eq)
            continue;
        *eq++ = '\0';
        v = espd_cfg_trim(eq);
        k = espd_cfg_trim(k);
        if (!strcmp(k, "wifi_ssid"))
        {
            snprintf(espd_wifi_ssid, sizeof(espd_wifi_ssid), "%s", v);
            ssid_from_file = (espd_wifi_ssid[0] != '\0');
        }
        else if (!strcmp(k, "wifi_password"))
        {
            snprintf(espd_wifi_password, sizeof(espd_wifi_password), "%s", v);
        }
        else if (!strcmp(k, "wifi_enable"))
        {
            espd_wifi_force_enable = (atoi(v) != 0);
        }
    }
    fclose(f);
    if (ssid_from_file && !espd_wifi_force_enable)
        espd_wifi_force_enable = 1;
    ESP_LOGI(TAG, "loaded WiFi config from %s (ssid=%s, force=%d)",
             ESPD_SDCARD_CONFIG_PATH, espd_wifi_ssid, espd_wifi_force_enable);
}
#endif
#endif

#if defined(PD_USE_SDCARD) && defined(PD_USE_ANALOG0)
/* Parsed from /sdcard/config.txt before pd_analog0_init (see espd.h). */
static int s_analog_cfg_disable;
static int s_analog_cfg_have_pins;
static int s_analog_cfg_n;
static int s_analog_cfg_pins[8];

static void espd_analog_load_sdcard_config(void)
{
    FILE *f;
    char line[256];

    s_analog_cfg_disable = 0;
    s_analog_cfg_have_pins = 0;
    s_analog_cfg_n = 0;
    f = fopen(ESPD_SDCARD_CONFIG_PATH, "r");
    if (!f)
        return;
    while (fgets(line, sizeof(line), f))
    {
        char *eq, *k, *v;
        char *comment = strchr(line, '#');
        if (comment)
            *comment = '\0';
        k = espd_cfg_trim(line);
        if (*k == '\0')
            continue;
        eq = strchr(k, '=');
        if (!eq)
            continue;
        *eq++ = '\0';
        v = espd_cfg_trim(eq);
        k = espd_cfg_trim(k);
        if (!strcmp(k, "analog_enable"))
        {
            s_analog_cfg_disable = (atoi(v) == 0);
        }
        else if (!strcmp(k, "analog_pins"))
        {
            int n = 0;
            s_analog_cfg_have_pins = 1;
            if (!v || !*v)
            {
                s_analog_cfg_n = 0;
            }
            else
            {
                char *p = v;
                while (*p && n < 8)
                {
                    char *end;
                    long pin = strtol(p, &end, 10);
                    if (p == end)
                        break;
                    s_analog_cfg_pins[n++] = (int)pin;
                    p = end;
                    while (*p == ',' || *p == ' ' || *p == '\t')
                        p++;
                }
                s_analog_cfg_n = n;
            }
        }
    }
    fclose(f);
    if (s_analog_cfg_disable)
    {
        ESP_LOGI(TAG, "analog: %s: analog_enable=0", ESPD_SDCARD_CONFIG_PATH);
    }
    else if (s_analog_cfg_have_pins)
    {
        if (s_analog_cfg_n == 0)
            ESP_LOGI(TAG, "analog: %s: analog_pins= (off)", ESPD_SDCARD_CONFIG_PATH);
        else
            ESP_LOGI(TAG, "analog: %s: %d channel(s) from analog_pins", ESPD_SDCARD_CONFIG_PATH, s_analog_cfg_n);
    }
}
#endif

#ifdef PD_USE_ANALOG0
static adc_oneshot_unit_handle_t pd_adc_handle;
#define ESPD_ANALOG_MAX_CHANNELS 8
static int pd_adc_pins[ESPD_ANALOG_MAX_CHANNELS] = {
    ESPD_ANALOG_PIN_0, ESPD_ANALOG_PIN_1, ESPD_ANALOG_PIN_2, ESPD_ANALOG_PIN_3,
    ESPD_ANALOG_PIN_4, ESPD_ANALOG_PIN_5, ESPD_ANALOG_PIN_6, ESPD_ANALOG_PIN_7
};
static adc_channel_t pd_adc_channels[ESPD_ANALOG_MAX_CHANNELS];
static int pd_adc_active[ESPD_ANALOG_MAX_CHANNELS];
/* Producer-side deadband reference (written only by the ADC task). */
static int pd_adc_last[ESPD_ANALOG_MAX_CHANNELS];
/* Producer -> consumer handoff. The ADC task writes `pd_adc_latest` then bumps
 * `pd_adc_seq` with release ordering; the audio-thread consumer reads seq with
 * acquire ordering, then latest, guaranteeing it never sees a torn value.
 * Atomics are used because producer (core 0) and consumer (core 1) live on
 * different cores. */
static _Atomic int pd_adc_latest[ESPD_ANALOG_MAX_CHANNELS];
static _Atomic uint32_t pd_adc_seq[ESPD_ANALOG_MAX_CHANNELS];
/* Consumer-side last-seen sequence number (audio thread only). */
static uint32_t pd_adc_last_sent_seq[ESPD_ANALOG_MAX_CHANNELS];
static TaskHandle_t pd_adc_task;
static unsigned pd_analog_block_counter;

static void pd_send_ain_value(int idx, int raw)
{
    char name[16];
    t_symbol *sym;
    t_pd *dest;
    t_float v;
    snprintf(name, sizeof(name), "espd/ain/%d", idx);
    sym = gensym(name);
    dest = sym ? sym->s_thing : NULL;
    v = (t_float)raw * (1.0f / 4095.0f);
    if (v < 0)
        v = 0;
    else if (v > 1)
        v = 1;
    if (dest)
        pd_float(dest, v);
}

static int pd_pin_to_adc1_channel(int pin, adc_channel_t *channel)
{
    switch (pin)
    {
    case 1:  *channel = ADC_CHANNEL_0; return 1;
    case 2:  *channel = ADC_CHANNEL_1; return 1;
    case 3:  *channel = ADC_CHANNEL_2; return 1;
    case 4:  *channel = ADC_CHANNEL_3; return 1;
    case 5:  *channel = ADC_CHANNEL_4; return 1;
    case 6:  *channel = ADC_CHANNEL_5; return 1;
    case 7:  *channel = ADC_CHANNEL_6; return 1;
    case 8:  *channel = ADC_CHANNEL_7; return 1;
    case 9:  *channel = ADC_CHANNEL_8; return 1;
    case 10: *channel = ADC_CHANNEL_9; return 1;
#if CONFIG_IDF_TARGET_ESP32
    case 36: *channel = ADC_CHANNEL_0; return 1;
    case 37: *channel = ADC_CHANNEL_1; return 1;
    case 38: *channel = ADC_CHANNEL_2; return 1;
    case 39: *channel = ADC_CHANNEL_3; return 1;
    case 32: *channel = ADC_CHANNEL_4; return 1;
    case 33: *channel = ADC_CHANNEL_5; return 1;
    case 34: *channel = ADC_CHANNEL_6; return 1;
    case 35: *channel = ADC_CHANNEL_7; return 1;
#endif
    default: return 0;
    }
}

/* Producer: runs on a dedicated task (core 0). Reads every active channel at
 * a fixed cadence, applies the deadband, and — on each change — publishes the
 * new raw value + bumps the per-channel sequence number. Never calls into Pd. */
static void pd_adc_task_fn(void *arg)
{
    (void)arg;
    const TickType_t period = pdMS_TO_TICKS(ESPD_ANALOG_TASK_PERIOD_MS);
    TickType_t next = xTaskGetTickCount();
    for (;;)
    {
        int i;
        vTaskDelayUntil(&next, period > 0 ? period : 1);
        if (!pd_adc_handle)
            continue;
        for (i = 0; i < ESPD_ANALOG_MAX_CHANNELS; i++)
        {
            int raw;
            if (!pd_adc_active[i])
                continue;
            if (adc_oneshot_read(pd_adc_handle, pd_adc_channels[i], &raw)
                    != ESP_OK)
                continue;
            if (raw < pd_adc_last[i] - ESPD_ANALOG_DEADBAND ||
                raw > pd_adc_last[i] + ESPD_ANALOG_DEADBAND)
            {
                pd_adc_last[i] = raw;
                /* Publish new raw value first (relaxed — consumer will pair
                 * it with the acquire-load of seq below), then release the
                 * sequence bump so the consumer sees a consistent snapshot. */
                atomic_store_explicit(&pd_adc_latest[i], raw,
                    memory_order_relaxed);
                atomic_fetch_add_explicit(&pd_adc_seq[i], 1u,
                    memory_order_release);
            }
        }
    }
}

static void pd_analog0_init(void)
{
    adc_oneshot_unit_init_cfg_t unit_cfg = {
        .unit_id = ADC_UNIT_1,
        .ulp_mode = ADC_ULP_MODE_DISABLE,
    };
    adc_oneshot_chan_cfg_t chan_cfg = {
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    int i;
    int enabled = 0;
    int nchan;
#if defined(PD_USE_SDCARD) && defined(PD_USE_ANALOG0)
    if (s_analog_cfg_disable) {
        ESP_LOGI(TAG, "analog: not started (analog_enable=0 in config.txt)");
        return;
    }
    if (s_analog_cfg_have_pins) {
        if (s_analog_cfg_n <= 0) {
            ESP_LOGI(TAG, "analog: not started (analog_pins= empty in config.txt)");
            return;
        }
        nchan = s_analog_cfg_n;
        if (nchan > ESPD_ANALOG_MAX_CHANNELS)
            nchan = ESPD_ANALOG_MAX_CHANNELS;
        for (i = 0; i < nchan; i++)
            pd_adc_pins[i] = s_analog_cfg_pins[i];
        for (i = nchan; i < ESPD_ANALOG_MAX_CHANNELS; i++)
            pd_adc_pins[i] = -1;
    } else
#endif
    {
        nchan = ESPD_ANALOG_NUM_CHANNELS;
    }
    if (nchan < 0)
        nchan = 0;
    if (nchan > ESPD_ANALOG_MAX_CHANNELS)
        nchan = ESPD_ANALOG_MAX_CHANNELS;
    esp_err_t err = adc_oneshot_new_unit(&unit_cfg, &pd_adc_handle);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "adc_oneshot_new_unit failed: %d", (int)err);
        return;
    }

    for (i = 0; i < ESPD_ANALOG_MAX_CHANNELS; i++)
    {
        pd_adc_active[i] = 0;
        pd_adc_last[i] = -100000;
        atomic_store_explicit(&pd_adc_latest[i], 0, memory_order_relaxed);
        atomic_store_explicit(&pd_adc_seq[i], 0u, memory_order_relaxed);
        pd_adc_last_sent_seq[i] = 0u;
    }

    for (i = 0; i < nchan; i++)
    {
        int pin = pd_adc_pins[i];
        adc_channel_t ch;
        if (pin < 0)
            continue;
        if (!pd_pin_to_adc1_channel(pin, &ch))
        {
            ESP_LOGW(TAG, "analog ain%d ignored: GPIO%d is not ADC1-capable", i, pin);
            continue;
        }
        err = adc_oneshot_config_channel(pd_adc_handle, ch, &chan_cfg);
        if (err != ESP_OK)
        {
            ESP_LOGW(TAG, "analog ain%d setup failed on GPIO%d: %d", i, pin, (int)err);
            continue;
        }
        pd_adc_channels[i] = ch;
        pd_adc_active[i] = 1;
        enabled++;
        ESP_LOGI(TAG, "analog ain%d enabled on GPIO%d", i, pin);
    }
    if (!enabled) {
        ESP_LOGW(TAG, "analog enabled but no valid channels configured");
        return;
    }

    /* Spawn the producer task on the opposite core from Pd audio (core 1), so
     * adc_oneshot_read() blocking never steals time from senddacs(). */
    if (!pd_adc_task) {
        BaseType_t ok = xTaskCreatePinnedToCore(pd_adc_task_fn,
            "pd_adc", 3072, NULL, ESPD_ANALOG_TASK_PRIO,
            &pd_adc_task, ESPD_ANALOG_TASK_CORE);
        if (ok != pdPASS) {
            ESP_LOGW(TAG, "failed to create pd_adc task; falling back to audio-thread polling");
            pd_adc_task = NULL;
        } else {
            ESP_LOGI(TAG, "pd_adc task: core=%d prio=%d period=%d ms, %d channel(s)",
                (int)ESPD_ANALOG_TASK_CORE, (int)ESPD_ANALOG_TASK_PRIO,
                (int)ESPD_ANALOG_TASK_PERIOD_MS, enabled);
        }
    }
}

/* Consumer: runs on the audio thread each loop iteration. Cheap per-channel
 * seq compare; only touches Pd when the producer has published a new value.
 * ADC reads themselves happen on pd_adc_task, so no blocking hits the block
 * budget. ESPD_ANALOG_REPORT_EVERY_N_BLOCKS remains as a belt-and-braces
 * rate limiter in case the producer period is ever set very low. */
static void pd_pollanalog0(void)
{
    int i;
    int report_every = ESPD_ANALOG_REPORT_EVERY_N_BLOCKS;
    if (!pd_adc_handle)
        return;
    if (report_every < 1)
        report_every = 1;
    if (++pd_analog_block_counter < (unsigned)report_every)
        return;
    pd_analog_block_counter = 0;

    /* Fallback: if the producer task failed to start, poll inline so ain
     * channels still work (same behavior as before). */
    if (!pd_adc_task)
    {
        for (i = 0; i < ESPD_ANALOG_MAX_CHANNELS; i++)
        {
            int raw;
            if (!pd_adc_active[i])
                continue;
            if (adc_oneshot_read(pd_adc_handle, pd_adc_channels[i], &raw)
                    != ESP_OK)
                continue;
            if (raw < pd_adc_last[i] - ESPD_ANALOG_DEADBAND ||
                raw > pd_adc_last[i] + ESPD_ANALOG_DEADBAND)
            {
                pd_adc_last[i] = raw;
                pd_send_ain_value(i, raw);
            }
        }
        return;
    }

    for (i = 0; i < ESPD_ANALOG_MAX_CHANNELS; i++)
    {
        uint32_t s;
        int raw;
        if (!pd_adc_active[i])
            continue;
        /* Acquire-load pairs with the producer's release bump so the raw read
         * below cannot be reordered before the seq observation. */
        s = atomic_load_explicit(&pd_adc_seq[i], memory_order_acquire);
        if (s == pd_adc_last_sent_seq[i])
            continue;
        raw = atomic_load_explicit(&pd_adc_latest[i], memory_order_relaxed);
        pd_adc_last_sent_seq[i] = s;
        pd_send_ain_value(i, raw);
    }
}
#endif

#ifdef PD_USE_USB_MSC
static int usb_msc_active = 0;

static void usb_init(void)
{
    ESP_LOGI(TAG, "USB MSC init starting");
    // 1. Find the partition named "storage" from your partitions_pd.csv
    const esp_partition_t *data_partition = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_FAT, "storage");

    if (!data_partition) {
        ESP_LOGE(TAG, "USB MSC: 'storage' partition not found");
        return;
    }

    // 2. Mount wear leveling on that partition
    ESP_ERROR_CHECK(wl_mount(data_partition, &wl_handle));

    //tinyusb_msc_register_callback(TINYUSB_MSC_EVENT_MOUNT_CHANGED, storage_mount_changed_cb); 

    // 3. Configure TinyUSB MSC to use this flash handle
    const tinyusb_msc_spiflash_config_t msc_config = {
        .wl_handle = wl_handle
    };
    ESP_ERROR_CHECK(tinyusb_msc_storage_init_spiflash(&msc_config));

    // 4. Install TinyUSB driver
    tinyusb_config_t tusb_cfg = TINYUSB_DEFAULT_CONFIG();
    ESP_ERROR_CHECK(tinyusb_driver_install(&tusb_cfg));

    const esp_vfs_fat_mount_config_t mount_config = {
        .max_files = 64,
        .format_if_mount_failed = true, // <--- This does the formatting
        .allocation_unit_size = CONFIG_WL_SECTOR_SIZE,
        .use_one_fat = false,
    };

    // This handles finding the partition, initializing wear leveling,
    // and mounting (or formatting then mounting)
    esp_err_t err = esp_vfs_fat_spiflash_mount_rw_wl("/storage", "storage", &mount_config, &wl_handle);

    if (err == ESP_OK) {
        FRESULT res = f_setlabel("0:ESPD");
        ESP_LOGI(TAG, "USB MSC: partition ready at /storage");
        usb_msc_active = 1;
    } else {
        ESP_LOGE(TAG, "USB MSC: failed to mount /storage: %s", esp_err_to_name(err));
    }
}
#endif

static void espd_nvs_flash_init(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
}

extern void pdmain_tick( void);
void pdmain_init( void);


void sd_init( void);
void espd_control_io_init(void);

#ifndef OBSOLETEAPI
static i2s_chan_handle_t tx_handle;
#ifdef USEADC
static i2s_chan_handle_t rx_handle;
#endif
#endif /* OBSOLETEAPI */

#define BLKSIZE 64
float soundin[IOCHANS * BLKSIZE], soundout[IOCHANS * BLKSIZE];
static uint64_t s_rt_blocks;
static uint64_t s_rt_overruns;
static uint32_t s_rt_max_us;

void senddacs( void)
{
    int i, j, ret;
    size_t transferred;
    short poodle[IOCHANS * BLKSIZE];

    for (i = j = 0; i < BLKSIZE; i++, j += IOCHANS)
    {
        int ch1 = floor(0.5 + 32768.*soundout[i]);
#if IOCHANS > 1
        int ch2 = floor(0.5 + 32768.*soundout[i+BLKSIZE]);
#endif
        if (ch1 > 32767)
            ch1 = 32767;
        else if (ch1 < -32768)
            ch1 = -32768;
        ch1 &= 0xffff;
#if IOCHANS > 1
        if (ch2 > 32767)
            ch2 = 32767;
        else if (ch2 < -32768)
            ch2 = -32768;
        ch2 &= 0xffff;
#endif
        poodle[j] = ch1;
        soundout[i] = 0;
#if IOCHANS > 1
        poodle[j+1] = ch2;
        soundout[i+BLKSIZE] = 0;
#endif
    }
#ifdef OBSOLETEAPI
    ret = i2s_write(I2S_NUM_0, poodle, sizeof(poodle), &transferred,
        portMAX_DELAY);
    if (ret != ESP_OK)
        ESP_LOGE(TAG, "error writing");
#else
#if defined(ESPD_BOARD_WAVESHARE_S3)
    if (espd_waveshare_s3_codec_write(poodle, (int)sizeof(poodle)) != 0)
        ESP_LOGE(TAG, "waveshare codec write failed");
#ifndef USEADC
    (void)transferred;
#endif
#else
    i2s_channel_write(tx_handle, poodle, sizeof(poodle), &transferred,
        portMAX_DELAY);
#endif
#endif

#ifdef USEADC
#ifdef OBSOLETEAPI
    ret = i2s_read(I2S_NUM_0, poodle, sizeof(poodle), &transferred,
        portMAX_DELAY);
    if (ret != ESP_OK)
        ESP_LOGE(TAG, "error reading");
#else
#if defined(ESPD_BOARD_WAVESHARE_S3)
    if (espd_waveshare_s3_codec_read(poodle, (int)sizeof(poodle)) != 0) {
        i2s_channel_read(rx_handle, poodle, sizeof(poodle), &transferred,
            portMAX_DELAY);
    }
#else
    i2s_channel_read(rx_handle, poodle, sizeof(poodle), &transferred,
        portMAX_DELAY);
#endif
#endif
    
    for (i = j = 0; i < BLKSIZE; i++, j += IOCHANS)
    {
        uint32_t ch1 = poodle[j] & 0xffff;
#if IOCHANS > 1
        uint32_t ch2 = poodle[j+1] & 0xffff;
        if (ch2 & 0x8000)
            soundin[i+BLKSIZE] = (ch2*(1./32768.)) - 2;
        else soundin[i+BLKSIZE] = (ch2*(1./32768.));
#endif
        if (ch1 & 0x8000)
            soundin[i] = (ch1*(1./32768.)) - 2;
        else soundin[i] = (ch1*(1./32768.));
    }
#endif /* USEADC */
}

#ifdef OBSOLETEAPI
    /* allow deprecated form if new one unavailable */
#ifndef I2S_COMM_FORMAT_STAND_I2S
#define I2S_COMM_FORMAT_STAND_I2S I2S_COMM_FORMAT_I2S
#endif

static void initdacs( void)
{
    i2s_config_t i2s_config =
    {
        .mode = (I2S_MODE_MASTER | I2S_MODE_TX
#ifdef USEADC
            | I2S_MODE_RX
#endif
            ),
        .sample_rate = 48000,
        .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
#if IOCHANS > 1
        .channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT,
#else
        .channel_format = I2S_CHANNEL_FMT_ONLY_RIGHT,
#endif
        .communication_format = I2S_COMM_FORMAT_STAND_I2S,
        .dma_buf_count = 4,
        .dma_buf_len = 256,
#ifdef PD_LYRAT
        .use_apll=1,
        .intr_alloc_flags = ESP_INTR_FLAG_LEVEL2,
#else
        .use_apll=0,
        .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1, /* high interrupt priority */
        .tx_desc_auto_clear= true, 
        .fixed_mclk=-1
 #endif
   };

    ESP_LOGI(TAG, "[ 1 ] Start audio codec chip");

#ifdef PD_LYRAT
    audio_board_handle_t board_handle = audio_board_init();
    audio_hal_ctrl_codec(board_handle->audio_hal,
        AUDIO_HAL_CODEC_MODE_BOTH, AUDIO_HAL_CTRL_START);
    audio_hal_set_volume(board_handle->audio_hal, 100);
#endif

    i2s_driver_install(I2S_NUM_0, &i2s_config, 0, NULL);

    {
#ifdef PD_LYRAT
        i2s_pin_config_t i2s_pin_cfg;
        get_i2s_pins(I2S_NUM_0, (board_i2s_pin_t *)(&i2s_pin_cfg));
#else /* PD_LYRAT */
        i2s_pin_config_t i2s_pin_cfg = {
        .bck_io_num = PIN_BIT_CLOCK,    /* bit clock */
        .ws_io_num = PIN_WORD_SELECT,   /* Word select, aka left right clock */
        .data_out_num = PIN_DATA_OUT,   /* Data out ESP32 - to DIN on 38357A */
        .data_in_num = PIN_DATA_IN      /* data from ADC */
        };
#endif /* PD_LYRAT */
        i2s_set_pin(I2S_NUM_0, &i2s_pin_cfg);
    }               
}
#else /* OBSOLETEAPI */

static void initdacs( void)
{
#if defined(ESPD_BOARD_WAVESHARE_S3)
    esp_err_t e = espd_waveshare_s3_audio_init(&tx_handle, &rx_handle);
    if (e != ESP_OK)
        ESP_LOGE(TAG, "waveshare audio init failed: %s", esp_err_to_name(e));
    return;
#endif
    /* Get the default channel configuration by the helper macro.
    * This helper macro is defined in `i2s_common.h` and shared by all the I2S
     communication modes. */
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0,
        I2S_ROLE_MASTER);
    /* Allocate a new TX channel and get the handle of this channel */
#ifdef USEADC
    i2s_new_channel(&chan_cfg, &tx_handle, &rx_handle);
#else
    i2s_new_channel(&chan_cfg, &tx_handle, NULL);
#endif
    /* Setting the configurations, the slot configuration and clock configuration
        can be generated by the macros defined in `i2s_std.h` which can only be
        used in STD mode. They can help to specify the slot and clock
        configurations for initialization or updating */
    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(48000),
        .slot_cfg = I2S_STD_MSB_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
            I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = PIN_BIT_CLOCK,
            .ws = PIN_WORD_SELECT,
            .dout = PIN_DATA_OUT,
            .din = PIN_DATA_IN,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };
    /* Initialize the channel */
    i2s_channel_init_std_mode(tx_handle, &std_cfg);

    /* Before writing data, start the TX channel first */
    i2s_channel_enable(tx_handle);
#ifdef USEADC
    i2s_channel_init_std_mode(rx_handle, &std_cfg);
    i2s_channel_enable(rx_handle);
#endif
}
#endif /* OBSOLETEAPI */

static int audiostate;

void sys_set_audio_state(int onoff)
{
/*
    if (onoff && !audiostate)
        i2s_start(I2S_NUM_0);
    else if (!onoff && audiostate)
        i2s_stop(I2S_NUM_0);
*/
    audiostate = onoff;
}


/* queue from host.  Need to make this a proper RTOS queue */
void *getbytes(size_t nbytes);
void freebytes(void *x, size_t nbytes);
void *resizebytes(void *x, size_t oldsize, size_t newsize);
static char *pd_bt_buf;
static int pd_bt_size;
static SemaphoreHandle_t pd_bt_mutex;
#include <ctype.h>

    /* enqueue a message from host to Pd */
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

#ifdef PD_USE_CONSOLE
static QueueHandle_t uart_queue;
static void console_init( void)
{
    (void)uart_queue;
    /* On ESP32-S3/IDF6 this app already has active console I/O via ROM/monitor.
     * Installing another UART driver here has caused boot-time crashes.
     * Keep console output via printf/pdmain_print, but disable host->Pd UART input. */
}
#endif

    /* dispatch messages enqueued above */
void pd_pollhost( void)
{
    int lastchar;
#ifdef PD_USE_CONSOLE
    /* Host->Pd UART input disabled (see console_init comment above). */
#endif
    if (!pd_bt_mutex)
        pd_bt_mutex = xSemaphoreCreateMutex();
    if (xSemaphoreTake(pd_bt_mutex, 0) != pdTRUE)
        return;

        /* only interpret this text if terminated by a semicolon */
    lastchar = pd_bt_size-1;
    while (lastchar >= 0 &&  isspace((int)(pd_bt_buf[lastchar])))
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

   /* this doesn't work as a printhook yet since posts are split into atoms */
void pdmain_print( const char *s)
{
    char y[81];
    if (s && *s)
        printf("%s", s);
    strncpy(y, s, 79);
    y[79]=0;
    strcat(y, ";");
#ifdef PD_USE_BLUETOOTH
    if (strlen(y) > 0)
        pd_bt_writeback((unsigned char *)y, strlen(y));
#endif
#ifdef PD_USE_WIFI
    if (espd_wifi_net_enabled && espd_net_send_ready()) {
        net_sendudp(y, strlen(y), CONFIG_ESP_WIFI_SENDPORT);
        net_sendtcp(y, strlen(y));
    }
#endif
}

void trymem(int foo);

static unsigned int cputime;
void espd_cputime_reset( void)
{
    cputime = 0;
}

unsigned int espd_cputime_get( void)
{
    return (cputime);
}

void app_main(void)
{
    esp_log_level_set("*", ESP_LOG_WARN);
    esp_log_level_set(TAG, ESP_LOG_INFO);

    /* Prefer external RAM for generic malloc as early as possible to reduce
     * internal heap pressure during heavy Pd abstraction/clone creation. */
    heap_caps_malloc_extmem_enable(0);

    espd_nvs_flash_init();
    espd_patch_store_init();
#ifdef PD_USE_WIFI
    espd_wifi_config_defaults();
#endif

    {
        /* readsf~/writesf~ use pthread worker tasks. Raise defaults early
         * so patch-driven pthread_create() gets a safer stack/core profile. */
        esp_pthread_cfg_t pth_cfg = esp_pthread_get_default_config();
        pth_cfg.stack_size = 8192;
        pth_cfg.prio = 5;
        pth_cfg.pin_to_core = 0;
        pth_cfg.stack_alloc_caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
        if (esp_pthread_set_cfg(&pth_cfg) != ESP_OK)
            ESP_LOGW(TAG, "esp_pthread_set_cfg failed; using IDF defaults");
    }

#ifdef ESPD_BOARD_WAVESHARE_S3
    /* Light the on-board WS2812 ring early so we can confirm boot visually,
     * even before WiFi/Pd come up. Failures are non-fatal. */
    espd_waveshare_s3_leds_init();
#endif

#ifdef PD_USE_USB_MSC
    // Initialize USB MSC storage
    usb_init();

    // should wait until USB drive is unmounted, but doesn't
    if (tud_connected()) {
        vTaskDelay(pdMS_TO_TICKS(500));
        while (tud_mounted()) {
            vTaskDelay(pdMS_TO_TICKS(10));  // Check every 10ms
            tud_task();
        }
    }
#endif

#if defined(PD_USE_SDCARD) && defined(ESPD_BOARD_WAVESHARE_S3)
    /* Mount SD before Pd so main.pd can load from ESPD_SDCARD_MOUNT. */
    {
        esp_err_t e = espd_waveshare_s3_sdcard_mount();
        if (e != ESP_OK)
            ESP_LOGW(TAG, "SD card not mounted at boot: %s", esp_err_to_name(e));
    }
#endif

#if defined(PD_USE_SDCARD) && defined(PD_USE_ANALOG0)
    espd_analog_load_sdcard_config();
#endif
#if defined(PD_USE_WIFI) && defined(PD_USE_SDCARD)
    espd_wifi_try_load_sdcard_config();
#endif

#ifdef PD_USE_WIFI
#if !ESPD_ENABLE_LEGACY_WIFI_TRANSPORT
    {
        int local_main_present = 0;
#ifdef PD_USE_USB_MSC
        if (espd_storage_main_pd_exists())
            local_main_present = 1;
#endif
#ifdef PD_USE_SDCARD
        if (espd_sdcard_main_pd_exists())
            local_main_present = 1;
#endif
        if (!local_main_present && espd_patch_store_main_pd_exists())
            local_main_present = 1;
        if (local_main_present && ESPD_SKIP_WIFI_WHEN_MAIN_PD_ON_DISK &&
            !espd_wifi_force_enable) {
            espd_wifi_net_enabled = 0;
            /* CONFIG_LOG_DEFAULT_LEVEL is often WARN on waveshare_s3; printf matches sdcard. */
            printf("wifi: skipped (main.pd on disk; set wifi_enable=1 or wifi_ssid in"
                   " " ESPD_SDCARD_CONFIG_PATH " to force STA)\n");
            ESP_LOGI(TAG,
                     "main.pd detected on disk — skipping WiFi before Pd init");
        } else {
            espd_wifi_net_enabled = 1;
            ESP_LOGI(TAG, "[ 1a ] start network (early for Pd net objects)");
            wifi_init();
            espd_wifi_started = 1;
        }
    }
#endif
#endif

    initdacs();
#ifdef PD_USE_WIFI
    /* Bring up lwIP + default event loop unconditionally before Pd loads the
     * patch. If the patch contains [netreceive]/[netsend] but Wi-Fi has been
     * skipped (e.g. wifi_enable=0 or no AP), socket() would otherwise call
     * into the tcpip thread before it exists and abort() inside lwIP. This is
     * idempotent and a no-op if wifi_init() already ran above. */
    espd_netif_ensure_init();
#endif
    pdmain_init();
#ifdef PD_USE_ANALOG0
    pd_analog0_init();
#endif

#ifdef PD_USE_BLUETOOTH
    bt_init();
#endif
#ifdef PD_USE_SDCARD
    sd_init();
#endif
#ifdef PD_USE_WIFI
    if (espd_main_pd_loaded_from_store && ESPD_SKIP_WIFI_WHEN_MAIN_PD_ON_DISK &&
        !espd_wifi_force_enable) {
        espd_wifi_net_enabled = 0;
        ESP_LOGI(TAG,
                 "main.pd loaded from %s — skipping WiFi and TCP/UDP patch transport",
                 espd_main_pd_loaded_dir ? espd_main_pd_loaded_dir : ESPD_PATCH_STORE_MOUNT);
    } else {
        espd_wifi_net_enabled = 1;
        if (!espd_wifi_started) {
            ESP_LOGI(TAG, "[ 1a ] start network");
            wifi_init();
            espd_wifi_started = 1;
        }
#if ESPD_ENABLE_LEGACY_WIFI_TRANSPORT
        net_init();
        net_hello();
#else
        ESP_LOGI(TAG, "legacy espd TCP/UDP transport disabled (use Pd net objects)");
#endif
    }
#endif
#ifdef PD_USE_CONSOLE
    console_init();
#endif

    ESP_LOGI(TAG, "[ 2 ] now write some shit");

    while (1)
    {
        static unsigned loop_count = 0;
        /*
            int zz = 0;
            if (!((zz++)%1000))
            {
                trymem(5);
                ESP_LOGI(TAG, "tick");
            }
        */
        pd_pollhost();
#ifdef PD_USE_ANALOG0
        pd_pollanalog0();
#endif

#ifdef ESPD_BOARD_WAVESHARE_S3
        espd_waveshare_s3_buttons_poll();
        espd_waveshare_s3_leds_poll();
#endif
        {
            uint64_t t0 = (uint64_t)esp_timer_get_time();
            uint32_t sr = (uint32_t)sys_getsr();
            uint32_t budget_us = (sr > 0) ? (uint32_t)((1000000ULL * BLKSIZE) / sr) : 0;
            uint64_t dt;

            pdmain_tick();
            cputime += ((uint64_t)esp_timer_get_time() - t0);
            senddacs();

            dt = (uint64_t)esp_timer_get_time() - t0;
            s_rt_blocks++;
            if ((uint32_t)dt > s_rt_max_us)
                s_rt_max_us = (uint32_t)dt;
            if (budget_us > 0 && dt > budget_us)
                s_rt_overruns++;
        }
#ifdef PD_USE_WIFI
#if ESPD_ENABLE_LEGACY_WIFI_TRANSPORT
        if (espd_wifi_net_enabled)
            net_alive();
#endif
#endif
        /* Avoid starving IDLE0 under heavy message/network traffic.
         * Keep this sparse to minimize audio scheduling jitter. */
        if ((++loop_count & 0x1FF) == 0)
            vTaskDelay(1);
    }
}

void espd_control_io_init(void)
{
#if ESPD_AOUT_NUM_CHANNELS > 0
    pd_aout_init();
#endif
#ifdef ESPD_BOARD_WAVESHARE_S3
    pd_led_init();
#endif
}

#ifdef PD_USE_SDCARD
void sd_init( void)
{
    /* initialize SD card */
    ESP_LOGI(TAG, "[ 1 ] Mount sdcard");
#ifdef PD_LYRAT
    /* LyraT path: mount via ADF board helper. */
    esp_periph_config_t periph_cfg = DEFAULT_ESP_PERIPH_SET_CONFIG();
    esp_periph_set_handle_t set = esp_periph_set_init(&periph_cfg);
    audio_board_sdcard_init(set, SD_MODE_1_LINE);
#elif defined(ESPD_BOARD_WAVESHARE_S3)
    {
        esp_err_t e = espd_waveshare_s3_sdcard_mount();
        if (e != ESP_OK)
            ESP_LOGW(TAG, "SD mount: %s", esp_err_to_name(e));
    }
#else
    ESP_LOGW(TAG, "SD init not implemented for this board; checking %s only",
             ESPD_SDCARD_MOUNT);
#endif
    ESP_LOGI(TAG, "[ 1b ] done starting network");
}
#endif

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

static void espd_print_cpudiag(void)
{
#if (ESPD_ENABLE_CPU_STATS == 1) && (configGENERATE_RUN_TIME_STATS == 1)
    UBaseType_t ntasks = uxTaskGetNumberOfTasks();
    TaskStatus_t *tasks;
    uint32_t total = 0;
    uint64_t idle_now = 0;
    uint64_t total_now = 0;
    static uint64_t s_prev_idle;
    static uint64_t s_prev_total;
    uint64_t rt_blocks = s_rt_blocks;
    uint64_t rt_overruns = s_rt_overruns;
    uint32_t rt_max_us = s_rt_max_us;
    uint32_t sr = (uint32_t)sys_getsr();
    uint32_t budget_us = (sr > 0) ? (uint32_t)((1000000ULL * BLKSIZE) / sr) : 0;
    static uint64_t s_prev_diag_us;
    static unsigned s_busy_ema_x10;
    uint64_t now_us = (uint64_t)esp_timer_get_time();
    uint64_t elapsed_us = (s_prev_diag_us > 0 && now_us > s_prev_diag_us) ?
        (now_us - s_prev_diag_us) : 0;
    char msg[240];
    UBaseType_t i;

    s_rt_blocks = 0;
    s_rt_overruns = 0;
    s_rt_max_us = 0;

    tasks = pvPortMalloc(ntasks * sizeof(TaskStatus_t));
    if (!tasks)
    {
        pdmain_print("cpu: no memory for task stats\n");
        return;
    }

    ntasks = uxTaskGetSystemState(tasks, ntasks, &total);
    for (i = 0; i < ntasks; i++)
    {
        total_now += (uint64_t)tasks[i].ulRunTimeCounter;
        if (!strncmp(tasks[i].pcTaskName, "IDLE", 4))
            idle_now += (uint64_t)tasks[i].ulRunTimeCounter;
    }
    vPortFree(tasks);

    if (total_now > s_prev_total && idle_now >= s_prev_idle)
    {
        uint64_t d_total = total_now - s_prev_total;
        uint64_t d_idle = idle_now - s_prev_idle;
        if (d_idle > d_total)
            d_idle = d_total;
        unsigned idle_x10 = (unsigned)((d_idle * 1000ULL) / d_total);
        if (idle_x10 > 1000U)
            idle_x10 = 1000U;
        unsigned busy_x10 = 1000U - idle_x10;
        unsigned rt_over_x10 = 0;
        unsigned xruns_per_s_x10 = 0;
        /* Low-pass the load indicator for readability. */
        if (s_busy_ema_x10 == 0)
            s_busy_ema_x10 = busy_x10;
        else
            s_busy_ema_x10 = (s_busy_ema_x10 * 3U + busy_x10) / 4U;
        if (rt_blocks > 0)
            rt_over_x10 = (unsigned)((rt_overruns * 1000ULL) / rt_blocks);
        if (elapsed_us > 0)
            xruns_per_s_x10 = (unsigned)((rt_overruns * 10000000ULL) / elapsed_us);
        snprintf(msg, sizeof(msg), "rt: xruns=%u.%u/s over=%u.%u%%\n",
                 xruns_per_s_x10 / 10, xruns_per_s_x10 % 10,
                 rt_over_x10 / 10, rt_over_x10 % 10);
    }
    else if (total > 0)
    {
        /* First sample (or counter reset) has no valid delta yet. */
        snprintf(msg, sizeof(msg), "cpu: sampling...\n");
    }
    else
    {
        snprintf(msg, sizeof(msg), "cpu: runtime stats unavailable (total=0)\n");
    }

    s_prev_total = total_now;
    s_prev_idle = idle_now;
    s_prev_diag_us = now_us;
    pdmain_print(msg);
#else
    pdmain_print("cpu: disabled (set ESPD_ENABLE_CPU_STATS=1 for profiling builds)\n");
#endif
}

void glob_mem(void *dummy)
{
    (void)dummy;
    espd_print_memdiag();
}

void glob_cpu(void *dummy)
{
    (void)dummy;
    espd_print_cpudiag();
}



