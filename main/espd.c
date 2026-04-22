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
#ifdef ESPD_BOARD_WAVESHARE_S3
#include "boards/waveshare_s3/waveshare_s3_audio.h"
#include "boards/waveshare_s3/waveshare_s3_sdcard.h"
#include "boards/waveshare_s3/waveshare_s3_usb_state.h"
#endif
#include "nvs.h"
#include "nvs_flash.h"
#include "esp_timer.h"
#ifdef PD_USE_ANALOG0
#include "esp_adc/adc_oneshot.h"
#endif
static const char *TAG = "ESPD";

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
        snprintf(name, sizeof(name), "aout%d", i);
        pd_bind((t_pd *)&pd_aout_receivers[i], gensym(name));
    }
}
#endif
#ifdef PD_USE_SDCARD
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <sys/stat.h>
#endif
int espd_main_pd_loaded_from_store;
const char *espd_main_pd_loaded_dir;
#ifdef PD_USE_WIFI
int espd_wifi_net_enabled = 1;
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
static int pd_adc_last[ESPD_ANALOG_MAX_CHANNELS];
static unsigned pd_analog_block_counter;

static void pd_send_ain_value(int idx, int raw)
{
    char name[16];
    t_symbol *sym;
    t_pd *dest;
    t_float v;
    snprintf(name, sizeof(name), "ain%d", idx);
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
    int nchan = ESPD_ANALOG_NUM_CHANNELS;
    esp_err_t err = adc_oneshot_new_unit(&unit_cfg, &pd_adc_handle);
    if (nchan < 0)
        nchan = 0;
    if (nchan > ESPD_ANALOG_MAX_CHANNELS)
        nchan = ESPD_ANALOG_MAX_CHANNELS;
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "adc_oneshot_new_unit failed: %d", (int)err);
        return;
    }

    for (i = 0; i < ESPD_ANALOG_MAX_CHANNELS; i++)
    {
        pd_adc_active[i] = 0;
        pd_adc_last[i] = -100000;
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
    if (!enabled)
        ESP_LOGW(TAG, "analog enabled but no valid channels configured");
}

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

    for (i = 0; i < ESPD_ANALOG_MAX_CHANNELS; i++)
    {
        int raw;
        if (!pd_adc_active[i])
            continue;
        if (adc_oneshot_read(pd_adc_handle, pd_adc_channels[i], &raw) != ESP_OK)
            continue;
        if (raw < pd_adc_last[i] - ESPD_ANALOG_DEADBAND ||
            raw > pd_adc_last[i] + ESPD_ANALOG_DEADBAND)
        {
            pd_adc_last[i] = raw;
            pd_send_ain_value(i, raw);
        }
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
static int espd_printdacs;

void senddacs( void)
{
    int i, j, ret;
    static int count;
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
    if (espd_printdacs && count++ > 250)
    {
        ESP_LOGI(TAG, "sample %lx", poodle[0]);
        count = 0;
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
    const int uart_buffer_size = 256;
    ESP_ERROR_CHECK(uart_driver_install(CONFIG_ESP_CONSOLE_UART_NUM,
        uart_buffer_size, uart_buffer_size, 10, &uart_queue, 0));
}
#endif

    /* dispatch messages enqueued above */
void pd_pollhost( void)
{
    int lastchar;
#ifdef PD_USE_CONSOLE
    uint8_t data[128];
    int length = 0;
    ESP_ERROR_CHECK(uart_get_buffered_data_len(CONFIG_ESP_CONSOLE_UART_NUM,  
        (size_t*)&length));
    if (length > 0)
    {
        int i;
        /* ESP_LOGI(TAG, "serial in %d", length); */
        length = uart_read_bytes(CONFIG_ESP_CONSOLE_UART_NUM,
            data, length, 100);
        for (i = 0; i < length; i++)
        {
            char foo[80];
            ESP_LOGI(TAG, " %d", data[i] & 0xff);
            sprintf(foo, "key %d;", data[i] & 0xff);
            pd_sendmsg(foo, strlen(foo));
        }
    }
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

void app_main(void)
{
    esp_log_level_set("*", ESP_LOG_WARN);
    esp_log_level_set(TAG, ESP_LOG_INFO);

    espd_nvs_flash_init();
    espd_patch_store_init();

#ifdef ESPD_BOARD_WAVESHARE_S3
    espd_waveshare_s3_usb_boot_before_pd();
#endif

#if defined(PD_USE_SDCARD) && defined(ESPD_BOARD_WAVESHARE_S3)
    /* Mount SD before Pd so main.pd can load from ESPD_SDCARD_MOUNT. */
    {
        esp_err_t e = espd_waveshare_s3_sdcard_mount();
        if (e != ESP_OK)
            ESP_LOGW(TAG, "SD card not mounted at boot: %s", esp_err_to_name(e));
    }
#endif

    pdmain_init();
    initdacs();
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
    if (espd_main_pd_loaded_from_store && ESPD_SKIP_WIFI_WHEN_MAIN_PD_ON_DISK) {
        espd_wifi_net_enabled = 0;
        ESP_LOGI(TAG,
                 "main.pd loaded from %s — skipping WiFi and TCP/UDP patch transport",
                 espd_main_pd_loaded_dir ? espd_main_pd_loaded_dir : ESPD_PATCH_STORE_MOUNT);
    } else {
        espd_wifi_net_enabled = 1;
        ESP_LOGI(TAG, "[ 1a ] start network");
        wifi_init();
        net_init();
        net_hello();
    }
#endif
#ifdef PD_USE_CONSOLE
    console_init();
#endif

    ESP_LOGI(TAG, "[ 2 ] now write some shit");

    while (1)
    {
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
        pdmain_tick();
        senddacs();
#ifdef ESPD_BOARD_WAVESHARE_S3
        espd_waveshare_s3_poll_usb_hotplug_restart();
#endif
#ifdef PD_USE_WIFI
        if (espd_wifi_net_enabled)
            net_alive();
#endif
    }
}

void espd_control_io_init(void)
{
#if ESPD_AOUT_NUM_CHANNELS > 0
    pd_aout_init();
#endif
}

#ifdef PD_USE_SDCARD
static void espd_sdcard_debug_list_root(void)
{
    const char *root = ESPD_SDCARD_MOUNT;
    DIR *d = opendir(root);
    if (!d) {
        ESP_LOGW(TAG, "SD card: cannot read %s (%s) — missing, unmounted, or not ready yet",
                 root, strerror(errno));
        return;
    }
    ESP_LOGI(TAG, "SD card listing (%s):", root);
    struct dirent *de;
    int n = 0;
    while ((de = readdir(d)) != NULL) {
        if (de->d_name[0] == '.' &&
            (de->d_name[1] == '\0' ||
             (de->d_name[1] == '.' && de->d_name[2] == '\0')))
            continue;
        char path[288];
        struct stat st;
        snprintf(path, sizeof(path), "%s/%s", root, de->d_name);
        if (stat(path, &st) == 0) {
            const char *kind = S_ISDIR(st.st_mode) ? "dir" : "file";
            ESP_LOGI(TAG, "  [%s] %s", kind, de->d_name);
        } else {
            ESP_LOGI(TAG, "  %s", de->d_name);
        }
        n++;
    }
    closedir(d);
    if (n == 0)
        ESP_LOGI(TAG, "  (empty)");
}

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
    espd_sdcard_debug_list_root();
    ESP_LOGI(TAG, "[ 1b ] done starting network");
}
#endif

static void espd_printtimediff( void)
{
    static int64_t whensent;
    int64_t newtime = esp_timer_get_time();
    int elapsed = (newtime - whensent)/1000;
    char msg[80];
    whensent = newtime;
    sprintf(msg, "elapsed msec %d\n", elapsed);
    pdmain_print(msg);
}

#define t_floatarg float
void glob_foo(void *dummy, t_floatarg f)
{
    if (f == 0)
        espd_printdacs = 0;
    else if (f == 1)
        espd_printdacs = 1;
    else if (f == 2)
        trymem(2);
    else if (f == 3)
        espd_printtimediff();
}


