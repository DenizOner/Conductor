/**
 * @file z906_transport.c
 * @brief Z906 UART-to-EventBus transport adapter implementation.
 *
 * Owns the UART RX task and its private startup handshake.
 *
 * ## Startup Handshake (FSR-01, FSR-02 fix)
 *
 * z906_transport_start() creates the static task and BLOCKS on a private
 * StaticSemaphore_t. The task calls xSemaphoreGive() immediately after
 * wdt_manager_register_task() and before entering its infinite loop.
 * This guarantees z906_transport_start() returns only after the task
 * is genuinely running — not just created.
 *
 * No EventGroupHandle_t is exposed in the public API.
 *
 * ## Layer Contract
 *
 *   hardware/uart → this module → core/event_bus
 *
 * No FSM logic, no Zigbee knowledge, no protocol decoding here.
 */

#include "z906_transport.h"

#include "uart_hal.h"
#include "event_bus.h"
#include "wdt_manager.h"
#include "z906_state.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "Z906_TRANSPORT";

/* -------------------------------------------------------------------------
 * Task configuration constants
 * ------------------------------------------------------------------------- */

/** @brief Stack depth in words for the UART RX byte-reader task */
#define UART_RX_TASK_STACK_WORDS  (4096u / sizeof(StackType_t))

/** @brief FreeRTOS priority for the UART RX task (highest in system) */
#define UART_RX_TASK_PRIORITY     5u

/**
 * @brief Maximum time (ms) to wait for the task to signal STARTED.
 *
 * The task should signal within one scheduler tick. 500 ms is a generous
 * upper bound that still provides a deterministic failure path.
 */
#define TRANSPORT_STARTUP_TIMEOUT_MS  500u

/* -------------------------------------------------------------------------
 * Static allocation — task TCB, stack, startup semaphore
 * ------------------------------------------------------------------------- */

static StackType_t  s_rx_task_stack[UART_RX_TASK_STACK_WORDS];
static StaticTask_t s_rx_task_tcb;

/** @brief Binary semaphore signalled by the task after it enters its main loop */
static SemaphoreHandle_t s_started_sem    = NULL;
static StaticSemaphore_t s_started_sem_buf;

/* -------------------------------------------------------------------------
 * Task body
 * ------------------------------------------------------------------------- */

/**
 * @brief UART RX task — drains Console and Amplifier FIFOs into the EventBus.
 *
 * No EventGroup gate needed: z906_transport_start() blocks on s_started_sem
 * until THIS task signals it, so by the time the caller returns from start(),
 * all dependencies (EventBus, FSM) are already fully initialized.
 *
 * @param pvParameters Unused.
 */
static void prv_uart_rx_task(void *pvParameters)
{
    (void)pvParameters; /* No gate handle — startup is handled by the semaphore */

    /* Register with Task Watchdog before signalling STARTED */
    wdt_manager_register_task();

    ESP_LOGI(TAG, "uart_rx_task running (Priority %u)", UART_RX_TASK_PRIORITY);

    /* Signal z906_transport_start() that the task is alive and running.
     * This completes the private startup handshake contract. */
    xSemaphoreGive(s_started_sem);

    uint8_t rx_byte = 0;
    uint32_t last_cable_epoch = z906_state_get_cable_epoch();
    uint32_t amp_stream_epoch = 0;

    while (1) {
        wdt_manager_reset();

        /* Drain one byte from the Console channel and post to EventBus */
        if (z906_uart_hal_read_console(&rx_byte, 1, pdMS_TO_TICKS(5)) > 0) {
            z906_event_t evt = {
                .type = EVENT_CONSOLE_RX_BYTE,
                .data = { .raw_byte = rx_byte }
            };
            event_bus_post(&evt, 0);
        }

        /* A cable transition invalidates the hardware RX ring even if the
         * cable notification was dropped from the (bounded) EventBus.
         * Flush only at the boundary: normal console passthrough is unchanged. */
        uint32_t read_epoch = z906_state_get_cable_epoch();
        if (read_epoch != last_cable_epoch) {
            z906_uart_hal_flush_amp_rx();
            last_cable_epoch = read_epoch;
            ++amp_stream_epoch;
        }
        /* Stamp the epoch BEFORE a potentially blocking UART read. If cable
         * changes mid-read, FSM will reject state from this old generation. */
        if (z906_uart_hal_read_amp(&rx_byte, 1, pdMS_TO_TICKS(5)) > 0) {
            z906_event_t evt = {
                .type = EVENT_AMP_RX_BYTE,
                .data = { .raw_byte = rx_byte },
                .received_at_us = esp_timer_get_time(),
                .cable_epoch = read_epoch,
                .rx_stream_epoch = amp_stream_epoch
            };
            if (!event_bus_post(&evt, 0)) {
                /* Dropped bytes cannot silently continue an in-flight frame. */
                ++amp_stream_epoch;
            }
        }

        vTaskDelay(pdMS_TO_TICKS(5));
    }
}

/* -------------------------------------------------------------------------
 * Public API
 * ------------------------------------------------------------------------- */

esp_err_t z906_transport_start(void)
{
    /* Create the binary startup semaphore — starts empty (task must give it) */
    s_started_sem = xSemaphoreCreateBinaryStatic(&s_started_sem_buf);
    if (s_started_sem == NULL) {
        /* xSemaphoreCreateBinaryStatic panics internally; this is defensive */
        ESP_LOGE(TAG, "Failed to create startup semaphore");
        return ESP_ERR_NO_MEM;
    }

    /* Create the static UART RX task */
    TaskHandle_t handle = xTaskCreateStatic(
        prv_uart_rx_task,
        "uart_rx_task",
        UART_RX_TASK_STACK_WORDS,
        NULL,                  /* pvParameters: none needed */
        UART_RX_TASK_PRIORITY,
        s_rx_task_stack,
        &s_rx_task_tcb
    );

    if (handle == NULL) {
        ESP_LOGE(TAG, "Failed to create uart_rx_task");
        return ESP_ERR_NO_MEM;
    }

    /* Block until task signals STARTED or timeout — private handshake */
    BaseType_t got_sem = xSemaphoreTake(s_started_sem,
                                        pdMS_TO_TICKS(TRANSPORT_STARTUP_TIMEOUT_MS));
    if (got_sem != pdTRUE) {
        /* Task was created but did not signal within the timeout window.
         * The system initializer will treat this as a fatal start failure. */
        ESP_LOGE(TAG, "uart_rx_task did not signal STARTED within %u ms",
                 TRANSPORT_STARTUP_TIMEOUT_MS);
        return ESP_ERR_TIMEOUT;
    }

    ESP_LOGI(TAG, "uart_rx_task STARTED (Priority %u, stack %u words)",
             UART_RX_TASK_PRIORITY, UART_RX_TASK_STACK_WORDS);
    return ESP_OK;
}
