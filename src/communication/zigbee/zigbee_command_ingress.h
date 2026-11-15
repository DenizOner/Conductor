/**
 * @file zigbee_command_ingress.h
 * @brief Non-blocking ZCL command ingress, bounded queuing, and verified hardware dispatch.
 *
 * Architecture and Design Guarantees (Phase 4 Corrective Hardening):
 *   - Intercepts incoming standard ZCL commands on EP1 (On/Off 0x0006, Level Control 0x0008).
 *   - Enforces exact ZCL Level Control semantics:
 *     - MoveToLevel: respects ExecuteIfOff (Options bit 0). If Off and ExecuteIfOff=0, drops with SUCCESS without mutating power.
 *     - MoveToLevelWithOnOff: atomic compound operation. If level > min, verifies Power ON first (if standby), then sets Volume. If level == min, exact ZCL behavior (sets volume, then turns Off if level 0).
 *     - Transition time: Z906 does not support smooth transitions; instantaneous step is executed.
 *     - Explicitly rejects continuous/relative/extended commands with EZB_ZCL_STATUS_UNSUP_CMD.
 *   - Rejects unverified commands, out-of-bounds parameters, and read-only attributes non-blockingly.
 *   - Strictly non-blocking inside Zigbee callbacks: zero UART I/O, zero mutex wait, zero dynamic memory allocation.
 *   - Guaranteed queue capacity: single-producer invariant ensures check_value_cb capacity check guarantees write_attr_cb enqueue.
 *   - Restores backing storage on queued writes to prevent unverified optimistic state persistence.
 *   - Dispatches queued commands via z906_cmd_verifier_execute() in a dedicated worker task.
 */

#ifndef ZIGBEE_COMMAND_INGRESS_H
#define ZIGBEE_COMMAND_INGRESS_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "z906_cmd_registry.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Bounded capacity of the static Zigbee command ingress queue.
 */
#define ZB_INGRESS_QUEUE_DEPTH 8

/**
 * @brief Ingress event classification.
 */
typedef enum {
    ZB_INGRESS_TYPE_HARDWARE_CMD = 0,    /**< Single verified Z906 hardware command */
    ZB_INGRESS_TYPE_COMPOUND_LEVEL_ONOFF,/**< Atomic compound MoveToLevelWithOnOff: Power + Volume */
    ZB_INGRESS_TYPE_FEATURE_LED,         /**< Application setting: Onboard LED enable/disable */
    ZB_INGRESS_TYPE_FEATURE_VERIFIER,    /**< Application setting: Command Verifier enable/disable */
} zb_ingress_event_type_t;

/**
 * @brief Queued ingress command event structure.
 *
 * Fully scalar; contains no SDK pointers or dynamic allocations.
 */
typedef struct {
    zb_ingress_event_type_t type;         /**< Event classification */
    z906_cmd_id_t           cmd_id;       /**< Primary command ID in z906_cmd_registry */
    int32_t                 param_val;    /**< Primary validated parameter value */
    int32_t                 secondary_val;/**< Secondary parameter for compound operations (e.g. volume) */
    uint8_t                 endpoint;     /**< Originating ZCL endpoint */
    uint16_t                cluster;      /**< Originating ZCL cluster ID */
} zb_ingress_event_t;

/**
 * @brief Initializes the Zigbee command ingress subsystem.
 *
 * Creates the static FreeRTOS queue, spawns the static bridge worker task,
 * registers raw frame callbacks for EP1 standard clusters, and registers
 * custom cluster callbacks for EP2, EP3, and EP4.
 *
 * MUST be called after zigbee_device_model_register() and before esp_zigbee_start().
 *
 * @return ESP_OK on success, ESP_FAIL otherwise.
 */
esp_err_t zigbee_command_ingress_init(void);

/**
 * @brief Checks if the ingress subsystem is initialized.
 *
 * @return true if initialized, false otherwise.
 */
bool zigbee_command_ingress_is_initialized(void);

/**
 * @brief Query remaining space in the static ingress queue.
 *
 * @return Number of free slots available in the queue.
 */
size_t zigbee_command_ingress_get_queue_free_spaces(void);

#ifdef __cplusplus
}
#endif

#endif /* ZIGBEE_COMMAND_INGRESS_H */
