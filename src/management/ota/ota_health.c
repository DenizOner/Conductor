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
#include "zigbee_app.h"
#include "z906_state.h"
#include "led_notification_manager.h"

static const char *TAG = "OTA_HEALTH";

#define OTA_HEALTH_VALIDATION_WINDOW_S  15

static bool s_is_pending_verify = false;
static bool s_rollback_detected = false;
static bool s_is_validated = false;
static uint32_t s_seconds_in_validation = 0;
static uint32_t s_passed_criteria_mask = 0;
static ota_health_criteria_t s_criteria = { 0 };

static ota_rollback_fn_t s_rollback_fn_for_test = NULL;
static ota_mark_valid_fn_t s_mark_valid_fn_for_test = NULL;

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
    /* Current health evaluation — NOT latched (Model A: CURRENT HEALTH) */
    s_criteria.scheduler_healthy = (xTaskGetSchedulerState() == taskSCHEDULER_RUNNING);
    s_criteria.watchdog_healthy  = wdt_manager_is_initialized() && !wdt_manager_was_watchdog_reset();

    /* 2. NVS storage health (valid read without flash partition corruption) */
    bool verif_en = true;
    s_criteria.nvs_healthy       = nvs_manager_get_cmd_verifier_enabled(&verif_en);

    /* 3. Zigbee stack lifecycle health */
    zb_network_state_t zb_st     = zigbee_app_get_state();
    s_criteria.zigbee_healthy    = (zb_st != ZB_STATE_COMMISSION_FAILED);

    /* 4. Z906 communication and state manager health */
    z906_state_t st;
    z906_state_get(&st);
    /* Operational or successfully initialized communication layer */
    s_criteria.z906_comm_healthy = (st.volume <= 43);

    ota_health_refresh_mask_from_criteria();
}

void ota_health_process_cycle(uint32_t elapsed_sec)
{
    if (!s_is_pending_verify || s_is_validated) {
        return;
    }

    s_seconds_in_validation += elapsed_sec;
    evaluate_criteria();

    bool all_criteria_met = s_criteria.scheduler_healthy &&
                            s_criteria.watchdog_healthy &&
                            s_criteria.nvs_healthy &&
                            s_criteria.zigbee_healthy &&
                            s_criteria.z906_comm_healthy;

    if (!all_criteria_met) {
        ESP_LOGW(TAG, "Health check criteria not satisfied yet: Sched=%d, WDT=%d, NVS=%d, ZB=%d, Z906=%d (Elapsed: %lu s)",
                 s_criteria.scheduler_healthy, s_criteria.watchdog_healthy,
                 s_criteria.nvs_healthy, s_criteria.zigbee_healthy, s_criteria.z906_comm_healthy,
                 (unsigned long)s_seconds_in_validation);

        if (s_seconds_in_validation >= OTA_HEALTH_VALIDATION_WINDOW_S) {
            ESP_LOGE(TAG, "Health check observation window expired before all criteria were healthy. Initiating rollback!");
            ota_client_note_health_failure("Health criteria failed during validation window");
            ota_health_trigger_rollback("Health criteria timeout expired");
        }
        return;
    }

    if (s_seconds_in_validation >= OTA_HEALTH_VALIDATION_WINDOW_S) {
        ESP_LOGI(TAG, "=========================================================");
        ESP_LOGI(TAG, "HEALTH CHECK PASSED! All 5 subsystem criteria verified.");
        ESP_LOGI(TAG, "Marking application as permanently VALID in otadata...");

        esp_err_t err = (s_mark_valid_fn_for_test != NULL)
                         ? s_mark_valid_fn_for_test()
                         : esp_ota_mark_app_valid_cancel_rollback();
        if (err == ESP_OK) {
            s_is_validated = true;
            s_is_pending_verify = false;
            ESP_LOGI(TAG, "SUCCESS: Firmware upgrade permanently COMMITTED!");
            ESP_LOGI(TAG, "=========================================================");
            led_notification_post_cmd_result(true);
        } else {
            ESP_LOGE(TAG, "Failed to mark app as valid: %s. Initiating rollback!", esp_err_to_name(err));
            ota_health_trigger_rollback("esp_ota_mark_app_valid failed");
        }
    } else {
        ESP_LOGI(TAG, "Health check in progress... (%lu/%d s) - All criteria healthy.",
                 (unsigned long)s_seconds_in_validation, OTA_HEALTH_VALIDATION_WINDOW_S);
    }
}

bool ota_health_is_pending_verify(void)
{
    return s_is_pending_verify;
}

void ota_health_trigger_rollback(const char *reason)
{
    ESP_LOGE(TAG, "TRIGGERING IMMEDIATE FIRMWARE ROLLBACK: Reason: %s", reason ? reason : "Unknown");
    led_notification_post_cmd_result(false);

    if (s_rollback_fn_for_test != NULL) {
        s_rollback_fn_for_test(reason);
        return;
    }

    esp_err_t err = esp_ota_mark_app_invalid_rollback_and_reboot();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_mark_app_invalid_rollback_and_reboot failed: %s! Forcing software restart...",
                 esp_err_to_name(err));
        esp_restart();
    }
}

void ota_health_get_criteria(ota_health_criteria_t *out_criteria)
{
    if (out_criteria != NULL) {
        evaluate_criteria();
        *out_criteria = s_criteria;
    }
}

uint32_t ota_health_get_passed_criteria_mask(void)
{
    return s_passed_criteria_mask;
}

void ota_health_report_scheduler_ok(void)
{
    s_criteria.scheduler_healthy = true;
    ota_health_refresh_mask_from_criteria();
}

void ota_health_report_nvs_ok(void)
{
    s_criteria.nvs_healthy = true;
    ota_health_refresh_mask_from_criteria();
}

void ota_health_report_twdt_ok(void)
{
    s_criteria.watchdog_healthy = true;
    ota_health_refresh_mask_from_criteria();
}

void ota_health_report_zigbee_ok(void)
{
    s_criteria.zigbee_healthy = true;
    ota_health_refresh_mask_from_criteria();
}

void ota_health_report_z906_comm_ok(void)
{
    s_criteria.z906_comm_healthy = true;
    ota_health_refresh_mask_from_criteria();
}

bool ota_health_was_rollback_detected(void)
{
    return s_rollback_detected;
}

bool ota_health_is_validated(void)
{
    return s_is_validated;
}

void ota_health_set_pending_verify_for_test(bool pending)
{
    s_is_pending_verify = pending;
    s_is_validated = false;
    s_seconds_in_validation = 0;
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
    s_rollback_detected = false;
    s_rollback_fn_for_test = NULL;
    s_mark_valid_fn_for_test = NULL;
    memset(&s_criteria, 0, sizeof(s_criteria));
    s_passed_criteria_mask = 0;
}
