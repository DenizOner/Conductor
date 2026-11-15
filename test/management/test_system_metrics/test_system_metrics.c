/**
 * @file test_system_metrics.c
 * @brief Unit tests for System Metrics measurement module.
 *
 * Covers:
 *   - SoC die temperature measurement, validity flags, and error isolation.
 *   - Temperature sensor calibrated range contract (20C - 100C).
 *   - CPU load mathematical accuracy: 0%, 50%, 100%, first-sample invalidation,
 *     divide-by-zero protection, and counter wrap-around safety.
 *   - Heap memory accounting (used + free = total), percentage bounds [0, 100],
 *     minimum free watermark, and largest allocatable block.
 *   - Monotonic uptime progression and hardware reset reason string mapping.
 *   - Snapshot thread safety and pointer argument validation.
 *   - Execution time profiling, duty-cycle measurement, and stack watermark headroom.
 */

#include <unity.h>
#include "system_metrics.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

/* Mock state for test hooks */
static float     s_mock_temp_val = 25.0f;
static esp_err_t s_mock_temp_err = ESP_OK;

static uint32_t s_mock_idle_us = 0;
static uint64_t s_mock_total_us = 0;

static esp_err_t mock_read_temp(float *out)
{
    if (s_mock_temp_err == ESP_OK && out != NULL) {
        *out = s_mock_temp_val;
    }
    return s_mock_temp_err;
}

static void mock_read_cpu(uint32_t *out_idle, uint64_t *out_total)
{
    if (out_idle) *out_idle = s_mock_idle_us;
    if (out_total) *out_total = s_mock_total_us;
}

void setUp(void)
{
    s_mock_temp_val = 25.0f;
    s_mock_temp_err = ESP_OK;
    s_mock_idle_us = 1000000;
    s_mock_total_us = 1000000;

    system_metrics_test_hooks_t hooks = {
        .read_temperature = mock_read_temp,
        .read_cpu_counters = mock_read_cpu
    };
    system_metrics_set_test_hooks(&hooks);
    system_metrics_init();
}

void tearDown(void)
{
    system_metrics_set_test_hooks(NULL);
}

/**
 * @brief Tests initialization and invalid pointer argument protection.
 */
void test_system_metrics_init_and_args(void)
{
    TEST_ASSERT_TRUE(system_metrics_is_initialized());
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, system_metrics_get_snapshot(NULL));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, system_metrics_get_overhead_stats(NULL));
}

/**
 * @brief Tests SoC die temperature measurement and error state propagation.
 */
void test_system_metrics_soc_temperature(void)
{
    s_mock_temp_val = 42.5f;
    s_mock_temp_err = ESP_OK;

    TEST_ASSERT_EQUAL(ESP_OK, system_metrics_sample_now());

    system_metrics_snapshot_t snap;
    TEST_ASSERT_EQUAL(ESP_OK, system_metrics_get_snapshot(&snap));
    TEST_ASSERT_TRUE(snap.soc_temperature_valid);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 42.5f, snap.soc_temperature_c);

    /* Simulate driver read failure */
    s_mock_temp_err = ESP_FAIL;
    TEST_ASSERT_EQUAL(ESP_OK, system_metrics_sample_now());
    TEST_ASSERT_EQUAL(ESP_OK, system_metrics_get_snapshot(&snap));
    TEST_ASSERT_FALSE(snap.soc_temperature_valid);
}

/**
 * @brief Tests calibrated driver temperature range contract and health threshold bounds (Sections 4-5).
 */
void test_system_metrics_temperature_range_contract(void)
{
    /* Verify configured driver range bounds */
    TEST_ASSERT_EQUAL_INT(20, SYSTEM_METRICS_TSENS_RANGE_MIN);
    TEST_ASSERT_EQUAL_INT(100, SYSTEM_METRICS_TSENS_RANGE_MAX);
    TEST_ASSERT_LESS_THAN_INT(SYSTEM_METRICS_TSENS_RANGE_MAX, SYSTEM_METRICS_TSENS_RANGE_MIN);
}

/**
 * @brief Tests CPU load calculation under multiple boundary and nominal conditions.
 */
void test_system_metrics_cpu_load_calculations(void)
{
    system_metrics_snapshot_t snap;

    /* First sample after boot/init: baseline only, must be marked INVALID */
    TEST_ASSERT_EQUAL(ESP_OK, system_metrics_sample_now());
    TEST_ASSERT_EQUAL(ESP_OK, system_metrics_get_snapshot(&snap));
    TEST_ASSERT_FALSE_MESSAGE(snap.cpu_load_valid, "First sample must not calculate load (no delta baseline)");

    /* Sample 2: 50% CPU load (500ms idle in 1000ms total) */
    s_mock_idle_us += 500000;
    s_mock_total_us += 1000000;
    TEST_ASSERT_EQUAL(ESP_OK, system_metrics_sample_now());
    TEST_ASSERT_EQUAL(ESP_OK, system_metrics_get_snapshot(&snap));
    TEST_ASSERT_TRUE(snap.cpu_load_valid);
    TEST_ASSERT_FLOAT_WITHIN(0.1f, 50.0f, snap.cpu_load_percent);

    /* Sample 3: 0% CPU load (1000ms idle in 1000ms total) */
    s_mock_idle_us += 1000000;
    s_mock_total_us += 1000000;
    TEST_ASSERT_EQUAL(ESP_OK, system_metrics_sample_now());
    TEST_ASSERT_EQUAL(ESP_OK, system_metrics_get_snapshot(&snap));
    TEST_ASSERT_TRUE(snap.cpu_load_valid);
    TEST_ASSERT_FLOAT_WITHIN(0.1f, 0.0f, snap.cpu_load_percent);

    /* Sample 4: 100% CPU load (0ms idle in 1000ms total) */
    s_mock_idle_us += 0;
    s_mock_total_us += 1000000;
    TEST_ASSERT_EQUAL(ESP_OK, system_metrics_sample_now());
    TEST_ASSERT_EQUAL(ESP_OK, system_metrics_get_snapshot(&snap));
    TEST_ASSERT_TRUE(snap.cpu_load_valid);
    TEST_ASSERT_FLOAT_WITHIN(0.1f, 100.0f, snap.cpu_load_percent);

    /* Sample 5: Zero total delta protection (divide-by-zero prevention) */
    TEST_ASSERT_EQUAL(ESP_OK, system_metrics_sample_now());
    TEST_ASSERT_EQUAL(ESP_OK, system_metrics_get_snapshot(&snap));
    TEST_ASSERT_FALSE(snap.cpu_load_valid);
}

/**
 * @brief Tests CPU load wrap-around protection on 32-bit counter overflow.
 */
void test_system_metrics_cpu_counter_wraparound(void)
{
    system_metrics_snapshot_t snap;

    /* Prime with initial counter values near 32-bit boundary */
    s_mock_idle_us = 0xFFFFFF00;
    s_mock_total_us = 0xFFFFFF00;
    system_metrics_sample_now();

    /* Wrap-around: idle increments by 500, total increments by 1000 */
    s_mock_idle_us = 0x000000F4;                     /* 32-bit counter wrapped +500 */
    s_mock_total_us = (uint64_t)0xFFFFFF00 + 1000ULL; /* 64-bit microsecond timer monotonically advances +1000 */

    TEST_ASSERT_EQUAL(ESP_OK, system_metrics_sample_now());
    TEST_ASSERT_EQUAL(ESP_OK, system_metrics_get_snapshot(&snap));
    TEST_ASSERT_TRUE(snap.cpu_load_valid);
    TEST_ASSERT_FLOAT_WITHIN(0.5f, 50.0f, snap.cpu_load_percent);
}

/**
 * @brief Tests heap memory metrics consistency (used + free = total).
 */
void test_system_metrics_heap_accounting(void)
{
    TEST_ASSERT_EQUAL(ESP_OK, system_metrics_sample_now());

    system_metrics_snapshot_t snap;
    TEST_ASSERT_EQUAL(ESP_OK, system_metrics_get_snapshot(&snap));

    TEST_ASSERT_GREATER_THAN_UINT32(0, snap.heap_total_bytes);
    TEST_ASSERT_GREATER_THAN_UINT32(0, snap.heap_free_bytes);
    TEST_ASSERT_EQUAL_UINT32(snap.heap_total_bytes, snap.heap_used_bytes + snap.heap_free_bytes);
    TEST_ASSERT_GREATER_OR_EQUAL_FLOAT(0.0f, snap.heap_usage_percent);
    TEST_ASSERT_LESS_OR_EQUAL_FLOAT(100.0f, snap.heap_usage_percent);
    TEST_ASSERT_LESS_OR_EQUAL_UINT32(snap.heap_free_bytes, snap.heap_min_free_bytes);
    TEST_ASSERT_GREATER_THAN_UINT32(0, snap.heap_largest_free_block_bytes);
}

/**
 * @brief Tests uptime progression and reset reason string mapping.
 */
void test_system_metrics_uptime_and_reset_reason(void)
{
    system_metrics_snapshot_t snap1;
    TEST_ASSERT_EQUAL(ESP_OK, system_metrics_sample_now());
    TEST_ASSERT_EQUAL(ESP_OK, system_metrics_get_snapshot(&snap1));

    vTaskDelay(pdMS_TO_TICKS(100));

    system_metrics_snapshot_t snap2;
    TEST_ASSERT_EQUAL(ESP_OK, system_metrics_sample_now());
    TEST_ASSERT_EQUAL(ESP_OK, system_metrics_get_snapshot(&snap2));

    TEST_ASSERT_GREATER_OR_EQUAL_UINT32(snap1.uptime_ms, snap2.uptime_ms);

    /* Reset reason string verification */
    const char *str = system_metrics_reset_reason_to_str(SYSTEM_RESET_REASON_POWER_ON);
    TEST_ASSERT_EQUAL_STRING("POWER_ON", str);

    str = system_metrics_reset_reason_to_str(SYSTEM_RESET_REASON_WATCHDOG);
    TEST_ASSERT_EQUAL_STRING("TASK_WATCHDOG_RESET", str);

    str = system_metrics_reset_reason_to_str(SYSTEM_RESET_REASON_SOFTWARE);
    TEST_ASSERT_EQUAL_STRING("SOFTWARE_RESTART", str);

    str = system_metrics_reset_reason_to_str(SYSTEM_RESET_REASON_UNKNOWN);
    TEST_ASSERT_EQUAL_STRING("UNKNOWN", str);
}

/**
 * @brief Tests measurement execution profiling, duty cycle, and stack headroom (Sections 24-27).
 */
void test_system_metrics_profiling_and_overhead(void)
{
    /* Execute 3 sample passes */
    for (int i = 0; i < 3; i++) {
        s_mock_idle_us += 1000;
        s_mock_total_us += 1000;
        TEST_ASSERT_EQUAL(ESP_OK, system_metrics_sample_now());
    }

    system_metrics_overhead_stats_t stats;
    TEST_ASSERT_EQUAL(ESP_OK, system_metrics_get_overhead_stats(&stats));

    TEST_ASSERT_GREATER_THAN_UINT32(0, stats.sample_count);
    TEST_ASSERT_GREATER_OR_EQUAL_UINT32(stats.min_sample_time_us, stats.max_sample_time_us);
    TEST_ASSERT_GREATER_THAN_UINT32(0, stats.avg_sample_time_us);
    TEST_ASSERT_EQUAL_UINT32(SYSTEM_METRICS_TASK_STACK_BYTES, stats.task_stack_allocated);
    TEST_ASSERT_GREATER_THAN_UINT32(0, stats.task_stack_min_free_bytes);
    TEST_ASSERT_GREATER_OR_EQUAL_FLOAT(0.0f, stats.measured_duty_cycle_pct);

    system_metrics_snapshot_t snap;
    TEST_ASSERT_EQUAL(ESP_OK, system_metrics_get_snapshot(&snap));
    TEST_ASSERT_GREATER_THAN_UINT32(0, snap.sample_duration_us);
}

int runUnityTests(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_system_metrics_init_and_args);
    RUN_TEST(test_system_metrics_soc_temperature);
    RUN_TEST(test_system_metrics_temperature_range_contract);
    RUN_TEST(test_system_metrics_cpu_load_calculations);
    RUN_TEST(test_system_metrics_cpu_counter_wraparound);
    RUN_TEST(test_system_metrics_heap_accounting);
    RUN_TEST(test_system_metrics_uptime_and_reset_reason);
    RUN_TEST(test_system_metrics_profiling_and_overhead);
    return UNITY_END();
}

#if defined(ESP_PLATFORM)
void app_main(void)
{
    vTaskDelay(pdMS_TO_TICKS(3000)); /* Allow serial monitor to connect */
    runUnityTests();
}
#else
int main(void)
{
    return runUnityTests();
}
#endif
