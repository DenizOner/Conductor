/**
 * @file health_manager.c
 * @brief Implementation of the Health Manager module for Conductor.
 *
 * Responsibilities:
 *   - Continuous, autonomous monitoring and health interpretation.
 *   - Evaluates raw metrics from System Metrics against centralized health policies.
 *   - Autonomous staleness evaluation via lightweight periodic esp_timer (2000ms).
 *   - Classifies health: UNKNOWN, HEALTHY, DEGRADED, FAULT.
 *   - Hysteresis on temperature and sustained sample windows on CPU load.
 *   - Decoupled current memory health from historical minimum free watermark.
 *   - Change-only diagnostic logging (zero periodic log spam).
 *   - Strictly non-intrusive: Zero hardware calls, zero task restarts, zero rebooting.
 *   - Dispatches health transitions to registered neutral observers (no reverse dependencies).
 */

#include "health_manager.h"
#include "health_policy.h"
#include <string.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "health_mgr";

/** @brief Maximum mutex acquisition timeout */
#define HEALTH_MGR_MUTEX_TIMEOUT_TICKS pdMS_TO_TICKS(50)

/** @brief Autonomous staleness evaluation interval (2000 ms) */
#define HEALTH_MGR_STALENESS_TIMER_US  (2000 * 1000)

/* =========================================================================
 * Static Storage Allocations (Zero Dynamic Allocation at Runtime)
 * ========================================================================= */

/** @brief Mutex protecting the central health snapshot */
static StaticSemaphore_t s_mutex_buffer;
static SemaphoreHandle_t s_mutex = NULL;

/** @brief Authoritative system health snapshot cache */
static health_snapshot_t s_health_snapshot;

/** @brief Module initialization and activation flags */
static bool s_initialized = false;
static bool s_active = false;

/** @brief Periodic timer handle for autonomous staleness evaluation */
static esp_timer_handle_t s_staleness_timer = NULL;

/* Registered health change transition observer */
static health_manager_change_cb_t s_change_observer = NULL;
static void                      *s_change_observer_ctx = NULL;

/* =========================================================================
 * Hysteresis and Sustained Sample Counters (Internal Filter State)
 * ========================================================================= */

static uint32_t s_cpu_sustained_warn_count = 0;
static uint32_t s_cpu_sustained_crit_count = 0;
static bool     s_temp_warn_active = false;
static bool     s_temp_crit_active = false;
static uint64_t s_last_metrics_rx_time_ms = 0;

/* Tracks previously logged state to guarantee strict change-only logging */
static health_status_t    s_last_logged_status = HEALTH_STATUS_UNKNOWN;
static health_component_t s_last_logged_primary_comp = HEALTH_COMP_SYSTEM;
static health_reason_t    s_last_logged_primary_reas = HEALTH_REASON_NOT_INITIALIZED;
static bool               s_first_log_emitted = false;

/* =========================================================================
 * Private Helper Functions
 * ========================================================================= */

/**
 * @brief Recomputes overall system health and identifies primary failing component.
 *
 * Evaluation rules (Section 10):
 *   1. Critical FAULT exists -> overall FAULT
 *   2. No FAULT + any DEGRADED -> overall DEGRADED
 *   3. No FAULT + no DEGRADED, but any required component UNKNOWN -> overall UNKNOWN
 *   4. All required evaluated and HEALTHY -> overall HEALTHY
 *
 * MUST be called while holding s_mutex.
 */
static void prv_recompute_overall_health_locked(uint64_t now_ms)
{
    health_status_t highest_status = HEALTH_STATUS_HEALTHY;
    health_component_t primary_comp = HEALTH_COMP_SYSTEM;
    health_reason_t primary_reas = HEALTH_REASON_NONE;
    bool has_unknown = false;
    health_component_t first_unknown_comp = HEALTH_COMP_SYSTEM;
    health_reason_t first_unknown_reas = HEALTH_REASON_NOT_INITIALIZED;

    /* Prioritize severity: FAULT > DEGRADED */
    for (int i = 1; i < HEALTH_COMP_COUNT; i++) {
        health_status_t st = s_health_snapshot.components[i].status;
        health_reason_t re = s_health_snapshot.components[i].reason;

        if (st == HEALTH_STATUS_FAULT) {
            highest_status = HEALTH_STATUS_FAULT;
            primary_comp = (health_component_t)i;
            primary_reas = re;
            break; /* FAULT is highest possible severity */
        } else if (st == HEALTH_STATUS_DEGRADED) {
            if (highest_status < HEALTH_STATUS_DEGRADED) {
                highest_status = HEALTH_STATUS_DEGRADED;
                primary_comp = (health_component_t)i;
                primary_reas = re;
            }
        } else if (st == HEALTH_STATUS_UNKNOWN) {
            if (!has_unknown) {
                has_unknown = true;
                first_unknown_comp = (health_component_t)i;
                first_unknown_reas = re;
            }
        }
    }

    /* If no FAULT or DEGRADED, check if required baseline components are UNKNOWN */
    if (highest_status < HEALTH_STATUS_DEGRADED && has_unknown) {
        /* Core required baseline components: Metrics, Temperature, CPU, Memory, Initializer */
        if (s_health_snapshot.components[HEALTH_COMP_SYSTEM_METRICS].status == HEALTH_STATUS_UNKNOWN ||
            s_health_snapshot.components[HEALTH_COMP_SOC_TEMPERATURE].status == HEALTH_STATUS_UNKNOWN ||
            s_health_snapshot.components[HEALTH_COMP_CPU].status == HEALTH_STATUS_UNKNOWN ||
            s_health_snapshot.components[HEALTH_COMP_MEMORY].status == HEALTH_STATUS_UNKNOWN ||
            s_health_snapshot.components[HEALTH_COMP_INITIALIZER].status == HEALTH_STATUS_UNKNOWN) {
            highest_status = HEALTH_STATUS_UNKNOWN;
            primary_comp = first_unknown_comp;
            primary_reas = first_unknown_reas;
        }
    }

    health_status_t prev_overall = s_health_snapshot.overall_status;

    s_health_snapshot.overall_status = highest_status;
    s_health_snapshot.primary_component = primary_comp;
    s_health_snapshot.primary_reason = primary_reas;
    s_health_snapshot.components[HEALTH_COMP_SYSTEM].status = highest_status;
    s_health_snapshot.components[HEALTH_COMP_SYSTEM].reason = primary_reas;
    s_health_snapshot.components[HEALTH_COMP_SYSTEM].last_update_ms = now_ms;
    s_health_snapshot.snapshot_timestamp_ms = now_ms;

    if (highest_status != prev_overall) {
        s_health_snapshot.status_since_ms = now_ms;
    }

    /* Change-Only Diagnostic Logging & Notification: Trigger ONLY on state/reason transition */
    if (!s_first_log_emitted ||
        highest_status != s_last_logged_status ||
        primary_comp != s_last_logged_primary_comp ||
        primary_reas != s_last_logged_primary_reas) {

        s_first_log_emitted = true;
        s_last_logged_status = highest_status;
        s_last_logged_primary_comp = primary_comp;
        s_last_logged_primary_reas = primary_reas;

        if (highest_status == HEALTH_STATUS_FAULT) {
            ESP_LOGE(TAG, "[HEALTH TRANSITION] %s -> FAULT (Primary: %s, Reason: %s)",
                     health_status_to_str(prev_overall),
                     health_component_to_str(primary_comp),
                     health_reason_to_str(primary_reas));
        } else if (highest_status == HEALTH_STATUS_DEGRADED) {
            ESP_LOGW(TAG, "[HEALTH TRANSITION] %s -> DEGRADED (Primary: %s, Reason: %s)",
                     health_status_to_str(prev_overall),
                     health_component_to_str(primary_comp),
                     health_reason_to_str(primary_reas));
        } else if (highest_status == HEALTH_STATUS_HEALTHY) {
            ESP_LOGI(TAG, "[HEALTH TRANSITION] %s -> HEALTHY (System fully nominal)",
                     health_status_to_str(prev_overall));
        } else {
            ESP_LOGI(TAG, "[HEALTH TRANSITION] %s -> UNKNOWN (Primary: %s, Reason: %s)",
                     health_status_to_str(prev_overall),
                     health_component_to_str(primary_comp),
                     health_reason_to_str(primary_reas));
        }

        /* Dispatch to neutral transition observer if registered */
        if (s_change_observer != NULL) {
            s_change_observer(&s_health_snapshot, s_change_observer_ctx);
        }
    }
}

/**
 * @brief Evaluates SoC die temperature with hysteresis.
 */
static void prv_evaluate_temperature_locked(const system_metrics_snapshot_t *m, uint64_t now_ms)
{
    health_component_state_t *state = &s_health_snapshot.components[HEALTH_COMP_SOC_TEMPERATURE];
    state->last_update_ms = now_ms;

    if (!m->soc_temperature_valid) {
        if (s_last_metrics_rx_time_ms == 0) {
            state->status = HEALTH_STATUS_UNKNOWN;
            state->reason = HEALTH_REASON_METRIC_NOT_READY;
        } else {
            state->status = HEALTH_STATUS_DEGRADED;
            state->reason = HEALTH_REASON_SOC_TEMP_SENSOR_ERROR;
        }
        s_temp_warn_active = false;
        s_temp_crit_active = false;
        return;
    }

    float t = m->soc_temperature_c;

    /* Critical threshold evaluation with hysteresis */
    if (s_temp_crit_active) {
        if (t < HEALTH_POLICY_SOC_TEMP_CRIT_EXIT_C) {
            s_temp_crit_active = false;
        }
    } else {
        if (t >= HEALTH_POLICY_SOC_TEMP_CRIT_ENTER_C) {
            s_temp_crit_active = true;
        }
    }

    if (s_temp_crit_active) {
        state->status = HEALTH_STATUS_FAULT;
        state->reason = HEALTH_REASON_SOC_TEMP_CRITICAL;
        return;
    }

    /* Warning threshold evaluation with hysteresis */
    if (s_temp_warn_active) {
        if (t < HEALTH_POLICY_SOC_TEMP_WARN_EXIT_C) {
            s_temp_warn_active = false;
        }
    } else {
        if (t >= HEALTH_POLICY_SOC_TEMP_WARN_ENTER_C) {
            s_temp_warn_active = true;
        }
    }

    if (s_temp_warn_active) {
        state->status = HEALTH_STATUS_DEGRADED;
        state->reason = HEALTH_REASON_SOC_TEMP_WARNING;
        return;
    }

    /* Nominal temperature */
    state->status = HEALTH_STATUS_HEALTHY;
    state->reason = HEALTH_REASON_NONE;
}

/**
 * @brief Evaluates CPU load with sustained sample filtering.
 */
static void prv_evaluate_cpu_locked(const system_metrics_snapshot_t *m, uint64_t now_ms)
{
    health_component_state_t *state = &s_health_snapshot.components[HEALTH_COMP_CPU];
    state->last_update_ms = now_ms;

    if (!m->cpu_load_valid) {
        if (s_last_metrics_rx_time_ms == 0) {
            state->status = HEALTH_STATUS_UNKNOWN;
            state->reason = HEALTH_REASON_METRIC_NOT_READY;
        } else {
            state->status = HEALTH_STATUS_UNKNOWN;
            state->reason = HEALTH_REASON_METRIC_NOT_READY;
        }
        s_cpu_sustained_warn_count = 0;
        s_cpu_sustained_crit_count = 0;
        return;
    }

    float load = m->cpu_load_percent;

    /* Critical CPU load evaluation (>= 95% sustained for 10 samples) */
    if (load >= HEALTH_POLICY_CPU_LOAD_CRIT_ENTER_PCT) {
        s_cpu_sustained_crit_count++;
    } else {
        s_cpu_sustained_crit_count = 0;
    }

    if (s_cpu_sustained_crit_count >= HEALTH_POLICY_CPU_SUSTAINED_CRIT_SAMPLES) {
        state->status = HEALTH_STATUS_FAULT;
        state->reason = HEALTH_REASON_CPU_LOAD_CRITICAL;
        return;
    }

    /* Warning CPU load evaluation with hysteresis */
    if (load >= HEALTH_POLICY_CPU_LOAD_WARN_ENTER_PCT) {
        s_cpu_sustained_warn_count++;
    } else if (load < HEALTH_POLICY_CPU_LOAD_WARN_EXIT_PCT) {
        s_cpu_sustained_warn_count = 0;
    }

    if (s_cpu_sustained_warn_count >= HEALTH_POLICY_CPU_SUSTAINED_WARN_SAMPLES) {
        state->status = HEALTH_STATUS_DEGRADED;
        state->reason = HEALTH_REASON_CPU_LOAD_SUSTAINED_HIGH;
    } else {
        state->status = HEALTH_STATUS_HEALTHY;
        state->reason = HEALTH_REASON_NONE;
    }
}

/**
 * @brief Evaluates current internal heap capacity and fragmentation.
 *
 * Disentangles current health from historical minimum free watermark (Section 12):
 *   Current health is derived solely from live heap usage %, current free bytes,
 *   and largest contiguous free block.
 */
static void prv_evaluate_memory_locked(const system_metrics_snapshot_t *m, uint64_t now_ms)
{
    health_component_state_t *state = &s_health_snapshot.components[HEALTH_COMP_MEMORY];
    state->last_update_ms = now_ms;

    if (m->heap_total_bytes == 0) {
        state->status = HEALTH_STATUS_UNKNOWN;
        state->reason = HEALTH_REASON_METRIC_NOT_READY;
        return;
    }

    /* 1. Critical current memory exhaustion danger: usage >= 95% OR free < 16 KB */
    if (m->heap_usage_percent >= HEALTH_POLICY_HEAP_CRIT_USAGE_PCT ||
        m->heap_free_bytes < HEALTH_POLICY_HEAP_CRIT_CURRENT_FREE_BYTES) {
        state->status = HEALTH_STATUS_FAULT;
        state->reason = HEALTH_REASON_HEAP_CRITICAL;
        return;
    }

    /* 2. Warning current memory headroom: usage >= 85% OR free < 32 KB */
    if (m->heap_usage_percent >= HEALTH_POLICY_HEAP_WARN_USAGE_PCT ||
        m->heap_free_bytes < HEALTH_POLICY_HEAP_WARN_CURRENT_FREE_BYTES) {
        state->status = HEALTH_STATUS_DEGRADED;
        state->reason = HEALTH_REASON_HEAP_WARNING;
        return;
    }

    /* 3. Warning internal heap fragmentation: largest block < 8 KB */
    if (m->heap_largest_free_block_bytes < HEALTH_POLICY_HEAP_WARN_LARGEST_BLOCK) {
        state->status = HEALTH_STATUS_DEGRADED;
        state->reason = HEALTH_REASON_HEAP_FRAGMENTED;
        return;
    }

    /* 4. Current memory health is nominal */
    state->status = HEALTH_STATUS_HEALTHY;
    state->reason = HEALTH_REASON_NONE;
}

/**
 * @brief Evaluates reset cause for watchdog / panic historical traces.
 */
static void prv_evaluate_reset_diagnostics_locked(const system_metrics_snapshot_t *m, uint64_t now_ms)
{
    health_component_state_t *wdt_state = &s_health_snapshot.components[HEALTH_COMP_WATCHDOG];
    wdt_state->last_update_ms = now_ms;

    if (m->reset_reason == SYSTEM_RESET_REASON_WATCHDOG) {
        wdt_state->status = HEALTH_STATUS_DEGRADED;
        wdt_state->reason = HEALTH_REASON_WATCHDOG_RESET_DETECTED;
    } else {
        wdt_state->status = HEALTH_STATUS_HEALTHY;
        wdt_state->reason = HEALTH_REASON_NONE;
    }
}

/**
 * @brief Checks for stale metrics and enforces expiration policies.
 */
static void prv_evaluate_staleness_locked(uint64_t now_ms)
{
    health_component_state_t *metrics_state = &s_health_snapshot.components[HEALTH_COMP_SYSTEM_METRICS];

    if (s_last_metrics_rx_time_ms == 0) {
        metrics_state->status = HEALTH_STATUS_UNKNOWN;
        if (metrics_state->reason == HEALTH_REASON_NONE) {
            metrics_state->reason = HEALTH_REASON_METRIC_NOT_READY;
        }
        return;
    }

    uint64_t age_ms = now_ms - s_last_metrics_rx_time_ms;
    if (age_ms > HEALTH_POLICY_METRICS_STALE_AGE_MS) {
        metrics_state->status = HEALTH_STATUS_DEGRADED;
        metrics_state->reason = HEALTH_REASON_METRIC_STALE;
    } else {
        metrics_state->status = HEALTH_STATUS_HEALTHY;
        metrics_state->reason = HEALTH_REASON_NONE;
    }
    metrics_state->last_update_ms = now_ms;
}

/**
 * @brief Periodic timer callback for autonomous staleness and health evaluation.
 *
 * Runs every 2000ms. Guarantees that if metric producers become completely silent,
 * staleness transitions are detected and logged automatically without waiting for a CLI query.
 */
static void prv_staleness_timer_callback(void *arg)
{
    (void)arg;

    if (!s_initialized || !s_active || s_mutex == NULL) {
        return;
    }

    if (xSemaphoreTake(s_mutex, HEALTH_MGR_MUTEX_TIMEOUT_TICKS) != pdTRUE) {
        return;
    }

    uint64_t now_ms = (uint64_t)esp_timer_get_time() / 1000;
    prv_evaluate_staleness_locked(now_ms);
    prv_recompute_overall_health_locked(now_ms);

    xSemaphoreGive(s_mutex);
}

/* =========================================================================
 * Public API Implementation
 * ========================================================================= */

bool health_manager_init(void)
{
    if (s_initialized) {
        ESP_LOGW(TAG, "health_manager_init() called again — already initialized");
        return true;
    }

    s_mutex = xSemaphoreCreateMutexStatic(&s_mutex_buffer);
    if (s_mutex == NULL) {
        ESP_LOGE(TAG, "Failed to create static health manager mutex");
        return false;
    }

    uint64_t now_ms = (uint64_t)esp_timer_get_time() / 1000;

    memset(&s_health_snapshot, 0, sizeof(s_health_snapshot));
    s_health_snapshot.overall_status = HEALTH_STATUS_UNKNOWN;
    s_health_snapshot.primary_component = HEALTH_COMP_SYSTEM;
    s_health_snapshot.primary_reason = HEALTH_REASON_NOT_INITIALIZED;
    s_health_snapshot.status_since_ms = now_ms;
    s_health_snapshot.snapshot_timestamp_ms = now_ms;

    for (int i = 0; i < HEALTH_COMP_COUNT; i++) {
        s_health_snapshot.components[i].status = HEALTH_STATUS_UNKNOWN;
        s_health_snapshot.components[i].reason = HEALTH_REASON_NOT_INITIALIZED;
        s_health_snapshot.components[i].last_update_ms = now_ms;
    }

    /* Configure autonomous low-frequency staleness evaluation timer */
    const esp_timer_create_args_t timer_args = {
        .callback = prv_staleness_timer_callback,
        .arg = NULL,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "hm_stale",
        .skip_unhandled_events = true
    };

    esp_err_t err = esp_timer_create(&timer_args, &s_staleness_timer);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create autonomous staleness timer (err: %d)", err);
        vSemaphoreDelete(s_mutex);
        s_mutex = NULL;
        return false;
    }

    s_initialized = true;
    ESP_LOGI(TAG, "Health Manager initialized successfully (Autonomous staleness timer created)");
    return true;
}

bool health_manager_start(void)
{
    if (!s_initialized) {
        ESP_LOGE(TAG, "health_manager_start() called before health_manager_init()");
        return false;
    }

    if (s_active) {
        ESP_LOGW(TAG, "health_manager_start() called again — already active");
        return true;
    }

    s_active = true;

    /* Start autonomous periodic staleness monitoring */
    if (s_staleness_timer != NULL) {
        esp_timer_start_periodic(s_staleness_timer, HEALTH_MGR_STALENESS_TIMER_US);
        ESP_LOGI(TAG, "Autonomous staleness evaluation timer started (%d ms period)",
                 (int)(HEALTH_MGR_STALENESS_TIMER_US / 1000));
    }

    ESP_LOGI(TAG, "Health Manager activated (Continuous monitoring mode)");
    return true;
}

void health_manager_process_metrics(const system_metrics_snapshot_t *metrics)
{
    if (metrics == NULL || !s_initialized) {
        return;
    }

    if (xSemaphoreTake(s_mutex, HEALTH_MGR_MUTEX_TIMEOUT_TICKS) != pdTRUE) {
        ESP_LOGW(TAG, "Mutex timeout in health_manager_process_metrics()");
        return;
    }

    uint64_t now_ms = (uint64_t)esp_timer_get_time() / 1000;
    s_last_metrics_rx_time_ms = now_ms;

    /* 1. Evaluate Metrics module health */
    s_health_snapshot.components[HEALTH_COMP_SYSTEM_METRICS].status = HEALTH_STATUS_HEALTHY;
    s_health_snapshot.components[HEALTH_COMP_SYSTEM_METRICS].reason = HEALTH_REASON_NONE;
    s_health_snapshot.components[HEALTH_COMP_SYSTEM_METRICS].last_update_ms = now_ms;

    /* 2. Evaluate sub-dimensions */
    prv_evaluate_temperature_locked(metrics, now_ms);
    prv_evaluate_cpu_locked(metrics, now_ms);
    prv_evaluate_memory_locked(metrics, now_ms);
    prv_evaluate_reset_diagnostics_locked(metrics, now_ms);

    /* 3. Recompute overall status */
    prv_recompute_overall_health_locked(now_ms);

    xSemaphoreGive(s_mutex);
}

void health_manager_tick_now(void)
{
    if (!s_initialized || s_mutex == NULL) {
        return;
    }

    if (xSemaphoreTake(s_mutex, HEALTH_MGR_MUTEX_TIMEOUT_TICKS) != pdTRUE) {
        return;
    }

    uint64_t now_ms = (uint64_t)esp_timer_get_time() / 1000;
    prv_evaluate_staleness_locked(now_ms);
    prv_recompute_overall_health_locked(now_ms);

    xSemaphoreGive(s_mutex);
}

void health_manager_set_component_health(health_component_t comp,
                                        health_status_t status,
                                        health_reason_t reason)
{
    if (!s_initialized || comp >= HEALTH_COMP_COUNT || comp == HEALTH_COMP_SYSTEM) {
        return;
    }

    if (xSemaphoreTake(s_mutex, HEALTH_MGR_MUTEX_TIMEOUT_TICKS) != pdTRUE) {
        return;
    }

    uint64_t now_ms = (uint64_t)esp_timer_get_time() / 1000;
    s_health_snapshot.components[comp].status = status;
    s_health_snapshot.components[comp].reason = reason;
    s_health_snapshot.components[comp].last_update_ms = now_ms;

    prv_recompute_overall_health_locked(now_ms);

    xSemaphoreGive(s_mutex);
}

esp_err_t health_manager_get_snapshot(health_snapshot_t *out_snapshot)
{
    if (out_snapshot == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!s_initialized || s_mutex == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    if (xSemaphoreTake(s_mutex, HEALTH_MGR_MUTEX_TIMEOUT_TICKS) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    uint64_t now_ms = (uint64_t)esp_timer_get_time() / 1000;

    /* Check staleness of metrics sample on snapshot read */
    prv_evaluate_staleness_locked(now_ms);
    prv_recompute_overall_health_locked(now_ms);

    memcpy(out_snapshot, &s_health_snapshot, sizeof(health_snapshot_t));
    xSemaphoreGive(s_mutex);

    return ESP_OK;
}

esp_err_t health_manager_register_change_observer(health_manager_change_cb_t observer, void *user_ctx)
{
    if (s_mutex != NULL) {
        if (xSemaphoreTake(s_mutex, HEALTH_MGR_MUTEX_TIMEOUT_TICKS) != pdTRUE) {
            return ESP_ERR_TIMEOUT;
        }
    }

    s_change_observer = observer;
    s_change_observer_ctx = user_ctx;

    if (s_mutex != NULL) {
        xSemaphoreGive(s_mutex);
    }
    return ESP_OK;
}

const char *health_status_to_str(health_status_t status)
{
    switch (status) {
        case HEALTH_STATUS_HEALTHY:  return "HEALTHY";
        case HEALTH_STATUS_DEGRADED: return "DEGRADED";
        case HEALTH_STATUS_FAULT:    return "FAULT";
        case HEALTH_STATUS_UNKNOWN:
        default:
            return "UNKNOWN";
    }
}

const char *health_reason_to_str(health_reason_t reason)
{
    switch (reason) {
        case HEALTH_REASON_NONE:                     return "NONE";
        case HEALTH_REASON_NOT_INITIALIZED:          return "NOT_INITIALIZED";
        case HEALTH_REASON_METRIC_NOT_READY:         return "METRIC_NOT_READY";
        case HEALTH_REASON_METRIC_STALE:             return "METRIC_STALE";
        case HEALTH_REASON_SOC_TEMP_WARNING:         return "SOC_TEMP_WARNING";
        case HEALTH_REASON_SOC_TEMP_CRITICAL:        return "SOC_TEMP_CRITICAL";
        case HEALTH_REASON_SOC_TEMP_SENSOR_ERROR:    return "SOC_TEMP_SENSOR_ERROR";
        case HEALTH_REASON_CPU_LOAD_SUSTAINED_HIGH:  return "CPU_LOAD_SUSTAINED_HIGH";
        case HEALTH_REASON_CPU_LOAD_CRITICAL:        return "CPU_LOAD_CRITICAL";
        case HEALTH_REASON_HEAP_WARNING:             return "HEAP_WARNING";
        case HEALTH_REASON_HEAP_CRITICAL:            return "HEAP_CRITICAL";
        case HEALTH_REASON_HEAP_FRAGMENTED:          return "HEAP_FRAGMENTED";
        case HEALTH_REASON_HEAP_HISTORICAL_PRESSURE: return "HEAP_HISTORICAL_PRESSURE";
        case HEALTH_REASON_WATCHDOG_RESET_DETECTED:  return "WATCHDOG_RESET_DETECTED";
        case HEALTH_REASON_WATCHDOG_TASK_STALLED:    return "WATCHDOG_TASK_STALLED";
        case HEALTH_REASON_ZIGBEE_COMMISSION_FAILED: return "ZIGBEE_COMMISSION_FAILED";
        case HEALTH_REASON_ZIGBEE_DEGRADED:          return "ZIGBEE_DEGRADED";
        case HEALTH_REASON_Z906_STALE:               return "Z906_STALE";
        case HEALTH_REASON_Z906_COMMUNICATION_FAULT: return "Z906_COMMUNICATION_FAULT";
        case HEALTH_REASON_COMMAND_VERIFIER_UNAVAILABLE: return "COMMAND_VERIFIER_UNAVAILABLE";
        case HEALTH_REASON_OTA_HEALTH_FAULT:         return "OTA_HEALTH_FAULT";
        case HEALTH_REASON_NVS_FAULT:                return "NVS_FAULT";
        case HEALTH_REASON_INITIALIZER_FAILED:       return "INITIALIZER_FAILED";
        default:
            return "UNKNOWN_REASON";
    }
}

const char *health_component_to_str(health_component_t comp)
{
    switch (comp) {
        case HEALTH_COMP_SYSTEM:           return "System (Overall)";
        case HEALTH_COMP_SYSTEM_METRICS:   return "System Metrics";
        case HEALTH_COMP_SOC_TEMPERATURE:  return "SoC Temperature";
        case HEALTH_COMP_CPU:              return "CPU";
        case HEALTH_COMP_MEMORY:           return "Memory";
        case HEALTH_COMP_WATCHDOG:         return "Watchdog";
        case HEALTH_COMP_NVS:              return "NVS";
        case HEALTH_COMP_INITIALIZER:      return "Initializer";
        case HEALTH_COMP_ZIGBEE:           return "Zigbee";
        case HEALTH_COMP_Z906_TRANSPORT:   return "Z906 Transport";
        case HEALTH_COMP_COMMAND_VERIFIER: return "Command Verifier";
        case HEALTH_COMP_OTA:              return "OTA";
        default:
            return "Unknown Component";
    }
}
