/**
 * @file telemetry.c
 * @brief Implementation of hardware telemetry, health auditing, stateful query polling,
 *        and freshness / timeout accounting.
 */

#include "telemetry.h"
#include "adc_hal.h"
#include "uart_hal.h"
#include "event_bus.h"
#include "z906_state.h"
#include "z906_commands.h"
#include "z906_cmd_verifier.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "ota_health.h"
#include "ota_client.h"
#include <string.h>

static const char *TAG = "TELEMETRY";
static bool s_last_cable_connected = false;
static uint32_t s_cycle_counter = 0;
static uint8_t s_poll_stage = 0;

static telemetry_stat_t s_stats[TELEMETRY_QUERY_COUNT] = { 0 };

/* Track active pending query and dispatch timestamp */
static int s_pending_query_id = -1;
static int64_t s_pending_dispatch_time_us = 0;
#define TELEMETRY_TIMEOUT_US (2000000) /* 2.0 seconds timeout for amplifier response */
#define OTA_STATUS_POLL_INTERVAL_US (4000000LL) /* Verified CONFIG no older than 5s */
static int64_t s_last_ota_status_request_us = 0;

static telemetry_query_id_t opcode_to_query_id(uint8_t opcode)
{
    switch (opcode) {
        case Z906_CMD_READ_TEMP:      return TELEMETRY_QUERY_TEMP;
        case Z906_CMD_READ_INPUT_VOL: return TELEMETRY_QUERY_INPUT_VOL;
        case Z906_CMD_READ_IDLE:      return TELEMETRY_QUERY_IDLE;
        case Z906_CMD_READ_STATUS:
        default:                      return TELEMETRY_QUERY_STATUS;
    }
}

bool telemetry_dispatch_query(uint8_t query_opcode)
{
    if (z906_cmd_verifier_is_busy()) {
        return false;
    }

    if (!z906_cmd_verifier_try_lock()) {
        return false;
    }

    int written = z906_uart_hal_write_amp(&query_opcode, 1);
    z906_cmd_verifier_unlock();

    if (written == 1) {
        telemetry_query_id_t qid = opcode_to_query_id(query_opcode);
        int64_t now = esp_timer_get_time();
        s_stats[qid].request_count++;
        s_stats[qid].last_request_time_us = now;

        s_pending_query_id = (int)qid;
        s_pending_dispatch_time_us = now;
        return true;
    }

    return false;
}

void telemetry_notify_response_received(telemetry_query_id_t query_id)
{
    if ((int)query_id >= 0 && query_id < TELEMETRY_QUERY_COUNT) {
        int64_t now = esp_timer_get_time();
        s_stats[query_id].success_count++;
        s_stats[query_id].consecutive_failures = 0;
        s_stats[query_id].last_success_time_us = now;

        if (s_pending_query_id == (int)query_id) {
            s_pending_query_id = -1;
        }
    }
}

bool telemetry_get_stats(telemetry_query_id_t query_id, telemetry_stat_t *out_stat)
{
    if ((int)query_id < 0 || query_id >= TELEMETRY_QUERY_COUNT || out_stat == NULL) {
        return false;
    }
    *out_stat = s_stats[query_id];
    return true;
}

void telemetry_process_cycle(void)
{
    /* 1. Evaluate physical DE-15 cable connection presence */
    bool cable_now = z906_adc_hal_is_cable_connected();

    if (cable_now != s_last_cable_connected) {
        ESP_LOGW(TAG, "Cable presence status changed: %s", cable_now ? "CONNECTED" : "DISCONNECTED");
        s_last_cable_connected = cable_now;
        z906_state_set_cable_status(cable_now);

        z906_event_t evt = {
            .type = EVENT_CABLE_STATUS_CHANGED,
            .data = { .cable_connected = cable_now }
        };
        event_bus_post(&evt, 0);
    }

    if (!cable_now) {
        s_pending_query_id = -1;
        return;
    }

    /* 2. Check for timeout on any pending telemetry query */
    int64_t now = esp_timer_get_time();
    if (s_pending_query_id >= 0) {
        if ((now - s_pending_dispatch_time_us) > TELEMETRY_TIMEOUT_US) {
            telemetry_query_id_t timed_out_id = (telemetry_query_id_t)s_pending_query_id;
            s_stats[timed_out_id].timeout_count++;
            s_stats[timed_out_id].consecutive_failures++;

            if (s_stats[timed_out_id].consecutive_failures <= 3 ||
                s_stats[timed_out_id].consecutive_failures % 10 == 0) {
                ESP_LOGW(TAG, "Telemetry query %d timed out (consecutive failures: %lu)",
                         (int)timed_out_id, (unsigned long)s_stats[timed_out_id].consecutive_failures);
            }
            s_pending_query_id = -1;
        }
    }

    /* While pending, poll STATUS approximately every 4 seconds, including AFTER
     * the first valid CONFIG reply. The OTA health decision requires a recent
     * response, not a sticky main_state_valid flag from earlier in the window.
     * Reuse the verifier mutex, never preempt user commands or another query. */
    if (ota_health_is_pending_verify()) {
        if (s_pending_query_id < 0 &&
            (s_last_ota_status_request_us == 0 ||
             now - s_last_ota_status_request_us >= OTA_STATUS_POLL_INTERVAL_US)) {
            if (telemetry_dispatch_query(Z906_CMD_READ_STATUS)) {
                s_last_ota_status_request_us = now;
            }
        }
        return;
    }
    s_last_ota_status_request_us = 0;

    /* 3. Execute non-interfering sequential telemetry polling every 5 seconds (5 * 1000ms ticks) */
    s_cycle_counter++;
    if (s_cycle_counter < 5) {
        return;
    }
    s_cycle_counter = 0;

    z906_state_t cur;
    z906_state_get(&cur);

    /* Telemetry query stages:
     * Stage 0: 0x25 (Read Temperatures: 3 sensors)
     * Stage 1: 0x2F (Read Input Volume / Signal Level - active mode only)
     * Stage 2: 0x31 (Read Idle Seconds)
     * Stage 3: 0x34 (Read Status / Configuration)
     */
    switch (s_poll_stage) {
        case 0:
            ESP_LOGD(TAG, "Polling stage 0: Read Temperature (0x25)");
            telemetry_dispatch_query(Z906_CMD_READ_TEMP);
            s_poll_stage = 1;
            break;

        case 1:
            if (cur.power) {
                ESP_LOGD(TAG, "Polling stage 1: Read Input Signal Level (0x2F)");
                telemetry_dispatch_query(Z906_CMD_READ_INPUT_VOL);
            }
            s_poll_stage = 2;
            break;

        case 2:
            ESP_LOGD(TAG, "Polling stage 2: Read Idle Duration (0x31)");
            telemetry_dispatch_query(Z906_CMD_READ_IDLE);
            s_poll_stage = 3;
            break;

        case 3:
        default:
            ESP_LOGD(TAG, "Polling stage 3: Read Status/Configuration (0x34)");
            telemetry_dispatch_query(Z906_CMD_READ_STATUS);
            s_poll_stage = 0;
            break;
    }
}

void telemetry_audit_stack_usage(TaskHandle_t task, const char *task_name)
{
    if (task == NULL || task_name == NULL) {
        return;
    }

    UBaseType_t watermark = uxTaskGetStackHighWaterMark(task);
    ESP_LOGD(TAG, "Task [%s] stack watermark: %u bytes remaining", task_name, (unsigned int)(watermark * sizeof(StackType_t)));
}

/* =========================================================================
 * Telemetry Task — owned by management/telemetry (telemetry.c)
 *
 * Ownership rationale: The telemetry task runs telemetry_process_cycle(), which
 * is a private concern of this module. Lifecycle (create/start/run) belongs here.
 * The system initializer only calls telemetry_start() — it never touches the loop.
 * ========================================================================= */

#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "wdt_manager.h"

/** @brief Stack depth in words for the telemetry polling task */
#define TELEMETRY_TASK_STACK_WORDS    (2048u / sizeof(StackType_t))

/** @brief FreeRTOS priority for the telemetry task */
#define TELEMETRY_TASK_PRIORITY       1u

/** @brief Max ms to wait for the task to signal STARTED before timeout */
#define TELEMETRY_STARTUP_TIMEOUT_MS  500u

static StackType_t  s_telemetry_stack[TELEMETRY_TASK_STACK_WORDS];
static StaticTask_t s_telemetry_tcb;

/** @brief Private startup binary semaphore — given by task once it enters main loop */
static SemaphoreHandle_t s_started_sem    = NULL;
static StaticSemaphore_t s_started_sem_buf;

/**
 * @brief Telemetry polling task body.
 *
 * No EventGroup gate — startup handled by private semaphore handshake in
 * telemetry_start(). Task signals STARTED immediately before entering loop.
 * Registers with TWDT and resets it every iteration. Also reports TWDT
 * health to OTA health engine once per cycle (criterion: TWDT).
 *
 * @param pvParameters Unused.
 */
static void prv_telemetry_task(void *pvParameters)
{
    (void)pvParameters; /* No gate handle — startup handled by private semaphore */

    wdt_manager_register_task();
    ESP_LOGI(TAG, "telemetry_task running (Priority %u)", TELEMETRY_TASK_PRIORITY);

    /* Signal telemetry_start() that this task is alive and entering its loop */
    xSemaphoreGive(s_started_sem);

    bool twdt_reported = false;

    while (1) {
        wdt_manager_reset();
        telemetry_process_cycle();

        /* Report TWDT health to OTA engine once after a successful cycle.
         * This proves all TWDT-registered tasks are alive and feeding. */
        if (!twdt_reported) {
            ota_health_report_twdt_ok();
            twdt_reported = true;
        }

        /* Single runtime owner of the pending-image health evaluation. */
        ota_client_process_cycle();

        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

bool telemetry_start(void)
{
    /* Create the binary startup semaphore — starts empty (task must give it) */
    s_started_sem = xSemaphoreCreateBinaryStatic(&s_started_sem_buf);

    TaskHandle_t handle = xTaskCreateStatic(
        prv_telemetry_task,
        "telemetry_task",
        TELEMETRY_TASK_STACK_WORDS,
        NULL,                    /* pvParameters: no gate handle needed */
        TELEMETRY_TASK_PRIORITY,
        s_telemetry_stack,
        &s_telemetry_tcb
    );

    if (handle == NULL) {
        ESP_LOGE(TAG, "Failed to create telemetry_task");
        return false;
    }

    /* Block until task signals STARTED or timeout */
    BaseType_t got = xSemaphoreTake(s_started_sem,
                                    pdMS_TO_TICKS(TELEMETRY_STARTUP_TIMEOUT_MS));
    if (got != pdTRUE) {
        ESP_LOGE(TAG, "telemetry_task did not signal STARTED within %u ms",
                 TELEMETRY_STARTUP_TIMEOUT_MS);
        return false;
    }

    ESP_LOGI(TAG, "telemetry_task STARTED (Priority %u, stack %u words)",
             TELEMETRY_TASK_PRIORITY, TELEMETRY_TASK_STACK_WORDS);
    return true;
}
