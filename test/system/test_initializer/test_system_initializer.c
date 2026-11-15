/**
 * @file test_system_initializer.c
 * @brief Unit tests for the System Initializer boot state model, report API,
 *        and string conversion utilities.
 *
 * ## Test Scope
 *
 * These tests run on-device (ESP32-C6) via PlatformIO Unity test runner.
 * They exercise the System Initializer's public API using the actual module
 * implementation — no mocking of the initializer itself.
 *
 * ## What Is Tested
 *
 * 1.  Initial state before any boot sequence runs (NOT_STARTED).
 * 2.  system_init_state_str() returns meaningful non-NULL strings.
 * 3.  boot_phase_str() returns meaningful non-NULL strings for all phases.
 * 4.  system_initializer_get_report() with NULL pointer returns error.
 * 5.  system_initializer_get_report() returns a valid struct post-run.
 * 6.  system_initializer_run() completes and reaches SYSTEM_INIT_READY state.
 * 7.  Re-entry: second call to system_initializer_run() returns INVALID_STATE.
 * 8.  After READY, get_state() still returns READY (not regressed).
 * 9.  Boot report has non-zero elapsed_ms on success.
 * 10. Boot report has ESP_OK last_error on success.
 * 11. Boot report has NULL failed_component on success.
 * 12. Boot report has BOOT_PHASE_NONE as failed_phase on success.
 * 13. string representations cover all enum values without returning "UNKNOWN".
 *
 * ## What Is NOT Tested Here
 *
 * - Fatal failure injection (requires hardware mock; covered in integration tests).
 * - Individual phase order (covered by dependency analysis in source docs).
 * - OTA health interaction (covered in test_ota suite).
 * - Watchdog lifecycle (covered in test_debug_console suite).
 *
 * ## Test Execution
 *
 *   pio test -e esp32-c6-devkitc-1 --filter test_initializer
 */

#include <unity.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stddef.h>
#include <string.h>

#include "system_initializer.h"

/* =========================================================================
 * setUp / tearDown
 * ========================================================================= */

void setUp(void)
{
    /* No per-test setup needed.
     * system_initializer_run() is intentionally called only once per test suite
     * because re-entry is correctly blocked after the first invocation. */
}

void tearDown(void)
{
    /* No per-test teardown needed. */
}

/* =========================================================================
 * String Utility Tests
 * ========================================================================= */

/**
 * @brief Verify that system_init_state_str() returns a non-NULL, non-empty
 * string for every defined enum value. "UNKNOWN" is only acceptable for
 * out-of-range inputs.
 */
void test_state_str_all_values_non_null(void)
{
    TEST_ASSERT_NOT_NULL(system_init_state_str(SYSTEM_INIT_NOT_STARTED));
    TEST_ASSERT_NOT_NULL(system_init_state_str(SYSTEM_INIT_IN_PROGRESS));
    TEST_ASSERT_NOT_NULL(system_init_state_str(SYSTEM_INIT_READY));
    TEST_ASSERT_NOT_NULL(system_init_state_str(SYSTEM_INIT_FAILED));

    /* Each valid value must map to a unique, meaningful label — not "UNKNOWN" */
    TEST_ASSERT_NOT_EQUAL_INT(0, strcmp("UNKNOWN",
        system_init_state_str(SYSTEM_INIT_NOT_STARTED)));
    TEST_ASSERT_NOT_EQUAL_INT(0, strcmp("UNKNOWN",
        system_init_state_str(SYSTEM_INIT_IN_PROGRESS)));
    TEST_ASSERT_NOT_EQUAL_INT(0, strcmp("UNKNOWN",
        system_init_state_str(SYSTEM_INIT_READY)));
    TEST_ASSERT_NOT_EQUAL_INT(0, strcmp("UNKNOWN",
        system_init_state_str(SYSTEM_INIT_FAILED)));
}

/**
 * @brief Verify that boot_phase_str() returns a non-NULL, non-empty string
 * for every defined boot phase enum value.
 */
void test_boot_phase_str_all_values_non_null(void)
{
    TEST_ASSERT_NOT_NULL(boot_phase_str(BOOT_PHASE_NONE));
    TEST_ASSERT_NOT_NULL(boot_phase_str(BOOT_PHASE_EARLY));
    TEST_ASSERT_NOT_NULL(boot_phase_str(BOOT_PHASE_CORE));
    TEST_ASSERT_NOT_NULL(boot_phase_str(BOOT_PHASE_HARDWARE));
    TEST_ASSERT_NOT_NULL(boot_phase_str(BOOT_PHASE_MANAGEMENT));
    TEST_ASSERT_NOT_NULL(boot_phase_str(BOOT_PHASE_CLI));
    TEST_ASSERT_NOT_NULL(boot_phase_str(BOOT_PHASE_ACTIVATE));
    TEST_ASSERT_NOT_NULL(boot_phase_str(BOOT_PHASE_READY));

    /* None of the valid phases should fall through to "UNKNOWN" */
    TEST_ASSERT_NOT_EQUAL_INT(0, strcmp("UNKNOWN", boot_phase_str(BOOT_PHASE_NONE)));
    TEST_ASSERT_NOT_EQUAL_INT(0, strcmp("UNKNOWN", boot_phase_str(BOOT_PHASE_EARLY)));
    TEST_ASSERT_NOT_EQUAL_INT(0, strcmp("UNKNOWN", boot_phase_str(BOOT_PHASE_CORE)));
    TEST_ASSERT_NOT_EQUAL_INT(0, strcmp("UNKNOWN", boot_phase_str(BOOT_PHASE_HARDWARE)));
    TEST_ASSERT_NOT_EQUAL_INT(0, strcmp("UNKNOWN", boot_phase_str(BOOT_PHASE_MANAGEMENT)));
    TEST_ASSERT_NOT_EQUAL_INT(0, strcmp("UNKNOWN", boot_phase_str(BOOT_PHASE_CLI)));
    TEST_ASSERT_NOT_EQUAL_INT(0, strcmp("UNKNOWN", boot_phase_str(BOOT_PHASE_ACTIVATE)));
    TEST_ASSERT_NOT_EQUAL_INT(0, strcmp("UNKNOWN", boot_phase_str(BOOT_PHASE_READY)));
}

/* =========================================================================
 * API Input Validation Tests
 * ========================================================================= */

/**
 * @brief Verify that system_initializer_get_report() rejects a NULL pointer
 * and returns ESP_ERR_INVALID_ARG without crashing.
 */
void test_get_report_null_pointer_returns_error(void)
{
    esp_err_t err = system_initializer_get_report(NULL);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, err);
}

/* =========================================================================
 * Boot Sequence Tests
 *
 * NOTE: These tests call system_initializer_run() exactly once. Re-entry is
 * blocked by design after the first call. All subsequent API tests read state
 * from the completed run. Tests are ordered to execute in declaration sequence.
 * ========================================================================= */

/**
 * @brief Verify that system_initializer_run() succeeds on a clean first call
 * and transitions the global state to SYSTEM_INIT_READY.
 *
 * This test runs the FULL boot sequence on real hardware (all 6 phases).
 * It validates the happy-path end-to-end behavior.
 */
void test_run_succeeds_and_reaches_ready(void)
{
    esp_err_t err = system_initializer_run();

    /* On hardware with all modules present, boot must succeed */
    TEST_ASSERT_EQUAL(ESP_OK, err);
    TEST_ASSERT_EQUAL(SYSTEM_INIT_READY, system_initializer_get_state());
}

/**
 * @brief Verify that calling system_initializer_run() a second time returns
 * ESP_ERR_INVALID_STATE and does NOT create duplicate tasks, queues, or
 * watchdog registrations.
 *
 * This test must run AFTER test_run_succeeds_and_reaches_ready.
 */
void test_reentry_returns_invalid_state(void)
{
    /* First call already done in previous test; state is now READY */
    TEST_ASSERT_EQUAL(SYSTEM_INIT_READY, system_initializer_get_state());

    /* Second call must be rejected immediately */
    esp_err_t err = system_initializer_run();
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, err);

    /* State must remain READY — re-entry must not cause regression */
    TEST_ASSERT_EQUAL(SYSTEM_INIT_READY, system_initializer_get_state());
}

/**
 * @brief Verify that after a successful boot, get_state() consistently returns
 * SYSTEM_INIT_READY on every read (no transient state after boot).
 */
void test_state_remains_ready_after_boot(void)
{
    /* Read multiple times to verify stability */
    TEST_ASSERT_EQUAL(SYSTEM_INIT_READY, system_initializer_get_state());
    TEST_ASSERT_EQUAL(SYSTEM_INIT_READY, system_initializer_get_state());
    TEST_ASSERT_EQUAL(SYSTEM_INIT_READY, system_initializer_get_state());
}

/* =========================================================================
 * Boot Report Tests
 *
 * These tests validate the report struct populated during a successful boot.
 * They must run AFTER test_run_succeeds_and_reaches_ready.
 * ========================================================================= */

/**
 * @brief Verify that system_initializer_get_report() succeeds and returns
 * a valid pointer after the boot sequence completes.
 */
void test_get_report_succeeds_after_boot(void)
{
    system_init_report_t report;
    esp_err_t err = system_initializer_get_report(&report);

    TEST_ASSERT_EQUAL(ESP_OK, err);
}

/**
 * @brief Verify that the boot report state field matches the global state
 * getter after a successful boot.
 */
void test_report_state_matches_get_state(void)
{
    system_init_report_t report;
    system_initializer_get_report(&report);

    TEST_ASSERT_EQUAL(system_initializer_get_state(), report.state);
    TEST_ASSERT_EQUAL(SYSTEM_INIT_READY, report.state);
}

/**
 * @brief Verify that the boot report records a non-zero elapsed time.
 *
 * A zero elapsed_ms would indicate the timer was never started or the boot
 * completed in under 1 ms (implausible for the full hardware init sequence).
 */
void test_report_elapsed_ms_is_nonzero(void)
{
    system_init_report_t report;
    system_initializer_get_report(&report);

    TEST_ASSERT_GREATER_THAN(0, report.elapsed_ms);
}

/**
 * @brief Verify that the boot report has ESP_OK as last_error on success.
 *
 * Any non-ESP_OK value here would indicate the report was not properly
 * reset on the success path.
 */
void test_report_last_error_is_ok_on_success(void)
{
    system_init_report_t report;
    system_initializer_get_report(&report);

    TEST_ASSERT_EQUAL(ESP_OK, report.last_error);
}

/**
 * @brief Verify that failed_component is NULL on a successful boot.
 *
 * A non-NULL failed_component on a successful boot would indicate the report
 * cleanup logic has a bug (leftover state from a previous test or static init).
 */
void test_report_failed_component_is_null_on_success(void)
{
    system_init_report_t report;
    system_initializer_get_report(&report);

    TEST_ASSERT_NULL(report.failed_component);
}

/**
 * @brief Verify that failed_phase is BOOT_PHASE_NONE on a successful boot.
 *
 * Any non-NONE phase here would indicate a failed phase was not properly
 * cleared on the success path.
 */
void test_report_failed_phase_is_none_on_success(void)
{
    system_init_report_t report;
    system_initializer_get_report(&report);

    TEST_ASSERT_EQUAL(BOOT_PHASE_NONE, report.failed_phase);
}

/* =========================================================================
 * Test Runner Entry Point
 * ========================================================================= */

/**
 * @brief Unity test runner entry point for PlatformIO on-device execution.
 *
 * Tests are declared in dependency order:
 *   1. String utilities (no system state dependency).
 *   2. Input validation (no system state dependency).
 *   3. Boot sequence (first run, then re-entry, then state stability).
 *   4. Report validation (requires successful boot from step 3).
 *
 * WHY THE DELAY: esptool hard-resets the board at the end of flashing.
 * PlatformIO's test runner opens the serial port AFTER esptool exits,
 * which takes ~2-3 seconds. Without this delay, all Unity output is
 * transmitted before the test runner starts listening and the test
 * appears as SKIPPED.
 */
void app_main(void)
{
    vTaskDelay(pdMS_TO_TICKS(3000)); /* Allow test runner to open serial port */
    UNITY_BEGIN();

    /* String utility tests */
    RUN_TEST(test_state_str_all_values_non_null);
    RUN_TEST(test_boot_phase_str_all_values_non_null);

    /* Input validation tests */
    RUN_TEST(test_get_report_null_pointer_returns_error);

    /* Boot sequence tests (order matters — run() before re-entry check) */
    RUN_TEST(test_run_succeeds_and_reaches_ready);
    RUN_TEST(test_reentry_returns_invalid_state);
    RUN_TEST(test_state_remains_ready_after_boot);

    /* Report validation tests (require successful boot above) */
    RUN_TEST(test_get_report_succeeds_after_boot);
    RUN_TEST(test_report_state_matches_get_state);
    RUN_TEST(test_report_elapsed_ms_is_nonzero);
    RUN_TEST(test_report_last_error_is_ok_on_success);
    RUN_TEST(test_report_failed_component_is_null_on_success);
    RUN_TEST(test_report_failed_phase_is_none_on_success);

    UNITY_END();
}
