/**
 * @file ota_client.h
 * @brief Native Zigbee OTA Upgrade Client engine, state machine, block downloader, and image verifier.
 */

#ifndef OTA_CLIENT_H
#define OTA_CLIENT_H

#include <stdbool.h>
#include <stdint.h>
#include "ota_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initializes the native Zigbee OTA Client engine.
 * Discovers inactive A/B OTA slot and verifies flash readiness.
 * @return true on success.
 */
bool ota_client_init(void);

/**
 * @brief Retrieves current OTA lifecycle state.
 */
ota_state_t ota_client_get_state(void);

/**
 * @brief Converts OTA state to string for logging and CLI display.
 */
const char *ota_client_state_str(ota_state_t state);

/**
 * @brief Retrieves last OTA operation result.
 */
ota_result_t ota_client_get_last_result(void);

/**
 * @brief Converts OTA result to string.
 */
const char *ota_client_result_str(ota_result_t res);

/**
 * @brief Dispatches a Query Next Image Request to coordinator/OTA server.
 * Uses central firmware version, manufacturer code (0x131B), and image type (0x0906).
 * Hardware version field is intentionally absent (fc=0x00) — no canonical HW version exists.
 * @return true if request dispatched successfully.
 */
bool ota_client_query_next_image(void);

/* NOTE: ota_client_handle_query_response() and ota_client_handle_image_block() have been
 * permanently removed (Phase 7 Corrective Hardening — Flash Ownership Consolidation).
 *
 * Flash primitives (esp_ota_begin/write/end/set_boot_partition) are owned exclusively
 * by ota_zigbee_transport.c. Callers must not attempt to re-add these APIs. */

/**
 * @brief Handles incoming Image Notify command from coordinator.
 * Automatically schedules non-blocking Query Next Image check.
 * @param payload_type Notify payload type.
 * @param jitter Random jitter value.
 * @param manufacturer_code Manufacturer code if present.
 * @param image_type Image type if present.
 * @param new_file_version New file version if present.
 */
void ota_client_handle_image_notify(uint8_t payload_type,
                                    uint8_t jitter,
                                    uint16_t manufacturer_code,
                                    uint16_t image_type,
                                    uint32_t new_file_version);

/**
 * @brief Periodic lifecycle ticker called from zigbee_task (typically every 1 second).
 * Manages query timing, block timeout detection, and reboot delay countdown.
 */
void ota_client_process_cycle(void);

/**
 * @brief Aborts active OTA download cleanly without corrupting the running system.
 * Cleans up OTA handles and resets state to IDLE.
 * @param reason Reason description.
 */
void ota_client_abort(const char *reason);

/**
 * @brief Records a post-update health failure before rollback is triggered.
 * This keeps OTA state/result signals aligned with the actual recovery path.
 * @param reason Human-readable rollback reason.
 */
void ota_client_note_health_failure(const char *reason);

/**
 * @brief Populates complete diagnostics snapshot for CLI and Zigbee reporting.
 * @param out_diag Pointer to struct to fill.
 */
void ota_client_get_diagnostics(ota_diagnostics_t *out_diag);

#ifdef __cplusplus
}
#endif

#endif /* OTA_CLIENT_H */
