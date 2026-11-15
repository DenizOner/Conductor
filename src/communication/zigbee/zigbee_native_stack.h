/**
 * @file zigbee_native_stack.h
 * @brief Native ESP Zigbee SDK v2 stack lifecycle, commissioning, and network management.
 *
 * Responsibilities:
 *   - Dedicated NVS partition ("zb_storage") initialization and error handling.
 *   - Router role platform and device configuration (ESP_ZIGBEE_RADIO_MODE_NATIVE,
 *     EZB_NWK_DEVICE_TYPE_ROUTER, max_children=10, install_code_policy=false).
 *   - Core stack initialization (esp_zigbee_init) and version logging.
 *   - Application lifecycle signal handler registration via ezb_app_signal_add_handler.
 *   - BDB commissioning lifecycle:
 *       * SKIP_STARTUP -> ezb_bdb_start_top_level_commissioning(EZB_BDB_MODE_INITIALIZATION)
 *       * DEVICE_FIRST_START -> Factory-new idle, no auto-steering.
 *       * DEVICE_REBOOT -> Rejoin old network from zb_storage credentials.
 *       * STEERING -> Network steering result handling and publisher full sync.
 *       * LEAVE -> Distinguished local reset vs. network rejoin.
 *       * NETWORK_STATUS -> Non-destructive diagnostic tracking.
 *   - Asynchronous, thread-safe CLI command request dispatching via esp_zigbee_task_queue_post.
 *   - Authoritative RAM-only diagnostic snapshot of network metadata (PAN ID, Channel, Short Addr, Ext PAN).
 *   - Zero dynamic memory allocation and pure ESP Zigbee SDK v2 API usage.
 */

#ifndef ZIGBEE_NATIVE_STACK_H
#define ZIGBEE_NATIVE_STACK_H

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Discrete native Zigbee stack lifecycle states.
 */
typedef enum {
    ZB_STACK_STATE_UNINITIALIZED = 0, /**< Initial state before stack init */
    ZB_STACK_STATE_INITIALIZING,     /**< Partition and stack config in progress */
    ZB_STACK_STATE_READY,            /**< esp_zigbee_init succeeded, handlers registered */
    ZB_STACK_STATE_RUNNING,          /**< Mainloop active */
    ZB_STACK_STATE_FAILED,           /**< Initialization or runtime failure */
    ZB_STACK_STATE_STOPPED           /**< Cleanly stopped via esp_zigbee_stop */
} zb_stack_state_t;

/**
 * @brief Discrete native Zigbee network connection / commissioning states.
 */
typedef enum {
    ZB_NET_STATE_UNINITIALIZED = 0,   /**< Stack not yet started or initializing */
    ZB_NET_STATE_FACTORY_NEW_IDLE,    /**< Factory-new device; idle, awaiting CLI pair request */
    ZB_NET_STATE_STEERING,            /**< Active network steering / pairing window in progress */
    ZB_NET_STATE_JOINED,              /**< Successfully joined and authenticated with coordinator */
    ZB_NET_STATE_REJOINING,           /**< Attempting to rejoin previous network after reboot or link loss */
    ZB_NET_STATE_LEAVING_RESET,       /**< Local reset leave in progress */
    ZB_NET_STATE_COMMISSION_FAILED    /**< Last commissioning/rejoin attempt failed (idle/not ready) */
} zb_network_lifecycle_state_t;

/**
 * @brief Status return code for asynchronous CLI commissioning requests.
 */
typedef enum {
    ZB_REQ_ACCEPTED = 0,   /**< Request accepted and queued for execution on Zigbee task */
    ZB_REQ_BUSY,           /**< Another commissioning or reset operation is currently active */
    ZB_REQ_FAILED_TO_START /**< Failed to enqueue request onto Zigbee task queue */
} zb_request_status_t;

/**
 * @brief RAM-only network diagnostic telemetry snapshot.
 *
 * Guaranteed to NEVER log or expose network keys, TC link keys, or cryptographic material.
 */
typedef struct {
    zb_stack_state_t stack_state;
    zb_network_lifecycle_state_t net_state;
    bool factory_new;
    bool sdk_joined;
    bool network_ready;
    bool metadata_valid;
    uint16_t pan_id;
    uint8_t channel;
    uint16_t short_addr;
    uint8_t ext_pan_id[8];
    uint8_t last_bdb_status;
    uint8_t last_nwk_status;
    uint8_t rejoin_attempt_count;
} zb_network_diagnostics_t;

/**
 * @brief Initializes dedicated Zigbee NVS storage partition ("zb_storage").
 *
 * Enforces single ownership: does NOT erase credentials on failure.
 *
 * @return ESP_OK on success, error code otherwise.
 */
esp_err_t zigbee_native_storage_init(void);

/**
 * @brief Initializes the native ESP Zigbee SDK v2 stack as an IEEE 802.15.4 Router.
 *
 * Performs:
 *   1. zigbee_native_storage_init()
 *   2. esp_zigbee_init(&config) with dedicated storage partition and native radio.
 *   3. Registers lifecycle signal handler via ezb_app_signal_add_handler.
 *
 * @return ESP_OK on success, error code otherwise.
 */
esp_err_t zigbee_native_stack_init(void);

/**
 * @brief Starts the Zigbee stack with autostart=false and launches the blocking main loop.
 *
 * NOTE: This function executes esp_zigbee_launch_mainloop() which blocks until
 * esp_zigbee_stop() is called. It must be executed within its own dedicated FreeRTOS task.
 *
 * @return ESP_OK if mainloop exited cleanly, or error code on failure.
 */
esp_err_t zigbee_native_stack_run(void);

/**
 * @brief Spawns the static Zigbee task and blocks until the native stack signals STARTED.
 *
 * The static task TCB and stack live inside zigbee_native_stack.c (single ownership).
 * zigbee_native_stack_init() and zigbee_native_stack_run() are called from inside
 * the task body — required by ESP Zigbee SDK v2 architecture.
 *
 * Uses an internal StaticSemaphore_t handshake (FSR-04 fix):
 *   - Task create success DOES NOT equal start success.
 *   - Returns only after the task signals that zigbee_native_stack_init() succeeded.
 *
 * @return true  if init succeeded and mainloop was entered.
 *         false if task creation failed or zigbee_native_stack_init() returned an error.
 */
bool zigbee_native_stack_start(void);

/**
 * @brief Requests the native Zigbee stack to stop its mainloop.
 *
 * @return ESP_OK on acceptance, error code otherwise.
 */
esp_err_t zigbee_native_stack_stop(void);

/**
 * @brief Retrieves the current native stack lifecycle state.
 *
 * @return Current zb_stack_state_t.
 */
zb_stack_state_t zigbee_native_stack_get_state(void);

/**
 * @brief Converts a native stack lifecycle state to a human-readable string.
 *
 * @param state Lifecycle state enum value.
 * @return Static string representation.
 */
const char *zigbee_native_stack_state_to_str(zb_stack_state_t state);

/**
 * @brief Converts a network commissioning lifecycle state to a human-readable string.
 *
 * @param state Network state enum value.
 * @return Static string representation.
 */
const char *zigbee_native_net_state_to_str(zb_network_lifecycle_state_t state);

/**
 * @brief Converts a BDB commissioning status code to a human-readable string.
 *
 * @param status BDB status code.
 * @return Static string representation.
 */
const char *zigbee_native_bdb_status_to_str(uint8_t status);

/**
 * @brief Requests network pairing (network steering) from user CLI context.
 *
 * Thread-safe:
 * - If device is factory-new and unjoined, posts steering request to Zigbee task queue.
 * - If device has existing network history (joined or disconnected with datasets),
 *   executes deterministic re-pair (Strategy B: persists one-shot pair intent, suppresses
 *   reports, and invokes esp_zigbee_factory_reset() to restart cleanly into factory-new steering).
 *
 * @return ZB_REQ_ACCEPTED, ZB_REQ_BUSY, or ZB_REQ_FAILED_TO_START.
 */
zb_request_status_t zigbee_native_request_pair(void);

/**
 * @brief Requests Zigbee-only factory reset (`dev zb_reset`).
 *
 * Clears only the Zigbee network datasets in zb_storage and legacy app Zigbee cache,
 * preserves non-Zigbee settings (LED, verifier, etc.), and restarts the device.
 *
 * @note This function triggers esp_zigbee_factory_reset() which does not return.
 */
void zigbee_native_request_reset(void);

/**
 * @brief Populates the RAM diagnostic telemetry snapshot.
 *
 * @param[out] diag Pointer to receive diagnostics struct.
 */
void zigbee_native_get_diagnostics(zb_network_diagnostics_t *diag);

/**
 * @brief Checks if the native network is joined and ready for ZCL reporting.
 *
 * @return true if joined and ready, false otherwise.
 */
bool zigbee_native_is_network_ready(void);

/**
 * @brief Test seams for private startup handshake verification.
 */
typedef esp_err_t (*zb_native_init_fn_t)(void);
typedef esp_err_t (*zb_native_run_fn_t)(void);

void zigbee_native_stack_set_init_fn_for_test(zb_native_init_fn_t fn);
void zigbee_native_stack_set_run_fn_for_test(zb_native_run_fn_t fn);
void zigbee_native_stack_set_timeout_for_test(uint32_t timeout_ms);
void zigbee_native_stack_reset_for_test(void);

#ifdef __cplusplus
}
#endif

#endif /* ZIGBEE_NATIVE_STACK_H */
