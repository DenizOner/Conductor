/**
 * @file led_nvs_storage.h
 * @brief Persistent storage manager for LED mappings and global enabled state.
 *
 * Implements atomic updates, independent keys, corruption fallback, and write deduplication.
 */

#ifndef LED_NVS_STORAGE_H
#define LED_NVS_STORAGE_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Stable event / state IDs representing firmware notification contexts.
 */
typedef enum {
    LED_EVENT_BOOT = 0,
    LED_EVENT_PAIRING,
    LED_EVENT_NORMAL,
    LED_EVENT_CMD_FAILURE,
    LED_EVENT_CMD_SUCCESS,
    LED_EVENT_COUNT
} led_event_type_t;

#define LED_ANIM_ID_MAX_LEN 32

/**
 * @brief Complete LED configuration bundle.
 */
typedef struct {
    bool enabled;                                     /**< Global LED activity (Default: true) */
    char mapping[LED_EVENT_COUNT][LED_ANIM_ID_MAX_LEN]; /**< Active animation ID for each event */
} led_config_bundle_t;

/**
 * @brief Converts event enum to stable CLI string identifier.
 */
const char *led_event_to_string(led_event_type_t event);

/**
 * @brief Parses CLI event string into enum.
 * @return true if parsed, false if unrecognized string.
 */
bool led_event_from_string(const char *str, led_event_type_t *out_event);

/**
 * @brief Returns the factory default animation ID for a given event.
 */
const char *led_get_default_animation_for_event(led_event_type_t event);

/**
 * @brief Initializes storage and loads configuration from NVS.
 * Defaults are used on first boot, missing keys, or corrupted partition.
 * @param out_config Pointer to bundle struct to populate.
 * @return true on success.
 */
bool led_nvs_storage_load(led_config_bundle_t *out_config);

/**
 * @brief Saves the global enabled state to NVS (deduplicated).
 * @param enabled Desired state.
 * @return true on success.
 */
bool led_nvs_storage_save_enabled(bool enabled);

/**
 * @brief Saves a single event mapping to NVS (deduplicated).
 * @param event Event type enum.
 * @param anim_id Target animation ID.
 * @return true on success.
 */
bool led_nvs_storage_save_mapping(led_event_type_t event, const char *anim_id);

/**
 * @brief Resets a single event mapping or all event mappings to factory defaults in NVS.
 * Does NOT reset the global enabled flag.
 * @param event Specific event or -1 for all events.
 * @param config Active in-memory config bundle to update.
 * @return true on success.
 */
bool led_nvs_storage_reset_mappings(int event_or_all, led_config_bundle_t *config);

#ifdef __cplusplus
}
#endif

#endif /* LED_NVS_STORAGE_H */
