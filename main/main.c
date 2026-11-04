/* BSD Socket API Example

   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/
#include <string.h>
#include <stdint.h>
#include <sys/param.h>

#include "sdkconfig.h"
#include "main/tcp_server.h"
#include "main/tcp_netconn.h"
#include "main/kcp_server.h"
#include "main/uart_bridge.h"
#include "main/timer.h"
#include "main/wifi_configuration.h"
#include "main/wifi_handle.h"

#if (USE_OTA == 1)
#include "components/corsacOTA/src/corsacOTA.h"
#endif

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_log.h"
#include "nvs_flash.h"

#include "lwip/err.h"
#include "lwip/sockets.h"
#include "lwip/sys.h"
#include <lwip/netdb.h>

#if (USE_MDNS == 1)
#include "mdns.h"
#endif

extern void DAP_Setup(void);
extern void DAP_Thread(void *argument);
extern void SWO_Thread();

TaskHandle_t kDAPTaskHandle = NULL;


#if (USE_MDNS == 1)
static const char *MDNS_TAG = "server_common";

void mdns_setup() {
    // initialize mDNS
    int ret;
    ret = mdns_init();
    if (ret != ESP_OK) {
        ESP_LOGW(MDNS_TAG, "mDNS initialize failed:%d", ret);
        return;
    }

    // set mDNS hostname
    ret = mdns_hostname_set(MDNS_HOSTNAME);
    if (ret != ESP_OK) {
        ESP_LOGW(MDNS_TAG, "mDNS set hostname failed:%d", ret);
        return;
    }
    ESP_LOGI(MDNS_TAG, "mDNS hostname set to: [%s]", MDNS_HOSTNAME);

    // set default mDNS instance name
    ret = mdns_instance_name_set(MDNS_INSTANCE);
    if (ret != ESP_OK) {
        ESP_LOGW(MDNS_TAG, "mDNS set instance name failed:%d", ret);
        return;
    }
    ESP_LOGI(MDNS_TAG, "mDNS instance name set to: [%s]", MDNS_INSTANCE);
}
#endif  /* USE_MDNS */

void app_main() {
    // struct rst_info *rtc_info = system_get_rst_info();

    // os_printf("reset reason: %x\n", rtc_info->reason);

    // if (rtc_info->reason == REASON_WDT_RST ||
    //     rtc_info->reason == REASON_EXCEPTION_RST ||
    //     rtc_info->reason == REASON_SOFT_WDT_RST)
    // {
    // if (rtc_info->reason == REASON_EXCEPTION_RST)
    // {
    //     os_printf("Fatal exception (%d):\n", rtc_info->exccause);
    // }
    // os_printf("epc1=0x%08x, epc2=0x%08x, epc3=0x%08x,excvaddr=0x%08x, depc=0x%08x\n",
    //             rtc_info->epc1, rtc_info->epc2, rtc_info->epc3,
    //             rtc_info->excvaddr, rtc_info->depc);
    // }

    ESP_ERROR_CHECK(nvs_flash_init());

#if (USE_UART_BRIDGE == 1)
    uart_bridge_init();
#endif
    wifi_init();
    DAP_Setup();
    timer_init();

#if (USE_MDNS == 1)
    mdns_setup();
#endif


#if (USE_OTA == 1)
    co_handle_t handle;
    co_config_t config = {
        .thread_name = "corsacOTA",
        .stack_size = 3192,
        .thread_prio = 8,
        .listen_port = 3241,
        .max_listen_num = 2,
        .wait_timeout_sec = 60,
        .wait_timeout_usec = 0,
    };

    corsacOTA_init(&handle, &config);
#endif

    // Specify the usbip server task
#if (USE_TCP_NETCONN == 1)
    xTaskCreate(tcp_netconn_task, "tcp_server", 4096, NULL, 14, NULL);
#else // BSD style
    xTaskCreate(tcp_server_task, "tcp_server", 4096, NULL, 14, NULL);
#endif

    // DAP handle task
    // 2048 was enough for the ESP8266/ESP32 ports, but the ESP32-S3 SWD
    // bit-bang path (DAP_ProcessCommand -> SWD_Transfer, plus the first-connect
    // printf) overflows it: "A stack overflow in task DAP_Task".
    //
    // Pinned to core 1 with a raised priority: SWD is bit-banged in this task,
    // and the Wi-Fi driver (priority 23) lives on core 0. When it preempted us
    // in the middle of an SWD frame the clock train got corrupted, the target's
    // SWD state machine lost sync and a flash erase died with
    // "Error connecting DP: cannot read IDR".
    xTaskCreatePinnedToCore(DAP_Thread, "DAP_Task", 6144, NULL, 20, &kDAPTaskHandle, 1);

#if defined CONFIG_IDF_TARGET_ESP8266
    #define UART_BRIDGE_TASK_STACK_SIZE 1024
#else
    #define UART_BRIDGE_TASK_STACK_SIZE 2048
#endif

    //// FIXME: potential stack overflow
#if (USE_UART_BRIDGE == 1)
    xTaskCreate(uart_bridge_task, "uart_server", UART_BRIDGE_TASK_STACK_SIZE, NULL, 2, NULL);
#endif
}
