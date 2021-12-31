/*

   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

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
#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_gap_bt_api.h"
#include "esp_bt_device.h"
#include "esp_spp_api.h"
#include "esp_peripherals.h"
#include "periph_sdcard.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "ESPD";
#define TEST_I2S_NUM  I2S_NUM_0

extern void pdmain_tick( void);
void pdmain_init( void);
void pd_bt_poll( void);

void bt_init( void);
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

void pd_bt_writeback(unsigned char *s, int length);

void pdmain_print( const char *s)
{
    char y[80];
    strncpy(y, s, 79);
    y[79]=0;
    ESP_LOGI(TAG, "%s", y);
    pd_bt_writeback((unsigned char *)y, strlen(y));
}

void trymem(int foo);

    /* allow deprecated form if new one unavailable */
#ifndef I2S_COMM_FORMAT_STAND_I2S
#define I2S_COMM_FORMAT_STAND_I2S I2S_COMM_FORMAT_I2S
#endif

void app_main(void)
{
    i2s_config_t i2s_cfg = {
        .mode = I2S_MODE_MASTER | I2S_MODE_TX | I2S_MODE_RX,
        .sample_rate = 48000,
        .bits_per_sample = 16,
        /* .channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT, */
        .channel_format = I2S_CHANNEL_FMT_ONLY_RIGHT,
        .communication_format = I2S_COMM_FORMAT_STAND_I2S,
        .dma_buf_count = 3,
        .dma_buf_len = 300,
        .use_apll = 1,
        .intr_alloc_flags = ESP_INTR_FLAG_LEVEL2,
    };
    i2s_pin_config_t i2s_pin_cfg = {0};

    pdmain_init();

    esp_log_level_set("*", ESP_LOG_WARN);
    esp_log_level_set(TAG, ESP_LOG_INFO);
    ESP_LOGI(TAG, "[ 1 ] Start audio codec chip");

    audio_board_handle_t board_handle = audio_board_init();
    audio_hal_ctrl_codec(board_handle->audio_hal, AUDIO_HAL_CODEC_MODE_BOTH, AUDIO_HAL_CTRL_START);

    i2s_driver_install(I2S_NUM_0, &i2s_cfg, 0, NULL);
    get_i2s_pins(I2S_NUM_0, &i2s_pin_cfg);
    i2s_set_pin(I2S_NUM_0, &i2s_pin_cfg);
    i2s_mclk_gpio_select(I2S_NUM_0, GPIO_NUM_0);

    bt_init();
    sd_init();

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
        pd_bt_poll();
        pdmain_tick();
        senddacs();
    }
}

/* -------------------- bluetooth and SD card ------------------ */
/* queue from bluetooth.  Need to make this a proper RTOS queue */
static char *pd_bt_buf;
static int pd_bt_size;
static SemaphoreHandle_t pd_bt_mutex;

void *getbytes(size_t nbytes);
void freebytes(void *x, size_t nbytes);
void *resizebytes(void *x, size_t oldsize, size_t newsize);
#include <ctype.h>
void pd_sendmsg(char *buf, int bufsize);

void pd_bt_dispatch(char *data, size_t size)
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

void pd_bt_poll( void)
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

static uint32_t pd_bt_writehandle;

void pd_bt_writeback(unsigned char *s, int length)
{
#if 1
    if (pd_bt_writehandle)
    {
        if (esp_spp_write(pd_bt_writehandle, length, s) != ESP_OK)
            ESP_LOGI(TAG, "bt writeback error");
    }
    else ESP_LOGI(TAG, "writeback: not open");
#endif
}

static const esp_spp_mode_t esp_spp_mode = ESP_SPP_MODE_CB;
static const esp_spp_sec_t sec_mask = ESP_SPP_SEC_AUTHENTICATE;
static const esp_spp_role_t role_slave = ESP_SPP_ROLE_SLAVE;

static void esp_spp_cb(esp_spp_cb_event_t event, esp_spp_cb_param_t *param)
{
    switch (event) {
    case ESP_SPP_INIT_EVT:
        ESP_LOGI(TAG, "ESP_SPP_INIT_EVT");
        esp_bt_dev_set_device_name("pure_data");
#ifdef ESP_BT_SCAN_MODE_CONNECTABLE_DISCOVERABLE
        esp_bt_gap_set_scan_mode(ESP_BT_SCAN_MODE_CONNECTABLE_DISCOVERABLE);
#else
        esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_GENERAL_DISCOVERABLE);
#endif
        esp_spp_start_srv(sec_mask,role_slave, 0, "pd_server");
        break;
    case ESP_SPP_DISCOVERY_COMP_EVT:
        ESP_LOGI(TAG, "ESP_SPP_DISCOVERY_COMP_EVT");
        break;
    case ESP_SPP_OPEN_EVT:
        ESP_LOGI(TAG, "ESP_SPP_OPEN_EVT");
        break;
    case ESP_SPP_CLOSE_EVT:
        pd_bt_writehandle = 0;
        ESP_LOGI(TAG, "ESP_SPP_CLOSE_EVT");
        break;
    case ESP_SPP_START_EVT:
        ESP_LOGI(TAG, "ESP_SPP_START_EVT");
        break;
    case ESP_SPP_CL_INIT_EVT:
        ESP_LOGI(TAG, "ESP_SPP_CL_INIT_EVT");
        break;
    case ESP_SPP_DATA_IND_EVT:

#if 0
        if (param->data_ind.len > 2)
            ESP_LOGI(TAG, "ESP_SPP_DATA_IND_EVT len=%d handle=%d",
                 param->data_ind.len, param->data_ind.handle);
        {
            char foo[80];
            int len = (param->data_ind.len > 79 ? 79 : param->data_ind.len);
            memcpy(foo, param->data_ind.data, len);
            foo[len] = 0;
            ESP_LOGI(TAG, "message %s", foo);
        }
#endif
        pd_bt_dispatch((char *)(param->data_ind.data), param->data_ind.len);

        break;
    case ESP_SPP_CONG_EVT:
        ESP_LOGI(TAG, "ESP_SPP_CONG_EVT");
        break;
    case ESP_SPP_WRITE_EVT:
        ESP_LOGI(TAG, "ESP_SPP_WRITE_EVT");
        break;
    case ESP_SPP_SRV_OPEN_EVT:
        pd_bt_writehandle = param->write.handle;
            // ((struct spp_open_evt_param *)param)->handle;
        ESP_LOGI(TAG, "ESP_SPP_SRV_OPEN_EVT");
        break;
    default:
        break;
    }
}

void esp_bt_gap_cb(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t *param)
{
    switch (event) {
    case ESP_BT_GAP_AUTH_CMPL_EVT:{
        if (param->auth_cmpl.stat == ESP_BT_STATUS_SUCCESS) {
            ESP_LOGI(TAG, "authentication success: %s", param->auth_cmpl.device_name);
            esp_log_buffer_hex(TAG, param->auth_cmpl.bda, ESP_BD_ADDR_LEN);
        } else {
            ESP_LOGE(TAG, "authentication failed, status:%d", param->auth_cmpl.stat);
        }
        break;
    }
    case ESP_BT_GAP_PIN_REQ_EVT:{
        ESP_LOGI(TAG, "ESP_BT_GAP_PIN_REQ_EVT min_16_digit:%d", param->pin_req.min_16_digit);
        if (param->pin_req.min_16_digit) {
            ESP_LOGI(TAG, "Input pin code: 0000 0000 0000 0000");
            esp_bt_pin_code_t pin_code = {0};
            esp_bt_gap_pin_reply(param->pin_req.bda, true, 16, pin_code);
        } else {
            ESP_LOGI(TAG, "Input pin code: 1234");
            esp_bt_pin_code_t pin_code;
            pin_code[0] = '1';
            pin_code[1] = '2';
            pin_code[2] = '3';
            pin_code[3] = '4';
            esp_bt_gap_pin_reply(param->pin_req.bda, true, 4, pin_code);
        }
        break;
    }

#if (CONFIG_BT_SSP_ENABLED == true)
    case ESP_BT_GAP_CFM_REQ_EVT:
        ESP_LOGI(TAG, "ESP_BT_GAP_CFM_REQ_EVT Please compare the numeric value: %d", param->cfm_req.num_val);
        esp_bt_gap_ssp_confirm_reply(param->cfm_req.bda, true);
        break;
    case ESP_BT_GAP_KEY_NOTIF_EVT:
        ESP_LOGI(TAG, "ESP_BT_GAP_KEY_NOTIF_EVT passkey:%d", param->key_notif.passkey);
        break;
    case ESP_BT_GAP_KEY_REQ_EVT:
        ESP_LOGI(TAG, "ESP_BT_GAP_KEY_REQ_EVT Please enter passkey!");
        break;
#endif

    default: {
        ESP_LOGI(TAG, "event: %d", event);
        break;
    }
    }
    return;
}

void bt_init( void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK( ret );

    ESP_ERROR_CHECK(esp_bt_controller_mem_release(ESP_BT_MODE_BLE));

    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    if ((ret = esp_bt_controller_init(&bt_cfg)) != ESP_OK) {
        ESP_LOGE(TAG, "%s initialize controller failed: %s\n", __func__, esp_err_to_name(ret));
        return;
    }

    if ((ret = esp_bt_controller_enable(ESP_BT_MODE_CLASSIC_BT)) != ESP_OK) {
        ESP_LOGE(TAG, "%s enable controller failed: %s\n", __func__, esp_err_to_name(ret));
        return;
    }

    if ((ret = esp_bluedroid_init()) != ESP_OK) {
        ESP_LOGE(TAG, "%s initialize bluedroid failed: %s\n", __func__, esp_err_to_name(ret));
        return;
    }

    if ((ret = esp_bluedroid_enable()) != ESP_OK) {
        ESP_LOGE(TAG, "%s enable bluedroid failed: %s\n", __func__, esp_err_to_name(ret));
        return;
    }

    if ((ret = esp_bt_gap_register_callback(esp_bt_gap_cb)) != ESP_OK) {
        ESP_LOGE(TAG, "%s gap register failed: %s\n", __func__, esp_err_to_name(ret));
        return;
    }

    if ((ret = esp_spp_register_callback(esp_spp_cb)) != ESP_OK) {
        ESP_LOGE(TAG, "%s spp register failed: %s\n", __func__, esp_err_to_name(ret));
        return;
    }

    if ((ret = esp_spp_init(esp_spp_mode)) != ESP_OK) {
        ESP_LOGE(TAG, "%s spp init failed: %s\n", __func__, esp_err_to_name(ret));
        return;
    }

#if (CONFIG_BT_SSP_ENABLED == true)
    /* Set default parameters for Secure Simple Pairing */
    esp_bt_sp_param_t param_type = ESP_BT_SP_IOCAP_MODE;
    esp_bt_io_cap_t iocap = ESP_BT_IO_CAP_IO;
    esp_bt_gap_set_security_param(param_type, &iocap, sizeof(uint8_t));
#endif

    /*
     * Set default parameters for Legacy Pairing
     * Use variable pin, input pin code when pairing
     */
    esp_bt_pin_type_t pin_type = ESP_BT_PIN_TYPE_VARIABLE;
    esp_bt_pin_code_t pin_code;
    esp_bt_gap_set_pin(pin_type, 0, pin_code);
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
}
