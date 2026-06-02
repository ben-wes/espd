#include "../main/espd.h"
#ifdef ESPD_USE_WIFI
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "sdkconfig.h"
#include "esp_mac.h"
#include "esp_netif.h"

/* lwip (lightweight IP") for sockets: */
#include "lwip/err.h"
#include "lwip/sockets.h"
#include "lwip/sys.h"
#include <lwip/netdb.h>

#include <stdio.h>
#include <string.h>

static const char *TAG = "ESPD";
int tcp_socket;

static int s_net_send_ready;

int espd_net_send_ready(void)
{
    return s_net_send_ready;
}

void tcpreceivertask(void *z)
{
    char rx_buffer[4000];
    int ip_protocol = 0, err, newsocket;
    struct sockaddr_in dest_addr;

    ESP_LOGI(TAG, "tcpreceivertask starting...");
    dest_addr.sin_addr.s_addr = inet_addr(CONFIG_ESP_WIFI_SENDADDR);
    dest_addr.sin_family = AF_INET;
    dest_addr.sin_port = htons(CONFIG_ESP_WIFI_SENDPORT);

    newsocket = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (newsocket < 0) {
        ESP_LOGE(TAG, "Unable to create socket: errno %d", errno);
        return;
    }

    ESP_LOGI(TAG, "TCP connecting...");
    while ((err = connect(newsocket,
        (struct sockaddr *)&dest_addr, sizeof(dest_addr)) < 0))
    {
        ESP_LOGE(TAG, "Socket unable to connect: errno %d - retrying", errno);
        close(newsocket);
        vTaskDelay(2000 / portTICK_PERIOD_MS);
        newsocket = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
        if (newsocket < 0) {
            ESP_LOGE(TAG, "Unable to create socket: errno %d", errno);
            return;
        }
    }
    tcp_socket = newsocket;
    ESP_LOGI(TAG, "TCP connected.");
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

void udpreceivertask(void *z)
{
    char rx_buffer[1000];
    int ip_protocol = 0, err, udpsocket;
    struct sockaddr_in dest_addr;

    ESP_LOGI(TAG, "udpreceivertask starting...");
    dest_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    dest_addr.sin_family = AF_INET;
    dest_addr.sin_port = htons(CONFIG_ESP_WIFI_LISTENPORT);

    udpsocket = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (udpsocket < 0) {
        ESP_LOGE(TAG, "Unable to create socket: errno %d", errno);
        return;
    }

    ESP_LOGI(TAG, "udp binding...");
    err = bind(udpsocket, (struct sockaddr *)&dest_addr, sizeof(dest_addr));
    if (err < 0) {
        ESP_LOGE(TAG, "Socket unable to bind: errno %d", errno);
    }
    ESP_LOGI(TAG, "udp bound. receiving ...");
    while (1)
    {
        int len = recv(udpsocket, rx_buffer, sizeof(rx_buffer) - 1, 0);

        if (len < 0) {
            ESP_LOGE(TAG, "UDP recvfrom failed: errno %d -- restarting", errno);
            esp_restart();     
        }
        else if (len == 0)
        {
            ESP_LOGE(TAG, "UDP unexpected EOF on socket");
            vTaskDelay(500 / portTICK_PERIOD_MS);
        }
        else
        {
            rx_buffer[len] = 0;
            ESP_LOGI(TAG, "UDP rcv: %s", rx_buffer);
            pd_fromhost(rx_buffer, strlen(rx_buffer));
        }
    }
}

static int udp_out_sock = -1;
static struct sockaddr_in udp_out_addr;
static int udp_out_addr_ready;
static int espd_set_udp_broadcast_addr_from_sta_ip(void)
{
    esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t ip_info;
    uint32_t ip;
    uint32_t mask;
    uint32_t bcast;

    if (!sta)
        return 0;
    if (esp_netif_get_ip_info(sta, &ip_info) != ESP_OK)
        return 0;

    ip = ip_info.ip.addr;
    mask = ip_info.netmask.addr;
    bcast = (ip & mask) | ~mask;
    udp_out_addr.sin_addr.s_addr = bcast;
    ESP_LOGI(TAG, "UDP broadcast addr set from STA ip/netmask");
    return 1;
}

static int espd_ensure_udp_out_socket(void)
{
    int one = 1;
    if (udp_out_sock >= 0)
        return 1;
    udp_out_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (udp_out_sock < 0) {
        ESP_LOGE(TAG, "udp out socket create failed: errno %d", errno);
        return 0;
    }
    if (setsockopt(udp_out_sock, SOL_SOCKET, SO_BROADCAST, &one, sizeof(one)) < 0)
        ESP_LOGW(TAG, "udp out socket SO_BROADCAST failed: errno %d", errno);
    return 1;
}

static void espd_configure_udp_out_addr(void)
{
    if (!espd_set_udp_broadcast_addr_from_sta_ip())
        udp_out_addr.sin_addr.s_addr = inet_addr(CONFIG_ESP_WIFI_SENDADDR);
    udp_out_addr.sin_family = AF_INET;
    udp_out_addr.sin_port = htons(CONFIG_ESP_WIFI_SENDPORT);
    udp_out_addr_ready = 1;
}

void net_init( void)
{
    s_net_send_ready = 0;
    esp_log_level_set(TAG, ESP_LOG_INFO);
    ESP_LOGI(TAG, "net_init...");
    if (espd_ensure_udp_out_socket())
        espd_configure_udp_out_addr();

    /*
     * Stack size is in bytes (ESP-IDF). tcprcv holds rx_buffer[4000] plus
     * lwIP/socket and pd_fromhost() — 6000 was far too small (overflow).
     * Pin to Core 0 to prevent CPU contention with the CPU 1 audio thread.
     */
    xTaskCreatePinnedToCore(tcpreceivertask, "tcprcv", 20480, NULL, PRIORITY_WIFI, NULL, 0);
    xTaskCreatePinnedToCore(udpreceivertask, "udprcv", 8192, NULL, PRIORITY_WIFI, NULL, 0);
    s_net_send_ready = 1;
}

static int64_t whensent;

void net_sendudp(void *msg, int len, int port)
{
    int err;
    if (!espd_ensure_udp_out_socket())
        return;
    if (!udp_out_addr_ready)
        espd_configure_udp_out_addr();
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
    if (!s_net_send_ready || !tcp_socket)
        return;
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

void net_hello( void)
{
    int elapsed = esp_timer_get_time() - whensent;
    char buf[80];
    uint8_t mac[6];
    {
        esp_base_mac_addr_get(mac);
        sprintf(buf, "hello %02x:%02x:%02x:%02x:%02x:%02x %s;\n",
            mac[0],mac[1],mac[2],mac[3],mac[4],mac[5], wifi_ipaddr);
        net_sendtcp(buf, strlen(buf)); 
    }
}

void net_alive( void)
{
    int elapsed = esp_timer_get_time() - whensent;
    char buf[80];
    uint8_t mac[6];
    if (elapsed > 1000000)
    {
        esp_base_mac_addr_get(mac);
        sprintf(buf, "alive %02x:%02x:%02x:%02x:%02x:%02x;\n",
            mac[0],mac[1],mac[2],mac[3],mac[4],mac[5]);
        net_sendudp(buf, strlen(buf), CONFIG_ESP_WIFI_SENDPORT); 
        net_sendtcp(buf, strlen(buf)); 
    }
}
#endif  /* ESPD_USE_WIFI */
