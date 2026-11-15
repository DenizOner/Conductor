/**
 * @file zigbee_app.h
 * @brief Native Zigbee 3.0 stack initialization, pairing management, and attribute reporting lifecycle.
 */

#ifndef ZIGBEE_APP_H
#define ZIGBEE_APP_H

#include <stdbool.h>
#include <stdint.h>
#include "z906_state.h"
#include "nvs_manager.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Discrete Zigbee network connection and commissioning states.
 */
typedef enum {
    ZB_STATE_UNPAIRED = 0,          /**< Factory default or left network; idle, waiting for pairing command */
    ZB_STATE_PAIRING,               /**< Steering window active (180s); scanning and negotiating */
    ZB_STATE_JOINED,                /**< Successfully joined and authenticated with coordinator */
    ZB_STATE_REJOINING,             /**< Reconnecting after boot or temporary connection loss (retries every 30s) */
    ZB_STATE_COMMISSION_FAILED,     /**< Steering window timed out without finding coordinator */
    ZB_STATE_LEAVING                /**< Cleanly leaving current network and clearing persistent credentials */
} zb_network_state_t;

/**
 * @brief Initializes the Zigbee 3.0 stack, checks NVS credentials, and sets initial state.
 * If previous pairing is found in NVS, enters ZB_STATE_REJOINING to preserve credentials.
 * If no pairing exists, enters ZB_STATE_UNPAIRED without scanning.
 * @return true on success.
 */
bool zigbee_app_init(void);

/**
 * @brief Starts Zigbee background operations. If in rejoining state, triggers rejoin attempt.
 * @return true on success.
 */
bool zigbee_app_start(void);

/**
 * @brief Retrieves the current Zigbee network state.
 * @return Current zb_network_state_t value.
 */
zb_network_state_t zigbee_app_get_state(void);

/**
 * @brief Converts a network state enum into a human-readable string.
 * @param state Network state.
 * @return String representation of the state.
 */
const char *zigbee_app_state_str(zb_network_state_t state);

/**
 * @brief Initiates network pairing (commissioning/steering window for 180 seconds).
 * If the device is already paired or rejoining and force_repair is true, cleanly leaves
 * the current network, erases previous NVS credentials, and opens steering.
 * @param force_repair When true, forces existing network clearance before opening pairing.
 * @return true if pairing mode was entered successfully, false otherwise.
 */
bool zigbee_app_start_pairing(bool force_repair);

/**
 * @brief Commands the device to cleanly leave the network, erases NVS credentials, and returns to UNPAIRED.
 * @return true on success.
 */
bool zigbee_app_leave_network(void);

/**
 * @brief Periodic lifecycle process function invoked by zigbee_task (typically every 1 second).
 * Manages the 180s pairing timeout and 30s rejoin retry intervals non-blockingly.
 */
void zigbee_app_process_cycle(void);

/**
 * @brief Checks if device is fully joined and authenticated on an active Zigbee network.
 * @return true if state == ZB_STATE_JOINED.
 */
bool zigbee_app_is_joined(void);

/**
 * @brief Checks if device possesses valid pairing credentials (either JOINED or REJOINING).
 * @return true if paired.
 */
bool zigbee_app_is_paired(void);

/**
 * @brief Retrieves active or stored Zigbee network metadata.
 * @param out_info Pointer to receive network info.
 * @return true if valid network info was loaded, false otherwise.
 */
bool zigbee_app_get_net_info(zb_persistent_net_info_t *out_info);

/**
 * @brief Returns remaining pairing countdown time in seconds.
 * @return Seconds remaining (0 to 180).
 */
int zigbee_app_get_pairing_time_left(void);

/**
 * @brief Generic ZCL Attribute Report Record container.
 */
typedef struct {
    uint8_t endpoint;
    uint16_t cluster_id;
    uint16_t attr_id;
    uint8_t attr_type;
    uint32_t val_u32;
    int16_t val_i16;
} zcl_report_record_t;

/**
 * @brief Dispatches a single ZCL attribute update and network report to coordinator.
 * Thread-safe and context-checked.
 * @param rec Report record parameters.
 * @return true if successfully dispatched / queued.
 */
bool zigbee_app_report_zcl_attribute(const zcl_report_record_t *rec);

/**
 * @brief Reports updated confirmed amplifier telemetry to the Zigbee coordinator.
 * Detects state deltas and publishes ZCL attribute reports only when state changes.
 * @param state Snapshot of the current confirmed state.
 */
void zigbee_app_report_state(const z906_state_t *state);

/**
 * @brief Forces a complete ZCL attribute report regardless of delta caching.
 * @param state Snapshot of the current confirmed state.
 */
void zigbee_app_report_state_force(const z906_state_t *state);

/**
 * @brief Returns the total count of ZCL attribute reports dispatched since boot/reset.
 * @return Number of attribute reports.
 */
uint32_t zigbee_app_get_report_count(void);

/**
 * @brief Retrieves a copy of the last reported state for testing and verification.
 * @param out_state Pointer to receive state copy.
 */
void zigbee_app_get_last_reported_state(z906_state_t *out_state);

/**
 * @brief Retrieves the last reported standard 0x0402 temperature in (Celsius * 100) for EP11, EP12, or EP13.
 * Returns ZCL_TEMP_INVALID_VALUE (0x8000) if temp was invalid / unmeasured.
 * @param endpoint 11 (Temp1), 12 (Temp2), or 13 (Temp3/PSU).
 * @return int16_t measured value in 0.01 °C units or 0x8000.
 */
int16_t zigbee_app_get_last_temp_reported(uint8_t endpoint);

/* ========================================================================= */
/* Test & Simulation API Hooks                                               */
/* ========================================================================= */

/**
 * @brief Simulates successful Zigbee association/steering callback with coordinator.
 * Saves network info to NVS and transitions to ZB_STATE_JOINED.
 * @param pan_id 16-bit PAN ID.
 * @param channel 802.15.4 channel.
 * @param ext_pan 8-byte Extended PAN ID.
 * @return true on success.
 */
bool zigbee_app_simulate_join(uint16_t pan_id, uint8_t channel, const uint8_t *ext_pan);

/**
 * @brief Simulates a temporary rejoin attempt failure (coordinator offline).
 * Retains credentials in NVS and remains in ZB_STATE_REJOINING.
 */
void zigbee_app_simulate_rejoin_failure(void);

/**
 * @brief Simulates a commissioning failure (steering timeout without coordinator).
 * Transitions state to ZB_STATE_COMMISSION_FAILED.
 */
void zigbee_app_simulate_commission_failure(void);

/**
 * @brief Resets in-memory state for testing harness.
 */
void zigbee_app_reset_state(void);

#ifdef __cplusplus
}
#endif

#endif /* ZIGBEE_APP_H */
