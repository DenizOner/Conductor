/**
 * @file uart_hal.c
 * @brief Implementation of dual-channel hardware UART driver for Z906.
 *
 * ## Responsibility Boundary
 *
 * This file owns ONLY low-level UART hardware configuration and I/O.
 * No FreeRTOS tasks, no EventBus integration, no protocol logic.
 * The UART RX byte-forwarding task lives in src/communication/z906_transport/.
 */

#include "uart_hal.h"
#include "config.h"
#include "driver/uart.h"
#include "esp_log.h"

static const char *TAG = "UART_HAL";

bool z906_uart_hal_init(void)
{
    const uart_config_t uart_config = {
        .baud_rate  = Z906_UART_BAUD_RATE,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_ODD,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT
    };

    /* 1. Install Console UART driver (UART1) */
    if (!uart_is_driver_installed(Z906_CONSOLE_UART_PORT)) {
        if (uart_driver_install(Z906_CONSOLE_UART_PORT,
                                Z906_UART_RX_BUFFER_SIZE,
                                Z906_UART_TX_BUFFER_SIZE, 0, NULL, 0) != ESP_OK) {
            ESP_LOGE(TAG, "Failed to install Console UART driver");
            return false;
        }
    }
    uart_param_config(Z906_CONSOLE_UART_PORT, &uart_config);
    uart_set_pin(Z906_CONSOLE_UART_PORT,
                 Z906_PIN_CONSOLE_TX, Z906_PIN_CONSOLE_RX,
                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);

    /* 2. Install Amplifier UART driver (UART0) */
    if (!uart_is_driver_installed(Z906_AMP_UART_PORT)) {
        if (uart_driver_install(Z906_AMP_UART_PORT,
                                Z906_UART_RX_BUFFER_SIZE,
                                Z906_UART_TX_BUFFER_SIZE, 0, NULL, 0) != ESP_OK) {
            ESP_LOGE(TAG, "Failed to install Amplifier UART driver");
            return false;
        }
    }
    uart_param_config(Z906_AMP_UART_PORT, &uart_config);
    uart_set_pin(Z906_AMP_UART_PORT,
                 Z906_PIN_AMP_TX, Z906_PIN_AMP_RX,
                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);

    ESP_LOGI(TAG,
             "Dual UART initialized "
             "(Console: UART1 RX%d/TX%d, Amp: UART0 RX%d/TX%d, 57600 8O1)",
             Z906_PIN_CONSOLE_RX, Z906_PIN_CONSOLE_TX,
             Z906_PIN_AMP_RX,     Z906_PIN_AMP_TX);
    return true;
}

int z906_uart_hal_write_console(const uint8_t *data, size_t len)
{
    if (data == NULL || len == 0) {
        return 0;
    }
    return uart_write_bytes(Z906_CONSOLE_UART_PORT, (const char *)data, len);
}

int z906_uart_hal_write_amp(const uint8_t *data, size_t len)
{
    if (data == NULL || len == 0) {
        return 0;
    }
    return uart_write_bytes(Z906_AMP_UART_PORT, (const char *)data, len);
}

int z906_uart_hal_read_console(uint8_t *buffer, size_t max_len, TickType_t wait_ticks)
{
    if (buffer == NULL || max_len == 0) {
        return 0;
    }
    return uart_read_bytes(Z906_CONSOLE_UART_PORT, buffer, max_len, wait_ticks);
}

int z906_uart_hal_read_amp(uint8_t *buffer, size_t max_len, TickType_t wait_ticks)
{
    if (buffer == NULL || max_len == 0) {
        return 0;
    }
    return uart_read_bytes(Z906_AMP_UART_PORT, buffer, max_len, wait_ticks);
}

void z906_uart_hal_flush_amp_rx(void)
{
    uart_flush_input(Z906_AMP_UART_PORT);
}
