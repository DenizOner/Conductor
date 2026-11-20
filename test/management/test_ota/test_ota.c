/**
 * @file test_ota.c
 * @brief Unit tests for Native Zigbee OTA Client, Image Header Verification, Health Check Logic,
 *        and Failure-Path / Rollback Transitions (Model A: CURRENT HEALTH).
 */

#include <unity.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "ota_types.h"
#include "ota_client.h"
#include "ota_health.h"
#include "zigbee_app.h"
#include "z906_state.h"
#include "nvs_manager.h"
#include "wdt_manager.h"
#include "config.h"
#include "esp_ota_ops.h"
#include <string.h>

static int s_rollback_call_count = 0;
static int s_mark_valid_call_count = 0;
static bool s_inject_rollback_during_sdk_call = false;
static esp_err_t s_mark_valid_result = ESP_OK;
static int64_t s_delay_before_critical_us = 0;
static const char *s_last_rollback_reason = NULL;
static int64_t s_fake_time_us = 1000000;
static bool s_fake_zigbee_ready = true;
static bool s_fake_nvs_ok = true;
static bool s_fake_twdt_ok = true;
static bool s_fake_fallback_available = true;
static bool s_auto_status_refresh = false;
static unsigned s_status_tick_counter = 0;
static int64_t s_fake_status_time_us = 0;

static int64_t mock_health_time(void) { return s_fake_time_us; }
static int64_t mock_status_time(void) { return s_fake_status_time_us; }
static bool mock_zigbee_ready(void) { return s_fake_zigbee_ready; }
static bool mock_nvs_ok(void) { return s_fake_nvs_ok; }
static bool mock_twdt_ok(void) { return s_fake_twdt_ok; }
static bool mock_fallback_available(void) { return s_fake_fallback_available; }

static void advance_health_second(void)
{
    s_fake_time_us += 1000000;
    if (s_auto_status_refresh && ((s_status_tick_counter++ % 4U) == 0U)) {
        /* Simulate one verified CONFIG frame for each 4-second OTA STATUS poll. */
        z906_state_set_main_valid(true);
        s_fake_status_time_us = s_fake_time_us;
    }
    ota_health_process_cycle(1);
}

/* Explicitly enter this test's pending client state instead of relying on
 * static ota_client state retained from the preceding Unity test. */
static void prime_ota_client_pending_validation(void)
{
    ota_client_process_cycle();
    TEST_ASSERT_EQUAL(OTA_STATE_PENDING_VALIDATION, ota_client_get_state());
}

static void mock_rollback_fn(const char *reason)
{
    s_rollback_call_count++;
    s_last_rollback_reason = reason;
}

static esp_err_t mock_mark_valid_ok(void)
{
    ++s_mark_valid_call_count;
    if (s_inject_rollback_during_sdk_call) {
        ota_health_trigger_manual_rollback("Concurrent CLI rollback inside SDK stub");
    }
    return s_mark_valid_result;
}

static void inject_rx_drop_at_final_admission(void)
{
    uint32_t cable = z906_state_get_cable_epoch();
    (void)z906_state_note_amp_rx_loss(cable);
    s_fake_status_time_us = 0;
}
static void request_rollback_at_sdk_handoff(void)
{
    ota_health_trigger_manual_rollback("CLI rollback before SDK admission");
}
static void request_rollback_after_atomic_claim(void)
{
    ota_health_trigger_manual_rollback("CLI rollback after software claim before SDK entry");
}
static void manual_rollback_then_rx_drop_before_claim(void)
{
    ota_health_trigger_manual_rollback("Concurrent CLI rollback before OTA claim");
    inject_rx_drop_at_final_admission();
}
static void expire_status_before_atomic_claim(void)
{
    /* Longer preemption cannot allow an expired matched STATUS to be accepted. */
    s_fake_time_us += 6000000;
}
static void advance_clock_inside_state_mutex_before_critical(void)
{
    s_fake_time_us += s_delay_before_critical_us;
}

void setUp(void)
{
    nvs_manager_init();
    wdt_manager_init(10);
    z906_state_init();
    z906_state_set_cable_status(true);
    z906_state_set_power(true);
    zigbee_app_reset_state();
    ota_health_reset_for_test();
    s_fake_time_us = 1000000;
    s_fake_zigbee_ready = true;
    s_fake_nvs_ok = true;
    s_fake_twdt_ok = true;
    s_fake_fallback_available = true;
    s_auto_status_refresh = false;
    s_status_tick_counter = 0;
    s_fake_status_time_us = 0;
    ota_health_set_time_fn_for_test(mock_health_time);
    ota_health_set_z906_status_time_fn_for_test(mock_status_time);
    ota_health_set_zigbee_ready_fn_for_test(mock_zigbee_ready);
    ota_health_set_nvs_healthy_fn_for_test(mock_nvs_ok);
    ota_health_set_twdt_healthy_fn_for_test(mock_twdt_ok);
    ota_health_set_fallback_available_fn_for_test(mock_fallback_available);
    s_rollback_call_count = 0;
    s_mark_valid_call_count = 0;
    s_inject_rollback_during_sdk_call = false;
    s_mark_valid_result = ESP_OK;
    s_delay_before_critical_us = 0;
    s_last_rollback_reason = NULL;
}

void tearDown(void)
{
    ota_health_reset_for_test();
    zigbee_app_reset_state();
}

/**
 * @brief Tests OTA client state machine string conversion.
 */
void test_ota_state_strings(void)
{
    TEST_ASSERT_EQUAL_STRING("IDLE", ota_client_state_str(OTA_STATE_IDLE));
    TEST_ASSERT_EQUAL_STRING("CHECKING", ota_client_state_str(OTA_STATE_CHECKING));
    TEST_ASSERT_EQUAL_STRING("AVAILABLE", ota_client_state_str(OTA_STATE_AVAILABLE));
    TEST_ASSERT_EQUAL_STRING("DOWNLOADING", ota_client_state_str(OTA_STATE_DOWNLOADING));
    TEST_ASSERT_EQUAL_STRING("VERIFYING", ota_client_state_str(OTA_STATE_VERIFYING));
    TEST_ASSERT_EQUAL_STRING("READY_TO_REBOOT", ota_client_state_str(OTA_STATE_READY_TO_REBOOT));
    TEST_ASSERT_EQUAL_STRING("PENDING_VALIDATION", ota_client_state_str(OTA_STATE_PENDING_VALIDATION));
    TEST_ASSERT_EQUAL_STRING("SUCCESS", ota_client_state_str(OTA_STATE_SUCCESS));
    TEST_ASSERT_EQUAL_STRING("FAILED", ota_client_state_str(OTA_STATE_FAILED));
    TEST_ASSERT_EQUAL_STRING("ROLLBACK", ota_client_state_str(OTA_STATE_ROLLBACK));
}

/**
 * @brief Tests OTA result string conversion.
 */
void test_ota_result_strings(void)
{
    TEST_ASSERT_EQUAL_STRING("NONE", ota_client_result_str(OTA_RESULT_NONE));
    TEST_ASSERT_EQUAL_STRING("SUCCESS", ota_client_result_str(OTA_RESULT_SUCCESS));
    TEST_ASSERT_EQUAL_STRING("DOWNLOAD_FAILED", ota_client_result_str(OTA_RESULT_DOWNLOAD_FAILED));
    TEST_ASSERT_EQUAL_STRING("VALIDATION_FAILED", ota_client_result_str(OTA_RESULT_VALIDATION_FAILED));
    TEST_ASSERT_EQUAL_STRING("INCOMPATIBLE_IMAGE", ota_client_result_str(OTA_RESULT_INCOMPATIBLE_IMAGE));
    TEST_ASSERT_EQUAL_STRING("FLASH_FAILED", ota_client_result_str(OTA_RESULT_FLASH_FAILED));
    TEST_ASSERT_EQUAL_STRING("HEALTH_CHECK_FAILED", ota_client_result_str(OTA_RESULT_HEALTH_CHECK_FAILED));
    TEST_ASSERT_EQUAL_STRING("ROLLBACK_DETECTED", ota_client_result_str(OTA_RESULT_ROLLBACK_DETECTED));
    TEST_ASSERT_EQUAL_STRING("MANUAL_ROLLBACK_REQUESTED", ota_client_result_str(OTA_RESULT_MANUAL_ROLLBACK_REQUESTED));
    TEST_ASSERT_EQUAL_STRING("SDK_MARK_VALID_FAILED", ota_client_result_str(OTA_RESULT_SDK_MARK_VALID_FAILED));
    /* Append-only OTA diagnostics: previous result identities stay stable. */
    TEST_ASSERT_EQUAL_INT(0, OTA_RESULT_NONE);
    TEST_ASSERT_EQUAL_INT(1, OTA_RESULT_SUCCESS);
    TEST_ASSERT_EQUAL_INT(2, OTA_RESULT_DOWNLOAD_FAILED);
    TEST_ASSERT_EQUAL_INT(3, OTA_RESULT_VALIDATION_FAILED);
    TEST_ASSERT_EQUAL_INT(4, OTA_RESULT_INCOMPATIBLE_IMAGE);
    TEST_ASSERT_EQUAL_INT(5, OTA_RESULT_FLASH_FAILED);
    TEST_ASSERT_EQUAL_INT(6, OTA_RESULT_HEALTH_CHECK_FAILED);
    TEST_ASSERT_EQUAL_INT(7, OTA_RESULT_ROLLBACK_DETECTED);
    TEST_ASSERT_EQUAL_INT(8, OTA_RESULT_MANUAL_ROLLBACK_REQUESTED);
    TEST_ASSERT_EQUAL_INT(9, OTA_RESULT_SDK_MARK_VALID_FAILED);
}

/**
 * @brief Verifies ota_client_handle_image_notify() schedules a query.
 */
void test_ota_incompatible_image_rejection(void)
{
    ota_client_init();
    TEST_ASSERT_EQUAL(OTA_STATE_IDLE, ota_client_get_state());

    ota_client_handle_image_notify(
        0,
        0,
        Z906_OTA_MANUFACTURER_CODE,
        Z906_OTA_IMAGE_TYPE,
        Z906_OTA_FILE_VERSION + 1
    );

    TEST_ASSERT_EQUAL(OTA_STATE_CHECKING, ota_client_get_state());

    ota_client_abort("test cleanup");
    TEST_ASSERT_EQUAL(OTA_STATE_IDLE, ota_client_get_state());
}

/**
 * @brief Tests canonical Zigbee OTA header data structure size and alignment.
 */
void test_zigbee_ota_header_structure(void)
{
    TEST_ASSERT_EQUAL_UINT32(56, sizeof(zigbee_ota_header_t));

    zigbee_ota_header_t hdr;
    memset(&hdr, 0, sizeof(hdr));
    hdr.upgrade_file_id = ZIGBEE_OTA_FILE_IDENTIFIER;
    hdr.header_version = ZIGBEE_OTA_HEADER_VERSION;
    hdr.header_length = 56;
    hdr.manufacturer_code = Z906_OTA_MANUFACTURER_CODE;
    hdr.image_type = Z906_OTA_IMAGE_TYPE;
    hdr.file_version = Z906_OTA_FILE_VERSION;

    TEST_ASSERT_EQUAL_HEX32(ZIGBEE_OTA_FILE_IDENTIFIER, hdr.upgrade_file_id);
    TEST_ASSERT_EQUAL_HEX16(0x0100, hdr.header_version);
    TEST_ASSERT_EQUAL_UINT16(56, hdr.header_length);
    TEST_ASSERT_EQUAL_HEX16(0x131B, hdr.manufacturer_code);
    TEST_ASSERT_EQUAL_HEX16(0x0906, hdr.image_type);
    TEST_ASSERT_EQUAL_HEX32(0x010400AA, hdr.file_version);
}

/**
 * @brief Tests Health Check subsystem initial state and criteria reporting.
 */
void test_ota_health_check_subsystem(void)
{
    ota_health_init();
    TEST_ASSERT_FALSE(ota_health_is_pending_verify());

    ota_health_criteria_t criteria;
    ota_health_get_criteria(&criteria);
    TEST_ASSERT_TRUE(criteria.scheduler_healthy);
}

/**
 * @brief Tests Diagnostics structure population.
 */
void test_ota_diagnostics_retrieval(void)
{
    ota_diagnostics_t diag;
    memset(&diag, 0, sizeof(diag));
    ota_client_get_diagnostics(&diag);

    TEST_ASSERT_EQUAL_HEX32(Z906_OTA_FILE_VERSION, diag.current_file_version);
    TEST_ASSERT_NOT_NULL(diag.running_partition_name);
    TEST_ASSERT_NOT_NULL(diag.target_partition_name);
}

/**
 * @brief Tests OTA client explicit failure note recording and state reflection.
 */
void test_ota_health_failure_recording(void)
{
    ota_client_init();
    TEST_ASSERT_EQUAL(OTA_STATE_IDLE, ota_client_get_state());

    ota_client_note_health_failure("Subsystem validation failed during health observation window");
    TEST_ASSERT_EQUAL(OTA_STATE_FAILED, ota_client_get_state());
    TEST_ASSERT_EQUAL(OTA_RESULT_HEALTH_CHECK_FAILED, ota_client_get_last_result());

    ota_client_abort("Reset after health failure test");
    TEST_ASSERT_EQUAL(OTA_STATE_IDLE, ota_client_get_state());
}

/**
 * @brief Tests OTA criteria bitmask manipulation and individual subsystem reporting.
 */
void test_ota_health_criteria_reporting_and_mask(void)
{
    ota_health_init();

    ota_health_report_scheduler_ok();
    ota_health_report_twdt_ok();
    ota_health_report_nvs_ok();
    ota_health_report_zigbee_ok();
    ota_health_report_z906_comm_ok();

    uint32_t mask = ota_health_get_passed_criteria_mask();
    TEST_ASSERT_TRUE((mask & OTA_CRITERION_SCHEDULER) != 0);
    TEST_ASSERT_TRUE((mask & OTA_CRITERION_TWDT) != 0);
    TEST_ASSERT_TRUE((mask & OTA_CRITERION_NVS) != 0);
    TEST_ASSERT_TRUE((mask & OTA_CRITERION_ZIGBEE) != 0);
    TEST_ASSERT_TRUE((mask & OTA_CRITERION_Z906_COMM) != 0);
    TEST_ASSERT_EQUAL_HEX32(OTA_ALL_CRITERIA_PASS_MASK, mask & OTA_ALL_CRITERIA_PASS_MASK);
}

/* ========================================================================= */
/* TC-15 & TC-16: Model A (CURRENT HEALTH) Transition Regression Tests       */
/* ========================================================================= */

/**
 * @brief Transition A: healthy -> healthy throughout entire window.
 * Image must be committed permanently (validated=true, no rollback).
 */
void test_ota_health_transition_a_healthy_throughout_validates(void)
{
    ota_health_set_rollback_fn_for_test(mock_rollback_fn);
    ota_health_set_mark_valid_fn_for_test(mock_mark_valid_ok);
    s_auto_status_refresh = true;
    ota_health_set_pending_verify_for_test(true);

    /* 15 one-second evaluation cycles while all subsystems are healthy */
    for (int i = 0; i < 15; i++) {
        advance_health_second();
    }

    TEST_ASSERT_EQUAL_INT(0, s_rollback_call_count);
    TEST_ASSERT_TRUE_MESSAGE(ota_health_is_validated(),
        "Transition A: Firmware must be permanently committed when healthy throughout");
    TEST_ASSERT_FALSE(ota_health_is_pending_verify());
}

/**
 * @brief Transition B: healthy -> unhealthy (Model A critical invariant).
 * In Model A, criteria are NOT latched. Subsystem degradation during window
 * must cause health failure and trigger automatic rollback!
 */
void test_ota_health_transition_b_healthy_to_unhealthy_triggers_rollback(void)
{
    ota_health_set_rollback_fn_for_test(mock_rollback_fn);
    ota_health_set_mark_valid_fn_for_test(mock_mark_valid_ok);
    s_auto_status_refresh = true;
    ota_health_set_pending_verify_for_test(true);

    /* 5 seconds healthy */
    for (int i = 0; i < 5; i++) {
        advance_health_second();
    }
    TEST_ASSERT_EQUAL_INT(0, s_rollback_call_count);

    /* Native Zigbee stack readiness fails at second 6. */
    s_fake_zigbee_ready = false;

    /* Remaining 10 seconds */
    for (int i = 0; i < 10; i++) {
        advance_health_second();
    }

    /* Model A INVARIANT: Must NOT commit! Must trigger rollback! */
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, s_rollback_call_count,
        "Transition B: Subsystem failure during observation window MUST trigger rollback");
    TEST_ASSERT_FALSE_MESSAGE(ota_health_is_validated(),
        "Firmware must NOT be validated when subsystem fails");
    TEST_ASSERT_EQUAL(OTA_HEALTH_RECOVERY_REQUESTED,
                      ota_health_get_recovery_state());
}

/**
 * @brief Transition C: unhealthy -> healthy (recovers before window expires).
 * If a transient fault clears and all criteria are healthy at window completion,
 * firmware validates successfully.
 */
void test_ota_health_transition_c_unhealthy_to_healthy_validates(void)
{
    ota_health_set_rollback_fn_for_test(mock_rollback_fn);
    ota_health_set_mark_valid_fn_for_test(mock_mark_valid_ok);
    s_auto_status_refresh = true;
    ota_health_set_pending_verify_for_test(true);

    /* Start with transient fault */
    s_fake_zigbee_ready = false;

    /* 5 seconds in transient fault */
    for (int i = 0; i < 5; i++) {
        advance_health_second();
    }
    TEST_ASSERT_EQUAL_INT(0, s_rollback_call_count);

    /* Native Zigbee mainloop recovers at second 6. */
    s_fake_zigbee_ready = true;

    /* Complete window (remaining 10 seconds) */
    for (int i = 0; i < 10; i++) {
        advance_health_second();
    }

    TEST_ASSERT_EQUAL_INT(0, s_rollback_call_count);
    TEST_ASSERT_TRUE_MESSAGE(ota_health_is_validated(),
        "Transition C: Recovery before window expiration must allow valid commit");
}

/**
 * @brief Transition D: unhealthy throughout entire observation window.
 * Must trigger automatic rollback at expiration!
 */
void test_ota_health_transition_d_unhealthy_throughout_triggers_rollback(void)
{
    ota_health_set_rollback_fn_for_test(mock_rollback_fn);
    ota_health_set_mark_valid_fn_for_test(mock_mark_valid_ok);
    s_auto_status_refresh = true;
    ota_health_set_pending_verify_for_test(true);

    /* Remains in failed state throughout */
    s_fake_zigbee_ready = false;

    for (int i = 0; i < 15; i++) {
        advance_health_second();
    }

    TEST_ASSERT_EQUAL_INT(1, s_rollback_call_count);
    TEST_ASSERT_FALSE(ota_health_is_validated());
}

/** @brief No amplifier CONFIG frame must never approve an OTA image. */
void test_ota_pending_without_verified_status_rolls_back(void)
{
    ota_health_set_rollback_fn_for_test(mock_rollback_fn);
    ota_health_set_mark_valid_fn_for_test(mock_mark_valid_ok);
    ota_health_set_pending_verify_for_test(true);
    for (int i = 0; i < 15; i++) {
        advance_health_second();
    }
    TEST_ASSERT_EQUAL_INT(1, s_rollback_call_count);
    TEST_ASSERT_FALSE(ota_health_is_validated());
}

/** @brief A lost DE-15 cable revokes a previously verified state. */
void test_ota_cable_loss_revokes_health(void)
{
    ota_health_set_rollback_fn_for_test(mock_rollback_fn);
    ota_health_set_mark_valid_fn_for_test(mock_mark_valid_ok);
    s_auto_status_refresh = true;
    ota_health_set_pending_verify_for_test(true);
    advance_health_second();
    s_auto_status_refresh = false;
    z906_state_set_cable_status(false);
    for (int i = 1; i < 15; i++) {
        advance_health_second();
    }
    TEST_ASSERT_EQUAL_INT(1, s_rollback_call_count);
    TEST_ASSERT_FALSE(ota_health_is_validated());
}

/** @brief Monotonic time detects a stalled task despite one callback invocation. */
void test_ota_single_late_tick_cannot_bypass_validation(void)
{
    ota_health_set_rollback_fn_for_test(mock_rollback_fn);
    ota_health_set_mark_valid_fn_for_test(mock_mark_valid_ok);
    ota_health_set_pending_verify_for_test(true);
    s_fake_time_us += 16000000;
    ota_health_process_cycle(1);
    TEST_ASSERT_EQUAL_INT(1, s_rollback_call_count);
    TEST_ASSERT_FALSE(ota_health_is_validated());
}

/** @brief Without a known-good fallback, never force a restart loop. */
void test_ota_without_fallback_stays_pending(void)
{
    s_fake_fallback_available = false;
    ota_health_set_rollback_fn_for_test(mock_rollback_fn);
    ota_health_set_pending_verify_for_test(true);
    s_fake_time_us += 16000000;
    ota_health_process_cycle(1);
    ota_health_process_cycle(1);
    TEST_ASSERT_EQUAL_INT(0, s_rollback_call_count);
    TEST_ASSERT_FALSE(ota_health_is_validated());
    TEST_ASSERT_TRUE(ota_health_is_pending_verify());
    TEST_ASSERT_EQUAL(OTA_HEALTH_RECOVERY_NO_FALLBACK,
                      ota_health_get_recovery_state());
    ota_client_process_cycle();
    TEST_ASSERT_EQUAL(OTA_STATE_FAILED, ota_client_get_state());
    TEST_ASSERT_EQUAL(OTA_RESULT_HEALTH_CHECK_FAILED, ota_client_get_last_result());
}

/** @brief An authenticated CONFIG frame during the window permits validation. */
void test_ota_status_arrives_during_window(void)
{
    ota_health_set_rollback_fn_for_test(mock_rollback_fn);
    ota_health_set_mark_valid_fn_for_test(mock_mark_valid_ok);
    ota_health_set_pending_verify_for_test(true);
    for (int i = 0; i < 5; i++) {
        advance_health_second();
    }
    s_auto_status_refresh = true;
    for (int i = 5; i < 15; i++) {
        advance_health_second();
    }
    TEST_ASSERT_EQUAL_INT(0, s_rollback_call_count);
    TEST_ASSERT_TRUE(ota_health_is_validated());
}

/** @brief The first CONFIG frame cannot be reused after communications stop. */
void test_ota_stale_status_does_not_approve_image(void)
{
    ota_health_set_rollback_fn_for_test(mock_rollback_fn);
    ota_health_set_mark_valid_fn_for_test(mock_mark_valid_ok);
    ota_health_set_pending_verify_for_test(true);
    s_auto_status_refresh = true;
    advance_health_second();
    s_auto_status_refresh = false;
    for (int i = 1; i < 15; i++) {
        advance_health_second();
    }
    TEST_ASSERT_EQUAL_INT(1, s_rollback_call_count);
    TEST_ASSERT_FALSE(ota_health_is_validated());
}

/** @brief A pre-pending CONFIG frame is not evidence of OTA boot health. */
void test_ota_status_before_pending_is_not_fresh(void)
{
    ota_health_set_rollback_fn_for_test(mock_rollback_fn);
    ota_health_set_mark_valid_fn_for_test(mock_mark_valid_ok);
    z906_state_set_main_valid(true);
    s_fake_status_time_us = s_fake_time_us - 1000;
    ota_health_set_pending_verify_for_test(true);
    for (int i = 0; i < 15; i++) {
        advance_health_second();
    }
    TEST_ASSERT_EQUAL_INT(1, s_rollback_call_count);
    TEST_ASSERT_FALSE(ota_health_is_validated());
}

/** @brief Real NVS I/O failures must not be hidden by verifier defaults. */
void test_ota_nvs_read_failure_rejects_image(void)
{
    ota_health_set_rollback_fn_for_test(mock_rollback_fn);
    ota_health_set_mark_valid_fn_for_test(mock_mark_valid_ok);
    ota_health_set_pending_verify_for_test(true);
    s_auto_status_refresh = true;
    s_fake_nvs_ok = false;
    for (int i = 0; i < 15; i++) {
        advance_health_second();
    }
    TEST_ASSERT_EQUAL_INT(1, s_rollback_call_count);
    TEST_ASSERT_FALSE(ota_health_is_validated());
}

/** @brief An unregistered or failed-feed owner must never confirm firmware. */
void test_ota_watchdog_owner_failure_rejects_image(void)
{
    ota_health_set_rollback_fn_for_test(mock_rollback_fn);
    ota_health_set_mark_valid_fn_for_test(mock_mark_valid_ok);
    ota_health_set_pending_verify_for_test(true);
    s_auto_status_refresh = true;
    s_fake_twdt_ok = false;
    for (int i = 0; i < 15; i++) {
        advance_health_second();
    }
    TEST_ASSERT_EQUAL_INT(1, s_rollback_call_count);
    TEST_ASSERT_FALSE(ota_health_is_validated());
}

/** @brief RX failure after criteria PASS, before mark-valid cannot confirm. */
void test_ota_rx_loss_between_pass_and_final_admission_rejects_commit(void)
{
    ota_health_set_rollback_fn_for_test(mock_rollback_fn);
    ota_health_set_mark_valid_fn_for_test(mock_mark_valid_ok);
    ota_health_set_pending_verify_for_test(true);
    ota_health_set_before_final_admission_fn_for_test(inject_rx_drop_at_final_admission);
    s_auto_status_refresh = true;
    for (int i = 0; i < 15; i++) {
        advance_health_second();
    }
    TEST_ASSERT_EQUAL_INT(0, s_mark_valid_call_count);
    TEST_ASSERT_EQUAL_INT(1, s_rollback_call_count);
    TEST_ASSERT_FALSE(ota_health_is_validated());
    TEST_ASSERT_EQUAL_INT64(0, z906_state_get_matched_status_reply_time_us());
}

/** @brief Without a fallback, late RX loss leaves image pending and unapproved. */
void test_ota_final_admission_loss_without_fallback_remains_pending(void)
{
    ota_health_set_rollback_fn_for_test(mock_rollback_fn);
    ota_health_set_mark_valid_fn_for_test(mock_mark_valid_ok);
    ota_health_set_pending_verify_for_test(true);
    ota_health_set_before_final_admission_fn_for_test(inject_rx_drop_at_final_admission);
    s_auto_status_refresh = true;
    s_fake_fallback_available = false;
    for (int i = 0; i < 15; i++) {
        advance_health_second();
    }
    TEST_ASSERT_EQUAL_INT(0, s_mark_valid_call_count);
    TEST_ASSERT_EQUAL_INT(0, s_rollback_call_count);
    TEST_ASSERT_FALSE(ota_health_is_validated());
    TEST_ASSERT_TRUE(ota_health_is_pending_verify());
    TEST_ASSERT_EQUAL(OTA_HEALTH_RECOVERY_NO_FALLBACK,
                      ota_health_get_recovery_state());
}

/** @brief The second SDK handoff guard must reject RX loss after PASS. */
void test_ota_rx_loss_after_final_proof_before_sdk_handoff(void)
{
    ota_health_set_rollback_fn_for_test(mock_rollback_fn);
    ota_health_set_mark_valid_fn_for_test(mock_mark_valid_ok);
    ota_health_set_pending_verify_for_test(true);
    ota_health_set_before_sdk_handoff_fn_for_test(inject_rx_drop_at_final_admission);
    s_auto_status_refresh = true;
    for (int i = 0; i < 15; ++i) advance_health_second();
    TEST_ASSERT_EQUAL_INT(0, s_mark_valid_call_count);
    TEST_ASSERT_EQUAL_INT(1, s_rollback_call_count);
    TEST_ASSERT_FALSE(ota_health_is_validated());
}

/** @brief A concurrent manual rollback before SDK claim must cancel VALID. */
void test_ota_queued_rollback_wins_over_pending_commit(void)
{
    ota_health_set_rollback_fn_for_test(mock_rollback_fn);
    ota_health_set_mark_valid_fn_for_test(mock_mark_valid_ok);
    ota_health_set_pending_verify_for_test(true);
    ota_health_set_before_sdk_handoff_fn_for_test(request_rollback_at_sdk_handoff);
    s_auto_status_refresh = true;
    for (int i = 0; i < 15; ++i) advance_health_second();
    TEST_ASSERT_EQUAL_INT(0, s_mark_valid_call_count);
    TEST_ASSERT_EQUAL_INT(1, s_rollback_call_count);
    TEST_ASSERT_EQUAL(OTA_HEALTH_RECOVERY_REQUESTED,
                      ota_health_get_recovery_state());
    ota_health_criteria_t criteria = {0};
    ota_health_get_criteria(&criteria);
    TEST_ASSERT_TRUE(criteria.z906_comm_healthy);
    TEST_ASSERT_TRUE(ota_health_get_passed_criteria_mask() & OTA_CRITERION_Z906_COMM);
    TEST_ASSERT_TRUE(ota_health_was_manual_only_rollback());
}

/** @brief 19-K: the former final snapshot-to-claim gap must fail closed. */
void test_ota_atomic_admission_rejects_rx_loss_before_claim(void)
{
    ota_health_set_rollback_fn_for_test(mock_rollback_fn);
    ota_health_set_mark_valid_fn_for_test(mock_mark_valid_ok);
    ota_health_set_pending_verify_for_test(true);
    ota_health_set_before_atomic_claim_fn_for_test(inject_rx_drop_at_final_admission);
    s_auto_status_refresh = true;
    for (int i = 0; i < 15; ++i) advance_health_second();
    TEST_ASSERT_EQUAL_INT(0, s_mark_valid_call_count);
    TEST_ASSERT_EQUAL_INT(1, s_rollback_call_count);
    ota_health_criteria_t criteria = {0};
    ota_health_get_criteria(&criteria);
    TEST_ASSERT_FALSE(criteria.z906_comm_healthy);
    TEST_ASSERT_FALSE(ota_health_get_passed_criteria_mask() & OTA_CRITERION_Z906_COMM);
}

/** @brief 19-K: 5-second STATUS freshness must be checked at the locked claim. */
void test_ota_atomic_admission_rejects_status_expired_during_preemption(void)
{
    ota_health_set_rollback_fn_for_test(mock_rollback_fn);
    ota_health_set_mark_valid_fn_for_test(mock_mark_valid_ok);
    ota_health_set_pending_verify_for_test(true);
    ota_health_set_before_atomic_claim_fn_for_test(expire_status_before_atomic_claim);
    s_auto_status_refresh = true;
    for (int i = 0; i < 15; ++i) advance_health_second();
    TEST_ASSERT_EQUAL_INT(0, s_mark_valid_call_count);
    TEST_ASSERT_EQUAL_INT(1, s_rollback_call_count);
    TEST_ASSERT_FALSE(ota_health_is_validated());
}

/** @brief AUD-19K-01: exactly 5 s at the health-mux claim is acceptable. */
void test_ota_locked_claim_lease_exactly_five_seconds_passes(void)
{
    ota_health_set_rollback_fn_for_test(mock_rollback_fn);
    ota_health_set_mark_valid_fn_for_test(mock_mark_valid_ok);
    ota_health_set_pending_verify_for_test(true);
    /* Start t=1; STATUS refreshes at t=2,6,10,14; final cycle is t=16.
     * An additional 3s gives exactly 5s STATUS age at claim t=19. */
    s_delay_before_critical_us = 3000000;
    ota_health_set_before_locked_claim_critical_fn_for_test(
        advance_clock_inside_state_mutex_before_critical);
    s_auto_status_refresh = true;
    for (int i = 0; i < 15; ++i) advance_health_second();
    TEST_ASSERT_EQUAL_INT(1, s_mark_valid_call_count);
    TEST_ASSERT_EQUAL_INT(0, s_rollback_call_count);
    TEST_ASSERT_TRUE(ota_health_is_validated());
}

/** @brief AUD-19K-01: 5 s + 1 us after a callback preemption must fail. */
void test_ota_locked_claim_expired_inside_state_mutex_rejects(void)
{
    ota_health_set_rollback_fn_for_test(mock_rollback_fn);
    ota_health_set_mark_valid_fn_for_test(mock_mark_valid_ok);
    ota_health_set_pending_verify_for_test(true);
    /* Same STATUS t=14 and claim t=19+1us: exactly 5s+1us is rejected. */
    s_delay_before_critical_us = 3000001;
    ota_health_set_before_locked_claim_critical_fn_for_test(
        advance_clock_inside_state_mutex_before_critical);
    s_auto_status_refresh = true;
    for (int i = 0; i < 15; ++i) advance_health_second();
    TEST_ASSERT_EQUAL_INT(0, s_mark_valid_call_count);
    TEST_ASSERT_EQUAL_INT(1, s_rollback_call_count);
    TEST_ASSERT_FALSE(ota_health_is_validated());
    ota_health_criteria_t criteria = {0};
    ota_health_get_criteria(&criteria);
    TEST_ASSERT_FALSE(criteria.z906_comm_healthy);
}

/** @brief 19-K: a manual veto is not a Z906 communication failure. */
void test_ota_atomic_admission_manual_veto_keeps_healthy_z906(void)
{
    ota_health_set_rollback_fn_for_test(mock_rollback_fn);
    ota_health_set_mark_valid_fn_for_test(mock_mark_valid_ok);
    ota_health_set_pending_verify_for_test(true);
    ota_health_set_before_atomic_claim_fn_for_test(request_rollback_at_sdk_handoff);
    s_auto_status_refresh = true;
    for (int i = 0; i < 15; ++i) advance_health_second();
    TEST_ASSERT_EQUAL_INT(0, s_mark_valid_call_count);
    TEST_ASSERT_EQUAL_INT(1, s_rollback_call_count);
    ota_health_criteria_t criteria = {0};
    ota_health_get_criteria(&criteria);
    TEST_ASSERT_TRUE(criteria.z906_comm_healthy);
    TEST_ASSERT_TRUE(ota_health_get_passed_criteria_mask() & OTA_CRITERION_Z906_COMM);
    TEST_ASSERT_TRUE(ota_health_was_manual_only_rollback());
}

/** @brief 19-K option A: later physical loss does not undo software admission. */
void test_ota_loss_after_atomic_claim_does_not_retroactively_cancel(void)
{
    ota_health_set_rollback_fn_for_test(mock_rollback_fn);
    ota_health_set_mark_valid_fn_for_test(mock_mark_valid_ok);
    ota_health_set_pending_verify_for_test(true);
    ota_health_set_after_atomic_claim_fn_for_test(inject_rx_drop_at_final_admission);
    s_auto_status_refresh = true;
    for (int i = 0; i < 15; ++i) advance_health_second();
    TEST_ASSERT_EQUAL_INT(1, s_mark_valid_call_count);
    TEST_ASSERT_EQUAL_INT(0, s_rollback_call_count);
    TEST_ASSERT_TRUE(ota_health_is_validated());
}

/** @brief 19-K option A: after-claim CLI request is diagnosed TOO_LATE. */
void test_ota_manual_rollback_after_atomic_claim_is_too_late(void)
{
    ota_health_set_rollback_fn_for_test(mock_rollback_fn);
    ota_health_set_mark_valid_fn_for_test(mock_mark_valid_ok);
    ota_health_set_pending_verify_for_test(true);
    ota_health_set_after_atomic_claim_fn_for_test(request_rollback_after_atomic_claim);
    s_auto_status_refresh = true;
    for (int i = 0; i < 15; ++i) advance_health_second();
    TEST_ASSERT_EQUAL_INT(1, s_mark_valid_call_count);
    TEST_ASSERT_EQUAL_INT(0, s_rollback_call_count);
    TEST_ASSERT_EQUAL(OTA_HEALTH_RECOVERY_REQUEST_TOO_LATE,
                      ota_health_get_recovery_state());
}

/** @brief Operator-only rollback is never reported as failed Z906 health. */
void test_ota_manual_rollback_result_is_distinct_from_health_failure(void)
{
    ota_health_set_rollback_fn_for_test(mock_rollback_fn);
    ota_health_set_pending_verify_for_test(true);
    prime_ota_client_pending_validation();
    ota_health_trigger_manual_rollback("Operator cancelled pending OTA");
    ota_client_process_cycle();
    TEST_ASSERT_TRUE(ota_health_was_manual_only_rollback());
    TEST_ASSERT_EQUAL(OTA_RESULT_MANUAL_ROLLBACK_REQUESTED, ota_client_get_last_result());
    TEST_ASSERT_EQUAL(OTA_STATE_FAILED, ota_client_get_state());
    TEST_ASSERT_EQUAL_INT(1, s_rollback_call_count);
}

/** @brief A rollback during the irreversible API cannot be falsely reported. */
void test_ota_rollback_during_sdk_is_reported_too_late(void)
{
    ota_health_set_rollback_fn_for_test(mock_rollback_fn);
    ota_health_set_mark_valid_fn_for_test(mock_mark_valid_ok);
    ota_health_set_pending_verify_for_test(true);
    s_inject_rollback_during_sdk_call = true;
    s_auto_status_refresh = true;
    for (int i = 0; i < 15; ++i) advance_health_second();
    TEST_ASSERT_EQUAL_INT(1, s_mark_valid_call_count);
    TEST_ASSERT_EQUAL_INT(0, s_rollback_call_count);
    TEST_ASSERT_TRUE(ota_health_is_validated());
    TEST_ASSERT_EQUAL(OTA_HEALTH_RECOVERY_REQUEST_TOO_LATE,
                      ota_health_get_recovery_state());
}

/** @brief A real RX health failure must supersede a simultaneous manual veto. */
void test_ota_concurrent_manual_and_real_rx_failure_reports_health_failure(void)
{
    ota_health_set_rollback_fn_for_test(mock_rollback_fn);
    ota_health_set_mark_valid_fn_for_test(mock_mark_valid_ok);
    ota_health_set_pending_verify_for_test(true);
    prime_ota_client_pending_validation();
    ota_health_set_before_atomic_claim_fn_for_test(
        manual_rollback_then_rx_drop_before_claim);
    s_auto_status_refresh = true;
    for (int i = 0; i < 15; ++i) advance_health_second();
    ota_client_process_cycle();
    TEST_ASSERT_EQUAL_INT(0, s_mark_valid_call_count);
    TEST_ASSERT_EQUAL_INT(1, s_rollback_call_count);
    TEST_ASSERT_FALSE(ota_health_was_manual_only_rollback());
    TEST_ASSERT_EQUAL(OTA_RESULT_HEALTH_CHECK_FAILED, ota_client_get_last_result());
    TEST_ASSERT_EQUAL(OTA_STATE_FAILED, ota_client_get_state());
    ota_health_criteria_t criteria = {0};
    ota_health_get_criteria(&criteria);
    TEST_ASSERT_FALSE(criteria.z906_comm_healthy);
}

/** @brief AUD-19K-02: SDK failure beats TOO_LATE manual in client result. */
void test_ota_sdk_commit_failure_overrides_late_manual_with_fallback(void)
{
    ota_health_set_rollback_fn_for_test(mock_rollback_fn);
    ota_health_set_mark_valid_fn_for_test(mock_mark_valid_ok);
    ota_health_set_pending_verify_for_test(true);
    prime_ota_client_pending_validation();
    s_mark_valid_result = ESP_FAIL;
    s_inject_rollback_during_sdk_call = true;
    s_auto_status_refresh = true;
    for (int i = 0; i < 15; ++i) advance_health_second();
    ota_client_process_cycle();
    TEST_ASSERT_EQUAL_INT(1, s_mark_valid_call_count);
    TEST_ASSERT_EQUAL_INT(1, s_rollback_call_count);
    TEST_ASSERT_FALSE(ota_health_is_validated());
    TEST_ASSERT_TRUE(ota_health_was_sdk_mark_valid_failed());
    TEST_ASSERT_FALSE(ota_health_was_manual_only_rollback());
    TEST_ASSERT_EQUAL(OTA_HEALTH_RECOVERY_REQUESTED, ota_health_get_recovery_state());
    TEST_ASSERT_EQUAL(OTA_RESULT_SDK_MARK_VALID_FAILED, ota_client_get_last_result());
    TEST_ASSERT_EQUAL(OTA_STATE_FAILED, ota_client_get_state());
    ota_health_criteria_t criteria = {0};
    ota_health_get_criteria(&criteria);
    TEST_ASSERT_TRUE(criteria.z906_comm_healthy);
}

/** @brief SDK commit failure with no fallback cannot force an unsafe reboot. */
void test_ota_sdk_commit_failure_late_manual_without_fallback(void)
{
    ota_health_set_rollback_fn_for_test(mock_rollback_fn);
    ota_health_set_mark_valid_fn_for_test(mock_mark_valid_ok);
    ota_health_set_pending_verify_for_test(true);
    prime_ota_client_pending_validation();
    s_mark_valid_result = ESP_FAIL;
    s_inject_rollback_during_sdk_call = true;
    s_fake_fallback_available = false;
    s_auto_status_refresh = true;
    for (int i = 0; i < 15; ++i) advance_health_second();
    ota_client_process_cycle();
    TEST_ASSERT_EQUAL_INT(1, s_mark_valid_call_count);
    TEST_ASSERT_EQUAL_INT(0, s_rollback_call_count);
    TEST_ASSERT_EQUAL(OTA_HEALTH_RECOVERY_NO_FALLBACK, ota_health_get_recovery_state());
    TEST_ASSERT_EQUAL(OTA_RESULT_SDK_MARK_VALID_FAILED, ota_client_get_last_result());
    TEST_ASSERT_EQUAL(OTA_STATE_FAILED, ota_client_get_state());
}

/** @brief A successful SDK commit stays valid despite a too-late CLI request. */
void test_ota_sdk_commit_success_preserves_too_late_diagnostic(void)
{
    ota_health_set_rollback_fn_for_test(mock_rollback_fn);
    ota_health_set_mark_valid_fn_for_test(mock_mark_valid_ok);
    ota_health_set_pending_verify_for_test(true);
    prime_ota_client_pending_validation();
    s_inject_rollback_during_sdk_call = true;
    s_auto_status_refresh = true;
    for (int i = 0; i < 15; ++i) advance_health_second();
    ota_client_process_cycle();
    TEST_ASSERT_EQUAL_INT(1, s_mark_valid_call_count);
    TEST_ASSERT_TRUE(ota_health_is_validated());
    TEST_ASSERT_FALSE(ota_health_was_sdk_mark_valid_failed());
    TEST_ASSERT_EQUAL(OTA_HEALTH_RECOVERY_REQUEST_TOO_LATE, ota_health_get_recovery_state());
    TEST_ASSERT_EQUAL(OTA_STATE_SUCCESS, ota_client_get_state());
    TEST_ASSERT_EQUAL_INT(0, s_rollback_call_count);
    /* The current client only updates last_result on failure; testing a
     * fresh SUCCESS result needs an explicit production-client contract. */
}

int runUnityTests(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_ota_state_strings);
    RUN_TEST(test_ota_result_strings);
    RUN_TEST(test_ota_incompatible_image_rejection);
    RUN_TEST(test_zigbee_ota_header_structure);
    RUN_TEST(test_ota_health_check_subsystem);
    RUN_TEST(test_ota_diagnostics_retrieval);
    RUN_TEST(test_ota_health_failure_recording);
    RUN_TEST(test_ota_health_criteria_reporting_and_mask);
    RUN_TEST(test_ota_health_transition_a_healthy_throughout_validates);
    RUN_TEST(test_ota_health_transition_b_healthy_to_unhealthy_triggers_rollback);
    RUN_TEST(test_ota_health_transition_c_unhealthy_to_healthy_validates);
    RUN_TEST(test_ota_health_transition_d_unhealthy_throughout_triggers_rollback);
    RUN_TEST(test_ota_pending_without_verified_status_rolls_back);
    RUN_TEST(test_ota_cable_loss_revokes_health);
    RUN_TEST(test_ota_single_late_tick_cannot_bypass_validation);
    RUN_TEST(test_ota_without_fallback_stays_pending);
    RUN_TEST(test_ota_status_arrives_during_window);
    RUN_TEST(test_ota_stale_status_does_not_approve_image);
    RUN_TEST(test_ota_status_before_pending_is_not_fresh);
    RUN_TEST(test_ota_nvs_read_failure_rejects_image);
    RUN_TEST(test_ota_watchdog_owner_failure_rejects_image);
    RUN_TEST(test_ota_rx_loss_between_pass_and_final_admission_rejects_commit);
    RUN_TEST(test_ota_final_admission_loss_without_fallback_remains_pending);
    RUN_TEST(test_ota_rx_loss_after_final_proof_before_sdk_handoff);
    RUN_TEST(test_ota_queued_rollback_wins_over_pending_commit);
    RUN_TEST(test_ota_atomic_admission_rejects_rx_loss_before_claim);
    RUN_TEST(test_ota_atomic_admission_rejects_status_expired_during_preemption);
    RUN_TEST(test_ota_locked_claim_lease_exactly_five_seconds_passes);
    RUN_TEST(test_ota_locked_claim_expired_inside_state_mutex_rejects);
    RUN_TEST(test_ota_atomic_admission_manual_veto_keeps_healthy_z906);
    RUN_TEST(test_ota_loss_after_atomic_claim_does_not_retroactively_cancel);
    RUN_TEST(test_ota_manual_rollback_after_atomic_claim_is_too_late);
    RUN_TEST(test_ota_manual_rollback_result_is_distinct_from_health_failure);
    RUN_TEST(test_ota_rollback_during_sdk_is_reported_too_late);
    RUN_TEST(test_ota_concurrent_manual_and_real_rx_failure_reports_health_failure);
    RUN_TEST(test_ota_sdk_commit_failure_overrides_late_manual_with_fallback);
    RUN_TEST(test_ota_sdk_commit_failure_late_manual_without_fallback);
    RUN_TEST(test_ota_sdk_commit_success_preserves_too_late_diagnostic);
    return UNITY_END();
}

#if defined(ESP_PLATFORM)
void app_main(void)
{
    vTaskDelay(pdMS_TO_TICKS(3000)); /* Allow serial monitor to open */
    runUnityTests();
}
#else
int main(void)
{
    return runUnityTests();
}
#endif
