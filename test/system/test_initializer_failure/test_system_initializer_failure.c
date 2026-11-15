/**
 * @file test_system_initializer_failure.c
 * @brief Failure injection tests for the System Initializer.
 *
 * Uses the sys_init_hooks_t test seam to inject fatal init and start failures.
 * Each test scenario verifies: FAILED state, correct phase, correct component.
 *
 * ## Stub Strategy for Start-Failure Tests (AC-02, AC-03, AC-04)
 *
 * Start failures require all INIT steps to complete first. However, letting real
 * init functions run and then resetting state leaves modules in a partially-init'd
 * state that is unsafe to re-init in the next test.
 *
 * Solution: ALL init hooks are set to NOP stubs (return true without doing anything),
 * and ALL earlier start hooks are set to NOP stubs (return ESP_OK / true). Only the
 * target start hook is set to fail. This isolates the state machine behavior without
 * touching real hardware or FreeRTOS resources.
 *
 * The final test (test_real_boot_succeeds_after_reset) clears ALL hooks and runs the
 * real boot sequence, confirming recovery from injected failures works.
 */

#include <unity.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include "esp_err.h"
#include "system_initializer.h"

/* ============================================================
 * Common stubs
 * ============================================================ */

/** @brief FATAL init stub: simulates a module init that returns false (failure). */
static bool stub_fail_false(void)  { return false; }

/** @brief NOP init stub: returns true without initializing anything (safe for multi-test sequencing). */
static bool stub_nop_true(void)    { return true; }

/** @brief NOP start stub (esp_err_t return): returns ESP_OK without starting anything. */
static esp_err_t stub_nop_ok(void) { return ESP_OK; }

/** @brief FATAL start stub (esp_err_t return): simulates a start function failure. */
static esp_err_t stub_fail_err(void) { return ESP_FAIL; }

/** @brief NOP start stub (bool return): returns true without starting anything. */
static bool stub_nop_start_true(void) { return true; }

/** @brief FATAL start stub (bool return): simulates a bool-returning start failure. */
static bool stub_fail_start_false(void) { return false; }

/* ============================================================
 * Hook table helpers for start-failure tests
 * - All inits are NOP (no real hardware or RTOS resources created)
 * - All earlier starts are NOP (no tasks actually spawned)
 * - Only the target start is set to fail
 * ============================================================ */

/** @brief Base hook table: all inits NOP, all starts NOP (used as a template). */
static const sys_init_hooks_t k_all_nop = {
    /* Fatal inits — all NOP */
    .nvs_init          = stub_nop_true,
    .event_bus_init    = stub_nop_true,
    .fsm_init          = stub_nop_true,
    .gpio_init         = stub_nop_true,
    .uart_init         = stub_nop_true,
    /* Fatal starts — all NOP */
    .transport_start   = stub_nop_ok,
    .fsm_start         = stub_nop_ok,
    .zigbee_start      = stub_nop_start_true,
    .telemetry_start   = stub_nop_start_true,
    /* Degraded inits — all NOP (prevent real hardware calls without real inits) */
    .wdt_init          = stub_nop_true,
    .debug_console_init = stub_nop_true,
    .cmd_verifier_init = stub_nop_true,
    .adc_init          = stub_nop_true,
    .led_init          = stub_nop_true,
    .ota_init          = stub_nop_true,
    .cli_init          = stub_nop_true,
};

/* ============================================================
 * setUp / tearDown
 * ============================================================ */

void setUp(void)
{
    /* Reset state machine for each test — previous run's outcome must not leak. */
    system_initializer_reset_state_for_test();
}

void tearDown(void)
{
    /* Restore real hooks so the next test starts with a clean slate. */
    system_initializer_set_hooks(NULL);
}

/* ============================================================
 * FC-06a: NVS fatal init failure → FAILED at EARLY phase
 * ============================================================ */
void test_fatal_nvs_failure_yields_failed_state(void)
{
    sys_init_hooks_t h = { .nvs_init = stub_fail_false };
    system_initializer_set_hooks(&h);

    TEST_ASSERT_NOT_EQUAL(ESP_OK, system_initializer_run());
    TEST_ASSERT_EQUAL(SYSTEM_INIT_FAILED, system_initializer_get_state());

    system_init_report_t r;
    system_initializer_get_report(&r);
    TEST_ASSERT_EQUAL(SYSTEM_INIT_FAILED, r.state);
    TEST_ASSERT_EQUAL(BOOT_PHASE_EARLY, r.failed_phase);
    TEST_ASSERT_NOT_NULL(r.failed_component);
    TEST_ASSERT_EQUAL_STRING("NVS", r.failed_component);
    TEST_ASSERT_NOT_EQUAL(ESP_OK, r.last_error);
}

/* ============================================================
 * FC-06b: EventBus fatal init failure → FAILED at CORE phase
 * ============================================================ */
void test_fatal_eventbus_failure_yields_failed_state(void)
{
    sys_init_hooks_t h = { .nvs_init = stub_nop_true, .event_bus_init = stub_fail_false };
    system_initializer_set_hooks(&h);

    TEST_ASSERT_NOT_EQUAL(ESP_OK, system_initializer_run());
    TEST_ASSERT_EQUAL(SYSTEM_INIT_FAILED, system_initializer_get_state());

    system_init_report_t r;
    system_initializer_get_report(&r);
    TEST_ASSERT_EQUAL(BOOT_PHASE_CORE, r.failed_phase);
    TEST_ASSERT_EQUAL_STRING("EventBus", r.failed_component);
}

/* ============================================================
 * FC-06c: FSM fatal init failure → FAILED at CORE phase
 * ============================================================ */
void test_fatal_fsm_failure_yields_failed_state(void)
{
    sys_init_hooks_t h = { .nvs_init = stub_nop_true, .fsm_init = stub_fail_false };
    system_initializer_set_hooks(&h);

    TEST_ASSERT_NOT_EQUAL(ESP_OK, system_initializer_run());
    TEST_ASSERT_EQUAL(SYSTEM_INIT_FAILED, system_initializer_get_state());

    system_init_report_t r;
    system_initializer_get_report(&r);
    TEST_ASSERT_EQUAL(BOOT_PHASE_CORE, r.failed_phase);
    TEST_ASSERT_EQUAL_STRING("FSM", r.failed_component);
}

/* ============================================================
 * FC-06d: GPIO fatal init failure → FAILED at HARDWARE phase
 * ============================================================ */
void test_fatal_gpio_failure_yields_failed_state(void)
{
    sys_init_hooks_t h = { .nvs_init = stub_nop_true, .gpio_init = stub_fail_false };
    system_initializer_set_hooks(&h);

    TEST_ASSERT_NOT_EQUAL(ESP_OK, system_initializer_run());
    TEST_ASSERT_EQUAL(SYSTEM_INIT_FAILED, system_initializer_get_state());

    system_init_report_t r;
    system_initializer_get_report(&r);
    TEST_ASSERT_EQUAL(BOOT_PHASE_HARDWARE, r.failed_phase);
    TEST_ASSERT_EQUAL_STRING("GPIO", r.failed_component);
}

/* ============================================================
 * AC-01: UART fatal init failure → FAILED at HARDWARE phase
 * ============================================================ */
void test_fatal_uart_failure_yields_failed_state(void)
{
    /* NVS must succeed (real NVS or NOP) before HARDWARE phase is reached.
     * All other non-UART hooks left NULL (real functions). Since only GPIO
     * precedes UART, and GPIO uses the real function (which succeeds on real hw),
     * setting only uart_init = fail is sufficient. */
    sys_init_hooks_t h = { .uart_init = stub_fail_false };
    system_initializer_set_hooks(&h);

    TEST_ASSERT_NOT_EQUAL(ESP_OK, system_initializer_run());
    TEST_ASSERT_EQUAL(SYSTEM_INIT_FAILED, system_initializer_get_state());

    system_init_report_t r;
    system_initializer_get_report(&r);
    TEST_ASSERT_EQUAL(BOOT_PHASE_HARDWARE, r.failed_phase);
    TEST_ASSERT_EQUAL_STRING("UART", r.failed_component);
    TEST_ASSERT_NOT_EQUAL(ESP_OK, r.last_error);
}

/* ============================================================
 * AC-02: transport_start() failure → FAILED at ACTIVATE phase
 *
 * All inits are NOP. transport_start fails. No real tasks are created.
 * ============================================================ */
void test_fatal_transport_start_failure_yields_failed_state(void)
{
    sys_init_hooks_t h   = k_all_nop;
    h.transport_start    = stub_fail_err; /* Override: transport fails to start */
    system_initializer_set_hooks(&h);

    TEST_ASSERT_NOT_EQUAL(ESP_OK, system_initializer_run());
    TEST_ASSERT_EQUAL(SYSTEM_INIT_FAILED, system_initializer_get_state());

    system_init_report_t r;
    system_initializer_get_report(&r);
    TEST_ASSERT_EQUAL(BOOT_PHASE_ACTIVATE, r.failed_phase);
    /* component name is the task name, not the module name */
    TEST_ASSERT_EQUAL_STRING("uart_rx_task", r.failed_component);
    TEST_ASSERT_NOT_EQUAL(ESP_OK, r.last_error);
}

/* ============================================================
 * AC-03: fsm_start() failure → FAILED at ACTIVATE phase
 *
 * transport_start is NOP-success; fsm_start fails. No real tasks created.
 * ============================================================ */
void test_fatal_fsm_start_failure_yields_failed_state(void)
{
    sys_init_hooks_t h   = k_all_nop;
    h.transport_start    = stub_nop_ok;   /* transport starts fine (NOP) */
    h.fsm_start          = stub_fail_err; /* FSM task fails to start     */
    system_initializer_set_hooks(&h);

    TEST_ASSERT_NOT_EQUAL(ESP_OK, system_initializer_run());
    TEST_ASSERT_EQUAL(SYSTEM_INIT_FAILED, system_initializer_get_state());

    system_init_report_t r;
    system_initializer_get_report(&r);
    TEST_ASSERT_EQUAL(BOOT_PHASE_ACTIVATE, r.failed_phase);
    TEST_ASSERT_EQUAL_STRING("z906_fsm_task", r.failed_component);
    TEST_ASSERT_NOT_EQUAL(ESP_OK, r.last_error);
}

/* ============================================================
 * AC-04: zigbee_native_stack_start() failure → FAILED at ACTIVATE phase
 *
 * transport and FSM start fine (NOP); Zigbee stack init/start fails.
 * This validates: task create ≠ subsystem started invariant.
 * ============================================================ */
void test_fatal_zigbee_start_failure_yields_failed_state(void)
{
    sys_init_hooks_t h   = k_all_nop;
    h.zigbee_start       = stub_fail_start_false; /* Zigbee stack fails to init */
    system_initializer_set_hooks(&h);

    TEST_ASSERT_NOT_EQUAL(ESP_OK, system_initializer_run());
    TEST_ASSERT_EQUAL(SYSTEM_INIT_FAILED, system_initializer_get_state());

    system_init_report_t r;
    system_initializer_get_report(&r);
    TEST_ASSERT_EQUAL(BOOT_PHASE_ACTIVATE, r.failed_phase);
    TEST_ASSERT_EQUAL_STRING("zigbee_task", r.failed_component);
}

/* ============================================================
 * FC-08 Control: real boot after all failures succeeds (MUST BE LAST)
 *
 * Clears all hooks (real functions). Verifies the initializer correctly
 * reaches READY after a sequence of injected failures in the same binary.
 * ============================================================ */
void test_real_boot_succeeds_after_reset(void)
{
    system_initializer_set_hooks(NULL); /* Restore real function pointers */

    TEST_ASSERT_EQUAL(ESP_OK, system_initializer_run());
    TEST_ASSERT_EQUAL(SYSTEM_INIT_READY, system_initializer_get_state());

    system_init_report_t r;
    system_initializer_get_report(&r);
    TEST_ASSERT_EQUAL(SYSTEM_INIT_READY, r.state);
    TEST_ASSERT_NULL(r.failed_component);
    TEST_ASSERT_EQUAL(ESP_OK, r.last_error);
    TEST_ASSERT_GREATER_THAN(0, r.elapsed_ms);
}

/* ============================================================
 * Test Runner Entry Point
 * ============================================================ */
/**
 * @brief Failure injection test entry point.
 *
 * WHY THE DELAY: esptool hard-resets the board after flashing. PlatformIO's
 * test runner opens the serial port AFTER esptool exits (~2-3s). Without the
 * delay, all Unity output is missed and the test appears SKIPPED.
 *
 * ORDER IS CRITICAL:
 *   - NOP-based failure tests (AC-02..04) run first to avoid hardware state pollution.
 *   - UART test (AC-01) uses real init flow; runs before real boot.
 *   - Real boot (FC-08) MUST be last — it leaves tasks running.
 */
void app_main(void)
{
    vTaskDelay(pdMS_TO_TICKS(3000)); /* Allow test runner to open serial port */
    UNITY_BEGIN();

    /* Fatal INIT failure tests (AC-06a..06d, AC-01) */
    RUN_TEST(test_fatal_nvs_failure_yields_failed_state);
    RUN_TEST(test_fatal_eventbus_failure_yields_failed_state);
    RUN_TEST(test_fatal_fsm_failure_yields_failed_state);
    RUN_TEST(test_fatal_gpio_failure_yields_failed_state);

    /* Fatal START failure tests (AC-02..04) — NOP-based, no real hw touched */
    RUN_TEST(test_fatal_transport_start_failure_yields_failed_state);
    RUN_TEST(test_fatal_fsm_start_failure_yields_failed_state);
    RUN_TEST(test_fatal_zigbee_start_failure_yields_failed_state);

    /* UART fatal init — runs real init up to UART; must precede real boot */
    RUN_TEST(test_fatal_uart_failure_yields_failed_state);

    /* Control: real boot MUST be last (leaves tasks running) */
    RUN_TEST(test_real_boot_succeeds_after_reset);

    UNITY_END();
}
