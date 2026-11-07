#pragma once

#include "esp_err.h"

/*
 * 板载三色状态灯（WS2812 / 可寻址 RGB）
 *
 * DevKitC-1 上就是一个 WS2812，单根数据线（默认 GPIO48，见
 * main/dap_configuration.h 的 BOARD_RGB_LED_GPIO）。
 *
 * 颜色含义：
 *   红色闪（1Hz）      没连上 Wi-Fi 热点
 *   蓝色慢呼吸          Wi-Fi 已连上，等 PC 挂载（USBIP 无客户端）
 *   绿色暗              PC 已挂载，链路空闲
 *   绿色亮              正在传数据（OpenOCD / CubeIDE 在跑）
 *
 * 初始化时先做一次 红→绿→蓝 自检，方便确认灯和引脚对不对。
 */
esp_err_t rgb_led_init(void);

/* 手动设置颜色（0..255），主要给自检和调试用 */
void rgb_led_set_rgb(uint8_t r, uint8_t g, uint8_t b);
