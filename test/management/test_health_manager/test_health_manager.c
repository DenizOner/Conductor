/**
 * @file test_health_manager.c
 * @brief Unit tests for Health Manager aggregation, hysteresis, UNKNOWN lifecycle,
 *        memory semantics, and autonomous staleness monitoring.
 *
 * Covers:
 *   - Overall health state progression: UNKNOWN, HEALTHY, DEGRADED, FAULT.
 *   - UNKNOWN lifecycle: baseline pending, CPU baseline invalid, temperature invalid.
 *   - Temperature range contract and hysteresis (75C warn, 70C exit, 85C crit, 80C exit).
 *   - Memory semantics: current free vs historical minimum free watermark.
 *   - CPU sustained window filtering (momentary spike rejection vs sustained alarm).
 *   - Autonomous staleness enforcement when telemetry stream ceases and recovery on fresh data.
 *   - Subsystem reporting and component priority ranking.
 */

#include <unity.h>
#include "health_manager.h"
#include "health_policy.h"
#include "system_metrics.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

static void prv_populate_nominal_metrics(system_metrics_snapshot_t *m)
{
    memset(m, 0, sizeof(*m));
    m->timestamp_ms = (uint64_t)esp_timer_get_time() / 1000;
    m->uptime_ms = m->timestamp_ms;
    m->soc_temperature_c = 45.0f;
    m->soc_temperature_valid = true;
    m->cpu_load_percent = 15.0f;
    m->cpu_load_valid = true;
    m->heap_total_bytes = 200 * 1024;
    m->heap_used_bytes = 80 * 1024;
    m->heap_free_bytes = 120 * 1024;
    m->heap_usage_percent = 40.0f;
    m->heap_min_free_bytes = 100 * 1024;
    m->heap_largest_free_block_bytes = 64 * 1024;
    m->reset_reason = SYSTEM_RESET_REASON_POWER_ON;
}

void setUp(void)
{
    health_manager_init();
}

void tearDown(void)
{
}

/**
 * @brief Tests initial boot state: all components UNKNOWN before first telemetry ingest.
 */
void test_health_manager_initial_state(void)
{
    health_snapshot_t snap;
    TEST_ASSERT_EQUAL(ESP_OK, health_manager_get_snapshot(&snap));
    TEST_ASSERT_EQUAL(HEALTH_STATUS_UNKNOWN, snap.overall_status);
    TEST_ASSERT_EQUAL(HEALTH_REASON_NOT_INITIALIZED, snap.primary_reason);
}

/**
 * @brief Tests UNKNOWN state transitions and baseline pending evaluation (Sections 7-10).
 */
void test_health_manager_unknown_semantics(void)
{
    system_metrics_snapshot_t metrics;
    prv_populate_nominal_metrics(&metrics);

    /* 1. CPU baseline pending: cpu_load_valid = false -> CPU must be UNKNOWN, overall UNKNOWN */
    metrics.cpu_load_valid = false;
    health_manager_process_metrics(&metrics);
    health_manager_set_component_health(HEALTH_COMP_INITIALIZER, HEALTH_STATUS_HEALTHY, HEALTH_REASON_NONE);

    health_snapshot_t snap;
    TEST_ASSERT_EQUAL(ESP_OK, health_manager_get_snapshot(&snap));
    TEST_ASSERT_EQUAL(HEALTH_STATUS_UNKNOWN, snap.components[HEALTH_COMP_CPU].status);
    TEST_ASSERT_EQUAL(HEALTH_REASON_METRIC_NOT_READY, snap.components[HEALTH_COMP_CPU].reason);
    TEST_ASSERT_EQUAL(HEALTH_STATUS_UNKNOWN, snap.overall_status);

    /* 2. When CPU becomes valid, transition to HEALTHY */
    metrics.cpu_load_valid = true;
    health_manager_process_metrics(&metrics);
    TEST_ASSERT_EQUAL(ESP_OK, health_manager_get_snapshot(&snap));
    TEST_ASSERT_EQUAL(HEALTH_STATUS_HEALTHY, snap.components[HEALTH_COMP_CPU].status);
    TEST_ASSERT_EQUAL(HEALTH_STATUS_HEALTHY, snap.overall_status);

    /* 3. Initializer UNKNOWN -> overall UNKNOWN */
    health_manager_set_component_health(HEALTH_COMP_INITIALIZER, HEALTH_STATUS_UNKNOWN, HEALTH_REASON_NOT_INITIALIZED);
    TEST_ASSERT_EQUAL(ESP_OK, health_manager_get_snapshot(&snap));
    TEST_ASSERT_EQUAL(HEALTH_STATUS_UNKNOWN, snap.overall_status);

    /* 4. Restore initializer -> overall HEALTHY */
    health_manager_set_component_health(HEALTH_COMP_INITIALIZER, HEALTH_STATUS_HEALTHY, HEALTH_REASON_NONE);
    TEST_ASSERT_EQUAL(ESP_OK, health_manager_get_snapshot(&snap));
    TEST_ASSERT_EQUAL(HEALTH_STATUS_HEALTHY, snap.overall_status);
}

/**
 * @brief Tests that nominal metrics yield an overall HEALTHY system.
 */
void test_health_manager_nominal_evaluation(void)
{
    system_metrics_snapshot_t metrics;
    prv_populate_nominal_metrics(&metrics);

    health_manager_process_metrics(&metrics);
    health_manager_set_component_health(HEALTH_COMP_INITIALIZER, HEALTH_STATUS_HEALTHY, HEALTH_REASON_NONE);

    health_snapshot_t snap;
    TEST_ASSERT_EQUAL(ESP_OK, health_manager_get_snapshot(&snap));
    TEST_ASSERT_EQUAL(HEALTH_STATUS_HEALTHY, snap.overall_status);
    TEST_ASSERT_EQUAL(HEALTH_STATUS_HEALTHY, snap.components[HEALTH_COMP_SOC_TEMPERATURE].status);
    TEST_ASSERT_EQUAL(HEALTH_STATUS_HEALTHY, snap.components[HEALTH_COMP_CPU].status);
    TEST_ASSERT_EQUAL(HEALTH_STATUS_HEALTHY, snap.components[HEALTH_COMP_MEMORY].status);
    TEST_ASSERT_EQUAL(HEALTH_STATUS_HEALTHY, snap.components[HEALTH_COMP_SYSTEM_METRICS].status);
}

/**
 * @brief Tests CPU transient spike rejection vs sustained high load alarm.
 */
void test_health_manager_cpu_spike_vs_sustained(void)
{
    system_metrics_snapshot_t metrics;
    prv_populate_nominal_metrics(&metrics);

    health_manager_process_metrics(&metrics);
    health_manager_set_component_health(HEALTH_COMP_INITIALIZER, HEALTH_STATUS_HEALTHY, HEALTH_REASON_NONE);

    /* 1. Brief high spike (2 samples at 90%) MUST NOT trigger DEGRADED */
    metrics.cpu_load_percent = 90.0f;
    health_manager_process_metrics(&metrics);
    health_manager_process_metrics(&metrics);

    health_snapshot_t snap;
    TEST_ASSERT_EQUAL(ESP_OK, health_manager_get_snapshot(&snap));
    TEST_ASSERT_EQUAL(HEALTH_STATUS_HEALTHY, snap.components[HEALTH_COMP_CPU].status);

    /* 2. Sustained high load (total >= 5 samples) MUST trigger DEGRADED */
    health_manager_process_metrics(&metrics);
    health_manager_process_metrics(&metrics);
    health_manager_process_metrics(&metrics);

    TEST_ASSERT_EQUAL(ESP_OK, health_manager_get_snapshot(&snap));
    TEST_ASSERT_EQUAL(HEALTH_STATUS_DEGRADED, snap.components[HEALTH_COMP_CPU].status);
    TEST_ASSERT_EQUAL(HEALTH_REASON_CPU_LOAD_SUSTAINED_HIGH, snap.components[HEALTH_COMP_CPU].reason);
    TEST_ASSERT_EQUAL(HEALTH_STATUS_DEGRADED, snap.overall_status);
    TEST_ASSERT_EQUAL(HEALTH_COMP_CPU, snap.primary_component);

    /* 3. Recovery below exit threshold recovers to HEALTHY */
    metrics.cpu_load_percent = 50.0f;
    health_manager_process_metrics(&metrics);
    TEST_ASSERT_EQUAL(ESP_OK, health_manager_get_snapshot(&snap));
    TEST_ASSERT_EQUAL(HEALTH_STATUS_HEALTHY, snap.components[HEALTH_COMP_CPU].status);
}

/**
 * @brief Tests temperature warning (75C enter, 70C exit) and critical fault (85C enter, 80C exit).
 */
void test_health_manager_temperature_hysteresis(void)
{
    system_metrics_snapshot_t metrics;
    prv_populate_nominal_metrics(&metrics);
    health_manager_set_component_health(HEALTH_COMP_INITIALIZER, HEALTH_STATUS_HEALTHY, HEALTH_REASON_NONE);

    /* 1. Normal */
    metrics.soc_temperature_c = 65.0f;
    health_manager_process_metrics(&metrics);
    health_snapshot_t snap;
    TEST_ASSERT_EQUAL(ESP_OK, health_manager_get_snapshot(&snap));
    TEST_ASSERT_EQUAL(HEALTH_STATUS_HEALTHY, snap.components[HEALTH_COMP_SOC_TEMPERATURE].status);

    /* 2. Enter Warning at 76C */
    metrics.soc_temperature_c = 76.0f;
    health_manager_process_metrics(&metrics);
    TEST_ASSERT_EQUAL(ESP_OK, health_manager_get_snapshot(&snap));
    TEST_ASSERT_EQUAL(HEALTH_STATUS_DEGRADED, snap.components[HEALTH_COMP_SOC_TEMPERATURE].status);
    TEST_ASSERT_EQUAL(HEALTH_REASON_SOC_TEMP_WARNING, snap.components[HEALTH_COMP_SOC_TEMPERATURE].reason);

    /* 3. Cools slightly to 72C (above exit threshold 70C) -> remains DEGRADED (hysteresis) */
    metrics.soc_temperature_c = 72.0f;
    health_manager_process_metrics(&metrics);
    TEST_ASSERT_EQUAL(ESP_OK, health_manager_get_snapshot(&snap));
    TEST_ASSERT_EQUAL(HEALTH_STATUS_DEGRADED, snap.components[HEALTH_COMP_SOC_TEMPERATURE].status);

    /* 4. Cools below 70C -> recovers to HEALTHY */
    metrics.soc_temperature_c = 69.0f;
    health_manager_process_metrics(&metrics);
    TEST_ASSERT_EQUAL(ESP_OK, health_manager_get_snapshot(&snap));
    TEST_ASSERT_EQUAL(HEALTH_STATUS_HEALTHY, snap.components[HEALTH_COMP_SOC_TEMPERATURE].status);

    /* 5. Critical Fault at 86C (within 20C-100C calibrated range) */
    metrics.soc_temperature_c = 86.0f;
    health_manager_process_metrics(&metrics);
    TEST_ASSERT_EQUAL(ESP_OK, health_manager_get_snapshot(&snap));
    TEST_ASSERT_EQUAL(HEALTH_STATUS_FAULT, snap.components[HEALTH_COMP_SOC_TEMPERATURE].status);
    TEST_ASSERT_EQUAL(HEALTH_REASON_SOC_TEMP_CRITICAL, snap.components[HEALTH_COMP_SOC_TEMPERATURE].reason);
    TEST_ASSERT_EQUAL(HEALTH_STATUS_FAULT, snap.overall_status);

    /* 6. Cools to 82C (above critical exit threshold 80C) -> remains FAULT */
    metrics.soc_temperature_c = 82.0f;
    health_manager_process_metrics(&metrics);
    TEST_ASSERT_EQUAL(ESP_OK, health_manager_get_snapshot(&snap));
    TEST_ASSERT_EQUAL(HEALTH_STATUS_FAULT, snap.components[HEALTH_COMP_SOC_TEMPERATURE].status);

    /* 7. Sensor read error at runtime -> DEGRADED with SOC_TEMP_SENSOR_ERROR */
    metrics.soc_temperature_valid = false;
    health_manager_process_metrics(&metrics);
    TEST_ASSERT_EQUAL(ESP_OK, health_manager_get_snapshot(&snap));
    TEST_ASSERT_EQUAL(HEALTH_STATUS_DEGRADED, snap.components[HEALTH_COMP_SOC_TEMPERATURE].status);
    TEST_ASSERT_EQUAL(HEALTH_REASON_SOC_TEMP_SENSOR_ERROR, snap.components[HEALTH_COMP_SOC_TEMPERATURE].reason);
}

/**
 * @brief Tests memory semantics: current free vs historical minimum watermark (Sections 11-13).
 */
void test_health_manager_memory_semantics(void)
{
    system_metrics_snapshot_t metrics;
    prv_populate_nominal_metrics(&metrics);
    health_manager_set_component_health(HEALTH_COMP_INITIALIZER, HEALTH_STATUS_HEALTHY, HEALTH_REASON_NONE);

    /* Scenario A: Current free is healthy (120 KB), but historical min_free was very low (10 KB).
     * Current health MUST be HEALTHY because historical watermark does not lock current state. */
    metrics.heap_free_bytes = 120 * 1024;
    metrics.heap_usage_percent = 40.0f;
    metrics.heap_min_free_bytes = 10 * 1024; /* Historically dropped low */
    health_manager_process_metrics(&metrics);

    health_snapshot_t snap;
    TEST_ASSERT_EQUAL(ESP_OK, health_manager_get_snapshot(&snap));
    TEST_ASSERT_EQUAL(HEALTH_STATUS_HEALTHY, snap.components[HEALTH_COMP_MEMORY].status);

    /* Scenario B: Current free drops below warning threshold (< 32 KB) -> DEGRADED */
    metrics.heap_free_bytes = 25 * 1024;
    metrics.heap_usage_percent = 87.0f;
    health_manager_process_metrics(&metrics);
    TEST_ASSERT_EQUAL(ESP_OK, health_manager_get_snapshot(&snap));
    TEST_ASSERT_EQUAL(HEALTH_STATUS_DEGRADED, snap.components[HEALTH_COMP_MEMORY].status);
    TEST_ASSERT_EQUAL(HEALTH_REASON_HEAP_WARNING, snap.components[HEALTH_COMP_MEMORY].reason);

    /* Scenario C: Current free drops critically low (< 16 KB) -> FAULT */
    metrics.heap_free_bytes = 12 * 1024;
    metrics.heap_usage_percent = 94.0f;
    health_manager_process_metrics(&metrics);
    TEST_ASSERT_EQUAL(ESP_OK, health_manager_get_snapshot(&snap));
    TEST_ASSERT_EQUAL(HEALTH_STATUS_FAULT, snap.components[HEALTH_COMP_MEMORY].status);
    TEST_ASSERT_EQUAL(HEALTH_REASON_HEAP_CRITICAL, snap.components[HEALTH_COMP_MEMORY].reason);

    /* Scenario D: Fragmentation: free is large (100 KB) but largest block < 8 KB -> DEGRADED */
    metrics.heap_free_bytes = 100 * 1024;
    metrics.heap_usage_percent = 50.0f;
    metrics.heap_largest_free_block_bytes = 4 * 1024;
    health_manager_process_metrics(&metrics);
    TEST_ASSERT_EQUAL(ESP_OK, health_manager_get_snapshot(&snap));
    TEST_ASSERT_EQUAL(HEALTH_STATUS_DEGRADED, snap.components[HEALTH_COMP_MEMORY].status);
    TEST_ASSERT_EQUAL(HEALTH_REASON_HEAP_FRAGMENTED, snap.components[HEALTH_COMP_MEMORY].reason);

    /* Scenario E: Recovery after garbage collection / heap free restoration -> recovers to HEALTHY */
    metrics.heap_free_bytes = 120 * 1024;
    metrics.heap_usage_percent = 40.0f;
    metrics.heap_largest_free_block_bytes = 64 * 1024;
    health_manager_process_metrics(&metrics);
    TEST_ASSERT_EQUAL(ESP_OK, health_manager_get_snapshot(&snap));
    TEST_ASSERT_EQUAL(HEALTH_STATUS_HEALTHY, snap.components[HEALTH_COMP_MEMORY].status);
}

/**
 * @brief Tests autonomous staleness enforcement and recovery on fresh data (Sections 17-18).
 */
void test_health_manager_staleness_and_recovery(void)
{
    system_metrics_snapshot_t metrics;
    prv_populate_nominal_metrics(&metrics);
    health_manager_process_metrics(&metrics);
    health_manager_set_component_health(HEALTH_COMP_INITIALIZER, HEALTH_STATUS_HEALTHY, HEALTH_REASON_NONE);

    health_snapshot_t snap;
    TEST_ASSERT_EQUAL(ESP_OK, health_manager_get_snapshot(&snap));
    TEST_ASSERT_EQUAL(HEALTH_STATUS_HEALTHY, snap.overall_status);

    /* Simulate elapsed time without new metrics (stale threshold is 5000 ms) */
    vTaskDelay(pdMS_TO_TICKS(5200));

    /* Trigger tick to evaluate staleness */
    health_manager_tick_now();

    TEST_ASSERT_EQUAL(ESP_OK, health_manager_get_snapshot(&snap));
    TEST_ASSERT_EQUAL(HEALTH_STATUS_DEGRADED, snap.components[HEALTH_COMP_SYSTEM_METRICS].status);
    TEST_ASSERT_EQUAL(HEALTH_REASON_METRIC_STALE, snap.components[HEALTH_COMP_SYSTEM_METRICS].reason);
    TEST_ASSERT_EQUAL(HEALTH_STATUS_DEGRADED, snap.overall_status);

    /* Fresh metrics return -> automatic recovery */
    prv_populate_nominal_metrics(&metrics);
    health_manager_process_metrics(&metrics);

    TEST_ASSERT_EQUAL(ESP_OK, health_manager_get_snapshot(&snap));
    TEST_ASSERT_EQUAL(HEALTH_STATUS_HEALTHY, snap.components[HEALTH_COMP_SYSTEM_METRICS].status);
    TEST_ASSERT_EQUAL(HEALTH_STATUS_HEALTHY, snap.overall_status);
}

/**
 * @brief Tests individual subsystem component health classification and primary fault attribution.
 */
void test_health_manager_subsystem_reporting(void)
{
    system_metrics_snapshot_t metrics;
    prv_populate_nominal_metrics(&metrics);
    health_manager_process_metrics(&metrics);
    health_manager_set_component_health(HEALTH_COMP_INITIALIZER, HEALTH_STATUS_HEALTHY, HEALTH_REASON_NONE);

    /* Report Z906 communication fault */
    health_manager_set_component_health(HEALTH_COMP_Z906_TRANSPORT,
                                        HEALTH_STATUS_FAULT,
                                        HEALTH_REASON_Z906_COMMUNICATION_FAULT);

    health_snapshot_t snap;
    TEST_ASSERT_EQUAL(ESP_OK, health_manager_get_snapshot(&snap));
    TEST_ASSERT_EQUAL(HEALTH_STATUS_FAULT, snap.overall_status);
    TEST_ASSERT_EQUAL(HEALTH_COMP_Z906_TRANSPORT, snap.primary_component);
    TEST_ASSERT_EQUAL(HEALTH_REASON_Z906_COMMUNICATION_FAULT, snap.primary_reason);

    /* Clear fault to healthy */
    health_manager_set_component_health(HEALTH_COMP_Z906_TRANSPORT, HEALTH_STATUS_HEALTHY, HEALTH_REASON_NONE);
    TEST_ASSERT_EQUAL(ESP_OK, health_manager_get_snapshot(&snap));
    TEST_ASSERT_EQUAL(HEALTH_STATUS_HEALTHY, snap.overall_status);
}

int runUnityTests(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_health_manager_initial_state);
    RUN_TEST(test_health_manager_unknown_semantics);
    RUN_TEST(test_health_manager_nominal_evaluation);
    RUN_TEST(test_health_manager_cpu_spike_vs_sustained);
    RUN_TEST(test_health_manager_temperature_hysteresis);
    RUN_TEST(test_health_manager_memory_semantics);
    RUN_TEST(test_health_manager_staleness_and_recovery);
    RUN_TEST(test_health_manager_subsystem_reporting);
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
