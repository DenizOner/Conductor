/**
 * @file telemetry.h
 * @brief Background hardware line voltage monitoring, FreeRTOS stack watermarking,
 *        and stateful request/response telemetry polling with timeout and freshness tracking.
 */

#ifndef TELEMETRY_H
#define TELEMETRY_H

#include <stdbool.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Telemetry query identifiers for stateful lifecycle tracking.
 */
typedef enum {
    TELEMETRY_QUERY_TEMP = 0,    /**< 0x25: 3-channel temperature */
    TELEMETRY_QUERY_INPUT_VOL,   /**< 0x2F: Audio input signal level */
    TELEMETRY_QUERY_IDLE,        /**< 0x31: Idle timer duration */
    TELEMETRY_QUERY_STATUS,      /**< 0x34: 20-byte amplifier configuration frame */
    TELEMETRY_QUERY_COUNT
} telemetry_query_id_t;

/**
 * @brief Diagnostic telemetry health and freshness record.
 */
typedef struct {
    uint32_t request_count;
    uint32_t success_count;
    uint32_t timeout_count;
    uint32_t consecutive_failures;
    int64_t  last_request_time_us;
    int64_t  last_success_time_us;
} telemetry_stat_t;

/**
 * @brief Performs one telemetry evaluation cycle (cable check and sequential telemetry polling).
 */
void telemetry_process_cycle(void);

/**
 * @brief Dispatches an explicit telemetry query byte to the amplifier if the UART bus is idle.
 * Routes through command verifier mutex to avoid bus collision.
 *
 * @param query_opcode Opcode byte (0x25, 0x2F, 0x31, 0x34).
 * @return true if dispatched, false if bus was busy or unavailable.
 */
bool telemetry_dispatch_query(uint8_t query_opcode);

/**
 * @brief Notifies the telemetry manager that a valid response was received and decoded from UART.
 * Updates freshness timestamp and resets consecutive failure counters.
 *
 * @param query_id Query identifier.
 */
void telemetry_notify_response_received(telemetry_query_id_t query_id);

/**
 * @brief Retrieves statistical tracking information for a telemetry query.
 *
 * @param query_id Query identifier.
 * @param out_stat Pointer to destination telemetry_stat_t buffer.
 * @return true on success, false if query_id is invalid.
 */
bool telemetry_get_stats(telemetry_query_id_t query_id, telemetry_stat_t *out_stat);

/**
 * @brief Logs FreeRTOS task high watermarks to detect potential stack overflows.
 */
void telemetry_audit_stack_usage(TaskHandle_t task, const char *task_name);

/**
 * @brief Spawns the static telemetry task and waits for it to signal STARTED.
 *
 * Preconditions: UART and ADC hardware must be initialized before calling.
 * The static task TCB and stack live inside telemetry.c (single ownership).
 * Uses an internal StaticSemaphore_t handshake: returns only after the task
 * has signalled that it entered its main loop (STARTED state confirmed).
 *
 * @return true if task started, false if creation failed or timeout elapsed.
 */
bool telemetry_start(void);

#ifdef __cplusplus
}
#endif

#endif /* TELEMETRY_H */
