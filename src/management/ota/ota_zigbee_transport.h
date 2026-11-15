/**
 * @file ota_zigbee_transport.h
 * @brief Native Zigbee OTA Upgrade Client transport adapter and streaming parser.
 *
 * Responsibilities:
 *   - Registers and handles native ESP Zigbee SDK v2 core action callbacks:
 *       * EZB_ZCL_CORE_OTA_UPGRADE_CLIENT_PROGRESS_CB_ID
 *       * EZB_ZCL_CORE_OTA_UPGRADE_QUERY_NEXT_IMAGE_RSP_CB_ID
 *   - Streaming Zigbee OTA container file parser (file header, optional fields, sub-elements)
 *     that streams payload bytes directly into esp_ota_write() without full-image RAM buffering.
 *   - Metadata validation: Manufacturer ID (0x131B), Image Type (0x0906), monotonic file version,
 *     and target partition boundary checking.
 *   - Pinned Issue #906 Policy: In EZB_ZCL_OTA_UPGRADE_PROGRESS_FINISH, immediately resets device
 *     (esp_restart()) when validly APPLIED to prevent upstream esp-zigbee-lib 2.0.4 assertion failure.
 *   - Thread-safe query dispatch: ota_zigbee_transport_query_next_image().
 */

#ifndef OTA_ZIGBEE_TRANSPORT_H
#define OTA_ZIGBEE_TRANSPORT_H

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initializes the native Zigbee OTA transport adapter.
 *
 * Configures the OTA Upgrade client cluster on Endpoint 1 (client role init,
 * download block size 223 bytes; hardware version field intentionally absent) and
 * registers the core action callback handler.
 *
 * @return ESP_OK on success, or error code on failure.
 */
esp_err_t ota_zigbee_transport_init(void);

/**
 * @brief Dispatches a manual Query Next Image Request to the coordinator / OTA server.
 *
 * Requires that the device is joined and the Zigbee stack is running.
 * Dispatches to endpoint 1 of coordinator (short address 0x0000 or UpgradeServerID).
 *
 * @return ESP_OK on success, or error code if busy/unjoined/failed.
 */
esp_err_t ota_zigbee_transport_query_next_image(void);

/**
 * @brief Resets the streaming parser state and aborts any active flash write session.
 *
 * @param reason Human-readable reason for abort.
 */
void ota_zigbee_transport_abort(const char *reason);

/**
 * @brief Periodic lifecycle ticker for OTA transport (e.g. block timeout, health).
 */
void ota_zigbee_transport_process_cycle(void);

/**
 * @brief Thin diagnostics snapshot exposing active transfer-session state.
 *
 * These fields are owned by ota_zigbee_transport (which holds the esp_ota_* handles).
 * Callers must NOT cache this snapshot across tasks; always query fresh on each display.
 */
typedef struct {
    uint32_t offered_file_version;    /**< File version from parsed OTA header (0 if not yet parsed) */
    uint32_t payload_bytes_written;   /**< Raw firmware bytes streamed to flash so far */
    uint32_t total_expected_size;     /**< Total firmware payload expected (from sub-element length) */
    uint8_t  progress_percent;        /**< 0-100; 0 when idle or header not yet parsed */
    bool     session_active;          /**< true while DOWNLOADING or CHECKED */
} ota_transport_diag_t;

/**
 * @brief Populates an ota_transport_diag_t snapshot from the active streaming parser context.
 *
 * This is the single source of truth for transfer-progress diagnostics.
 * Safe to call from any task (reads only atomic/static context fields).
 *
 * @param out_diag Non-NULL pointer to struct to fill.
 */
void ota_zigbee_transport_get_diagnostics(ota_transport_diag_t *out_diag);

#ifdef __cplusplus
}
#endif

#endif /* OTA_ZIGBEE_TRANSPORT_H */
