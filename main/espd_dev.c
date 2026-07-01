/*
 * Host dev sync over TinyUSB CDC (rapid patching to local storage target).
 *
 * Protocol (host -> device):
 *   STATUS
 *   PUT <relpath> <nbytes> <crc32hex>
 *       -> +OK PUT skip (file already matches) or +OK PUT ready window=<bytes>
 *          host sends <=window raw bytes per round, device replies +OK PUT ack <received>
 *          repeat until all bytes received -> +OK PUT done <crc>
 *   LIST  (recursive file list under active sync target)
 *   RM <relpath>  (delete one file)
 *   RELOAD
 *   MSG <pd-message>  (queued; evaluated on audio thread via pd_sendmsg)
 *   RESET  (reboot ESP after reply)
 *   APOFF  (SoftAP builds only: stop SoftAP, keep STA if configured)
 *
 * WiFi (CONFIG_ESPD_WIFI_AP_SYNC): same line protocol over TCP port 4499 on
 * the SoftAP interface (device at 192.168.4.1).
 *   +OK ...
 *   -ERR ...
 */

#include "espd.h"
#include "espd_dev.h"
#include "espd_sync_io.h"
#include "espd_storage.h"
#if CONFIG_ESPD_WIFI_AP_SYNC
#include "espd_dev_wifi.h"
#endif

#if CONFIG_ESPD_DEV_SYNC

#include <driver/uart.h>
#include "driver/uart_vfs.h"
#if CONFIG_ESPD_DEV_CDC_SYNC
#include <tinyusb.h>
#include <tinyusb_cdc_acm.h>
#endif
#if ESPD_DEV_SERIAL_SYNC_USJ
#include <driver/usb_serial_jtag.h>
#endif

#include <freertos/task.h>
#include <esp_system.h>
#include <esp_crc.h>
#include <inttypes.h>
#include <sys/stat.h>
#include <unistd.h>
#include <dirent.h>
#include <stdbool.h>
#include <errno.h>

static const char *TAG = "espd_dev";

#define ESPD_DEV_TASK_CORE          0
/* Below TinyUSB device task (4) so CDC RX is not starved during PUT. */
#define ESPD_DEV_TASK_PRIO          3
#define ESPD_DEV_RX_CHUNK           4192
/* PUT recv + stdio; 6 KiB stack overflowed before RX_CHUNK was 4 KiB. */
#define ESPD_DEV_TASK_STACK         8192
/* Room for PUT <path-with-spaces> <size> <crc> (path up to ESPD_DEV_PATH_MAX). */
#define ESPD_DEV_LINE_MAX           256
#define ESPD_DEV_PATH_MAX           384
#define ESPD_DEV_PUT_WINDOW         8192
#if CONFIG_ESPD_DEV_CDC_SYNC
#else
#define ESPD_DEV_IDLE_WAIT_MS       50
#endif

typedef enum {
    DEV_CMD_NONE = 0,
    DEV_CMD_PUT,
} dev_cmd_t;

typedef enum {
    DEV_TARGET_SD = 0,
    DEV_TARGET_FLASH,
} dev_target_t;

static TaskHandle_t s_dev_task;
static volatile bool s_reload_pending;
static volatile bool s_pdmsg_pending;
static char s_pdmsg[ESPD_DEV_LINE_MAX];

static dev_cmd_t s_cmd;
static char s_put_rel[ESPD_DEV_PATH_MAX];
static size_t s_put_total;
static size_t s_put_remain;
static uint32_t s_put_expect_crc;
static uint32_t s_put_crc;
static FILE *s_put_fp;
static char s_put_tmp[ESPD_DEV_PATH_MAX];
#if CONFIG_SPIRAM
#define ESPD_DEV_PSRAM_RESERVE (256u * 1024u)  /* headroom kept for Pd/audio */
static uint8_t *s_put_buf;   /* PSRAM-backed receive buffer, or NULL */
static size_t   s_put_buf_pos;
#endif
static dev_target_t s_target = DEV_TARGET_SD;

/* Direct-mapped CRC cache: skip re-reading large unchanged files on PUT offer.
 * Keyed on (path-hash, size, mtime); validated cheaply via stat(). PSRAM-backed.
 * Populated by dev_file_hash() after a real read, and for free by
 * dev_put_finish() since we already know the CRC of what we just wrote. */
#define ESPD_DEV_CRC_CACHE_SLOTS    128
#define ESPD_DEV_CRC_CACHE_MIN_SIZE (128u * 1024u)
struct dev_crc_cache_entry {
    uint32_t path_hash;
    uint32_t size;
    uint32_t mtime;
    uint32_t crc;
    bool valid;
};
static struct dev_crc_cache_entry *s_crc_cache;

static volatile bool s_sync_active = false;
/* Shared HW read buffer for line commands and PUT windows (espd_dev task only). */
static uint8_t s_cdc_rx_buf[ESPD_DEV_RX_CHUNK];

static void dev_reply(const char *msg);
static void dev_put_cleanup_temp(void);
static void dev_put_finish(void);
static void dev_put_recv_window(void);
static int dev_put_read_hw(uint8_t *buf, size_t max);
static bool dev_put_write(const uint8_t *data, size_t len);
static void dev_put_fail(const char *err);
#if CONFIG_ESPD_DEV_CDC_SYNC
static void dev_maybe_suspend(void);
#endif


#if CONFIG_ESPD_DEV_CDC_SYNC
static void dev_maybe_suspend(void)
{
    if (s_sync_active || s_cmd != DEV_CMD_NONE)
        return;
    (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(0));
    vTaskSuspend(NULL);
}
#endif

static int dev_put_read_hw(uint8_t *buf, size_t max)
{
    size_t rx = 0;
    size_t want = max;

    if (want > ESPD_DEV_RX_CHUNK)
        want = ESPD_DEV_RX_CHUNK;
#if CONFIG_ESPD_WIFI_AP_SYNC
    {
        int wn = espd_dev_wifi_read_hw(buf, want);
        if (wn > 0)
            return wn;
    }
#endif
#if CONFIG_ESPD_DEV_SERIAL_SYNC
#if ESPD_DEV_SERIAL_SYNC_UART
    int n = uart_read_bytes(CONFIG_ESP_CONSOLE_UART_NUM, buf, want, pdMS_TO_TICKS(0));
    return n > 0 ? n : 0;
#elif ESPD_DEV_SERIAL_SYNC_USJ
    int n = usb_serial_jtag_read_bytes(buf, want, pdMS_TO_TICKS(0));
    return n > 0 ? n : 0;
#else
    int n = uart_read_bytes(UART_NUM_0, buf, want, pdMS_TO_TICKS(0));
    return n > 0 ? n : 0;
#endif
#elif CONFIG_ESPD_DEV_CDC_SYNC
    if (!tinyusb_cdcacm_initialized(TINYUSB_CDC_ACM_0))
        return 0;
    if (tinyusb_cdcacm_read(TINYUSB_CDC_ACM_0, buf, want, &rx) != ESP_OK || rx == 0)
        return 0;
    return (int)rx;
#else
    (void)buf;
    (void)want;
    return 0;
#endif
}

#if CONFIG_ESPD_DEV_CDC_SYNC
void espd_dev_cdc_rx_cb(int itf, cdcacm_event_t *event)
{
    (void)event;
    if (itf != TINYUSB_CDC_ACM_0)
        return;
    /* All CDC reads run on espd_dev task (TinyUSB callback stack is too small for
     * ESPD_DEV_RX_CHUNK drain loops; host probes the port even without espd_sync). */
    if (!s_dev_task)
        return;
    if (eTaskGetState(s_dev_task) == eSuspended)
        vTaskResume(s_dev_task);
    xTaskNotifyGive(s_dev_task);
}
#endif

static void dev_reply(const char *msg)
{
    char line[192];
    int n;

    if (!msg)
        return;
    n = snprintf(line, sizeof(line), "%s\r\n", msg);
    if (n <= 0)
        return;
    espd_sync_write(line, (size_t)n);
}

static const char *dev_target_mount(dev_target_t target)
{
    if (target == DEV_TARGET_SD)
        return ESPD_SDCARD_MOUNT;
    return ESPD_STORAGE_MOUNT;
}

static const char *dev_target_name(dev_target_t target)
{
    if (target == DEV_TARGET_SD)
        return "sd";
    return "flash";
}

static int dev_sdcard_available(void)
{
#ifdef ESPD_USE_SDCARD
    return espd_storage_sdcard_ready();
#else
    return 0;
#endif
}

static int dev_flash_available(void)
{
    struct stat st;
    if (stat(ESPD_STORAGE_MOUNT, &st) == 0 && S_ISDIR(st.st_mode))
        return 1;

    return 0;
}

static dev_target_t dev_default_target(void)
{
#ifdef ESPD_USE_SDCARD
    if (dev_sdcard_available())
        return DEV_TARGET_SD;
#endif
    return DEV_TARGET_FLASH;
}

static int dev_target_ready(dev_target_t target)
{
    if (target == DEV_TARGET_FLASH) {
        struct stat st;
        if (stat(ESPD_STORAGE_MOUNT, &st) != 0 || !S_ISDIR(st.st_mode))
            return 0;
        return 1;
    }
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
    const unsigned char *p;

    if (!rel || !rel[0] || rel[0] == '/')
        return 0;
    if (strstr(rel, "..") != NULL)
        return 0;
    n = strlen(rel);
    if (n >= ESPD_DEV_PATH_MAX)
        return 0;
    for (p = (const unsigned char *)rel; *p; p++) {
        if (*p < 0x20 || *p >= 0x7f)
            return 0;
        if (*p == '\\')
            return 0;
    }
    return 1;
}

static int dev_name_component_ok(const char *name)
{
    const unsigned char *p;

    if (!name || !name[0] || name[0] == '.')
        return 0;
    for (p = (const unsigned char *)name; *p; p++) {
        if (*p < 0x20 || *p >= 0x7f)
            return 0;
        if (*p == '\\' || *p == ':')
            return 0;
    }
    return 1;
}

static int dev_build_path(char *out, size_t outsz, const char *rel)
{
    int n = snprintf(out, outsz, "%s/%s", dev_target_mount(s_target), rel);
    if (n < 0 || (size_t)n >= outsz)
        return 0;
    return 1;
}

static struct dev_crc_cache_entry *dev_crc_cache_slot(const char *full, uint32_t *out_hash)
{
    uint32_t h = esp_crc32_le(0, (const uint8_t *)full, (uint32_t)strlen(full));
    if (out_hash) *out_hash = h;
    if (!s_crc_cache)
        return NULL;
    return &s_crc_cache[h % ESPD_DEV_CRC_CACHE_SLOTS];
}

static int dev_crc_cache_lookup(const char *full, uint32_t size, uint32_t mtime, uint32_t *out_crc)
{
    uint32_t h;
    struct dev_crc_cache_entry *e = dev_crc_cache_slot(full, &h);
    if (!e)
        return 0;
    if (e->valid && e->path_hash == h && e->size == size && e->mtime == mtime) {
        *out_crc = e->crc;
        return 1;
    }
    return 0;
}

static void dev_crc_cache_store(const char *full, uint32_t size, uint32_t mtime, uint32_t crc)
{
    uint32_t h;
    struct dev_crc_cache_entry *e = dev_crc_cache_slot(full, &h);
    if (!e)
        return;
    e->path_hash = h;
    e->size = size;
    e->mtime = mtime;
    e->crc = crc;
    e->valid = true;
}

static void dev_crc_cache_invalidate(const char *full)
{
    uint32_t h;
    struct dev_crc_cache_entry *e = dev_crc_cache_slot(full, &h);
    if (!e)
        return;
    if (e->valid && e->path_hash == h)
        e->valid = false;
}

/* CRC-32 (same polynomial as Python zlib.crc32). Runs on espd_dev task only. */
static int dev_file_hash(const char *full, size_t *out_size, uint32_t *out_crc)
{
    FILE *fp;
    size_t n, total = 0;
    uint32_t crc = 0;
    struct stat st;
    unsigned chunks = 0;

    if (stat(full, &st) != 0 || !S_ISREG(st.st_mode))
        return -1;
    if ((uint32_t)st.st_size >= ESPD_DEV_CRC_CACHE_MIN_SIZE
            && dev_crc_cache_lookup(full, (uint32_t)st.st_size, (uint32_t)st.st_mtime, &crc)) {
        *out_size = (size_t)st.st_size;
        *out_crc = crc;
        return 0;
    }
    fp = fopen(full, "rb");
    if (!fp)
        return -2;
    while ((n = fread(s_cdc_rx_buf, 1, sizeof(s_cdc_rx_buf), fp)) > 0) {
        crc = esp_crc32_le(crc, s_cdc_rx_buf, (uint32_t)n);
        total += n;
        if ((++chunks & 7u) == 0)
            vTaskDelay(1);
    }
    fclose(fp);
    if (total != (size_t)st.st_size)
        return -2;
    if ((uint32_t)st.st_size >= ESPD_DEV_CRC_CACHE_MIN_SIZE)
        dev_crc_cache_store(full, (uint32_t)st.st_size, (uint32_t)st.st_mtime, crc);
    *out_size = total;
    *out_crc = crc;
    return 0;
}

static void dev_put_cleanup_temp(void)
{
    if (s_put_tmp[0]) {
        unlink(s_put_tmp);
        s_put_tmp[0] = '\0';
    }
#if CONFIG_SPIRAM
    if (s_put_buf) {
        ESP_LOGI(TAG, "dev_put_cleanup_temp: freeing PSRAM buf");
        heap_caps_free(s_put_buf);
        s_put_buf = NULL;
        s_put_buf_pos = 0;
    }
#endif
}

static int dev_put_commit(const char *final_full)
{
    if (!s_put_tmp[0])
        return -1;
    unlink(final_full);
    if (rename(s_put_tmp, final_full) != 0) {
        ESP_LOGW(TAG, "rename %s -> %s failed (%d)", s_put_tmp, final_full, errno);
        dev_put_cleanup_temp();
        return -1;
    }
    s_put_tmp[0] = '\0';
    return 0;
}

static int dev_mkdir_parents(const char *fullpath)
{
    char tmp[ESPD_DEV_PATH_MAX];
    char *p;
    struct stat st;

    if (strlen(fullpath) >= sizeof(tmp))
        return -1;
    strcpy(tmp, fullpath);
    for (p = tmp + 1; *p; p++) {
        if (*p != '/')
            continue;
        *p = '\0';
        if (stat(tmp, &st) == 0) {
            if (!S_ISDIR(st.st_mode)) {
                ESP_LOGW(TAG, "path component is not a directory: %s", tmp);
                *p = '/';
                return -1;
            }
        } else if (mkdir(tmp, 0755) != 0) {
            ESP_LOGW(TAG, "mkdir %s failed (%d)", tmp, errno);
            *p = '/';
            return -1;
        }
        *p = '/';
    }
    return 0;
}

static void dev_put_offer(const char *rel, size_t nbytes, uint32_t expect_crc)
{
    char full[ESPD_DEV_PATH_MAX];
    int open_errno = 0;
    size_t on_disk = 0;
    uint32_t disk_crc = 0;
    int err;
    struct stat st;

#if CONFIG_ESPD_USE_USB_MSC
    if (s_target == DEV_TARGET_FLASH && espd_usb_msc_storage_present()) {
        esp_err_t mnt = espd_usb_ensure_msc_app_mount();
        if (mnt != ESP_OK) {
            dev_reply("-ERR reclaim /storage from host failed (eject USB volume on host)");
            return;
        }
    }
#endif
    if (!dev_target_ready(s_target)) {
        char msg[96];
        snprintf(msg, sizeof(msg), "-ERR target %s not mounted", dev_target_name(s_target));
        dev_reply(msg);
        return;
    }
    if (!dev_rel_path_ok(rel)) {
        dev_reply("-ERR bad path");
        return;
    }
    if (nbytes == 0) {
        dev_reply("-ERR bad size");
        return;
    }
    if (!dev_build_path(full, sizeof(full), rel)) {
        dev_reply("-ERR path too long");
        return;
    }

    if (stat(full, &st) == 0 && S_ISREG(st.st_mode) && (size_t)st.st_size == nbytes) {
        err = dev_file_hash(full, &on_disk, &disk_crc);
        if (err == 0 && disk_crc == expect_crc) {
            dev_reply("+OK PUT skip");
            return;
        }
    }

    /* If updating an existing file, remove it now so the .tmp only competes
     * with free space, not with the old copy. On failure (new file) this is a
     * no-op. Then check free space against the full payload. */
    if (stat(full, &st) == 0 && S_ISREG(st.st_mode)) {
        dev_crc_cache_invalidate(full);
        unlink(full);
    }
    {
        espd_storage_stats_t stats;
        if (espd_storage_get_stats(dev_target_mount(s_target), &stats) == ESP_OK
                && (uint64_t)stats.free_kb * 1024u < (uint64_t)nbytes) {
            char msg[96];
            snprintf(msg, sizeof(msg),
                "-ERR no space: need %zu bytes, %lu KB free",
                nbytes, (unsigned long)stats.free_kb);
            dev_reply(msg);
            return;
        }
    }

    /* Drop any bytes already in the driver queue before binary PUT. */
    while (dev_put_read_hw(s_cdc_rx_buf, sizeof(s_cdc_rx_buf)) > 0)
        continue;

    if (dev_mkdir_parents(full) != 0) {
        dev_reply("-ERR invalid parent path");
        return;
    }
    dev_put_cleanup_temp();

#if CONFIG_SPIRAM
    /* Try to receive into a PSRAM buffer sized to the file — up to whatever the
     * largest contiguous free PSRAM block is.  Falls through to the .tmp path
     * if PSRAM is exhausted or the allocation fails. */
    size_t psram_avail = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (psram_avail > ESPD_DEV_PSRAM_RESERVE && nbytes <= psram_avail - ESPD_DEV_PSRAM_RESERVE) {
        s_put_buf = heap_caps_malloc(nbytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (s_put_buf) {
            s_put_buf_pos = 0;
            //ESP_LOGI(TAG, "PUT %s: using PSRAM buffer", rel);
            goto put_ready;
        }
    }
    ESP_LOGI(TAG, "PUT %s: using .tmp file", rel);
#endif

    if (snprintf(s_put_tmp, sizeof(s_put_tmp), "%s.tmp", full) >= (int)sizeof(s_put_tmp)) {
        dev_reply("-ERR path too long");
        return;
    }
    /* Parent creation for tmp path too (handles reconnect/mount races). */
    if (dev_mkdir_parents(s_put_tmp) != 0) {
        dev_reply("-ERR invalid parent path");
        s_put_tmp[0] = '\0';
        return;
    }
    /* Drop a partial temp from an earlier attempt; keep s_put_tmp for fopen. */
    unlink(s_put_tmp);
    s_put_fp = fopen(s_put_tmp, "wb");
    if (!s_put_fp && errno == ENOENT) {
        /* Deterministic fallback: ensure parent exists, then retry once. */
        if (dev_mkdir_parents(s_put_tmp) != 0) {
            dev_reply("-ERR invalid parent path");
            s_put_tmp[0] = '\0';
            return;
        }
        s_put_fp = fopen(s_put_tmp, "wb");
    }
    if (s_put_fp)
        setvbuf(s_put_fp, NULL, _IONBF, 0);
    if (!s_put_fp) {
        char msg[80];
        open_errno = errno;
        snprintf(msg, sizeof(msg), "-ERR open failed (%d)", open_errno);
        dev_reply(msg);
        ESP_LOGW(TAG, "PUT open %s failed (%d)", s_put_tmp, open_errno);
        s_put_tmp[0] = '\0';
        return;
    }
#if CONFIG_SPIRAM
put_ready:
#endif

    strncpy(s_put_rel, rel, sizeof(s_put_rel) - 1);
    s_put_rel[sizeof(s_put_rel) - 1] = '\0';
    s_put_total = nbytes;
    s_put_remain = nbytes;
    s_put_expect_crc = expect_crc;
    s_put_crc = 0;
    s_cmd = DEV_CMD_PUT;
    {
        char reply[48];
        snprintf(reply, sizeof(reply), "+OK PUT ready window=%u",
            (unsigned)ESPD_DEV_PUT_WINDOW);
        dev_reply(reply);
    }
}

static void dev_put_fail(const char *err)
{
    if (s_put_fp) {
        fclose(s_put_fp);
        s_put_fp = NULL;
    }
    s_put_remain = 0;
    s_cmd = DEV_CMD_NONE;
    dev_put_cleanup_temp();
    dev_reply(err);
}

static bool dev_put_write(const uint8_t *data, size_t len)
{
    size_t n = len;

    if (s_cmd != DEV_CMD_PUT)
        return false;
    if (n > s_put_remain)
        n = s_put_remain;
    if (!n)
        return true;
#if CONFIG_SPIRAM
    if (s_put_buf) {
        memcpy(s_put_buf + s_put_buf_pos, data, n);
        s_put_buf_pos += n;
        s_put_crc = esp_crc32_le(s_put_crc, data, (uint32_t)n);
        s_put_remain -= n;
        return true;
    }
#endif
    if (!s_put_fp)
        return false;
    if (fwrite(data, 1, n, s_put_fp) != n)
        return false;
    s_put_crc = esp_crc32_le(s_put_crc, data, (uint32_t)n);
    s_put_remain -= n;
    return true;
}

static void dev_put_recv_window(void)
{
    char reply[48];
    size_t need;
    size_t got;

#if CONFIG_SPIRAM
    if (!s_put_fp && !s_put_buf)
        return;
#else
    if (!s_put_fp)
        return;
#endif
    if (s_cmd != DEV_CMD_PUT || s_put_remain == 0)
        return;

    need = s_put_remain;
    if (need > ESPD_DEV_PUT_WINDOW)
        need = ESPD_DEV_PUT_WINDOW;
    got = 0;
    while (got < need) {
        int rx = dev_put_read_hw(s_cdc_rx_buf, need - got);
        if (rx <= 0) {
            taskYIELD();
            (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(10));
            continue;
        }
        size_t n = (size_t)rx;
        if (n > need - got)
            n = need - got;
        if (!dev_put_write(s_cdc_rx_buf, n)) {
            dev_put_fail("-ERR write failed");
            return;
        }
        got += n;
    }
    snprintf(reply, sizeof(reply), "+OK PUT ack %zu", s_put_total - s_put_remain);
    dev_reply(reply);
}

static void dev_put_finish(void)
{
    char final_full[ESPD_DEV_PATH_MAX];
    char reply[96];

#if CONFIG_SPIRAM
    if (!s_put_buf && !s_put_fp)
        return;
#else
    if (!s_put_fp)
        return;
#endif
    if (s_cmd != DEV_CMD_PUT || s_put_remain > 0)
        return;
    s_cmd = DEV_CMD_NONE;

    if (s_put_crc != s_put_expect_crc) {
        dev_put_cleanup_temp();
        snprintf(reply, sizeof(reply),
            "-ERR PUT crc exp %08" PRIx32 " got %08" PRIx32,
            s_put_expect_crc, s_put_crc);
        dev_reply(reply);
        return;
    }
    if (!dev_build_path(final_full, sizeof(final_full), s_put_rel)) {
        dev_put_cleanup_temp();
        dev_reply("-ERR commit failed (path)");
        return;
    }

#if CONFIG_SPIRAM
    if (s_put_buf) {
        FILE *fp = fopen(final_full, "wb");
        if (!fp) {
            ESP_LOGW(TAG, "PUT commit fopen %s failed (%d)", final_full, errno);
            dev_put_cleanup_temp();
            dev_reply("-ERR commit failed (open)");
            return;
        }
        size_t written = fwrite(s_put_buf, 1, s_put_total, fp);
        fclose(fp);
        heap_caps_free(s_put_buf);
        s_put_buf = NULL;
        s_put_buf_pos = 0;
        if (written != s_put_total) {
            ESP_LOGW(TAG, "PUT commit fwrite %s: %zu/%zu", final_full, written, s_put_total);
            unlink(final_full);
            dev_reply("-ERR commit failed (write)");
            return;
        }
    } else
#endif
    {
        ESP_LOGI(TAG, "PUT commit %s via .tmp rename (%zu bytes)", final_full, s_put_total);
        FILE *fp = s_put_fp;
        s_put_fp = NULL;
        fflush(fp);
        fclose(fp);
        if (dev_put_commit(final_full) != 0) {
            dev_reply("-ERR commit failed (rename)");
            return;
        }
    }

    /* Cache the CRC we just computed during the write — free hit for next sync. */
    if (s_put_total >= ESPD_DEV_CRC_CACHE_MIN_SIZE) {
        struct stat st;
        if (stat(final_full, &st) == 0 && S_ISREG(st.st_mode))
            dev_crc_cache_store(final_full, (uint32_t)st.st_size,
                (uint32_t)st.st_mtime, s_put_crc);
    }
    snprintf(reply, sizeof(reply), "+OK PUT done %08" PRIx32, s_put_crc);
    dev_reply(reply);
    espd_storage_resolve_paths();
}

static void dev_queue_pdmsg(const char *text)
{
    size_t n;

    if (!g_espd_pd_running) {
        dev_reply("-ERR PD not running");
        return;
    }

    if (!text || !text[0]) {
        dev_reply("-ERR MSG empty");
        return;
    }
    n = strlen(text);
    if (n >= sizeof(s_pdmsg))
        n = sizeof(s_pdmsg) - 1;
    memcpy(s_pdmsg, text, n);
    s_pdmsg[n] = '\0';
    s_pdmsg_pending = true;
    dev_reply("+OK MSG queued");
}

static size_t dev_list_dir(const char *dir_full, const char *rel_prefix)
{
    DIR *d;
    struct dirent *ent;
    size_t count = 0;

    d = opendir(dir_full);
    if (!d)
        return 0;

    while ((ent = readdir(d)) != NULL) {
        char child_rel[ESPD_DEV_PATH_MAX];
        char child_full[ESPD_DEV_PATH_MAX];
        struct stat st;
        int n;

        if (ent->d_name[0] == '.')
            continue;
        if (!dev_name_component_ok(ent->d_name))
            continue;
        if (rel_prefix[0]) {
            n = snprintf(child_rel, sizeof(child_rel), "%s/%s", rel_prefix, ent->d_name);
        } else {
            n = snprintf(child_rel, sizeof(child_rel), "%s", ent->d_name);
        }
        if (n < 0 || (size_t)n >= sizeof(child_rel))
            continue;
        n = snprintf(child_full, sizeof(child_full), "%s/%s", dir_full, ent->d_name);
        if (n < 0 || (size_t)n >= sizeof(child_full))
            continue;
        if (stat(child_full, &st) != 0)
            continue;
        if (S_ISDIR(st.st_mode)) {
            count += dev_list_dir(child_full, child_rel);
        } else if (S_ISREG(st.st_mode)) {
            char line[ESPD_DEV_PATH_MAX + 16];

            if (!dev_rel_path_ok(child_rel))
                continue;
            snprintf(line, sizeof(line), "+FILE %s", child_rel);
            dev_reply(line);
            count++;
        }
    }
    closedir(d);
    return count;
}

static void dev_do_list(void)
{
    char root[64];
    char reply[48];
    size_t n;

#if CONFIG_ESPD_USE_USB_MSC
    if (s_target == DEV_TARGET_FLASH && espd_usb_msc_storage_present()) {
        esp_err_t mnt = espd_usb_ensure_msc_app_mount();
        if (mnt != ESP_OK) {
            dev_reply("-ERR reclaim /storage from host failed (eject USB volume on host)");
            return;
        }
    }
#endif
    if (!dev_target_ready(s_target)) {
        char msg[96];
        snprintf(msg, sizeof(msg), "-ERR target %s not mounted", dev_target_name(s_target));
        dev_reply(msg);
        return;
    }
    strncpy(root, dev_target_mount(s_target), sizeof(root) - 1);
    root[sizeof(root) - 1] = '\0';
    dev_reply("+OK LIST begin");
    n = dev_list_dir(root, "");
    snprintf(reply, sizeof(reply), "+OK LIST done %u", (unsigned)n);
    dev_reply(reply);
}

static void dev_do_rm(const char *rel)
{
    char full[ESPD_DEV_PATH_MAX];
    struct stat st;

#if CONFIG_ESPD_USE_USB_MSC
    if (s_target == DEV_TARGET_FLASH && espd_usb_msc_storage_present()) {
        esp_err_t mnt = espd_usb_ensure_msc_app_mount();
        if (mnt != ESP_OK) {
            dev_reply("-ERR reclaim /storage from host failed (eject USB volume on host)");
            return;
        }
    }
#endif
    if (!dev_target_ready(s_target)) {
        char msg[96];
        snprintf(msg, sizeof(msg), "-ERR target %s not mounted", dev_target_name(s_target));
        dev_reply(msg);
        return;
    }
    if (!dev_rel_path_ok(rel)) {
        dev_reply("-ERR bad path");
        return;
    }
    if (!dev_build_path(full, sizeof(full), rel)) {
        dev_reply("-ERR path too long");
        return;
    }
    if (stat(full, &st) != 0) {
        dev_reply("-ERR not found");
        return;
    }
    if (!S_ISREG(st.st_mode)) {
        dev_reply("-ERR not a file");
        return;
    }
    if (unlink(full) != 0) {
        dev_reply("-ERR unlink failed");
        return;
    }
    dev_reply("+OK RM done");
    espd_storage_resolve_paths();
}

static void dev_do_reload(void)
{
#if CONFIG_ESPD_USE_USB_MSC
    if (s_target == DEV_TARGET_FLASH && espd_usb_msc_storage_present()) {
        esp_err_t mnt = espd_usb_ensure_msc_app_mount();
        if (mnt != ESP_OK) {
            dev_reply("-ERR reclaim /storage from host failed (eject USB volume on host)");
            return;
        }
    }
#endif
    if (!dev_target_ready(s_target)) {
        char msg[96];
        snprintf(msg, sizeof(msg), "-ERR target %s not mounted", dev_target_name(s_target));
        dev_reply(msg);
        return;
    }
    if (!g_espd_pd_running) {
        dev_reply("-ERR PD not running");
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

/* PUT <relpath> <nbytes> <crc> — path may contain spaces; size/CRC are last tokens. */
static int dev_parse_put_line(char *line, char *rel, size_t relsz,
    size_t *nbytes, uint32_t *expect_crc)
{
    char *body = line + 4;
    char *crc_tok;
    char *size_tok;
    char *end;
    unsigned long nb;
    unsigned long crc;

    if (strncmp(line, "PUT ", 4) != 0)
        return 0;
    crc_tok = strrchr(body, ' ');
    if (!crc_tok)
        return 0;
    *crc_tok = '\0';
    size_tok = strrchr(body, ' ');
    if (!size_tok) {
        *crc_tok = ' ';
        return 0;
    }
    crc = strtoul(crc_tok + 1, &end, 16);
    if (end == crc_tok + 1)
        return 0;
    nb = strtoul(size_tok + 1, &end, 10);
    if (end == size_tok + 1 || nb == 0) {
        *crc_tok = ' ';
        return 0;
    }
    *size_tok = '\0';
    if (strlen(body) >= relsz) {
        *size_tok = ' ';
        *crc_tok = ' ';
        return 0;
    }
    strcpy(rel, body);
    *nbytes = (size_t)nb;
    *expect_crc = (uint32_t)crc;
    return 1;
}

static void dev_handle_line(char *line)
{
    if (!line)
        return;
    dev_trim_line(line);
    if (!line[0])
        return;

    if (!strcmp(line, "STATUS")) {
        // activate sync mode after STATUS command
        s_sync_active = false;
        char reply[128];
        snprintf(reply, sizeof(reply), "+OK STATUS sdcard=%s internal=%s",
            dev_sdcard_available() ? "yes" : "no",
            dev_flash_available() ? "yes" : "no");
        dev_reply(reply);
        return;
    }
    if (!strncmp(line, "MSG ", 4)) {
        const char *body = line + 4;
        while (*body == ' ' || *body == '\t')
            body++;
        dev_queue_pdmsg(body);
        return;
    }

    // deactivate sync mode for all other commands
    s_sync_active = true;

    if (!strcmp(line, "RELOAD")) {
        dev_do_reload();
        s_sync_active = false;
        return;
    }
    if (!strcmp(line, "RESET")) {
        dev_reply("+OK RESET rebooting");
        vTaskDelay(pdMS_TO_TICKS(100));
        esp_restart();
        return;
    }
#if CONFIG_ESPD_WIFI_AP_SYNC
    if (!strcmp(line, "APOFF")) {
        dev_reply("+OK APOFF");
        espd_dev_wifi_stop_ap();
        s_sync_active = false;
        return;
    }
#endif
    if (!strcmp(line, "LIST")) {
        dev_do_list();
        return;
    }
    if (!strncmp(line, "RM ", 3)) {
        const char *rel = line + 3;
        while (*rel == ' ' || *rel == '\t')
            rel++;
        dev_do_rm(rel);
        return;
    }
    if (!strncmp(line, "PUT ", 4)) {
        char rel[ESPD_DEV_PATH_MAX];
        size_t nbytes = 0;
        uint32_t expect_crc = 0;
        if (dev_parse_put_line(line, rel, sizeof(rel), &nbytes, &expect_crc))
            dev_put_offer(rel, nbytes, expect_crc);
        else
            dev_reply("-ERR PUT syntax");
        return;
    }
    dev_reply("-ERR unknown command");
}

static void dev_feed_bytes(const uint8_t *buf, size_t rx)
{
    static char line[ESPD_DEV_LINE_MAX];
    static size_t line_len;
    size_t i;

    for (i = 0; i < rx; i++) {
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
    int n;

    while ((n = dev_put_read_hw(s_cdc_rx_buf, sizeof(s_cdc_rx_buf))) > 0)
        dev_feed_bytes(s_cdc_rx_buf, (size_t)n);
}

static void espd_dev_task(void *arg)
{
    (void)arg;
    for (;;) {
        if (s_cmd == DEV_CMD_PUT) {
            if (s_put_remain > 0)
                dev_put_recv_window();
            else if (s_put_fp
#if CONFIG_SPIRAM
                     || s_put_buf
#endif
                    )
                dev_put_finish();
            taskYIELD();
            (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(0));
        } else {
            dev_poll_rx();
#if CONFIG_ESPD_DEV_CDC_SYNC
            dev_maybe_suspend();
#else
            (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(ESPD_DEV_IDLE_WAIT_MS));
#endif
        }
    }
}

void espd_dev_init(void)
{
    if (s_dev_task)
        return;

    s_put_tmp[0] = '\0';
    s_cmd = DEV_CMD_NONE;
    s_reload_pending = false;
    s_sync_active = false;
    s_target = dev_default_target();

#if CONFIG_SPIRAM
    /* Allocate CRC cache in PSRAM to save internal RAM */
    s_crc_cache = heap_caps_malloc(sizeof(struct dev_crc_cache_entry) * ESPD_DEV_CRC_CACHE_SLOTS,
                                   MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_crc_cache) {
        ESP_LOGW(TAG, "CRC cache PSRAM allocation failed, cache disabled");
    }
#else
    /* Fallback to internal RAM if PSRAM not available */
    s_crc_cache = heap_caps_malloc(sizeof(struct dev_crc_cache_entry) * ESPD_DEV_CRC_CACHE_SLOTS,
                                   MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!s_crc_cache) {
        ESP_LOGW(TAG, "CRC cache allocation failed, cache disabled");
    }
#endif

#if CONFIG_ESPD_DEV_SERIAL_SYNC
#if ESPD_DEV_SERIAL_SYNC_UART
    const int uart_num = CONFIG_ESP_CONSOLE_UART_NUM;
    uart_config_t uart_cfg = {
        .baud_rate = CONFIG_ESP_CONSOLE_UART_BAUDRATE,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    if (uart_is_driver_installed(uart_num)) {
        uart_driver_delete(uart_num);
    }
    ESP_ERROR_CHECK(uart_driver_install(uart_num, ESPD_DEV_PUT_WINDOW, 1024, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(uart_num, &uart_cfg));
#if CONFIG_ESP_CONSOLE_UART_CUSTOM
    ESP_ERROR_CHECK(uart_set_pin(uart_num, CONFIG_ESP_CONSOLE_UART_TX_GPIO,
        CONFIG_ESP_CONSOLE_UART_RX_GPIO, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
#endif
    uart_vfs_dev_use_driver(uart_num);
#elif ESPD_DEV_SERIAL_SYNC_USJ
    usb_serial_jtag_driver_config_t usj_config = {
        .rx_buffer_size = ESPD_DEV_PUT_WINDOW,
        .tx_buffer_size = 1024,
    };
    usb_serial_jtag_driver_install(&usj_config);
#else
    /* UART0 may already be installed by console; check before installing */
    if (!uart_is_driver_installed(UART_NUM_0)) {
        uart_driver_install(UART_NUM_0, ESPD_DEV_PUT_WINDOW, 1024, 0, NULL, 0);
    }
#endif
#endif

    if (xTaskCreatePinnedToCore(espd_dev_task, "espd_dev", ESPD_DEV_TASK_STACK, NULL,
            ESPD_DEV_TASK_PRIO, &s_dev_task, ESPD_DEV_TASK_CORE) != pdPASS) {
        ESP_LOGW(TAG, "dev sync task create failed");
        s_dev_task = NULL;
        return;
    }
#if CONFIG_ESPD_DEV_CDC_SYNC
    ESP_LOGI(TAG, "CDC dev sync: PUT/RELOAD -> %s (USB CDC)",
        dev_target_mount(s_target));
#elif CONFIG_ESPD_DEV_SERIAL_SYNC
#if ESPD_DEV_SERIAL_SYNC_USJ
    ESP_LOGI(TAG, "Serial dev sync: PUT/RELOAD -> %s (USB Serial JTAG)",
        dev_target_mount(s_target));
#elif ESPD_DEV_SERIAL_SYNC_UART
    ESP_LOGI(TAG, "Serial dev sync: PUT/RELOAD -> %s (UART)",
        dev_target_mount(s_target));
#else
    ESP_LOGI(TAG, "Serial dev sync: PUT/RELOAD -> %s (UART0)",
        dev_target_mount(s_target));
#endif
#endif
}

TaskHandle_t espd_dev_task_handle(void)
{
    return s_dev_task;
}

bool espd_dev_reload_pending(void)
{
    return s_reload_pending;
}

void espd_dev_clear_reload_pending(void)
{
    s_reload_pending = false;
}

const char *espd_dev_reload_dir(void)
{
    return dev_target_mount(s_target);
}

bool espd_dev_pdmsg_take(char *out, size_t outsz)
{
    size_t n;

    if (!s_pdmsg_pending || !out || outsz == 0)
        return false;
    n = strlen(s_pdmsg);
    if (n >= outsz)
        n = outsz - 1;
    memcpy(out, s_pdmsg, n);
    out[n] = '\0';
    s_pdmsg_pending = false;
    return true;
}

bool espd_dev_sync_poll(void)
{
    if (espd_dev_reload_pending()) {
        ESP_LOGI(TAG, "RELOAD executing from %s", espd_dev_reload_dir());
        pdmain_reload_patch_from(espd_dev_reload_dir());
        espd_dev_clear_reload_pending();
    }
    if (s_pdmsg_pending) {
        char pdmsg[ESPD_DEV_LINE_MAX + 16];
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
    return s_sync_active;
}

#else /* !CONFIG_ESPD_DEV_SYNC */

void espd_dev_init(void) {}
bool espd_dev_reload_pending(void) { return false; }
void espd_dev_clear_reload_pending(void) {}
const char *espd_dev_reload_dir(void) { return NULL; }
bool espd_dev_pdmsg_take(char *out, size_t outsz)
{
    (void)out;
    (void)outsz;
    return false;
}
bool espd_dev_sync_poll(void) { return false; }
TaskHandle_t espd_dev_task_handle(void) { return NULL; }

#endif /* CONFIG_ESPD_DEV_SYNC */
