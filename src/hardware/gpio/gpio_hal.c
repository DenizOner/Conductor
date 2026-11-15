/**
 * @file gpio_hal.c
 * @brief Implementation of GPIO hardware configuration and line stabilization.
 */

#include "gpio_hal.h"
#include "config.h"
#include "esp_log.h"

static const char *TAG = "GPIO_HAL";

bool z906_gpio_hal_init(void)
{
    /* Assert HIGH idle state on TX lines prior to UART driver attachment */
    gpio_config_t tx_conf = {
        .pin_bit_mask = (1ULL << Z906_PIN_CONSOLE_TX) | (1ULL << Z906_PIN_AMP_TX),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };

    if (gpio_config(&tx_conf) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure serial TX pins");
        return false;
    }

    gpio_set_level(Z906_PIN_CONSOLE_TX, 1);
    gpio_set_level(Z906_PIN_AMP_TX, 1);

    ESP_LOGI(TAG, "Serial TX pins stabilized to HIGH idle state");
    return true;
}

bool z906_gpio_hal_set_level(gpio_num_t pin, uint32_t level)
{
    return (gpio_set_level(pin, level) == ESP_OK);
}

int z906_gpio_hal_get_level(gpio_num_t pin)
{
    return gpio_get_level(pin);
}
