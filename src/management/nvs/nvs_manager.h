/**
 * @file nvs_manager.h
 * @brief Non-Volatile Storage (NVS) management for persistent Zigbee credentials and system state.
 */

#ifndef NVS_MANAGER_H
#define NVS_MANAGER_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Persistent Zigbee network metadata stored in NVS.
 */
typedef struct {
    bool is_paired;             /**< true if device has joined and authenticated on a network */
    uint16_t pan_id;            /**< 16-bit Zigbee PAN Identifier */
    uint8_t channel;            /**< Active 2.4 GHz IEEE 802.15.4 channel (11 - 26) */
    uint8_t ext_pan_id[8];      /**< 64-bit Extended PAN Identifier (EUI-64) */
} zb_persistent_net_info_t;

/**
 * @brief Initializes NVS flash partition. Erases and recovers if truncated or incompatible.
 * @return true on success, false otherwise.
 */
bool nvs_manager_init(void);

/**
 * @brief Retrieves the persistent Zigbee paired status from NVS.
 * @param out_is_paired Pointer to receive boolean paired flag.
 * @return true if successfully read, false if key not found or read error.
 */
bool nvs_manager_get_zb_paired(bool *out_is_paired);

/**
 * @brief Saves complete Zigbee network metadata (paired flag, PAN ID, channel, Ext PAN) to NVS.
 *
 * This is the canonical persistence API for a Zigbee join event. It is the sole
 * writer of the 'zb_paired' key in 'zb_store' — do not introduce a second writer
 * for this key. Write deduplication is applied: if all four stored values already
 * match the incoming struct exactly, no NVS write or commit is performed.
 *
 * @param net_info Pointer to network info structure to persist.
 * @return true on success or when all values match (no write needed), false on failure.
 */
bool nvs_manager_save_zb_net_info(const zb_persistent_net_info_t *net_info);

/**
 * @brief Loads complete Zigbee network metadata from NVS.
 * @param out_net_info Pointer to receive network info.
 * @return true on success, false if records do not exist or read failed.
 */
bool nvs_manager_load_zb_net_info(zb_persistent_net_info_t *out_net_info);

/**
 * @brief Clears Zigbee network credentials and pairing flag from NVS namespace without wiping entire partition.
 * @return true on success, false on failure.
 */
bool nvs_manager_clear_zb_net_info(void);

/**
 * @brief Retrieves the persistent Command Verifier enabled status from NVS.
 * Defaults to true if key is not found in NVS.
 * @param out_enabled Pointer to receive boolean enabled flag.
 * @return true if successfully read, false if error (out_enabled still set to default true).
 */
bool nvs_manager_get_cmd_verifier_enabled(bool *out_enabled);

/** @brief Read-only OTA NVS health: missing optional key is healthy;
 * genuine open/read/type errors are not. Never erases or writes NVS.
 */
bool nvs_manager_check_read_health(void);

/**
 * @brief Saves the persistent Command Verifier enabled status to NVS with write deduplication.
 * @param enabled Desired state (true = enabled, false = disabled).
 * @return true on success, false on failure.
 */
bool nvs_manager_set_cmd_verifier_enabled(bool enabled);

/**
 * @brief Retrieves the persistent one-shot pair intent flag from NVS.
 * Used exclusively for power-loss safe disconnected/joined re-pair flow.
 * @param out_intent Pointer to receive boolean intent flag.
 * @return true if successfully read, false if key not found or read error (out_intent set to false).
 */
bool nvs_manager_get_pair_after_reset(bool *out_intent);

/**
 * @brief Saves the persistent one-shot pair intent flag to NVS with write deduplication.
 * @param intent Desired state (true = pair after reset, false = no intent).
 * @return true on success, false on failure.
 */
bool nvs_manager_set_pair_after_reset(bool intent);

/**
 * @brief Clears the persistent one-shot pair intent flag from NVS.
 * @return true on success or if key already absent, false on error.
 */
bool nvs_manager_clear_pair_after_reset(void);

/**
 * @brief Erases the entire application NVS partition (full factory reset).
 *
 * WARNING: This erases ALL namespaces including 'zb_store' (Zigbee credentials)
 * AND 'led_store' (LED preferences) and any other NVS data. This is NOT a
 * Zigbee-only reset. Use nvs_manager_clear_zb_net_info() for Zigbee-only clearing.
 *
 * @return true on success, false on failure.
 */
bool nvs_manager_factory_reset(void);

#ifdef __cplusplus
}
#endif

#endif /* NVS_MANAGER_H */
