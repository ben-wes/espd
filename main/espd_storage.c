#include "espd_storage.h"

#include "espd.h"
#include "espd_io.h"

#include "esp_err.h"
#include "esp_log.h"
#include "esp_spiffs.h"
#include <sys/stat.h>
#include <string.h>

static const char *TAG = "espd_storage";

static bool s_spiffs_mounted;
static const char *s_config_path;
static const char *s_main_pd_mount;
#ifdef ESPD_USE_SDCARD
static bool s_sd_mount_tried;
static esp_err_t s_sd_mount_last;
#endif

static int espd_storage_file_exists(const char *path)
{
    struct stat st;
    if (!path)
        return 0;
    if (stat(path, &st) != 0)
        return 0;
    return S_ISREG(st.st_mode);
}

static void espd_storage_mount_spiffs(void)
{
    s_spiffs_mounted = false;

    esp_vfs_spiffs_conf_t cfg = {
        .base_path = ESPD_PATCH_STORE_MOUNT,
        .partition_label = ESPD_PATCH_SPIFFS_PARTITION_LABEL,
        .max_files = 5,
        .format_if_mount_failed = true,
    };
    esp_err_t err = esp_vfs_spiffs_register(&cfg);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "SPIFFS mount (%s @ %s) failed: %s",
            ESPD_PATCH_SPIFFS_PARTITION_LABEL, ESPD_PATCH_STORE_MOUNT,
            esp_err_to_name(err));
        return;
    }

    s_spiffs_mounted = true;
    ESP_LOGI(TAG, "SPIFFS mounted at %s (partition %s)", ESPD_PATCH_STORE_MOUNT,
        ESPD_PATCH_SPIFFS_PARTITION_LABEL);
}

bool espd_storage_sdcard_ready(void)
{
#ifdef ESPD_USE_SDCARD
    struct stat st;
    if (stat(ESPD_SDCARD_MOUNT, &st) != 0 || !S_ISDIR(st.st_mode))
        return false;
    return true;
#else
    return false;
#endif
}

bool espd_storage_flash_ready(void)
{
#if CONFIG_ESPD_USE_USB_MSC
    struct stat st;
    if (stat(ESPD_STORAGE_MOUNT, &st) != 0 || !S_ISDIR(st.st_mode))
        return false;
    return true;
#else
    return false;
#endif
}

/* Active local patch store: SD if mounted, else internal flash, else SPIFFS. */
static const char *espd_storage_active_mount(void)
{
#ifdef ESPD_USE_SDCARD
    if (espd_storage_sdcard_ready())
        return ESPD_SDCARD_MOUNT;
#endif
#if CONFIG_ESPD_USE_USB_MSC
    if (espd_storage_flash_ready())
        return ESPD_STORAGE_MOUNT;
#endif
    if (s_spiffs_mounted)
        return ESPD_PATCH_STORE_MOUNT;
    return NULL;
}

static int espd_storage_mount_is(const char *mount, const char *path)
{
    return mount && path && strcmp(mount, path) == 0;
}

static const char *espd_storage_main_pd_path_for_mount(const char *mount)
{
    if (!mount)
        return NULL;
#ifdef ESPD_USE_SDCARD
    if (espd_storage_mount_is(mount, ESPD_SDCARD_MOUNT))
        return ESPD_SDCARD_MAIN_PD_PATH;
#endif
#if CONFIG_ESPD_USE_USB_MSC
    if (espd_storage_mount_is(mount, ESPD_STORAGE_MOUNT))
        return ESPD_STORAGE_MAIN_PD_PATH;
#endif
    return ESPD_MAIN_PD_PATH;
}

static const char *espd_storage_config_on_mount(const char *mount)
{
    if (!mount)
        return NULL;
#ifdef ESPD_USE_SDCARD
    if (espd_storage_mount_is(mount, ESPD_SDCARD_MOUNT)) {
        if (espd_storage_file_exists(ESPD_SDCARD_CONFIG_PATH))
            return ESPD_SDCARD_CONFIG_PATH;
        return NULL;
    }
#endif
#if CONFIG_ESPD_USE_USB_MSC
    if (espd_storage_mount_is(mount, ESPD_STORAGE_MOUNT)) {
        if (espd_storage_file_exists(ESPD_STORAGE_CONFIG_PATH))
            return ESPD_STORAGE_CONFIG_PATH;
        return NULL;
    }
#endif
    if (espd_storage_mount_is(mount, ESPD_PATCH_STORE_MOUNT)
            && espd_storage_file_exists(ESPD_PATCH_STORE_CONFIG_PATH)) {
        return ESPD_PATCH_STORE_CONFIG_PATH;
    }
    return NULL;
}

static void espd_storage_resolve_paths(void)
{
    const char *mount = espd_storage_active_mount();

    s_main_pd_mount = mount;
    s_config_path = espd_storage_config_on_mount(mount);
}

void espd_storage_init(void)
{
    s_config_path = NULL;
    s_main_pd_mount = NULL;

    espd_storage_mount_spiffs();
    espd_storage_resolve_paths();

    if (s_config_path)
        ESP_LOGI(TAG, "using config.txt at %s", s_config_path);
    else if (espd_storage_active_mount())
        ESP_LOGI(TAG, "no config.txt on %s", espd_storage_active_mount());
    else
        ESP_LOGI(TAG, "no config.txt on local storage");

    if (s_main_pd_mount) {
        const char *main_pd = espd_storage_main_pd_path_for_mount(s_main_pd_mount);
        ESP_LOGI(TAG, "patch store: %s (main.pd%s)",
            s_main_pd_mount,
            (main_pd && espd_storage_file_exists(main_pd)) ? "" : " not present yet");
    } else {
        ESP_LOGI(TAG, "no local patch store mounted");
    }
}

esp_err_t espd_storage_mount_sdcard(void)
{
#ifdef ESPD_USE_SDCARD
    esp_err_t e;

    if (s_sd_mount_tried)
        return s_sd_mount_last;
    s_sd_mount_tried = true;

    e = espd_io_sdcard_mount();
    s_sd_mount_last = e;
    if (e == ESP_OK) {
        ESP_LOGI(TAG, "SD card mounted at %s", ESPD_SDCARD_MOUNT);
        espd_storage_resolve_paths();
        if (s_config_path)
            ESP_LOGI(TAG, "using config.txt at %s", s_config_path);
        if (s_main_pd_mount)
            ESP_LOGI(TAG, "patch store: %s", s_main_pd_mount);
    } else if (e != ESP_ERR_NOT_SUPPORTED) {
        ESP_LOGD(TAG, "no SD card (%s)", esp_err_to_name(e));
        espd_storage_resolve_paths();
    }
    return e;
#else
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

const char *espd_storage_config_path(void)
{
    return s_config_path;
}

const char *espd_storage_main_pd_mount_dir(void)
{
    return s_main_pd_mount;
}

bool espd_storage_spiffs_mounted(void)
{
    return s_spiffs_mounted;
}

bool espd_patch_store_is_mounted(void)
{
    return s_spiffs_mounted;
}

bool espd_patch_store_main_pd_exists(void)
{
    return s_spiffs_mounted && espd_storage_file_exists(ESPD_MAIN_PD_PATH);
}

bool espd_sdcard_main_pd_exists(void)
{
#ifdef ESPD_USE_SDCARD
    return espd_storage_sdcard_ready()
        && espd_storage_file_exists(ESPD_SDCARD_MAIN_PD_PATH);
#else
    return false;
#endif
}

bool espd_storage_main_pd_exists(void)
{
#if CONFIG_ESPD_USE_USB_MSC
    return espd_storage_flash_ready()
        && espd_storage_file_exists(ESPD_STORAGE_MAIN_PD_PATH);
#else
    return false;
#endif
}

void espd_storage_refresh_paths(void)
{
    espd_storage_resolve_paths();
}
