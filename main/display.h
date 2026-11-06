#pragma once

#include "esp_err.h"

/*
 * ESP32-S3-BOX-3 板载 2.4" 320x240 LCD 状态显示。
 *
 * 初始化 SPI + ILI9341，启动背光，并创建一个 1Hz 刷新的状态任务，
 * 把 Wi-Fi / USBIP / DAP / 系统信息画到屏幕上。
 *
 * 引脚（来自 espressif/esp-box BSP，esp-box-3.h）：
 *   MOSI=GPIO6  SCLK=GPIO7  CS=GPIO5  DC=GPIO4  RST=GPIO48  BL=GPIO47
 */
esp_err_t display_init(void);
