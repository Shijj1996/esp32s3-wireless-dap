/**
 * @file wifi_configuration.h
 * @brief Wi-Fi configuration for the ESP32-S3-BOX-3 wireless DAP
 *
 * Profile: the box joins the **PC's mobile hotspot** ("ESP32-DAP") as a
 * station, so the PC keeps its normal internet connection while debugging.
 * The box uses a static address on the Windows ICS subnet (192.168.137.0/24)
 * that the hotspot always creates, so no DHCP discovery is needed.
 *
 * @copyright Copyright (c) 2020
 *
 */
#ifndef __WIFI_CONFIGURATION__
#define __WIFI_CONFIGURATION__

#include <stdio.h>

/* ------------------------------------------------------------------ */
/* Connection mode                                                     */
/*   0 = STA     (default: join the PC hotspot in wifi_list[])         */
/*   1 = SoftAP  (fallback: the box creates its own "DAP" hotspot)     */
/* ------------------------------------------------------------------ */
#define USE_SOFTAP 0

/* SoftAP settings, only used when USE_SOFTAP == 1 */
#define DAP_AP_SSID       "DAP"
#define DAP_AP_PASSWORD   "12345678"
#define DAP_AP_CHANNEL    1
#define DAP_AP_MAX_CONN   4

/* STA list, used when USE_SOFTAP == 0.
 * The PC's mobile hotspot must be named "ESP32-DAP" with password 12345678. */
static struct {
    const char *ssid;
    const char *password;
} wifi_list[] __attribute__((unused)) = {
    {.ssid = "ESP32-DAP", .password = "12345678"},
    // {.ssid = "OTA", .password = "12345678"},
};

#define WIFI_LIST_SIZE (sizeof(wifi_list) / sizeof(wifi_list[0]))

/* mDNS is a managed component on IDF 5.x; keep it off for now. */
#define USE_MDNS       0
#define MDNS_HOSTNAME "dap"
#define MDNS_INSTANCE "DAP mDNS"

/* Static IP on the Windows ICS hotspot subnet.
 * Windows Mobile Hotspot always serves 192.168.137.0/24 with gateway .1,
 * so fixing the box at .50 avoids having to discover a DHCP lease. */
#define USE_STATIC_IP 1
#define DAP_IP_ADDRESS 192, 168, 137, 50
#define DAP_IP_GATEWAY 192, 168, 137, 1
#define DAP_IP_NETMASK 255, 255, 255, 0

#define USE_OTA              0

#define USE_UART_BRIDGE      0
#define UART_BRIDGE_PORT     1234
#define UART_BRIDGE_BAUDRATE 74880

/* DO NOT CHANGE */
#define USE_TCP_NETCONN 0

#define PORT                3240
#define CONFIG_EXAMPLE_IPV4 1
#define USE_KCP             0
#define MTU_SIZE            1500

#if (USE_TCP_NETCONN == 1 && USE_KCP == 1)
#error Can not use KCP and TCP at the same time!
#endif

#if (USE_KCP == 1)
#warning KCP is a very experimental feature, and it should not be used under any circumstances.
#endif

/* The upstream code calls os_printf() everywhere; on ESP-IDF it is simply
 * stdout (routed to the USB-Serial/JTAG console). */
#define os_printf(fmt, ...) printf(fmt, ##__VA_ARGS__)

#endif
