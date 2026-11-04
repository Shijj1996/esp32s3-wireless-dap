/**
 * @file timer.c
 * @brief Timestamp source for CMSIS-DAP
 *
 * ESP8266  : FRC2 free running counter (5 MHz)
 * ESP32/S3 : esp_timer, 1 us resolution (TIMESTAMP_CLOCK == 1000000)
 *
 * @version 0.3
 * @date 2020-01-22
 *
 * @copyright Copyright (c) 2021
 *
 */
#include <stdint.h>
#include <stdbool.h>

#include "main/timer.h"

#ifdef CONFIG_IDF_TARGET_ESP8266
    #include "hw_timer.h"
#else
    #include "esp_timer.h"
#endif

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/*
    Note:
        System bus frequency is 80MHz, will not be affected by CPU frequency. The
        frequency of UART, SPI, or other peripheral devices, are divided from system
        bus frequency, so they will not be affected by CPU frequency either.
*/
#ifdef CONFIG_IDF_TARGET_ESP8266
#define TIMER_BASE 0x60000600
volatile frc2_struct_t * frc2  = (frc2_struct_t *)(TIMER_BASE + (1) * 0x20);
#endif

void timer_init()
{
#ifdef CONFIG_IDF_TARGET_ESP8266
    vPortEnterCritical();
    frc2->ctrl.div = TIMER_CLKDIV_16;  // 80MHz / 16 = 5MHz
    frc2->ctrl.intr_type = TIMER_EDGE_INT;
    frc2->ctrl.reload = 0x01;          // enable auto reload
    frc2->load.val = 0x7FFFFFFF;      // 31bit max
    frc2->ctrl.en = 0x01;
    vPortExitCritical();
#else
    /* esp_timer is initialised by the startup code; nothing to do here. */
#endif
}

// ESP8266: 5 MHz counter, wraps every 429 s
// ESP32/S3: 1 MHz (esp_timer) → 0.2 us resolution, wraps every ~71 min
uint32_t get_timer_count()
{
#ifdef CONFIG_IDF_TARGET_ESP8266
    return (uint32_t)frc2->count.data;
#else
    return (uint32_t)(esp_timer_get_time() & 0xFFFFFFFFULL);
#endif
}
