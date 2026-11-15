/**
 * @file test_debug_console.c
 * @brief Unit tests for Debug Console service and Watchdog diagnostic integration.
 */

#include <unity.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "debug_console.h"
#include "wdt_manager.h"
#include "esp_system.h"
#include <string.h>

void setUp(void)
{
    debug_console_init();
}

void tearDown(void)
{
    debug_console_clear_tag_filters();
}

/**
 * @brief Tests Debug Console initialization and default state.
 */
void test_debug_console_initialization(void)
{
    TEST_ASSERT_TRUE(debug_console_init());
    TEST_ASSERT_TRUE(debug_console_is_initialized());
    TEST_ASSERT_EQUAL(ESP_LOG_INFO, debug_console_get_global_level());
    TEST_ASSERT_EQUAL_UINT(0, debug_console_get_filter_count());
}

/**
 * @brief Tests setting and getting global log levels across all supported levels.
 */
void test_debug_console_global_log_level(void)
{
    TEST_ASSERT_TRUE(debug_console_set_global_level(ESP_LOG_NONE));
    TEST_ASSERT_EQUAL(ESP_LOG_NONE, debug_console_get_global_level());

    TEST_ASSERT_TRUE(debug_console_set_global_level(ESP_LOG_ERROR));
    TEST_ASSERT_EQUAL(ESP_LOG_ERROR, debug_console_get_global_level());

    TEST_ASSERT_TRUE(debug_console_set_global_level(ESP_LOG_WARN));
    TEST_ASSERT_EQUAL(ESP_LOG_WARN, debug_console_get_global_level());

    TEST_ASSERT_TRUE(debug_console_set_global_level(ESP_LOG_INFO));
    TEST_ASSERT_EQUAL(ESP_LOG_INFO, debug_console_get_global_level());

    TEST_ASSERT_TRUE(debug_console_set_global_level(ESP_LOG_DEBUG));
    TEST_ASSERT_EQUAL(ESP_LOG_DEBUG, debug_console_get_global_level());

    TEST_ASSERT_TRUE(debug_console_set_global_level(ESP_LOG_VERBOSE));
    TEST_ASSERT_EQUAL(ESP_LOG_VERBOSE, debug_console_get_global_level());
}

/**
 * @brief Tests bidirectional string to enum and enum to string log level conversions.
 */
void test_debug_console_level_string_conversion(void)
{
    TEST_ASSERT_EQUAL_STRING("none", debug_console_level_to_str(ESP_LOG_NONE));
    TEST_ASSERT_EQUAL_STRING("error", debug_console_level_to_str(ESP_LOG_ERROR));
    TEST_ASSERT_EQUAL_STRING("warn", debug_console_level_to_str(ESP_LOG_WARN));
    TEST_ASSERT_EQUAL_STRING("info", debug_console_level_to_str(ESP_LOG_INFO));
    TEST_ASSERT_EQUAL_STRING("debug", debug_console_level_to_str(ESP_LOG_DEBUG));
    TEST_ASSERT_EQUAL_STRING("verbose", debug_console_level_to_str(ESP_LOG_VERBOSE));

    TEST_ASSERT_EQUAL(ESP_LOG_NONE, debug_console_str_to_level("none"));
    TEST_ASSERT_EQUAL(ESP_LOG_ERROR, debug_console_str_to_level("error"));
    TEST_ASSERT_EQUAL(ESP_LOG_WARN, debug_console_str_to_level("warn"));
    TEST_ASSERT_EQUAL(ESP_LOG_WARN, debug_console_str_to_level("warning"));
    TEST_ASSERT_EQUAL(ESP_LOG_INFO, debug_console_str_to_level("info"));
    TEST_ASSERT_EQUAL(ESP_LOG_DEBUG, debug_console_str_to_level("debug"));
    TEST_ASSERT_EQUAL(ESP_LOG_VERBOSE, debug_console_str_to_level("verbose"));

    /* Case-insensitive parsing */
    TEST_ASSERT_EQUAL(ESP_LOG_DEBUG, debug_console_str_to_level("DEBUG"));
    TEST_ASSERT_EQUAL(ESP_LOG_ERROR, debug_console_str_to_level("Error"));

    /* Invalid string defaults to NONE */
    TEST_ASSERT_EQUAL(ESP_LOG_NONE, debug_console_str_to_level("invalid_level"));
    TEST_ASSERT_EQUAL(ESP_LOG_NONE, debug_console_str_to_level(NULL));
}

/**
 * @brief Tests module/tag specific log level filtering and override mechanisms.
 */
void test_debug_console_tag_filtering(void)
{
    debug_console_set_global_level(ESP_LOG_INFO);

    /* 1. Unset tag should return global level */
    TEST_ASSERT_EQUAL(ESP_LOG_INFO, debug_console_get_tag_level("ZIGBEE_APP"));

    /* 2. Add custom tag filter */
    TEST_ASSERT_TRUE(debug_console_set_tag_level("ZIGBEE_APP", ESP_LOG_DEBUG));
    TEST_ASSERT_EQUAL(ESP_LOG_DEBUG, debug_console_get_tag_level("ZIGBEE_APP"));
    TEST_ASSERT_EQUAL_UINT(1, debug_console_get_filter_count());

    /* 3. Add second tag filter */
    TEST_ASSERT_TRUE(debug_console_set_tag_level("CMD_VERIFY", ESP_LOG_VERBOSE));
    TEST_ASSERT_EQUAL(ESP_LOG_VERBOSE, debug_console_get_tag_level("CMD_VERIFY"));
    TEST_ASSERT_EQUAL_UINT(2, debug_console_get_filter_count());

    /* 4. Update existing tag filter (should not increase count) */
    TEST_ASSERT_TRUE(debug_console_set_tag_level("ZIGBEE_APP", ESP_LOG_WARN));
    TEST_ASSERT_EQUAL(ESP_LOG_WARN, debug_console_get_tag_level("ZIGBEE_APP"));
    TEST_ASSERT_EQUAL_UINT(2, debug_console_get_filter_count());

    /* 5. Read back filter by index */
    char tag_buf[DEBUG_CONSOLE_TAG_MAX_LEN];
    esp_log_level_t lvl;
    TEST_ASSERT_TRUE(debug_console_get_filter(0, tag_buf, sizeof(tag_buf), &lvl));
    TEST_ASSERT_EQUAL_STRING("ZIGBEE_APP", tag_buf);
    TEST_ASSERT_EQUAL(ESP_LOG_WARN, lvl);

    /* 6. Clear tag filters */
    debug_console_clear_tag_filters();
    TEST_ASSERT_EQUAL_UINT(0, debug_console_get_filter_count());
    TEST_ASSERT_EQUAL(ESP_LOG_INFO, debug_console_get_tag_level("ZIGBEE_APP"));
}

/**
 * @brief Tests tag filter table capacity boundary (maximum 16 filters).
 */
void test_debug_console_tag_filter_capacity_boundary(void)
{
    char tag_name[DEBUG_CONSOLE_TAG_MAX_LEN];

    /* Fill table up to maximum */
    for (int i = 0; i < DEBUG_CONSOLE_MAX_TAG_FILTERS; i++) {
        snprintf(tag_name, sizeof(tag_name), "TAG_%02d", i);
        TEST_ASSERT_TRUE(debug_console_set_tag_level(tag_name, ESP_LOG_DEBUG));
    }
    TEST_ASSERT_EQUAL_UINT(DEBUG_CONSOLE_MAX_TAG_FILTERS, debug_console_get_filter_count());

    /* 17th tag insertion must be rejected safely without memory corruption */
    TEST_ASSERT_FALSE(debug_console_set_tag_level("TAG_OVERFLOW", ESP_LOG_ERROR));
    TEST_ASSERT_EQUAL_UINT(DEBUG_CONSOLE_MAX_TAG_FILTERS, debug_console_get_filter_count());

    /* Updating an existing tag in a full table should still succeed */
    TEST_ASSERT_TRUE(debug_console_set_tag_level("TAG_00", ESP_LOG_ERROR));
    TEST_ASSERT_EQUAL(ESP_LOG_ERROR, debug_console_get_tag_level("TAG_00"));
}

/**
 * @brief Tests Watchdog diagnostic information inspection.
 */
void test_wdt_manager_diagnostics(void)
{
    /* Initialize WDT with known test timeout */
    TEST_ASSERT_TRUE(wdt_manager_init(12));
    TEST_ASSERT_TRUE(wdt_manager_is_initialized());
    TEST_ASSERT_EQUAL_UINT32(12, wdt_manager_get_timeout_seconds());

    /* Query reset reason */
    esp_reset_reason_t reason = wdt_manager_get_reset_reason();
    const char *reason_str = wdt_manager_get_reset_reason_str();
    TEST_ASSERT_NOT_NULL(reason_str);
    TEST_ASSERT_TRUE(strlen(reason_str) > 0);

    /* Reason enum must be within valid ESP-IDF bounds */
    TEST_ASSERT_TRUE(reason >= ESP_RST_UNKNOWN);
}

/**
 * @brief Tests that Debug Console diagnostic printing is non-blocking and safe.
 */
void test_debug_console_print_safety(void)
{
    /* These functions must execute cleanly without hanging or crashing */
    debug_console_print_status();
    debug_console_print_wdt_status();
    TEST_ASSERT_TRUE(debug_console_is_initialized());
}

int runUnityTests(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_debug_console_initialization);
    RUN_TEST(test_debug_console_global_log_level);
    RUN_TEST(test_debug_console_level_string_conversion);
    RUN_TEST(test_debug_console_tag_filtering);
    RUN_TEST(test_debug_console_tag_filter_capacity_boundary);
    RUN_TEST(test_wdt_manager_diagnostics);
    RUN_TEST(test_debug_console_print_safety);
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
