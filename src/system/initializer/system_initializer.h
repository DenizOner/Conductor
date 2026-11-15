/**
 * @file system_initializer.h
 * @brief Public API for the System Initializer — the single orchestration authority
 *        for Conductor's boot dependency graph, lifecycle phasing, failure policy,
 *        and global initialization readiness.
 *
 * ## Design Principles
 *
 * The System Initializer is NOT a bag of init() calls. It is the compile-time
 * encoded boot dependency graph of the entire firmware.
 *
 * Responsibilities:
 *   - Enforces the correct initialization ORDER based on verified source dependencies.
 *   - Separates INIT (prepare internal state, create resources) from ACTIVATE
 *     (spawn runtime tasks, enable interrupt-driven streams, start async services).
 *   - Classifies each init step as FATAL or DEGRADED.
 *   - Maintains the SINGLE SOURCE OF TRUTH for global initialization readiness.
 *   - Reports boot time, failed component, and phase for diagnostics.
 *   - Provides a stable read-only API for OTA health and CLI diagnostics.
 *
 * Non-Responsibilities:
 *   - Does NOT own internal module logic (UART registers, Zigbee endpoints, etc.)
 *   - Does NOT maintain duplicate copies of any module's runtime state.
 *   - Does NOT perform polling-based readiness waiting.
 *   - Does NOT fabricate READY status on partial initialization.
 *
 * ## Verified Boot Dependency Order (from source analysis)
 *
 * PHASE_EARLY:
 *   nvs_manager_init()        -- required by: cmd_verifier (NVS settings),
 *                                LED (NVS config), Zigbee (NVS credentials)
 *   wdt_manager_init()        -- required by: all tasks (must register before starting)
 *   debug_console_init()      -- depends on: wdt_manager (status queries)
 *
 * PHASE_CORE:
 *   service_locator_init()    -- required by: cmd_verifier registration, CLI lookup
 *   event_bus_init()          -- required by: UART RX task (posts), FSM (consumes)
 *   z906_state_init()         -- required by: FSM, cmd_verifier, telemetry, Zigbee pub
 *   z906_fsm_init()           -- depends on: event_bus, z906_state
 *   z906_cmd_verifier_init()  -- depends on: NVS; registers in ServiceLocator
 *
 * PHASE_HARDWARE:
 *   z906_gpio_hal_init()      -- must precede UART (serial line GPIO stabilization)
 *   z906_adc_hal_init()       -- required by: telemetry (cable/voltage sensing)
 *   z906_uart_hal_init()      -- required by: UART RX task, FSM, cmd_verifier, telemetry
 *
 * PHASE_MANAGEMENT:
 *   led_notification_init()   -- depends on: NVS (loads led_store config)
 *   ota_client_init()         -- calls ota_health_init() internally
 *   system_metrics_init()     -- measures raw RTOS and hardware metrics (DEGRADED)
 *   health_manager_init()     -- interprets health metrics and signals (DEGRADED)
 *
 * PHASE_CLI:
 *   cli_console_init()        -- depends on: service_locator (cmd_verifier registered)
 *
 * PHASE_ACTIVATE:
 *   xTaskCreateStatic()       -- spawns runtime tasks in priority order
 *   led_notification_register_task_handle() -- must run AFTER led_task is spawned
 *   system_metrics_start()    -- starts background metrics sampler (DEGRADED)
 *   health_manager_start()    -- activates health evaluation engine (DEGRADED)
 *
 * ## READY Semantics
 *
 * READY means: All required internal firmware infrastructure has been correctly
 * initialized and all runtime FreeRTOS tasks have been successfully spawned.
 *
 * READY does NOT mean:
 *   - Zigbee has joined a coordinator (async operational condition)
 *   - Z906 amplifier is physically connected (runtime operational condition)
 *   - OTA server is reachable (external dependency)
 *
 * ## Thread Safety
 *
 * system_initializer_get_state() and system_initializer_get_report() are safe
 * to call from any task after system_initializer_run() returns ESP_OK.
 *
 * Synchronization model:
 *   Each subsystem module's _start() API uses a private StaticSemaphore_t
 *   binary semaphore. The task gives the semaphore immediately before entering
 *   its while(1) loop, so _start() returns only after the task is STARTED.
 *   There is NO shared EventGroupHandle_t passed between modules.
 *
 *   s_state = READY is written by system_initializer_run() after phase_activate()
 *   returns (all tasks confirmed STARTED). At that point no concurrent writer
 *   exists, so no additional barrier is needed.
 *
 *   volatile is explicitly NOT used — it does not provide a memory barrier;
 *   it only prevents compiler register caching, not CPU store-buffer ordering.
 */

#ifndef SYSTEM_INITIALIZER_H
#define SYSTEM_INITIALIZER_H

#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* =========================================================================
 * Boot Phase Identifiers
 * ========================================================================= */

/**
 * @brief Sequential boot phases executed by the system initializer.
 *
 * Phase names encode the semantic layer of the firmware architecture,
 * making log output and failure reports self-documenting.
 */
typedef enum {
    BOOT_PHASE_NONE = 0,    /**< Not yet started */
    BOOT_PHASE_EARLY,       /**< NVS, Watchdog, Debug Console */
    BOOT_PHASE_CORE,        /**< Service Locator, Event Bus, State, FSM, Verifier */
    BOOT_PHASE_HARDWARE,    /**< GPIO, ADC, UART drivers (in dependency order) */
    BOOT_PHASE_MANAGEMENT,  /**< LED Notification, OTA Client + OTA Health, System Metrics, Health Manager */
    BOOT_PHASE_CLI,         /**< CLI Console initialization */
    BOOT_PHASE_ACTIVATE,    /**< FreeRTOS runtime task spawning */
    BOOT_PHASE_READY        /**< All phases complete; system operational */
} boot_phase_t;

/* =========================================================================
 * System Initialization State
 * ========================================================================= */

/**
 * @brief Global system initialization lifecycle state.
 *
 * This is the single source of truth for initialization readiness.
 * No other module may declare or publish a competing "system ready" signal.
 *
 * State transitions:
 *   NOT_STARTED -> IN_PROGRESS -> READY    (happy path)
 *   NOT_STARTED -> IN_PROGRESS -> FAILED   (fatal failure path)
 *
 * FAILED is terminal: it never transitions back to READY.
 * READY is set only after BOOT_PHASE_READY completes successfully.
 */
typedef enum {
    SYSTEM_INIT_NOT_STARTED = 0, /**< system_initializer_run() not yet called */
    SYSTEM_INIT_IN_PROGRESS,     /**< Boot sequence currently executing */
    SYSTEM_INIT_READY,           /**< All required phases completed successfully */
    SYSTEM_INIT_FAILED           /**< Fatal failure; system cannot operate safely */
} system_init_state_t;

/* =========================================================================
 * Boot Report
 * ========================================================================= */

/**
 * @brief Structured boot outcome report for diagnostics, CLI, and OTA health.
 *
 * Populated at the end of system_initializer_run() regardless of outcome.
 * All fields are valid to read after that function returns.
 * Memory: Entirely static, no heap allocation.
 */
typedef struct {
    system_init_state_t  state;             /**< Final initialization state */
    boot_phase_t         failed_phase;      /**< Phase where failure occurred (BOOT_PHASE_NONE if none) */
    const char          *failed_component;  /**< Name of failing component (NULL if none) */
    esp_err_t            last_error;        /**< ESP error code of the fatal step (ESP_OK if none) */
    uint32_t             elapsed_ms;        /**< Total wall-clock boot time in milliseconds */
} system_init_report_t;

/* =========================================================================
 * Public API
 * ========================================================================= */

/**
 * @brief Executes the complete system initialization sequence.
 *
 * Runs all boot phases in verified dependency order:
 *   EARLY -> CORE -> HARDWARE -> MANAGEMENT -> CLI -> ACTIVATE -> READY
 *
 * On success: returns ESP_OK, sets state to SYSTEM_INIT_READY.
 * On fatal failure: returns the failing esp_err_t, sets state to SYSTEM_INIT_FAILED,
 *   and records the failed phase and component in the report.
 *
 * Re-entry protection: If called again after READY or FAILED, returns
 * ESP_ERR_INVALID_STATE immediately. This prevents double task creation,
 * double queue initialization, and double watchdog registration.
 *
 * @return ESP_OK on success, or the esp_err_t of the first fatal failure.
 */
esp_err_t system_initializer_run(void);

/**
 * @brief Returns the current global initialization state.
 *
 * Safe to call from any context after system_initializer_run() returns.
 *
 * @return Current system_init_state_t value.
 */
system_init_state_t system_initializer_get_state(void);

/**
 * @brief Retrieves the boot report snapshot.
 *
 * Always valid to call after system_initializer_run() returns.
 * Populated on both success and failure paths.
 *
 * @param out_report Pointer to the report buffer to fill. Must not be NULL.
 * @return ESP_OK on success, ESP_ERR_INVALID_ARG if out_report is NULL.
 */
esp_err_t system_initializer_get_report(system_init_report_t *out_report);

/**
 * @brief Returns a human-readable string for a system init state.
 * @param state The state enum value.
 * @return Static constant string (never NULL).
 */
const char *system_init_state_str(system_init_state_t state);

/**
 * @brief Returns a human-readable string for a boot phase.
 * @param phase The boot phase enum value.
 * @return Static constant string (never NULL).
 */
const char *boot_phase_str(boot_phase_t phase);

/* =========================================================================
 * Failure Injection Test Seam (Minimal)
 * ========================================================================= */

/**
 * @brief Function pointer type for boolean init functions (no-arg, returns bool).
 *
 * Used to override individual init steps in failure injection tests without
 * requiring #ifdef UNIT_TEST guards or modifying production logic paths.
 */
typedef bool (*sys_init_fn_t)(void);

/**
 * @brief Function pointer type for start functions returning esp_err_t.
 *
 * Used to override z906_transport_start() and z906_fsm_start() in failure
 * injection tests. ESP_FAIL return triggers record_fatal_failure().
 */
typedef esp_err_t (*sys_init_err_fn_t)(void);

/**
 * @brief Hook table for overriding FATAL and DEGRADED init steps in failure injection tests.
 *
 * Defaults: each field points to the real module init function.
 * Test binaries may replace individual entries via system_initializer_set_hooks()
 * before calling system_initializer_run().
 *
 * Only FATAL init functions are hookable — DEGRADED steps do not affect the
 * outcome being tested. Unset (NULL) hooks fall back to the real function.
 */
typedef struct {
    /* ── FATAL init hooks (PHASE_EARLY / CORE / HARDWARE) ─────────────────── */
    sys_init_fn_t nvs_init;          /**< Replaces nvs_manager_init()       — FATAL */
    sys_init_fn_t event_bus_init;    /**< Replaces event_bus_init()         — FATAL */
    sys_init_fn_t fsm_init;          /**< Replaces z906_fsm_init()          — FATAL */
    sys_init_fn_t gpio_init;         /**< Replaces z906_gpio_hal_init()     — FATAL */
    sys_init_fn_t uart_init;         /**< Replaces z906_uart_hal_init()     — FATAL */

    /* ── FATAL start hooks (PHASE_ACTIVATE) ───────────────────────────────── */
    sys_init_err_fn_t transport_start; /**< Replaces z906_transport_start() — FATAL */
    sys_init_err_fn_t fsm_start;       /**< Replaces z906_fsm_start()       — FATAL */
    sys_init_fn_t     zigbee_start;    /**< Replaces zigbee_native_stack_start() — FATAL */
    sys_init_fn_t     telemetry_start; /**< Replaces telemetry_start()      — FATAL */

    /* ── DEGRADED init hooks (PHASE_EARLY / CORE / HARDWARE / MGMT / CLI) ── */
    sys_init_fn_t wdt_init;            /**< Replaces wdt_manager_init()     — DEGRADED */
    sys_init_fn_t debug_console_init;  /**< Replaces debug_console_init()   — DEGRADED */
    sys_init_fn_t cmd_verifier_init;   /**< Replaces z906_cmd_verifier_init() — DEGRADED */
    sys_init_fn_t adc_init;            /**< Replaces z906_adc_hal_init()    — DEGRADED */
    sys_init_fn_t led_init;            /**< Replaces led_notification_init() — DEGRADED */
    sys_init_fn_t ota_init;            /**< Replaces ota_client_init()      — DEGRADED */
    sys_init_fn_t cli_init;            /**< Replaces cli_console_init()     — DEGRADED */
    sys_init_fn_t system_metrics_init; /**< Replaces system_metrics_init()  — DEGRADED */
    sys_init_fn_t health_manager_init; /**< Replaces health_manager_init()  — DEGRADED */

    /* ── DEGRADED start hooks (PHASE_ACTIVATE) ────────────────────────────── */
    sys_init_fn_t led_start;           /**< Replaces led_notification_start() — DEGRADED */
    sys_init_fn_t cli_start;           /**< Replaces cli_console_start()    — DEGRADED */
    sys_init_fn_t system_metrics_start;/**< Replaces system_metrics_start() — DEGRADED */
    sys_init_fn_t health_manager_start;/**< Replaces health_manager_start() — DEGRADED */
} sys_init_hooks_t;

/**
 * @brief Installs custom init-step hooks for failure injection testing.
 *
 * Must be called BEFORE system_initializer_run(). NULL entries in the hook
 * table are ignored; the corresponding real function is used instead.
 * Pass NULL to reset all hooks to the real implementations.
 *
 * @param hooks Pointer to hook table, or NULL to restore defaults.
 */
void system_initializer_set_hooks(const sys_init_hooks_t *hooks);

/**
 * @brief Resets the initializer state to NOT_STARTED for sequential test scenarios.
 *
 * Clears s_state and s_report so that system_initializer_run() can be called
 * again in the same binary. The caller is responsible for ensuring that any
 * FreeRTOS resources or module states initialized in the prior run have been
 * dealt with appropriately (typically: only use after a FAILED run that did
 * not successfully initialize any dynamic resources).
 *
 * ONLY FOR TEST BINARIES. Never call from production app_main.
 */
void system_initializer_reset_state_for_test(void);

#ifdef __cplusplus
}
#endif

#endif /* SYSTEM_INITIALIZER_H */
