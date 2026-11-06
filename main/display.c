/*
 * ESP32-S3-BOX-3 状态显示屏（2.4" 320x240，ILI9341，SPI3）
 *
 * 只做一件事：每秒把「Wi-Fi / USBIP / DAP / 系统」状态画到屏幕上，
 * 方便现场不看串口也能知道盒子在干什么。
 */

#include <string.h>
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "driver/spi_master.h"
#include "driver/ledc.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_types.h"
#include "esp_lcd_ili9341.h"

#include "esp_log.h"
#include "esp_check.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "esp_wifi.h"
#include "esp_netif.h"

#include "display.h"
#include "font_pingfang14.h"
#include "usbip_server.h"

// DAP 运行状态（DAP.c 里的全局）
#include "components/DAP/include/DAP.h"
extern DAP_Data_t DAP_Data;

static const char *TAG = "display";

/* ------------------------------------------------------------------ 引脚 --- */
#define LCD_H_RES           320
#define LCD_V_RES           240

#define LCD_PIN_MOSI        6
#define LCD_PIN_SCLK        7
#define LCD_PIN_CS          5
#define LCD_PIN_DC          4
#define LCD_PIN_RST         48
#define LCD_PIN_BL          47

#define LCD_PIXEL_CLOCK_HZ  (40 * 1000 * 1000)
#define LCD_SPI_HOST        SPI3_HOST

/* 颜色：面板是 BGR 顺序，这里用 bgr() 按“肉眼 RGB”书写，避免红蓝颠倒 */
#define bgr(r, g, b) ((uint16_t)((((b) & 0xF8) << 8) | (((g) & 0xFC) << 3) | (((r) & 0xF8) >> 3)))

#define COL_BG      bgr(0x10, 0x14, 0x1C)   // 深灰蓝底
#define COL_TITLE   bgr(0x60, 0xD0, 0xFF)   // 天蓝
#define COL_LABEL   bgr(0x9A, 0xA4, 0xB0)   // 灰
#define COL_VALUE   bgr(0xE8, 0xF0, 0xF8)   // 近白
#define COL_OK      bgr(0x60, 0xE0, 0x80)   // 绿
#define COL_WARN    bgr(0xFF, 0xC0, 0x50)   // 橙
#define COL_ERR     bgr(0xFF, 0x70, 0x70)   // 红
#define COL_LINE    bgr(0x30, 0x38, 0x44)   // 分隔线

static esp_lcd_panel_handle_t s_panel = NULL;
static esp_lcd_panel_io_handle_t s_io = NULL;

/* 一行文本的绘制缓冲：320 x 16 像素 */
static uint16_t s_line_buf[LCD_H_RES * FONT14_LINE_HEIGHT];

/* ------------------------------------------------------------------ 绘制 --- */

static inline uint16_t blend565(uint16_t bg, uint16_t fg, uint8_t a)
{
    if (a == 0)   return bg;
    if (a == 255) return fg;

    uint32_t br = (bg >> 11) & 0x1F, bgc = (bg >> 5) & 0x3F, bb = bg & 0x1F;
    uint32_t fr = (fg >> 11) & 0x1F, fgc = (fg >> 5) & 0x3F, fb = fg & 0x1F;

    uint32_t r = (fr * a + br * (255 - a)) / 255;
    uint32_t g = (fgc * a + bgc * (255 - a)) / 255;
    uint32_t b = (fb * a + bb * (255 - a)) / 255;

    return (uint16_t)((r << 11) | (g << 5) | b);
}

/* 把一整个 16px 高的文本行（含背景）刷到屏幕第 y 行 */
static void flush_line(int y, uint16_t bg)
{
    esp_lcd_panel_draw_bitmap(s_panel, 0, y, LCD_H_RES, y + FONT14_LINE_HEIGHT, s_line_buf);
}

/* 在行缓冲里画一个字符，返回新的光标 x（像素） */
static int draw_char(int cx, uint8_t ch, uint16_t fg, uint16_t bg)
{
    if (ch < 0x20 || ch > 0x7F) ch = '?';

    const g_font14_glyph_t *g = &g_font14_glyph[(ch - 0x20) + 1];
    if (g->box_w == 0 || g->box_h == 0) {
        return cx + (g->adv_w / 16);
    }

    int gx = cx + g->ofs_x;
    int baseline = FONT14_LINE_HEIGHT - FONT14_BASE_LINE;   // 13
    int gy = baseline - g->ofs_y - g->box_h;

    int bytes_per_row = (g->box_w * FONT14_BPP + 7) / 8;

    for (int row = 0; row < g->box_h; row++) {
        int py = gy + row;
        if (py < 0 || py >= FONT14_LINE_HEIGHT) continue;

        for (int col = 0; col < g->box_w; col++) {
            int px = gx + col;
            if (px < 0 || px >= LCD_H_RES) continue;

            int byte_idx = g->bitmap_index + row * bytes_per_row + col / 2;
            uint8_t byte = g_font14_bitmap[byte_idx];
            uint8_t nib = (col & 1) ? (byte & 0x0F) : (byte >> 4);
            if (nib == 0) continue;

            uint8_t a = (uint8_t)(nib * 17);   // 0..15 -> 0..255
            uint16_t *dst = &s_line_buf[py * LCD_H_RES + px];
            *dst = blend565(bg, fg, a);
        }
    }

    return cx + (g->adv_w / 16);
}

/* 画一整行：bg 铺满，文本从 x=0 开始 */
static void draw_line(int y, const char *text, uint16_t fg, uint16_t bg)
{
    for (int i = 0; i < LCD_H_RES * FONT14_LINE_HEIGHT; i++) {
        s_line_buf[i] = bg;
    }

    int cx = 0;
    for (const char *p = text; *p && cx < LCD_H_RES; p++) {
        cx = draw_char(cx, (uint8_t)*p, fg, bg);
    }

    flush_line(y, bg);
}

/* 一条 1px 高分隔线 */
static void draw_separator(int y)
{
    for (int i = 0; i < LCD_H_RES; i++) {
        s_line_buf[i] = COL_LINE;
    }
    esp_lcd_panel_draw_bitmap(s_panel, 0, y, LCD_H_RES, y + 1, s_line_buf);
}

/* ------------------------------------------------------------ 状态采集 --- */

static const char *dap_port_name(uint8_t port)
{
    switch (port) {
        case DAP_PORT_SWD:  return "SWD";
        case DAP_PORT_JTAG: return "JTAG";
        default:            return "OFF";
    }
}

static const char *usbip_state_name(void)
{
    switch (kState) {
        case ACCEPTING:    return "listening";
        case ATTACHING:    return "handshake";
        case EMULATING:    return "emulating";
        case EL_DATA_PHASE:return "elaphureLink";
        default:           return "?";
    }
}

static void format_uptime(char *buf, size_t n)
{
    int64_t s = esp_timer_get_time() / 1000000;
    int h = (int)(s / 3600), m = (int)((s % 3600) / 60), sec = (int)(s % 60);
    snprintf(buf, n, "%02d:%02d:%02d", h, m, sec);
}

/* ---------------------------------------------------------------- 任务 --- */

static void display_task(void *arg)
{
    char line[64];
    int y;
    int tick = 0;

    // 清屏
    ESP_LOGI(TAG, "task start, clearing screen");
    for (int i = 0; i < LCD_H_RES * FONT14_LINE_HEIGHT; i++) s_line_buf[i] = COL_BG;
    for (y = 0; y < LCD_V_RES; y += FONT14_LINE_HEIGHT) {
        flush_line(y, COL_BG);
    }
    ESP_LOGI(TAG, "screen cleared, first draw");
    draw_line(0, "Wireless CMSIS-DAP", COL_TITLE, COL_BG);
    ESP_LOGI(TAG, "first line drawn, entering loop");

    for (;;) {
        y = 0;
        draw_line(y, "Wireless CMSIS-DAP", COL_TITLE, COL_BG); y += FONT14_LINE_HEIGHT;
        draw_separator(y); y += 2;

        /* ---- Wi-Fi ---- */
        wifi_ap_record_t ap;
        memset(&ap, 0, sizeof(ap));
        bool wifi_ok = (esp_wifi_sta_get_ap_info(&ap) == ESP_OK);

        esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
        esp_netif_ip_info_t ip;
        memset(&ip, 0, sizeof(ip));
        bool ip_ok = (netif && esp_netif_get_ip_info(netif, &ip) == ESP_OK && ip.ip.addr != 0);

        snprintf(line, sizeof(line), "WiFi %-18.18s %s",
                 wifi_ok ? (const char *)ap.ssid : "(not connected)",
                 wifi_ok ? "OK" : "--");
        draw_line(y, line, wifi_ok ? COL_OK : COL_ERR, COL_BG); y += FONT14_LINE_HEIGHT;

        snprintf(line, sizeof(line), "ch %-3d %-4d dBm  %s",
                 wifi_ok ? ap.primary : 0,
                 wifi_ok ? ap.rssi : 0,
                 wifi_ok ? "connected" : "disconnected");
        draw_line(y, line, COL_VALUE, COL_BG); y += FONT14_LINE_HEIGHT;

        snprintf(line, sizeof(line), "IP   %d.%d.%d.%d  :3240",
                 (int)(ip.ip.addr & 0xFF), (int)((ip.ip.addr >> 8) & 0xFF),
                 (int)((ip.ip.addr >> 16) & 0xFF), (int)((ip.ip.addr >> 24) & 0xFF));
        draw_line(y, line, ip_ok ? COL_VALUE : COL_WARN, COL_BG); y += FONT14_LINE_HEIGHT;

        draw_separator(y); y += 2;

        /* ---- USBIP ---- */
        snprintf(line, sizeof(line), "USBIP %-14s %s",
                 usbip_state_name(),
                 kSock >= 0 ? "client up" : "no client");
        draw_line(y, line, (kSock >= 0) ? COL_OK : COL_LABEL, COL_BG); y += FONT14_LINE_HEIGHT;

        snprintf(line, sizeof(line), "Socket %-3d  DAP %-4s  %s",
                 kSock, dap_port_name(DAP_Data.debug_port),
                 DAP_Data.fast_clock ? "fast" : "norm");
        draw_line(y, line, COL_VALUE, COL_BG); y += FONT14_LINE_HEIGHT;

        draw_separator(y); y += 2;

        /* ---- 系统 ---- */
        char up[16];
        format_uptime(up, sizeof(up));
        snprintf(line, sizeof(line), "Uptime %s", up);
        draw_line(y, line, COL_LABEL, COL_BG); y += FONT14_LINE_HEIGHT;

        snprintf(line, sizeof(line), "Heap %u KB  min %u KB",
                 (unsigned)(esp_get_free_heap_size() / 1024),
                 (unsigned)(esp_get_minimum_free_heap_size() / 1024));
        draw_line(y, line, COL_LABEL, COL_BG); y += FONT14_LINE_HEIGHT;

        // 每 10 秒打一条心跳，方便从串口确认显示任务活着
        if (++tick % 10 == 0) {
            ESP_LOGI(TAG, "refresh #%d  wifi=%d  sock=%d  dap=%s",
                     tick, (int)wifi_ok, kSock, dap_port_name(DAP_Data.debug_port));
        }

        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

/* ---------------------------------------------------------------- 初始化 --- */

esp_err_t display_init(void)
{
    ESP_LOGI(TAG, "init LCD (ILI9341, SPI3, %dx%d)", LCD_H_RES, LCD_V_RES);

    /* 1) SPI 总线 */
    spi_bus_config_t buscfg = {
        .sclk_io_num     = LCD_PIN_SCLK,
        .mosi_io_num     = LCD_PIN_MOSI,
        .miso_io_num     = -1,
        .quadwp_io_num   = -1,
        .quadhd_io_num   = -1,
        .max_transfer_sz = LCD_H_RES * FONT14_LINE_HEIGHT * sizeof(uint16_t) * 2,
    };
    ESP_RETURN_ON_ERROR(spi_bus_initialize(LCD_SPI_HOST, &buscfg, SPI_DMA_CH_AUTO),
                        TAG, "spi bus init failed");

    /* 2) panel IO */
    esp_lcd_panel_io_spi_config_t io_config = {
        .dc_gpio_num       = LCD_PIN_DC,
        .cs_gpio_num       = LCD_PIN_CS,
        .pclk_hz           = LCD_PIXEL_CLOCK_HZ,
        .lcd_cmd_bits      = 8,
        .lcd_param_bits    = 8,
        .spi_mode          = 0,
        .trans_queue_depth = 1,   // 1 = 同步阻塞发送，最简单可靠
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_SPI_HOST,
                                                 &io_config, &s_io),
                        TAG, "panel io init failed");

    /* 3) ILI9341 面板 */
    esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = LCD_PIN_RST,
        .rgb_ele_order  = LCD_RGB_ELEMENT_ORDER_BGR,
        .bits_per_pixel = 16,
        .flags.reset_active_high = 1,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_ili9341(s_io, &panel_config, &s_panel),
                        TAG, "panel init failed");

    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(s_panel), TAG, "panel reset failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(s_panel),  TAG, "panel init failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_mirror(s_panel, true, true), TAG, "panel mirror failed");

    esp_lcd_panel_disp_on_off(s_panel, true);

    /* 4) 背光（LEDC，5kHz，全亮） */
    ledc_timer_config_t bl_timer = {
        .speed_mode      = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_10_BIT,
        .timer_num       = LEDC_TIMER_0,
        .freq_hz         = 5000,
        .clk_cfg         = LEDC_AUTO_CLK,
    };
    ESP_RETURN_ON_ERROR(ledc_timer_config(&bl_timer), TAG, "backlight timer failed");

    ledc_channel_config_t bl_ch = {
        .gpio_num   = LCD_PIN_BL,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel    = LEDC_CHANNEL_0,
        .intr_type  = LEDC_INTR_DISABLE,
        .timer_sel  = LEDC_TIMER_0,
        .duty       = 1023,
        .hpoint     = 0,
    };
    ESP_RETURN_ON_ERROR(ledc_channel_config(&bl_ch), TAG, "backlight channel failed");

    xTaskCreate(display_task, "display", 4096, NULL, 3, NULL);

    ESP_LOGI(TAG, "LCD ready");
    return ESP_OK;
}
