/*
 * signal_generator.c
 *
 * Generates test PWM pulses on GPIO output pins.
 *
 * ──────────────────────────────────────────────────────────────────────────
 * THIS FILE IS NOT PART OF THE pulse_decoder LIBRARY.
 *
 * In a real application pulses come from external hardware.
 * This file exists only to produce signals so the decoder has something
 * to measure in this example.  Do not model your application code on this.
 * ──────────────────────────────────────────────────────────────────────────
 */

#include <stdint.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "driver/gpio.h"
#include "esp_rom_sys.h"    /* esp_rom_delay_us */

#include "signal_generator.h"

typedef struct {
    uint8_t         gpio_num;
    const uint32_t *pulse_widths_us;
    uint8_t         total_signals;
    uint32_t        interval_ms;
} generator_config_t;

static void generator_task(void *args)
{
    generator_config_t *cfg = (generator_config_t *)args;

    /* Configure the GPIO as push-pull output */
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << cfg->gpio_num),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);
    gpio_set_level(cfg->gpio_num, 0);

    /* Warm up esp_rom_delay_us so first pulse is accurate */
    esp_rom_delay_us(1);

    uint8_t idx = 0;

    while (1) {
        uint32_t width_us = cfg->pulse_widths_us[idx];

        /* Generate one pulse */
        gpio_set_level(cfg->gpio_num, 0);
        esp_rom_delay_us(50);
        gpio_set_level(cfg->gpio_num, 1);
        esp_rom_delay_us(width_us);
        gpio_set_level(cfg->gpio_num, 0);
        esp_rom_delay_us(50);

        /* Advance to next signal, wrapping around */
        idx = (idx + 1) % cfg->total_signals;

        vTaskDelay(pdMS_TO_TICKS(cfg->interval_ms));
    }
}

void signalGeneratorStart(uint8_t         gpio_num,
                           const uint32_t *pulse_widths_us,
                           uint8_t         total_signals,
                           uint32_t        interval_ms)
{
    /* Intentionally leaked — this is a demo utility, not production code */
    generator_config_t *cfg = malloc(sizeof(generator_config_t));
    cfg->gpio_num        = gpio_num;
    cfg->pulse_widths_us = pulse_widths_us;
    cfg->total_signals   = total_signals;
    cfg->interval_ms     = interval_ms;

    xTaskCreate(generator_task,
                "sig_gen",
                2048,
                cfg,
                4,
                NULL);
}