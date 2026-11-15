/**
 * @file health_policy.h
 * @brief Centralized operational health evaluation thresholds, hysteresis parameters,
 *        and component criticality rules for the Conductor Health Manager.
 *
 * NOTE: These thresholds represent Conductor project operational engineering policies,
 * NOT physical silicon destruction limits.
 */

#ifndef HEALTH_POLICY_H
#define HEALTH_POLICY_H

#include <stdint.h>
#include <stdbool.h>
#include <assert.h>

#ifdef __cplusplus
extern "C" {
#endif

/* =========================================================================
 * SoC Die Temperature Sensor Hardware Limits & Thresholds (Celsius)
 *
 * ESP-IDF v6.1 / ESP32-C6 hardware driver provides 5 calibrated ranges:
 *   {-2, 5,  50, 125, 3} -> [50C, 125C], error < 3C
 *   {-1, 7,  20, 100, 2} -> [20C, 100C], error < 2C  <-- CONFIGURED RANGE
 *   { 0, 15, -10, 80, 1} -> [-10C, 80C], error < 1C
 *   { 1, 11, -30, 50, 2} -> [-30C, 50C], error < 2C
 *   { 2, 10, -40, 20, 3} -> [-40C, 20C], error < 3C
 *
 * Range [20C, 100C] fully accommodates both WARNING (75C) and CRITICAL (85C)
 * operational health thresholds without clipping or exceeding measurement limits.
 * ========================================================================= */

/** @brief Configured driver range minimum for ESP32-C6 internal temp sensor */
#define HEALTH_POLICY_SOC_TEMP_SENSOR_MIN_C     20.0f

/** @brief Configured driver range maximum for ESP32-C6 internal temp sensor */
#define HEALTH_POLICY_SOC_TEMP_SENSOR_MAX_C     100.0f

/** @brief SoC temperature warning threshold (DEGRADED entry) */
#define HEALTH_POLICY_SOC_TEMP_WARN_ENTER_C     75.0f

/** @brief SoC temperature warning recovery threshold (DEGRADED exit with hysteresis) */
#define HEALTH_POLICY_SOC_TEMP_WARN_EXIT_C      70.0f

/** @brief SoC temperature critical threshold (FAULT entry) */
#define HEALTH_POLICY_SOC_TEMP_CRIT_ENTER_C     85.0f

/** @brief SoC temperature critical recovery threshold (FAULT exit with hysteresis) */
#define HEALTH_POLICY_SOC_TEMP_CRIT_EXIT_C      80.0f

/* =========================================================================
 * CPU Load Utilization Thresholds & Sustained Windows
 * ========================================================================= */

/** @brief CPU load warning threshold (percent) */
#define HEALTH_POLICY_CPU_LOAD_WARN_ENTER_PCT   85.0f

/** @brief CPU load warning recovery threshold (percent with hysteresis) */
#define HEALTH_POLICY_CPU_LOAD_WARN_EXIT_PCT    75.0f

/** @brief CPU load critical threshold (percent) */
#define HEALTH_POLICY_CPU_LOAD_CRIT_ENTER_PCT   95.0f

/** @brief CPU load critical recovery threshold (percent with hysteresis) */
#define HEALTH_POLICY_CPU_LOAD_CRIT_EXIT_PCT    85.0f

/**
 * @brief Number of consecutive high samples required before declaring CPU DEGRADED.
 * Prevents momentary transient spikes (e.g. crypto/hash/init) from triggering alarms.
 */
#define HEALTH_POLICY_CPU_SUSTAINED_WARN_SAMPLES 5

/**
 * @brief Number of consecutive critical samples required before declaring CPU FAULT.
 */
#define HEALTH_POLICY_CPU_SUSTAINED_CRIT_SAMPLES 10

/* =========================================================================
 * Heap Memory Capacity & Fragmentation Thresholds
 *
 * Current Health vs Historical Watermark Separation:
 *   Current health is evaluated solely from live dynamic metrics:
 *     - current heap usage percent
 *     - current free heap bytes
 *     - current largest contiguous block
 *   Historical minimum free heap is retained purely as a diagnostic indicator
 *   and does NOT lock current health in permanent FAULT/DEGRADED state.
 * ========================================================================= */

/** @brief Current internal heap utilization percentage warning threshold (DEGRADED) */
#define HEALTH_POLICY_HEAP_WARN_USAGE_PCT           85.0f

/** @brief Current internal heap utilization percentage critical threshold (FAULT) */
#define HEALTH_POLICY_HEAP_CRIT_USAGE_PCT           95.0f

/** @brief Current internal heap free bytes warning threshold (32 KB) */
#define HEALTH_POLICY_HEAP_WARN_CURRENT_FREE_BYTES  (32 * 1024)

/** @brief Current internal heap free bytes critical threshold (16 KB) */
#define HEALTH_POLICY_HEAP_CRIT_CURRENT_FREE_BYTES  (16 * 1024)

/** @brief Current internal heap largest free block fragmentation threshold (8 KB) */
#define HEALTH_POLICY_HEAP_WARN_LARGEST_BLOCK       (8 * 1024)

/** @brief Historical minimum free heap pressure diagnostic watermark threshold (32 KB) */
#define HEALTH_POLICY_HEAP_HISTORICAL_PRESSURE_BYTES (32 * 1024)

/* =========================================================================
 * Staleness Thresholds
 * ========================================================================= */

/** @brief Maximum age of System Metrics sample before declaring metrics STALE (5000 ms) */
#define HEALTH_POLICY_METRICS_STALE_AGE_MS      5000

/** @brief Maximum age of Z906 physical amplifier communication before declaring STALE (10000 ms) */
#define HEALTH_POLICY_Z906_STALE_AGE_MS         10000

/* =========================================================================
 * Compile-Time Self-Consistency Assertions
 * ========================================================================= */

#if defined(__STDC_VERSION__) && (__STDC_VERSION__ >= 201112L)
_Static_assert(HEALTH_POLICY_SOC_TEMP_SENSOR_MIN_C < HEALTH_POLICY_SOC_TEMP_SENSOR_MAX_C,
               "Sensor min must be less than sensor max");
_Static_assert(HEALTH_POLICY_SOC_TEMP_WARN_EXIT_C < HEALTH_POLICY_SOC_TEMP_WARN_ENTER_C,
               "Temperature warn exit must be less than warn enter (hysteresis)");
_Static_assert(HEALTH_POLICY_SOC_TEMP_WARN_ENTER_C < HEALTH_POLICY_SOC_TEMP_CRIT_ENTER_C,
               "Temperature warn enter must be less than crit enter");
_Static_assert(HEALTH_POLICY_SOC_TEMP_CRIT_EXIT_C < HEALTH_POLICY_SOC_TEMP_CRIT_ENTER_C,
               "Temperature crit exit must be less than crit enter (hysteresis)");
_Static_assert(HEALTH_POLICY_SOC_TEMP_CRIT_ENTER_C <= HEALTH_POLICY_SOC_TEMP_SENSOR_MAX_C,
               "Temperature crit enter must be within measurable sensor max");
_Static_assert(HEALTH_POLICY_SOC_TEMP_SENSOR_MIN_C <= HEALTH_POLICY_SOC_TEMP_WARN_EXIT_C,
               "Temperature warn exit must be within measurable sensor min");
_Static_assert(HEALTH_POLICY_CPU_LOAD_WARN_EXIT_PCT < HEALTH_POLICY_CPU_LOAD_WARN_ENTER_PCT,
               "CPU warn exit must be less than warn enter");
_Static_assert(HEALTH_POLICY_CPU_LOAD_WARN_ENTER_PCT < HEALTH_POLICY_CPU_LOAD_CRIT_ENTER_PCT,
               "CPU warn enter must be less than crit enter");
_Static_assert(HEALTH_POLICY_CPU_LOAD_CRIT_EXIT_PCT < HEALTH_POLICY_CPU_LOAD_CRIT_ENTER_PCT,
               "CPU crit exit must be less than crit enter");
_Static_assert(HEALTH_POLICY_HEAP_CRIT_CURRENT_FREE_BYTES < HEALTH_POLICY_HEAP_WARN_CURRENT_FREE_BYTES,
               "Heap crit free bytes must be less than warn free bytes");
#endif

#ifdef __cplusplus
}
#endif

#endif /* HEALTH_POLICY_H */
