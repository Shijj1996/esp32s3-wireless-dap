#ifndef __DAP_CONFIGURATION_H__
#define __DAP_CONFIGURATION_H__

#include "sdkconfig.h"

/**
 * @brief Enable the 40 MHz SPI acceleration path (used when the host asks
 *        for a SWD clock >= 10 MHz).
 *
 *        ESP32-S3: not implemented yet. GPSPI2 would have to be routed to
 *        GPIO11/GPIO12 through the GPIO matrix, which needs its own bring-up
 *        and timing validation. Until then the S3 build clamps every clock
 *        request to the GPIO path (<= a few MHz).
 */
#if defined CONFIG_IDF_TARGET_ESP32S3
#define USE_SPI_ACCEL 0
#else
#define USE_SPI_ACCEL 1
#endif

/**
 * @brief Specify the use of WINUSB
 *
 */
#define USE_WINUSB 1

/**
 * @brief Enable this option, no need to physically connect MOSI and MISO
 *
 */
#define USE_SPI_SIO 1


/**
 * @brief 板载 LCD 状态屏（ESP32-S3-BOX-3 的 2.4" 320x240 ILI9341）
 *
 *        BOX-3 上显示 Wi-Fi / USBIP / DAP 的运行状态；
 *        DevKitC-1 之类没有屏幕的板子置 0，编译时不引入显示代码。
 */
#define USE_BOX3_DISPLAY 1


/**
 * @brief Specify to enable USB 3.0
 *
 */
#define USE_USB_3_0 0


// For USB 3.0, it must be 1024 byte.
#if (USE_USB_3_0 == 1)
    #define USB_ENDPOINT_SIZE 1024U
#else
    #define USB_ENDPOINT_SIZE 512U
#endif

/// Maximum Package Size for Command and Response data.
/// This configuration settings is used to optimize the communication performance with the
/// debugger and depends on the USB peripheral. Typical vales are 64 for Full-speed USB HID or WinUSB,
/// 1024 for High-speed USB HID and 512 for High-speed USB WinUSB.

#if (USE_WINUSB == 1)
    #define DAP_PACKET_SIZE 512U // 512 for WinUSB.
#else
    #define DAP_PACKET_SIZE 255U // 255 for USB HID
#endif


#endif
