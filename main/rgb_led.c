/*
 * 板载三色状态灯（WS2812）
 *
 * 这块 DevKitC-1 没有屏幕，灯就是唯一的"现场指示器"：
 * 不接串口也能一眼看出盒子卡在哪一步。
 *
 * 状态映射（详见 docs/状态灯说明.md）：
 *   红闪 1Hz     未连上 Wi-Fi
 *   蓝呼吸       已连 Wi-Fi，等 PC 挂载
 *   绿暗         已挂载，空闲
 *   绿亮         正在传数据
 */

#include <stdbool.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_check.h"
#include "esp_wifi.h"

#include "led_strip.h"

#include "rgb_led.h"
#include "dap_configuration.h"
#include "usbip_server.h"

static const char *TAG = "rgb_led";

#define RGB_TICK_MS         50      // 刷新周期
#define RGB_BLINK_TICKS     10      // 500ms 亮 / 500ms 灭
#define RGB_BREATH_TICKS    40      // 2s 一个呼吸周期
#define RGB_ACTIVE_BREATH   24      // 1.2s 一个呼吸周期（传数据时更快）
#define RGB_ACTIVE_WINDOW   6       // 300ms 内有数据就算"正在传"

static led_strip_handle_t s_strip = NULL;

/* 三角波：period 个 tick 走完一个来回，峰值 max */
static uint8_t breath(uint32_t tick, uint32_t period, uint32_t max)
{
    uint32_t half  = period / 2;
    uint32_t phase = tick % period;
    uint32_t v     = (phase < half) ? phase : (period - phase);
    return (uint8_t)(v * max / half);
}

void rgb_led_set_rgb(uint8_t r, uint8_t g, uint8_t b)
{
    if (!s_strip) return;
    led_strip_set_pixel(s_strip, 0, r, g, b);
    led_strip_refresh(s_strip);
}

/* 开机自检：红 -> 绿 -> 蓝 -> 灭，每步 250ms */
static void self_test(void)
{
    const uint8_t steps[][3] = {
        { 100, 0, 0 },      // 红
        { 0, 100, 0 },      // 绿
        { 0, 0, 100 },      // 蓝
        { 0, 0, 0 },        // 灭
    };
    for (int i = 0; i < (int)(sizeof(steps) / sizeof(steps[0])); i++) {
        rgb_led_set_rgb(steps[i][0], steps[i][1], steps[i][2]);
        vTaskDelay(pdMS_TO_TICKS(250));
    }
}

static void rgb_led_task(void *arg)
{
    uint32_t tick = 0;
    uint32_t last_cmd_count = g_usbip_cmd_count;
    uint32_t last_active_tick = 0;
    int last_state = -1;

    for (;;) {
        wifi_ap_record_t ap;
        bool wifi_ok = (esp_wifi_sta_get_ap_info(&ap) == ESP_OK);

        uint8_t r = 0, g = 0, b = 0;

        /* 状态变化时打一条日志：不用看灯也能从串口确认状态机对不对 */
        int state = (!wifi_ok) ? 0 : (kSock < 0 ? 1 : 2);
        if (state != last_state) {
            static const char *names[] = { "NO_WIFI(red blink)", "WAITING(blue breath)", "ATTACHED(green idle / cyan active)" };
            ESP_LOGI(TAG, "state -> %s  (kSock=%d)", names[state], kSock);
            last_state = state;
        }

        if (!wifi_ok) {
            /* 红闪 —— 找不到热点 / 没连上 */
            bool on = ((tick / RGB_BLINK_TICKS) & 1U) == 0U;
            r = on ? 120 : 0;
        } else if (kSock < 0) {
            /* 蓝呼吸 —— 已连热点，等 PC 来挂载 */
            b = breath(tick, RGB_BREATH_TICKS, 95);
        } else {
            /* 已挂载：空闲绿呼吸；有数据流动时蓝绿呼吸（更快） */
            uint32_t now = g_usbip_cmd_count;
            if (now != last_cmd_count) {
                last_cmd_count  = now;
                last_active_tick = tick;
            }
            bool hot = (tick - last_active_tick) < RGB_ACTIVE_WINDOW;

            if (hot) {
                uint8_t v = breath(tick, RGB_ACTIVE_BREATH, 150);
                g = v;
                b = v;                  /* 绿 + 蓝 = 蓝绿（青） */
            } else {
                g = breath(tick, RGB_BREATH_TICKS, 110);
            }
        }

        rgb_led_set_rgb(r, g, b);

        tick++;
        vTaskDelay(pdMS_TO_TICKS(RGB_TICK_MS));
    }
}

esp_err_t rgb_led_init(void)
{
    ESP_LOGI(TAG, "init WS2812 on GPIO%d", BOARD_RGB_LED_GPIO);

    led_strip_config_t strip_config = {
        .strip_gpio_num = BOARD_RGB_LED_GPIO,
        .max_leds       = 1,
    };
    led_strip_rmt_config_t rmt_config = {
        .resolution_hz = 10 * 1000 * 1000,   // 10 MHz -> 0.1us 分辨率，WS2812 够用
    };

    ESP_RETURN_ON_ERROR(led_strip_new_rmt_device(&strip_config, &rmt_config, &s_strip),
                        TAG, "create led strip failed");

    led_strip_clear(s_strip);

    self_test();

    xTaskCreate(rgb_led_task, "rgb_led", 3072, NULL, 2, NULL);

    ESP_LOGI(TAG, "status LED ready");
    return ESP_OK;
}
