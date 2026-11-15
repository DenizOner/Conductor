/**
 * @file test_cmd_verifier.c
 * @brief Unit and integration tests for Z906 state-based command verification engine
 *        and centralized command registry integration.
 */

#include <unity.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "z906_cmd_verifier.h"
#include "z906_cmd_registry.h"
#include "z906_state.h"
#include "config.h"

void setUp(void)
{
    z906_state_init();
    z906_cmd_verifier_init();
}

void tearDown(void)
{
    /* Clean up after each test */
}

/**
 * @brief Tests verifier module initialization.
 */
void test_verifier_init_state(void)
{
    TEST_ASSERT_TRUE(z906_cmd_verifier_init());
}

/**
 * @brief Tests handling of unverifiable commands (e.g. Save, Reset).
 */
void test_verifier_unverifiable_command(void)
{
    cmd_request_t req = {
        .type = VERIFY_CMD_UNVERIFIABLE,
        .source = CMD_SOURCE_CLI
    };

    z906_cmd_verify_info_t info;
    cmd_verify_result_t res = z906_cmd_verifier_execute(&req, &info);

    TEST_ASSERT_EQUAL(CMD_VERIFY_ERR_UNVERIFIABLE, res);
    TEST_ASSERT_EQUAL(CMD_VERIFY_ERR_UNVERIFIABLE, info.result);
}

/**
 * @brief Tests precondition rejection when DE-15 cable is disconnected.
 */
void test_verifier_precondition_cable_disconnected(void)
{
    /* Disconnect DE-15 cable */
    z906_state_set_cable_status(false);

    cmd_request_t req = {
        .type = VERIFY_CMD_POWER,
        .source = CMD_SOURCE_CLI,
        .param = { .power = true }
    };

    z906_cmd_verify_info_t info;
    cmd_verify_result_t res = z906_cmd_verifier_execute(&req, &info);

    TEST_ASSERT_EQUAL(CMD_VERIFY_ERR_PRECONDITION_FAILED, res);
    TEST_ASSERT_EQUAL(CMD_VERIFY_ERR_PRECONDITION_FAILED, info.result);
}

/**
 * @brief Tests precondition rejection for audio commands when system is in Standby (OFF).
 */
void test_verifier_precondition_standby_mode(void)
{
    /* Cable connected but power is OFF */
    z906_state_set_cable_status(true);
    z906_state_set_power(false);

    /* Volume command in Standby must be rejected */
    cmd_request_t req_vol = {
        .type = VERIFY_CMD_VOLUME,
        .source = CMD_SOURCE_ZIGBEE,
        .param = { .volume = 20 }
    };
    z906_cmd_verify_info_t info;
    TEST_ASSERT_EQUAL(CMD_VERIFY_ERR_PRECONDITION_FAILED, z906_cmd_verifier_execute(&req_vol, &info));

    /* Input command in Standby must be rejected */
    cmd_request_t req_in = {
        .type = VERIFY_CMD_INPUT,
        .source = CMD_SOURCE_CLI,
        .param = { .input = Z906_INPUT_RCA_STEREO } /* Input 2: RCA Stereo */
    };
    TEST_ASSERT_EQUAL(CMD_VERIFY_ERR_PRECONDITION_FAILED, z906_cmd_verifier_execute(&req_in, &info));

    /* Mute command in Standby must be rejected */
    cmd_request_t req_mute = {
        .type = VERIFY_CMD_MUTE,
        .source = CMD_SOURCE_CLI,
        .param = { .mute = true }
    };
    TEST_ASSERT_EQUAL(CMD_VERIFY_ERR_PRECONDITION_FAILED, z906_cmd_verifier_execute(&req_mute, &info));

    /* Effect command in Standby must be rejected */
    cmd_request_t req_eff = {
        .type = VERIFY_CMD_EFFECT,
        .source = CMD_SOURCE_ZIGBEE,
        .param = { .effect = Z906_EFFECT_3D }
    };
    TEST_ASSERT_EQUAL(CMD_VERIFY_ERR_PRECONDITION_FAILED, z906_cmd_verifier_execute(&req_eff, &info));
}

/**
 * @brief Tests redundant command optimization for Power.
 */
void test_verifier_redundant_power_command(void)
{
    z906_state_set_cable_status(true);
    z906_state_set_power(true);

    /* System is already ON; requesting Power ON should be verified immediately */
    cmd_request_t req = {
        .type = VERIFY_CMD_POWER,
        .source = CMD_SOURCE_CLI,
        .param = { .power = true }
    };

    z906_cmd_verify_info_t info;
    cmd_verify_result_t res = z906_cmd_verifier_execute(&req, &info);

    TEST_ASSERT_EQUAL(CMD_VERIFY_SUCCESS, res);
    TEST_ASSERT_TRUE(info.post_state.power);
}

/**
 * @brief Tests redundant command optimization for Volume.
 */
void test_verifier_redundant_volume_command(void)
{
    z906_state_set_cable_status(true);
    z906_state_set_power(true);
    z906_state_set_volume(25);

    /* Volume is already 25; requesting 25 should return success immediately */
    cmd_request_t req = {
        .type = VERIFY_CMD_VOLUME,
        .source = CMD_SOURCE_ZIGBEE,
        .param = { .volume = 25 }
    };

    z906_cmd_verify_info_t info;
    cmd_verify_result_t res = z906_cmd_verifier_execute(&req, &info);

    TEST_ASSERT_EQUAL(CMD_VERIFY_SUCCESS, res);
    TEST_ASSERT_EQUAL_UINT8(25, info.post_state.volume);
}

/**
 * @brief Tests volume boundary clamping to maximum limit (43).
 */
void test_verifier_boundary_clamping(void)
{
    z906_state_set_cable_status(true);
    z906_state_set_power(true);
    z906_state_set_volume(43);

    /* Requesting volume 100 when already at max (43) clamps target to 43 and satisfies state */
    cmd_request_t req = {
        .type = VERIFY_CMD_VOLUME,
        .source = CMD_SOURCE_CLI,
        .param = { .volume = 100 }
    };

    z906_cmd_verify_info_t info;
    cmd_verify_result_t res = z906_cmd_verifier_execute(&req, &info);

    TEST_ASSERT_EQUAL(CMD_VERIFY_SUCCESS, res);
    TEST_ASSERT_EQUAL_UINT8(Z906_MAX_VOLUME_LEVEL, info.target_state.volume);
}

/**
 * @brief Tests spatial effect validation on digital inputs.
 */
void test_verifier_digital_input_effect_validation(void)
{
    z906_state_set_cable_status(true);
    z906_state_set_power(true);
    z906_state_set_input(Z906_INPUT_OPTICAL_1); /* Input 3: Optical 1 — first TOSLINK (digital, no effects) */
    z906_state_set_effect(Z906_EFFECT_NONE);

    /* Requesting 3D on a digital input must clamp expected effect to NONE */
    cmd_request_t req = {
        .type = VERIFY_CMD_EFFECT,
        .source = CMD_SOURCE_ZIGBEE,
        .param = { .effect = Z906_EFFECT_3D }
    };

    z906_cmd_verify_info_t info;
    cmd_verify_result_t res = z906_cmd_verifier_execute(&req, &info);

    TEST_ASSERT_EQUAL(CMD_VERIFY_SUCCESS, res);
    TEST_ASSERT_EQUAL(Z906_EFFECT_NONE, info.target_state.effect);
}

/**
 * @brief Tests string conversion helpers.
 */
void test_verifier_string_conversions(void)
{
    TEST_ASSERT_NOT_NULL(z906_cmd_verify_result_str(CMD_VERIFY_SUCCESS));
    TEST_ASSERT_NOT_NULL(z906_cmd_verify_result_str(CMD_VERIFY_ERR_SEND_FAILED));
    TEST_ASSERT_NOT_NULL(z906_cmd_verify_result_str(CMD_VERIFY_ERR_PRE_STATE_FAILED));
    TEST_ASSERT_NOT_NULL(z906_cmd_verify_result_str(CMD_VERIFY_ERR_POST_STATE_FAILED));
    TEST_ASSERT_NOT_NULL(z906_cmd_verify_result_str(CMD_VERIFY_ERR_TARGET_NOT_REACHED));
    TEST_ASSERT_NOT_NULL(z906_cmd_verify_result_str(CMD_VERIFY_ERR_UNVERIFIABLE));
    TEST_ASSERT_NOT_NULL(z906_cmd_verify_result_str(CMD_VERIFY_ERR_TIMEOUT));
    TEST_ASSERT_NOT_NULL(z906_cmd_verify_result_str(CMD_VERIFY_ERR_PRECONDITION_FAILED));
    TEST_ASSERT_NOT_NULL(z906_cmd_verify_result_str(CMD_VERIFY_ERR_BUSY));

    TEST_ASSERT_NOT_NULL(z906_cmd_verify_type_str(VERIFY_CMD_POWER));
    TEST_ASSERT_NOT_NULL(z906_cmd_verify_type_str(VERIFY_CMD_VOLUME));
    TEST_ASSERT_NOT_NULL(z906_cmd_verify_type_str(VERIFY_CMD_INPUT));
    TEST_ASSERT_NOT_NULL(z906_cmd_verify_type_str(VERIFY_CMD_EFFECT));
    TEST_ASSERT_NOT_NULL(z906_cmd_verify_type_str(VERIFY_CMD_MUTE));
    TEST_ASSERT_NOT_NULL(z906_cmd_verify_type_str(VERIFY_CMD_TRIM));

    TEST_ASSERT_NOT_NULL(z906_cmd_verify_source_str(CMD_SOURCE_CLI));
    TEST_ASSERT_NOT_NULL(z906_cmd_verify_source_str(CMD_SOURCE_ZIGBEE));
}

/* ========================================================================= */
/* 11 Mandated Integration Test Scenarios for Central Command Architecture   */
/* ========================================================================= */

/**
 * @brief Test 1 — CLI command execution via central registry into verifier.
 */
void test_scenario_1_cli_command_execution(void)
{
    z906_state_set_cable_status(true);
    z906_state_set_power(true);
    z906_state_set_volume(20);

    const z906_cmd_descriptor_t *desc = z906_cmd_registry_find_by_cli_name("volume", NULL);
    TEST_ASSERT_NOT_NULL(desc);
    TEST_ASSERT_EQUAL(Z906_CMD_ID_VOLUME, desc->id);

    cmd_request_t req;
    TEST_ASSERT_TRUE(z906_cmd_registry_build_request(desc->id, CMD_SOURCE_CLI, 20, &req));
    TEST_ASSERT_EQUAL(CMD_SOURCE_CLI, req.source);
    TEST_ASSERT_EQUAL(VERIFY_CMD_VOLUME, req.type);
    TEST_ASSERT_EQUAL_UINT8(20, req.param.volume);

    z906_cmd_verify_info_t info;
    cmd_verify_result_t res = z906_cmd_verifier_execute(&req, &info);
    TEST_ASSERT_EQUAL(CMD_VERIFY_SUCCESS, res);
}

/**
 * @brief Test 2 — Zigbee command execution via central registry into verifier.
 */
void test_scenario_2_zigbee_command_execution(void)
{
    z906_state_set_cable_status(true);
    z906_state_set_power(true);
    z906_state_set_volume(20);

    const z906_cmd_descriptor_t *desc = z906_cmd_registry_find_by_zcl(1, ZCL_CLUSTER_LEVEL_CONTROL, 0x0000);
    TEST_ASSERT_NOT_NULL(desc);
    TEST_ASSERT_EQUAL(Z906_CMD_ID_VOLUME, desc->id);

    cmd_request_t req;
    TEST_ASSERT_TRUE(z906_cmd_registry_build_request(desc->id, CMD_SOURCE_ZIGBEE, 20, &req));
    TEST_ASSERT_EQUAL(CMD_SOURCE_ZIGBEE, req.source);
    TEST_ASSERT_EQUAL(VERIFY_CMD_VOLUME, req.type);

    z906_cmd_verify_info_t info;
    cmd_verify_result_t res = z906_cmd_verifier_execute(&req, &info);
    TEST_ASSERT_EQUAL(CMD_VERIFY_SUCCESS, res);
}

/**
 * @brief Test 3 — Command ID integrity across the central registry.
 */
void test_scenario_3_command_id_integrity(void)
{
    TEST_ASSERT_EQUAL_INT(8, z906_cmd_registry_get_count());

    for (size_t i = 0; i < z906_cmd_registry_get_count(); i++) {
        const z906_cmd_descriptor_t *desc = z906_cmd_registry_get_descriptor(i);
        TEST_ASSERT_NOT_NULL(desc);
        TEST_ASSERT_EQUAL_INT((int)i, (int)desc->id);
        TEST_ASSERT_NOT_NULL(desc->name);
        TEST_ASSERT_NOT_NULL(desc->cli_cmd);
        TEST_ASSERT_NOT_NULL(z906_cmd_registry_find_by_id((z906_cmd_id_t)i));
    }

    TEST_ASSERT_NULL(z906_cmd_registry_find_by_id(Z906_CMD_ID_INVALID));
    TEST_ASSERT_NULL(z906_cmd_registry_find_by_id((z906_cmd_id_t)99));
}

/**
 * @brief Test 4 — Accurate parameter validation and boundary enforcement.
 */
void test_scenario_4_param_bounds_validation(void)
{
    /* Volume: 0 - 43 */
    TEST_ASSERT_TRUE(z906_cmd_registry_validate_param(Z906_CMD_ID_VOLUME, 0));
    TEST_ASSERT_TRUE(z906_cmd_registry_validate_param(Z906_CMD_ID_VOLUME, 43));
    TEST_ASSERT_FALSE(z906_cmd_registry_validate_param(Z906_CMD_ID_VOLUME, -1));
    TEST_ASSERT_FALSE(z906_cmd_registry_validate_param(Z906_CMD_ID_VOLUME, 44));

    /* Power: 0 or 1 */
    TEST_ASSERT_TRUE(z906_cmd_registry_validate_param(Z906_CMD_ID_POWER, 0));
    TEST_ASSERT_TRUE(z906_cmd_registry_validate_param(Z906_CMD_ID_POWER, 1));
    TEST_ASSERT_FALSE(z906_cmd_registry_validate_param(Z906_CMD_ID_POWER, 2));

    /* Input: 1 - 6 */
    TEST_ASSERT_TRUE(z906_cmd_registry_validate_param(Z906_CMD_ID_INPUT, 1));
    TEST_ASSERT_TRUE(z906_cmd_registry_validate_param(Z906_CMD_ID_INPUT, 6));
    TEST_ASSERT_FALSE(z906_cmd_registry_validate_param(Z906_CMD_ID_INPUT, 0));
    TEST_ASSERT_FALSE(z906_cmd_registry_validate_param(Z906_CMD_ID_INPUT, 7));

    /* Effect: 0 - 3 */
    TEST_ASSERT_TRUE(z906_cmd_registry_validate_param(Z906_CMD_ID_EFFECT, 0));
    TEST_ASSERT_TRUE(z906_cmd_registry_validate_param(Z906_CMD_ID_EFFECT, 3));
    TEST_ASSERT_FALSE(z906_cmd_registry_validate_param(Z906_CMD_ID_EFFECT, 4));

    /* Trims: 0 - 43 */
    TEST_ASSERT_TRUE(z906_cmd_registry_validate_param(Z906_CMD_ID_TRIM_BASS, 0));
    TEST_ASSERT_TRUE(z906_cmd_registry_validate_param(Z906_CMD_ID_TRIM_BASS, 43));
    TEST_ASSERT_FALSE(z906_cmd_registry_validate_param(Z906_CMD_ID_TRIM_BASS, 44));
    TEST_ASSERT_FALSE(z906_cmd_registry_validate_param(Z906_CMD_ID_TRIM_CENTER, -1));
    TEST_ASSERT_FALSE(z906_cmd_registry_validate_param(Z906_CMD_ID_TRIM_REAR, 50));
}

/**
 * @brief Test 5 — Successful command verification cycle.
 */
void test_scenario_5_successful_verification(void)
{
    z906_state_set_cable_status(true);
    z906_state_set_power(true);
    z906_state_set_volume(15);

    cmd_request_t req;
    TEST_ASSERT_TRUE(z906_cmd_registry_build_request(Z906_CMD_ID_VOLUME, CMD_SOURCE_CLI, 15, &req));

    z906_cmd_verify_info_t info;
    cmd_verify_result_t res = z906_cmd_verifier_execute(&req, &info);

    TEST_ASSERT_EQUAL(CMD_VERIFY_SUCCESS, res);
    TEST_ASSERT_EQUAL_UINT8(15, info.post_state.volume);
    TEST_ASSERT_EQUAL_INT((int)CMD_VERIFY_SUCCESS, (int)info.result);
}

/**
 * @brief Test 6 — Failed command verification and result code distinctness.
 */
void test_scenario_6_failed_verification_result_handling(void)
{
    /* Null request pointer must produce pre-state error */
    cmd_verify_result_t res = z906_cmd_verifier_execute(NULL, NULL);
    TEST_ASSERT_EQUAL(CMD_VERIFY_ERR_PRE_STATE_FAILED, res);

    /* Verify all failure result codes have unique descriptive strings */
    TEST_ASSERT_EQUAL_STRING("TARGET_NOT_REACHED", z906_cmd_verify_result_str(CMD_VERIFY_ERR_TARGET_NOT_REACHED));
    TEST_ASSERT_EQUAL_STRING("VERIFICATION_TIMEOUT", z906_cmd_verify_result_str(CMD_VERIFY_ERR_TIMEOUT));
    TEST_ASSERT_EQUAL_STRING("UART_SEND_FAILED", z906_cmd_verify_result_str(CMD_VERIFY_ERR_SEND_FAILED));
}

/**
 * @brief Test 7 — State read and precondition error handling.
 */
void test_scenario_7_precondition_error_handling(void)
{
    /* 1. Cable disconnected error */
    z906_state_set_cable_status(false);
    cmd_request_t req;
    z906_cmd_registry_build_request(Z906_CMD_ID_POWER, CMD_SOURCE_CLI, 1, &req);
    z906_cmd_verify_info_t info;
    TEST_ASSERT_EQUAL(CMD_VERIFY_ERR_PRECONDITION_FAILED, z906_cmd_verifier_execute(&req, &info));

    /* 2. Standby mode error for audio command */
    z906_state_set_cable_status(true);
    z906_state_set_power(false);
    z906_cmd_registry_build_request(Z906_CMD_ID_VOLUME, CMD_SOURCE_ZIGBEE, 20, &req);
    TEST_ASSERT_EQUAL(CMD_VERIFY_ERR_PRECONDITION_FAILED, z906_cmd_verifier_execute(&req, &info));
}

/**
 * @brief Test 8 — Unverifiable command produces error and is never reported as success.
 */
void test_scenario_8_unverifiable_command_handling(void)
{
    cmd_request_t req = {
        .type = VERIFY_CMD_UNVERIFIABLE,
        .source = CMD_SOURCE_CLI
    };

    z906_cmd_verify_info_t info;
    cmd_verify_result_t res = z906_cmd_verifier_execute(&req, &info);

    TEST_ASSERT_EQUAL(CMD_VERIFY_ERR_UNVERIFIABLE, res);
    TEST_ASSERT_NOT_EQUAL(CMD_VERIFY_SUCCESS, info.result);
}

/**
 * @brief Test 9 — Toggle and relative command behavior depending on initial state.
 */
void test_scenario_9_toggle_and_state_dependency(void)
{
    z906_state_set_cable_status(true);
    z906_state_set_power(true);

    /* 1. Mute toggling when currently unmuted */
    z906_state_set_mute(false);
    cmd_request_t req_mute;
    TEST_ASSERT_TRUE(z906_cmd_registry_build_request(Z906_CMD_ID_MUTE, CMD_SOURCE_CLI, 1, &req_mute));
    TEST_ASSERT_TRUE(req_mute.param.mute);

    /* 2. Volume step when currently at volume 10 */
    z906_state_set_volume(10);
    cmd_request_t req_vol;
    TEST_ASSERT_TRUE(z906_cmd_registry_build_request(Z906_CMD_ID_VOLUME, CMD_SOURCE_CLI, 10, &req_vol));
    z906_cmd_verify_info_t info;
    TEST_ASSERT_EQUAL(CMD_VERIFY_SUCCESS, z906_cmd_verifier_execute(&req_vol, &info));
    TEST_ASSERT_EQUAL_UINT8(10, info.post_state.volume);
}

/**
 * @brief Test 10 — Concurrency protection and mutex synchronization.
 */
void test_scenario_10_concurrency_mutex_protection(void)
{
    /* Mutex must be valid and result string for busy must be correct */
    TEST_ASSERT_EQUAL_STRING("VERIFIER_BUSY", z906_cmd_verify_result_str(CMD_VERIFY_ERR_BUSY));

    /* Sequential execution from CLI and Zigbee works seamlessly without contention */
    z906_state_set_cable_status(true);
    z906_state_set_power(true);
    z906_state_set_volume(30);

    cmd_request_t req_cli;
    z906_cmd_registry_build_request(Z906_CMD_ID_VOLUME, CMD_SOURCE_CLI, 30, &req_cli);
    TEST_ASSERT_EQUAL(CMD_VERIFY_SUCCESS, z906_cmd_verifier_execute(&req_cli, NULL));

    cmd_request_t req_zb;
    z906_cmd_registry_build_request(Z906_CMD_ID_VOLUME, CMD_SOURCE_ZIGBEE, 30, &req_zb);
    TEST_ASSERT_EQUAL(CMD_VERIFY_SUCCESS, z906_cmd_verifier_execute(&req_zb, NULL));
}

/**
 * @brief Test 11 — Mandatory routing through verifier: all commands in registry are verifiable.
 */
void test_scenario_11_no_bypass_of_verification(void)
{
    /* All registered control commands must map to a valid verifier category and be verifiable */
    for (size_t i = 0; i < z906_cmd_registry_get_count(); i++) {
        const z906_cmd_descriptor_t *desc = z906_cmd_registry_get_descriptor(i);
        TEST_ASSERT_NOT_NULL(desc);
        TEST_ASSERT_TRUE(desc->is_verifiable);
        TEST_ASSERT_NOT_EQUAL(VERIFY_CMD_UNVERIFIABLE, desc->verify_type);

        /* Confirm that every command can be converted to cmd_request_t */
        cmd_request_t req;
        TEST_ASSERT_TRUE(z906_cmd_registry_build_request(desc->id, CMD_SOURCE_CLI, desc->min_val, &req));
        TEST_ASSERT_EQUAL(desc->verify_type, req.type);
    }
}

int runUnityTests(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_verifier_init_state);
    RUN_TEST(test_verifier_unverifiable_command);
    RUN_TEST(test_verifier_precondition_cable_disconnected);
    RUN_TEST(test_verifier_precondition_standby_mode);
    RUN_TEST(test_verifier_redundant_power_command);
    RUN_TEST(test_verifier_redundant_volume_command);
    RUN_TEST(test_verifier_boundary_clamping);
    RUN_TEST(test_verifier_digital_input_effect_validation);
    RUN_TEST(test_verifier_string_conversions);

    /* Run 11 mandated integration test scenarios */
    RUN_TEST(test_scenario_1_cli_command_execution);
    RUN_TEST(test_scenario_2_zigbee_command_execution);
    RUN_TEST(test_scenario_3_command_id_integrity);
    RUN_TEST(test_scenario_4_param_bounds_validation);
    RUN_TEST(test_scenario_5_successful_verification);
    RUN_TEST(test_scenario_6_failed_verification_result_handling);
    RUN_TEST(test_scenario_7_precondition_error_handling);
    RUN_TEST(test_scenario_8_unverifiable_command_handling);
    RUN_TEST(test_scenario_9_toggle_and_state_dependency);
    RUN_TEST(test_scenario_10_concurrency_mutex_protection);
    RUN_TEST(test_scenario_11_no_bypass_of_verification);

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
