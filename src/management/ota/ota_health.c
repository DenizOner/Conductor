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
/* Rollback requests during a cancellable admission must never disappear. */
static bool s_rollback_requested_during_admission = false;
static bool s_commit_admitted = false;
/* Preserve operator vs verified-health recovery cause across the handoff. */
static bool s_manual_rollback_requested = false;
static bool s_health_failure_recorded = false;
static ota_health_recovery_state_t s_recovery_state = OTA_HEALTH_RECOVERY_NONE;
/* Short state snapshots only: SDK / NVS / OTA APIs are NEVER called under this spinlock. */
static portMUX_TYPE s_health_mux = portMUX_INITIALIZER_UNLOCKED;
static ota_health_bool_fn_t s_fallback_available_fn_for_test = NULL;
/* A single coherent RX generation is sampled with the five health criteria. */
static uint32_t s_sampled_cable_epoch = 0;
static uint32_t s_sampled_rx_stream_epoch = 0;
/* Test seam: inject a drop after PASS and finalization claim, before admission. */
static ota_health_void_fn_t s_before_final_admission_fn_for_test = NULL;
/* Deterministic interleaving after the first proof check, before SDK handoff. */
static ota_health_void_fn_t s_before_sdk_handoff_fn_for_test = NULL;
/* Test seams define both sides of the short software-admission boundary. */
static ota_health_void_fn_t s_before_atomic_claim_fn_for_test = NULL;
static ota_health_void_fn_t s_after_atomic_claim_fn_for_test = NULL;

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

/* Distinguish actual health failure from an operator's rollback request. */
static void ota_health_note_verified_failure(const char *reason)
{
    portENTER_CRITICAL(&s_health_mux);
    s_health_failure_recorded = true;
    portEXIT_CRITICAL(&s_health_mux);
    ota_client_note_health_failure(reason);
}

bool ota_health_init(void)
{
    s_passed_criteria_mask = 0;
    s_criteria = (ota_health_criteria_t){ 0 };
    s_is_pending_verify = false;
    s_rollback_handled = false;
    s_finalization_in_progress = false;
    s_rollback_requested_during_admission = false;
    s_commit_admitted = false;
    s_manual_rollback_requested = false;
    s_health_failure_recorded = false;
    s_recovery_state = OTA_HEALTH_RECOVERY_NONE;
    s_rollback_detected = false;
    s_sampled_cable_epoch = 0;
    s_sampled_rx_stream_epoch = 0;
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

    /* Read validity, RX generation and matched STATUS from ONE state mutex
     * snapshot. Separate getters can span a drop and produce a mixed epoch. */
    z906_ota_status_snapshot_t rx = { 0 };
    bool have_rx = z906_state_get_ota_status_snapshot(&rx);
    int64_t now_us = ota_health_now_us();
    int64_t status_us = s_status_time_fn_for_test != NULL
        ? s_status_time_fn_for_test() : rx.matched_status_us;
    sample.z906_comm_healthy = have_rx && rx.main_valid &&
        status_us > s_validation_start_us &&
        status_us <= now_us &&
        (now_us - status_us) <= OTA_Z906_STATUS_MAX_AGE_US;

    portENTER_CRITICAL(&s_health_mux);
    s_criteria = sample;
    s_sampled_cable_epoch = rx.cable_epoch;
    s_sampled_rx_stream_epoch = rx.rx_stream_epoch;
    ota_health_refresh_mask_from_criteria();
    portEXIT_CRITICAL(&s_health_mux);
}

/* The state-store mutex remains held across this short health-mux claim.
 * No API is invoked from this callback except the bounded critical section.
 * This makes the last physical proof and software admission one decision. */
typedef struct {
    uint32_t expected_cable_epoch;
    uint32_t expected_rx_epoch;
    int64_t checked_at_us;
    int64_t test_status_us;
    bool use_test_status;
    bool evaluated;
    bool proof_invalid;
    bool rollback_veto;
} ota_atomic_admission_t;

static bool ota_health_claim_locked_proof(const z906_ota_status_snapshot_t *proof,
                                          void *context)
{
    ota_atomic_admission_t *decision = (ota_atomic_admission_t *)context;
    decision->evaluated = true;
    /* The freshness clock must be read INSIDE the state mutex. A preemption
     * before the locked callback must not extend a 5-second proof lease. */
    decision->checked_at_us = ota_health_now_us();
    int64_t status_us = decision->use_test_status
        ? decision->test_status_us : proof->matched_status_us;
    bool proof_ok = proof->main_valid &&
        proof->cable_epoch == decision->expected_cable_epoch &&
        proof->rx_stream_epoch == decision->expected_rx_epoch &&
        status_us > s_validation_start_us &&
        status_us <= decision->checked_at_us &&
        (decision->checked_at_us - status_us) <= OTA_Z906_STATUS_MAX_AGE_US;
    decision->proof_invalid = !proof_ok;

    /* Lock order: state mutex -> health portMUX, never the reverse. */
    portENTER_CRITICAL(&s_health_mux);
    decision->rollback_veto = s_rollback_requested_during_admission;
    bool admitted = proof_ok && !decision->rollback_veto &&
                    s_finalization_in_progress && !s_commit_admitted;
    if (admitted) {
        s_commit_admitted = true; /* Linearization point for product option A. */
    } else {
        s_finalization_in_progress = false;
        s_rollback_requested_during_admission = false;
        /* A human rollback request does not prove a Z906 communication fault. */
        if (!proof_ok) {
            s_criteria.z906_comm_healthy = false;
            ota_health_refresh_mask_from_criteria();
        }
    }
    portEXIT_CRITICAL(&s_health_mux);
    return admitted;
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
            ota_health_note_verified_failure("Health criteria failed during validation window");
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
            s_commit_admitted = false;
            s_rollback_requested_during_admission = false;
        }
        portEXIT_CRITICAL(&s_health_mux);
        if (!claimed) {
            return;
        }

        /* A physical RX loss may preempt between the PASS snapshot and this
         * admission boundary. Test it explicitly without any SDK flash calls
         * under s_health_mux or the state mutex. */
        if (s_before_final_admission_fn_for_test != NULL) {
            s_before_final_admission_fn_for_test();
        }
        z906_ota_status_snapshot_t live = { 0 };
        bool live_read = z906_state_get_ota_status_snapshot(&live);
        int64_t admission_us = ota_health_now_us();
        int64_t live_status_us = s_status_time_fn_for_test != NULL
            ? s_status_time_fn_for_test() : live.matched_status_us;
        bool z906_still_healthy = live_read && live.main_valid &&
            live.cable_epoch == s_sampled_cable_epoch &&
            live.rx_stream_epoch == s_sampled_rx_stream_epoch &&
            live_status_us > s_validation_start_us &&
            live_status_us <= admission_us &&
            (admission_us - live_status_us) <= OTA_Z906_STATUS_MAX_AGE_US;
        if (!z906_still_healthy) {
            portENTER_CRITICAL(&s_health_mux);
            s_criteria.z906_comm_healthy = false;
            ota_health_refresh_mask_from_criteria();
            s_finalization_in_progress = false;
            portEXIT_CRITICAL(&s_health_mux);
            ESP_LOGE(TAG, "Fresh STATUS proof changed after health PASS; refusing OTA approval");
            ota_health_note_verified_failure("Z906 RX generation changed before OTA approval");
            ota_health_trigger_rollback("Z906 RX integrity lost before commit admission");
            return;
        }

        /* A second interleaving point covers manual rollback/RX loss after
         * the first snapshot, but before the final SDK admission claim. */
        if (s_before_sdk_handoff_fn_for_test != NULL) {
            s_before_sdk_handoff_fn_for_test();
        }
        /* 19-K: inject RX loss/manual rollback at the formerly unprotected
         * last-snapshot -> health-claim boundary before taking the state lock. */
        if (s_before_atomic_claim_fn_for_test != NULL) {
            s_before_atomic_claim_fn_for_test();
        }
        ota_atomic_admission_t decision = {
            .expected_cable_epoch = live.cable_epoch,
            .expected_rx_epoch = live.rx_stream_epoch,
            .checked_at_us = 0, /* Populated under the state mutex. */
            .use_test_status = (s_status_time_fn_for_test != NULL),
            .test_status_us = s_status_time_fn_for_test != NULL
                ? s_status_time_fn_for_test() : 0,
            .evaluated = false,
            .proof_invalid = true,
            .rollback_veto = false
        };
        bool admitted = z906_state_with_locked_ota_snapshot(
            ota_health_claim_locked_proof, &decision);
        if (!admitted) {
            /* On a missing state service the callback could not clear claim
             * state. Fail closed; do not infer a physical communication PASS. */
            if (!decision.evaluated) {
                portENTER_CRITICAL(&s_health_mux);
                s_finalization_in_progress = false;
                s_rollback_requested_during_admission = false;
                s_criteria.z906_comm_healthy = false;
                ota_health_refresh_mask_from_criteria();
                portEXIT_CRITICAL(&s_health_mux);
            }
            if (decision.proof_invalid) {
                ota_health_note_verified_failure("Z906 proof lost before atomic OTA admission");
            } else if (decision.rollback_veto) {
                ESP_LOGW(TAG, "Operator rollback vetoed OTA admission; Z906 proof remains healthy");
            }
            ota_health_trigger_rollback("Rollback veto or Z906 proof invalid before admission");
            return;
        }

        /* Test-only preemption AFTER successful software admission. A subsequent
         * rollback is TOO_LATE by the product-approved boundary, even if the
         * ESP-IDF function has not yet been entered. */
        if (s_after_atomic_claim_fn_for_test != NULL) {
            s_after_atomic_claim_fn_for_test();
        }
        /* SDK runs outside all locks; no guarantee extends to SDK entry. */
        esp_err_t err = s_mark_valid_fn_for_test != NULL
            ? s_mark_valid_fn_for_test()
            : esp_ota_mark_app_valid_cancel_rollback();
        portENTER_CRITICAL(&s_health_mux);
        if (err == ESP_OK) {
            s_is_validated = true;
            s_is_pending_verify = false;
        }
        bool too_late_rollback = s_recovery_state == OTA_HEALTH_RECOVERY_REQUEST_TOO_LATE;
        s_finalization_in_progress = false;
        s_commit_admitted = false;
        s_rollback_requested_during_admission = false;
        portEXIT_CRITICAL(&s_health_mux);
        if (too_late_rollback) {
            ESP_LOGE(TAG, "Rollback requested after OTA admission; manual recovery decision required");
        }
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

/* Manual provenance and the rollback veto MUST be recorded under ONE mux.
 * Otherwise telemetry could admit between setting the manual flag and queueing
 * the request, misclassifying a pre-admission rollback as TOO_LATE. */
static void ota_health_trigger_rollback_impl(const char *reason, bool manual)
{
    portENTER_CRITICAL(&s_health_mux);
    bool already_handled = s_rollback_handled;
    if (manual && !already_handled) {
        s_manual_rollback_requested = true;
    }
    bool queued = false;
    bool too_late = false;
    if (!already_handled && s_finalization_in_progress) {
        if (!s_commit_admitted) {
            s_rollback_requested_during_admission = true;
            s_recovery_state = OTA_HEALTH_RECOVERY_ROLLBACK_QUEUED;
            queued = true;
        } else {
            s_recovery_state = OTA_HEALTH_RECOVERY_REQUEST_TOO_LATE;
            too_late = true;
        }
    } else if (!already_handled) {
        s_rollback_handled = true;
    }
    portEXIT_CRITICAL(&s_health_mux);
    if (already_handled) {
        return;
    }
    if (queued) {
        ESP_LOGW(TAG, "Rollback queued: auto OTA approval must abort before mark-valid");
        return;
    }
    if (too_late) {
        ESP_LOGE(TAG, "Rollback requested after irreversible OTA admission; cannot undo SDK write");
        return;
    }
    ESP_LOGW(TAG, "OTA rollback requested: %s", reason ? reason : "Unknown");
    led_notification_post_cmd_result(false);

    bool fallback_available = s_fallback_available_fn_for_test != NULL
        ? s_fallback_available_fn_for_test()
        : esp_ota_check_rollback_is_possible();
    if (!fallback_available) {
        /* No unbootable restart loop. The bootloader may still reject this
         * pending image on a later reboot: do not promise permanent safety. */
        portENTER_CRITICAL(&s_health_mux);
        s_recovery_state = OTA_HEALTH_RECOVERY_NO_FALLBACK;
        portEXIT_CRITICAL(&s_health_mux);
        ESP_LOGE(TAG, "No valid fallback; pending image remains unconfirmed; recovery required");
        return;
    }

    portENTER_CRITICAL(&s_health_mux);
    s_recovery_state = OTA_HEALTH_RECOVERY_REQUESTED;
    portEXIT_CRITICAL(&s_health_mux);

    if (s_rollback_fn_for_test != NULL) {
        s_rollback_fn_for_test(reason);
        return;
    }

    esp_err_t err = esp_ota_mark_app_invalid_rollback_and_reboot();
    if (err != ESP_OK) {
        portENTER_CRITICAL(&s_health_mux);
        s_recovery_state = OTA_HEALTH_RECOVERY_ROLLBACK_ERROR;
        portEXIT_CRITICAL(&s_health_mux);
        ESP_LOGE(TAG, "Rollback rejected: %s; pending image requires manual recovery",
                 esp_err_to_name(err));
    }
}

void ota_health_trigger_rollback(const char *reason)
{
    ota_health_trigger_rollback_impl(reason, false);
}

void ota_health_trigger_manual_rollback(const char *reason)
{
    ota_health_trigger_rollback_impl(reason, true);
}

bool ota_health_was_manual_only_rollback(void)
{
    portENTER_CRITICAL(&s_health_mux);
    bool manual_only = s_manual_rollback_requested && !s_health_failure_recorded;
    portEXIT_CRITICAL(&s_health_mux);
    return manual_only;
}

ota_health_recovery_state_t ota_health_get_recovery_state(void)
{
    portENTER_CRITICAL(&s_health_mux);
    ota_health_recovery_state_t state = s_recovery_state;
    portEXIT_CRITICAL(&s_health_mux);
    return state;
}

const char *ota_health_recovery_state_str(ota_health_recovery_state_t state)
{
    switch (state) {
        case OTA_HEALTH_RECOVERY_NONE:           return "NONE";
        case OTA_HEALTH_RECOVERY_REQUESTED:      return "ROLLBACK_REQUESTED";
        case OTA_HEALTH_RECOVERY_NO_FALLBACK:    return "NO_VALID_FALLBACK";
        case OTA_HEALTH_RECOVERY_ROLLBACK_ERROR: return "ROLLBACK_API_ERROR";
        case OTA_HEALTH_RECOVERY_ROLLBACK_QUEUED: return "ROLLBACK_QUEUED";
        case OTA_HEALTH_RECOVERY_REQUEST_TOO_LATE: return "ROLLBACK_TOO_LATE";
        default:                                return "UNKNOWN";
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
    s_rollback_requested_during_admission = false;
    s_commit_admitted = false;
    s_manual_rollback_requested = false;
    s_health_failure_recorded = false;
    s_recovery_state = OTA_HEALTH_RECOVERY_NONE;
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

void ota_health_set_before_final_admission_fn_for_test(ota_health_void_fn_t fn)
{
    s_before_final_admission_fn_for_test = fn;
}

void ota_health_set_before_sdk_handoff_fn_for_test(ota_health_void_fn_t fn)
{
    s_before_sdk_handoff_fn_for_test = fn;
}

void ota_health_set_before_atomic_claim_fn_for_test(ota_health_void_fn_t fn)
{
    s_before_atomic_claim_fn_for_test = fn;
}

void ota_health_set_after_atomic_claim_fn_for_test(ota_health_void_fn_t fn)
{
    s_after_atomic_claim_fn_for_test = fn;
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
    s_rollback_requested_during_admission = false;
    s_commit_admitted = false;
    s_manual_rollback_requested = false;
    s_health_failure_recorded = false;
    s_recovery_state = OTA_HEALTH_RECOVERY_NONE;
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
    s_before_final_admission_fn_for_test = NULL;
    s_before_sdk_handoff_fn_for_test = NULL;
    s_before_atomic_claim_fn_for_test = NULL;
    s_after_atomic_claim_fn_for_test = NULL;
    s_sampled_cable_epoch = 0;
    s_sampled_rx_stream_epoch = 0;
    memset(&s_criteria, 0, sizeof(s_criteria));
    s_passed_criteria_mask = 0;
}
