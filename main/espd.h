#ifndef ESPD_H
#define ESPD_H

#if defined(ESPD_BOARD_WAVESHARE_S3)
#include "boards/waveshare_s3/board_profile.h"
#else
/* #define PD_USE_BLUETOOTH */  /* messages to Pd over bluetooth */
#define PD_USE_WIFI             /* messages to/from Pd over wifi TCP */
#define PD_USE_CONSOLE          /* messages to Pd over "console" (USB serial) */
/* #define PD_INCLUDEPATCH */   /* load the patch defined in "testpatch.c" */
/* #define PD_LYRAT */          /* using LyraT or LyraT mini board */
#define USEADC                  /* enable audio input (output always enabled) */
/* #define PD_USE_GYRO */       /* complex Arts board with BNO085 gyro */
#define IOCHANS 2
#define OBSOLETEAPI       /* need this for LyraT boards */
#endif

/* task priorities */
#define  PRIORITY_WIFI 2

    /* if WIFI is enabled we need to define the WIFI name and password,
    the peer machine, and the send and receive ports.  This can be done
    using main/locale.h (copy from locale.h.example) or by defining
    CONFIG_ESP_WIFI_SSID, etc., in sdkconfig / compile flags. */
#if defined(PD_USE_WIFI)
#include "sdkconfig.h"
#endif
#if defined(CONFIG_LOCALE_FILE)
#include CONFIG_LOCALE_FILE
#else
#if defined(PD_USE_WIFI) && !defined(CONFIG_ESP_WIFI_SSID)
#include "locale.h"
#endif
#endif
#if defined(PD_USE_WIFI)
#ifndef CONFIG_ESP_WIFI_SSID
#define CONFIG_ESP_WIFI_SSID "espd"
#endif
#ifndef CONFIG_ESP_WIFI_PASSWORD
#define CONFIG_ESP_WIFI_PASSWORD ""
#endif
#ifndef CONFIG_ESP_WIFI_SENDPORT
#define CONFIG_ESP_WIFI_SENDPORT 4498
#endif
#ifndef CONFIG_ESP_WIFI_LISTENPORT
#define CONFIG_ESP_WIFI_LISTENPORT 4498
#endif
#ifndef CONFIG_ESP_WIFI_SENDADDR
#define CONFIG_ESP_WIFI_SENDADDR "192.168.4.255"
#endif
#endif

#include <sys/types.h>
void pd_sendmsg(char *buf, int bufsize);
void pd_fromhost(char *data, size_t size);

#ifdef PD_USE_BLUETOOTH
void pd_bt_poll( void);
void bt_init( void);
void pd_bt_writeback(unsigned char *s, int length);
#endif

#ifdef PD_USE_WIFI
void wifi_init(void);   /* wifi.c - manage 802.11 connection */
void net_init( void);   /* init */
void net_hello( void);  /* send initial TCP packet when connected */
void net_alive( void);  /* send keep-alive packet if needed */
void net_sendudp(void *msg, int len, int port); /* send whatev */
void net_sendtcp(void *msg, int len);
extern char wifi_ipaddr[];
#endif

#ifndef PIN_BIT_CLOCK       /* fallback pin locations for I2S audio I/O */
#define PIN_BIT_CLOCK 13    /* bit clock */
#define PIN_WORD_SELECT 33  /* word select */
#define PIN_DATA_OUT 32     /* data out from ESP32 to DAC */
#define PIN_DATA_IN 35      /* data in from ADC to ESP32 */
#endif /* PIN_BIT_CLOCK */

#ifndef ESPD_PATCH_STORE_MOUNT
#define ESPD_PATCH_STORE_MOUNT "/espd_pd"
#endif
#ifndef ESPD_PATCH_SPIFFS_PARTITION_LABEL
#define ESPD_PATCH_SPIFFS_PARTITION_LABEL "pdstore"
#endif
#ifndef ESPD_SKIP_WIFI_WHEN_MAIN_PD_ON_DISK
#define ESPD_SKIP_WIFI_WHEN_MAIN_PD_ON_DISK 0
#endif
/** VFS path where the SD card is mounted (ESP-ADF LyraT default: /sdcard). */
#ifndef ESPD_SDCARD_MOUNT
#define ESPD_SDCARD_MOUNT "/sdcard"
#endif
#define ESPD_MAIN_PD_PATH ESPD_PATCH_STORE_MOUNT "/main.pd"

#include "espd_patch_store.h"

extern int espd_main_pd_loaded_from_store;
#ifdef PD_USE_WIFI
extern int espd_wifi_net_enabled;
/** True after net_init() has finished (TCP patch link up). Safe gate for net_send*. */
int espd_net_send_ready(void);
#endif

#endif /* ESPD_H */
