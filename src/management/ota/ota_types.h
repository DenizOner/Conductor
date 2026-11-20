/**
 * @file ota_types.h
 * @brief Canonical data types, states, headers, and result codes for Native Zigbee OTA Upgrade.
 */

#ifndef OTA_TYPES_H
#define OTA_TYPES_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "esp_ota_ops.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Discrete state machine for native Zigbee OTA Upgrade Client lifecycle.
 */
typedef enum {
    OTA_STATE_IDLE = 0,             /**< Idle; running confirmed workable firmware */
    OTA_STATE_CHECKING,             /**< Querying coordinator/OTA server for new firmware */
    OTA_STATE_AVAILABLE,            /**< Newer compatible firmware image is available on server */
    OTA_STATE_DOWNLOADING,          /**< Actively requesting and writing image blocks to inactive slot */
    OTA_STATE_VERIFYING,            /**< All blocks received; validating image header & ESP signature */
    OTA_STATE_READY_TO_REBOOT,      /**< Validated and boot partition set; waiting to safely restart */
    OTA_STATE_PENDING_VALIDATION,   /**< First boot post-upgrade; running post-update health check */
    OTA_STATE_SUCCESS,              /**< Post-update health check passed; firmware marked VALID */
    OTA_STATE_FAILED,               /**< OTA download or verification failed */
    OTA_STATE_ROLLBACK              /**< Health check or boot failed; rollback to previous slot executed */
} ota_state_t;

/**
 * @brief Diagnostic error/result codes for last OTA operation.
 */
typedef enum {
    OTA_RESULT_NONE = 0,
    OTA_RESULT_SUCCESS,
    OTA_RESULT_DOWNLOAD_FAILED,
    OTA_RESULT_VALIDATION_FAILED,
    OTA_RESULT_INCOMPATIBLE_IMAGE,
    OTA_RESULT_FLASH_FAILED,
    OTA_RESULT_HEALTH_CHECK_FAILED,
    OTA_RESULT_ROLLBACK_DETECTED,
    OTA_RESULT_MANUAL_ROLLBACK_REQUESTED, /**< Operator action, not a failed health check */
    OTA_RESULT_SDK_MARK_VALID_FAILED      /**< ESP-IDF rejected permanent OTA approval */
} ota_result_t;

/**
 * @brief Canonical Zigbee OTA file header structure (ZCL Specification Section 11.4).
 */
typedef struct __attribute__((packed)) {
    uint32_t upgrade_file_id;       /**< 0x0BEEF11E (Zigbee OTA File Identifier, ZCL r08 §11.4) */
    uint16_t header_version;        /**< 0x0100 (Header Version 1.0) */
    uint16_t header_length;         /**< Total length of the header in bytes */
    uint16_t field_control;         /**< Field control bitmap (security, hardware versions, etc.) */
    uint16_t manufacturer_code;     /**< Manufacturer Code (e.g. 0x131B) */
    uint16_t image_type;            /**< Image Type (e.g. 0x0906) */
    uint32_t file_version;          /**< 32-bit integer version (e.g. 0x010400AA) */
    uint16_t zigbee_stack_version;  /**< Zigbee Stack Version */
    uint8_t  header_string[32];     /**< Human readable image string */
    uint32_t total_image_size;      /**< Total size of the file including header */
} zigbee_ota_header_t;

/* Canonical ZCL OTA Upgrade File Identifier (ZCL r08 §11.4.2.1.1).
 * Little-endian wire representation: 1E F1 EE 0B.
 * Note: the former value 0x0BEF11E0 (byte-reversed) was incorrect and is permanently rejected. */
#define ZIGBEE_OTA_FILE_IDENTIFIER  0x0BEEF11E
#define ZIGBEE_OTA_HEADER_VERSION   0x0100

/**
 * @brief Complete runtime diagnostics snapshot for CLI and Zigbee queries.
 */
typedef struct {
    ota_state_t  current_state;
    ota_result_t last_result;
    uint32_t     current_file_version;
    uint32_t     target_file_version;
    uint32_t     bytes_downloaded;
    uint32_t     total_image_size;
    uint8_t      progress_percent;
    const char  *running_partition_name;
    const char  *target_partition_name;
    bool         is_rollback_possible;
    bool         is_pending_verify;
} ota_diagnostics_t;

#ifdef __cplusplus
}
#endif

#endif /* OTA_TYPES_H */
