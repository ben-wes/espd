/* #define PD_USE_BLUETOOTH */
#define PD_USE_WIFI


/* task priorities */
#define  PRIORITY_WIFI 2

#if !defined(CONFIG_LOCALE_NUMBER) || (CONFIG_LOCALE_NUMBER==0)
#define CONFIG_ESP_WIFI_SSID "network"
#define CONFIG_ESP_WIFI_PASSWORD "password"
#define CONFIG_ESP_WIFI_SENDADDR "192.168.1.153"
#define CONFIG_ESP_WIFI_SENDPORT 4498
#define CONFIG_ESP_WIFI_LISTENPORT 4499
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
void net_alive( void);  /* send keep-alive packet if needed */
void net_sendudp(void *msg, int len, int port); /* send whatev */
#endif
