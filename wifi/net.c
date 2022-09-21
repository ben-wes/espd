#include "../main/espd.h"
#ifdef PD_USE_WIFI
#include "freertos/FreeRTOS.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "sdkconfig.h"
#include "esp_mac.h"

/* lwip (lightweight IP") for sockets: */
#include "lwip/err.h"
#include "lwip/sockets.h"
#include "lwip/sys.h"
#include <lwip/netdb.h>

#include <stdio.h>
#include <string.h>

static const char *TAG = "net";

void udpreceivertask(void *z)
{
    char rx_buffer[4000];
    int ip_protocol = 0;
    struct sockaddr_in dest_addr;

    dest_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    dest_addr.sin_family = AF_INET;
    dest_addr.sin_port = htons(CONFIG_ESP_WIFI_LISTENPORT);
    ip_protocol = IPPROTO_IP;

    int rcv_sock = socket(AF_INET, SOCK_DGRAM, ip_protocol);
    if (rcv_sock < 0) {
        ESP_LOGE(TAG, "Unable to create socket: errno %d", errno);
        return;
    }

    int err = bind(rcv_sock, (struct sockaddr *)&dest_addr, sizeof(dest_addr));
    if (err < 0) {
        ESP_LOGE(TAG, "Socket unable to bind: errno %d", errno);
    }
    ESP_LOGI(TAG, "Waiting for data");

    while (1)
    {
        int len = recv(rcv_sock, rx_buffer, sizeof(rx_buffer) - 1, 0);

        if (len < 0) {
            ESP_LOGE(TAG, "recvfrom failed: errno %d", errno);
            vTaskDelay(5000 / portTICK_PERIOD_MS);        
            continue;
        }
        else if (len == 0)
        {
            ESP_LOGE(TAG, "unexpected EOF on socket");
            vTaskDelay(500 / portTICK_PERIOD_MS);
        }
        else
        {
            rx_buffer[len] = 0;
            ESP_LOGI(TAG, "rcv: %s", rx_buffer);
            pd_fromhost(rx_buffer, strlen(rx_buffer));
        }
    }
}

static int xyz_sock;
static struct sockaddr_in xyz_dest_addr;

void net_init( void)
{
        /* socket to send over -- could use same one? */
    xyz_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    
    xyz_dest_addr.sin_addr.s_addr = inet_addr(CONFIG_ESP_WIFI_SENDADDR);
    xyz_dest_addr.sin_family = AF_INET;
        /* this will get overridden later: */
    xyz_dest_addr.sin_port = htons(CONFIG_ESP_WIFI_SENDPORT);

    xTaskCreate(udpreceivertask, "udprcv", 6000, NULL, PRIORITY_WIFI, NULL);
}

static int64_t whensent;

void net_sendudp(void *msg, int len, int port)
{
    int err;
    xyz_dest_addr.sin_port = htons(port);
    err = sendto(xyz_sock, msg, len, 0,
        (struct sockaddr *)&xyz_dest_addr, sizeof(xyz_dest_addr));
    if (err < 0) {
        static int errorcount;
        errorcount++;
        if (errorcount < 10 || (errorcount < 100 && !(errorcount%10))
            || !(errorcount%100))
        {
            ESP_LOGE(TAG, "net send error count %d", errno);
        }
    }
    else whensent = esp_timer_get_time();
}

extern char wifi_mac[];
/* esp_err_t esp_base_mac_addr_get(uint8_t *mac); */

void net_alive( void)
{
    int elapsed = esp_timer_get_time() - whensent;
    if (elapsed > 1000000)
    {
        char buf[80];
        uint8_t mac[6];
        esp_base_mac_addr_get(&mac);
        sprintf(buf, "alive %02x:%02x:%02x:%02x:%02x:%02x;\n",
            mac[0],mac[1],mac[2],mac[3],mac[4],mac[5]);
        net_sendudp(buf, strlen(buf), CONFIG_ESP_WIFI_SENDPORT); 
    }
}
#endif  /* PD_USE_WIFI */
