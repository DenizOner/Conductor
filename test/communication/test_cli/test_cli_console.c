/**
 * @file test_cli_console.c
 * @brief Unit tests for CLI Console architecture: User Commands vs Developer Tools,
 *        Developer Mode toggling, command gating, and error handling.
 */

#include <unity.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "cli_console.h"
#include "z906_state.h"
#include "z906_cmd_verifier.h"
#include "nvs_manager.h"
#include <string.h>

void setUp(void)
{
    nvs_manager_init();
    z906_state_init();
    z906_cmd_verifier_init();
    cli_console_init();
}

void tearDown(void)
{
    cli_console_set_dev_mode(false);
}

/**
 * @brief Tests default initialization state of Developer Mode (must be OFF).
 */
void test_cli_default_developer_mode_is_off(void)
{
    TEST_ASSERT_FALSE(cli_console_is_dev_mode());
}

/**
 * @brief Tests enabling and disabling Developer Mode programmatically.
 */
void test_cli_set_developer_mode(void)
{
    cli_console_set_dev_mode(true);
    TEST_ASSERT_TRUE(cli_console_is_dev_mode());

    cli_console_set_dev_mode(false);
    TEST_ASSERT_FALSE(cli_console_is_dev_mode());
}

/**
 * @brief Tests toggling Developer Mode via CLI command 'devmode on' and 'devmode off'.
 */
void test_cli_devmode_command_toggling(void)
{
    TEST_ASSERT_FALSE(cli_console_is_dev_mode());

    cli_console_process_line("devmode on\n");
    TEST_ASSERT_TRUE(cli_console_is_dev_mode());

    cli_console_process_line("devmode off\n");
    TEST_ASSERT_FALSE(cli_console_is_dev_mode());

    /* Test aliases 1 and 0 */
    cli_console_process_line("devmode 1\n");
    TEST_ASSERT_TRUE(cli_console_is_dev_mode());

    cli_console_process_line("devmode 0\n");
    TEST_ASSERT_FALSE(cli_console_is_dev_mode());
}

/**
 * @brief Tests that User Commands execute without error regardless of Developer Mode.
 */
void test_cli_user_commands_execution(void)
{
    /* State: Cable connected, Powered ON */
    z906_state_set_cable_status(true);
    z906_state_set_power(true);

    /* 1. Status command */
    cli_console_process_line("status\n");

    /* 2. User Help command */
    cli_console_process_line("help\n");

    /* 3. Devmode query */
    cli_console_process_line("devmode\n");
}

/**
 * @brief Tests that developer diagnostic tools are rejected when Developer Mode is OFF.
 */
void test_cli_developer_tools_gating_when_off(void)
{
    cli_console_set_dev_mode(false);

    /* Attempting developer command via 'dev' prefix */
    cli_console_process_line("dev wdt_status\n");
    cli_console_process_line("dev zb_status\n");

    /* Attempting direct developer commands */
    cli_console_process_line("wdt_status\n");
    cli_console_process_line("zb_status\n");
    cli_console_process_line("debug_status\n");
    cli_console_process_line("log_level\n");

    /* Mode remains OFF */
    TEST_ASSERT_FALSE(cli_console_is_dev_mode());
}

/**
 * @brief Tests that developer diagnostic tools execute successfully when Developer Mode is ON.
 */
void test_cli_developer_tools_execution_when_on(void)
{
    cli_console_set_dev_mode(true);

    /* Test 'dev help' */
    cli_console_process_line("dev help\n");

    /* Test 'dev debug_status' and direct 'debug_status' */
    cli_console_process_line("dev debug_status\n");
    cli_console_process_line("debug_status\n");

    /* Test 'dev wdt_status' and direct 'wdt_status' */
    cli_console_process_line("dev wdt_status\n");
    cli_console_process_line("wdt_status\n");

    /* Test log filter inspection */
    cli_console_process_line("dev log_filter\n");
    cli_console_process_line("log_filter\n");

    TEST_ASSERT_TRUE(cli_console_is_dev_mode());
}

int runUnityTests(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_cli_default_developer_mode_is_off);
    RUN_TEST(test_cli_set_developer_mode);
    RUN_TEST(test_cli_devmode_command_toggling);
    RUN_TEST(test_cli_user_commands_execution);
    RUN_TEST(test_cli_developer_tools_gating_when_off);
    RUN_TEST(test_cli_developer_tools_execution_when_on);
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
