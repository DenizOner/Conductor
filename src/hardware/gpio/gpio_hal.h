/**
 * @file gpio_hal.h
 * @brief GPIO Hardware Abstraction Layer for Z906 pin setup and safe power-up state.
 */

#ifndef GPIO_HAL_H
#define GPIO_HAL_H

#include <stdbool.h>
#include "driver/gpio.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initializes board GPIOs and asserts safe idle states on serial lines.
 * @return true on success.
 */
bool z906_gpio_hal_init(void);

/**
 * @brief Sets a GPIO pin output level.
 */
bool z906_gpio_hal_set_level(gpio_num_t pin, uint32_t level);

/**
 * @brief Reads a digital GPIO pin level.
 */
int z906_gpio_hal_get_level(gpio_num_t pin);

#ifdef __cplusplus
}
#endif

#endif /* GPIO_HAL_H */
