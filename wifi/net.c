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

static const char *TAG = "ESPD";
int tcp_socket;

void tcpreceivertask(void *z)
{
    char rx_buffer[4000];
    int ip_protocol = 0, err, newsocket;
    struct sockaddr_in dest_addr;

    ESP_LOGI(TAG, "tcpreceivertask starting...");
    ESP_LOGE(TAG, "(ignore this - testing error printout)");
    dest_addr.sin_addr.s_addr = inet_addr(CONFIG_ESP_WIFI_SENDADDR);
    dest_addr.sin_family = AF_INET;
    dest_addr.sin_port = htons(CONFIG_ESP_WIFI_SENDPORT);

    newsocket = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (newsocket < 0) {
        ESP_LOGE(TAG, "Unable to create socket: errno %d", errno);
        return;
    }

    ESP_LOGI(TAG, "connecting...");
    while (err = connect(newsocket, &dest_addr, sizeof(dest_addr)) < 0)
    {
        ESP_LOGE(TAG, "Socket unable to bind: errno %d - retrying", errno);
        close(newsocket);
        vTaskDelay(2000 / portTICK_PERIOD_MS);
        newsocket = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
        if (newsocket < 0) {
            ESP_LOGE(TAG, "Unable to create socket: errno %d", errno);
            return;
        }
    }
    tcp_socket = newsocket;
    while (1)
    {
        int len = recv(newsocket, rx_buffer, sizeof(rx_buffer) - 1, 0);

        if (len < 0) {
            ESP_LOGE(TAG, "recvfrom failed: errno %d -- restarting", errno);
            esp_restart();     
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

static int udp_out_sock;
static struct sockaddr_in udp_out_addr;

void net_init( void)
{
    ESP_LOGI(TAG, "net_init...");
        /* socket for sending UDP messages */
    udp_out_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    
    udp_out_addr.sin_addr.s_addr = inet_addr(CONFIG_ESP_WIFI_SENDADDR);
    udp_out_addr.sin_family = AF_INET;
        /* this will get overridden later: */
    udp_out_addr.sin_port = htons(CONFIG_ESP_WIFI_SENDPORT);

    xTaskCreate(tcpreceivertask, "udprcv", 6000, NULL, PRIORITY_WIFI, NULL);
    while (!tcp_socket)
    {
        ESP_LOGE(TAG, "sendtcp: waiting for socket");
        vTaskDelay(1000 / portTICK_PERIOD_MS);
    }
}

static int64_t whensent;

void net_sendudp(void *msg, int len, int port)
{
    int err;
    udp_out_addr.sin_port = htons(port);
    err = sendto(udp_out_sock, msg, len, 0,
        (struct sockaddr *)&udp_out_addr, sizeof(udp_out_addr));
    if (err < 0) {
        static int errorcount;
        errorcount++;
        if (errorcount < 10 || (errorcount < 100 && !(errorcount%10))
            || !(errorcount%100))
        {
            ESP_LOGE(TAG, "udp net send error %d, count %d", errno, errorcount);
        }
    }
    else whensent = esp_timer_get_time();
}

void net_sendtcp(void *msg, int len)
{
    int err;
    if (!tcp_socket)
    {
        ESP_LOGE(TAG, "sendtcp: no socket yet");
        return;
    }
    err = send(tcp_socket, msg, len, 0);
    if (err < 0)
    {
        static int errorcount;
        errorcount++;
        if (errorcount < 10 || (errorcount < 100 && !(errorcount%10))
            || !(errorcount%100))
        {
            ESP_LOGE(TAG, "tcp net send error %d, count %d",
                errno, errorcount);
        }
    }
}

extern char wifi_mac[];
/* esp_err_t esp_base_mac_addr_get(uint8_t *mac); */

void net_alive( void)
{
    int elapsed = esp_timer_get_time() - whensent;
    char buf[80];
    uint8_t mac[6];
    static int  notfirst;
    if (!notfirst)
    {
        notfirst = 1;
        esp_base_mac_addr_get(&mac);
        sprintf(buf, "hello %02x:%02x:%02x:%02x:%02x:%02x;\n",
            mac[0],mac[1],mac[2],mac[3],mac[4],mac[5]);
        net_sendtcp(buf, strlen(buf)); 
    }
    if (elapsed > 1000000)
    {
        esp_base_mac_addr_get(&mac);
        sprintf(buf, "alive %02x:%02x:%02x:%02x:%02x:%02x;\n",
            mac[0],mac[1],mac[2],mac[3],mac[4],mac[5]);
        net_sendudp(buf, strlen(buf), CONFIG_ESP_WIFI_SENDPORT); 
        net_sendtcp(buf, strlen(buf)); 
    }
}
#endif  /* PD_USE_WIFI */
