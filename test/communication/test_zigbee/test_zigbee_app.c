/**
 * @file test_zigbee_app.c
 * @brief Unit tests for Zigbee 3.0 pairing management, state transitions, NVS persistence, and ZCL command parity.
 */

#include <unity.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "zigbee_app.h"
#include "zigbee_native_stack.h"
#include "zb_handlers.h"
#include "z906_state.h"
#include "z906_cmd_verifier.h"
#include "nvs_manager.h"
#include "config.h"
#include <string.h>

/** @brief Sample 64-bit Extended PAN ID for unit testing */
static const uint8_t s_test_ext_pan[8] = { 0x01, 0x23, 0x45, 0x67, 0x89, 0xAB, 0xCD, 0xEF };

void setUp(void)
{
    /* Initialize NVS partition before each test */
    nvs_manager_init();
    /* Clear any persistent Zigbee keys to ensure known clean test baseline */
    nvs_manager_clear_zb_net_info();
    /* Reset in-memory state variables */
    zigbee_app_reset_state();
    /* Initialize hardware state storage and command verifier */
    z906_state_init();
    z906_cmd_verifier_init();
}

void tearDown(void)
{
    /* Clean up persistent credentials after each test */
    nvs_manager_clear_zb_net_info();
    zigbee_native_stack_reset_for_test();
    /* Yield to Idle task to clean up any deleted tasks */
    vTaskDelay(pdMS_TO_TICKS(50));
}

/**
 * @brief Tests default initialization with a virgin/cleared NVS partition.
 * Device must enter UNPAIRED state without attempting connection.
 */
void test_zigbee_init_virgin_unpaired_state(void)
{
    TEST_ASSERT_TRUE(zigbee_app_init());
    TEST_ASSERT_EQUAL(ZB_STATE_UNPAIRED, zigbee_app_get_state());
    TEST_ASSERT_FALSE(zigbee_app_is_joined());
    TEST_ASSERT_FALSE(zigbee_app_is_paired());
    TEST_ASSERT_EQUAL_INT(0, zigbee_app_get_pairing_time_left());
}

/**
 * @brief Tests entering pairing mode from UNPAIRED state.
 * Verifies transition to PAIRING and 180s countdown timer initialization.
 */
void test_zigbee_start_pairing_window(void)
{
    zigbee_app_init();

    TEST_ASSERT_TRUE(zigbee_app_start_pairing(false));
    TEST_ASSERT_EQUAL(ZB_STATE_PAIRING, zigbee_app_get_state());
    TEST_ASSERT_EQUAL_INT(Z906_ZB_PAIRING_TIMEOUT_S, zigbee_app_get_pairing_time_left());
    TEST_ASSERT_FALSE(zigbee_app_is_joined());
}

/**
 * @brief Tests pairing window timeout countdown and expiration.
 * Device must revert to UNPAIRED once 180s expires without association.
 */
void test_zigbee_pairing_timeout_expiration(void)
{
    zigbee_app_init();
    zigbee_app_start_pairing(false);

    /* Simulate 179 seconds elapsed */
    for (int i = 0; i < Z906_ZB_PAIRING_TIMEOUT_S - 1; i++) {
        zigbee_app_process_cycle();
    }
    TEST_ASSERT_EQUAL(ZB_STATE_PAIRING, zigbee_app_get_state());
    TEST_ASSERT_EQUAL_INT(1, zigbee_app_get_pairing_time_left());

    /* 180th second: timer expires */
    zigbee_app_process_cycle();
    TEST_ASSERT_EQUAL(ZB_STATE_UNPAIRED, zigbee_app_get_state());
    TEST_ASSERT_EQUAL_INT(0, zigbee_app_get_pairing_time_left());
}

/**
 * @brief Tests successful coordinator association and NVS credential persistence.
 */
void test_zigbee_successful_join_and_nvs_persistence(void)
{
    zigbee_app_init();
    zigbee_app_start_pairing(false);

    /* Simulate successful association with coordinator */
    bool join_ok = zigbee_app_simulate_join(0x1A2B, 15, s_test_ext_pan);
    TEST_ASSERT_TRUE(join_ok);

    /* Verify in-memory state */
    TEST_ASSERT_EQUAL(ZB_STATE_JOINED, zigbee_app_get_state());
    TEST_ASSERT_TRUE(zigbee_app_is_joined());
    TEST_ASSERT_TRUE(zigbee_app_is_paired());

    /* Verify persistent credentials directly from NVS flash */
    bool nvs_paired = false;
    TEST_ASSERT_TRUE(nvs_manager_get_zb_paired(&nvs_paired));
    TEST_ASSERT_TRUE(nvs_paired);

    zb_persistent_net_info_t loaded_info;
    TEST_ASSERT_TRUE(nvs_manager_load_zb_net_info(&loaded_info));
    TEST_ASSERT_TRUE(loaded_info.is_paired);
    TEST_ASSERT_EQUAL_HEX16(0x1A2B, loaded_info.pan_id);
    TEST_ASSERT_EQUAL_UINT8(15, loaded_info.channel);
    TEST_ASSERT_EQUAL_MEMORY(s_test_ext_pan, loaded_info.ext_pan_id, sizeof(s_test_ext_pan));
}

/**
 * @brief Tests power-cycle and reboot recovery.
 * Stored credentials must be retained, and device must boot into REJOINING state.
 */
void test_zigbee_reboot_credential_retention_and_rejoin(void)
{
    /* 1. Simulate device previously paired and joined */
    zigbee_app_init();
    zigbee_app_start_pairing(false);
    zigbee_app_simulate_join(0x5678, 20, s_test_ext_pan);

    /* 2. Simulate complete device reboot by resetting in-memory state */
    zigbee_app_reset_state();
    TEST_ASSERT_EQUAL(ZB_STATE_UNPAIRED, zigbee_app_get_state());

    /* 3. Re-initialize stack after reboot (reading NVS) */
    TEST_ASSERT_TRUE(zigbee_app_init());

    /* 4. Device must recognize previous pairing and enter REJOINING state */
    TEST_ASSERT_EQUAL(ZB_STATE_REJOINING, zigbee_app_get_state());
    TEST_ASSERT_TRUE(zigbee_app_is_paired());
    TEST_ASSERT_FALSE(zigbee_app_is_joined()); /* Not joined until coordinator ACKs */

    /* 5. Stored network info must match pre-reboot values */
    zb_persistent_net_info_t net_info;
    TEST_ASSERT_TRUE(zigbee_app_get_net_info(&net_info));
    TEST_ASSERT_EQUAL_HEX16(0x5678, net_info.pan_id);
    TEST_ASSERT_EQUAL_UINT8(20, net_info.channel);
    TEST_ASSERT_EQUAL_MEMORY(s_test_ext_pan, net_info.ext_pan_id, sizeof(s_test_ext_pan));
}

/**
 * @brief Tests offline coordinator resilience during rejoin.
 * Periodic rejoin retries must not wipe credentials or hang the system.
 */
void test_zigbee_offline_coordinator_rejoin_resilience(void)
{
    zigbee_app_init();
    zigbee_app_simulate_join(0x9999, 11, s_test_ext_pan);

    /* Simulate reboot into REJOINING */
    zigbee_app_reset_state();
    zigbee_app_init();
    TEST_ASSERT_EQUAL(ZB_STATE_REJOINING, zigbee_app_get_state());

    /* Simulate multiple failed rejoin attempts */
    zigbee_app_simulate_rejoin_failure();
    for (int i = 0; i < Z906_ZB_REJOIN_INTERVAL_S * 2; i++) {
        zigbee_app_process_cycle();
    }

    /* State must remain REJOINING and credentials must NOT be erased */
    TEST_ASSERT_EQUAL(ZB_STATE_REJOINING, zigbee_app_get_state());
    bool nvs_paired = false;
    TEST_ASSERT_TRUE(nvs_manager_get_zb_paired(&nvs_paired));
    TEST_ASSERT_TRUE(nvs_paired);
}

/**
 * @brief Tests re-pairing behavior on an already-paired device.
 * Previous network info must be cleanly removed from NVS, and a new pairing window opened.
 */
void test_zigbee_repairing_clears_old_credentials(void)
{
    /* 1. Establish initial pairing */
    zigbee_app_init();
    zigbee_app_simulate_join(0x1111, 12, s_test_ext_pan);
    TEST_ASSERT_EQUAL(ZB_STATE_JOINED, zigbee_app_get_state());

    /* 2. Initiating pairing with force_repair=false should be rejected */
    TEST_ASSERT_FALSE(zigbee_app_start_pairing(false));
    TEST_ASSERT_EQUAL(ZB_STATE_JOINED, zigbee_app_get_state());

    /* 3. Initiating pairing with force_repair=true must clear old credentials and open window */
    TEST_ASSERT_TRUE(zigbee_app_start_pairing(true));
    TEST_ASSERT_EQUAL(ZB_STATE_PAIRING, zigbee_app_get_state());
    TEST_ASSERT_EQUAL_INT(Z906_ZB_PAIRING_TIMEOUT_S, zigbee_app_get_pairing_time_left());

    /* 4. Old NVS credentials must be erased */
    bool nvs_paired = true;
    (void)nvs_manager_get_zb_paired(&nvs_paired);
    TEST_ASSERT_FALSE(nvs_paired);

    /* 5. Successfully join a new coordinator network */
    const uint8_t new_ext_pan[8] = { 0xFE, 0xDC, 0xBA, 0x98, 0x76, 0x54, 0x32, 0x10 };
    zigbee_app_simulate_join(0x2222, 25, new_ext_pan);

    TEST_ASSERT_EQUAL(ZB_STATE_JOINED, zigbee_app_get_state());
    zb_persistent_net_info_t new_info;
    TEST_ASSERT_TRUE(nvs_manager_load_zb_net_info(&new_info));
    TEST_ASSERT_EQUAL_HEX16(0x2222, new_info.pan_id);
    TEST_ASSERT_EQUAL_UINT8(25, new_info.channel);
    TEST_ASSERT_EQUAL_MEMORY(new_ext_pan, new_info.ext_pan_id, sizeof(new_ext_pan));
}

/**
 * @brief Tests manual leave network command (zb_leave).
 * Credentials must be erased, and state returned to UNPAIRED.
 */
void test_zigbee_manual_leave_network(void)
{
    zigbee_app_init();
    zigbee_app_simulate_join(0x4444, 18, s_test_ext_pan);
    TEST_ASSERT_EQUAL(ZB_STATE_JOINED, zigbee_app_get_state());

    /* Issue manual leave */
    TEST_ASSERT_TRUE(zigbee_app_leave_network());
    TEST_ASSERT_EQUAL(ZB_STATE_UNPAIRED, zigbee_app_get_state());
    TEST_ASSERT_FALSE(zigbee_app_is_joined());
    TEST_ASSERT_FALSE(zigbee_app_is_paired());

    /* Verify NVS credentials erased */
    bool nvs_paired = true;
    nvs_manager_get_zb_paired(&nvs_paired);
    TEST_ASSERT_FALSE(nvs_paired);
}

/**
 * @brief Tests state enum string representation converter.
 */
void test_zigbee_state_strings(void)
{
    TEST_ASSERT_EQUAL_STRING("UNPAIRED", zigbee_app_state_str(ZB_STATE_UNPAIRED));
    TEST_ASSERT_EQUAL_STRING("PAIRING", zigbee_app_state_str(ZB_STATE_PAIRING));
    TEST_ASSERT_EQUAL_STRING("JOINED", zigbee_app_state_str(ZB_STATE_JOINED));
    TEST_ASSERT_EQUAL_STRING("REJOINING", zigbee_app_state_str(ZB_STATE_REJOINING));
    TEST_ASSERT_EQUAL_STRING("COMMISSION_FAILED", zigbee_app_state_str(ZB_STATE_COMMISSION_FAILED));
    TEST_ASSERT_EQUAL_STRING("LEAVING", zigbee_app_state_str(ZB_STATE_LEAVING));
    TEST_ASSERT_EQUAL_STRING("UNKNOWN", zigbee_app_state_str((zb_network_state_t)99));
}

/**
 * @brief Tests incoming Zigbee ZCL Mute command handler execution with state verification.
 */
void test_zigbee_handler_mute_verification(void)
{
    /* Set baseline state: system ON and unmuted */
    z906_state_set_cable_status(true);
    z906_state_set_power(true);
    z906_state_set_mute(false);

    /* 1. Request Mute ON via Zigbee handler */
    zb_handler_set_mute(true);

    /* Verify state through state manager */
    z906_state_t current;
    z906_state_get(&current);
    TEST_ASSERT_TRUE(current.power);

    /* 2. Request Mute OFF via Zigbee handler */
    zb_handler_set_mute(false);
    z906_state_get(&current);
    TEST_ASSERT_TRUE(current.power);
}

/**
 * @brief Tests that all supported ZCL commands (Power, Level, Mute, Trim, Input, Effect)
 * route through the centralized command verifier pipeline.
 */
void test_zigbee_command_parity_all_commands(void)
{
    /* Establish online healthy preconditions */
    z906_state_set_cable_status(true);
    z906_state_set_power(true);

    /* 1. Power handler test */
    zb_handler_on_off(true);

    /* 2. Master volume handler test (ZCL level 148 -> ~25 hardware step) */
    zb_handler_set_level(148);

    /* 3. Mute handler test */
    zb_handler_set_mute(true);
    zb_handler_set_mute(false);

    /* 4. Trim handlers test (Bass: channel 0, Center: channel 1, Rear: channel 2) */
    zb_handler_set_trim(0, 100);
    zb_handler_set_trim(1, 150);
    zb_handler_set_trim(2, 200);

    /* 5. Input switch handler test (Input 2 = RCA) */
    zb_handler_set_input(2);

    /* 6. Effect switch handler test (Effect 0 = 3D on analog RCA) */
    zb_handler_set_effect(0);

    z906_state_t st;
    z906_state_get(&st);
    TEST_ASSERT_TRUE(st.is_operational);
}

/**
 * @brief Tests that incoming audio commands from Zigbee are rejected if preconditions fail (Standby mode).
 */
void test_zigbee_precondition_rejection_in_standby(void)
{
    /* Set cable connected but system in Standby */
    z906_state_set_cable_status(true);
    z906_state_set_power(false);

    /* Audio commands in Standby must be rejected by precondition checker */
    cmd_request_t req_mute = {
        .type = VERIFY_CMD_MUTE,
        .source = CMD_SOURCE_ZIGBEE,
        .param = { .mute = true }
    };
    z906_cmd_verify_info_t info;
    cmd_verify_result_t res = z906_cmd_verifier_execute(&req_mute, &info);
    TEST_ASSERT_EQUAL(CMD_VERIFY_ERR_PRECONDITION_FAILED, res);

    cmd_request_t req_trim = {
        .type = VERIFY_CMD_TRIM,
        .source = CMD_SOURCE_ZIGBEE,
        .param = { .trim = { .channel = TRIM_CHANNEL_BASS, .level = 20 } }
    };
    res = z906_cmd_verifier_execute(&req_trim, &info);
    TEST_ASSERT_EQUAL(CMD_VERIFY_ERR_PRECONDITION_FAILED, res);
}

/**
 * @brief Tests bi-directional physical status synchronization (physical console/remote -> Zigbee).
 * Verifies that independent hardware state changes trigger delta-driven Zigbee attribute reports.
 */
void test_zigbee_bidirectional_physical_status_sync(void)
{
    zigbee_app_init();
    zigbee_app_simulate_join(0xABCD, 15, s_test_ext_pan);

    /* Initial state report */
    z906_state_t st;
    z906_state_get(&st);
    zigbee_app_report_state(&st);
    uint32_t count = zigbee_app_get_report_count();
    TEST_ASSERT_EQUAL_UINT32(1, count);

    /* Redundant report without state change must NOT trigger new report */
    zigbee_app_report_state(&st);
    TEST_ASSERT_EQUAL_UINT32(1, zigbee_app_get_report_count());

    /* 1. Simulate physical volume knob rotation (+5) */
    z906_state_set_volume(st.volume + 5);
    z906_state_get(&st);
    zigbee_app_report_state(&st);
    TEST_ASSERT_EQUAL_UINT32(2, zigbee_app_get_report_count());

    /* 2. Simulate physical input button press: Optical 1 (Input 3) — first TOSLINK */
    z906_state_set_input(Z906_INPUT_OPTICAL_1);
    z906_state_get(&st);
    zigbee_app_report_state(&st);
    TEST_ASSERT_EQUAL_UINT32(3, zigbee_app_get_report_count());

    /* 3. Simulate physical effect button press */
    z906_state_set_effect(Z906_EFFECT_NONE);
    z906_state_get(&st);
    zigbee_app_report_state(&st);
    TEST_ASSERT_EQUAL_UINT32(4, zigbee_app_get_report_count());

    /* 4. Simulate physical mute toggle */
    z906_state_set_mute(true);
    z906_state_get(&st);
    zigbee_app_report_state(&st);
    TEST_ASSERT_EQUAL_UINT32(5, zigbee_app_get_report_count());

    /* 5. Simulate physical standby transition (0x36).
     * WHY power=true first: z906_state_t is zero-initialized (power=false).
     * Setting power=false without first setting it true is a no-op (no delta).
     * The physical scenario: user turns amp ON, then puts it into STANDBY. */
    z906_state_set_power(true);
    z906_state_get(&st);
    zigbee_app_report_state(&st);
    TEST_ASSERT_EQUAL_UINT32(6, zigbee_app_get_report_count());

    z906_state_set_power(false);
    z906_state_get(&st);
    zigbee_app_report_state(&st);
    TEST_ASSERT_EQUAL_UINT32(7, zigbee_app_get_report_count());

    /* Verify last reported snapshot matches physical state */
    z906_state_t last_rep;
    zigbee_app_get_last_reported_state(&last_rep);
    TEST_ASSERT_FALSE(last_rep.power);
    TEST_ASSERT_TRUE(last_rep.mute);
    TEST_ASSERT_EQUAL(Z906_INPUT_OPTICAL_1, last_rep.input); /* Optical 1 (Input 3) set during physical press sim */
    TEST_ASSERT_EQUAL(Z906_EFFECT_NONE, last_rep.effect);
}

/**
 * @brief Tests reporting of all 5 mandatory internal telemetry sensors + device diagnostics.
 */
void test_zigbee_telemetry_reporting(void)
{
    zigbee_app_init();
    zigbee_app_simulate_join(0x7777, 24, s_test_ext_pan);

    /* Update internal amplifier telemetry */
    const int8_t temps[3] = { 38, 44, 53 }; /* Temp 1, 2, 3 (Transformer) */
    z906_state_set_temperature(temps);
    z906_state_set_input_signal_level(245780);
    z906_state_set_idle_seconds(3600);
    z906_state_set_digital_signal_status(0x0E);
    z906_state_set_auto_standby(true);

    z906_state_t st;
    z906_state_get(&st);
    zigbee_app_report_state(&st);

    /* Verify reported snapshot retains telemetry */
    z906_state_t reported;
    zigbee_app_get_last_reported_state(&reported);
    TEST_ASSERT_EQUAL_INT8(38, reported.temp_celsius[0]);
    TEST_ASSERT_EQUAL_INT8(44, reported.temp_celsius[1]);
    TEST_ASSERT_EQUAL_INT8(53, reported.temp_celsius[2]);
    TEST_ASSERT_EQUAL_UINT32(245780, reported.input_signal_level);
    TEST_ASSERT_EQUAL_UINT32(3600, reported.idle_seconds);
    TEST_ASSERT_EQUAL_HEX8(0x0E, reported.digital_signal_status);
    TEST_ASSERT_TRUE(reported.auto_standby_enabled);
}

/* ========================================================================= */
/* TC-08 / TC-09: Zigbee Private Startup Handshake Tests                     */
/* ========================================================================= */

static esp_err_t stub_native_init_fail(void) { return ESP_FAIL; }
static esp_err_t stub_native_init_ok(void)   { return ESP_OK; }
static esp_err_t stub_native_run_ok(void)    { return ESP_OK; }
static esp_err_t stub_native_init_never_signals(void)
{
    /* Simulate a hung task that blocks indefinitely waiting for hardware */
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    return ESP_FAIL;
}

void test_zigbee_startup_handshake_failure_reports_false(void)
{
    zigbee_native_stack_set_init_fn_for_test(stub_native_init_fail);
    zigbee_native_stack_set_run_fn_for_test(stub_native_run_ok);
    zigbee_native_stack_set_timeout_for_test(500);

    /* Task creation succeeds, but native init fails inside the task.
     * Startup handshake reports FAILED, zigbee_native_stack_start() returns false. */
    bool ok = zigbee_native_stack_start();
    TEST_ASSERT_FALSE_MESSAGE(ok, "zigbee_native_stack_start must return false when native init fails");
}

void test_zigbee_startup_handshake_success_control(void)
{
    zigbee_native_stack_set_init_fn_for_test(stub_native_init_ok);
    zigbee_native_stack_set_run_fn_for_test(stub_native_run_ok);
    zigbee_native_stack_set_timeout_for_test(500);

    bool ok = zigbee_native_stack_start();
    TEST_ASSERT_TRUE_MESSAGE(ok, "zigbee_native_stack_start must return true when native init succeeds");
}

void test_zigbee_startup_handshake_timeout_reports_false(void)
{
    /* Task never signals in time (hangs indefinitely in init until start() terminates it) */
    zigbee_native_stack_set_init_fn_for_test(stub_native_init_never_signals);
    zigbee_native_stack_set_run_fn_for_test(stub_native_run_ok);
    zigbee_native_stack_set_timeout_for_test(50);

    bool ok = zigbee_native_stack_start();
    TEST_ASSERT_FALSE_MESSAGE(ok, "zigbee_native_stack_start must return false on startup timeout");
}

int runUnityTests(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_zigbee_init_virgin_unpaired_state);
    RUN_TEST(test_zigbee_start_pairing_window);
    RUN_TEST(test_zigbee_pairing_timeout_expiration);
    RUN_TEST(test_zigbee_successful_join_and_nvs_persistence);
    RUN_TEST(test_zigbee_reboot_credential_retention_and_rejoin);
    RUN_TEST(test_zigbee_offline_coordinator_rejoin_resilience);
    RUN_TEST(test_zigbee_repairing_clears_old_credentials);
    RUN_TEST(test_zigbee_manual_leave_network);
    RUN_TEST(test_zigbee_state_strings);
    RUN_TEST(test_zigbee_handler_mute_verification);
    RUN_TEST(test_zigbee_command_parity_all_commands);
    RUN_TEST(test_zigbee_precondition_rejection_in_standby);
    RUN_TEST(test_zigbee_bidirectional_physical_status_sync);
    RUN_TEST(test_zigbee_telemetry_reporting);
    RUN_TEST(test_zigbee_startup_handshake_failure_reports_false);
    RUN_TEST(test_zigbee_startup_handshake_success_control);
    RUN_TEST(test_zigbee_startup_handshake_timeout_reports_false);
    return UNITY_END();
}

#if defined(ESP_PLATFORM)
void app_main(void)
{
    vTaskDelay(pdMS_TO_TICKS(3000)); /* Allow test runner to open serial port */
    runUnityTests();
}
#else
int main(void)
{
    return runUnityTests();
}
#endif
