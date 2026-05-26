/*
 * Host dev sync over TinyUSB CDC (rapid patching to microSD only).
 *
 * Protocol (host -> device):
 *   PING
 *   PUT <relpath> <nbytes>\n  then exactly <nbytes> raw bytes
 *   RELOAD
 *   RESET  (reboot ESP after reply)
 *
 * Device replies (one line each, prefixed for filtering):
 *   +OK ...
 *   -ERR ...
 */

#include "espd_dev.h"

#if CONFIG_ESPD_DEV_CDC_SYNC

#include "espd.h"
#include "espd_storage.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_system.h"
#include "tinyusb_cdc_acm.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>

static const char *TAG = "espd_dev";

#define ESPD_DEV_TASK_CORE          0
#define ESPD_DEV_TASK_PRIO          5
#define ESPD_DEV_RX_CHUNK           256
#define ESPD_DEV_RX_RING            2048
#define ESPD_DEV_LINE_MAX           160
#define ESPD_DEV_PUT_MAX            (512 * 1024)
#define ESPD_DEV_PATH_MAX           192

typedef enum {
    DEV_CMD_NONE = 0,
    DEV_CMD_PUT,
    DEV_CMD_RELOAD,
    DEV_CMD_PING,
} dev_cmd_t;

static TaskHandle_t s_dev_task;
static volatile bool s_reload_pending;
static portMUX_TYPE s_rx_lock = portMUX_INITIALIZER_UNLOCKED;
static uint8_t s_rx_ring[ESPD_DEV_RX_RING];
static size_t s_rx_head;
static size_t s_rx_tail;

static dev_cmd_t s_cmd;
static char s_put_rel[ESPD_DEV_PATH_MAX];
static size_t s_put_remain;
static FILE *s_put_fp;

static void dev_reply(const char *msg);

static void dev_rx_push(const uint8_t *data, size_t len)
{
    size_t i;

    portENTER_CRITICAL(&s_rx_lock);
    for (i = 0; i < len; i++) {
        size_t next = (s_rx_head + 1) % ESPD_DEV_RX_RING;
        if (next == s_rx_tail)
            break;
        s_rx_ring[s_rx_head] = data[i];
        s_rx_head = next;
    }
    portEXIT_CRITICAL(&s_rx_lock);
}

static size_t dev_rx_pop(uint8_t *out, size_t max)
{
    size_t n = 0;

    portENTER_CRITICAL(&s_rx_lock);
    while (n < max && s_rx_tail != s_rx_head) {
        out[n++] = s_rx_ring[s_rx_tail];
        s_rx_tail = (s_rx_tail + 1) % ESPD_DEV_RX_RING;
    }
    portEXIT_CRITICAL(&s_rx_lock);
    return n;
}

static void dev_drain_cdc_hw(void)
{
    uint8_t buf[ESPD_DEV_RX_CHUNK];
    size_t rx;

    if (!tinyusb_cdcacm_initialized(TINYUSB_CDC_ACM_0))
        return;

    while (tinyusb_cdcacm_read(TINYUSB_CDC_ACM_0, buf, sizeof(buf), &rx) == ESP_OK && rx > 0)
        dev_rx_push(buf, rx);
}

void espd_dev_cdc_rx_cb(int itf, cdcacm_event_t *event)
{
    (void)event;
    if (itf != TINYUSB_CDC_ACM_0)
        return;
    dev_drain_cdc_hw();
    if (s_dev_task)
        xTaskNotifyGive(s_dev_task);
}

static void dev_cdc_line_cb(int itf, cdcacm_event_t *event)
{
    if (itf != TINYUSB_CDC_ACM_0 || !event)
        return;
    if (event->type == CDC_EVENT_LINE_STATE_CHANGED &&
            event->line_state_changed_data.dtr) {
        dev_reply("+OK dev ready");
    }
}

static void dev_reply(const char *msg)
{
    char line[192];
    int n;
    size_t off;
    TickType_t deadline;

    if (!msg || !tinyusb_cdcacm_initialized(TINYUSB_CDC_ACM_0))
        return;
    n = snprintf(line, sizeof(line), "%s\r\n", msg);
    if (n <= 0)
        return;

    off = 0;
    deadline = xTaskGetTickCount() + pdMS_TO_TICKS(500);
    while (off < (size_t)n) {
        size_t w = tinyusb_cdcacm_write_queue(TINYUSB_CDC_ACM_0,
                (const uint8_t *)line + off, (size_t)n - off);
        off += w;
        if (off < (size_t)n) {
            tinyusb_cdcacm_write_flush(TINYUSB_CDC_ACM_0, pdMS_TO_TICKS(20));
            if (xTaskGetTickCount() >= deadline) {
                ESP_LOGW(TAG, "reply truncated (TX busy): %s", msg);
                return;
            }
            vTaskDelay(1);
        }
    }
    tinyusb_cdcacm_write_flush(TINYUSB_CDC_ACM_0, pdMS_TO_TICKS(200));
}

static int dev_sdcard_ready(void)
{
#ifdef ESPD_USE_SDCARD
    struct stat st;
    if (stat(ESPD_SDCARD_MOUNT, &st) != 0 || !S_ISDIR(st.st_mode))
        return 0;
    return 1;
#else
    return 0;
#endif
}

static int dev_rel_path_ok(const char *rel)
{
    size_t n;

    if (!rel || !rel[0] || rel[0] == '/')
        return 0;
    if (strstr(rel, "..") != NULL)
        return 0;
    n = strlen(rel);
    if (n >= ESPD_DEV_PATH_MAX)
        return 0;
    return 1;
}

static int dev_build_path(char *out, size_t outsz, const char *rel)
{
    int n = snprintf(out, outsz, "%s/%s", ESPD_SDCARD_MOUNT, rel);
    if (n < 0 || (size_t)n >= outsz)
        return 0;
    return 1;
}

static void dev_mkdir_parents(const char *fullpath)
{
    char tmp[ESPD_DEV_PATH_MAX];
    char *p;

    if (strlen(fullpath) >= sizeof(tmp))
        return;
    strcpy(tmp, fullpath);
    for (p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(tmp, 0755);
            *p = '/';
        }
    }
}

static void dev_put_begin(const char *rel, size_t nbytes)
{
    char full[ESPD_DEV_PATH_MAX];
    char reply[ESPD_DEV_PATH_MAX + 32];

    if (!dev_sdcard_ready()) {
        dev_reply("-ERR no sdcard (insert card for rapid dev)");
        return;
    }
    if (!dev_rel_path_ok(rel)) {
        dev_reply("-ERR bad path");
        return;
    }
    if (nbytes == 0 || nbytes > ESPD_DEV_PUT_MAX) {
        dev_reply("-ERR bad size");
        return;
    }
    if (!dev_build_path(full, sizeof(full), rel)) {
        dev_reply("-ERR path too long");
        return;
    }

    dev_mkdir_parents(full);
    s_put_fp = fopen(full, "wb");
    if (!s_put_fp) {
        dev_reply("-ERR open failed");
        return;
    }

    strncpy(s_put_rel, rel, sizeof(s_put_rel) - 1);
    s_put_rel[sizeof(s_put_rel) - 1] = '\0';
    s_put_remain = nbytes;
    s_cmd = DEV_CMD_PUT;
    snprintf(reply, sizeof(reply), "+OK PUT %s %u", rel, (unsigned)nbytes);
    dev_reply(reply);
}

static void dev_put_data(const uint8_t *data, size_t len)
{
    size_t n;
    char reply[64];

    if (!s_put_fp || s_cmd != DEV_CMD_PUT) {
        return;
    }
    n = len;
    if (n > s_put_remain)
        n = s_put_remain;
    if (n && fwrite(data, 1, n, s_put_fp) != n) {
        fclose(s_put_fp);
        s_put_fp = NULL;
        s_put_remain = 0;
        s_cmd = DEV_CMD_NONE;
        dev_reply("-ERR write failed");
        return;
    }
    s_put_remain -= n;
    if (s_put_remain == 0) {
        fclose(s_put_fp);
        s_put_fp = NULL;
        s_cmd = DEV_CMD_NONE;
        snprintf(reply, sizeof(reply), "+OK PUT done %s", s_put_rel);
        dev_reply(reply);
        espd_storage_refresh_paths();
    }
}

static void dev_do_reload(void)
{
    if (!dev_sdcard_ready()) {
        dev_reply("-ERR no sdcard");
        return;
    }
    s_reload_pending = true;
    dev_reply("+OK RELOAD pending");
}

static void dev_trim_line(char *line)
{
    char *end;

    if (!line)
        return;
    while (*line == ' ' || *line == '\t')
        memmove(line, line + 1, strlen(line));
    end = line + strlen(line);
    while (end > line && (end[-1] == ' ' || end[-1] == '\t'))
        *--end = '\0';
}

static void dev_handle_line(char *line)
{
    if (!line)
        return;
    dev_trim_line(line);
    if (!line[0])
        return;

    if (!strcmp(line, "PING")) {
        if (dev_sdcard_ready())
            dev_reply("+OK PING sdcard");
        else
            dev_reply("+OK PING (no sdcard — rapid dev disabled)");
        return;
    }
    if (!strcmp(line, "RELOAD")) {
        dev_do_reload();
        return;
    }
    if (!strcmp(line, "RESET")) {
        dev_reply("+OK RESET");
        vTaskDelay(pdMS_TO_TICKS(100));
        esp_restart();
        return;
    }
    if (!strncmp(line, "PUT ", 4)) {
        char rel[ESPD_DEV_PATH_MAX];
        unsigned long nbytes = 0;
        if (sscanf(line + 4, "%190s %lu", rel, &nbytes) == 2)
            dev_put_begin(rel, (size_t)nbytes);
        else
            dev_reply("-ERR PUT syntax");
        return;
    }
    dev_reply("-ERR unknown command");
}

static void dev_feed_bytes(const uint8_t *buf, size_t rx, int line_mode)
{
    static char line[ESPD_DEV_LINE_MAX];
    static size_t line_len;
    size_t off = 0;
    size_t i;

    if (!line_mode) {
        if (s_cmd == DEV_CMD_PUT && s_put_remain > 0)
            dev_put_data(buf, rx);
        return;
    }

    if (s_cmd == DEV_CMD_PUT && s_put_remain > 0) {
        size_t n = rx;
        if (n > s_put_remain)
            n = s_put_remain;
        dev_put_data(buf, n);
        off = n;
        if (off >= rx)
            return;
    }
    for (i = off; i < rx; i++) {
        uint8_t c = buf[i];

        if (c == '\r')
            continue;
        if (c == '\n') {
            if (line_len > 0) {
                line[line_len] = '\0';
                dev_handle_line(line);
                line_len = 0;
            }
            continue;
        }
        if (line_len + 1 < sizeof(line))
            line[line_len++] = (char)c;
    }
}

static void dev_poll_rx(void)
{
    uint8_t buf[ESPD_DEV_RX_CHUNK];
    size_t n;
    int line_mode = (s_cmd != DEV_CMD_PUT || s_put_remain == 0);

    dev_drain_cdc_hw();
    while ((n = dev_rx_pop(buf, sizeof(buf))) > 0)
        dev_feed_bytes(buf, n, line_mode ? 1 : 0);
}

static void espd_dev_task(void *arg)
{
    (void)arg;
    for (;;) {
        dev_poll_rx();
        (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(10));
    }
}

void espd_dev_init(void)
{
    esp_err_t err;

    if (s_dev_task)
        return;
    s_cmd = DEV_CMD_NONE;
    s_reload_pending = false;
    s_rx_head = 0;
    s_rx_tail = 0;

    if (tinyusb_cdcacm_initialized(TINYUSB_CDC_ACM_0)) {
        err = tinyusb_cdcacm_register_callback(TINYUSB_CDC_ACM_0,
                CDC_EVENT_LINE_STATE_CHANGED, dev_cdc_line_cb);
        if (err != ESP_OK)
            ESP_LOGW(TAG, "CDC line callback register failed: %s", esp_err_to_name(err));
    }

    if (xTaskCreatePinnedToCore(espd_dev_task, "espd_dev", 4096, NULL,
            ESPD_DEV_TASK_PRIO, &s_dev_task, ESPD_DEV_TASK_CORE) != pdPASS) {
        ESP_LOGW(TAG, "dev CDC task create failed");
        s_dev_task = NULL;
        return;
    }
    ESP_LOGI(TAG, "CDC dev sync: PUT/RELOAD -> %s (SD required)", ESPD_SDCARD_MOUNT);
}

bool espd_dev_reload_pending(void)
{
    return s_reload_pending;
}

void espd_dev_clear_reload_pending(void)
{
    s_reload_pending = false;
}

#else /* !CONFIG_ESPD_DEV_CDC_SYNC */

void espd_dev_init(void) {}
bool espd_dev_reload_pending(void) { return false; }
void espd_dev_clear_reload_pending(void) {}

#endif /* CONFIG_ESPD_DEV_CDC_SYNC */
