/*
 * ESP32-S3-BOX-3 状态显示屏（2.4" 320x240，ILI9341，SPI3）
 *
 * 每秒把「Wi-Fi / USBIP / DAP / 系统」状态画到屏幕，现场不看串口也能知道盒子状态。
 *
 * 渲染要点：
 *   - 逐带绘制：一次组一条 16px（或缩放后 32px）高的横带，再推给面板，
 *     不用整屏 framebuffer（320x240x2 = 150KB 省下来）。
 *   - 字节序：面板按 RGB565 大端接收，ESP32 内存是小端。**必须**在推屏前交换高低字节。
 *     注意要用独立的输出缓冲，不能就地交换（复用缓冲会重复交换，画出来是条纹）。
 *   - 字库：PingFang 14px / 4bpp / LVGL 连续 nibble 打包（行间不补齐）。
 */

#include <string.h>
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

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

/* ------------------------------------------------------------------ 配色 --- */
/* RGB565（驱动里配置成 BGR 元素顺序，与官方 BSP 一致） */
#define rgb565(r, g, b) ((uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | (((b) & 0xF8) >> 3)))

#define COL_BG      rgb565(0x0B, 0x10, 0x17)   // 近黑背景
#define COL_HEAD    rgb565(0x12, 0x1C, 0x28)   // 标题栏
#define COL_ACCENT  rgb565(0x38, 0xC8, 0xE0)   // 青色强调
#define COL_TITLE   rgb565(0xCF, 0xF2, 0xFF)   // 标题文字
#define COL_LABEL   rgb565(0x7E, 0x90, 0xA4)   // 字段名
#define COL_VALUE   rgb565(0xE8, 0xF0, 0xF8)   // 数值
#define COL_OK      rgb565(0x4C, 0xE0, 0x8C)   // 绿
#define COL_WARN    rgb565(0xFF, 0xC2, 0x4B)   // 橙
#define COL_ERR     rgb565(0xFF, 0x6E, 0x6E)   // 红
#define COL_RULE    rgb565(0x1E, 0x2C, 0x3C)   // 分隔线

/* ------------------------------------------------------------------ 状态 --- */
static esp_lcd_panel_handle_t s_panel = NULL;
static esp_lcd_panel_io_handle_t s_io = NULL;

#define BAND_MAX_H 32                       // 支持 2 倍放大（所有横带都 <= 32 行）
static uint16_t s_band[LCD_H_RES * BAND_MAX_H];
static uint16_t s_out[LCD_H_RES * BAND_MAX_H];
static int s_band_h = 16;

/* 推屏完成信号：draw_bitmap 只是"排队"，必须等 DMA 真的读完缓冲，
 * 否则下一次组带会覆盖正在发送的数据，画面出现撕裂/重影。 */
static SemaphoreHandle_t s_flush_done = NULL;

static bool IRAM_ATTR on_color_trans_done(esp_lcd_panel_io_handle_t panel_io,
                                          esp_lcd_panel_io_event_data_t *edata,
                                          void *user_ctx)
{
    BaseType_t hp = pdFALSE;
    if (s_flush_done) xSemaphoreGiveFromISR(s_flush_done, &hp);
    return hp == pdTRUE;
}

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

static void band_begin(int h, uint16_t bg)
{
    if (h > BAND_MAX_H) h = BAND_MAX_H;
    s_band_h = h;
    for (int i = 0; i < LCD_H_RES * h; i++) s_band[i] = bg;
}

/* 在横带内画字符串，返回结束光标 x。scale=1 正常，2 为放大一倍 */
static int band_str(int x, int y0, int scale, const char *s, uint16_t fg)
{
    int cx = x;
    int baseline = (FONT14_LINE_HEIGHT - FONT14_BASE_LINE) * scale;

    for (; *s; s++) {
        uint8_t ch = (uint8_t)*s;
        if (ch < 0x20 || ch > 0x7F) ch = '?';

        const g_font14_glyph_t *g = &g_font14_glyph[(ch - 0x20) + 1];
        if (g->box_w && g->box_h) {
            int gx = cx + g->ofs_x * scale;
            int gy = baseline - g->ofs_y * scale - g->box_h * scale;

            for (int row = 0; row < (int)g->box_h; row++) {
                for (int col = 0; col < (int)g->box_w; col++) {
                    /* LVGL 4bpp：连续 nibble 流，行间不补齐 */
                    int nib_idx  = row * (int)g->box_w + col;
                    int byte_idx = g->bitmap_index + nib_idx / 2;
                    uint8_t byte = g_font14_bitmap[byte_idx];
                    uint8_t nib  = (nib_idx & 1) ? (byte & 0x0F) : (byte >> 4);
                    if (nib == 0) continue;

                    uint8_t a = (uint8_t)(nib * 17);
                    for (int dy = 0; dy < scale; dy++) {
                        for (int dx = 0; dx < scale; dx++) {
                            int px = gx + col * scale + dx;
                            int py = gy + row * scale + dy + y0;
                            if (px < 0 || px >= LCD_H_RES || py < 0 || py >= s_band_h) continue;
                            s_band[py * LCD_H_RES + px] = blend565(s_band[py * LCD_H_RES + px], fg, a);
                        }
                    }
                }
            }
        }
        cx += (g->adv_w * scale) / 16;
    }
    return cx;
}

/* 把当前横带推到屏幕（做字节序交换） */
static void band_flush(int y)
{
    int n = LCD_H_RES * s_band_h;
    for (int i = 0; i < n; i++) {
        uint16_t v = s_band[i];
        s_out[i] = (uint16_t)((v >> 8) | (v << 8));
    }
    esp_lcd_panel_draw_bitmap(s_panel, 0, y, LCD_H_RES, y + s_band_h, s_out);
    if (s_flush_done) xSemaphoreTake(s_flush_done, pdMS_TO_TICKS(200));
}

/* 一行两列：label 在 x=8，value 在 x=104，状态在 x=250 */
static void draw_row(int y, const char *label, const char *value, const char *status,
                     uint16_t vc, uint16_t sc)
{
    band_begin(16, COL_BG);
    band_str(8,   0, 1, label, COL_LABEL);
    if (value)  band_str(104, 0, 1, value, vc);
    if (status) band_str(248, 0, 1, status, sc);
    band_flush(y);
}

static void draw_rule(int y)
{
    band_begin(1, COL_RULE);
    band_flush(y);
}

static void draw_accent_line(int y, int h, uint16_t color)
{
    band_begin(h, color);
    band_flush(y);
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
        case ACCEPTING:     return "listening";
        case ATTACHING:     return "handshake";
        case EMULATING:     return "emulating";
        case EL_DATA_PHASE: return "elaphureLink";
        default:            return "?";
    }
}

/* ---------------------------------------------------------------- 任务 --- */

static void display_task(void *arg)
{
    char v1[48], v2[48], v3[48], v4[48], v5[48], v6[48];
    int tick = 0;

    /* 底部大状态条：先把整个屏幕刷成背景 */
    for (int y = 0; y < LCD_V_RES; y += BAND_MAX_H) {
        int h = (LCD_V_RES - y >= BAND_MAX_H) ? BAND_MAX_H : (LCD_V_RES - y);
        band_begin(h, COL_BG);
        band_flush(y);
    }

    for (;;) {
        /* ---------- 标题栏（32px，2 倍字号） ---------- */
        band_begin(32, COL_HEAD);
        band_str(8, 0, 2, "WIRELESS DAP", COL_TITLE);
        band_str(248, 8, 1, "BOX-3", COL_LABEL);
        band_flush(0);
        draw_accent_line(32, 2, COL_ACCENT);

        /* ---------- 采集 ---------- */
        wifi_ap_record_t ap;
        memset(&ap, 0, sizeof(ap));
        bool wifi_ok = (esp_wifi_sta_get_ap_info(&ap) == ESP_OK);

        esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
        esp_netif_ip_info_t ip;
        memset(&ip, 0, sizeof(ip));
        bool ip_ok = (netif && esp_netif_get_ip_info(netif, &ip) == ESP_OK && ip.ip.addr != 0);

        /* ---------- Wi-Fi ---------- */
        draw_row(36, "WiFi",
                 wifi_ok ? (const char *)ap.ssid : "(not connected)",
                 wifi_ok ? "OK" : "--",
                 wifi_ok ? COL_VALUE : COL_ERR,
                 wifi_ok ? COL_OK : COL_ERR);

        snprintf(v1, sizeof(v1), "ch %-2d  %d dBm", wifi_ok ? ap.primary : 0, wifi_ok ? ap.rssi : 0);
        draw_row(52, "Link", v1, wifi_ok ? "connected" : "down",
                 COL_VALUE, wifi_ok ? COL_OK : COL_ERR);

        snprintf(v2, sizeof(v2), "%d.%d.%d.%d",
                 (int)(ip.ip.addr & 0xFF), (int)((ip.ip.addr >> 8) & 0xFF),
                 (int)((ip.ip.addr >> 16) & 0xFF), (int)((ip.ip.addr >> 24) & 0xFF));
        draw_row(68, "IPv4", ip_ok ? v2 : "(none)", ip_ok ? ":3240" : "",
                 ip_ok ? COL_VALUE : COL_WARN, COL_LABEL);

        draw_rule(84);

        /* ---------- USBIP / DAP ---------- */
        draw_row(88, "USBIP", usbip_state_name(),
                 kSock >= 0 ? "client up" : "no client",
                 COL_VALUE, (kSock >= 0) ? COL_OK : COL_LABEL);

        draw_row(104, "DAP", dap_port_name(DAP_Data.debug_port),
                 DAP_Data.fast_clock ? "fast" : "norm",
                 DAP_Data.debug_port ? COL_OK : COL_LABEL, COL_LABEL);

        snprintf(v3, sizeof(v3), "%d", kSock);
        draw_row(120, "Socket", v3, NULL, COL_VALUE, COL_LABEL);

        draw_rule(136);

        /* ---------- 系统 ---------- */
        int64_t s = esp_timer_get_time() / 1000000;
        snprintf(v4, sizeof(v4), "%02d:%02d:%02d", (int)(s / 3600), (int)((s % 3600) / 60), (int)(s % 60));
        draw_row(140, "Uptime", v4, NULL, COL_VALUE, COL_LABEL);

        snprintf(v5, sizeof(v5), "%u / %u KB",
                 (unsigned)(esp_get_free_heap_size() / 1024),
                 (unsigned)(esp_get_minimum_free_heap_size() / 1024));
        draw_row(156, "Heap", v5, NULL, COL_VALUE, COL_LABEL);

        uint8_t mac[6] = {0};
        esp_wifi_get_mac(WIFI_IF_STA, mac);
        snprintf(v6, sizeof(v6), "%02X:%02X:%02X:%02X:%02X:%02X",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        draw_row(172, "MAC", v6, NULL, COL_LABEL, COL_LABEL);

        draw_row(188, "SDK", "ESP-IDF v5.5.4", NULL, COL_LABEL, COL_LABEL);

        /* ---------- 底部大状态条（32px，贴底） ---------- */
        uint16_t bar_col;
        const char *bar_txt;
        if (!wifi_ok)          { bar_col = COL_ERR;    bar_txt = "NO WIFI"; }
        else if (kSock >= 0)   { bar_col = COL_OK;     bar_txt = "DAP ACTIVE"; }
        else                   { bar_col = COL_ACCENT; bar_txt = "READY  :3240"; }

        band_begin(32, bar_col);
        band_str(16, 0, 2, bar_txt, COL_BG);       // 2 倍字号，深色字压在亮底上
        band_flush(208);

        if (++tick % 10 == 0) {
            ESP_LOGI(TAG, "refresh #%d wifi=%d sock=%d dap=%s heap=%uK",
                     tick, (int)wifi_ok, kSock, dap_port_name(DAP_Data.debug_port),
                     (unsigned)(esp_get_free_heap_size() / 1024));
        }

        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

/* ---------------------------------------------------------------- 初始化 --- */

esp_err_t display_init(void)
{
    ESP_LOGI(TAG, "init LCD (ILI9341, SPI3, %dx%d)", LCD_H_RES, LCD_V_RES);

    spi_bus_config_t buscfg = {
        .sclk_io_num     = LCD_PIN_SCLK,
        .mosi_io_num     = LCD_PIN_MOSI,
        .miso_io_num     = -1,
        .quadwp_io_num   = -1,
        .quadhd_io_num   = -1,
        .max_transfer_sz = LCD_H_RES * BAND_MAX_H * sizeof(uint16_t) * 2,
    };
    ESP_RETURN_ON_ERROR(spi_bus_initialize(LCD_SPI_HOST, &buscfg, SPI_DMA_CH_AUTO),
                        TAG, "spi bus init failed");

    esp_lcd_panel_io_spi_config_t io_config = {
        .dc_gpio_num       = LCD_PIN_DC,
        .cs_gpio_num       = LCD_PIN_CS,
        .pclk_hz           = LCD_PIXEL_CLOCK_HZ,
        .lcd_cmd_bits      = 8,
        .lcd_param_bits    = 8,
        .spi_mode          = 0,
        .trans_queue_depth = 1,
        .on_color_trans_done = on_color_trans_done,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_SPI_HOST,
                                                 &io_config, &s_io),
                        TAG, "panel io init failed");

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

    s_flush_done = xSemaphoreCreateBinary();
    ESP_RETURN_ON_FALSE(s_flush_done, ESP_ERR_NO_MEM, TAG, "flush semaphore failed");

    xTaskCreate(display_task, "display", 4096, NULL, 3, NULL);

    ESP_LOGI(TAG, "LCD ready");
    return ESP_OK;
}
