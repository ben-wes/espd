#include "espd.h"
#include "espd_storage.h"
#include "espd_bsp_sdcard.h"
#include "espd_usb.h"

#include "esp_err.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include <string.h>
#include <sys/stat.h>

static const char *TAG = "espd_storage";

static const char *s_config_path;
static const char *s_main_pd_mount;
#ifdef ESPD_USE_SDCARD
static bool s_sd_mount_tried;
static esp_err_t s_sd_mount_last;
#endif

static int espd_storage_file_exists(const char *path) {
  struct stat st;
  if (!path)
    return 0;
  if (stat(path, &st) != 0)
    return 0;
  return S_ISREG(st.st_mode);
}

bool espd_storage_sdcard_ready(void) {
#ifdef ESPD_USE_SDCARD
  struct stat st;
  if (stat(ESPD_SDCARD_MOUNT, &st) != 0 || !S_ISDIR(st.st_mode))
    return false;
  return true;
#else
  return false;
#endif
}

bool espd_storage_flash_ready(void) {
  struct stat st;
  if (stat(ESPD_STORAGE_MOUNT, &st) == 0 && S_ISDIR(st.st_mode))
    return true;
  /* Host may own FAT; partition still exists for STATUS / dev sync. */
#if CONFIG_ESPD_USE_USB_MSC
  return espd_usb_msc_storage_present();
#else
  return false;
#endif
}

/* Active local patch store: SD if mounted, else internal flash. */
static const char *espd_storage_active_mount(void) {
#ifdef ESPD_USE_SDCARD
  if (espd_storage_sdcard_ready())
    return ESPD_SDCARD_MOUNT;
#endif
  if (espd_storage_flash_ready())
    return ESPD_STORAGE_MOUNT;
  return NULL;
}

static int espd_storage_mount_is(const char *mount, const char *path) {
  return mount && path && strcmp(mount, path) == 0;
}

static const char *espd_storage_main_pd_path_for_mount(const char *mount) {
  if (!mount)
    return NULL;
#ifdef ESPD_USE_SDCARD
  if (espd_storage_mount_is(mount, ESPD_SDCARD_MOUNT))
    return ESPD_SDCARD_MAIN_PD_PATH;
#endif
  if (espd_storage_mount_is(mount, ESPD_STORAGE_MOUNT))
    return ESPD_STORAGE_MAIN_PD_PATH;
  return NULL;
}

static const char *espd_storage_config_on_mount(const char *mount) {
  if (!mount)
    return NULL;
#ifdef ESPD_USE_SDCARD
  if (espd_storage_mount_is(mount, ESPD_SDCARD_MOUNT)) {
    if (espd_storage_file_exists(ESPD_SDCARD_CONFIG_PATH))
      return ESPD_SDCARD_CONFIG_PATH;
    return NULL;
  }
#endif
  if (espd_storage_mount_is(mount, ESPD_STORAGE_MOUNT)) {
    if (espd_storage_file_exists(ESPD_STORAGE_CONFIG_PATH))
      return ESPD_STORAGE_CONFIG_PATH;
    return NULL;
  }
  return NULL;
}

void espd_storage_resolve_paths(void) {
  const char *mount = espd_storage_active_mount();

  s_main_pd_mount = mount;
  s_config_path = espd_storage_config_on_mount(mount);
}

void espd_storage_init(void) {
  s_config_path = NULL;
  s_main_pd_mount = NULL;

  espd_storage_resolve_paths();
  espd_print_storage_stats();

  if (s_config_path)
    ESP_LOGI(TAG, "using config.txt at %s", s_config_path);
  else if (espd_storage_active_mount())
    ESP_LOGI(TAG, "no config.txt on %s", espd_storage_active_mount());
  else
    ESP_LOGI(TAG, "no config.txt on local storage");

  if (s_main_pd_mount) {
    const char *main_pd = espd_storage_main_pd_path_for_mount(s_main_pd_mount);
    ESP_LOGI(TAG, "patch store: %s (main.pd%s)", s_main_pd_mount,
             (main_pd && espd_storage_file_exists(main_pd))
                 ? ""
                 : " not present yet");
  } else {
    ESP_LOGI(TAG, "no local patch store mounted");
  }
}

esp_err_t espd_storage_mount_sdcard(void) {
#ifdef ESPD_USE_SDCARD
  esp_err_t e;

  if (s_sd_mount_tried)
    return s_sd_mount_last;
  s_sd_mount_tried = true;

  e = espd_bsp_sdcard_mount(ESPD_SDCARD_MOUNT);
  s_sd_mount_last = e;
  if (e == ESP_OK) {
    ESP_LOGI(TAG, "SD card mounted at %s", ESPD_SDCARD_MOUNT);
  } else if (e == ESP_ERR_NOT_SUPPORTED) {
    ESP_LOGW(TAG, "No SD card support on this board");
  } else {
    ESP_LOGW(TAG, "SD card mount failed: %s", esp_err_to_name(e));
  }
  return e;
#else
  return ESP_ERR_NOT_SUPPORTED;
#endif
}

const char *espd_storage_config_path(void) { return s_config_path; }

const char *espd_storage_main_pd_mount_dir(void) { return s_main_pd_mount; }

bool espd_sdcard_main_pd_exists(void) {
#ifdef ESPD_USE_SDCARD
  return espd_storage_sdcard_ready() &&
         espd_storage_file_exists(ESPD_SDCARD_MAIN_PD_PATH);
#else
  return false;
#endif
}

bool espd_storage_main_pd_exists(void) {
  return espd_storage_flash_ready() &&
         espd_storage_file_exists(ESPD_STORAGE_MAIN_PD_PATH);
}

esp_err_t espd_print_storage_stats(void) {
  const char *mount = espd_storage_main_pd_mount_dir();
  espd_storage_stats_t stats;

  if (!mount)
    return ESP_ERR_INVALID_STATE;

  esp_err_t err = espd_storage_get_stats(mount, &stats);
  if (err == ESP_OK) {
    ESP_LOGI(TAG, "%s: %lu KB total, %lu KB used, %lu KB free",
             mount, stats.total_kb, stats.used_kb, stats.free_kb);
  }
  return err;
}

esp_err_t espd_storage_get_stats(const char *path, espd_storage_stats_t *stats)
{
    if (!path || !stats)
        return ESP_ERR_INVALID_ARG;

    uint64_t total_bytes = 0;
    uint64_t free_bytes = 0;
    esp_err_t err = esp_vfs_fat_info(path, &total_bytes, &free_bytes);
    if (err != ESP_OK)
        return err;

    stats->total_kb = (uint32_t)(total_bytes / 1024);
    stats->free_kb = (uint32_t)(free_bytes / 1024);
    stats->used_kb = stats->total_kb - stats->free_kb;
    return ESP_OK;
}
