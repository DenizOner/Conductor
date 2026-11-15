/**
 * @file system_metrics.h
 * @brief System Metrics measurement module for ESP32-C6 / ESP-IDF / FreeRTOS.
 *
 * Sits at the bottom of the observability hierarchy:
 *   System Metrics (MEASURE) -> Health Manager (INTERPRET) -> Telemetry/CLI (EXPOSE)
 *
 * Responsibilities:
 *   - Measures raw hardware and RTOS runtime metrics: SoC die temperature,
 *     O(1) CPU load percentage, internal heap allocation statistics,
 *     monotonic uptime, and confirmed hardware reset causes.
 *   - Provides atomic, thread-safe, immutable snapshot copies to consumers.
 *   - Pure measurement layer: DOES NOT interpret health, apply alarm thresholds,
 *     restart tasks, or trigger recovery policies (Strict Separation of Concerns).
 *   - Zero dynamic runtime memory allocation (100% static allocation).
 *   - Zero flash / NVS writes on sampling cycles.
 */

#ifndef SYSTEM_METRICS_H
#define SYSTEM_METRICS_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"
#include "esp_system.h"

#ifdef __cplusplus
extern "C" {
#endif

/* =========================================================================
 * Hardware Constants & Sensor Configuration Limits
 * ========================================================================= */

/** @brief Minimum calibrated range value for ESP32-C6 internal SoC die temp sensor (Celsius) */
#define SYSTEM_METRICS_TSENS_RANGE_MIN          20

/** @brief Maximum calibrated range value for ESP32-C6 internal SoC die temp sensor (Celsius) */
#define SYSTEM_METRICS_TSENS_RANGE_MAX          100

/** @brief Static FreeRTOS task stack allocation size (Bytes) */
#define SYSTEM_METRICS_TASK_STACK_BYTES         3072

/** @brief Static FreeRTOS task priority */
#define SYSTEM_METRICS_TASK_PRIORITY            1

/** @brief Deterministic periodic sampling interval (Milliseconds) */
#define SYSTEM_METRICS_SAMPLE_INTERVAL_MS       1000

/* =========================================================================
 * Reset Reason Enumeration
 * ========================================================================= */

/**
 * @brief Normalized system reset reasons mapped from ESP-IDF esp_reset_reason().
 */
typedef enum {
    SYSTEM_RESET_REASON_UNKNOWN = 0,    /**< Unknown or unclassified reset cause */
    SYSTEM_RESET_REASON_POWER_ON,       /**< VDD power-on reset */
    SYSTEM_RESET_REASON_SOFTWARE,       /**< Software requested reset via esp_restart() */
    SYSTEM_RESET_REASON_PANIC,          /**< Software panic / exception / unhandled abort */
    SYSTEM_RESET_REASON_WATCHDOG,       /**< Task watchdog or interrupt watchdog reset */
    SYSTEM_RESET_REASON_BROWNOUT,       /**< Brownout detection circuit reset */
    SYSTEM_RESET_REASON_DEEP_SLEEP,     /**< Wakeup from deep sleep */
} system_reset_reason_t;

/* =========================================================================
 * System Metrics Snapshot Structure
 * ========================================================================= */

/**
 * @brief Atomic snapshot structure capturing all raw system measurements.
 *
 * Thread-safe: Access must be performed exclusively via system_metrics_get_snapshot().
 */
typedef struct {
    uint64_t timestamp_ms;                  /**< Monotonic timestamp when snapshot was taken */
    uint64_t uptime_ms;                     /**< Monotonic system uptime in milliseconds */

    float    soc_temperature_c;             /**< ESP32-C6 internal SoC die temperature in Celsius */
    bool     soc_temperature_valid;         /**< true if sensor read succeeded; false on driver error */

    float    cpu_load_percent;              /**< CPU utilization percentage (0.0% - 100.0%) */
    bool     cpu_load_valid;                /**< true if delta calculation was valid; false on initial sample */

    size_t   heap_total_bytes;              /**< Total addressable internal 8-bit heap capacity (used + free) */
    size_t   heap_used_bytes;               /**< Allocated internal heap bytes */
    size_t   heap_free_bytes;               /**< Available internal heap bytes */
    float    heap_usage_percent;            /**< Internal heap utilization percentage (used / total * 100) */
    size_t   heap_min_free_bytes;           /**< Lifetime minimum recorded free heap bytes (historical watermark) */
    size_t   heap_largest_free_block_bytes; /**< Largest contiguous allocatable block in internal heap */

    system_reset_reason_t reset_reason;     /**< Normalized system reset reason */
    esp_reset_reason_t    reset_reason_raw; /**< Native ESP-IDF hardware reset reason code */

    /* Runtime execution profiling */
    uint32_t sample_duration_us;            /**< Duration of the most recent measurement cycle in microseconds */
    size_t   task_stack_min_free_bytes;     /**< FreeRTOS task minimum recorded stack watermark in bytes */
} system_metrics_snapshot_t;

/* =========================================================================
 * Measurement Profiling & Overhead Statistics
 * ========================================================================= */

/**
 * @brief Cumulative runtime execution statistics for System Metrics sampling.
 */
typedef struct {
    uint32_t sample_count;              /**< Total number of executed sampling cycles */
    uint32_t min_sample_time_us;        /**< Minimum observed execution duration in microseconds */
    uint32_t max_sample_time_us;        /**< Maximum observed execution duration in microseconds */
    uint32_t avg_sample_time_us;        /**< Mean execution duration in microseconds */
    uint32_t last_sample_time_us;       /**< Duration of the last sample in microseconds */
    float    measured_duty_cycle_pct;   /**< Measured sampling duty-cycle (avg_us / interval_us * 100) */
    size_t   task_stack_allocated;      /**< Total configured stack size (3072 B) */
    size_t   task_stack_min_free_bytes; /**< High water mark remaining stack headroom in bytes */
} system_metrics_overhead_stats_t;

/* =========================================================================
 * Test Seam Injection (for Unit Testing / Mocks)
 * ========================================================================= */

/**
 * @brief Function pointer type for temperature sensor reader mock.
 */
typedef esp_err_t (*system_metrics_temp_fn_t)(float *out_celsius);

/**
 * @brief Function pointer type for CPU runtime counter reader mock.
 */
typedef void (*system_metrics_cpu_fn_t)(uint32_t *out_idle_time_us, uint64_t *out_total_time_us);

/**
 * @brief Test seam hooks to inject mock measurements during automated unit tests.
 */
typedef struct {
    system_metrics_temp_fn_t read_temperature; /**< Mock temperature reader */
    system_metrics_cpu_fn_t  read_cpu_counters;/**< Mock CPU idle/total counter reader */
} system_metrics_test_hooks_t;

/* =========================================================================
 * Public API
 * ========================================================================= */

/**
 * @brief Initializes the System Metrics measurement module.
 *
 * Prepares synchronization mutexes, configures the internal ESP32-C6 temperature
 * sensor driver for 20C-100C, reads initial boot metadata, and populates the baseline snapshot.
 *
 * Lifecycle: Called during PHASE_MANAGEMENT by System Initializer.
 *
 * @return true if initialization succeeded, false on driver or mutex allocation failure.
 */
bool system_metrics_init(void);

/**
 * @brief Checks if the System Metrics module has been initialized.
 *
 * @return true if initialized, false otherwise.
 */
bool system_metrics_is_initialized(void);

/**
 * @brief Spawns the static background measurement sampler task.
 *
 * The task samples hardware metrics at a deterministic 1000ms interval.
 * Uses a private StaticSemaphore_t startup handshake to guarantee the task
 * is in its RUNNING/STARTED state before returning.
 *
 * Lifecycle: Called during PHASE_ACTIVATE by System Initializer.
 *
 * @return true if task spawned and confirmed started, false on timeout.
 */
bool system_metrics_start(void);

/**
 * @brief Retrieves a consistent, atomic snapshot of the latest system metrics.
 *
 * Thread-safe: Copies internal metrics under mutex lock to prevent torn reads.
 *
 * @param[out] out_snapshot Pointer to destination snapshot structure. Must not be NULL.
 * @return ESP_OK on success, ESP_ERR_INVALID_ARG if out_snapshot is NULL,
 *         or ESP_ERR_TIMEOUT if synchronization mutex could not be acquired.
 */
esp_err_t system_metrics_get_snapshot(system_metrics_snapshot_t *out_snapshot);

/**
 * @brief Retrieves cumulative measurement execution profiling and stack headroom statistics.
 *
 * @param[out] out_stats Pointer to destination stats structure. Must not be NULL.
 * @return ESP_OK on success, or ESP_ERR_INVALID_ARG if out_stats is NULL.
 */
esp_err_t system_metrics_get_overhead_stats(system_metrics_overhead_stats_t *out_stats);

/**
 * @brief Manually executes a single measurement sample cycle.
 *
 * Useful for synchronous unit tests and initial on-demand sampling without
 * waiting for the periodic background task delay.
 *
 * @return ESP_OK on success, or error code on failure.
 */
esp_err_t system_metrics_sample_now(void);

/**
 * @brief Converts a normalized system reset reason enum to a human-readable string.
 *
 * @param reason Reset reason enum value.
 * @return Constant string description (never NULL).
 */
const char *system_metrics_reset_reason_to_str(system_reset_reason_t reason);

/**
 * @brief Injects mock measurement hooks for failure and boundary testing.
 *
 * Pass NULL to restore native ESP32-C6 hardware drivers.
 *
 * @param hooks Pointer to test hooks structure, or NULL to reset to defaults.
 */
void system_metrics_set_test_hooks(const system_metrics_test_hooks_t *hooks);

#ifdef __cplusplus
}
#endif

#endif /* SYSTEM_METRICS_H */
