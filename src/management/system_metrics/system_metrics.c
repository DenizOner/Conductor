/**
 * @file system_metrics.c
 * @brief Implementation of the System Metrics measurement module for ESP32-C6.
 *
 * Responsibilities:
 *   - Samples raw hardware/RTOS telemetry at 1000ms intervals.
 *   - Strictly O(1) CPU load delta calculation.
 *   - Internal 8-bit heap capability telemetry.
 *   - SoC die temperature via on-chip sensor (-10C to +100C).
 *   - Atomic, mutex-protected snapshot delivery.
 *   - Measured runtime execution profiling and stack watermark metrics.
 *   - Pure measurement: Zero alarm thresholds, zero task restarts, zero recovery.
 */

#include "system_metrics.h"
#include <string.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "driver/temperature_sensor.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

static const char *TAG = "sys_metrics";

/* Forward declaration of optional Health Manager notification observer */
extern void health_manager_process_metrics(const system_metrics_snapshot_t *metrics) __attribute__((weak));

/** @brief Maximum mutex acquisition timeout in ticks */
#define SYSTEM_METRICS_MUTEX_TIMEOUT_TICKS pdMS_TO_TICKS(50)

/* =========================================================================
 * Static Storage Allocations (Zero Dynamic Allocation at Runtime)
 * ========================================================================= */

/** @brief Mutex protecting the active snapshot and execution stats from concurrent access */
static StaticSemaphore_t s_mutex_buffer;
static SemaphoreHandle_t s_mutex = NULL;

/** @brief Semaphore used for deterministic startup handshake in system_metrics_start() */
static StaticSemaphore_t s_started_sem_buffer;
static SemaphoreHandle_t s_started_sem = NULL;

/** @brief Static FreeRTOS task control block and stack buffer */
static StaticTask_t s_task_tcb;
static StackType_t  s_task_stack[SYSTEM_METRICS_TASK_STACK_BYTES / sizeof(StackType_t)];
static TaskHandle_t s_task_handle = NULL;

/** @brief Cached active snapshot populated by the measurement engine */
static system_metrics_snapshot_t s_active_snapshot;

/** @brief Tracks module initialization state */
static bool s_initialized = false;

/** @brief Tracks whether the background sampler task has been spawned */
static bool s_task_started = false;

/** @brief ESP32-C6 internal temperature sensor driver handle */
static temperature_sensor_handle_t s_tsens_handle = NULL;
static bool s_tsens_installed = false;

/** @brief Previous CPU counter state for differential delta calculation */
static uint32_t s_prev_idle_counter_us = 0;
static uint64_t s_prev_total_time_us = 0;
static bool     s_cpu_has_baseline = false;

/** @brief Cumulative measurement profiling statistics */
static uint32_t s_sample_count = 0;
static uint32_t s_min_sample_time_us = UINT32_MAX;
static uint32_t s_max_sample_time_us = 0;
static uint64_t s_total_sample_time_us = 0;
static uint32_t s_last_sample_time_us = 0;
static size_t   s_task_min_free_stack_bytes = SYSTEM_METRICS_TASK_STACK_BYTES;

/** @brief Test hooks table for unit testing and simulated fault injection */
static system_metrics_test_hooks_t s_test_hooks = {
    .read_temperature = NULL,
    .read_cpu_counters = NULL,
};

/* =========================================================================
 * Private Helper Functions
 * ========================================================================= */

/**
 * @brief Maps native ESP-IDF reset reason to normalized internal enum.
 */
static system_reset_reason_t prv_map_reset_reason(esp_reset_reason_t r)
{
    switch (r) {
        case ESP_RST_POWERON:   return SYSTEM_RESET_REASON_POWER_ON;
        case ESP_RST_SW:        return SYSTEM_RESET_REASON_SOFTWARE;
        case ESP_RST_PANIC:     return SYSTEM_RESET_REASON_PANIC;
        case ESP_RST_INT_WDT:
        case ESP_RST_TASK_WDT:
        case ESP_RST_WDT:       return SYSTEM_RESET_REASON_WATCHDOG;
        case ESP_RST_BROWNOUT:   return SYSTEM_RESET_REASON_BROWNOUT;
        case ESP_RST_DEEPSLEEP: return SYSTEM_RESET_REASON_DEEP_SLEEP;
        case ESP_RST_UNKNOWN:
        default:                return SYSTEM_RESET_REASON_UNKNOWN;
    }
}

/**
 * @brief Measures SoC die temperature using on-chip temperature sensor.
 */
static void prv_sample_soc_temperature(system_metrics_snapshot_t *snap)
{
    float temp_c = 0.0f;
    esp_err_t err = ESP_FAIL;

    if (s_test_hooks.read_temperature != NULL) {
        err = s_test_hooks.read_temperature(&temp_c);
    } else if (s_tsens_installed && s_tsens_handle != NULL) {
        err = temperature_sensor_get_celsius(s_tsens_handle, &temp_c);
    }

    if (err == ESP_OK) {
        snap->soc_temperature_c = temp_c;
        snap->soc_temperature_valid = true;
    } else {
        snap->soc_temperature_c = 0.0f;
        snap->soc_temperature_valid = false;
    }
}

/**
 * @brief Computes CPU utilization percentage via FreeRTOS idle task runtime delta.
 *
 * Mathematical derivation:
 *   idle_delta = current_idle - prev_idle
 *   total_delta = current_time - prev_time
 *   idle_fraction = idle_delta / total_delta
 *   CPU Load = 100 * (1 - idle_delta / total_delta)
 *
 * Guarantees:
 *   - O(1) execution time: queries idle task counter directly, no task list scanning.
 *   - 0.0% to 100.0% strictly bounded.
 *   - Counter wrap-around safe via unsigned 32-bit arithmetic.
 *   - Divide-by-zero protection.
 *   - First sample returns invalid (cpu_load_valid = false) to establish baseline.
 */
static void prv_sample_cpu_load(system_metrics_snapshot_t *snap)
{
    uint32_t current_idle_us = 0;
    uint64_t current_total_us = 0;

    if (s_test_hooks.read_cpu_counters != NULL) {
        s_test_hooks.read_cpu_counters(&current_idle_us, &current_total_us);
    } else {
#if (configGENERATE_RUN_TIME_STATS == 1)
        /* ulTaskGetIdleRunTimeCounter returns microseconds since clock is ESP Timer at 1MHz */
        current_idle_us = (uint32_t)ulTaskGetIdleRunTimeCounter();
        current_total_us = (uint64_t)esp_timer_get_time();
#else
        snap->cpu_load_valid = false;
        return;
#endif
    }

    if (!s_cpu_has_baseline) {
        /* Establish initial baseline: delta cannot be computed from a single sample */
        s_prev_idle_counter_us = current_idle_us;
        s_prev_total_time_us = current_total_us;
        s_cpu_has_baseline = true;
        snap->cpu_load_valid = false;
        return;
    }

    /* Unsigned 32-bit subtraction naturally handles 32-bit counter overflow/wrap */
    uint32_t idle_delta_us = current_idle_us - s_prev_idle_counter_us;
    uint64_t total_delta_us = current_total_us - s_prev_total_time_us;

    /* Update stored previous baseline for next cycle */
    s_prev_idle_counter_us = current_idle_us;
    s_prev_total_time_us = current_total_us;

    /* Divide-by-zero and zero-progress guards */
    if (total_delta_us == 0) {
        snap->cpu_load_valid = false;
        return;
    }

    /* Guard against potential counter jitter where idle delta exceeds total delta */
    if ((uint64_t)idle_delta_us > total_delta_us) {
        idle_delta_us = (uint32_t)total_delta_us;
    }

    float idle_fraction = (float)idle_delta_us / (float)total_delta_us;
    float cpu_load = 100.0f * (1.0f - idle_fraction);

    /* Enforce strict [0.0, 100.0] mathematical bounds */
    if (cpu_load < 0.0f) {
        cpu_load = 0.0f;
    } else if (cpu_load > 100.0f) {
        cpu_load = 100.0f;
    }

    snap->cpu_load_percent = cpu_load;
    snap->cpu_load_valid = true;
}

/**
 * @brief Measures internal 8-bit capable heap memory statistics.
 */
static void prv_sample_heap_metrics(system_metrics_snapshot_t *snap)
{
    multi_heap_info_t info;
    heap_caps_get_info(&info, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);

    snap->heap_used_bytes = info.total_allocated_bytes;
    snap->heap_free_bytes = info.total_free_bytes;
    snap->heap_min_free_bytes = info.minimum_free_bytes;
    snap->heap_largest_free_block_bytes = info.largest_free_block;

    size_t total_bytes = info.total_allocated_bytes + info.total_free_bytes;
    snap->heap_total_bytes = total_bytes;

    if (total_bytes > 0) {
        float usage_pct = ((float)info.total_allocated_bytes / (float)total_bytes) * 100.0f;
        if (usage_pct < 0.0f) usage_pct = 0.0f;
        if (usage_pct > 100.0f) usage_pct = 100.0f;
        snap->heap_usage_percent = usage_pct;
    } else {
        snap->heap_usage_percent = 0.0f;
    }
}

/**
 * @brief Executes one complete measurement pass and safely updates the cached snapshot.
 */
static esp_err_t prv_execute_sample_cycle(void)
{
    uint64_t t_start = (uint64_t)esp_timer_get_time();

    system_metrics_snapshot_t next_snap;
    memset(&next_snap, 0, sizeof(next_snap));

    /* 1. Timestamps & Uptime */
    next_snap.timestamp_ms = t_start / 1000;
    next_snap.uptime_ms = next_snap.timestamp_ms;

    /* 2. Reset Reason */
    next_snap.reset_reason_raw = esp_reset_reason();
    next_snap.reset_reason = prv_map_reset_reason(next_snap.reset_reason_raw);

    /* 3. SoC Die Temperature */
    prv_sample_soc_temperature(&next_snap);

    /* 4. CPU Load */
    prv_sample_cpu_load(&next_snap);

    /* 5. Heap Memory */
    prv_sample_heap_metrics(&next_snap);

    /* 6. Execution profiling duration calculation */
    uint64_t t_end = (uint64_t)esp_timer_get_time();
    uint32_t sample_dur_us = (uint32_t)(t_end - t_start);
    next_snap.sample_duration_us = sample_dur_us;

    /* 7. Stack High Water Mark measurement if task is created */
    size_t task_free_stack_bytes = SYSTEM_METRICS_TASK_STACK_BYTES;
    if (s_task_handle != NULL) {
        UBaseType_t hwm_words = uxTaskGetStackHighWaterMark(s_task_handle);
        task_free_stack_bytes = (size_t)hwm_words * sizeof(StackType_t);
    }
    next_snap.task_stack_min_free_bytes = task_free_stack_bytes;

    /* 8. Commit snapshot and cumulative statistics under mutex lock */
    if (xSemaphoreTake(s_mutex, SYSTEM_METRICS_MUTEX_TIMEOUT_TICKS) != pdTRUE) {
        ESP_LOGW(TAG, "Failed to acquire mutex to commit metrics snapshot");
        return ESP_ERR_TIMEOUT;
    }

    s_active_snapshot = next_snap;

    /* Update cumulative overhead statistics */
    s_sample_count++;
    s_last_sample_time_us = sample_dur_us;
    s_total_sample_time_us += sample_dur_us;
    if (sample_dur_us < s_min_sample_time_us) {
        s_min_sample_time_us = sample_dur_us;
    }
    if (sample_dur_us > s_max_sample_time_us) {
        s_max_sample_time_us = sample_dur_us;
    }
    if (task_free_stack_bytes < s_task_min_free_stack_bytes) {
        s_task_min_free_stack_bytes = task_free_stack_bytes;
    }

    xSemaphoreGive(s_mutex);

    /* 9. Notify Health Manager observer if registered (weak symbol linkage) */
    if (health_manager_process_metrics != NULL) {
        health_manager_process_metrics(&next_snap);
    }

    return ESP_OK;
}

/**
 * @brief Background FreeRTOS task function for periodic measurement.
 */
static void prv_system_metrics_task(void *pvParameters)
{
    (void)pvParameters;

    ESP_LOGI(TAG, "System metrics sampler task entered main loop (Priority %d, Interval %d ms)",
             SYSTEM_METRICS_TASK_PRIORITY, SYSTEM_METRICS_SAMPLE_INTERVAL_MS);

    /* Signal deterministic startup handshake */
    if (s_started_sem != NULL) {
        xSemaphoreGive(s_started_sem);
    }

    TickType_t last_wake_time = xTaskGetTickCount();
    const TickType_t period_ticks = pdMS_TO_TICKS(SYSTEM_METRICS_SAMPLE_INTERVAL_MS);

    while (1) {
        /* Enforce exact periodic sampling interval without drift */
        vTaskDelayUntil(&last_wake_time, period_ticks);

        prv_execute_sample_cycle();
    }
}

/* =========================================================================
 * Public API Implementation
 * ========================================================================= */

bool system_metrics_init(void)
{
    if (s_initialized) {
        ESP_LOGW(TAG, "system_metrics_init() called again — already initialized");
        return true;
    }

    /* Initialize static mutex */
    s_mutex = xSemaphoreCreateMutexStatic(&s_mutex_buffer);
    if (s_mutex == NULL) {
        ESP_LOGE(TAG, "Failed to create static mutex");
        return false;
    }

    /* Initialize startup handshake semaphore */
    s_started_sem = xSemaphoreCreateBinaryStatic(&s_started_sem_buffer);
    if (s_started_sem == NULL) {
        ESP_LOGE(TAG, "Failed to create static started semaphore");
        return false;
    }

    /* Configure and enable ESP32-C6 internal temperature sensor.
     * Range [20C, 100C] matches hardware attribute table {-1, 7, 20, 100, 2}
     * ensuring that warning (75C) and critical (85C) thresholds are strictly within
     * the calibrated, driver-supported operational range. */
    temperature_sensor_config_t tsens_config =
        TEMPERATURE_SENSOR_CONFIG_DEFAULT(SYSTEM_METRICS_TSENS_RANGE_MIN, SYSTEM_METRICS_TSENS_RANGE_MAX);
    esp_err_t err = temperature_sensor_install(&tsens_config, &s_tsens_handle);
    if (err == ESP_OK) {
        err = temperature_sensor_enable(s_tsens_handle);
        if (err == ESP_OK) {
            s_tsens_installed = true;
            ESP_LOGI(TAG, "Internal SoC temperature sensor driver enabled (%dC to +%dC)",
                     SYSTEM_METRICS_TSENS_RANGE_MIN, SYSTEM_METRICS_TSENS_RANGE_MAX);
        } else {
            ESP_LOGW(TAG, "Failed to enable temperature sensor: %s", esp_err_to_name(err));
        }
    } else {
        ESP_LOGW(TAG, "Failed to install temperature sensor driver: %s", esp_err_to_name(err));
    }

    /* Reset state variables */
    memset(&s_active_snapshot, 0, sizeof(s_active_snapshot));
    s_cpu_has_baseline = false;
    s_prev_idle_counter_us = 0;
    s_prev_total_time_us = 0;
    s_sample_count = 0;
    s_min_sample_time_us = UINT32_MAX;
    s_max_sample_time_us = 0;
    s_total_sample_time_us = 0;
    s_last_sample_time_us = 0;
    s_task_min_free_stack_bytes = SYSTEM_METRICS_TASK_STACK_BYTES;

    /* Execute initial sample synchronously to populate baseline metadata */
    prv_execute_sample_cycle();

    s_initialized = true;
    ESP_LOGI(TAG, "System Metrics module initialized successfully");
    return true;
}

bool system_metrics_is_initialized(void)
{
    return s_initialized;
}

bool system_metrics_start(void)
{
    if (!s_initialized) {
        ESP_LOGE(TAG, "Cannot start system_metrics: module not initialized");
        return false;
    }

    if (s_task_started) {
        ESP_LOGW(TAG, "system_metrics_start() called again — task already running");
        return true;
    }

    /* Spawn static FreeRTOS task */
    s_task_handle = xTaskCreateStatic(
        prv_system_metrics_task,
        "sys_metrics",
        sizeof(s_task_stack) / sizeof(StackType_t),
        NULL,
        SYSTEM_METRICS_TASK_PRIORITY,
        s_task_stack,
        &s_task_tcb
    );

    if (s_task_handle == NULL) {
        ESP_LOGE(TAG, "Failed to create static sys_metrics task");
        return false;
    }

    /* Await deterministic task startup confirmation */
    if (xSemaphoreTake(s_started_sem, pdMS_TO_TICKS(1000)) != pdTRUE) {
        ESP_LOGE(TAG, "Timeout awaiting sys_metrics task startup handshake");
        return false;
    }

    s_task_started = true;
    ESP_LOGI(TAG, "sys_metrics task successfully started and verified");
    return true;
}

esp_err_t system_metrics_get_snapshot(system_metrics_snapshot_t *out_snapshot)
{
    if (out_snapshot == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!s_initialized || s_mutex == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    if (xSemaphoreTake(s_mutex, SYSTEM_METRICS_MUTEX_TIMEOUT_TICKS) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    memcpy(out_snapshot, &s_active_snapshot, sizeof(system_metrics_snapshot_t));
    xSemaphoreGive(s_mutex);

    return ESP_OK;
}

esp_err_t system_metrics_get_overhead_stats(system_metrics_overhead_stats_t *out_stats)
{
    if (out_stats == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!s_initialized || s_mutex == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    if (xSemaphoreTake(s_mutex, SYSTEM_METRICS_MUTEX_TIMEOUT_TICKS) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    out_stats->sample_count = s_sample_count;
    out_stats->min_sample_time_us = (s_min_sample_time_us == UINT32_MAX) ? 0 : s_min_sample_time_us;
    out_stats->max_sample_time_us = s_max_sample_time_us;
    out_stats->last_sample_time_us = s_last_sample_time_us;
    out_stats->task_stack_allocated = SYSTEM_METRICS_TASK_STACK_BYTES;
    out_stats->task_stack_min_free_bytes = s_task_min_free_stack_bytes;

    if (s_sample_count > 0) {
        out_stats->avg_sample_time_us = (uint32_t)(s_total_sample_time_us / s_sample_count);
        /* Duty cycle = avg_us / 1,000,000 * 100 */
        out_stats->measured_duty_cycle_pct = ((float)out_stats->avg_sample_time_us / 1000000.0f) * 100.0f;
    } else {
        out_stats->avg_sample_time_us = 0;
        out_stats->measured_duty_cycle_pct = 0.0f;
    }

    xSemaphoreGive(s_mutex);
    return ESP_OK;
}

esp_err_t system_metrics_sample_now(void)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    return prv_execute_sample_cycle();
}

const char *system_metrics_reset_reason_to_str(system_reset_reason_t reason)
{
    switch (reason) {
        case SYSTEM_RESET_REASON_POWER_ON:  return "POWER_ON";
        case SYSTEM_RESET_REASON_SOFTWARE:  return "SOFTWARE_RESTART";
        case SYSTEM_RESET_REASON_PANIC:     return "PANIC_RESET";
        case SYSTEM_RESET_REASON_WATCHDOG:  return "TASK_WATCHDOG_RESET";
        case SYSTEM_RESET_REASON_BROWNOUT:  return "BROWNOUT_RESET";
        case SYSTEM_RESET_REASON_DEEP_SLEEP:return "DEEP_SLEEP_WAKE";
        case SYSTEM_RESET_REASON_UNKNOWN:
        default:                            return "UNKNOWN";
    }
}

void system_metrics_set_test_hooks(const system_metrics_test_hooks_t *hooks)
{
    if (hooks != NULL) {
        s_test_hooks = *hooks;
    } else {
        s_test_hooks.read_temperature = NULL;
        s_test_hooks.read_cpu_counters = NULL;
    }
}
