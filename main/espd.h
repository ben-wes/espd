/* #define PD_USE_BLUETOOTH */  /* messages to Pd over bluetooth */
#define PD_USE_WIFI             /* messages to/from Pd over wifi TCP */
#define PD_USE_CONSOLE          /* messages to Pd over "console" (USB serial) */
/* #define PD_INCLUDEPATCH */   /* load the patch defined in "testpatch.c" */
/* #define PD_LYRAT */          /* using LyraT or LyraT mini board */
 #define USEADC                 /* enable audio input (output always enabled) */
#define IOCHANS 2
#define OBSOLETEAPI 0

/* task priorities */
#define  PRIORITY_WIFI 2

    /* if WIFI is enabled we need to define the WIFI name and password,
    the peer machine, and the send and receive ports.  This can be done
    using a "locale.h" or by defining CONFIG_ESP_WIFI_SSID, etc., in
    some other way, such as in the sdkconfig file. */
#if defined(CONFIG_LOCALE_FILE)
#include CONFIG_LOCALE_FILE
#else
#if defined(PD_USE_WIFI) && !defined(CONFIG_ESP_WIFI_SSID)
#include "locale.h"
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
