/**
 * @file health_manager.h
 * @brief Health Manager module for Conductor: central health aggregation and classification.
 *
 * Sits in the middle of the observability hierarchy:
 *   System Metrics (MEASURE) -> Health Manager (INTERPRET) -> Telemetry/CLI (EXPOSE)
 *
 * Responsibilities:
 *   - Collects, aggregates, and interprets metrics and subsystem health signals.
 *   - Classifies health status: UNKNOWN, HEALTHY, DEGRADED, FAULT.
 *   - Identifies the primary failing component and root cause reason.
 *   - Applies operational hysteresis and sustained thresholds to prevent alarm flip-flop.
 *   - Tracks data freshness and enforces staleness deadlines autonomously via periodic timer.
 *   - Provides atomic, thread-safe health snapshots.
 *   - Dispatches health transition events to registered observers (event-driven, change-only).
 *
 * Non-Responsibilities (Mandatory Constraints):
 *   - DOES NOT measure hardware or query drivers directly (System Metrics responsibility).
 *   - DOES NOT perform automatic recovery: no task restarts, no reboots, no Zigbee reset.
 *   - DOES NOT feed or own the Task Watchdog (Watchdog responsibility).
 *   - DOES NOT make OTA rollback decisions (OTA Health responsibility).
 *   - DOES NOT call Zigbee or ZCL APIs directly (No reverse dependency).
 */

#ifndef HEALTH_MANAGER_H
#define HEALTH_MANAGER_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"
#include "system_metrics.h"

#ifdef __cplusplus
extern "C" {
#endif

/* =========================================================================
 * Health Status Enumeration
 * ========================================================================= */

/**
 * @brief Discrete health states for the system and individual components.
 */
typedef enum {
    HEALTH_STATUS_UNKNOWN = 0,  /**< Insufficient telemetry, uninitialized, or baseline pending */
    HEALTH_STATUS_HEALTHY,      /**< Fully operational within acceptable boundaries */
    HEALTH_STATUS_DEGRADED,     /**< Operational with non-fatal impairment or warnings */
    HEALTH_STATUS_FAULT         /**< Critical failure requiring attention */
} health_status_t;

/* =========================================================================
 * Health Component Enumeration
 * ========================================================================= */

/**
 * @brief Evaluated architectural components in the Conductor system.
 */
typedef enum {
    HEALTH_COMP_SYSTEM = 0,         /**< Aggregate top-level system health */
    HEALTH_COMP_SYSTEM_METRICS,     /**< Metrics measurement engine freshness */
    HEALTH_COMP_SOC_TEMPERATURE,    /**< ESP32-C6 internal die temperature */
    HEALTH_COMP_CPU,                /**< CPU utilization load */
    HEALTH_COMP_MEMORY,             /**< Internal 8-bit heap capacity & fragmentation */
    HEALTH_COMP_WATCHDOG,           /**< Task Watchdog and reset diagnostics */
    HEALTH_COMP_NVS,                /**< Non-Volatile Storage subsystem */
    HEALTH_COMP_INITIALIZER,        /**< Boot dependency sequence outcome */
    HEALTH_COMP_ZIGBEE,             /**< Zigbee native network stack */
    HEALTH_COMP_Z906_TRANSPORT,     /**< Z906 UART serial communications */
    HEALTH_COMP_COMMAND_VERIFIER,   /**< Hardware state verifier engine */
    HEALTH_COMP_OTA,                /**< Firmware OTA client & partition health */
    HEALTH_COMP_COUNT               /**< Total number of evaluated components */
} health_component_t;

/* =========================================================================
 * Health Reason Codes
 * ========================================================================= */

/**
 * @brief Root cause reason codes for degraded or fault health statuses.
 */
typedef enum {
    HEALTH_REASON_NONE = 0,                 /**< Nominal / no degradation */
    HEALTH_REASON_NOT_INITIALIZED,          /**< Subsystem not yet initialized */
    HEALTH_REASON_METRIC_NOT_READY,         /**< Baseline sample pending */
    HEALTH_REASON_METRIC_STALE,             /**< Telemetry sample age exceeded threshold */

    /* Temperature */
    HEALTH_REASON_SOC_TEMP_WARNING,         /**< SoC temperature above warning threshold */
    HEALTH_REASON_SOC_TEMP_CRITICAL,        /**< SoC temperature above critical threshold */
    HEALTH_REASON_SOC_TEMP_SENSOR_ERROR,    /**< Hardware sensor read failure */

    /* CPU */
    HEALTH_REASON_CPU_LOAD_SUSTAINED_HIGH,  /**< CPU load sustained above warning threshold */
    HEALTH_REASON_CPU_LOAD_CRITICAL,        /**< CPU load sustained at critical saturation */

    /* Memory */
    HEALTH_REASON_HEAP_WARNING,             /**< Current heap utilization high or free low */
    HEALTH_REASON_HEAP_CRITICAL,            /**< Current heap critical exhaustion danger */
    HEALTH_REASON_HEAP_FRAGMENTED,          /**< Current heap largest block severely fragmented */
    HEALTH_REASON_HEAP_HISTORICAL_PRESSURE, /**< Historical minimum free watermark observed (diagnostic) */

    /* Subsystems */
    HEALTH_REASON_WATCHDOG_RESET_DETECTED,  /**< Previous boot was triggered by TWDT */
    HEALTH_REASON_WATCHDOG_TASK_STALLED,    /**< Task starvation detected */
    HEALTH_REASON_ZIGBEE_COMMISSION_FAILED, /**< Pairing timeout or join rejected */
    HEALTH_REASON_ZIGBEE_DEGRADED,          /**< Network degraded or parent lost */
    HEALTH_REASON_Z906_STALE,               /**< Subwoofer telemetry age exceeded timeout */
    HEALTH_REASON_Z906_COMMUNICATION_FAULT, /**< Framing errors or CRC mismatches */
    HEALTH_REASON_COMMAND_VERIFIER_UNAVAILABLE, /**< Verifier mutex timeout or disabled */
    HEALTH_REASON_OTA_HEALTH_FAULT,         /**< Firmware rollback triggered */
    HEALTH_REASON_NVS_FAULT,                /**< Persistent storage failure */
    HEALTH_REASON_INITIALIZER_FAILED,       /**< Boot lifecycle component failure */
} health_reason_t;

/* =========================================================================
 * Health Snapshot Structures
 * ========================================================================= */

/**
 * @brief Discrete health state of an individual evaluated component.
 */
typedef struct {
    health_status_t status;         /**< Current health status */
    health_reason_t reason;         /**< Primary reason code explaining status */
    uint64_t        last_update_ms; /**< Timestamp of last status evaluation */
} health_component_state_t;

/**
 * @brief Complete immutable health snapshot for the entire Conductor system.
 */
typedef struct {
    health_status_t overall_status;         /**< Unified top-level health classification */
    health_component_t primary_component;   /**< Primary component causing degradation/fault */
    health_reason_t primary_reason;         /**< Primary root cause reason */
    uint64_t        status_since_ms;        /**< Timestamp when current overall status began */
    uint64_t        snapshot_timestamp_ms;  /**< Timestamp when snapshot was assembled */

    /** Detailed health breakdown per architectural component */
    health_component_state_t components[HEALTH_COMP_COUNT];
} health_snapshot_t;

/* =========================================================================
 * Observer Callback Interface
 * ========================================================================= */

/**
 * @brief Health transition observer callback signature.
 *
 * Invoked ONLY when overall status, primary component, or primary reason changes.
 * The callback is executed in the context of the metrics processing or staleness timer.
 * Implementations MUST NOT block, perform long calculations, or make Zigbee ZCL calls directly.
 *
 * @param snapshot Pointer to immutable snapshot capturing the transition.
 * @param user_ctx User-supplied context pointer.
 */
typedef void (*health_manager_change_cb_t)(const health_snapshot_t *snapshot, void *user_ctx);

/* =========================================================================
 * Public API
 * ========================================================================= */

/**
 * @brief Initializes the Health Manager module and loads evaluation policies.
 *
 * Prepares the synchronization mutex, initializes all component states to
 * HEALTH_STATUS_UNKNOWN with HEALTH_REASON_NOT_INITIALIZED, and creates the
 * low-frequency staleness evaluation timer.
 *
 * Lifecycle: Called during PHASE_MANAGEMENT by System Initializer.
 *
 * @return true if initialization succeeded, false on mutex/timer failure.
 */
bool health_manager_init(void);

/**
 * @brief Activates the Health Manager and starts the autonomous staleness timer.
 *
 * Starts the periodic timer (2000ms) to ensure continuous monitoring and
 * autonomous staleness detection even if metric producers become completely silent.
 *
 * Lifecycle: Called during PHASE_ACTIVATE by System Initializer.
 *
 * @return true on success, false if not initialized.
 */
bool health_manager_start(void);

/**
 * @brief Ingests and interprets a newly published System Metrics snapshot.
 *
 * Evaluates SoC temperature with hysteresis, CPU load with sustained samples,
 * and current heap memory capacity. Recomputes overall system health atomically.
 *
 * @param metrics Pointer to fresh System Metrics snapshot.
 */
void health_manager_process_metrics(const system_metrics_snapshot_t *metrics);

/**
 * @brief Manually triggers a staleness and health evaluation pass.
 *
 * Useful for automated testing and immediate evaluation without waiting for the timer.
 */
void health_manager_tick_now(void);

/**
 * @brief Updates the health status of an external subsystem component.
 *
 * Thread-safe: Acquires health manager mutex, updates component state,
 * and recalculates top-level overall system health.
 *
 * @param comp Architectural component to update.
 * @param status New health status classification.
 * @param reason Detailed root cause reason code.
 */
void health_manager_set_component_health(health_component_t comp,
                                        health_status_t status,
                                        health_reason_t reason);

/**
 * @brief Retrieves an atomic snapshot of current system health.
 *
 * @param[out] out_snapshot Pointer to destination snapshot structure. Must not be NULL.
 * @return ESP_OK on success, ESP_ERR_INVALID_ARG if out_snapshot is NULL,
 *         or ESP_ERR_TIMEOUT if synchronization mutex could not be acquired.
 */
esp_err_t health_manager_get_snapshot(health_snapshot_t *out_snapshot);

/**
 * @brief Registers a single global observer for health transition events.
 *
 * Thread-safe: Sets the transition observer callback.
 * Passing NULL removes the current observer.
 *
 * @param observer Callback function to invoke on health transitions.
 * @param user_ctx User context pointer passed to callback.
 * @return ESP_OK on success.
 */
esp_err_t health_manager_register_change_observer(health_manager_change_cb_t observer, void *user_ctx);

/**
 * @brief Converts a health status enum to a human-readable string.
 *
 * @param status Health status enum value.
 * @return Constant string description (e.g. "HEALTHY", "DEGRADED", "FAULT", "UNKNOWN").
 */
const char *health_status_to_str(health_status_t status);

/**
 * @brief Converts a health reason enum to a human-readable string.
 *
 * @param reason Health reason enum value.
 * @return Constant string description.
 */
const char *health_reason_to_str(health_reason_t reason);

/**
 * @brief Converts a component enum to a human-readable string.
 *
 * @param comp Component enum value.
 * @return Constant string description.
 */
const char *health_component_to_str(health_component_t comp);

#ifdef __cplusplus
}
#endif

#endif /* HEALTH_MANAGER_H */
