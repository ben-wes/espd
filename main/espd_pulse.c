/*
 * RMT-backed pulse generator for [espdpulse].
 *
 * Timing is hardware-driven (not Pd audio rate). One RMT TX channel per
 * pulse_pins= GPIO. Each period is a single RMT symbol (high then low);
 * finite / infinite repeats use loop_count when the SoC supports it.
 */

#include "espd_pulse.h"
#include "espd_config.h"
#include "espd_config_file.h"

#include <driver/rmt_tx.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <inttypes.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <soc/soc_caps.h>

#ifdef ESPD_USE_PULSE

static const char *TAG = "espd_pulse";

#define ESPD_PULSE_RESOLUTION_HZ 1000000u /* 1 tick = 1 µs */
#define ESPD_PULSE_MAX_DURATION  32767u   /* 15-bit RMT duration field */
#define ESPD_PULSE_MIN_RATE_HZ   20.0f
#define ESPD_PULSE_MAX_RATE_HZ   250000.0f
/* Fallback burst buffer when SOC lacks TX loop-count (classic ESP32). */
#define ESPD_PULSE_MAX_BURST     4096

typedef struct {
    rmt_channel_handle_t chan;
    rmt_encoder_handle_t encoder;
    rmt_symbol_word_t symbol;
    rmt_symbol_word_t *burst; /* optional multi-symbol payload */
    int burst_cap;
    float rate_hz;
    float duty;
    bool ready;
    bool continuous;
} espd_pulse_ch_t;

static espd_pulse_ch_t s_ch[ESPD_PULSE_MAX_CHANNELS];
static int s_n;

static void pulse_rebuild_symbol(espd_pulse_ch_t *c)
{
    float rate = c->rate_hz;
    float duty = c->duty;
    uint32_t period;
    uint32_t high;
    uint32_t low;

    if (rate < ESPD_PULSE_MIN_RATE_HZ)
        rate = ESPD_PULSE_MIN_RATE_HZ;
    if (rate > ESPD_PULSE_MAX_RATE_HZ)
        rate = ESPD_PULSE_MAX_RATE_HZ;
    if (duty < 0.f)
        duty = 0.f;
    if (duty > 1.f)
        duty = 1.f;

    period = (uint32_t)lroundf((float)ESPD_PULSE_RESOLUTION_HZ / rate);
    if (period < 2u)
        period = 2u;
    if (period > 2u * ESPD_PULSE_MAX_DURATION)
        period = 2u * ESPD_PULSE_MAX_DURATION;

    high = (uint32_t)lroundf((float)period * duty);
    if (duty > 0.f && high < 1u)
        high = 1u;
    if (duty < 1.f && high >= period)
        high = period - 1u;
    if (high > ESPD_PULSE_MAX_DURATION)
        high = ESPD_PULSE_MAX_DURATION;
    low = period - high;
    if (low < 1u)
        low = 1u;
    if (low > ESPD_PULSE_MAX_DURATION)
        low = ESPD_PULSE_MAX_DURATION;

    c->symbol.level0 = 1;
    c->symbol.duration0 = (uint16_t)high;
    c->symbol.level1 = 0;
    c->symbol.duration1 = (uint16_t)low;
}

static esp_err_t pulse_stop(espd_pulse_ch_t *c)
{
    esp_err_t err;

    if (!c->chan)
        return ESP_OK;
    err = rmt_disable(c->chan);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE)
        return err;
    err = rmt_enable(c->chan);
    c->continuous = false;
    return err;
}

static esp_err_t pulse_ensure_burst(espd_pulse_ch_t *c, int n)
{
    if (n <= 0)
        return ESP_ERR_INVALID_ARG;
    if (c->burst && c->burst_cap >= n)
        return ESP_OK;
    rmt_symbol_word_t *p = heap_caps_realloc(c->burst,
        (size_t)n * sizeof(rmt_symbol_word_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!p)
        return ESP_ERR_NO_MEM;
    c->burst = p;
    c->burst_cap = n;
    return ESP_OK;
}

static esp_err_t pulse_start(espd_pulse_ch_t *c, int32_t n)
{
    rmt_transmit_config_t tx = {
        .loop_count = 0,
        .flags = {
            .eot_level = 0,
            .queue_nonblocking = true,
        },
    };
    const void *payload = &c->symbol;
    size_t payload_bytes = sizeof(c->symbol);
    esp_err_t err;

    err = pulse_stop(c);
    if (err != ESP_OK)
        return err;

    pulse_rebuild_symbol(c);

    if (n < 0) {
        tx.loop_count = -1;
        c->continuous = true;
    } else if (n == 1) {
        tx.loop_count = 0;
        c->continuous = false;
#if SOC_RMT_SUPPORT_TX_LOOP_COUNT
    } else {
        tx.loop_count = (int)n;
        c->continuous = false;
#else
    } else {
        int i;
        if (n > ESPD_PULSE_MAX_BURST)
            return ESP_ERR_INVALID_SIZE;
        err = pulse_ensure_burst(c, (int)n);
        if (err != ESP_OK)
            return err;
        for (i = 0; i < (int)n; i++)
            c->burst[i] = c->symbol;
        payload = c->burst;
        payload_bytes = (size_t)n * sizeof(rmt_symbol_word_t);
        tx.loop_count = 0;
        c->continuous = false;
#endif
    }

    return rmt_transmit(c->chan, c->encoder, payload, payload_bytes, &tx);
}

void espd_pulse_init(void)
{
    int i;

    memset(s_ch, 0, sizeof(s_ch));
    s_n = 0;

    if (!g_espd_cfg.pulse_have_pins || g_espd_cfg.pulse_n <= 0) {
        ESP_LOGI(TAG, "off (no pulse_pins= in config.txt)");
        return;
    }

    s_n = g_espd_cfg.pulse_n;
    if (s_n > ESPD_PULSE_MAX_CHANNELS)
        s_n = ESPD_PULSE_MAX_CHANNELS;

    for (i = 0; i < s_n; i++) {
        espd_pulse_ch_t *c = &s_ch[i];
        int pin = g_espd_cfg.pulse_pins[i];
        rmt_tx_channel_config_t tx_cfg = {
            .clk_src = RMT_CLK_SRC_DEFAULT,
            .gpio_num = pin,
            .mem_block_symbols = 64,
            .resolution_hz = ESPD_PULSE_RESOLUTION_HZ,
            .trans_queue_depth = 4,
            .flags = {
                .invert_out = false,
                .with_dma = false,
                .init_level = 0,
            },
        };
        rmt_copy_encoder_config_t enc_cfg = {};
        esp_err_t err;

        c->rate_hz = 1000.f;
        c->duty = 0.5f;
        pulse_rebuild_symbol(c);

        err = rmt_new_tx_channel(&tx_cfg, &c->chan);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "ch%d GPIO%d: rmt_new_tx_channel: %s", i, pin,
                esp_err_to_name(err));
            c->chan = NULL;
            continue;
        }
        err = rmt_new_copy_encoder(&enc_cfg, &c->encoder);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "ch%d: copy encoder: %s", i, esp_err_to_name(err));
            rmt_del_channel(c->chan);
            c->chan = NULL;
            continue;
        }
        err = rmt_enable(c->chan);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "ch%d: rmt_enable: %s", i, esp_err_to_name(err));
            rmt_del_encoder(c->encoder);
            rmt_del_channel(c->chan);
            c->chan = NULL;
            c->encoder = NULL;
            continue;
        }
        c->ready = true;
        ESP_LOGI(TAG, "ch%d → GPIO%d (RMT)", i, pin);
    }
}

bool espd_pulse_channel_ready(int ch)
{
    return ch >= 0 && ch < s_n && s_ch[ch].ready;
}

void espd_pulse_set_rate(int ch, float hz)
{
    espd_pulse_ch_t *c;

    if (!espd_pulse_channel_ready(ch))
        return;
    c = &s_ch[ch];
    if (!(hz > 0.f) || !isfinite(hz))
        return;
    c->rate_hz = hz;
    pulse_rebuild_symbol(c);
    if (c->continuous)
        (void)pulse_start(c, -1);
}

void espd_pulse_set_duty(int ch, float duty)
{
    espd_pulse_ch_t *c;

    if (!espd_pulse_channel_ready(ch))
        return;
    c = &s_ch[ch];
    if (!isfinite(duty))
        return;
    c->duty = duty;
    pulse_rebuild_symbol(c);
    if (c->continuous)
        (void)pulse_start(c, -1);
}

void espd_pulse_pulses(int ch, int32_t n)
{
    espd_pulse_ch_t *c;
    esp_err_t err;

    if (!espd_pulse_channel_ready(ch))
        return;
    c = &s_ch[ch];

    if (n == 0) {
        err = pulse_stop(c);
        if (err != ESP_OK)
            ESP_LOGW(TAG, "ch%d stop: %s", ch, esp_err_to_name(err));
        return;
    }
    if (n < -1)
        n = -1;

    err = pulse_start(c, n);
    if (err != ESP_OK)
        ESP_LOGW(TAG, "ch%d pulses %" PRId32 ": %s", ch, n, esp_err_to_name(err));
}

#else /* !ESPD_USE_PULSE */

void espd_pulse_init(void) {}
bool espd_pulse_channel_ready(int ch) { (void)ch; return false; }
void espd_pulse_set_rate(int ch, float hz) { (void)ch; (void)hz; }
void espd_pulse_set_duty(int ch, float duty) { (void)ch; (void)duty; }
void espd_pulse_pulses(int ch, int32_t n) { (void)ch; (void)n; }

#endif /* ESPD_USE_PULSE */
