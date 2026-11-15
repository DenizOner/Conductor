/**
 * @file z906_transport.h
 * @brief Z906 UART-to-EventBus transport adapter.
 *
 * ## Layer Position
 *
 *   hardware/uart (UART HAL — pure I/O)
 *        ↓
 *   communication/z906_transport  (this module — adapter)
 *        ↓
 *   core/event_bus (EventBus — publish/subscribe)
 *        ↓
 *   core/state_machine (FSM — consumes events)
 *
 * ## Responsibility
 *
 * Owns the static UART RX task that drains both UART FIFOs and forwards
 * raw bytes to the EventBus as EVENT_CONSOLE_RX_BYTE / EVENT_AMP_RX_BYTE.
 * This is a pure data-forwarding role with no Z906 protocol knowledge.
 *
 * ## Start Contract
 *
 * z906_transport_start() returns ESP_OK ONLY after:
 *   1. The static UART RX task has been successfully created AND
 *   2. The task has signalled its internal readiness semaphore
 *      (confirming the FreeRTOS task is running and entering its loop).
 *
 * This eliminates the ambiguity between "task created" and "task started".
 */

#ifndef Z906_TRANSPORT_H
#define Z906_TRANSPORT_H

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Starts the static UART RX transport task.
 *
 * Preconditions:
 *   - z906_uart_hal_init() must have returned true.
 *   - event_bus_init() must have been called.
 *
 * Blocks (up to TRANSPORT_STARTUP_TIMEOUT_MS) until the spawned task
 * signals that it has entered its main loop, confirming STARTED state.
 *
 * @return ESP_OK if task started successfully.
 *         ESP_ERR_NO_MEM if task creation failed.
 *         ESP_ERR_TIMEOUT if task did not start within the timeout.
 */
esp_err_t z906_transport_start(void);

#ifdef __cplusplus
}
#endif

#endif /* Z906_TRANSPORT_H */
