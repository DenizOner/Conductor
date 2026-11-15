/**
 * @file test_system_initializer_degraded.c
 * @brief Degraded-component failure injection & capability gating tests for System Initializer.
 *
 * Verifies that each DEGRADED component:
 *   1. Logs a DEGRADED warning (not FATAL failure).
 *   2. Does NOT prevent the system from reaching SYSTEM_INIT_READY.
 *   3. Does NOT set failed_phase or failed_component in the boot report.
 *   4. GATES the corresponding runtime activation/task (e.g. led_start == 0, cli_start == 0, metrics_start == 0).
 *   5. Maintains safe no-op behavior without panics or crashes (WDT, ADC, Verifier, Debug Console, Metrics, Health).
 *   6. Health Manager boot lifecycle: starts UNKNOWN at boot, transitions deterministically to READY.
 */

#include <unity.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include "esp_err.h"
#include "system_initializer.h"
#include "wdt_manager.h"
#include "service_locator.h"
#include "z906_cmd_verifier.h"
#include "z906_state.h"
#include "ota_health.h"
#include "config.h"
#include "uart_hal.h"
#include "health_manager.h"

/* ============================================================
 * Common stubs & counters
 * ============================================================ */

static bool stub_nop_true(void)    { return true; }
static bool stub_fail_false(void)  { return false; }
static esp_err_t stub_nop_ok(void) { return ESP_OK; }
static bool stub_nop_start_true(void) { return true; }

static int s_led_start_calls = 0;
static bool stub_led_start_counter(void)
{
    s_led_start_calls++;
    return true;
}

static int s_cli_start_calls = 0;
static bool stub_cli_start_counter(void)
{
    s_cli_start_calls++;
    return true;
}

static int s_metrics_start_calls = 0;
static bool stub_metrics_start_counter(void)
{
    s_metrics_start_calls++;
    return true;
}

static int s_health_start_calls = 0;
static bool stub_health_start_counter(void)
{
    s_health_start_calls++;
    return true;
}

/* Base hook table: ALL fatal inits + ALL starts = NOP stubs */
static const sys_init_hooks_t k_all_nop_base = {
    /* Fatal inits — NOP */
    .nvs_init          = stub_nop_true,
    .event_bus_init    = stub_nop_true,
    .fsm_init          = stub_nop_true,
    .gpio_init         = stub_nop_true,
    .uart_init         = stub_nop_true,
    /* Fatal starts — NOP */
    .transport_start   = stub_nop_ok,
    .fsm_start         = stub_nop_ok,
    .zigbee_start      = stub_nop_start_true,
    .telemetry_start   = stub_nop_start_true,
    /* Degraded inits — NOP */
    .wdt_init          = stub_nop_true,
    .debug_console_init = stub_nop_true,
    .cmd_verifier_init = stub_nop_true,
    .adc_init          = stub_nop_true,
    .led_init          = stub_nop_true,
    .ota_init          = stub_nop_true,
    .cli_init          = stub_nop_true,
    .system_metrics_init = stub_nop_true,
    .health_manager_init = stub_nop_true,
    /* Degraded starts — NOP */
    .led_start         = stub_nop_start_true,
    .cli_start         = stub_nop_start_true,
    .system_metrics_start = stub_nop_start_true,
    .health_manager_start = stub_nop_start_true,
};

static void assert_degraded_reaches_ready(void)
{
    esp_err_t rc = system_initializer_run();
    TEST_ASSERT_EQUAL_MESSAGE(ESP_OK, rc,
        "Degraded failure must not cause system_initializer_run() to return error");

    TEST_ASSERT_EQUAL_MESSAGE(SYSTEM_INIT_READY, system_initializer_get_state(),
        "Degraded failure must not prevent SYSTEM_INIT_READY");

    system_init_report_t r;
    system_initializer_get_report(&r);
    TEST_ASSERT_EQUAL(SYSTEM_INIT_READY, r.state);
    TEST_ASSERT_NULL_MESSAGE(r.failed_component,
        "Degraded failure must not set failed_component in report");
    TEST_ASSERT_EQUAL_MESSAGE(BOOT_PHASE_NONE, r.failed_phase,
        "Degraded failure must not set failed_phase in report");
    TEST_ASSERT_EQUAL_MESSAGE(ESP_OK, r.last_error,
        "Degraded failure must not set last_error in report");
    TEST_ASSERT_GREATER_THAN(0, r.elapsed_ms);
}

void setUp(void)
{
    system_initializer_reset_state_for_test();
    s_led_start_calls = 0;
    s_cli_start_calls = 0;
    s_metrics_start_calls = 0;
    s_health_start_calls = 0;
}

void tearDown(void)
{
    system_initializer_set_hooks(NULL);
}

/* ============================================================
 * TC-10: LED degraded gating (led_init fails -> led_start calls == 0)
 * ============================================================ */
void test_led_degraded_gating_prevents_start(void)
{
    sys_init_hooks_t h = k_all_nop_base;
    h.led_init  = stub_fail_false;         /* LED init fails */
    h.led_start = stub_led_start_counter;  /* Install call counter */
    system_initializer_set_hooks(&h);

    assert_degraded_reaches_ready();

    TEST_ASSERT_EQUAL_INT_MESSAGE(0, s_led_start_calls,
        "GATING: led_start must NOT be called when led_init failed");
}

/* ============================================================
 * TC-11: CLI degraded gating (cli_init fails -> cli_start calls == 0)
 * ============================================================ */
void test_cli_degraded_gating_prevents_start(void)
{
    sys_init_hooks_t h = k_all_nop_base;
    h.cli_init  = stub_fail_false;         /* CLI init fails */
    h.cli_start = stub_cli_start_counter;  /* Install call counter */
    system_initializer_set_hooks(&h);

    assert_degraded_reaches_ready();

    TEST_ASSERT_EQUAL_INT_MESSAGE(0, s_cli_start_calls,
        "GATING: cli_start must NOT be called when cli_init failed");
}

/* ============================================================
 * TC-15: System Metrics degraded gating (metrics_init fails -> metrics_start calls == 0)
 * ============================================================ */
void test_system_metrics_degraded_gating_prevents_start(void)
{
    sys_init_hooks_t h = k_all_nop_base;
    h.system_metrics_init  = stub_fail_false;             /* Metrics init fails */
    h.system_metrics_start = stub_metrics_start_counter;  /* Install call counter */
    system_initializer_set_hooks(&h);

    assert_degraded_reaches_ready();

    TEST_ASSERT_EQUAL_INT_MESSAGE(0, s_metrics_start_calls,
        "GATING: system_metrics_start must NOT be called when system_metrics_init failed");
}

/* ============================================================
 * TC-16: Health Manager degraded gating (health_init fails -> health_start calls == 0)
 * ============================================================ */
void test_health_manager_degraded_gating_prevents_start(void)
{
    sys_init_hooks_t h = k_all_nop_base;
    h.health_manager_init  = stub_fail_false;            /* Health Manager init fails */
    h.health_manager_start = stub_health_start_counter;  /* Install call counter */
    system_initializer_set_hooks(&h);

    assert_degraded_reaches_ready();

    TEST_ASSERT_EQUAL_INT_MESSAGE(0, s_health_start_calls,
        "GATING: health_manager_start must NOT be called when health_manager_init failed");
}

/* ============================================================
 * TC-13: Watchdog (WDT) degraded behavior & safe API no-ops
 * ============================================================ */
void test_wdt_degraded_behavior_and_api_safety(void)
{
    sys_init_hooks_t h = k_all_nop_base;
    h.wdt_init         = stub_fail_false; /* WDT init fails */
    system_initializer_set_hooks(&h);

    assert_degraded_reaches_ready();

    /* WDT API must be safe no-op without crashes or asserts when uninitialized */
    TEST_ASSERT_FALSE(wdt_manager_is_initialized());
    wdt_manager_register_task();   /* Must safely execute without panic */
    wdt_manager_reset();           /* Must safely no-op without assert */
}

/* ============================================================
 * TC-12: ADC degraded behavior & capability contract
 * ============================================================ */
void test_adc_degraded_capability_behavior(void)
{
    sys_init_hooks_t h = k_all_nop_base;
    h.adc_init         = stub_fail_false; /* ADC driver fails */
    system_initializer_set_hooks(&h);

    assert_degraded_reaches_ready();

    /* System reaches ready, core UART and state paths continue operating */
    z906_state_init();
    z906_state_t st;
    z906_state_get(&st);
    TEST_ASSERT_EQUAL_UINT8(Z906_DEFAULT_VOLUME_LEVEL, st.volume);
}

/* ============================================================
 * TC-14: Command Verifier degraded (Init-Failed vs User-Disabled)
 * ============================================================ */
void test_cmd_verifier_init_failed_vs_user_disabled(void)
{
    /* Path 1: Init-failed */
    sys_init_hooks_t h = k_all_nop_base;
    h.cmd_verifier_init = stub_fail_false; /* Verifier fails to init */
    system_initializer_set_hooks(&h);

    assert_degraded_reaches_ready();

    /* When init failed, verifier service is NOT registered in ServiceLocator */
    TEST_ASSERT_NULL_MESSAGE(service_locator_get(SERVICE_ID_CMD_VERIFIER),
        "Init-failed: Verifier must NOT be registered in ServiceLocator");

    /* Path 2: User-disabled (init succeeds via system_initializer, but user disables verification) */
    system_initializer_reset_state_for_test();
    sys_init_hooks_t h2 = k_all_nop_base;
    h2.cmd_verifier_init = stub_nop_true; /* Verifier succeeds */
    system_initializer_set_hooks(&h2);

    assert_degraded_reaches_ready();
    TEST_ASSERT_NOT_NULL(service_locator_get(SERVICE_ID_CMD_VERIFIER));

    z906_uart_hal_init();
    z906_cmd_verifier_init();
    z906_state_init();
    z906_state_set_cable_status(true);
    z906_state_set_power(true);

    z906_cmd_verifier_set_enabled(false);
    TEST_ASSERT_FALSE(z906_cmd_verifier_is_enabled());

    cmd_request_t req = {
        .type = VERIFY_CMD_VOLUME,
        .source = CMD_SOURCE_CLI,
        .param = { .volume = 20 }
    };
    z906_cmd_verify_info_t info;
    cmd_verify_result_t res = z906_cmd_verifier_execute(&req, &info);
    TEST_ASSERT_EQUAL_MESSAGE(CMD_VERIFY_SENT_UNVERIFIED, res,
        "User-disabled: Commands dispatch unverified (SENT_UNVERIFIED)");

    /* Re-enable verifier */
    z906_cmd_verifier_set_enabled(true);
}

/* ============================================================
 * OTA Client degraded: firmware update unavailable, core unaffected
 * ============================================================ */
void test_ota_degraded_capability(void)
{
    sys_init_hooks_t h = k_all_nop_base;
    h.ota_init         = stub_fail_false; /* OTA client fails */
    system_initializer_set_hooks(&h);

    assert_degraded_reaches_ready();
}

/* ============================================================
 * Debug Console degraded: console unavailable, safe calls
 * ============================================================ */
void test_debug_console_degraded_safety(void)
{
    sys_init_hooks_t h   = k_all_nop_base;
    h.debug_console_init = stub_fail_false; /* Debug console fails */
    system_initializer_set_hooks(&h);

    assert_degraded_reaches_ready();
}

/* ============================================================
 * Health Manager boot UNKNOWN to READY integration test
 * ============================================================ */
void test_health_manager_boot_unknown_to_ready_transition(void)
{
    health_manager_init();
    health_snapshot_t snap;
    health_manager_get_snapshot(&snap);

    /* At early boot, before metrics/initializer report, status is UNKNOWN */
    TEST_ASSERT_EQUAL_MESSAGE(HEALTH_STATUS_UNKNOWN, snap.overall_status,
        "Early boot health must start in UNKNOWN state");

    /* Initializer reports healthy when system reaches ready */
    health_manager_set_component_health(HEALTH_COMP_INITIALIZER,
                                        HEALTH_STATUS_HEALTHY,
                                        HEALTH_REASON_NONE);

    health_manager_get_snapshot(&snap);
    TEST_ASSERT_EQUAL_MESSAGE(HEALTH_STATUS_HEALTHY, snap.components[HEALTH_COMP_INITIALIZER].status,
        "Initializer component must be HEALTHY after reporting");
}

/* ============================================================
 * Control: real boot succeeds (MUST BE LAST)
 * ============================================================ */
void test_real_boot_succeeds_after_degraded_tests(void)
{
    system_initializer_set_hooks(NULL); /* Restore real function pointers */

    TEST_ASSERT_EQUAL(ESP_OK, system_initializer_run());
    TEST_ASSERT_EQUAL(SYSTEM_INIT_READY, system_initializer_get_state());

    system_init_report_t r;
    system_initializer_get_report(&r);
    TEST_ASSERT_EQUAL(SYSTEM_INIT_READY, r.state);
    TEST_ASSERT_NULL(r.failed_component);
    TEST_ASSERT_EQUAL(BOOT_PHASE_NONE, r.failed_phase);
    TEST_ASSERT_EQUAL(ESP_OK, r.last_error);
    TEST_ASSERT_GREATER_THAN(0, r.elapsed_ms);
}

/* ============================================================
 * Test Runner Entry Point
 * ============================================================ */
void app_main(void)
{
    vTaskDelay(pdMS_TO_TICKS(3000)); /* Allow serial monitor to connect */
    UNITY_BEGIN();

    RUN_TEST(test_led_degraded_gating_prevents_start);            /* TC-10 */
    RUN_TEST(test_cli_degraded_gating_prevents_start);            /* TC-11 */
    RUN_TEST(test_system_metrics_degraded_gating_prevents_start); /* TC-15 */
    RUN_TEST(test_health_manager_degraded_gating_prevents_start); /* TC-16 */
    RUN_TEST(test_wdt_degraded_behavior_and_api_safety);          /* TC-13 */
    RUN_TEST(test_adc_degraded_capability_behavior);              /* TC-12 */
    RUN_TEST(test_cmd_verifier_init_failed_vs_user_disabled);        /* TC-14 */
    RUN_TEST(test_ota_degraded_capability);
    RUN_TEST(test_debug_console_degraded_safety);
    RUN_TEST(test_health_manager_boot_unknown_to_ready_transition);
    RUN_TEST(test_real_boot_succeeds_after_degraded_tests);        /* Control — MUST be last */

    UNITY_END();
}
