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

static const char *espd_storage_probe_config(void)
{
#ifdef PD_USE_SDCARD
    if (espd_storage_file_exists(ESPD_SDCARD_CONFIG_PATH))
        return ESPD_SDCARD_CONFIG_PATH;
#endif
    if (s_spiffs_mounted && espd_storage_file_exists(ESPD_PATCH_STORE_CONFIG_PATH))
        return ESPD_PATCH_STORE_CONFIG_PATH;
#ifdef PD_USE_USB_MSC
    if (espd_storage_file_exists(ESPD_STORAGE_CONFIG_PATH))
        return ESPD_STORAGE_CONFIG_PATH;
#endif
    return NULL;
}

static const char *espd_storage_probe_main_pd(void)
{
#ifdef PD_USE_SDCARD
    if (espd_storage_file_exists(ESPD_SDCARD_MAIN_PD_PATH))
        return ESPD_SDCARD_MOUNT;
#endif
    if (s_spiffs_mounted && espd_storage_file_exists(ESPD_MAIN_PD_PATH))
        return ESPD_PATCH_STORE_MOUNT;
#ifdef PD_USE_USB_MSC
    if (espd_storage_file_exists(ESPD_STORAGE_MAIN_PD_PATH))
        return ESPD_STORAGE_MOUNT;
#endif
    return NULL;
}

static void espd_storage_resolve_paths(void)
{
    s_config_path = espd_storage_probe_config();
    s_main_pd_mount = espd_storage_probe_main_pd();
}

void espd_storage_init(void)
{
    s_config_path = NULL;
    s_main_pd_mount = NULL;

    espd_storage_mount_spiffs();
    espd_storage_resolve_paths();

    if (s_config_path)
        ESP_LOGI(TAG, "using config.txt at %s", s_config_path);
    else
        ESP_LOGI(TAG, "no config.txt on SD or %s", ESPD_PATCH_STORE_MOUNT);

    if (s_main_pd_mount)
        ESP_LOGI(TAG, "main.pd on %s", s_main_pd_mount);
    else
        ESP_LOGI(TAG, "no main.pd on local storage");
}

esp_err_t espd_storage_mount_sdcard(void)
{
#ifdef PD_USE_SDCARD
    esp_err_t e = espd_io_sdcard_mount();
    if (e == ESP_OK) {
        ESP_LOGI(TAG, "SD card mounted at %s", ESPD_SDCARD_MOUNT);
        espd_storage_resolve_paths();
        if (s_config_path)
            ESP_LOGI(TAG, "using config.txt at %s", s_config_path);
        if (s_main_pd_mount)
            ESP_LOGI(TAG, "main.pd on %s", s_main_pd_mount);
    } else if (e != ESP_ERR_NOT_SUPPORTED) {
        ESP_LOGW(TAG, "SD card not mounted: %s", esp_err_to_name(e));
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

bool espd_storage_local_main_pd_present(void)
{
    return s_main_pd_mount != NULL;
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
#ifdef PD_USE_SDCARD
    return espd_storage_file_exists(ESPD_SDCARD_MAIN_PD_PATH);
#else
    return false;
#endif
}

bool espd_storage_main_pd_exists(void)
{
#ifdef PD_USE_USB_MSC
    return espd_storage_file_exists(ESPD_STORAGE_MAIN_PD_PATH);
#else
    return false;
#endif
}
