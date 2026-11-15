/**
 * @file led_driver.h
 * @brief Hardware driver interface for onboard WS2812 addressable RGB LED on ESP32-C6.
 *
 * Implements non-blocking, zero runtime allocation LED control.
 */

#ifndef LED_DRIVER_H
#define LED_DRIVER_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 24-bit RGB color representation.
 */
typedef struct {
    uint8_t r; /**< Red component (0 - 255) */
    uint8_t g; /**< Green component (0 - 255) */
    uint8_t b; /**< Blue component (0 - 255) */
} led_color_t;

/**
 * @brief Common predefined colors.
 */
#define LED_COLOR_BLACK   ((led_color_t){0, 0, 0})
#define LED_COLOR_WHITE   ((led_color_t){255, 255, 255})
#define LED_COLOR_RED     ((led_color_t){255, 0, 0})
#define LED_COLOR_GREEN   ((led_color_t){0, 255, 0})
#define LED_COLOR_BLUE    ((led_color_t){0, 0, 255})
#define LED_COLOR_YELLOW  ((led_color_t){255, 255, 0})
#define LED_COLOR_CYAN    ((led_color_t){0, 255, 255})
#define LED_COLOR_MAGENTA ((led_color_t){255, 0, 255})

/**
 * @brief Initializes the hardware WS2812 RGB LED driver on GPIO 8.
 * @return true on success, false if driver initialization failed.
 */
bool led_driver_init(void);

/**
 * @brief Sets the physical RGB color and transmits frame to the LED.
 * @param color 24-bit RGB color structure.
 * @return true on success, false on transmit failure.
 */
bool led_driver_set_color(led_color_t color);

/**
 * @brief Turns off the LED immediately (sets color to black 0,0,0).
 * @return true on success.
 */
bool led_driver_clear(void);

/**
 * @brief Checks if the hardware LED driver has been successfully initialized.
 * @return true if initialized and ready.
 */
bool led_driver_is_ready(void);

/**
 * @brief Retrieves the last commanded color.
 * @return Last commanded led_color_t.
 */
led_color_t led_driver_get_current_color(void);

#ifdef __cplusplus
}
#endif

#endif /* LED_DRIVER_H */
