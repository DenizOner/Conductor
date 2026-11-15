/**
 * @file led_driver.c
 * @brief Implementation of hardware WS2812 RGB LED driver using ESP-IDF RMT peripheral.
 *
 * Provides accurate 800kHz single-wire NRZ pulse timing (T0H/T0L and T1H/T1L),
 * color caching, and zero runtime allocation.
 */

#include "led_driver.h"
#include "config.h"
#include "esp_log.h"
#include "driver/rmt_tx.h"
#include "driver/rmt_encoder.h"

static const char *TAG = "LED_DRIVER";

/**
 * @brief WS2812 Timing constants at 10 MHz resolution (1 tick = 0.1 µs = 100 ns).
 * - Bit 0: T0H = 0.35 µs (3.5 ~ 4 ticks), T0L = 0.8 µs (8 ticks) -> Period ~ 1.2 µs
 * - Bit 1: T1H = 0.7 µs (7 ticks), T1L = 0.6 µs (6 ticks) -> Period ~ 1.3 µs
 * - Reset: > 50 µs low (enforced via wait_all_done or eot_level 0)
 */
#define WS2812_RESOLUTION_HZ    10000000 /* 10MHz, 1 tick = 100ns */
#define WS2812_T0H_TICKS        3        /* 300ns */
#define WS2812_T0L_TICKS        9        /* 900ns */
#define WS2812_T1H_TICKS        8        /* 800ns */
#define WS2812_T1L_TICKS        4        /* 400ns */

static bool s_is_initialized = false;
static led_color_t s_current_color = {0, 0, 0};

static rmt_channel_handle_t s_rmt_channel = NULL;
static rmt_encoder_handle_t s_bytes_encoder = NULL;

bool led_driver_init(void)
{
    if (s_is_initialized) {
        return true;
    }

    rmt_tx_channel_config_t tx_chan_config = {
        .gpio_num = Z906_PIN_WS2812_LED,
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = WS2812_RESOLUTION_HZ,
        .mem_block_symbols = 64,
        .trans_queue_depth = 4,
        .intr_priority = 0,
        .flags = {
            .invert_out = 0,
            .with_dma = 0,
            .allow_pd = 0,
            .init_level = 0,
        }
    };

    esp_err_t err = rmt_new_tx_channel(&tx_chan_config, &s_rmt_channel);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create RMT TX channel on GPIO %d (err: 0x%x)", Z906_PIN_WS2812_LED, err);
        return false;
    }

    /* Configure standard bytes encoder for WS2812 NRZ bit format (MSB first) */
    rmt_bytes_encoder_config_t bytes_encoder_config = {
        .bit0 = {
            .duration0 = WS2812_T0H_TICKS,
            .level0 = 1,
            .duration1 = WS2812_T0L_TICKS,
            .level1 = 0,
        },
        .bit1 = {
            .duration0 = WS2812_T1H_TICKS,
            .level0 = 1,
            .duration1 = WS2812_T1L_TICKS,
            .level1 = 0,
        },
        .flags = {
            .msb_first = 1,
        }
    };

    err = rmt_new_bytes_encoder(&bytes_encoder_config, &s_bytes_encoder);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create RMT bytes encoder (err: 0x%x)", err);
        rmt_del_channel(s_rmt_channel);
        s_rmt_channel = NULL;
        return false;
    }

    err = rmt_enable(s_rmt_channel);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to enable RMT channel (err: 0x%x)", err);
        rmt_del_encoder(s_bytes_encoder);
        rmt_del_channel(s_rmt_channel);
        s_bytes_encoder = NULL;
        s_rmt_channel = NULL;
        return false;
    }

    s_is_initialized = true;
    s_current_color = LED_COLOR_BLACK;

    /* Transmit initial black frame */
    led_driver_clear();

    ESP_LOGI(TAG, "WS2812 RGB LED RMT driver initialized successfully on GPIO %d (10MHz resolution)", Z906_PIN_WS2812_LED);
    return true;
}

bool led_driver_set_color(led_color_t color)
{
    if (!s_is_initialized) {
        if (!led_driver_init()) {
            return false;
        }
    }

    s_current_color = color;

    /* Standard WS2812 expects color data in GRB byte order: Green, Red, Blue */
    uint8_t grb_payload[3] = {
        color.g,
        color.r,
        color.b
    };

    rmt_transmit_config_t tx_config = {
        .loop_count = 0,
        .flags = {
            .eot_level = 0,
            .queue_nonblocking = 0,
        }
    };

    esp_err_t err = rmt_transmit(s_rmt_channel, s_bytes_encoder, grb_payload, sizeof(grb_payload), &tx_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to transmit RMT symbols for WS2812 (err: 0x%x)", err);
        return false;
    }

    /* Wait for transfer to complete (at 800kHz, 24 bits take ~30µs, 5ms timeout is plenty safe) */
    rmt_tx_wait_all_done(s_rmt_channel, 5);

    return true;
}

bool led_driver_clear(void)
{
    return led_driver_set_color(LED_COLOR_BLACK);
}

bool led_driver_is_ready(void)
{
    return s_is_initialized;
}

led_color_t led_driver_get_current_color(void)
{
    return s_current_color;
}
