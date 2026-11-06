#pragma once

#include <stdint.h>

/*
 * PingFang 14px、4bpp 的等宽/比例混合点阵字体，ASCII 0x20..0x7F。
 *
 * 数据来源：espressif/esp-box 示例里的 LVGL 字体（ui_font_PingFangEN14.c），
 * 这里去掉了 LVGL 依赖，只保留位图和字形描述，由 display.c 自己渲染。
 *
 * 坐标约定（与 LVGL v8 一致）：
 *   - 行高 16px，基线距行顶 13px（= LINE_HEIGHT - BASE_LINE）
 *   - 字形位图左上角 = (cursor_x + ofs_x, baseline - ofs_y - box_h)
 *   - 推进量 = adv_w / 16 像素
 */
#define FONT14_LINE_HEIGHT 16
#define FONT14_BASE_LINE   3
#define FONT14_BPP         4

typedef struct {
    uint16_t bitmap_index;  // 在位图数组中的字节偏移
    uint16_t adv_w;         // 推进量，单位 1/16 像素
    uint16_t box_w;         // 位图宽（像素）
    uint16_t box_h;         // 位图高（像素）
    int16_t  ofs_x;         // 相对光标的 x 偏移
    int16_t  ofs_y;         // 相对基线的 y 偏移（向上为正）
} g_font14_glyph_t;

extern const uint8_t         g_font14_bitmap[];
extern const g_font14_glyph_t g_font14_glyph[];   // [0] 保留，[1]=' ' ... [96]='\x7F'
