/*

   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

#include "espd.h"
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "audio_element.h"
#include "audio_pipeline.h"
#include "audio_event_iface.h"
#include "audio_mem.h"
#include "audio_common.h"
#include "i2s_stream.h"
#include "mp3_decoder.h"
#include "filter_resample.h"
#include "board.h"
#include "esp_peripherals.h"
#include "periph_sdcard.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "ESPD";
#define TEST_I2S_NUM  I2S_NUM_0

extern void pdmain_tick( void);
void pdmain_init( void);


void sd_init( void);

#define INCHANS 1
#define OUTCHANS 1
#define BLKSIZE 64
float soundin[OUTCHANS * BLKSIZE], soundout[OUTCHANS * BLKSIZE];

void senddacs( void)
{
    int i, ret;
    size_t written;
    short poodle[OUTCHANS * BLKSIZE];

    for (i = 0; i < BLKSIZE; i+= OUTCHANS)
    {
        int ch1 = 32767*soundout[i];
         if (ch1 > 32767)
            ch1 = 32767;
        else if (ch1 < -32768)
            ch1 = -32768;
        poodle[i] = ch1;
        /* 
        ch1 = 32767*soundout[BLKSIZE+i];
        if (ch1 > 32767)
            ch1 = 32767;
        else if (ch1 < -32768)
            ch1 = -32768;
        poodle[i+1] = ch2;  */
        
        soundout[i] = 0;
    }

    ret = i2s_write(TEST_I2S_NUM, poodle, sizeof(poodle), &written,
        portMAX_DELAY);
    if (ret != ESP_OK)
        ESP_LOGE(TAG, "error writing");
}

static void initdacs( void)
{
    i2s_config_t i2s_cfg = {
        .mode = I2S_MODE_MASTER | I2S_MODE_TX | I2S_MODE_RX,
        .sample_rate = 48000,
        .bits_per_sample = 16,
        /* .channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT, */
        .channel_format = I2S_CHANNEL_FMT_ONLY_RIGHT,
        .communication_format = I2S_COMM_FORMAT_STAND_I2S,
        .dma_buf_count = 16,
        .dma_buf_len = 256,
        .use_apll = 1,
        .intr_alloc_flags = ESP_INTR_FLAG_LEVEL2,
    };
    i2s_pin_config_t i2s_pin_cfg = {0};

    ESP_LOGI(TAG, "[ 1 ] Start audio codec chip");

    audio_board_handle_t board_handle = audio_board_init();
    audio_hal_ctrl_codec(board_handle->audio_hal, AUDIO_HAL_CODEC_MODE_BOTH, AUDIO_HAL_CTRL_START);

    i2s_driver_install(I2S_NUM_0, &i2s_cfg, 0, NULL);
    get_i2s_pins(I2S_NUM_0, &i2s_pin_cfg);
    i2s_set_pin(I2S_NUM_0, &i2s_pin_cfg);
    i2s_mclk_gpio_select(I2S_NUM_0, GPIO_NUM_0);

}

static int audiostate;

void sys_set_audio_state(int onoff)
{
    /*
    if (onoff && !audiostate)
        i2s_start(I2S_NUM_0);
    else if (!onoff && audiostate)
        i2s_stop(I2S_NUM_0);
    audiostate = onoff;
    */
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

    /* dispatch messages enqueued above */
void pd_pollhost( void)
{
    int lastchar;
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
#if 0   /* this doesn't work all that well since posts are split into atoms */
    net_sendudp(y, strlen(y), CONFIG_ESP_WIFI_SENDPORT); 
    net_sendtcp(y, strlen(y));
#endif
}

void trymem(int foo);
void net_alive( void);

    /* allow deprecated form if new one unavailable */
#ifndef I2S_COMM_FORMAT_STAND_I2S
#define I2S_COMM_FORMAT_STAND_I2S I2S_COMM_FORMAT_I2S
#endif

void app_main(void)
{
    esp_log_level_set("*", ESP_LOG_WARN);
    esp_log_level_set(TAG, ESP_LOG_INFO);

    pdmain_init();
    initdacs();

#ifdef PD_USE_BLUETOOTH
    bt_init();
#endif
    sd_init();
#ifdef PD_USE_WIFI
    ESP_LOGI(TAG, "[ 1a ] start network");
    wifi_init();
    net_init();
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
        net_alive();
    }
}


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
