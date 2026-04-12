/*

   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

#include "espd.h"
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
#include "boards/waveshare_s3/waveshare_s3_usb_state.h"
#endif
#include "nvs.h"
#include "nvs_flash.h"
#include "esp_timer.h"
#ifdef PD_USE_CONSOLE
#include "driver/uart.h"
#include "esp_console.h"
#endif
static const char *TAG = "ESPD";

int espd_main_pd_loaded_from_store;
#ifdef PD_USE_WIFI
int espd_wifi_net_enabled = 1;
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

    pdmain_init();
    initdacs();

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
                 ESPD_PATCH_STORE_MOUNT);
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

#ifdef PD_USE_SDCARD
void sd_init( void)
{
        /* initialize SD card */
    ESP_LOGI(TAG, "[ 1 ] Mount sdcard");
    // Initialize peripherals management
    esp_periph_config_t periph_cfg = DEFAULT_ESP_PERIPH_SET_CONFIG();
    esp_periph_set_handle_t set = esp_periph_set_init(&periph_cfg);

    // Initialize SD Card peripheral
    audio_board_sdcard_init(set, SD_MODE_1_LINE);
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


