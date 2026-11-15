/**
 * @file uart_hal.h
 * @brief Dual-channel UART driver for Z906 Console (UART1) and Amplifier (UART0).
 *
 * ## Responsibility Boundary
 *
 * This HAL layer owns ONLY low-level UART hardware configuration and I/O:
 *   - Driver initialization (57600 baud, 8O1)
 *   - Synchronous read/write primitives
 *   - FIFO flush
 *
 * It does NOT own any FreeRTOS tasks, EventBus integration, or protocol logic.
 * Those concerns belong to src/communication/z906_transport/.
 */

#ifndef UART_HAL_H
#define UART_HAL_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initializes both hardware UART controllers (57600 baud, 8O1).
 * @return true on success.
 */
bool z906_uart_hal_init(void);

/**
 * @brief Transmits a buffer to the physical Z906 Control Console.
 */
int z906_uart_hal_write_console(const uint8_t *data, size_t len);

/**
 * @brief Transmits a buffer to the physical Z906 Subwoofer Amplifier.
 */
int z906_uart_hal_write_amp(const uint8_t *data, size_t len);

/**
 * @brief Reads incoming bytes from the Console ring buffer.
 */
int z906_uart_hal_read_console(uint8_t *buffer, size_t max_len, TickType_t wait_ticks);

/**
 * @brief Reads incoming bytes from the Amplifier ring buffer.
 */
int z906_uart_hal_read_amp(uint8_t *buffer, size_t max_len, TickType_t wait_ticks);

/**
 * @brief Discards any pending data in the amplifier UART RX FIFO.
 */
void z906_uart_hal_flush_amp_rx(void);

#ifdef __cplusplus
}
#endif

#endif /* UART_HAL_H */
