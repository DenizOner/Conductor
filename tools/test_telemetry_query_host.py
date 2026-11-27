#!/usr/bin/env python3
"""Exercise the production telemetry query lifecycle with host-only SDK stubs.

A protocol frame has no sequence ID, so matching is bounded by query type and
a 2-second window; the test does not claim cryptographic request identity.
"""
from pathlib import Path
from tempfile import TemporaryDirectory
import subprocess

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "src/management/telemetry/telemetry.c"

HEADERS = {
    "freertos/FreeRTOS.h": """
#pragma once
#include <stdbool.h>
#include <stdint.h>
typedef uint32_t TickType_t;
typedef uint32_t StackType_t;
typedef unsigned UBaseType_t;
typedef int BaseType_t;
typedef void * SemaphoreHandle_t;
typedef struct { int dummy; } StaticSemaphore_t;
typedef int portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(m) ((void)(m))
#define portEXIT_CRITICAL(m) ((void)(m))
#define pdTRUE 1
#define pdMS_TO_TICKS(ms) (ms)
""",
    "freertos/task.h": """
#pragma once
#include "freertos/FreeRTOS.h"
typedef void * TaskHandle_t;
typedef struct { int dummy; } StaticTask_t;
TaskHandle_t xTaskCreateStatic(void (*)(void *), const char *, unsigned,
                               void *, UBaseType_t, StackType_t *, StaticTask_t *);
UBaseType_t uxTaskGetStackHighWaterMark(TaskHandle_t);
void vTaskDelay(TickType_t);
""",
    "freertos/event_groups.h": """
#pragma once
#include "freertos/FreeRTOS.h"
SemaphoreHandle_t xSemaphoreCreateBinaryStatic(StaticSemaphore_t *);
BaseType_t xSemaphoreGive(SemaphoreHandle_t);
BaseType_t xSemaphoreTake(SemaphoreHandle_t, TickType_t);
""",
    "adc_hal.h": """
#pragma once
#include <stdbool.h>
bool z906_adc_hal_is_cable_connected(void);
""",
    "uart_hal.h": """
#pragma once
#include <stdint.h>
#include <stddef.h>
int z906_uart_hal_write_amp(const uint8_t *, size_t);
""",
    "event_bus.h": """
#pragma once
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
enum { EVENT_CABLE_STATUS_CHANGED = 1 };
typedef struct {
    int type;
    struct { bool cable_connected; } data;
} z906_event_t;
bool event_bus_post(const z906_event_t *, TickType_t);
""",
    "z906_state.h": """
#pragma once
#include <stdbool.h>
#include <stdint.h>
typedef struct { bool power; } z906_state_t;
void z906_state_get(z906_state_t *);
void z906_state_set_cable_status(bool);
void z906_state_note_matched_status_reply(int64_t);
""",
    "z906_commands.h": """
#pragma once
#define Z906_CMD_READ_TEMP 0x25
#define Z906_CMD_READ_INPUT_VOL 0x2F
#define Z906_CMD_READ_IDLE 0x31
#define Z906_CMD_READ_STATUS 0x34
""",
    "z906_cmd_verifier.h": """
#pragma once
#include <stdbool.h>
bool z906_cmd_verifier_is_busy(void);
bool z906_cmd_verifier_try_lock(void);
void z906_cmd_verifier_unlock(void);
""",
    "ota_health.h": """
#pragma once
#include <stdbool.h>
bool ota_health_is_pending_verify(void);
void ota_health_report_twdt_observation(bool);
""",
    "ota_client.h": """
#pragma once
void ota_client_process_cycle(void);
""",
    "wdt_manager.h": """
#pragma once
#include <stdbool.h>
bool wdt_manager_register_task(void);
bool wdt_manager_reset_checked(void);
""",
    "esp_timer.h": """
#pragma once
#include <stdint.h>
int64_t esp_timer_get_time(void);
""",
    "esp_log.h": """
#pragma once
#include <stdio.h>
#define ESP_LOGI(tag, fmt, ...) do { (void)(tag); if (0) fprintf(stderr, fmt, ##__VA_ARGS__); } while (0)
#define ESP_LOGW(tag, fmt, ...) do { (void)(tag); if (0) fprintf(stderr, fmt, ##__VA_ARGS__); } while (0)
#define ESP_LOGE(tag, fmt, ...) do { (void)(tag); if (0) fprintf(stderr, fmt, ##__VA_ARGS__); } while (0)
#define ESP_LOGD(tag, fmt, ...) do { (void)(tag); if (0) fprintf(stderr, fmt, ##__VA_ARGS__); } while (0)
"""
}
TEST = r"""
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include "telemetry.h"
#include "z906_state.h"
#include "event_bus.h"
#include "esp_timer.h"
#include <stddef.h>

static int64_t s_now = 1000000;
static bool s_busy, s_connected = true, s_ota_pending;
static int s_writes, s_matched, s_disconnects;
static bool s_inject_rx_during_write;
static int s_write_result = 1;
static uint8_t s_last_opcode;
int64_t esp_timer_get_time(void) { return s_now; }
bool z906_adc_hal_is_cable_connected(void) { return s_connected; }
bool z906_cmd_verifier_is_busy(void) { return s_busy; }
bool z906_cmd_verifier_try_lock(void) { return !s_busy; }
void z906_cmd_verifier_unlock(void) {}
int z906_uart_hal_write_amp(const uint8_t *b, size_t n) {
    assert(n == 1);
    s_last_opcode = *b;
    ++s_writes;
    /* A pre-commit RX callback must never be mistaken for a sent command. */
    if (s_inject_rx_during_write) {
        telemetry_notify_response_received(TELEMETRY_QUERY_STATUS, s_now);
    }
    return s_write_result;
}
void z906_state_note_matched_status_reply(int64_t stamp) {
    assert(stamp == s_now); ++s_matched;
}
void z906_state_set_cable_status(bool cable) {
    if (!cable) ++s_disconnects;
}
void z906_state_get(z906_state_t *st) { st->power = true; }
bool event_bus_post(const z906_event_t *e, TickType_t wait) {
    (void)e; (void)wait; return true;
}
bool ota_health_is_pending_verify(void) { return s_ota_pending; }

int main(void) {
    telemetry_stat_t st;
    /* Prime the physical cable state, then test an unsolicited STATUS packet. */
    telemetry_process_cycle();
    telemetry_notify_response_received(TELEMETRY_QUERY_STATUS, s_now);
    assert(s_matched == 0);
    assert(telemetry_get_stats(TELEMETRY_QUERY_STATUS, &st));
    assert(st.success_count == 0);

    /* Active verifier holds priority: an unsolicited poll must be skipped. */
    s_busy = true;
    assert(!telemetry_dispatch_query(0x34));
    assert(s_writes == 0);
    s_busy = false;
    /* TX failure, including a decoder preemption during write, is not success. */
    s_inject_rx_during_write = true;
    s_write_result = 0;
    assert(!telemetry_dispatch_query(0x34));
    assert(s_matched == 0);
    assert(telemetry_get_stats(TELEMETRY_QUERY_STATUS, &st));
    assert(st.success_count == 0 && st.request_count == 0);
    s_write_result = 1;
    assert(telemetry_dispatch_query(0x34));
    assert(s_matched == 0); /* Pre-commit injection ignored even if TX succeeds. */
    s_inject_rx_during_write = false;
    assert(s_last_opcode == 0x34 && s_writes == 2);
    assert(!telemetry_dispatch_query(0x34)); /* No overlapping queries */
    telemetry_notify_response_received(TELEMETRY_QUERY_TEMP, s_now);
    assert(s_matched == 0);
    /* An old queued frame that began BEFORE TX must be rejected. */
    telemetry_notify_response_received(TELEMETRY_QUERY_STATUS, s_now - 1);
    assert(s_matched == 0);
    s_now += 1000000;
    telemetry_notify_response_received(TELEMETRY_QUERY_STATUS, s_now);
    assert(s_matched == 1);
    assert(telemetry_get_stats(TELEMETRY_QUERY_STATUS, &st));
    assert(st.success_count == 1);

    /* Valid CONFIG after the 2-second query timeout is NOT OTA evidence. */
    assert(telemetry_dispatch_query(0x34));
    s_now += 3000000;
    telemetry_notify_response_received(TELEMETRY_QUERY_STATUS, s_now);
    assert(s_matched == 1);
    telemetry_process_cycle();
    assert(telemetry_get_stats(TELEMETRY_QUERY_STATUS, &st));
    assert(st.timeout_count == 1);

    /* Cable loss revokes pending query; reconnect cannot reuse old response. */
    s_connected = false;
    telemetry_process_cycle();
    assert(s_disconnects == 1);
    s_connected = true;
    telemetry_process_cycle();

    /* Pending OTA requests an early STATUS even before normal 20-second cycle. */
    s_ota_pending = true;
    s_now += 1000000;
    telemetry_process_cycle();
    assert(s_last_opcode == 0x34);
    int prev = s_writes;
    s_now += 1000000;
    telemetry_notify_response_received(TELEMETRY_QUERY_STATUS, s_now);
    s_now += 4000000;
    telemetry_process_cycle();
    assert(s_writes == prev + 1);

    puts("PASS: real telemetry query lifecycle host cases (pre-TX, failed TX, stale RX, timeout, cable)");
}
"""

def main():
    with TemporaryDirectory(prefix="conductor-query-") as tmp:
        path = Path(tmp)
        for name, content in HEADERS.items():
            file = path / name
            file.parent.mkdir(parents=True, exist_ok=True)
            file.write_text(content, encoding="utf-8")
        test = path / "query_host.c"
        test.write_text(TEST, encoding="utf-8")
        executable = path / "query_host"
        subprocess.run([
            "cc", "-std=gnu11", "-Wall", "-Wextra", "-Werror",
            "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections",
            "-I", str(path), "-I", str(SOURCE.parent),
            str(SOURCE), str(test), "-o", str(executable)
        ], cwd=ROOT, check=True)
        subprocess.run([str(executable)], cwd=ROOT, check=True)

if __name__ == "__main__":
    main()
