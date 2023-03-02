#include <stdio.h>
#include <string.h>
#include "esp_wifi.h"
#include "esp_err.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "lwip/err.h"
#include "lwip/sys.h"
#include "lwip/opt.h"
#include "lwip/lwip_napt.h"
#include "lwip/netif.h"
#include "lwip/dhcp.h"
#include "/root/esp/esp-idf/components/lwip/apps/dhcpserver/dhcpserver.c"
#include "apps/dhcpserver/dhcpserver.h"
#include "lwip/sockets.h"


EventGroupHandle_t wifi_event_group;

#define DNS_IP_ADDR    0x08080808

const uint16_t MAXIMUM_RETRY_NUM = 10;

#define WIFI_SSID       "WiFi"
#define WIFI_PASSWD     "12345678"

#define ESP32_SSID      "ESP32_AP"
#define ESP32_PASSWD    "12345678"

#define WIFI_FAIL_BIT       BIT0
#define WIFI_CONNECTED_BIT  BIT1
#define WIFI_START          BIT3

static int16_t retry_num = 0;

void nvs_init(void)
{
    //Initialize NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) 
    {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
}

static void *make_request(void *pt)
{
    const struct addrinfo hints = {
        .ai_family = AF_INET,
        .ai_socktype = SOCK_STREAM,
    };
    struct addrinfo *res;
    struct in_addr *addr;
    int s, r;
    char recv_buf[64];

    while(1) {
        int err = getaddrinfo(WEB_SERVER, WEB_PORT, &hints, &res);


        /* Code to print the resolved IP.
           Note: inet_ntoa is non-reentrant, look at ipaddr_ntoa_r for "real" code */
        addr = &((struct sockaddr_in *)res->ai_addr)->sin_addr;
        ESP_LOGI(TAG, "DNS lookup succeeded. IP=%s", inet_ntoa(*addr));

        s = socket(res->ai_family, res->ai_socktype, 0);
        if(s < 0) {
            ESP_LOGE(TAG, "... Failed to allocate socket.");
            freeaddrinfo(res);
            vTaskDelay(1000 / portTICK_PERIOD_MS);
            continue;
        }
        ESP_LOGI(TAG, "... allocated socket");

        if(connect(s, res->ai_addr, res->ai_addrlen) != 0) {
            ESP_LOGE(TAG, "... socket connect failed errno=%d", errno);
            close(s);
            freeaddrinfo(res);
            vTaskDelay(4000 / portTICK_PERIOD_MS);
            continue;
        }

        ESP_LOGI(TAG, "... connected");
        freeaddrinfo(res);

        if (write(s, REQUEST, strlen(REQUEST)) < 0) {
            ESP_LOGE(TAG, "... socket send failed");
            close(s);
            vTaskDelay(4000 / portTICK_PERIOD_MS);
            continue;
        }
        ESP_LOGI(TAG, "... socket send success");

        struct timeval receiving_timeout;
        receiving_timeout.tv_sec = 5;
        receiving_timeout.tv_usec = 0;
        if (setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &receiving_timeout,
                sizeof(receiving_timeout)) < 0) {
            ESP_LOGE(TAG, "... failed to set socket receiving timeout");
            close(s);
            vTaskDelay(4000 / portTICK_PERIOD_MS);
            continue;
        }
        ESP_LOGI(TAG, "... set socket receiving timeout success");

    }
}

void usr_wifi_close(void)
{
    esp_wifi_disconnect();

    esp_wifi_stop();

    esp_wifi_deinit();
}

void usr_wifi_sacn(void)
{
    ESP_LOGI("WIFI STATION", "Wi-Fi扫描");
    wifi_country_t wifi_country_config = {
        .cc = "CN",
        .schan = 1,
        .nchan = 13,
    };
    ESP_ERROR_CHECK(esp_wifi_set_country(&wifi_country_config));
    ESP_ERROR_CHECK(esp_wifi_scan_start(NULL, true));
    uint16_t ap_num = 0;
    ESP_ERROR_CHECK(esp_wifi_scan_get_ap_num(&ap_num));
    ESP_LOGI("WIFI", "AP Count : %d", ap_num);

    uint16_t max_aps = 20;
    wifi_ap_record_t ap_records[max_aps];
    memset(ap_records, 0, sizeof(ap_records));

    uint16_t aps_count = max_aps;
    ESP_ERROR_CHECK(esp_wifi_scan_get_ap_records(&aps_count, ap_records));

    ESP_LOGI("WIFI", "AP Count: %d", aps_count);

    printf("%30s %s %s %s\n", "SSID", "频道", "强度", "MAC地址");

    for (int i = 0; i < aps_count; i++)
    {
        printf("%30s  %3d  %3d  %02X-%02X-%02X-%02X-%02X-%02X\n", 
                ap_records[i].ssid, 
                ap_records[i].primary, 
                ap_records[i].rssi, 
                ap_records[i].bssid[0], 
                ap_records[i].bssid[1], 
                ap_records[i].bssid[2], 
                ap_records[i].bssid[3], 
                ap_records[i].bssid[4], 
                ap_records[i].bssid[5]
            );
    }
}

void usr_wifi_connect(void)
{
    esp_err_t ret = esp_wifi_connect();
    //TODO : send request and auto connect to NJIT
    vTaskDelay(3000/portTICK_PERIOD_MS);
    if(ret == ESP_OK)
    {
        ESP_LOGI("WIFI STATION", "wifi连接成功");
    }
}

void run_on_event(void* event_handler_arg, esp_event_base_t event_base, int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) 
    {
        xEventGroupSetBits(wifi_event_group, WIFI_START);
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) 
    {
        if (retry_num < MAXIMUM_RETRY_NUM) 
        {
            usr_wifi_connect();
            retry_num++;
            ESP_LOGI("WIFI STATION", "retry to connect to the AP");
        } else 
        {
            xEventGroupSetBits(wifi_event_group, WIFI_FAIL_BIT);
        }
        ESP_LOGI("WIFI STATION","connect to the AP fail");
    }else if(event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_STACONNECTED){
        //wifi_event_ap_staconnected_t* event = (wifi_event_ap_staconnected_t*) event_data;
    }
    else if(event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_STADISCONNECTED){
        //wifi_event_ap_stadisconnected_t* event = (wifi_event_ap_stadisconnected_t*) event_data;
    } 
    else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) 
    {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI("WIFI STATION", "got ip:" IPSTR, IP2STR(&event->ip_info.ip));
        retry_num = 0;
        xEventGroupSetBits(wifi_event_group, WIFI_CONNECTED_BIT);
    }
}



void wifi_init_sta(void)
{
    wifi_event_group = xEventGroupCreate();
    //create LwIP
    ESP_ERROR_CHECK(esp_netif_init());

    //create event loop
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    
    esp_netif_create_default_wifi_ap();
    esp_netif_create_default_wifi_sta();

    //create app task
    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, run_on_event, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, run_on_event, NULL);

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    wifi_config_t wifi_sta_config = {
        .sta = {
            .ssid = WIFI_SSID,
            },
    };
    wifi_config_t wifi_ap_config = {
        .ap = {
            .ssid = ESP32_SSID,
            .ssid_len = strlen(ESP32_SSID),
            .channel = 1,
            .password = ESP32_PASSWD,
            .authmode = WIFI_AUTH_WPA_WPA2_PSK,
            .max_connection = 5,
            .pmf_cfg = {
                    .required = false,
            },
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_sta_config));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_ap_config));

    dhcps_t my_dhcp;
    dhcps_offer_t dhcps_dns_value = OFFER_DNS;
    dhcps_set_option_info(&my_dhcp, ESP_NETIF_DOMAIN_NAME_SERVER, &dhcps_dns_value, sizeof(dhcps_dns_value));
    ip_addr_t dnsserver;
    // Set custom dns server address for dhcp server
    dnsserver.u_addr.ip4.addr = htonl(DNS_IP_ADDR);
    dnsserver.type = IPADDR_TYPE_V4;
    dhcps_dns_setserver(&my_dhcp, &dnsserver);

    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI("WIFI STATION", "WIFI 启动成功");
    xEventGroupWaitBits(wifi_event_group,
            WIFI_START,
            pdFALSE,
            pdFALSE,
            portMAX_DELAY);
    usr_wifi_sacn();
    usr_wifi_connect();

    EventBits_t bits = xEventGroupWaitBits(wifi_event_group,
            WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
            pdFALSE,
            pdFALSE,
            portMAX_DELAY);
    if (bits & WIFI_CONNECTED_BIT) 
    {
        ESP_LOGI("WIFI STATION", "connected to ap SSID:%s password:%s",
                 WIFI_SSID, WIFI_PASSWD);
    } else if (bits & WIFI_FAIL_BIT) 
    {
        ESP_LOGI("WIFI STATION", "Failed to connect to SSID:%s, password:%s",
                 WIFI_SSID, WIFI_PASSWD);
    } 

}

void app_main(void)
{
    nvs_init();
    ESP_LOGI("main", "nvs初始化成功");
    wifi_init_sta();
    u32_t napt_netif_ip = 0xC0A80401; // Set to ip address of softAP netif (Default is 192.168.4.1)
    ip_napt_enable(htonl(napt_netif_ip), 1);
    while(true)
    {
        vTaskDelay(1000/portTICK_PERIOD_MS);
    }
}
