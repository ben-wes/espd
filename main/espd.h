/* #define PD_USE_BLUETOOTH */
#define PD_USE_WIFI


/* task priorities */
#define  PRIORITY_WIFI 2

#if defined(CONFIG_LOCALE_FILE)
#include CONFIG_LOCALE_FILE
#else
#include "locale.h"
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
