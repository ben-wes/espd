/*
 * GPIO Peripherals: Hardware Initialization and Drivers.
 * Config comes from g_espd_cfg.
 */

#include "espd_gpio_peripherals.h"
#include "espd.h"
#include "espd_config_file.h"
#include "espd_pd_io.h"
#include "bsp/bsp_io.h"
#include "../pd/src/m_pd.h"

#include "esp_log.h"
#include "driver/gpio.h"
#include <string.h>
#include <stdlib.h>

#if defined(ESPD_USE_AIN) || defined(ESPD_USE_TOUCH)
#include <stdatomic.h>
#endif
#ifdef ESPD_USE_AIN
#include "esp_adc/adc_oneshot.h"
#endif
#ifdef ESPD_USE_TOUCH
#include "driver/touch_sensor.h"
#endif
#ifdef ESPD_USE_AOUT
#include "driver/ledc.h"
#endif

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "espd_gpio_peripherals";

/* ─── AOUT (PWM) ─── */

#ifdef ESPD_USE_AOUT
#define ESPD_AOUT_MAX_CHANNELS 4
#define ESPD_AOUT_PWM_RES LEDC_TIMER_12_BIT
#define ESPD_AOUT_PWM_MAX_DUTY ((1u << 12) - 1u)

typedef struct _espd_aout_receiver {
    t_pd x_pd;
    int idx;
} t_espd_aout_receiver;

static t_class *espd_aout_receiver_class;
static int espd_aout_pins[ESPD_AOUT_MAX_CHANNELS];
static int espd_aout_active[ESPD_AOUT_MAX_CHANNELS];
static t_espd_aout_receiver espd_aout_receivers[ESPD_AOUT_MAX_CHANNELS];

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

void espd_aout_init(void)
{
    int pwm_freq = (g_espd_cfg.aout_pwm_freq_hz >= 0)
        ? g_espd_cfg.aout_pwm_freq_hz : ESPD_AOUT_PWM_FREQ_HZ;
    ledc_timer_config_t timer_cfg = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = ESPD_AOUT_PWM_RES,
        .timer_num = LEDC_TIMER_0,
        .freq_hz = pwm_freq,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    int i, nchan, enabled = 0;

    if (!g_espd_cfg.aout_have_pins || g_espd_cfg.aout_n <= 0)
        return;
    nchan = g_espd_cfg.aout_n;
    if (nchan > ESPD_AOUT_MAX_CHANNELS)
        nchan = ESPD_AOUT_MAX_CHANNELS;
    for (i = 0; i < nchan; i++)
        espd_aout_pins[i] = g_espd_cfg.aout_pins[i];
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
                     pwm_freq, ESPD_AOUT_PWM_FALLBACK_FREQ_HZ);
            return;
        }
        ESP_LOGW(TAG, "aout: %d Hz unavailable at %d-bit PWM; using %d Hz",
                 pwm_freq, 12, ESPD_AOUT_PWM_FALLBACK_FREQ_HZ);
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
#else
void espd_aout_init(void) {}
#endif

/* ─── DOUT (GPIO) ─── */

#ifdef ESPD_USE_DOUT
#define ESPD_DOUT_MAX_CHANNELS 8

typedef struct _espd_dout_receiver {
    t_pd x_pd;
    int idx;
} t_espd_dout_receiver;

static t_class *espd_dout_receiver_class;
static int espd_dout_pins[ESPD_DOUT_MAX_CHANNELS];
static int espd_dout_active[ESPD_DOUT_MAX_CHANNELS];
static t_espd_dout_receiver espd_dout_receivers[ESPD_DOUT_MAX_CHANNELS];

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

void espd_dout_init(void)
{
    int i, nchan, enabled = 0;

    if (!g_espd_cfg.dout_have_pins || g_espd_cfg.dout_n <= 0)
        return;
    nchan = g_espd_cfg.dout_n;
    if (nchan > ESPD_DOUT_MAX_CHANNELS)
        nchan = ESPD_DOUT_MAX_CHANNELS;

    for (i = 0; i < ESPD_DOUT_MAX_CHANNELS; i++)
        espd_dout_active[i] = 0;

    for (i = 0; i < nchan; i++)
    {
        int pin = g_espd_cfg.dout_pins[i];
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
#else
void espd_dout_init(void) {}
#endif

/* ─── DIN (GPIO, appended after BSP buttons) ─── */

#ifdef ESPD_USE_DIN
#define ESPD_DIN_GPIO_MAX_CHANNELS 8

static int espd_din_gpio_active_low = 1;
static int espd_din_gpio_task_period_ms = ESPD_DIN_GPIO_TASK_PERIOD_MS;
static int espd_din_gpio_pins[ESPD_DIN_GPIO_MAX_CHANNELS];
static int espd_din_gpio_active[ESPD_DIN_GPIO_MAX_CHANNELS];
static int espd_din_gpio_last_raw[ESPD_DIN_GPIO_MAX_CHANNELS];
static int espd_din_gpio_last_stable[ESPD_DIN_GPIO_MAX_CHANNELS];
static int espd_din_gpio_last_reported[ESPD_DIN_GPIO_MAX_CHANNELS];
static volatile int espd_din_gpio_dirty;
static TaskHandle_t espd_din_gpio_task_handle;

static int espd_din_gpio_read_level(int pin)
{
    int level = gpio_get_level(pin);
    if (espd_din_gpio_active_low)
        return level ? 0 : 1;
    return level ? 1 : 0;
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
        for (i = 0; i < g_espd_cfg.din_n; i++)
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

void espd_din_gpio_init(void)
{
    int i, enabled = 0;

    if (!g_espd_cfg.din_have_pins || g_espd_cfg.din_n <= 0)
        return;

    if (g_espd_cfg.din_active_low >= 0)
        espd_din_gpio_active_low = g_espd_cfg.din_active_low;
    if (g_espd_cfg.din_task_period_ms >= 0)
        espd_din_gpio_task_period_ms = g_espd_cfg.din_task_period_ms;

    for (i = 0; i < ESPD_DIN_GPIO_MAX_CHANNELS; i++)
    {
        espd_din_gpio_active[i] = 0;
        espd_din_gpio_last_raw[i] = -1;
        espd_din_gpio_last_stable[i] = -1;
        espd_din_gpio_last_reported[i] = -1;
    }

    for (i = 0; i < g_espd_cfg.din_n; i++)
    {
        int pin = g_espd_cfg.din_pins[i];
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

    if (!espd_din_gpio_task_handle)
    {
        BaseType_t ok = xTaskCreatePinnedToCore(espd_din_gpio_task_fn,
            "espd_din", 3072, NULL, ESPD_DIN_GPIO_TASK_PRIO,
            &espd_din_gpio_task_handle, ESPD_DIN_GPIO_TASK_CORE);
        if (ok != pdPASS)
        {
            ESP_LOGW(TAG, "failed to create espd_din task");
            espd_din_gpio_task_handle = NULL;
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
    for (i = 0; i < g_espd_cfg.din_n; i++)
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
#else
void espd_din_gpio_init(void) {}
void espd_din_gpio_poll(void) {}
#endif

void espd_din_log_map(void)
{
    int bsp_n = bsp_button_count();
    int i;

    for (i = 0; i < bsp_n; i++)
        ESP_LOGI(TAG, "espd/din/%d  board (BSP digital in)", i);
#ifdef ESPD_USE_DIN
    for (i = 0; i < g_espd_cfg.din_n; i++)
    {
        if (!espd_din_gpio_active[i])
            continue;
        ESP_LOGI(TAG, "espd/din/%d  GPIO%d (din_pins[%d], active-%s)",
            bsp_n + i, espd_din_gpio_pins[i], i,
            espd_din_gpio_active_low ? "low" : "high");
    }
#endif
}

/* ─── AIN (ADC) ─── */

#ifdef ESPD_USE_AIN
#define ESPD_AIN_MAX_CHANNELS 8

static adc_oneshot_unit_handle_t espd_ain_adc_handle;
static int espd_ain_adc_pins[ESPD_AIN_MAX_CHANNELS] = {
    ESPD_AIN_PIN_0, ESPD_AIN_PIN_1, ESPD_AIN_PIN_2, ESPD_AIN_PIN_3,
    ESPD_AIN_PIN_4, ESPD_AIN_PIN_5, ESPD_AIN_PIN_6, ESPD_AIN_PIN_7
};
static adc_channel_t espd_ain_adc_channels[ESPD_AIN_MAX_CHANNELS];
static int espd_ain_adc_active[ESPD_AIN_MAX_CHANNELS];
static int espd_ain_adc_last[ESPD_AIN_MAX_CHANNELS];
static _Atomic int espd_ain_adc_latest[ESPD_AIN_MAX_CHANNELS];
static _Atomic uint32_t espd_ain_adc_dirty;
static TaskHandle_t espd_ain_adc_task;
static unsigned espd_ain_block_counter;

static int espd_ain_task_period_ms;
static int espd_ain_deadband;
static int espd_ain_report_every_n_blocks;

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
    if (v < 0) v = 0;
    else if (v > 1) v = 1;
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

void espd_ain_init(void)
{
    adc_oneshot_unit_init_cfg_t unit_cfg = {
        .unit_id = ADC_UNIT_1,
        .ulp_mode = ADC_ULP_MODE_DISABLE,
    };
    adc_oneshot_chan_cfg_t chan_cfg = {
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    int i, nchan, enabled = 0;

    espd_ain_task_period_ms = (g_espd_cfg.ain_task_period_ms >= 0)
        ? g_espd_cfg.ain_task_period_ms : ESPD_AIN_TASK_PERIOD_MS;
    espd_ain_deadband = (g_espd_cfg.ain_deadband >= 0)
        ? g_espd_cfg.ain_deadband : ESPD_AIN_DEADBAND;
    espd_ain_report_every_n_blocks = (g_espd_cfg.ain_report_every >= 0)
        ? g_espd_cfg.ain_report_every : ESPD_AIN_REPORT_EVERY_N_BLOCKS;

    if (!g_espd_cfg.ain_have_pins || g_espd_cfg.ain_n <= 0)
        return;
    nchan = g_espd_cfg.ain_n;
    if (nchan > ESPD_AIN_MAX_CHANNELS)
        nchan = ESPD_AIN_MAX_CHANNELS;
    for (i = 0; i < nchan; i++)
        espd_ain_adc_pins[i] = g_espd_cfg.ain_pins[i];
    for (i = nchan; i < ESPD_AIN_MAX_CHANNELS; i++)
        espd_ain_adc_pins[i] = -1;

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

void espd_ain_poll(void)
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
#else
void espd_ain_init(void) {}
void espd_ain_poll(void) {}
#endif

/* ─── TOUCH ─── */

#ifdef ESPD_USE_TOUCH
#define ESPD_TOUCH_MAX_CHANNELS 8
static touch_pad_t espd_touch_channels[ESPD_TOUCH_MAX_CHANNELS];
static int espd_touch_pins[ESPD_TOUCH_MAX_CHANNELS];
static int espd_touch_active[ESPD_TOUCH_MAX_CHANNELS];
static uint32_t espd_touch_last[ESPD_TOUCH_MAX_CHANNELS];
static _Atomic uint32_t espd_touch_latest[ESPD_TOUCH_MAX_CHANNELS];
static _Atomic uint32_t espd_touch_dirty;
static TaskHandle_t espd_touch_task_handle;
static unsigned espd_touch_block_counter;
static int espd_touch_inited;

static int espd_touch_task_period_ms;
static int espd_touch_report_every_n_blocks;

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

void espd_touch_init(void)
{
    int i, nchan, enabled = 0;

    espd_touch_task_period_ms = (g_espd_cfg.touch_task_period_ms >= 0)
        ? g_espd_cfg.touch_task_period_ms : ESPD_TOUCH_TASK_PERIOD_MS;
    espd_touch_report_every_n_blocks = (g_espd_cfg.touch_report_every >= 0)
        ? g_espd_cfg.touch_report_every : ESPD_TOUCH_REPORT_EVERY_N_BLOCKS;

    if (!g_espd_cfg.touch_have_pins || g_espd_cfg.touch_n <= 0)
        return;
    nchan = g_espd_cfg.touch_n;
    if (nchan > ESPD_TOUCH_MAX_CHANNELS)
        nchan = ESPD_TOUCH_MAX_CHANNELS;
    for (i = 0; i < nchan; i++)
        espd_touch_pins[i] = g_espd_cfg.touch_pins[i];
    for (i = nchan; i < ESPD_TOUCH_MAX_CHANNELS; i++)
        espd_touch_pins[i] = -1;

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
    if (!espd_touch_task_handle) {
        BaseType_t ok = xTaskCreatePinnedToCore(espd_touch_task_fn,
            "espd_touch", 3072, NULL, ESPD_TOUCH_TASK_PRIO,
            &espd_touch_task_handle, ESPD_TOUCH_TASK_CORE);
        if (ok != pdPASS) {
            ESP_LOGW(TAG, "failed to create espd_touch task; falling back to audio-thread polling");
            espd_touch_task_handle = NULL;
        } else {
            ESP_LOGI(TAG, "espd_touch task: core=%d prio=%d period=%d ms report_every=%d %d ch",
                (int)ESPD_TOUCH_TASK_CORE, (int)ESPD_TOUCH_TASK_PRIO,
                espd_touch_task_period_ms, espd_touch_report_every_n_blocks, enabled);
        }
    }
}

void espd_touch_poll(void)
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

    if (!espd_touch_task_handle)
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
#else
void espd_touch_init(void) {}
void espd_touch_poll(void) {}
#endif
