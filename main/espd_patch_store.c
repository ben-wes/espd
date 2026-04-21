#include "espd_patch_store.h"

#include "espd.h"
#include "esp_log.h"
#include "esp_spiffs.h"
#include <sys/stat.h>
#include <unistd.h>

static const char *TAG = "espd_patch";

static bool s_spiffs_mounted;

void espd_patch_store_init(void)
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
        ESP_LOGW(TAG, "SPIFFS mount (%s @ %s) failed: %s — using embedded patch only",
                 ESPD_PATCH_SPIFFS_PARTITION_LABEL, ESPD_PATCH_STORE_MOUNT,
                 esp_err_to_name(err));
        return;
    }

    s_spiffs_mounted = true;
    ESP_LOGI(TAG, "patch store mounted at %s (partition %s)", ESPD_PATCH_STORE_MOUNT,
             ESPD_PATCH_SPIFFS_PARTITION_LABEL);
}

bool espd_patch_store_is_mounted(void)
{
    return s_spiffs_mounted;
}

bool espd_patch_store_main_pd_exists(void)
{
    struct stat st;
    if (!s_spiffs_mounted)
        return false;
    if (stat(ESPD_MAIN_PD_PATH, &st) != 0)
        return false;
    return S_ISREG(st.st_mode);
}

bool espd_sdcard_main_pd_exists(void)
{
#ifdef PD_USE_SDCARD
    struct stat st;
    if (stat(ESPD_SDCARD_MAIN_PD_PATH, &st) != 0)
        return false;
    return S_ISREG(st.st_mode);
#else
    return false;
#endif
}
