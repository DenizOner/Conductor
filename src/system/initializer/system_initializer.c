/**
 * @file system_initializer.c
 * @brief System Initializer — pure boot/lifecycle orchestration authority for
 *        "Conductor - Smart Zigbee Console for Logitech z906".
 *
 * ## Architecture: Pure Orchestrator
 *
 * This file is the SINGLE orchestration authority for the Conductor boot sequence.
 * It owns EXACTLY:
 *   1. The boot phase order (compile-time encoded dependency graph).
 *   2. The failure policy per init step (FATAL vs. DEGRADED).
 *   3. The static FreeRTOS EventGroup used as a start gate for all tasks.
 *   4. The global init state and boot report.
 *
 * This file owns NOTHING ELSE:
 *   - Task body loops: owned by the respective subsystem modules.
 *   - Task static TCBs and stacks: owned by the respective subsystem modules.
 *   - Hardware register details: owned by HAL modules.
 *   - Protocol logic: owned by codec/command modules.
 *
 * ## Init vs. Activate Separation
 *
 * INIT phases (EARLY, CORE, HARDWARE, MANAGEMENT, CLI):
 *   Create static resources (queues, mutexes, driver handles), load persistent
 *   config, and register callbacks. No tasks start. No interrupt-driven data
 *   flows. All resources exist before any task can attempt to use them.
 *
 * ACTIVATE phase:
 *   Calls each module's _start() which spawns its static FreeRTOS task.
 *   Each task holds on the start_gate EventGroup, so no task enters its main
 *   loop until system_initializer_run() sets the START_GATE_BIT at the end.
 *
 *   Boot sequence after tasks are created:
 *     1. Post OTA health scheduler signal.
 *     2. Set START_GATE_BIT → all tasks enter their main loops atomically.
 *     3. Transition state to SYSTEM_INIT_READY.
 *
 *   This eliminates the race where a newly spawned task (especially uart_rx_task
 *   at Priority 5) starts processing data before post-spawn bookkeeping completes.
 *
 * ## Degraded Activation Gating
 *
 * If led_notification_init() failed → led_notification_start() is NOT called.
 * If cli_console_init() failed → cli_console_start() is NOT called.
 * If system_metrics_init() failed → system_metrics_start() is NOT called.
 * If health_manager_init() failed → health_manager_start() is NOT called.
 * This prevents task bodies from calling module APIs on uninitialized state.
 *
 * ## Failure Policy
 *
 * FATAL (SYSTEM_INIT_FAILED, boot sequence halted):
 *   - NVS init, Event Bus, FSM, GPIO, UART, Task spawn
 *
 * DEGRADED (boot continues; reduced capability):
 *   - Watchdog, Debug Console, Cmd Verifier, ADC, LED, OTA Client, CLI, System Metrics, Health Manager
 *
 * ## Zigbee Initialization Note
 *
 * zigbee_native_stack_init() is called from inside the Zigbee task body because
 * esp_zigbee_launch_mainloop() must run within its own dedicated FreeRTOS task.
 * The system initializer only calls zigbee_native_stack_start() in PHASE_ACTIVATE.
 *
 * ## OTA Health Integration
 *
 * ota_health_report_scheduler_ok() is called AFTER all tasks are created but
 * BEFORE the start_gate is released. This is the correct ordering because:
 *   - The scheduler is provably running (tasks were successfully created).
 *   - Tasks have not yet entered their loops (gate not yet set).
 *   - The report_scheduler_ok() call is non-blocking.
 */

#include "system_initializer.h"

#include <string.h>
#include <stdio.h>

#include "esp_log.h"
#include "esp_err.h"
#include "esp_timer.h"

#include "freertos/FreeRTOS.h"

/* ---- PHASE_EARLY modules ---- */
#include "nvs_manager.h"
#include "wdt_manager.h"
#include "debug_console.h"

/* ---- PHASE_CORE modules ---- */
#include "service_locator.h"
#include "event_bus.h"
#include "z906_state.h"
#include "z906_fsm.h"
#include "z906_cmd_verifier.h"

/* ---- PHASE_HARDWARE drivers (order: GPIO -> ADC -> UART) ---- */
#include "gpio_hal.h"
#include "adc_hal.h"
#include "uart_hal.h"

/* ---- z906_transport: UART-to-EventBus adapter (owns uart_rx_task) ---- */
#include "z906_transport.h"

/* ---- PHASE_MANAGEMENT services ---- */
#include "led_notification_manager.h"
#include "ota_client.h"
#include "system_metrics.h"
#include "health_manager.h"

/* ---- PHASE_CLI ---- */
#include "cli_console.h"

/* ---- PHASE_ACTIVATE: module _start() APIs ---- */
#include "config.h"
#include "zigbee_native_stack.h"
#include "telemetry.h"
#include "ota_health.h"

/** @brief Logging tag for the System Initializer module */
static const char *TAG = "SYS_INIT";

/* =========================================================================
 * Module State (private)
 * ========================================================================= */

/**
 * @brief Global init state.
 *
 * Written only by system_initializer_run() on the app_main task.
 * Transitions: NOT_STARTED → IN_PROGRESS → READY|FAILED.
 *
 * After phase_activate() returns, all tasks are already STARTED (each
 * module's _start() blocked on a private semaphore until STARTED confirmed).
 * s_state is then written to READY with no concurrent writers.
 *
 * WHY NOT volatile: volatile does not provide a memory barrier (only prevents
 * compiler register caching). The semaphore handshake inside each _start()
 * call already provides the necessary inter-task synchronization before any
 * task reads s_state.
 */
static system_init_state_t s_state = SYSTEM_INIT_NOT_STARTED;

/**
 * @brief Boot report: populated during system_initializer_run().
 *
 * Valid to read after system_initializer_run() returns regardless of outcome.
 * All fields are zero-initialized via static storage duration.
 */
static system_init_report_t s_report;

/**
 * @brief Private wrapper to adapt wdt_manager_init(timeout_s) to sys_init_fn_t.
 *
 * WHY: sys_init_fn_t is bool(*)(void). wdt_manager_init() takes a timeout_s
 * parameter. This wrapper captures the production default (10 seconds) and
 * presents the correct no-arg signature needed by CALL_HOOK.
 */
static bool prv_wdt_init_default(void) { return wdt_manager_init(10); }

/**
 * @brief Failure injection hook table (test seam).
 *
 * Defaults to real module init functions. Test binaries can override individual
 * entries via system_initializer_set_hooks() to simulate fatal init failures
 * without modifying production code or requiring hardware mocks.
 *
 * NULL entries fall back to the real function (see CALL_HOOK macro).
 */
static sys_init_hooks_t s_hooks = {
    /* Fatal init hooks */
    .nvs_init          = nvs_manager_init,
    .event_bus_init    = event_bus_init,
    .fsm_init          = z906_fsm_init,
    .gpio_init         = z906_gpio_hal_init,
    .uart_init         = z906_uart_hal_init,
    /* Fatal start hooks */
    .transport_start   = z906_transport_start,
    .fsm_start         = z906_fsm_start,
    .zigbee_start      = zigbee_native_stack_start,
    .telemetry_start   = telemetry_start,
    /* Degraded init hooks — prv_wdt_init_default bridges the timeout_s parameter */
    .wdt_init          = prv_wdt_init_default,
    .debug_console_init = debug_console_init,
    .cmd_verifier_init = z906_cmd_verifier_init,
    .adc_init          = z906_adc_hal_init,
    .led_init          = led_notification_init,
    .ota_init          = ota_client_init,
    .cli_init          = cli_console_init,
    .system_metrics_init = system_metrics_init,
    .health_manager_init = health_manager_init,
    /* Degraded start hooks */
    .led_start         = led_notification_start,
    .cli_start         = cli_console_start,
    .system_metrics_start = system_metrics_start,
    .health_manager_start = health_manager_start,
};

/** @brief Tracks whether non-fatal subsystem inits succeeded to gate task startup */
static bool s_led_init_succeeded = false;
static bool s_cli_init_succeeded = false;
static bool s_system_metrics_init_succeeded = false;
static bool s_health_manager_init_succeeded = false;

/**
 * @brief Calls a hook if set, otherwise calls the real function.
 *
 * Both `hook_fn` and `real_fn` must have signature `bool (*)(void)`.
 * This macro avoids duplicating the NULL-check at every call site.
 */
#define CALL_HOOK(hook_fn, real_fn) \
    (((hook_fn) != NULL) ? (hook_fn)() : (real_fn)())

/**
 * @brief Like CALL_HOOK but for functions that return esp_err_t.
 *
 * Used for z906_transport_start() and z906_fsm_start() whose fatal path
 * propagates the original error code (not just bool false -> ESP_FAIL).
 */
#define CALL_HOOK_ERR(hook_fn, real_fn) \
    (((hook_fn) != NULL) ? (hook_fn)() : (real_fn)())


/* =========================================================================
 * Internal Helpers
 * ========================================================================= */

/**
 * @brief Records a fatal initialization failure and transitions state to FAILED.
 *
 * WHY A HELPER: Centralizing failure recording ensures all three fields
 * (phase, component, error) are always consistently written together.
 *
 * @param phase     Boot phase where the failure occurred.
 * @param component Human-readable name of the failing module (static string).
 * @param err       ESP error code from the failing init call.
 */
static void record_fatal_failure(boot_phase_t phase,
                                 const char  *component,
                                 esp_err_t    err)
{
    s_report.failed_phase     = phase;
    s_report.failed_component = component;
    s_report.last_error       = err;
    s_report.state            = SYSTEM_INIT_FAILED;
    s_state                   = SYSTEM_INIT_FAILED;

    ESP_LOGE(TAG, "[FATAL] Phase=%s Component=%s Error=%s (0x%x)",
             boot_phase_str(phase),
             component ? component : "unknown",
             esp_err_to_name(err),
             err);
}

/* =========================================================================
 * Phase Implementations
 * ========================================================================= */

/**
 * @brief PHASE_EARLY: NVS, Watchdog, Debug Console.
 *
 * Dependency rationale:
 *   NVS first: cmd_verifier reads verif_en, LED reads led_store,
 *     Zigbee reads zb_store credentials. All happen in later phases.
 *   Watchdog second: must exist before any task calls wdt_manager_register_task().
 *     Tasks start in PHASE_ACTIVATE.
 *   Debug Console last: debug_console_print_wdt_status() queries wdt_manager.
 *     Must follow wdt_manager_init().
 *
 * @return ESP_OK, or ESP_FAIL if NVS initialization fails (FATAL).
 */
static esp_err_t phase_early(void)
{
    /* NVS: initializes default flash partition, erases and recovers if corrupt */
    if (!CALL_HOOK(s_hooks.nvs_init, nvs_manager_init)) {
        /* FATAL: Without NVS, persistent settings cannot be loaded. */
        record_fatal_failure(BOOT_PHASE_EARLY, "NVS", ESP_FAIL);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "[INIT] NVS .......... OK");

    /* Watchdog: configures TWDT with 10-second timeout (default via prv_wdt_init_default) */
    if (!CALL_HOOK(s_hooks.wdt_init, prv_wdt_init_default)) {
        /* DEGRADED: No crash/hang protection. System continues. */
        ESP_LOGW(TAG, "[INIT] Watchdog ..... DEGRADED (no hang protection)");
    } else {
        ESP_LOGI(TAG, "[INIT] Watchdog ..... OK");
    }

    /* Debug Console: initializes diagnostic service with static tag filter table */
    if (!CALL_HOOK(s_hooks.debug_console_init, debug_console_init)) {
        /* DEGRADED: ESP_LOG output still works. Diagnostic CLI commands fail. */
        ESP_LOGW(TAG, "[INIT] DebugConsole . DEGRADED (diagnostic service unavailable)");
    } else {
        ESP_LOGI(TAG, "[INIT] DebugConsole . OK");
    }

    return ESP_OK;
}

/**
 * @brief PHASE_CORE: Service Locator, Event Bus, State Manager, FSM, Cmd Verifier.
 *
 * Dependency rationale:
 *   service_locator_init(): pure table clear, no dependencies, must precede
 *     cmd_verifier registration.
 *   event_bus_init(): creates the static queue consumed by z906_fsm_task and
 *     produced by uart_rx_task. Both tasks start in PHASE_ACTIVATE. Queue must
 *     exist before that.
 *   z906_state_init(): creates the state mutex. FSM, cmd_verifier, telemetry,
 *     and Zigbee publisher all take this mutex. Must precede all of them.
 *   z906_fsm_init(): depends on event_bus (receives events) and z906_state
 *     (reads hardware state). Both must be ready first.
 *   z906_cmd_verifier_init(): reads NVS verif_en setting and creates its
 *     own mutex. NVS must be initialized (PHASE_EARLY done). Registers
 *     the service interface in ServiceLocator afterward.
 *     NOTE: verifier does NOT call UART APIs during init — only during runtime.
 *     PHASE ordering (verifier before UART) is therefore correct.
 *
 * @return ESP_OK, or error code if a fatal step fails.
 */
static esp_err_t phase_core(void)
{
    /* Service Locator: simple static array clear — cannot fail */
    service_locator_init();
    ESP_LOGI(TAG, "[INIT] ServiceLocator OK");

    /* Event Bus: creates static xQueueCreateStatic FreeRTOS queue */
    if (!CALL_HOOK(s_hooks.event_bus_init, event_bus_init)) {
        /* FATAL: UART bytes cannot reach the FSM. Serial data path broken. */
        record_fatal_failure(BOOT_PHASE_CORE, "EventBus", ESP_FAIL);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "[INIT] EventBus ..... OK");

    /* State Manager: creates static mutex, zeroes state struct */
    z906_state_init();
    /* No return value — internal assert on allocation failure (correct for static) */
    ESP_LOGI(TAG, "[INIT] StateManager . OK");

    /* FSM: initializes state machine enum to FSM_STATE_BOOTING */
    if (!CALL_HOOK(s_hooks.fsm_init, z906_fsm_init)) {
        /* FATAL: Without the FSM, no state transitions can occur. */
        record_fatal_failure(BOOT_PHASE_CORE, "FSM", ESP_FAIL);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "[INIT] FSM .......... OK");

    /* Cmd Verifier: creates static mutex + loads NVS verif_en setting.
     * Does NOT access UART during init — UART dependency is runtime-only. */
    if (!CALL_HOOK(s_hooks.cmd_verifier_init, z906_cmd_verifier_init)) {
        /* DEGRADED: Commands dispatched via UART without post-verification. */
        ESP_LOGW(TAG, "[INIT] CmdVerifier .. DEGRADED (no state verification)");
    } else {
        /* Register the verified service interface in ServiceLocator for CLI access */
        service_locator_register(SERVICE_ID_CMD_VERIFIER,
                                 (void *)z906_cmd_verifier_get_service());
        ESP_LOGI(TAG, "[INIT] CmdVerifier .. OK");
    }

    return ESP_OK;
}

/**
 * @brief PHASE_HARDWARE: GPIO, ADC, UART (in dependency order).
 *
 * Dependency rationale:
 *   GPIO first: z906_gpio_hal_init() asserts safe idle states on the DE-15
 *     Z906 serial lines BEFORE the UART controller is configured.
 *   ADC after GPIO: ADC samples cable presence via GPIO-stabilized voltage divider.
 *   UART last: must follow GPIO stabilization. uart_rx_task reads UART APIs
 *     after PHASE_ACTIVATE; UART must be ready before the first task runs.
 *
 * @return ESP_OK, or error code if a fatal step fails.
 */
static esp_err_t phase_hardware(void)
{
    /* GPIO: configures DE-15 pins and asserts safe idle line states */
    if (!CALL_HOOK(s_hooks.gpio_init, z906_gpio_hal_init)) {
        /* FATAL: Undefined serial line states. */
        record_fatal_failure(BOOT_PHASE_HARDWARE, "GPIO", ESP_FAIL);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "[INIT] GPIO ......... OK");

    /* ADC: initializes ADC1 one-shot mode for cable/voltage sensing */
    if (!CALL_HOOK(s_hooks.adc_init, z906_adc_hal_init)) {
        /* DEGRADED: No cable/voltage sensing via ADC. */
        ESP_LOGW(TAG, "[INIT] ADC .......... DEGRADED (no ADC cable/voltage sensing)");
    } else {
        ESP_LOGI(TAG, "[INIT] ADC .......... OK");
    }

    /* UART: initializes both channels at 57600 baud, 8O1 parity */
    if (!CALL_HOOK(s_hooks.uart_init, z906_uart_hal_init)) {
        /* FATAL: No serial communication with Z906. */
        record_fatal_failure(BOOT_PHASE_HARDWARE, "UART", ESP_FAIL);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "[INIT] UART ......... OK");

    return ESP_OK;
}

/**
 * @brief PHASE_MANAGEMENT: LED Notification, OTA Client (+ OTA Health init), System Metrics, Health Manager.
 *
 * Dependency rationale:
 *   LED Notification: reads led_store NVS namespace. NVS must be initialized.
 *   OTA Client: ota_client_init() internally calls ota_health_init() which
 *     must happen early so the 15-second health window accumulates properly.
 *   System Metrics: initializes hardware temp sensor driver and baseline metrics snapshot.
 *   Health Manager: initializes central health status state machine.
 *
 * @return ESP_OK always (all steps are DEGRADED on failure).
 */
static esp_err_t phase_management(void)
{
    /* LED Notification: loads persistent config from NVS led_store namespace */
    if (!CALL_HOOK(s_hooks.led_init, led_notification_init)) {
        s_led_init_succeeded = false;
        /* DEGRADED: No LED feedback. Z906 audio control unaffected. */
        ESP_LOGW(TAG, "[INIT] LED .......... DEGRADED (no LED feedback)");
    } else {
        s_led_init_succeeded = true;
        ESP_LOGI(TAG, "[INIT] LED .......... OK");
    }

    /* OTA Client: also calls ota_health_init() for A/B partition detection */
    if (!CALL_HOOK(s_hooks.ota_init, ota_client_init)) {
        /* DEGRADED: No OTA firmware update capability. */
        ESP_LOGW(TAG, "[INIT] OTAClient .... DEGRADED (no OTA update capability)");
    } else {
        ESP_LOGI(TAG, "[INIT] OTAClient .... OK");
    }

    /* System Metrics: measures hardware temperature and RTOS metrics */
    if (!CALL_HOOK(s_hooks.system_metrics_init, system_metrics_init)) {
        s_system_metrics_init_succeeded = false;
        /* DEGRADED: Hardware and RTOS telemetry measurement unavailable. */
        ESP_LOGW(TAG, "[INIT] SysMetrics ... DEGRADED (metrics measurement unavailable)");
    } else {
        s_system_metrics_init_succeeded = true;
        ESP_LOGI(TAG, "[INIT] SysMetrics ... OK");
    }

    /* Health Manager: interprets and aggregates subsystem and metrics health */
    if (!CALL_HOOK(s_hooks.health_manager_init, health_manager_init)) {
        s_health_manager_init_succeeded = false;
        /* DEGRADED: System health classification unavailable. */
        ESP_LOGW(TAG, "[INIT] HealthMgr .... DEGRADED (health classification unavailable)");
    } else {
        s_health_manager_init_succeeded = true;
        ESP_LOGI(TAG, "[INIT] HealthMgr .... OK");
    }

    return ESP_OK;
}

/**
 * @brief PHASE_CLI: CLI Console initialization.
 *
 * WHY AFTER CORE: cli_console_init() may look up the cmd_verifier service
 *   via ServiceLocator. The verifier must be registered first (PHASE_CORE done).
 *
 * WHY BEFORE ACTIVATE: cli_console_init() registers command handler functions.
 *   If cli_task starts before this completes, cli_console_process_line() may
 *   receive commands without the handler table being populated.
 *
 * @return ESP_OK always (CLI failure is DEGRADED).
 */
static esp_err_t phase_cli(void)
{
    if (!CALL_HOOK(s_hooks.cli_init, cli_console_init)) {
        s_cli_init_succeeded = false;
        /* DEGRADED: No interactive USB serial console. Zigbee control still works. */
        ESP_LOGW(TAG, "[INIT] CLI .......... DEGRADED (no interactive console)");
    } else {
        s_cli_init_succeeded = true;
        ESP_LOGI(TAG, "[INIT] CLI .......... OK");
    }

    return ESP_OK;
}

/**
 * @brief PHASE_ACTIVATE: Starts static FreeRTOS runtime tasks via module _start() APIs.
 *
 * WHY LAST: Every task spawned here immediately accesses resources that must
 * already be fully initialized by earlier phases.
 *
 * STARTUP HANDSHAKE (FSR-01/02 fix):
 *   Each module's _start() API uses an internal StaticSemaphore_t handshake.
 *   It returns only after the task signals it entered its main loop (STARTED).
 *   There is NO shared EventGroupHandle_t passed to any module.
 *
 * DEGRADED GATING:
 *   led_start, cli_start, system_metrics_start, health_manager_start are gated
 *   on their respective init success flags. If init failed, start is not called
 *   (call count = 0), preventing dangling tasks.
 *
 * FATAL TASKS: z906_transport, z906_fsm, zigbee, telemetry.
 *   If any of these cannot start, the system cannot operate.
 *
 * NON-FATAL TASKS: led, cli, system_metrics, health_manager.
 *   Failure is DEGRADED — logged but boot continues.
 *
 * @return ESP_OK on success, ESP_FAIL if any FATAL task start fails.
 */
static esp_err_t phase_activate(void)
{
    /* z906_transport: Priority 5 — drains UART FIFOs into EventBus.
     * Blocks until the task signals STARTED (private semaphore handshake). */
    esp_err_t err = CALL_HOOK_ERR(s_hooks.transport_start, z906_transport_start);
    if (err != ESP_OK) {
        record_fatal_failure(BOOT_PHASE_ACTIVATE, "uart_rx_task", err);
        return ESP_FAIL;
    }

    /* z906_fsm: Priority 4 — processes events and drives state machine.
     * Blocks until the task signals STARTED (private semaphore handshake). */
    err = CALL_HOOK_ERR(s_hooks.fsm_start, z906_fsm_start);
    if (err != ESP_OK) {
        record_fatal_failure(BOOT_PHASE_ACTIVATE, "z906_fsm_task", err);
        return ESP_FAIL;
    }

    /* zigbee_task: Priority 3 — runs blocking ESP Zigbee SDK mainloop.
     * Blocks until zigbee_native_stack_init() signals success (FSR-04 fix).
     * Returns false if init fails or times out after ZB_STARTUP_TIMEOUT_MS. */
    if (!CALL_HOOK(s_hooks.zigbee_start, zigbee_native_stack_start)) {
        record_fatal_failure(BOOT_PHASE_ACTIVATE, "zigbee_task", ESP_FAIL);
        return ESP_FAIL;
    }

    /* telemetry_task: Priority 1 — periodic ADC and Z906 telemetry polling.
     * Blocks until the task signals STARTED (private semaphore handshake). */
    if (!CALL_HOOK(s_hooks.telemetry_start, telemetry_start)) {
        record_fatal_failure(BOOT_PHASE_ACTIVATE, "telemetry_task", ESP_FAIL);
        return ESP_FAIL;
    }

    /* led_task: Priority 1 — DEGRADED if led_notification_init() failed.
     * Gated by s_led_init_succeeded: if init failed, led_start is NOT called. */
    if (s_led_init_succeeded) {
        if (!CALL_HOOK(s_hooks.led_start, led_notification_start)) {
            ESP_LOGW(TAG, "[INIT] led_task ..... DEGRADED (LED start failed)");
        }
    } else {
        ESP_LOGW(TAG, "[INIT] led_task ..... GATED (LED init failed, start bypassed)");
    }

    /* cli_task: Priority 2 — DEGRADED if cli_console_init() failed.
     * Gated by s_cli_init_succeeded: if init failed, cli_start is NOT called. */
    if (s_cli_init_succeeded) {
        if (!CALL_HOOK(s_hooks.cli_start, cli_console_start)) {
            ESP_LOGW(TAG, "[INIT] cli_task ..... DEGRADED (CLI start failed)");
        }
    } else {
        ESP_LOGW(TAG, "[INIT] cli_task ..... GATED (CLI init failed, start bypassed)");
    }

    /* system_metrics_task: Priority 1 — DEGRADED if system_metrics_init() failed.
     * Gated by s_system_metrics_init_succeeded: if init failed, start is NOT called. */
    if (s_system_metrics_init_succeeded) {
        if (!CALL_HOOK(s_hooks.system_metrics_start, system_metrics_start)) {
            ESP_LOGW(TAG, "[INIT] sys_metrics .. DEGRADED (Metrics start failed)");
        }
    } else {
        ESP_LOGW(TAG, "[INIT] sys_metrics .. GATED (Metrics init failed, start bypassed)");
    }

    /* health_manager: Event/sample driven — DEGRADED if health_manager_init() failed.
     * Gated by s_health_manager_init_succeeded: if init failed, start is NOT called. */
    if (s_health_manager_init_succeeded) {
        if (!CALL_HOOK(s_hooks.health_manager_start, health_manager_start)) {
            ESP_LOGW(TAG, "[INIT] health_mgr ... DEGRADED (Health manager start failed)");
        }
    } else {
        ESP_LOGW(TAG, "[INIT] health_mgr ... GATED (Health manager init failed, start bypassed)");
    }

    ESP_LOGI(TAG, "[INIT] Tasks ........ all STARTED");
    return ESP_OK;
}


/* =========================================================================
 * Public API Implementation
 * ========================================================================= */

esp_err_t system_initializer_run(void)
{
    /* Re-entry guard: prevents duplicate task creation on accidental second call */
    if (s_state != SYSTEM_INIT_NOT_STARTED) {
        ESP_LOGW(TAG, "system_initializer_run() called again (state=%s) — ignored",
                 system_init_state_str(s_state));
        return ESP_ERR_INVALID_STATE;
    }

    /* Transition to IN_PROGRESS before any work begins */
    s_state = SYSTEM_INIT_IN_PROGRESS;
    s_report.state = SYSTEM_INIT_IN_PROGRESS;

    /* Record start time using esp_timer for microsecond-accurate wall-clock timing */
    int64_t start_us = esp_timer_get_time();

    ESP_LOGI(TAG, "=== System Initializer: boot sequence starting ===");
    ESP_LOGI(TAG, "Firmware: %s", Z906_FW_VERSION_STR);

    /* Phase execution — each phase returns immediately on fatal failure */
    ESP_LOGI(TAG, "--- Phase: EARLY ---");
    esp_err_t err = phase_early();
    if (err != ESP_OK) {
        goto boot_failed;
    }

    ESP_LOGI(TAG, "--- Phase: CORE ---");
    err = phase_core();
    if (err != ESP_OK) {
        goto boot_failed;
    }

    ESP_LOGI(TAG, "--- Phase: HARDWARE ---");
    err = phase_hardware();
    if (err != ESP_OK) {
        goto boot_failed;
    }

    ESP_LOGI(TAG, "--- Phase: MANAGEMENT ---");
    (void)phase_management(); /* No fatal path; DEGRADED outcomes logged inside */

    ESP_LOGI(TAG, "--- Phase: CLI ---");
    (void)phase_cli();        /* No fatal path; DEGRADED outcomes logged inside */

    ESP_LOGI(TAG, "--- Phase: ACTIVATE ---");
    err = phase_activate();
    if (err != ESP_OK) {
        goto boot_failed;
    }

    /* All phases complete. All tasks are already STARTED (each start() API blocks
     * on a private semaphore until the task enters its main loop).
     *
     * Sequence:
     *   1. Signal OTA health that the scheduler is running and tasks are alive.
     *   2. Update Initializer health component status in Health Manager.
     *   3. Compute elapsed time and populate the success report.
     *   4. Transition state to READY.
     */
    {
        /* Step 1: OTA health — scheduler criterion (tasks are STARTED and running) */
        ota_health_report_scheduler_ok();

        /* Step 2: Health Manager — report Initializer completed healthy */
        health_manager_set_component_health(HEALTH_COMP_INITIALIZER,
                                            HEALTH_STATUS_HEALTHY,
                                            HEALTH_REASON_NONE);

        /* Step 3: Populate success report */
        int64_t  end_us      = esp_timer_get_time();
        uint32_t elapsed_ms  = (uint32_t)((end_us - start_us) / 1000ULL);

        s_report.state            = SYSTEM_INIT_READY;
        s_report.failed_phase     = BOOT_PHASE_NONE;
        s_report.failed_component = NULL;
        s_report.last_error       = ESP_OK;
        s_report.elapsed_ms       = elapsed_ms;

        /* Step 4: Transition state to READY */
        s_state = SYSTEM_INIT_READY;

        ESP_LOGI(TAG, "=================================================");
        ESP_LOGI(TAG, "[INIT] System ....... READY (%lu ms)",
                 (unsigned long)elapsed_ms);
        ESP_LOGI(TAG, "=================================================");

        return ESP_OK;
    }

boot_failed:
    {
        /* Record elapsed time even on failure for diagnostic usefulness */
        int64_t  end_us     = esp_timer_get_time();
        s_report.elapsed_ms = (uint32_t)((end_us - start_us) / 1000ULL);

        /* Failure details already written by record_fatal_failure() */
        ESP_LOGE(TAG, "=================================================");
        ESP_LOGE(TAG, "[INIT] System ....... FAILED at phase=%s component=%s (%lu ms)",
                 boot_phase_str(s_report.failed_phase),
                 s_report.failed_component ? s_report.failed_component : "unknown",
                 (unsigned long)s_report.elapsed_ms);
        ESP_LOGE(TAG, "err=%s (0x%x)", esp_err_to_name(s_report.last_error),
                 s_report.last_error);
        ESP_LOGE(TAG, "=================================================");

        /* Health Manager — report Initializer fatal failure */
        health_manager_set_component_health(HEALTH_COMP_INITIALIZER,
                                            HEALTH_STATUS_FAULT,
                                            HEALTH_REASON_INITIALIZER_FAILED);

        /* NOTE: We do NOT restart here. app_main() decides the reboot policy.
         * An automatic restart here could create a boot loop if a hardware
         * failure (e.g., NVS corruption) persists across reboots. */
        return s_report.last_error;
    }
}

system_init_state_t system_initializer_get_state(void)
{
    /* Safe to read after system_initializer_run() returns because:
     * - If READY:  phase_activate() confirmed all tasks STARTED via private
     *   semaphore handshakes before s_state was written. No concurrent writer.
     * - If FAILED: no tasks were spawned beyond the failure point.
     * - During IN_PROGRESS: only app_main reads this; no other tasks exist yet. */
    return s_state;
}

esp_err_t system_initializer_get_report(system_init_report_t *out_report)
{
    if (out_report == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Struct copy without mutex:
     * - During boot: only app_main writes; no reader exists yet.
     * - After boot: report is immutable (no writer remains). */
    *out_report = s_report;
    return ESP_OK;
}

const char *system_init_state_str(system_init_state_t state)
{
    switch (state) {
        case SYSTEM_INIT_NOT_STARTED: return "NOT_STARTED";
        case SYSTEM_INIT_IN_PROGRESS: return "IN_PROGRESS";
        case SYSTEM_INIT_READY:       return "READY";
        case SYSTEM_INIT_FAILED:      return "FAILED";
        default:                      return "UNKNOWN";
    }
}

const char *boot_phase_str(boot_phase_t phase)
{
    switch (phase) {
        case BOOT_PHASE_NONE:       return "NONE";
        case BOOT_PHASE_EARLY:      return "EARLY";
        case BOOT_PHASE_CORE:       return "CORE";
        case BOOT_PHASE_HARDWARE:   return "HARDWARE";
        case BOOT_PHASE_MANAGEMENT: return "MANAGEMENT";
        case BOOT_PHASE_CLI:        return "CLI";
        case BOOT_PHASE_ACTIVATE:   return "ACTIVATE";
        case BOOT_PHASE_READY:      return "READY";
        default:                    return "UNKNOWN";
    }
}

void system_initializer_set_hooks(const sys_init_hooks_t *hooks)
{
    /* NULL resets ALL hooks to the real module implementations. */
    if (hooks == NULL) {
        /* Fatal init hooks */
        s_hooks.nvs_init          = nvs_manager_init;
        s_hooks.event_bus_init    = event_bus_init;
        s_hooks.fsm_init          = z906_fsm_init;
        s_hooks.gpio_init         = z906_gpio_hal_init;
        s_hooks.uart_init         = z906_uart_hal_init;
        /* Fatal start hooks */
        s_hooks.transport_start   = z906_transport_start;
        s_hooks.fsm_start         = z906_fsm_start;
        s_hooks.zigbee_start      = zigbee_native_stack_start;
        s_hooks.telemetry_start   = telemetry_start;
        /* Degraded init hooks */
        s_hooks.wdt_init          = prv_wdt_init_default;
        s_hooks.debug_console_init = debug_console_init;
        s_hooks.cmd_verifier_init = z906_cmd_verifier_init;
        s_hooks.adc_init          = z906_adc_hal_init;
        s_hooks.led_init          = led_notification_init;
        s_hooks.ota_init          = ota_client_init;
        s_hooks.cli_init          = cli_console_init;
        s_hooks.system_metrics_init = system_metrics_init;
        s_hooks.health_manager_init = health_manager_init;
        /* Degraded start hooks */
        s_hooks.led_start         = led_notification_start;
        s_hooks.cli_start         = cli_console_start;
        s_hooks.system_metrics_start = system_metrics_start;
        s_hooks.health_manager_start = health_manager_start;
        return;
    }

    /* Apply hook overrides: NULL entries in the struct mean "keep current function",
     * so we only replace if the caller provided a non-NULL pointer.
     * WHY: This allows callers to set only the hooks they care about without
     * having to fill in every field. */

    /* Fatal init hooks */
    if (hooks->nvs_init          != NULL) { s_hooks.nvs_init          = hooks->nvs_init; }
    if (hooks->event_bus_init    != NULL) { s_hooks.event_bus_init    = hooks->event_bus_init; }
    if (hooks->fsm_init          != NULL) { s_hooks.fsm_init          = hooks->fsm_init; }
    if (hooks->gpio_init         != NULL) { s_hooks.gpio_init         = hooks->gpio_init; }
    if (hooks->uart_init         != NULL) { s_hooks.uart_init         = hooks->uart_init; }
    /* Fatal start hooks */
    if (hooks->transport_start   != NULL) { s_hooks.transport_start   = hooks->transport_start; }
    if (hooks->fsm_start         != NULL) { s_hooks.fsm_start         = hooks->fsm_start; }
    if (hooks->zigbee_start      != NULL) { s_hooks.zigbee_start      = hooks->zigbee_start; }
    if (hooks->telemetry_start   != NULL) { s_hooks.telemetry_start   = hooks->telemetry_start; }
    /* Degraded init hooks */
    if (hooks->wdt_init          != NULL) { s_hooks.wdt_init          = hooks->wdt_init; }
    if (hooks->debug_console_init != NULL){ s_hooks.debug_console_init = hooks->debug_console_init; }
    if (hooks->cmd_verifier_init != NULL) { s_hooks.cmd_verifier_init = hooks->cmd_verifier_init; }
    if (hooks->adc_init          != NULL) { s_hooks.adc_init          = hooks->adc_init; }
    if (hooks->led_init          != NULL) { s_hooks.led_init          = hooks->led_init; }
    if (hooks->ota_init          != NULL) { s_hooks.ota_init          = hooks->ota_init; }
    if (hooks->cli_init          != NULL) { s_hooks.cli_init          = hooks->cli_init; }
    if (hooks->system_metrics_init != NULL) { s_hooks.system_metrics_init = hooks->system_metrics_init; }
    if (hooks->health_manager_init != NULL) { s_hooks.health_manager_init = hooks->health_manager_init; }
    /* Degraded start hooks */
    if (hooks->led_start         != NULL) { s_hooks.led_start         = hooks->led_start; }
    if (hooks->cli_start         != NULL) { s_hooks.cli_start         = hooks->cli_start; }
    if (hooks->system_metrics_start != NULL) { s_hooks.system_metrics_start = hooks->system_metrics_start; }
    if (hooks->health_manager_start != NULL) { s_hooks.health_manager_start = hooks->health_manager_start; }
}

void system_initializer_reset_state_for_test(void)
{
    /* Reset ONLY the initializer's own state machine — does NOT re-initialize
     * any module resources. Safe to call after a FAILED run that did not
     * successfully create FreeRTOS tasks, queues, or hardware handles. */
    s_state = SYSTEM_INIT_NOT_STARTED;
    memset(&s_report, 0, sizeof(s_report));
    s_led_init_succeeded = false;
    s_cli_init_succeeded = false;
    s_system_metrics_init_succeeded = false;
    s_health_manager_init_succeeded = false;
    /* Restore default hooks so subsequent run uses real functions unless overridden */
    system_initializer_set_hooks(NULL);
    ESP_LOGW(TAG, "[TEST] Initializer state reset for failure injection test");
}
