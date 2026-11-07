/**
 * @file wifi_handle.c
 * @brief Wi-Fi bring-up for ESP-IDF 5.x (esp_netif + esp_event)
 *
 * Differences to the original ESP-IDF 4.4 implementation:
 *   - tcpip_adapter_*        -> esp_netif_*
 *   - esp_event_loop_init()  -> esp_event_loop_create_default()
 *                               + esp_event_handler_instance_register()
 *   - system_event_t         -> esp_event_base_t / int32_t id
 *   - ESP_IF_WIFI_*          -> WIFI_IF_*
 *
 * Default mode is SoftAP so that the box can be used without a router.
 */
#include "sdkconfig.h"

#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "main/wifi_configuration.h"
#include "main/wifi_handle.h"
#include "main/usbip_server.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "nvs_flash.h"

#include "lwip/ip4_addr.h"

static const char *TAG = "wifi";

static EventGroupHandle_t wifi_event_group;
static const int WIFI_READY_BIT = BIT0;

#if (USE_SOFTAP == 0)
static int ssid_index = 0;

/* Print the APs the radio can see. Used when the configured SSID is not
 * found, to tell apart "wrong SSID", "5 GHz AP" and "channel 12/13". */
static void scan_dump(void)
{
    wifi_scan_config_t scan = {
        .ssid = NULL, .bssid = NULL, .channel = 0,
        .show_hidden = true, .scan_type = WIFI_SCAN_TYPE_ACTIVE,
    };
    uint16_t n = 0;
    wifi_ap_record_t *recs;

    if (esp_wifi_scan_start(&scan, true) != ESP_OK) {
        ESP_LOGW(TAG, "scan failed");
        return;
    }
    esp_wifi_scan_get_ap_num(&n);
    if (n == 0) {
        ESP_LOGW(TAG, "scan: no AP visible at all");
        return;
    }
    if (n > 20) {
        n = 20;
    }
    recs = calloc(n, sizeof(wifi_ap_record_t));
    if (!recs) {
        return;
    }
    esp_wifi_scan_get_ap_records(&n, recs);

    ESP_LOGI(TAG, "scan found %u AP(s), showing %u:", (unsigned)n, (unsigned)n);
    for (int i = 0; i < n; i++) {
        ESP_LOGI(TAG, "  ch=%2u rssi=%4d auth=%d  \"%.32s\"",
                 recs[i].primary, recs[i].rssi, recs[i].authmode,
                 (const char *)recs[i].ssid);
    }
    free(recs);
}

static void ssid_change(void)
{
    wifi_config_t wifi_config = { 0 };

    if (ssid_index > (int)WIFI_LIST_SIZE - 1) {
        ssid_index = 0;
    }

    strncpy((char *)wifi_config.sta.ssid, wifi_list[ssid_index].ssid,
            sizeof(wifi_config.sta.ssid) - 1);
    strncpy((char *)wifi_config.sta.password, wifi_list[ssid_index].password,
            sizeof(wifi_config.sta.password) - 1);
    ssid_index++;

    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
}
#endif

static void wifi_event_handler(void *arg, esp_event_base_t base,
                               int32_t id, void *data)
{
    if (base == WIFI_EVENT) {
        switch (id) {
#if (USE_SOFTAP == 1)
        case WIFI_EVENT_AP_START:
            ESP_LOGI(TAG, "SoftAP started");
            xEventGroupSetBits(wifi_event_group, WIFI_READY_BIT);
            break;

        case WIFI_EVENT_AP_STACONNECTED: {
            wifi_event_ap_staconnected_t *e =
                (wifi_event_ap_staconnected_t *)data;
            ESP_LOGI(TAG, "station " MACSTR " joined, AID=%d",
                     MAC2STR(e->mac), e->aid);
            break;
        }

        case WIFI_EVENT_AP_STADISCONNECTED: {
            wifi_event_ap_stadisconnected_t *e =
                (wifi_event_ap_stadisconnected_t *)data;
            ESP_LOGI(TAG, "station " MACSTR " left, AID=%d",
                     MAC2STR(e->mac), e->aid);
            break;
        }
#else
        case WIFI_EVENT_STA_START:
            esp_wifi_connect();
            break;

        case WIFI_EVENT_STA_CONNECTED: {
            wifi_event_sta_connected_t *e = (wifi_event_sta_connected_t *)data;
            ESP_LOGI(TAG, "connected to \"%.*s\" (channel %d), link up",
                     e->ssid_len, (const char *)e->ssid, e->channel);
            /* With a static IP the address is already configured, so the
             * link-up event is what matters here (GOT_IP may not fire). */
            xEventGroupSetBits(wifi_event_group, WIFI_READY_BIT);
            break;
        }

        case WIFI_EVENT_STA_DISCONNECTED: {
            wifi_event_sta_disconnected_t *e =
                (wifi_event_sta_disconnected_t *)data;
            static int fail_count = 0;

            ESP_LOGW(TAG, "disconnected (reason %d), retrying", e->reason);
            xEventGroupClearBits(wifi_event_group, WIFI_READY_BIT);

            /* Wi-Fi 断了，原来的 TCP 连接不可能再恢复：请求 TCP 循环把它关掉。
             * 否则 kSock 会一直 >= 0 —— 状态灯永远显示"已挂载"，
             * 重新连上热点后也不会回到"等待挂载"。
             *
             * 注意这里**不需要**重建监听 socket：它绑的是 INADDR_ANY，
             * Wi-Fi 恢复拿到 IP 后照样能用。如果掉线期间 select/accept 真报错，
             * TCP 任务会自己重建（那条路径有 1 秒重试，不会退出）。 */
            kForceCloseClient = 1;

            /* reason 201 = NO_AP_FOUND: dump what the radio can actually see.
             * A 5 GHz or ch12/13 hotspot is the usual cause. */
            if (e->reason == WIFI_REASON_NO_AP_FOUND && (++fail_count % 3) == 0) {
                scan_dump();
            }
            ssid_change();
            esp_wifi_connect();
            break;
        }
#endif
        default:
            break;
        }
    } else if (base == IP_EVENT) {
#if (USE_SOFTAP == 0)
        if (id == IP_EVENT_STA_GOT_IP) {
            ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
            ESP_LOGI(TAG, "got ip: " IPSTR, IP2STR(&e->ip_info.ip));
            xEventGroupSetBits(wifi_event_group, WIFI_READY_BIT);
        }
#endif
    }
}

static void print_ip(const char *ifkey)
{
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey(ifkey);
    esp_netif_ip_info_t ip;

    if (netif == NULL) {
        return;
    }
    if (esp_netif_get_ip_info(netif, &ip) == ESP_OK) {
        ESP_LOGI(TAG, "%s ip: " IPSTR ", gw: " IPSTR, ifkey,
                 IP2STR(&ip.ip), IP2STR(&ip.gw));
    }
}

void wifi_init(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    wifi_event_group = xEventGroupCreate();
    configASSERT(wifi_event_group);

#if (USE_SOFTAP == 1)
    esp_netif_create_default_wifi_ap();
#else
    esp_netif_t *sta_netif = esp_netif_create_default_wifi_sta();

#if (USE_STATIC_IP == 1)
    /* Windows ICS hotspot always uses 192.168.137.1/24, so we can skip DHCP
     * entirely and use a fixed address. */
    {
        esp_netif_ip_info_t ip_info = { 0 };
        esp_netif_dhcpc_stop(sta_netif);
        /* The address macros expand to "192, 168, 137, 50"; the extra
         * variadic hop is required so IP4_ADDR() receives 4 arguments. */
#define DAP_IP4_SET(addr, ...) IP4_ADDR(addr, __VA_ARGS__)
        DAP_IP4_SET(&ip_info.ip,      DAP_IP_ADDRESS);
        DAP_IP4_SET(&ip_info.gw,      DAP_IP_GATEWAY);
        DAP_IP4_SET(&ip_info.netmask, DAP_IP_NETMASK);
#undef DAP_IP4_SET
        if (esp_netif_set_ip_info(sta_netif, &ip_info) != ESP_OK) {
            ESP_LOGW(TAG, "static IP setup failed, falling back to DHCP");
            esp_netif_dhcpc_start(sta_netif);
        } else {
            ESP_LOGI(TAG, "static IP: " IPSTR "/" IPSTR " gw " IPSTR,
                     IP2STR(&ip_info.ip), IP2STR(&ip_info.netmask),
                     IP2STR(&ip_info.gw));
        }
    }
#endif
#endif

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));

    /* China allows channels 1..13; the default "world" country code only
     * uses 1..11, which hides hotspots that Windows puts on ch 12/13. */
    esp_err_t cc = esp_wifi_set_country_code("CN", true);
    if (cc != ESP_OK) {
        ESP_LOGW(TAG, "set country code failed: %s", esp_err_to_name(cc));
    }

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));

#if (USE_SOFTAP == 1)
    {
        wifi_config_t wifi_config = { 0 };

        strncpy((char *)wifi_config.ap.ssid, DAP_AP_SSID,
                sizeof(wifi_config.ap.ssid) - 1);
        strncpy((char *)wifi_config.ap.password, DAP_AP_PASSWORD,
                sizeof(wifi_config.ap.password) - 1);
        wifi_config.ap.ssid_len = (uint8_t)strlen(DAP_AP_SSID);
        wifi_config.ap.channel = DAP_AP_CHANNEL;
        wifi_config.ap.max_connection = DAP_AP_MAX_CONN;
        wifi_config.ap.authmode = WIFI_AUTH_WPA2_PSK;
        wifi_config.ap.pmf_cfg.required = false;

        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_config));
        ESP_LOGI(TAG, "starting SoftAP ssid=\"%s\" password=\"%s\"",
                 DAP_AP_SSID, DAP_AP_PASSWORD);
    }
#else
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
    ssid_change();
    ESP_LOGI(TAG, "starting STA mode");
#endif

    ESP_ERROR_CHECK(esp_wifi_start());

    xEventGroupWaitBits(wifi_event_group, WIFI_READY_BIT, pdFALSE, pdTRUE,
                        portMAX_DELAY);

#if (USE_SOFTAP == 1)
    print_ip("WIFI_AP_DEF");
    ESP_LOGI(TAG, "connect the PC to Wi-Fi \"%s\" and run: "
                  "usbip attach -r 192.168.4.1 -b 1-1",
             DAP_AP_SSID);
#else
    print_ip("WIFI_STA_DEF");
#if (USE_STATIC_IP == 1)
    ESP_LOGI(TAG, "wireless DAP ready at 192.168.137.50:3240 "
                  "(PC hotspot SSID \"ESP32-DAP\")");
#else
    ESP_LOGI(TAG, "wireless DAP ready, use the IP printed above");
#endif
#endif
}
