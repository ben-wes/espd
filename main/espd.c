/*

   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

#include "espd.h"
#include "espd_audio.h"
#include "espd_board.h"
#include "espd_io.h"
#include "bsp/bsp_io.h"
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
#include "esp_system.h"
#include "esp_pthread.h"
#include "esp_task_wdt.h"
#include "esp_heap_caps.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "esp_timer.h"
#include <stdlib.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#if CONFIG_ESPD_DEV_CDC_SYNC
#include "espd_dev.h"
#endif
#if CONFIG_ESPD_USE_USB_OTG && CONFIG_SOC_USB_SERIAL_JTAG_SUPPORTED
#include "driver/usb_serial_jtag.h"
#endif
#if CONFIG_ESPD_USE_USB_OTG
#include "esp_partition.h"
#include "esp_vfs_fat.h"
#include "wear_levelling.h"
#include "tinyusb.h"
#include "tinyusb_msc.h"
#include "tinyusb_cdc_acm.h"
#include "tinyusb_console.h"
#include "tinyusb_default_config.h"
#endif
#if defined(ESPD_USE_AIN) || defined(ESPD_USE_TOUCH)
#include <stdatomic.h>
#endif
#ifdef ESPD_USE_AIN
#include "esp_adc/adc_oneshot.h"
#endif
#ifdef ESPD_USE_TOUCH
#include "driver/touch_sensor.h"
#endif
static const char *TAG = "ESPD";

#if CONFIG_ESPD_USE_USB_OTG && CONFIG_ESPD_USE_USB_MSC
static wl_handle_t wl_handle = WL_INVALID_HANDLE;
static tinyusb_msc_storage_handle_t msc_handle = NULL;
static bool s_flash_vfs_early;
RTC_NOINIT_ATTR static uint32_t s_usb_mode_magic_a;
RTC_NOINIT_ATTR static uint32_t s_usb_mode_magic_b;
#define ESPD_USB_MODE_MAGIC_A 0x45535044u /* "ESPD" */
#define ESPD_USB_MODE_MAGIC_B 0x4d534331u /* "MSC1" */
#endif

#if defined(ESPD_USE_AOUT)
#include "driver/ledc.h"
#endif
#ifdef ESPD_USE_CONSOLE
#include "driver/uart.h"
#include "esp_console.h"
#endif
#ifdef ESPD_USE_AOUT
#define ESPD_AOUT_MAX_CHANNELS 4
#define ESPD_AOUT_PWM_RES LEDC_TIMER_12_BIT
#define ESPD_AOUT_PWM_MAX_DUTY ((1u << 12) - 1u)

typedef struct _espd_aout_receiver
{
    t_pd x_pd;
    int idx;
} t_espd_aout_receiver;

static t_class *espd_aout_receiver_class;
static int espd_aout_pins[ESPD_AOUT_MAX_CHANNELS];
static int espd_aout_active[ESPD_AOUT_MAX_CHANNELS];
static t_espd_aout_receiver espd_aout_receivers[ESPD_AOUT_MAX_CHANNELS];
static int s_aout_cfg_have_pins;
static int s_aout_cfg_n;
static int s_aout_cfg_pins[ESPD_AOUT_MAX_CHANNELS];
static int espd_aout_pwm_freq_hz = ESPD_AOUT_PWM_FREQ_HZ;

static void espd_aout_set_value(int idx, t_float f)
{
    uint32_t duty;
    if (idx < 0 || idx >= ESPD_AOUT_MAX_CHANNELS || !espd_aout_active[idx])
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
    espd_aout_set_value(x->idx, (t_float)f);
}

static void espd_aout_init(void)
{
    ledc_timer_config_t timer_cfg = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = ESPD_AOUT_PWM_RES,
        .timer_num = LEDC_TIMER_0,
        .freq_hz = espd_aout_pwm_freq_hz,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    int i;
    int nchan;
    int enabled = 0;

    if (!s_aout_cfg_have_pins || s_aout_cfg_n <= 0)
        return;
    nchan = s_aout_cfg_n;
    if (nchan > ESPD_AOUT_MAX_CHANNELS)
        nchan = ESPD_AOUT_MAX_CHANNELS;
    for (i = 0; i < nchan; i++)
        espd_aout_pins[i] = s_aout_cfg_pins[i];
    for (i = nchan; i < ESPD_AOUT_MAX_CHANNELS; i++)
        espd_aout_pins[i] = -1;

    for (i = 0; i < ESPD_AOUT_MAX_CHANNELS; i++)
        espd_aout_active[i] = 0;

    if (ledc_timer_config(&timer_cfg) != ESP_OK)
    {
        timer_cfg.freq_hz = ESPD_AOUT_PWM_FALLBACK_FREQ_HZ;
        if (ledc_timer_config(&timer_cfg) != ESP_OK)
        {
            ESP_LOGE(TAG, "aout: LEDC timer init failed (freq=%d then %d)",
                     espd_aout_pwm_freq_hz, ESPD_AOUT_PWM_FALLBACK_FREQ_HZ);
            return;
        }
        ESP_LOGW(TAG, "aout: %d Hz unavailable at %d-bit PWM; using %d Hz",
                 espd_aout_pwm_freq_hz, 12, ESPD_AOUT_PWM_FALLBACK_FREQ_HZ);
    }

    for (i = 0; i < nchan; i++)
    {
        int pin = espd_aout_pins[i];
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
        espd_aout_active[i] = 1;
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
        if (!espd_aout_active[i])
            continue;
        espd_aout_receivers[i].x_pd = espd_aout_receiver_class;
        espd_aout_receivers[i].idx = i;
        snprintf(name, sizeof(name), "espd/aout/%d", i);
        pd_bind((t_pd *)&espd_aout_receivers[i], gensym(name));
    }
}
#endif

#ifdef ESPD_USE_DOUT
#define ESPD_DOUT_MAX_CHANNELS 8

typedef struct _espd_dout_receiver
{
    t_pd x_pd;
    int idx;
} t_espd_dout_receiver;

static t_class *espd_dout_receiver_class;
static int espd_dout_pins[ESPD_DOUT_MAX_CHANNELS];
static int espd_dout_active[ESPD_DOUT_MAX_CHANNELS];
static t_espd_dout_receiver espd_dout_receivers[ESPD_DOUT_MAX_CHANNELS];
static int s_dout_cfg_have_pins;
static int s_dout_cfg_n;
static int s_dout_cfg_pins[ESPD_DOUT_MAX_CHANNELS];

static void espd_dout_set_value(int idx, t_float f)
{
    if (idx < 0 || idx >= ESPD_DOUT_MAX_CHANNELS || !espd_dout_active[idx])
        return;
    gpio_set_level(espd_dout_pins[idx], (f >= 0.5f) ? 1 : 0);
}

static void espd_dout_receiver_float(t_espd_dout_receiver *x, t_floatarg f)
{
    espd_dout_set_value(x->idx, (t_float)f);
}

static void espd_dout_load_config(void)
{
    FILE *f;
    char line[256];
    const char *config_path = espd_storage_config_path();

    s_dout_cfg_have_pins = 0;
    s_dout_cfg_n = 0;
    if (!config_path)
    {
        ESP_LOGI(TAG, "dout: no config.txt — GPIO out off (set dout_pins= to enable)");
        return;
    }
    f = fopen(config_path, "r");
    if (!f)
    {
        ESP_LOGI(TAG, "dout: cannot read %s — GPIO out off", config_path);
        return;
    }
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
        if (!strcmp(k, "dout_pins"))
        {
            int n = 0;
            s_dout_cfg_have_pins = 1;
            if (!v || !*v)
                s_dout_cfg_n = 0;
            else
            {
                char *p = v;
                while (*p && n < ESPD_DOUT_MAX_CHANNELS)
                {
                    char *end;
                    long pin = strtol(p, &end, 10);
                    if (p == end)
                        break;
                    s_dout_cfg_pins[n++] = (int)pin;
                    p = end;
                    while (*p == ',' || *p == ' ' || *p == '\t')
                        p++;
                }
                s_dout_cfg_n = n;
            }
        }
    }
    fclose(f);
    if (!s_dout_cfg_have_pins)
        ESP_LOGI(TAG, "dout: %s: no dout_pins= — GPIO out off", config_path);
    else if (s_dout_cfg_n == 0)
        ESP_LOGI(TAG, "dout: %s: dout_pins= empty — GPIO out off", config_path);
    else
        ESP_LOGI(TAG, "dout: %s: %d channel(s) → espd/dout/0..%d",
            config_path, s_dout_cfg_n, s_dout_cfg_n - 1);
}

static void espd_dout_init(void)
{
    int i;
    int nchan;
    int enabled = 0;

    if (!s_dout_cfg_have_pins || s_dout_cfg_n <= 0)
        return;
    nchan = s_dout_cfg_n;
    if (nchan > ESPD_DOUT_MAX_CHANNELS)
        nchan = ESPD_DOUT_MAX_CHANNELS;

    for (i = 0; i < ESPD_DOUT_MAX_CHANNELS; i++)
        espd_dout_active[i] = 0;

    for (i = 0; i < nchan; i++)
    {
        int pin = s_dout_cfg_pins[i];
        gpio_config_t io = {
            .pin_bit_mask = (1ULL << (unsigned)pin),
            .mode = GPIO_MODE_OUTPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        if (gpio_config(&io) != ESP_OK)
        {
            ESP_LOGW(TAG, "dout%d setup failed on GPIO%d", i, pin);
            continue;
        }
        gpio_set_level(pin, 0);
        espd_dout_pins[i] = pin;
        espd_dout_active[i] = 1;
        enabled++;
        ESP_LOGI(TAG, "espd/dout/%d  GPIO%d", i, pin);
    }
    if (!enabled)
        return;

    if (!espd_dout_receiver_class)
    {
        espd_dout_receiver_class = class_new(gensym("_espd_dout_receiver"),
            0, 0, sizeof(t_espd_dout_receiver), CLASS_PD, 0);
        class_addfloat(espd_dout_receiver_class, (t_method)espd_dout_receiver_float);
    }

    for (i = 0; i < nchan; i++)
    {
        char name[16];
        if (!espd_dout_active[i])
            continue;
        espd_dout_receivers[i].x_pd = espd_dout_receiver_class;
        espd_dout_receivers[i].idx = i;
        snprintf(name, sizeof(name), "espd/dout/%d", i);
        pd_bind((t_pd *)&espd_dout_receivers[i], gensym(name));
    }
}
#endif /* ESPD_USE_DOUT */

#ifdef ESPD_USE_DIN
#define ESPD_DIN_GPIO_MAX_CHANNELS 8

static int s_din_gpio_cfg_have_pins;
static int s_din_gpio_cfg_n;
static int s_din_gpio_cfg_pins[ESPD_DIN_GPIO_MAX_CHANNELS];
static int espd_din_gpio_active_low = 1;
static int espd_din_gpio_task_period_ms = ESPD_DIN_GPIO_TASK_PERIOD_MS;

static int espd_din_gpio_pins[ESPD_DIN_GPIO_MAX_CHANNELS];
static int espd_din_gpio_active[ESPD_DIN_GPIO_MAX_CHANNELS];
static int espd_din_gpio_last_raw[ESPD_DIN_GPIO_MAX_CHANNELS];
static int espd_din_gpio_last_stable[ESPD_DIN_GPIO_MAX_CHANNELS];
static int espd_din_gpio_last_reported[ESPD_DIN_GPIO_MAX_CHANNELS];
static volatile int espd_din_gpio_dirty;
static TaskHandle_t espd_din_gpio_task;

static int espd_din_gpio_read_level(int pin)
{
    int level = gpio_get_level(pin);
    if (espd_din_gpio_active_low)
        return level ? 0 : 1;
    return level ? 1 : 0;
}

static void espd_din_load_config(void)
{
    FILE *f;
    char line[256];
    const char *config_path = espd_storage_config_path();

    s_din_gpio_cfg_have_pins = 0;
    s_din_gpio_cfg_n = 0;
    espd_din_gpio_active_low = ESPD_DIN_GPIO_ACTIVE_LOW;
    espd_din_gpio_task_period_ms = ESPD_DIN_GPIO_TASK_PERIOD_MS;
    if (!config_path)
    {
        ESP_LOGI(TAG, "din: no config.txt — no GPIO din (set din_pins= to add)");
        return;
    }
    f = fopen(config_path, "r");
    if (!f)
    {
        ESP_LOGI(TAG, "din: cannot read %s", config_path);
        return;
    }
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
        if (!strcmp(k, "din_pins"))
        {
            int n = 0;
            s_din_gpio_cfg_have_pins = 1;
            if (!v || !*v)
                s_din_gpio_cfg_n = 0;
            else
            {
                char *p = v;
                while (*p && n < ESPD_DIN_GPIO_MAX_CHANNELS)
                {
                    char *end;
                    long pin = strtol(p, &end, 10);
                    if (p == end)
                        break;
                    s_din_gpio_cfg_pins[n++] = (int)pin;
                    p = end;
                    while (*p == ',' || *p == ' ' || *p == '\t')
                        p++;
                }
                s_din_gpio_cfg_n = n;
            }
        }
        else if (!strcmp(k, "din_active_low"))
        {
            espd_din_gpio_active_low = (atoi(v) != 0);
        }
        else if (!strcmp(k, "din_task_period_ms"))
        {
            int t = atoi(v);
            if (t >= 1 && t <= 500)
                espd_din_gpio_task_period_ms = t;
        }
    }
    fclose(f);
    if (s_din_gpio_cfg_have_pins && s_din_gpio_cfg_n > 0)
    {
        int base = bsp_button_count();
        ESP_LOGI(TAG, "din: %s: %d GPIO channel(s) → espd/din/%d..%d",
            config_path, s_din_gpio_cfg_n, base,
            base + s_din_gpio_cfg_n - 1);
    }
    else if (s_din_gpio_cfg_have_pins)
        ESP_LOGI(TAG, "din: %s: din_pins= empty — no GPIO din", config_path);
}

static void espd_din_gpio_task_fn(void *arg)
{
    TickType_t next = xTaskGetTickCount();
    (void)arg;
    for (;;)
    {
        int i;
        TickType_t period = pdMS_TO_TICKS(espd_din_gpio_task_period_ms);
        if (period < 1)
            period = 1;
        vTaskDelayUntil(&next, period);
        for (i = 0; i < s_din_gpio_cfg_n; i++)
        {
            int raw;
            if (!espd_din_gpio_active[i])
                continue;
            raw = espd_din_gpio_read_level(espd_din_gpio_pins[i]);
            if (raw == espd_din_gpio_last_raw[i] && raw != espd_din_gpio_last_stable[i])
            {
                espd_din_gpio_last_stable[i] = raw;
                espd_din_gpio_dirty = 1;
            }
            espd_din_gpio_last_raw[i] = raw;
        }
    }
}

static void espd_din_gpio_init(void)
{
    int i;
    int enabled = 0;

    if (!s_din_gpio_cfg_have_pins || s_din_gpio_cfg_n <= 0)
        return;

    for (i = 0; i < ESPD_DIN_GPIO_MAX_CHANNELS; i++)
    {
        espd_din_gpio_active[i] = 0;
        espd_din_gpio_last_raw[i] = -1;
        espd_din_gpio_last_stable[i] = -1;
        espd_din_gpio_last_reported[i] = -1;
    }

    for (i = 0; i < s_din_gpio_cfg_n; i++)
    {
        int pin = s_din_gpio_cfg_pins[i];
        gpio_config_t io = {
            .pin_bit_mask = (1ULL << (unsigned)pin),
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = espd_din_gpio_active_low ? GPIO_PULLUP_ENABLE : GPIO_PULLUP_DISABLE,
            .pull_down_en = espd_din_gpio_active_low ? GPIO_PULLDOWN_DISABLE : GPIO_PULLDOWN_ENABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        if (gpio_config(&io) != ESP_OK)
        {
            ESP_LOGW(TAG, "din GPIO%d setup failed", pin);
            continue;
        }
        espd_din_gpio_pins[i] = pin;
        espd_din_gpio_active[i] = 1;
        enabled++;
    }
    if (!enabled)
        return;

    if (!espd_din_gpio_task)
    {
        BaseType_t ok = xTaskCreatePinnedToCore(espd_din_gpio_task_fn,
            "espd_din", 3072, NULL, ESPD_DIN_GPIO_TASK_PRIO,
            &espd_din_gpio_task, ESPD_DIN_GPIO_TASK_CORE);
        if (ok != pdPASS)
        {
            ESP_LOGW(TAG, "failed to create espd_din task");
            espd_din_gpio_task = NULL;
        }
    }
}

void espd_din_gpio_poll(void)
{
    int i;
    int base = bsp_button_count();

    if (!espd_din_gpio_dirty)
        return;
    espd_din_gpio_dirty = 0;
    for (i = 0; i < s_din_gpio_cfg_n; i++)
    {
        if (!espd_din_gpio_active[i])
            continue;
        if (espd_din_gpio_last_stable[i] != espd_din_gpio_last_reported[i])
        {
            espd_din_gpio_last_reported[i] = espd_din_gpio_last_stable[i];
            espd_din_changed(base + i, espd_din_gpio_last_stable[i]);
        }
    }
}
#endif /* ESPD_USE_DIN */

void espd_din_log_map(void)
{
    int bsp_n = bsp_button_count();
    int i;

    for (i = 0; i < bsp_n; i++)
        ESP_LOGI(TAG, "espd/din/%d  board (BSP digital in)", i);
#ifdef ESPD_USE_DIN
    for (i = 0; i < s_din_gpio_cfg_n; i++)
    {
        if (!espd_din_gpio_active[i])
            continue;
        ESP_LOGI(TAG, "espd/din/%d  GPIO%d (din_pins[%d], active-%s)",
            bsp_n + i, espd_din_gpio_pins[i], i,
            espd_din_gpio_active_low ? "low" : "high");
    }
#endif
}

#if defined(ESPD_USE_SDCARD) || CONFIG_ESPD_USE_USB_MSC || defined(ESPD_USE_WIFI) || \
    defined(ESPD_USE_AIN) || defined(ESPD_USE_TOUCH) || defined(ESPD_USE_AOUT) || \
    defined(ESPD_USE_DIN) || defined(ESPD_USE_DOUT)
#include <stdio.h>
#endif
#if defined(ESPD_USE_SDCARD) || CONFIG_ESPD_USE_USB_MSC
#include <dirent.h>
#include <errno.h>
#include <sys/stat.h>
#endif
#include "espd_storage.h"
#include "espd_runtime_config.h"

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

static void espd_audio_load_config(void)
{
    FILE *f;
    char line[256];
    const char *config_path = espd_storage_config_path();
    int overridden = 0;

    if (!config_path)
        return;
    f = fopen(config_path, "r");
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
        if (!strcmp(k, "audio_dma_desc_num"))
        {
            int n = atoi(v);
            if (n >= 2 && n <= 16)
            {
                espd_audio_set_dma_desc_num(n);
                overridden = 1;
            }
        }
        else if (!strcmp(k, "audio_dma_frame_num"))
        {
            int n = atoi(v);
            if (n >= 8 && n <= 1024)
            {
                espd_audio_set_dma_frame_num(n);
                overridden = 1;
            }
        }
        else if (!strcmp(k, "audio_sample_rate"))
        {
            int n = atoi(v);
            if (n >= ESPD_AUDIO_SAMPLE_RATE_MIN &&
                n <= ESPD_AUDIO_SAMPLE_RATE_MAX)
            {
                espd_audio_set_sample_rate(n);
                overridden = 1;
            }
            else
            {
                ESP_LOGW(TAG,
                    "audio: %s: ignoring audio_sample_rate=%s (range %d..%d)",
                    config_path, v, ESPD_AUDIO_SAMPLE_RATE_MIN,
                    ESPD_AUDIO_SAMPLE_RATE_MAX);
            }
        }
    }
    fclose(f);
    if (overridden)
    {
        int desc = espd_audio_dma_desc_num();
        int frames = espd_audio_dma_frame_num();
        int hz = espd_audio_sample_rate_hz();
        ESP_LOGI(TAG, "audio: %s: %d Hz, dma %d x %d frames (~%.1f ms)",
            config_path, hz, desc, frames,
            hz > 0 ? 1000.f * (float)desc * (float)frames / (float)hz : 0.f);
    }
}

#ifdef ESPD_USE_AOUT
static void espd_aout_load_config(void)
{
    FILE *f;
    char line[256];
    const char *config_path = espd_storage_config_path();

    s_aout_cfg_have_pins = 0;
    s_aout_cfg_n = 0;
    espd_aout_pwm_freq_hz = ESPD_AOUT_PWM_FREQ_HZ;
    if (!config_path)
    {
        ESP_LOGI(TAG, "aout: no config.txt — PWM off (set aout_pins= to enable)");
        return;
    }
    f = fopen(config_path, "r");
    if (!f)
    {
        ESP_LOGI(TAG, "aout: cannot read %s — PWM off", config_path);
        return;
    }
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
        if (!strcmp(k, "aout_pins"))
        {
            int n = 0;
            s_aout_cfg_have_pins = 1;
            if (!v || !*v)
                s_aout_cfg_n = 0;
            else
            {
                char *p = v;
                while (*p && n < ESPD_AOUT_MAX_CHANNELS)
                {
                    char *end;
                    long pin = strtol(p, &end, 10);
                    if (p == end)
                        break;
                    s_aout_cfg_pins[n++] = (int)pin;
                    p = end;
                    while (*p == ',' || *p == ' ' || *p == '\t')
                        p++;
                }
                s_aout_cfg_n = n;
            }
        }
        else if (!strcmp(k, "aout_pwm_freq_hz"))
        {
            int hz = atoi(v);
            if (hz >= 100 && hz <= 40000000)
                espd_aout_pwm_freq_hz = hz;
        }
    }
    fclose(f);
    if (!s_aout_cfg_have_pins)
        ESP_LOGI(TAG, "aout: %s: no aout_pins= — PWM off", config_path);
    else if (s_aout_cfg_n == 0)
        ESP_LOGI(TAG, "aout: %s: aout_pins= empty — PWM off", config_path);
    else
        ESP_LOGI(TAG, "aout: %s: %d channel(s) from aout_pins @ %d Hz",
            config_path, s_aout_cfg_n, espd_aout_pwm_freq_hz);
}
#endif

int espd_main_pd_loaded_from_store;
const char *espd_main_pd_loaded_dir;
#ifdef ESPD_USE_WIFI
int espd_wifi_net_enabled = 1;
static int espd_wifi_started;
char espd_wifi_ssid[33];
char espd_wifi_password[65];
int espd_log_broadcast_port;
static void espd_wifi_config_defaults(void)
{
    snprintf(espd_wifi_ssid, sizeof(espd_wifi_ssid), "%s", CONFIG_ESP_WIFI_SSID);
    snprintf(espd_wifi_password, sizeof(espd_wifi_password), "%s", CONFIG_ESP_WIFI_PASSWORD);
    espd_log_broadcast_port = 0;
}

static int s_wifi_credentials_in_config_txt;

static int espd_wifi_config_txt_allows_sta(void)
{
    if (!espd_storage_config_path()) {
#ifdef ESPD_USE_SDCARD
        return 0;
#else
        return 1;
#endif
    }
    return s_wifi_credentials_in_config_txt;
}

static void espd_wifi_try_load_config(void)
{
    const char *config_path = espd_storage_config_path();
    FILE *f;
    char line[256];
    int ssid_nonempty = 0;

    if (!config_path)
    {
        s_wifi_credentials_in_config_txt = 0;
        espd_wifi_ssid[0] = '\0';
        espd_wifi_password[0] = '\0';
#ifdef ESPD_USE_SDCARD
        ESP_LOGI(TAG, "wifi: no config.txt on SD or local flash");
#endif
        return;
    }

    f = fopen(config_path, "r");
    if (!f)
    {
        s_wifi_credentials_in_config_txt = 0;
        espd_wifi_ssid[0] = '\0';
        espd_wifi_password[0] = '\0';
        ESP_LOGI(TAG, "wifi: cannot read %s", config_path);
        return;
    }
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
            ssid_nonempty = (espd_wifi_ssid[0] != '\0');
        }
        else if (!strcmp(k, "wifi_password"))
        {
            snprintf(espd_wifi_password, sizeof(espd_wifi_password), "%s", v);
        }
        else if (!strcmp(k, "log_broadcast_port"))
        {
            long p = strtol(v, NULL, 10);
            if (p > 0 && p <= 65535)
                espd_log_broadcast_port = (int)p;
            else
                espd_log_broadcast_port = 0;
        }
    }
    fclose(f);
    s_wifi_credentials_in_config_txt = ssid_nonempty;
    if (!s_wifi_credentials_in_config_txt)
    {
        espd_wifi_ssid[0] = '\0';
        espd_wifi_password[0] = '\0';
    }
    ESP_LOGI(TAG, "wifi: %s (ssid=%s, sta=%s, log_port=%d)",
        config_path, espd_wifi_ssid[0] ? espd_wifi_ssid : "(none)",
        s_wifi_credentials_in_config_txt ? "on" : "off", espd_log_broadcast_port);
}
#endif /* ESPD_USE_WIFI */

#ifdef ESPD_USE_AIN
/* Run-time tuning (defaults from espd.h; config.txt may override with SD build). */
static int espd_ain_task_period_ms = ESPD_AIN_TASK_PERIOD_MS;
static int espd_ain_deadband = ESPD_AIN_DEADBAND;
static int espd_ain_report_every_n_blocks = ESPD_AIN_REPORT_EVERY_N_BLOCKS;
#endif
#ifdef ESPD_USE_TOUCH
/* Run-time tuning (defaults from espd.h; config.txt may override with SD build). */
static int espd_touch_task_period_ms = ESPD_TOUCH_TASK_PERIOD_MS;
static int espd_touch_report_every_n_blocks = ESPD_TOUCH_REPORT_EVERY_N_BLOCKS;
#endif

#if defined(ESPD_USE_AIN)
/* Parsed from config.txt (SD or /storage) before espd_ain_init. */
static int s_analog_cfg_have_pins;
static int s_analog_cfg_n;
static int s_analog_cfg_pins[8];

static void espd_ain_load_config(void)
{
    FILE *f;
    char line[256];
    const char *config_path = espd_storage_config_path();

    s_analog_cfg_have_pins = 0;
    s_analog_cfg_n = 0;
    if (!config_path)
    {
        ESP_LOGI(TAG, "ain: no config.txt — ADC off (set ain_pins= to enable)");
        return;
    }
    f = fopen(config_path, "r");
    if (!f)
    {
        ESP_LOGI(TAG, "ain: cannot read %s — ADC off", config_path);
        return;
    }
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
        if (!strcmp(k, "ain_pins"))
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
        else if (!strcmp(k, "ain_task_period_ms"))
        {
            int t = atoi(v);
            if (t >= 1 && t <= 500)
                espd_ain_task_period_ms = t;
        }
        else if (!strcmp(k, "ain_deadband"))
        {
            int d = atoi(v);
            if (d >= 1 && d <= 2047)
                espd_ain_deadband = d;
        }
        else if (!strcmp(k, "ain_report_every_n_blocks"))
        {
            int r = atoi(v);
            if (r >= 1 && r <= 64)
                espd_ain_report_every_n_blocks = r;
        }
    }
    fclose(f);
    if (!s_analog_cfg_have_pins)
        ESP_LOGI(TAG, "ain: %s: no ain_pins= — ADC off", config_path);
    else if (s_analog_cfg_n == 0)
        ESP_LOGI(TAG, "ain: %s: ain_pins= empty — ADC off", config_path);
    else
        ESP_LOGI(TAG, "ain: %s: %d channel(s) → espd/ain/0..%d",
            config_path, s_analog_cfg_n, s_analog_cfg_n - 1);
}
#endif

#if defined(ESPD_USE_TOUCH)
/* Parsed from config.txt (SD or /storage) before espd_touch_init. */
static int s_touch_cfg_have_pins;
static int s_touch_cfg_n;
static int s_touch_cfg_pins[8];

static void espd_touch_load_config(void)
{
    FILE *f;
    char line[256];
    const char *config_path = espd_storage_config_path();

    s_touch_cfg_have_pins = 0;
    s_touch_cfg_n = 0;
    if (!config_path)
    {
        ESP_LOGI(TAG, "touch: no config.txt — touch off (set touch_pins= to enable)");
        return;
    }
    f = fopen(config_path, "r");
    if (!f)
    {
        ESP_LOGI(TAG, "touch: cannot read %s — touch off", config_path);
        return;
    }
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
        if (!strcmp(k, "touch_pins"))
        {
            int n = 0;
            s_touch_cfg_have_pins = 1;
            if (!v || !*v)
            {
                s_touch_cfg_n = 0;
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
                    s_touch_cfg_pins[n++] = (int)pin;
                    p = end;
                    while (*p == ',' || *p == ' ' || *p == '\t')
                        p++;
                }
                s_touch_cfg_n = n;
            }
        }
        else if (!strcmp(k, "touch_task_period_ms"))
        {
            int t = atoi(v);
            if (t >= 1 && t <= 500)
                espd_touch_task_period_ms = t;
        }
        else if (!strcmp(k, "touch_report_every_n_blocks"))
        {
            int r = atoi(v);
            if (r >= 1 && r <= 64)
                espd_touch_report_every_n_blocks = r;
        }
    }
    fclose(f);
    if (!s_touch_cfg_have_pins)
        ESP_LOGI(TAG, "touch: %s: no touch_pins= — touch off", config_path);
    else if (s_touch_cfg_n == 0)
        ESP_LOGI(TAG, "touch: %s: touch_pins= empty — touch off", config_path);
    else
        ESP_LOGI(TAG, "touch: %s: %d channel(s) from touch_pins", config_path, s_touch_cfg_n);
}
#endif

#ifdef ESPD_USE_AIN
static adc_oneshot_unit_handle_t espd_ain_adc_handle;
#define ESPD_AIN_MAX_CHANNELS 8
static int espd_ain_adc_pins[ESPD_AIN_MAX_CHANNELS] = {
    ESPD_AIN_PIN_0, ESPD_AIN_PIN_1, ESPD_AIN_PIN_2, ESPD_AIN_PIN_3,
    ESPD_AIN_PIN_4, ESPD_AIN_PIN_5, ESPD_AIN_PIN_6, ESPD_AIN_PIN_7
};
static adc_channel_t espd_ain_adc_channels[ESPD_AIN_MAX_CHANNELS];
static int espd_ain_adc_active[ESPD_AIN_MAX_CHANNELS];
/* Producer-side deadband reference (written only by the ADC task). */
static int espd_ain_adc_last[ESPD_AIN_MAX_CHANNELS];
/* Producer -> consumer: task writes `espd_ain_adc_latest[i]` then ORs bit i in
 * `espd_ain_adc_dirty` (release). Audio thread exchanges dirty (acquire), forwards
 * only set channels — one atomic when idle vs eight per-channel seq checks. */
static _Atomic int espd_ain_adc_latest[ESPD_AIN_MAX_CHANNELS];
static _Atomic uint32_t espd_ain_adc_dirty;
static TaskHandle_t espd_ain_adc_task;
static unsigned espd_ain_block_counter;

static void espd_ain_send(int idx, int raw)
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

/* Producer: dedicated task. Reads channels each wake, deadband filter, ORs
 * dirty bits for changed channels. Never calls into Pd. */
static void espd_ain_adc_task_fn(void *arg)
{
    (void)arg;
    TickType_t next = xTaskGetTickCount();
    for (;;)
    {
        int i;
        TickType_t period = pdMS_TO_TICKS(espd_ain_task_period_ms);
        if (period < 1)
            period = 1;
        vTaskDelayUntil(&next, period);
        if (!espd_ain_adc_handle)
            continue;
        uint32_t dirty_mask = 0;
        for (i = 0; i < ESPD_AIN_MAX_CHANNELS; i++)
        {
            int raw;
            int db = espd_ain_deadband;
            if (!espd_ain_adc_active[i])
                continue;
            if (adc_oneshot_read(espd_ain_adc_handle, espd_ain_adc_channels[i], &raw)
                    != ESP_OK)
                continue;
            if (raw < espd_ain_adc_last[i] - db || raw > espd_ain_adc_last[i] + db)
            {
                espd_ain_adc_last[i] = raw;
                atomic_store_explicit(&espd_ain_adc_latest[i], raw,
                    memory_order_relaxed);
                dirty_mask |= (1u << (unsigned)i);
            }
        }
        if (dirty_mask)
            atomic_fetch_or_explicit(&espd_ain_adc_dirty, dirty_mask,
                memory_order_release);
    }
}

static void espd_ain_init(void)
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
#if defined(ESPD_USE_AIN)
    if (!s_analog_cfg_have_pins || s_analog_cfg_n <= 0)
        return;
    nchan = s_analog_cfg_n;
    if (nchan > ESPD_AIN_MAX_CHANNELS)
        nchan = ESPD_AIN_MAX_CHANNELS;
    for (i = 0; i < nchan; i++)
        espd_ain_adc_pins[i] = s_analog_cfg_pins[i];
    for (i = nchan; i < ESPD_AIN_MAX_CHANNELS; i++)
        espd_ain_adc_pins[i] = -1;
#else
    return;
#endif
    esp_err_t err = adc_oneshot_new_unit(&unit_cfg, &espd_ain_adc_handle);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "adc_oneshot_new_unit failed: %d", (int)err);
        return;
    }

    for (i = 0; i < ESPD_AIN_MAX_CHANNELS; i++)
    {
        espd_ain_adc_active[i] = 0;
        espd_ain_adc_last[i] = -100000;
        atomic_store_explicit(&espd_ain_adc_latest[i], 0, memory_order_relaxed);
    }

    for (i = 0; i < nchan; i++)
    {
        int pin = espd_ain_adc_pins[i];
        adc_channel_t ch;
        if (pin < 0)
            continue;
        if (!pd_pin_to_adc1_channel(pin, &ch))
        {
            ESP_LOGW(TAG, "espd/ain/%d: GPIO%d is not a valid analog-input pin", i, pin);
            continue;
        }
        err = adc_oneshot_config_channel(espd_ain_adc_handle, ch, &chan_cfg);
        if (err != ESP_OK)
        {
            ESP_LOGW(TAG, "analog ain%d setup failed on GPIO%d: %d", i, pin, (int)err);
            continue;
        }
        espd_ain_adc_channels[i] = ch;
        espd_ain_adc_active[i] = 1;
        enabled++;
        ESP_LOGI(TAG, "analog ain%d enabled on GPIO%d", i, pin);
    }
    if (!enabled)
    {
        ESP_LOGW(TAG, "analog enabled but no valid channels configured");
        return;
    }

    atomic_store_explicit(&espd_ain_adc_dirty, 0u, memory_order_relaxed);

    /* Spawn the producer task on the opposite core from Pd audio (core 1), so
     * adc_oneshot_read() blocking never steals time from senddacs(). */
    if (!espd_ain_adc_task) {
        BaseType_t ok = xTaskCreatePinnedToCore(espd_ain_adc_task_fn,
            "espd_ain_adc", 3072, NULL, ESPD_AIN_TASK_PRIO,
            &espd_ain_adc_task, ESPD_AIN_TASK_CORE);
        if (ok != pdPASS) {
            ESP_LOGW(TAG, "failed to create espd_ain_adc task; falling back to audio-thread polling");
            espd_ain_adc_task = NULL;
        } else {
            ESP_LOGI(TAG, "espd_ain_adc task: core=%d prio=%d period=%d ms deadband=%d report_every=%d %d ch",
                (int)ESPD_AIN_TASK_CORE, (int)ESPD_AIN_TASK_PRIO,
                espd_ain_task_period_ms, espd_ain_deadband,
                espd_ain_report_every_n_blocks, enabled);
        }
    }
}

/* Consumer: audio thread. One atomic exchange on the dirty mask; forwards
 * only channels that changed. ADC reads run on espd_ain_adc_task. */
static void espd_ain_poll(void)
{
    int i;
    int report_every = espd_ain_report_every_n_blocks;
    if (!espd_ain_adc_handle)
        return;
    if (report_every < 1)
        report_every = 1;
    if (++espd_ain_block_counter < (unsigned)report_every)
        return;
    espd_ain_block_counter = 0;

    /* Fallback: if the producer task failed to start, poll inline so ain
     * channels still work (same behavior as before). */
    if (!espd_ain_adc_task)
    {
        int db = espd_ain_deadband;
        for (i = 0; i < ESPD_AIN_MAX_CHANNELS; i++)
        {
            int raw;
            if (!espd_ain_adc_active[i])
                continue;
            if (adc_oneshot_read(espd_ain_adc_handle, espd_ain_adc_channels[i], &raw)
                    != ESP_OK)
                continue;
            if (raw < espd_ain_adc_last[i] - db || raw > espd_ain_adc_last[i] + db)
            {
                espd_ain_adc_last[i] = raw;
                espd_ain_send(i, raw);
            }
        }
        return;
    }

    {
        uint32_t dirty = atomic_exchange_explicit(&espd_ain_adc_dirty, 0,
            memory_order_acquire);
        if (!dirty)
            return;
        for (i = 0; i < ESPD_AIN_MAX_CHANNELS; i++)
        {
            int raw;
            if (!(dirty & (1u << (unsigned)i)))
                continue;
            raw = atomic_load_explicit(&espd_ain_adc_latest[i], memory_order_relaxed);
            espd_ain_send(i, raw);
        }
    }
}
#endif

#ifdef ESPD_USE_TOUCH
#define ESPD_TOUCH_MAX_CHANNELS 8
static touch_pad_t espd_touch_channels[ESPD_TOUCH_MAX_CHANNELS];
static int espd_touch_pins[ESPD_TOUCH_MAX_CHANNELS];
static int espd_touch_active[ESPD_TOUCH_MAX_CHANNELS];
static uint32_t espd_touch_last[ESPD_TOUCH_MAX_CHANNELS];
static _Atomic uint32_t espd_touch_latest[ESPD_TOUCH_MAX_CHANNELS];
static _Atomic uint32_t espd_touch_dirty;
static TaskHandle_t espd_touch_task;
static unsigned espd_touch_block_counter;
static int espd_touch_inited;

static void espd_touch_send(int idx, uint32_t raw)
{
    char name[20];
    t_symbol *sym;
    t_pd *dest;
    snprintf(name, sizeof(name), "espd/touch/%d", idx);
    sym = gensym(name);
    dest = sym ? sym->s_thing : NULL;
    if (dest)
        pd_float(dest, (t_float)raw);
}

static int pd_pin_to_touch_channel(int pin, touch_pad_t *channel)
{
#if CONFIG_IDF_TARGET_ESP32S3
    switch (pin)
    {
    case 1:  *channel = TOUCH_PAD_NUM1; return 1;
    case 2:  *channel = TOUCH_PAD_NUM2; return 1;
    case 3:  *channel = TOUCH_PAD_NUM3; return 1;
    case 4:  *channel = TOUCH_PAD_NUM4; return 1;
    case 5:  *channel = TOUCH_PAD_NUM5; return 1;
    case 6:  *channel = TOUCH_PAD_NUM6; return 1;
    case 7:  *channel = TOUCH_PAD_NUM7; return 1;
    case 8:  *channel = TOUCH_PAD_NUM8; return 1;
    case 9:  *channel = TOUCH_PAD_NUM9; return 1;
    case 10: *channel = TOUCH_PAD_NUM10; return 1;
    case 11: *channel = TOUCH_PAD_NUM11; return 1;
    case 12: *channel = TOUCH_PAD_NUM12; return 1;
    case 13: *channel = TOUCH_PAD_NUM13; return 1;
    case 14: *channel = TOUCH_PAD_NUM14; return 1;
    default: return 0;
    }
#elif CONFIG_IDF_TARGET_ESP32
    switch (pin)
    {
    case 4:  *channel = TOUCH_PAD_NUM0; return 1;
    case 0:  *channel = TOUCH_PAD_NUM1; return 1;
    case 2:  *channel = TOUCH_PAD_NUM2; return 1;
    case 15: *channel = TOUCH_PAD_NUM3; return 1;
    case 13: *channel = TOUCH_PAD_NUM4; return 1;
    case 12: *channel = TOUCH_PAD_NUM5; return 1;
    case 14: *channel = TOUCH_PAD_NUM6; return 1;
    case 27: *channel = TOUCH_PAD_NUM7; return 1;
    case 33: *channel = TOUCH_PAD_NUM8; return 1;
    case 32: *channel = TOUCH_PAD_NUM9; return 1;
    default: return 0;
    }
#else
    (void)pin;
    (void)channel;
    return 0;
#endif
}

/* Producer: dedicated task. Reads channels each wake and marks changed values. */
static void espd_touch_task_fn(void *arg)
{
    (void)arg;
    TickType_t next = xTaskGetTickCount();
    for (;;)
    {
        int i;
        TickType_t period = pdMS_TO_TICKS(espd_touch_task_period_ms);
        if (period < 1)
            period = 1;
        vTaskDelayUntil(&next, period);
        if (!espd_touch_inited)
            continue;
        {
            uint32_t dirty_mask = 0;
            for (i = 0; i < ESPD_TOUCH_MAX_CHANNELS; i++)
            {
                uint32_t raw = 0;
                if (!espd_touch_active[i])
                    continue;
                if (touch_pad_read_raw_data(espd_touch_channels[i], &raw) != ESP_OK)
                    continue;
                if (raw != espd_touch_last[i])
                {
                    espd_touch_last[i] = raw;
                    atomic_store_explicit(&espd_touch_latest[i], raw, memory_order_relaxed);
                    dirty_mask |= (1u << (unsigned)i);
                }
            }
            if (dirty_mask)
                atomic_fetch_or_explicit(&espd_touch_dirty, dirty_mask, memory_order_release);
        }
    }
}

static void espd_touch_init(void)
{
    int i;
    int nchan;
    int enabled = 0;
#if defined(ESPD_USE_TOUCH)
    if (!s_touch_cfg_have_pins || s_touch_cfg_n <= 0)
        return;
    nchan = s_touch_cfg_n;
    if (nchan > ESPD_TOUCH_MAX_CHANNELS)
        nchan = ESPD_TOUCH_MAX_CHANNELS;
    for (i = 0; i < nchan; i++)
        espd_touch_pins[i] = s_touch_cfg_pins[i];
    for (i = nchan; i < ESPD_TOUCH_MAX_CHANNELS; i++)
        espd_touch_pins[i] = -1;
#else
    return;
#endif

    if (touch_pad_init() != ESP_OK)
    {
        ESP_LOGW(TAG, "touch_pad_init failed");
        return;
    }
    if (touch_pad_set_fsm_mode(TOUCH_FSM_MODE_TIMER) != ESP_OK)
        ESP_LOGW(TAG, "touch: set fsm mode failed");

    for (i = 0; i < ESPD_TOUCH_MAX_CHANNELS; i++)
    {
        espd_touch_active[i] = 0;
        espd_touch_last[i] = UINT32_MAX;
        atomic_store_explicit(&espd_touch_latest[i], 0, memory_order_relaxed);
    }

    for (i = 0; i < nchan; i++)
    {
        int pin = espd_touch_pins[i];
        touch_pad_t ch;
        if (pin < 0)
            continue;
        if (!pd_pin_to_touch_channel(pin, &ch))
        {
            ESP_LOGW(TAG, "touch in%d ignored: GPIO%d is not touch-capable", i, pin);
            continue;
        }
        if (touch_pad_config(ch) != ESP_OK)
        {
            ESP_LOGW(TAG, "touch in%d setup failed on GPIO%d", i, pin);
            continue;
        }
        espd_touch_channels[i] = ch;
        espd_touch_active[i] = 1;
        enabled++;
        ESP_LOGI(TAG, "touch in%d enabled on GPIO%d", i, pin);
    }
    if (!enabled)
    {
        ESP_LOGW(TAG, "touch enabled but no valid channels configured");
        return;
    }

    if (touch_pad_fsm_start() != ESP_OK)
        ESP_LOGW(TAG, "touch: fsm start failed");

    espd_touch_inited = 1;
    atomic_store_explicit(&espd_touch_dirty, 0u, memory_order_relaxed);
    if (!espd_touch_task) {
        BaseType_t ok = xTaskCreatePinnedToCore(espd_touch_task_fn,
            "espd_touch", 3072, NULL, ESPD_TOUCH_TASK_PRIO,
            &espd_touch_task, ESPD_TOUCH_TASK_CORE);
        if (ok != pdPASS) {
            ESP_LOGW(TAG, "failed to create espd_touch task; falling back to audio-thread polling");
            espd_touch_task = NULL;
        } else {
            ESP_LOGI(TAG, "espd_touch task: core=%d prio=%d period=%d ms report_every=%d %d ch",
                (int)ESPD_TOUCH_TASK_CORE, (int)ESPD_TOUCH_TASK_PRIO,
                espd_touch_task_period_ms, espd_touch_report_every_n_blocks, enabled);
        }
    }
}

/* Consumer: audio thread. One atomic exchange on the dirty mask. */
static void espd_touch_poll(void)
{
    int i;
    int report_every = espd_touch_report_every_n_blocks;
    if (!espd_touch_inited)
        return;
    if (report_every < 1)
        report_every = 1;
    if (++espd_touch_block_counter < (unsigned)report_every)
        return;
    espd_touch_block_counter = 0;

    if (!espd_touch_task)
    {
        for (i = 0; i < ESPD_TOUCH_MAX_CHANNELS; i++)
        {
            uint32_t raw = 0;
            if (!espd_touch_active[i])
                continue;
            if (touch_pad_read_raw_data(espd_touch_channels[i], &raw) != ESP_OK)
                continue;
            if (raw != espd_touch_last[i])
            {
                espd_touch_last[i] = raw;
                espd_touch_send(i, raw);
            }
        }
        return;
    }

    {
        uint32_t dirty = atomic_exchange_explicit(&espd_touch_dirty, 0,
            memory_order_acquire);
        if (!dirty)
            return;
        for (i = 0; i < ESPD_TOUCH_MAX_CHANNELS; i++)
        {
            uint32_t raw;
            if (!(dirty & (1u << (unsigned)i)))
                continue;
            raw = atomic_load_explicit(&espd_touch_latest[i], memory_order_relaxed);
            espd_touch_send(i, raw);
        }
    }
}
#endif

#if CONFIG_ESPD_USE_USB_OTG
/* Pd/audio on CPU1 (board profile); TinyUSB on CPU0. */
#define ESPD_USB_TASK_CORE          0
#define ESPD_USB_INIT_TASK_PRIO     2
#define ESPD_USB_DEVICE_TASK_PRIO   4  /* step 2: drain MSC FIFO during host writes */

#if CONFIG_ESPD_USE_USB_MSC
bool espd_usb_msc_sync_mode_active(void)
{
    return s_usb_mode_magic_a == ESPD_USB_MODE_MAGIC_A
        && s_usb_mode_magic_b == ESPD_USB_MODE_MAGIC_B;
}

void espd_usb_msc_sync_mode_set(bool active)
{
    if (active) {
        s_usb_mode_magic_a = ESPD_USB_MODE_MAGIC_A;
        s_usb_mode_magic_b = ESPD_USB_MODE_MAGIC_B;
    } else {
        s_usb_mode_magic_a = 0;
        s_usb_mode_magic_b = 0;
    }
}

/* RTC survives esp_restart(); power-on / reset button must return to normal (host MSC). */
static void espd_usb_msc_sync_clear_unless_sw_reset(void)
{
    if (esp_reset_reason() != ESP_RST_SW) {
        s_usb_mode_magic_a = 0;
        s_usb_mode_magic_b = 0;
    }
}

#else
bool espd_usb_msc_sync_mode_active(void) { return false; }
void espd_usb_msc_sync_mode_set(bool active) { (void)active; }
#endif

static void espd_usb_apply_msc_volume_label_when_ready(void)
{
    /* Keep default FAT volume label behavior (typically "NO NAME"). */
}

#if CONFIG_ESPD_USE_USB_MSC
/* VFS /storage before TinyUSB — config.txt without MSC driver on the USB PHY. */
static esp_err_t espd_usb_mount_flash_early_vfs(void)
{
    esp_vfs_fat_mount_config_t mount_cfg;

    if (msc_handle != NULL || s_flash_vfs_early)
        return ESP_OK;

    mount_cfg = (esp_vfs_fat_mount_config_t){
        .max_files = 16,
        .format_if_mount_failed = true,
        .allocation_unit_size = CONFIG_WL_SECTOR_SIZE,
    };
    esp_err_t err = esp_vfs_fat_spiflash_mount_rw_wl(
        ESPD_STORAGE_MOUNT, "storage", &mount_cfg, &wl_handle);
    if (err != ESP_OK)
        return err;
    s_flash_vfs_early = true;
    espd_storage_refresh_paths();
    return ESP_OK;
}

static esp_err_t espd_usb_unmount_flash_early_vfs(void)
{
    if (!s_flash_vfs_early)
        return ESP_OK;
    esp_err_t err = esp_vfs_fat_spiflash_unmount_rw_wl(ESPD_STORAGE_MOUNT, wl_handle);
    s_flash_vfs_early = false;
    wl_handle = WL_INVALID_HANDLE;
    return err;
}

static esp_err_t espd_usb_msc_driver_ensure(void)
{
    tinyusb_msc_driver_config_t msc_drv_cfg = {
        /* Never auto-hand FAT to the host on plug (steals APP mount → main.pd EBADF). */
        .user_flags.auto_mount_off = 1,
    };
    esp_err_t err = tinyusb_msc_install_driver(&msc_drv_cfg);
    if (err == ESP_OK || err == ESP_ERR_INVALID_STATE)
        return ESP_OK;
    ESP_LOGW(TAG, "USB: MSC driver install: %s", esp_err_to_name(err));
    return err;
}

/* Mount /storage for app (MSC backend, APP mount point). */
static esp_err_t espd_usb_mount_storage_app(bool msc_sync_mode)
{
    const esp_partition_t *data_partition;
    esp_err_t err;

    if (msc_handle != NULL) {
        return ESP_OK;
    }

    if (s_flash_vfs_early) {
        err = espd_usb_unmount_flash_early_vfs();
        if (err != ESP_OK)
            return err;
    }

    err = espd_usb_msc_driver_ensure();
    if (err != ESP_OK)
        return err;

    data_partition = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_FAT, "storage");
    if (!data_partition) {
        ESP_LOGE(TAG, "USB: 'storage' partition not found");
        return ESP_ERR_NOT_FOUND;
    }

    if (wl_handle == WL_INVALID_HANDLE) {
        err = wl_mount(data_partition, &wl_handle);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "USB: wear levelling failed: %s", esp_err_to_name(err));
            return err;
        }
    }

    {
        tinyusb_msc_storage_config_t msc_storage_cfg = {
            .medium.wl_handle = wl_handle,
            .fat_fs = {
                .base_path = ESPD_STORAGE_MOUNT,
                .config = {
                    .max_files = 64,
                    /* Recover automatically after partition-layout changes. */
                    .format_if_mount_failed = true,
                    .allocation_unit_size = CONFIG_WL_SECTOR_SIZE,
                },
                .do_not_format = false,
                .format_flags = FM_FAT,
            },
            .mount_point = TINYUSB_MSC_STORAGE_MOUNT_APP,
        };
        err = tinyusb_msc_new_storage_spiflash(&msc_storage_cfg, &msc_handle);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "USB: MSC storage failed: %s", esp_err_to_name(err));
            return err;
        }
    }

    espd_storage_refresh_paths();
    if (msc_sync_mode) {
        ESP_LOGI(TAG, "USB: /storage APP-only (msc_sync — host MSC hidden)");
    } else {
        ESP_LOGI(TAG, "USB: /storage APP-only (normal)");
    }
    espd_usb_apply_msc_volume_label_when_ready();
    return ESP_OK;
}

bool espd_usb_msc_storage_present(void)
{
    return msc_handle != NULL;
}

bool espd_usb_msc_host_mounted(void)
{
    tinyusb_msc_mount_point_t mp;

    if (!msc_handle)
        return false;
    if (tinyusb_msc_get_storage_mount_point(msc_handle, &mp) != ESP_OK)
        return false;
    return mp == TINYUSB_MSC_STORAGE_MOUNT_USB;
}

esp_err_t espd_usb_expose_msc_to_host(void)
{
    if (!msc_handle)
        return ESP_ERR_INVALID_STATE;
    if (espd_usb_msc_sync_mode_active())
        return ESP_ERR_INVALID_STATE;
    if (espd_usb_msc_host_mounted())
        return ESP_OK;
    return tinyusb_msc_set_storage_mount_point(msc_handle,
        TINYUSB_MSC_STORAGE_MOUNT_USB);
}

esp_err_t espd_usb_ensure_msc_app_mount(void)
{
    if (!msc_handle)
        return ESP_ERR_INVALID_STATE;
    if (!espd_usb_msc_host_mounted())
        return ESP_OK;
    return tinyusb_msc_set_storage_mount_point(msc_handle,
        TINYUSB_MSC_STORAGE_MOUNT_APP);
}
#endif

#if CONFIG_ESPD_USE_USB_OTG && CONFIG_ESPD_DEV_CDC_SYNC
static SemaphoreHandle_t s_usb_cdc_tx_mux;

/* TX-only CDC; espd_dev owns RX. Mutex: esp_log and +OK lines must not interleave. */
void espd_usb_cdc_write(const void *data, size_t len)
{
    size_t off = 0;
    TickType_t deadline;

    if (!data || len == 0 || !tinyusb_cdcacm_initialized(TINYUSB_CDC_ACM_0))
        return;
    if (!s_usb_cdc_tx_mux) {
        s_usb_cdc_tx_mux = xSemaphoreCreateMutex();
        if (!s_usb_cdc_tx_mux)
            return;
    }
    if (xSemaphoreTake(s_usb_cdc_tx_mux, pdMS_TO_TICKS(500)) != pdTRUE)
        return;

    deadline = xTaskGetTickCount() + pdMS_TO_TICKS(200);
    while (off < len) {
        size_t w = tinyusb_cdcacm_write_queue(TINYUSB_CDC_ACM_0,
                (const uint8_t *)data + off, len - off);
        if (w == 0) {
            if (xTaskGetTickCount() >= deadline)
                break;
            vTaskDelay(1);
            continue;
        }
        off += w;
        if (off < len
                && tinyusb_cdcacm_write_flush(TINYUSB_CDC_ACM_0, 0) != ESP_OK)
            break;
    }
    (void)tinyusb_cdcacm_write_flush(TINYUSB_CDC_ACM_0, 0);
    xSemaphoreGive(s_usb_cdc_tx_mux);
}

static void espd_usb_cdc_write_bytes(const char *data, size_t len)
{
    espd_usb_cdc_write(data, len);
}

static int espd_usb_cdc_log_vprintf(const char *fmt, va_list args)
{
    char buf[256];
    int n = vsnprintf(buf, sizeof(buf), fmt, args);
    if (n > 0) {
        size_t w = (size_t)n;
        if (w >= sizeof(buf))
            w = sizeof(buf) - 1;
        espd_usb_cdc_write_bytes(buf, w);
    }
    return n;
}
#endif

static void espd_usb_release_usj_for_otg(void)
{
#if CONFIG_ESPD_USE_USB_OTG && CONFIG_SOC_USB_SERIAL_JTAG_SUPPORTED \
        && CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG_ENABLED
    fflush(stdout);
    fflush(stderr);
    (void)usb_serial_jtag_driver_uninstall();
#endif
}

static bool usb_init_on_core0(void)
{
    esp_err_t err;
    tinyusb_config_t tusb_cfg;
    bool msc_sync_mode = false;

    espd_usb_release_usj_for_otg();

#if CONFIG_ESPD_USE_USB_MSC
    msc_sync_mode = espd_usb_msc_sync_mode_active();
    if (espd_usb_msc_driver_ensure() != ESP_OK)
        ESP_LOGW(TAG, "USB: MSC driver pre-install failed");
#endif

    tusb_cfg = TINYUSB_DEFAULT_CONFIG();
    tusb_cfg.task = TINYUSB_TASK_CUSTOM(
        TINYUSB_DEFAULT_TASK_SIZE, ESPD_USB_DEVICE_TASK_PRIO, ESPD_USB_TASK_CORE);
    err = tinyusb_driver_install(&tusb_cfg);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "USB: TinyUSB install failed: %s", esp_err_to_name(err));
        return false;
    }

#if CONFIG_ESPD_DEV_CDC_SYNC
    tinyusb_config_cdcacm_t acm_cfg = {
        .cdc_port = TINYUSB_CDC_ACM_0,
        .callback_rx = espd_dev_cdc_rx_cb,
    };
#else
    tinyusb_config_cdcacm_t acm_cfg = {
        .cdc_port = TINYUSB_CDC_ACM_0,
    };
#endif
    err = tinyusb_cdcacm_init(&acm_cfg);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "USB: CDC init: %s", esp_err_to_name(err));
        return false;
    }

#if CONFIG_ESPD_DEV_CDC_SYNC
    espd_dev_init();
    esp_log_set_vprintf(espd_usb_cdc_log_vprintf);
#elif CONFIG_ESPD_USB_CONSOLE_CDC && CONFIG_ESPD_USE_CONSOLE
    err = tinyusb_console_init(TINYUSB_CDC_ACM_0);
    if (err != ESP_OK)
        ESP_LOGW(TAG, "USB: CDC console: %s", esp_err_to_name(err));
    else
        ESP_LOGI(TAG, "USB: logs on CDC (cu.usbmodem*)");
#elif CONFIG_ESPD_USE_USB_MSC
    ESP_LOGW(TAG, "USB: MSC only — enable OTG CDC for serial logs");
#endif

#if CONFIG_ESPD_USE_USB_MSC
    if (msc_handle == NULL) {
        err = espd_usb_mount_storage_app(msc_sync_mode);
        if (err != ESP_OK)
            ESP_LOGW(TAG, "USB: /storage mount failed: %s", esp_err_to_name(err));
    }
#endif
#if CONFIG_ESPD_DEV_CDC_SYNC
    if (msc_sync_mode)
        ESP_LOGI(TAG, "USB: cu.usbmodem%s1 (CDC, msc_sync)",
            CONFIG_TINYUSB_DESC_SERIAL_STRING);
    else
        ESP_LOGI(TAG, "USB: cu.usbmodem%s1 (CDC+MSC, APP mount)",
            CONFIG_TINYUSB_DESC_SERIAL_STRING);
#else
    ESP_LOGI(TAG, "USB: ready (cu.usbmodem%s1)", CONFIG_TINYUSB_DESC_SERIAL_STRING);
#endif
    return true;
}

#define ESPD_USB_BOOT_STACK        10240
#define ESPD_USB_BOOT_TIMEOUT_MS   15000

/* app_main is on CPU1; TinyUSB must install on CPU0. Notify waiter when done. */
static void usb_boot_task(void *arg)
{
    TaskHandle_t waiter = (TaskHandle_t)arg;
    bool ok = usb_init_on_core0();

    if (waiter)
        xTaskNotify(waiter, ok ? 1 : 0, eSetValueWithOverwrite);
    vTaskDelete(NULL);
}

static bool espd_usb_start_after_wifi(void)
{
    static bool started;
    TaskHandle_t waiter;
    uint32_t note = 0;

    if (started)
        return true;
    started = true;

    waiter = xTaskGetCurrentTaskHandle();
    if (xTaskCreatePinnedToCore(usb_boot_task, "usb_otg", ESPD_USB_BOOT_STACK, waiter,
            6, NULL, ESPD_USB_TASK_CORE) != pdPASS) {
        ESP_LOGE(TAG, "USB: task create failed");
        return false;
    }
    if (xTaskNotifyWait(0, UINT32_MAX, &note,
            pdMS_TO_TICKS(ESPD_USB_BOOT_TIMEOUT_MS)) != pdTRUE) {
        ESP_LOGE(TAG, "USB: init timeout (%d ms)", ESPD_USB_BOOT_TIMEOUT_MS);
        return false;
    }
    return note != 0;
}
#endif

#if !CONFIG_ESPD_USE_USB_OTG
bool espd_usb_msc_sync_mode_active(void) { return false; }
void espd_usb_msc_sync_mode_set(bool active) { (void)active; }
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
static espd_audio_t *s_audio;
#endif /* OBSOLETEAPI */

#define BLKSIZE 64
DRAM_ATTR float soundin[IOCHANS * BLKSIZE], soundout[IOCHANS * BLKSIZE];
DRAM_ATTR short poodle[IOCHANS * BLKSIZE];

static inline short espd_soundout_to_short(float x)
{
    if (x > 1.f)
        x = 1.f;
    else if (x < -1.f)
        x = -1.f;
    short y = (short)lrintf(x * 32768.f);
    if (y > 32767)
        y = 32767;
    else if (y < -32768)
        y = -32768;
    return y;
}

static inline float espd_soundin_from_i16(int16_t s)
{
    return (float)s * (1.f / 32768.f);
}

/*static inline float espd_soundin_from_u16(uint32_t u)
{
    const float inv32768 = 1.f / 32768.f;
    float x = (float)(u & 0xffffu) * inv32768;
    if (u & 0x8000u)
        x -= 2.f;
    return x;
}*/

void senddacs( void)
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
#ifdef OBSOLETEAPI
    {
        size_t transferred;
        int ret = i2s_write(I2S_NUM_0, poodle, sizeof(poodle), &transferred,
            portMAX_DELAY);
        if (ret != ESP_OK)
            ESP_LOGE(TAG, "error writing");
    }
#else
    err = espd_audio_write(s_audio, poodle, (size_t)(IOCHANS * BLKSIZE));
    if (err != ESP_OK)
        ESP_LOGE(TAG, "audio write failed: %s", esp_err_to_name(err));
#endif

#ifdef ESPD_USE_ADC
#ifdef OBSOLETEAPI
    {
        size_t transferred;
        int ret = i2s_read(I2S_NUM_0, poodle, sizeof(poodle), &transferred,
            portMAX_DELAY);
        if (ret != ESP_OK)
            ESP_LOGE(TAG, "error reading");
    }
#else
    err = espd_audio_read(s_audio, poodle, (size_t)(IOCHANS * BLKSIZE));
    if (err != ESP_OK && err != ESP_ERR_NOT_SUPPORTED)
        ESP_LOGE(TAG, "audio read failed: %s", esp_err_to_name(err));
#endif
    
    for (i = j = 0; i < BLKSIZE; i++, j += IOCHANS)
    {
        soundin[i] = espd_soundin_from_i16(poodle[j]);
    #if IOCHANS > 1
        soundin[i + BLKSIZE] = espd_soundin_from_i16(poodle[j + 1]);
    #endif
    }
#endif /* ESPD_USE_ADC */
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
#ifdef ESPD_USE_ADC
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
    esp_err_t e = espd_audio_init(&s_audio);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "audio init failed: %s — no sound output", esp_err_to_name(e));
        s_audio = NULL;
        return;
    }
    memset(soundout, 0, sizeof(soundout));
    memset(soundin, 0, sizeof(soundin));
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

#ifdef ESPD_USE_CONSOLE
static QueueHandle_t uart_queue;
static void console_init(void)
{
    (void)uart_queue;
#if !CONFIG_ESPD_USB_CONSOLE_CDC
    /* UART console: host->Pd UART input disabled (boot crashes if we install
     * a second driver). Output still goes via printf / pdmain_print. */
#endif
}
#endif

    /* dispatch messages enqueued above */
void pd_pollhost( void)
{
    int lastchar;
#ifdef ESPD_USE_CONSOLE
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
    int broadcast_only = 0;
#ifdef ESPD_USE_WIFI
    if (espd_log_broadcast_port > 0 && espd_wifi_net_enabled && wifi_ipaddr[0] != '\0')
        broadcast_only = 1;
#endif
    if (s && *s && !broadcast_only) {
#if CONFIG_ESPD_USE_USB_OTG && CONFIG_ESPD_DEV_CDC_SYNC
        if (tinyusb_cdcacm_initialized(TINYUSB_CDC_ACM_0))
            espd_usb_cdc_write_bytes(s, strlen(s));
        else
#endif
            printf("%s", s);
    }
    strncpy(y, s, 79);
    y[79]=0;
    strcat(y, ";");
#if defined(ESPD_USE_WIFI) && ESPD_ENABLE_LEGACY_WIFI_TRANSPORT
    if (espd_wifi_net_enabled && wifi_ipaddr[0] != '\0') {
        if (espd_log_broadcast_port > 0)
            net_sendudp(y, strlen(y), espd_log_broadcast_port);
        else {
            net_sendudp(y, strlen(y), CONFIG_ESP_WIFI_SENDPORT);
            net_sendtcp(y, strlen(y));
        }
    }
#endif
}

void trymem(int foo);

/* [cputime]: summed wall µs per audio loop for polls + pdmain_tick + net_alive
 * only (senddacs excluded — esp_timer is not CPU time and I2S often blocks). */
static unsigned int cputime;

void espd_cputime_reset(void)
{
    cputime = 0;
}

unsigned int espd_cputime_get(void)
{
    return cputime;
}

void app_main(void)
{
    esp_log_level_set("*", ESP_LOG_INFO);
    esp_log_level_set(TAG, ESP_LOG_INFO);

    espd_nvs_flash_init();
    espd_board_early_init();
    espd_storage_init();

#if CONFIG_ESPD_USE_USB_MSC
    espd_usb_msc_sync_clear_unless_sw_reset();
    /* /storage for config.txt before USB (both normal and msc_sync boots). */
    if (!espd_storage_sdcard_ready()) {
        esp_err_t mnt = espd_usb_mount_flash_early_vfs();
        if (mnt != ESP_OK)
            ESP_LOGW(TAG, "USB: early /storage VFS failed: %s", esp_err_to_name(mnt));
        espd_storage_refresh_paths();
    }
#endif

#ifdef ESPD_USE_WIFI
    espd_wifi_config_defaults();
    espd_wifi_try_load_config();
    if (!espd_wifi_config_txt_allows_sta())
        espd_wifi_net_enabled = 0;
#endif

    /* Wi‑Fi PHY before OTG; start STA early so DHCP overlaps USB bring-up. */
#ifdef ESPD_USE_WIFI
    wifi_prepare_phy();
    if (espd_wifi_net_enabled && !espd_wifi_started) {
        wifi_start_sta();
        espd_wifi_started = 1;
    }
#endif
#if CONFIG_ESPD_USE_USB_OTG
    if (!espd_usb_start_after_wifi())
        ESP_LOGE(TAG, "USB: boot init failed");
#endif

#if CONFIG_ESPD_USE_USB_MSC
    if (!espd_storage_sdcard_ready() && !espd_storage_flash_ready()) {
        esp_err_t mnt = espd_usb_mount_flash_early_vfs();
        if (mnt == ESP_OK) {
            ESP_LOGW(TAG, "USB: MSC unavailable — /storage on early VFS");
            espd_storage_refresh_paths();
        }
    }
#endif

#if CONFIG_ESP_MAIN_TASK_STACK_SIZE < 16384
    ESP_LOGW(TAG,
        "Main task stack is %d bytes — too small for Pd (use >= 32768, "
        "65536 for FFT-heavy patches). menuconfig → Component config → "
        "ESP System Settings → Main task stack size",
        CONFIG_ESP_MAIN_TASK_STACK_SIZE);
#endif

    heap_caps_malloc_extmem_enable(16384);

    {
        esp_pthread_cfg_t pth_cfg = esp_pthread_get_default_config();
        pth_cfg.stack_size = 8192;
        pth_cfg.prio = 5;
        pth_cfg.pin_to_core = 0;
        pth_cfg.stack_alloc_caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
        if (esp_pthread_set_cfg(&pth_cfg) != ESP_OK)
            ESP_LOGW(TAG, "esp_pthread_set_cfg failed; using IDF defaults");
    }

#ifdef ESPD_USE_SDCARD
    espd_storage_mount_sdcard();
#endif
    espd_storage_refresh_paths();

#ifdef ESPD_USE_AOUT
    espd_aout_load_config();
#endif
#ifdef ESPD_USE_DOUT
    espd_dout_load_config();
#endif
#ifdef ESPD_USE_DIN
    espd_din_load_config();
#endif
#ifdef ESPD_USE_AIN
    espd_ain_load_config();
#endif
#ifdef ESPD_USE_TOUCH
    espd_touch_load_config();
#endif
    espd_audio_load_config();

    initdacs();

    espd_board_init();
#ifdef ESPD_USE_DIN
    espd_din_gpio_init();
#endif
    espd_io_log_din_map();

#ifdef ESPD_USE_WIFI
    if (espd_wifi_net_enabled) {
        (void)wifi_wait_sta(pdMS_TO_TICKS(20000));
#if ESPD_ENABLE_LEGACY_WIFI_TRANSPORT
        net_init();
        net_hello();
#else
        ESP_LOGI(TAG, "legacy espd TCP/UDP transport disabled (use Pd net objects)");
#endif
    }
#endif

    pdmain_init();

#if CONFIG_ESPD_USE_USB_MSC
    /* Normal: USB mass-storage on host after Pd boot; dev sync reclaims APP for PUT. */
    if (!espd_usb_msc_sync_mode_active() && espd_usb_msc_storage_present()) {
        esp_err_t exp = espd_usb_expose_msc_to_host();
        if (exp == ESP_OK)
            ESP_LOGI(TAG, "USB: /storage exposed to host (normal)");
        else
            ESP_LOGW(TAG, "USB: host MSC expose: %s", esp_err_to_name(exp));
    }
#endif

#ifdef ESPD_USE_AIN
    espd_ain_init();
#endif
#ifdef ESPD_USE_TOUCH
    espd_touch_init();
#endif
#ifdef ESPD_USE_CONSOLE
    console_init();
#endif

    ESP_LOGI(TAG, "entering audio block loop");

    /* Audio loop priority: bump well above any non-critical CPU1 task.
     * IDF defaults: pthread workers / TinyUSB MSC ≈ 5, esp_timer 22 (CPU0),
     * WiFi 23 (CPU0), IPC 24. Picking 19 keeps us strictly above every
     * userland/system task that might land on CPU1, while staying below
     * esp_timer/IPC. Defensive clamp in case configMAX_PRIORITIES has been
     * trimmed down on a board build. */
    {
        UBaseType_t was = uxTaskPriorityGet(NULL);
        UBaseType_t want = 19;
        if (want > (UBaseType_t)(configMAX_PRIORITIES - 2))
            want = (UBaseType_t)(configMAX_PRIORITIES - 2);
        vTaskPrioritySet(NULL, want);
        ESP_LOGI(TAG, "audio loop priority set to %u (was %u)",
            (unsigned)want, (unsigned)was);
    }

    while (1)
    {
        uint64_t t0 = (uint64_t)esp_timer_get_time();
        pd_pollhost();
#ifdef ESPD_USE_AIN
        espd_ain_poll();
#endif
#ifdef ESPD_USE_TOUCH
        espd_touch_poll();
#endif
        espd_io_poll();

#if CONFIG_ESPD_DEV_CDC_SYNC
        if (espd_dev_reload_pending()) {
            pdmain_reload_patch_from(espd_dev_reload_dir());
            espd_dev_clear_reload_pending();
        }
        {
            char pdmsg[272];
            if (espd_dev_pdmsg_take(pdmsg, sizeof(pdmsg))) {
                size_t n = strlen(pdmsg);
                if (n > 0 && pdmsg[n - 1] != ';' && n + 1 < sizeof(pdmsg)) {
                    pdmsg[n++] = ';';
                    pdmsg[n] = '\0';
                }
                if (n > 0)
                    pd_sendmsg(pdmsg, (int)n);
            }
        }
#endif

        pdmain_tick();
#ifdef ESPD_USE_WIFI
#if ESPD_ENABLE_LEGACY_WIFI_TRANSPORT
        if (espd_wifi_net_enabled)
            net_alive();
#endif
#endif
        cputime += (unsigned int)((uint64_t)esp_timer_get_time() - t0);
        senddacs();
    }
}

void espd_control_io_init(void)
{
#ifdef ESPD_USE_AOUT
    espd_aout_init();
#endif
#ifdef ESPD_USE_DOUT
    espd_dout_init();
#endif
    espd_io_bind();
}

#ifdef ESPD_USE_SDCARD
void sd_init( void)
{
#ifdef PD_LYRAT
    ESP_LOGI(TAG, "[ 1 ] Mount sdcard");
    {
        esp_periph_config_t periph_cfg = DEFAULT_ESP_PERIPH_SET_CONFIG();
        esp_periph_set_handle_t set = esp_periph_set_init(&periph_cfg);
        audio_board_sdcard_init(set, SD_MODE_1_LINE);
    }
    ESP_LOGI(TAG, "[ 1b ] done starting network");
#else
    /* Idempotent; first call is before pdmain_init() in app_main. */
    (void)espd_storage_mount_sdcard();
#endif
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

void glob_mem(void *dummy)
{
    (void)dummy;
    espd_print_memdiag();
}



