/**
 * @file ota_health.c
 * @brief Implementation of post-update health check, criteria evaluation, and rollback trigger.
 */

#include "ota_health.h"
#include "ota_client.h"
#include "esp_ota_ops.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "wdt_manager.h"
#include "nvs_manager.h"
#include "zigbee_native_stack.h"
#include "zigbee_device_model.h"
#include "esp_timer.h"
#include <string.h>
#include "z906_state.h"
#include "led_notification_manager.h"

static const char *TAG = "OTA_HEALTH";

#define OTA_HEALTH_VALIDATION_WINDOW_S  15
#define OTA_Z906_STATUS_MAX_AGE_US     (5000000LL)

static bool s_is_pending_verify = false;
static bool s_rollback_detected = false;
static bool s_is_validated = false;
static uint32_t s_seconds_in_validation = 0;
static int64_t s_validation_start_us = 0;
static bool s_rollback_handled = false;
static uint32_t s_passed_criteria_mask = 0;
static ota_health_criteria_t s_criteria = { 0 };

static ota_rollback_fn_t s_rollback_fn_for_test = NULL;
static ota_mark_valid_fn_t s_mark_valid_fn_for_test = NULL;
static ota_health_time_fn_t s_time_fn_for_test = NULL;
static ota_health_time_fn_t s_status_time_fn_for_test = NULL;
static ota_health_bool_fn_t s_zigbee_ready_fn_for_test = NULL;
static ota_health_bool_fn_t s_nvs_healthy_fn_for_test = NULL;
static ota_health_bool_fn_t s_twdt_healthy_fn_for_test = NULL;
static bool s_telemetry_twdt_feed_ok = false;
static bool s_finalization_in_progress = false;
/* Short state snapshots only: SDK / NVS / OTA APIs are NEVER called under this spinlock. */
static portMUX_TYPE s_health_mux = portMUX_INITIALIZER_UNLOCKED;
static ota_health_bool_fn_t s_fallback_available_fn_for_test = NULL;

static int64_t ota_health_now_us(void)
{
    return s_time_fn_for_test != NULL ? s_time_fn_for_test() : esp_timer_get_time();
}

static void ota_health_refresh_mask_from_criteria(void)
{
    s_passed_criteria_mask = 0;
    if (s_criteria.scheduler_healthy) s_passed_criteria_mask |= OTA_CRITERION_SCHEDULER;
    if (s_criteria.watchdog_healthy) s_passed_criteria_mask |= OTA_CRITERION_TWDT;
    if (s_criteria.nvs_healthy) s_passed_criteria_mask |= OTA_CRITERION_NVS;
    if (s_criteria.zigbee_healthy) s_passed_criteria_mask |= OTA_CRITERION_ZIGBEE;
    if (s_criteria.z906_comm_healthy) s_passed_criteria_mask |= OTA_CRITERION_Z906_COMM;
}

static void evaluate_criteria(void);

bool ota_health_init(void)
{
    s_passed_criteria_mask = 0;
    s_criteria = (ota_health_criteria_t){ 0 };
    s_is_pending_verify = false;
    s_rollback_handled = false;
    s_finalization_in_progress = false;
    s_validation_start_us = 0;
    evaluate_criteria();

    const esp_partition_t *running = esp_ota_get_running_partition();
    if (running == NULL) {
        ESP_LOGE(TAG, "Failed to retrieve current running partition");
        return false;
    }

    esp_ota_img_states_t ota_state;
    esp_err_t err = esp_ota_get_state_partition(running, &ota_state);

    if (err == ESP_OK) {
        if (ota_state == ESP_OTA_IMG_PENDING_VERIFY) {
            s_is_pending_verify = true;
            s_is_validated = false;
            s_seconds_in_validation = 0;
            s_validation_start_us = ota_health_now_us();
            ESP_LOGW(TAG, "=========================================================");
            ESP_LOGW(TAG, "NEW FIRMWARE DETECTED IN PENDING_VERIFY STATE (Slot: %s)", running->label);
            ESP_LOGW(TAG, "Starting %d-second post-update health observation window.", OTA_HEALTH_VALIDATION_WINDOW_S);
            ESP_LOGW(TAG, "=========================================================");
        } else if (ota_state == ESP_OTA_IMG_VALID) {
            s_is_pending_verify = false;
            s_is_validated = true;
            ESP_LOGI(TAG, "Firmware in partition '%s' is confirmed VALID.", running->label);
        }
    } else {
        /* Not an OTA partition (e.g. factory or custom non-otadata) */
        s_is_pending_verify = false;
        s_is_validated = true;
    }

    /* Check if previous boot encountered an invalid partition and rolled back */
    const esp_partition_t *invalid_part = esp_ota_get_last_invalid_partition();
    if (invalid_part != NULL) {
        s_rollback_detected = true;
        ESP_LOGW(TAG, "NOTICE: Previous firmware upgrade failed. Rolled back from invalid slot '%s'.",
                 invalid_part->label);
        led_notification_post_cmd_result(false);
    }

    return true;
}

static void evaluate_criteria(void)
{
    ota_health_criteria_t sample = { 0 };
    sample.scheduler_healthy = (xTaskGetSchedulerState() == taskSCHEDULER_RUNNING);

    portENTER_CRITICAL(&s_health_mux);
    bool owner_fed = s_telemetry_twdt_feed_ok;
    portEXIT_CRITICAL(&s_health_mux);
    /* Current task = telemetry owner in the only production health cycle.
     * CLI diagnostics must never call evaluate_criteria(). */
    sample.watchdog_healthy = s_twdt_healthy_fn_for_test != NULL
        ? s_twdt_healthy_fn_for_test()
        : (wdt_manager_is_initialized() && !wdt_manager_was_watchdog_reset() &&
           owner_fed && wdt_manager_is_current_task_registered());

    sample.nvs_healthy = s_nvs_healthy_fn_for_test != NULL
        ? s_nvs_healthy_fn_for_test() : nvs_manager_check_read_health();

    sample.zigbee_healthy = s_zigbee_ready_fn_for_test != NULL
        ? s_zigbee_ready_fn_for_test()
        : (zigbee_native_stack_get_state() == ZB_STACK_STATE_RUNNING &&
           zigbee_device_model_is_registered());

    int64_t now_us = ota_health_now_us();
    int64_t status_us = s_status_time_fn_for_test != NULL
        ? s_status_time_fn_for_test()
        : z906_state_get_matched_status_reply_time_us();
    sample.z906_comm_healthy = z906_state_is_main_valid() &&
        status_us > s_validation_start_us &&
        status_us <= now_us &&
        (now_us - status_us) <= OTA_Z906_STATUS_MAX_AGE_US;

    portENTER_CRITICAL(&s_health_mux);
    s_criteria = sample;
    ota_health_refresh_mask_from_criteria();
    portEXIT_CRITICAL(&s_health_mux);
}

void ota_health_process_cycle(uint32_t elapsed_sec)
{
    portENTER_CRITICAL(&s_health_mux);
    bool can_evaluate = s_is_pending_verify && !s_is_validated &&
                        !s_rollback_handled && !s_finalization_in_progress;
    portEXIT_CRITICAL(&s_health_mux);
    if (!can_evaluate) {
        return;
    }

    /* Use monotonic time since pending boot; do not assume periodic task ticks. */
    (void)elapsed_sec; /* Kept for existing callers; elapsed time is measured. */
    int64_t elapsed_us = ota_health_now_us() - s_validation_start_us;
    if (elapsed_us < 0) {
        return; /* A clock anomaly must never approve an OTA image. */
    }
    s_seconds_in_validation = (uint32_t)(elapsed_us / 1000000LL);
    evaluate_criteria();
    ota_health_criteria_t sample;
    ota_health_get_criteria(&sample);

    bool all_criteria_met = sample.scheduler_healthy &&
                            sample.watchdog_healthy &&
                            sample.nvs_healthy &&
                            sample.zigbee_healthy &&
                            sample.z906_comm_healthy;

    if (!all_criteria_met) {
        ESP_LOGW(TAG, "Health check criteria not satisfied yet: Sched=%d, WDT=%d, NVS=%d, ZB=%d, Z906=%d (Elapsed: %lu s)",
                 sample.scheduler_healthy, sample.watchdog_healthy,
                 sample.nvs_healthy, sample.zigbee_healthy, sample.z906_comm_healthy,
                 (unsigned long)s_seconds_in_validation);

        if (s_seconds_in_validation >= OTA_HEALTH_VALIDATION_WINDOW_S) {
            ESP_LOGE(TAG, "Health check observation window expired before all criteria were healthy. Initiating rollback!");
            ota_client_note_health_failure("Health criteria failed during validation window");
            ota_health_trigger_rollback("Health criteria timeout expired");
        }
        return;
    }

    if (s_seconds_in_validation >= OTA_HEALTH_VALIDATION_WINDOW_S) {
        /* CLI manual rollback and telemetry automatic approval are mutually
         * exclusive even if scheduled on different FreeRTOS tasks. */
        portENTER_CRITICAL(&s_health_mux);
        bool claimed = s_is_pending_verify && !s_is_validated &&
                       !s_rollback_handled && !s_finalization_in_progress;
        if (claimed) {
            s_finalization_in_progress = true;
        }
        portEXIT_CRITICAL(&s_health_mux);
        if (!claimed) {
            return;
        }

        ESP_LOGI(TAG, "HEALTH CHECK PASSED! All 5 subsystem criteria verified.");
        esp_err_t err = s_mark_valid_fn_for_test != NULL
            ? s_mark_valid_fn_for_test()
            : esp_ota_mark_app_valid_cancel_rollback();
        portENTER_CRITICAL(&s_health_mux);
        if (err == ESP_OK) {
            s_is_validated = true;
            s_is_pending_verify = false;
        }
        s_finalization_in_progress = false;
        portEXIT_CRITICAL(&s_health_mux);
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "SUCCESS: Firmware upgrade permanently COMMITTED!");
            led_notification_post_cmd_result(true);
        } else {
            ESP_LOGE(TAG, "Failed to mark app as valid: %s; initiating rollback",
                     esp_err_to_name(err));
            ota_health_trigger_rollback("esp_ota_mark_app_valid failed");
        }
    } else {
        ESP_LOGI(TAG, "Health check in progress... (%lu/%d s) - All criteria healthy.",
                 (unsigned long)s_seconds_in_validation, OTA_HEALTH_VALIDATION_WINDOW_S);
    }
}

bool ota_health_is_pending_verify(void)
{
    portENTER_CRITICAL(&s_health_mux);
    bool pending = s_is_pending_verify;
    portEXIT_CRITICAL(&s_health_mux);
    return pending;
}

void ota_health_trigger_rollback(const char *reason)
{
    portENTER_CRITICAL(&s_health_mux);
    bool already_handled = s_rollback_handled || s_finalization_in_progress;
    if (!already_handled) {
        s_rollback_handled = true;
    }
    portEXIT_CRITICAL(&s_health_mux);
    if (already_handled) {
        return;
    }
    ESP_LOGE(TAG, "OTA health failed: %s", reason ? reason : "Unknown");
    led_notification_post_cmd_result(false);

    bool fallback_available = s_fallback_available_fn_for_test != NULL
        ? s_fallback_available_fn_for_test()
        : esp_ota_check_rollback_is_possible();
    if (!fallback_available) {
        /* Never force a restart loop without a known-good previous image.
         * Leave the running image pending and require a recovery decision. */
        ESP_LOGE(TAG, "No valid rollback partition available; refusing forced restart");
        return;
    }

    if (s_rollback_fn_for_test != NULL) {
        s_rollback_fn_for_test(reason);
        return;
    }

    esp_err_t err = esp_ota_mark_app_invalid_rollback_and_reboot();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Rollback rejected: %s; staying pending for diagnostics",
                 esp_err_to_name(err));
    }
}

void ota_health_get_criteria(ota_health_criteria_t *out_criteria)
{
    if (out_criteria != NULL) {
        /* Never evaluate health from CLI context: that would check the CLI task's
         * TWDT registration rather than the telemetry owner and mutate state. */
        portENTER_CRITICAL(&s_health_mux);
        *out_criteria = s_criteria;
        portEXIT_CRITICAL(&s_health_mux);
    }
}

uint32_t ota_health_get_passed_criteria_mask(void)
{
    portENTER_CRITICAL(&s_health_mux);
    uint32_t mask = s_passed_criteria_mask;
    portEXIT_CRITICAL(&s_health_mux);
    return mask;
}

void ota_health_report_scheduler_ok(void)
{
    /* Legacy diagnostics only; periodic evaluated criteria remain authoritative. */
    portENTER_CRITICAL(&s_health_mux);
    s_criteria.scheduler_healthy = true;
    ota_health_refresh_mask_from_criteria();
    portEXIT_CRITICAL(&s_health_mux);
}

void ota_health_report_nvs_ok(void)
{
    /* Legacy diagnostics only; periodic evaluated criteria remain authoritative. */
    portENTER_CRITICAL(&s_health_mux);
    s_criteria.nvs_healthy = true;
    ota_health_refresh_mask_from_criteria();
    portEXIT_CRITICAL(&s_health_mux);
}

void ota_health_report_twdt_observation(bool fed)
{
    portENTER_CRITICAL(&s_health_mux);
    s_telemetry_twdt_feed_ok = fed;
    portEXIT_CRITICAL(&s_health_mux);
}

void ota_health_report_twdt_ok(void)
{
    /* Legacy diagnostics only; periodic evaluated criteria remain authoritative. */
    portENTER_CRITICAL(&s_health_mux);
    s_criteria.watchdog_healthy = true;
    ota_health_refresh_mask_from_criteria();
    portEXIT_CRITICAL(&s_health_mux);
}

void ota_health_report_zigbee_ok(void)
{
    /* Legacy diagnostics only; periodic evaluated criteria remain authoritative. */
    portENTER_CRITICAL(&s_health_mux);
    s_criteria.zigbee_healthy = true;
    ota_health_refresh_mask_from_criteria();
    portEXIT_CRITICAL(&s_health_mux);
}

void ota_health_report_z906_comm_ok(void)
{
    /* Legacy diagnostics only; periodic evaluated criteria remain authoritative. */
    portENTER_CRITICAL(&s_health_mux);
    s_criteria.z906_comm_healthy = true;
    ota_health_refresh_mask_from_criteria();
    portEXIT_CRITICAL(&s_health_mux);
}

bool ota_health_was_rollback_detected(void)
{
    return s_rollback_detected;
}

bool ota_health_is_validated(void)
{
    portENTER_CRITICAL(&s_health_mux);
    bool validated = s_is_validated;
    portEXIT_CRITICAL(&s_health_mux);
    return validated;
}

void ota_health_set_pending_verify_for_test(bool pending)
{
    s_is_pending_verify = pending;
    s_is_validated = false;
    s_seconds_in_validation = 0;
    s_rollback_handled = false;
    s_finalization_in_progress = false;
    s_validation_start_us = ota_health_now_us();
}

void ota_health_set_time_fn_for_test(ota_health_time_fn_t fn)
{
    s_time_fn_for_test = fn;
}

void ota_health_set_z906_status_time_fn_for_test(ota_health_time_fn_t fn)
{
    s_status_time_fn_for_test = fn;
}

void ota_health_set_zigbee_ready_fn_for_test(ota_health_bool_fn_t fn)
{
    s_zigbee_ready_fn_for_test = fn;
}

void ota_health_set_twdt_healthy_fn_for_test(ota_health_bool_fn_t fn)
{
    s_twdt_healthy_fn_for_test = fn;
}

void ota_health_set_nvs_healthy_fn_for_test(ota_health_bool_fn_t fn)
{
    s_nvs_healthy_fn_for_test = fn;
}

void ota_health_set_fallback_available_fn_for_test(ota_health_bool_fn_t fn)
{
    s_fallback_available_fn_for_test = fn;
}

void ota_health_set_rollback_fn_for_test(ota_rollback_fn_t fn)
{
    s_rollback_fn_for_test = fn;
}

void ota_health_set_mark_valid_fn_for_test(ota_mark_valid_fn_t fn)
{
    s_mark_valid_fn_for_test = fn;
}

void ota_health_reset_for_test(void)
{
    s_is_pending_verify = false;
    s_is_validated = false;
    s_seconds_in_validation = 0;
    s_validation_start_us = 0;
    s_rollback_handled = false;
    s_finalization_in_progress = false;
    s_rollback_detected = false;
    s_time_fn_for_test = NULL;
    s_status_time_fn_for_test = NULL;
    s_zigbee_ready_fn_for_test = NULL;
    s_nvs_healthy_fn_for_test = NULL;
    s_twdt_healthy_fn_for_test = NULL;
    s_telemetry_twdt_feed_ok = false;
    s_fallback_available_fn_for_test = NULL;
    s_rollback_fn_for_test = NULL;
    s_mark_valid_fn_for_test = NULL;
    memset(&s_criteria, 0, sizeof(s_criteria));
    s_passed_criteria_mask = 0;
}
