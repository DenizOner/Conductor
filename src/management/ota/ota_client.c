/**
 * @file ota_client.c
 * @brief Implementation of native Zigbee OTA Upgrade Client engine, stream writing, and validation.
 */

#include "ota_client.h"
#include "ota_health.h"
#include "ota_zigbee_transport.h"
#include "config.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_log.h"
#include "esp_system.h"
#include "led_notification_manager.h"
#include <string.h>

static const char *TAG = "OTA_CLIENT";

/* OTA lifecycle state machine */
static ota_state_t  s_state = OTA_STATE_IDLE;
static ota_result_t s_last_result = OTA_RESULT_NONE;

/* Running and target partition — populated at init for diagnostics display only.
 * ota_client does NOT own flash; esp_ota_begin/write/end are managed exclusively
 * by ota_zigbee_transport via its streaming parser. */
static const esp_partition_t *s_running_partition = NULL;
static const esp_partition_t *s_target_partition  = NULL;

static void ota_client_sync_state_from_health(void)
{
    ota_health_recovery_state_t recovery = ota_health_get_recovery_state();
    if (recovery != OTA_HEALTH_RECOVERY_NONE &&
        recovery != OTA_HEALTH_RECOVERY_REQUEST_TOO_LATE) {
        /* Pending image is NOT a healthy pending download once validation
         * failed. Do not overwrite FAILED with PENDING_VALIDATION. */
        s_state = OTA_STATE_FAILED;
        /* Preserve an SDK error even if an operator also requested rollback.
         * This is not evidence of a failed Z906/physical health criterion. */
        s_last_result = ota_health_was_sdk_mark_valid_failed()
            ? OTA_RESULT_SDK_MARK_VALID_FAILED
            : (ota_health_was_manual_only_rollback()
                ? OTA_RESULT_MANUAL_ROLLBACK_REQUESTED
                : OTA_RESULT_HEALTH_CHECK_FAILED);
        return;
    }

    if (ota_health_is_pending_verify()) {
        if (s_state != OTA_STATE_DOWNLOADING &&
            s_state != OTA_STATE_VERIFYING &&
            s_state != OTA_STATE_READY_TO_REBOOT) {
            s_state = OTA_STATE_PENDING_VALIDATION;
        }
        return;
    }

    if (s_state == OTA_STATE_PENDING_VALIDATION) {
        s_state = OTA_STATE_SUCCESS;
    } else if (ota_health_was_rollback_detected() && s_state == OTA_STATE_IDLE) {
        s_state = OTA_STATE_ROLLBACK;
    }
}

bool ota_client_init(void)
{
    s_running_partition = esp_ota_get_running_partition();
    s_target_partition = esp_ota_get_next_update_partition(NULL);

    if (s_running_partition == NULL || s_target_partition == NULL) {
        ESP_LOGE(TAG, "Failed to identify running or target A/B OTA partitions!");
        return false;
    }

    ESP_LOGI(TAG, "Native Zigbee OTA Client Initialized:");
    ESP_LOGI(TAG, "  Running Slot:   %s (0x%08lx, Size: %lu KB)",
             s_running_partition->label, (unsigned long)s_running_partition->address,
             (unsigned long)(s_running_partition->size / 1024));
    ESP_LOGI(TAG, "  Target OTA Slot: %s (0x%08lx, Size: %lu KB)",
             s_target_partition->label, (unsigned long)s_target_partition->address,
             (unsigned long)(s_target_partition->size / 1024));
    ESP_LOGI(TAG, "  Identity: MFR=0x%04X, ImageType=0x%04X, Version=0x%08lX (%s)",
             Z906_OTA_MANUFACTURER_CODE, Z906_OTA_IMAGE_TYPE,
             (unsigned long)Z906_OTA_FILE_VERSION, Z906_FW_VERSION_STR);

    ota_health_init();
    return true;
}

ota_state_t ota_client_get_state(void)
{
    return s_state;
}

const char *ota_client_state_str(ota_state_t state)
{
    switch (state) {
        case OTA_STATE_IDLE:                return "IDLE";
        case OTA_STATE_CHECKING:            return "CHECKING";
        case OTA_STATE_AVAILABLE:           return "AVAILABLE";
        case OTA_STATE_DOWNLOADING:         return "DOWNLOADING";
        case OTA_STATE_VERIFYING:           return "VERIFYING";
        case OTA_STATE_READY_TO_REBOOT:     return "READY_TO_REBOOT";
        case OTA_STATE_PENDING_VALIDATION:  return "PENDING_VALIDATION";
        case OTA_STATE_SUCCESS:             return "SUCCESS";
        case OTA_STATE_FAILED:              return "FAILED";
        case OTA_STATE_ROLLBACK:            return "ROLLBACK";
        default:                            return "UNKNOWN";
    }
}

ota_result_t ota_client_get_last_result(void)
{
    return s_last_result;
}

const char *ota_client_result_str(ota_result_t res)
{
    switch (res) {
        case OTA_RESULT_NONE:                 return "NONE";
        case OTA_RESULT_SUCCESS:              return "SUCCESS";
        case OTA_RESULT_DOWNLOAD_FAILED:      return "DOWNLOAD_FAILED";
        case OTA_RESULT_VALIDATION_FAILED:    return "VALIDATION_FAILED";
        case OTA_RESULT_INCOMPATIBLE_IMAGE:   return "INCOMPATIBLE_IMAGE";
        case OTA_RESULT_FLASH_FAILED:         return "FLASH_FAILED";
        case OTA_RESULT_HEALTH_CHECK_FAILED:  return "HEALTH_CHECK_FAILED";
        case OTA_RESULT_ROLLBACK_DETECTED:    return "ROLLBACK_DETECTED";
        case OTA_RESULT_MANUAL_ROLLBACK_REQUESTED: return "MANUAL_ROLLBACK_REQUESTED";
        case OTA_RESULT_SDK_MARK_VALID_FAILED: return "SDK_MARK_VALID_FAILED";
        default:                              return "UNKNOWN";
    }
}

bool ota_client_query_next_image(void)
{
    if (s_state == OTA_STATE_DOWNLOADING || s_state == OTA_STATE_VERIFYING) {
        ESP_LOGW(TAG, "Cannot query next image: OTA already in progress (%s)", ota_client_state_str(s_state));
        return false;
    }

    s_state = OTA_STATE_CHECKING;
    ESP_LOGI(TAG, "Dispatching Query Next Image Request to Zigbee2MQTT OTA Server:");
    ESP_LOGI(TAG, "  Current FileVersion: 0x%08lX (%s), MFR: 0x%04X, ImageType: 0x%04X, hw_version: absent",
             (unsigned long)Z906_OTA_FILE_VERSION, Z906_FW_VERSION_STR,
             Z906_OTA_MANUFACTURER_CODE, Z906_OTA_IMAGE_TYPE);

    return true;
}

/* NOTE: ota_client_handle_query_response() and ota_client_handle_image_block() have been
 * intentionally removed as part of the Phase 7 Corrective Hardening.
 *
 * Rationale (Flash Ownership Consolidation):
 *   ota_zigbee_transport.c is the sole owner of ESP-IDF OTA flash primitives
 *   (esp_ota_begin, esp_ota_write, esp_ota_end, esp_ota_set_boot_partition).
 *   The transport owns the streaming parser state machine and calls these
 *   primitives directly from handle_client_progress() callbacks.
 *
 *   The old ota_client_handle_query_response / ota_client_handle_image_block
 *   functions duplicated these flash primitives and were never called from
 *   ota_zigbee_transport.c, creating a dual-ownership audit red flag.
 *   Keeping them as dead code would risk accidental wiring in future development
 *   that would corrupt the active OTA partition. They are permanently removed.
 */

void ota_client_handle_image_notify(uint8_t payload_type,
                                    uint8_t jitter,
                                    uint16_t manufacturer_code,
                                    uint16_t image_type,
                                    uint32_t new_file_version)
{
    (void)payload_type;
    (void)jitter;
    ESP_LOGI(TAG, "Received Zigbee Image Notify: MFR=0x%04X, ImageType=0x%04X, Version=0x%08lX",
             manufacturer_code, image_type, (unsigned long)new_file_version);

    if (manufacturer_code == 0xFFFF || manufacturer_code == Z906_OTA_MANUFACTURER_CODE) {
        if (image_type == 0xFFFF || image_type == Z906_OTA_IMAGE_TYPE) {
            if (new_file_version == 0 || new_file_version > Z906_OTA_FILE_VERSION) {
                ota_client_query_next_image();
            }
        }
    }
}

void ota_client_process_cycle(void)
{
    /* 1. Process post-update health check (owned by telemetry_task, ~1 Hz). */
    ota_health_process_cycle(1);
    ota_client_sync_state_from_health();

    /* NOTE: Block transfer timeout monitoring and reboot countdown management
     * have been removed from ota_client_process_cycle.
     *
     * The transport (ota_zigbee_transport.c) drives the complete flash lifecycle
     * via its streaming parser inside handle_client_progress() callbacks.
     * Block reception timeouts are implicitly guarded by the Zigbee stack's own
     * image block request retry and abort mechanisms. The reboot countdown was
     * superseded by the Issue #906 immediate esp_restart() policy in the FINISH
     * progress callback. No countdown is needed in ota_client. */
}

void ota_client_note_health_failure(const char *reason)
{
    s_last_result = OTA_RESULT_HEALTH_CHECK_FAILED;
    s_state = OTA_STATE_FAILED;
    ESP_LOGE(TAG, "OTA health check failed: %s", reason ? reason : "Unknown");
    led_notification_post_cmd_result(false);
}

void ota_client_abort(const char *reason)
{
    /* Abort is called by ota_health when post-update validation fails.
     * ota_client does not hold an OTA flash handle (flash ownership belongs to
     * ota_zigbee_transport). This function only resets the client state machine. */
    ESP_LOGW(TAG, "OTA client state reset: %s", reason ? reason : "Unknown");
    s_state = OTA_STATE_IDLE;
    led_notification_post_cmd_result(false);
}

void ota_client_get_diagnostics(ota_diagnostics_t *out_diag)
{
    if (out_diag == NULL) {
        return;
    }

    /* Fields owned by ota_client: lifecycle state, result codes, partition identity. */
    out_diag->current_state      = s_state;
    out_diag->last_result        = s_last_result;
    out_diag->current_file_version = Z906_OTA_FILE_VERSION;

    out_diag->running_partition_name = s_running_partition ? s_running_partition->label : "unknown";
    out_diag->target_partition_name  = s_target_partition  ? s_target_partition->label  : "unknown";
    out_diag->is_rollback_possible   = esp_ota_check_rollback_is_possible();
    out_diag->is_pending_verify      = ota_health_is_pending_verify();

    /* Transfer-progress fields are delegated to ota_zigbee_transport \u2014 the single source
     * of truth for flash-session data (esp_ota_begin/write/end ownership lives there).
     * This delegation ensures no fake/stale zero is shown when a session is active. */
    ota_transport_diag_t tr;
    ota_zigbee_transport_get_diagnostics(&tr);
    out_diag->target_file_version = tr.offered_file_version;
    out_diag->bytes_downloaded    = tr.payload_bytes_written;
    out_diag->total_image_size    = tr.total_expected_size;
    out_diag->progress_percent    = tr.progress_percent;
}

