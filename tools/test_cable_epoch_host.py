#!/usr/bin/env python3
"""Compile the production FSM and codec with a simulated bounded EventBus.

Test dropped cable notifications, stale queued amplifier bytes and dropped
RX stream bytes. This is host evidence, NOT a live DE-15/UART hardware test.
"""
from pathlib import Path
from tempfile import TemporaryDirectory
import subprocess

ROOT = Path(__file__).resolve().parents[1]

HEADERS = {
    "esp_err.h": """
#pragma once
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_ERR_NO_MEM 0x101
#define ESP_ERR_TIMEOUT 0x107
""",
    "config.h": "#pragma once\n",
    "service_locator.h": "#pragma once\n",
    "adc_hal.h": "#pragma once\n",
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
""",
    "freertos/FreeRTOS.h": """
#pragma once
#include <stdint.h>
#include <stdbool.h>
typedef uint32_t StackType_t;
typedef uint32_t TickType_t;
typedef unsigned UBaseType_t;
typedef int BaseType_t;
#define pdTRUE 1
#define pdMS_TO_TICKS(ms) (ms)
""",
    "freertos/queue.h": "#pragma once\n",
    "freertos/task.h": """
#pragma once
#include "freertos/FreeRTOS.h"
typedef void * TaskHandle_t;
typedef struct { int dummy; } StaticTask_t;
TaskHandle_t xTaskCreateStatic(void (*)(void *), const char *, unsigned,
                              void *, UBaseType_t, StackType_t *, StaticTask_t *);
void vTaskDelay(TickType_t);
""",
    "freertos/semphr.h": """
#pragma once
#include "freertos/FreeRTOS.h"
typedef void * SemaphoreHandle_t;
typedef struct { int dummy; } StaticSemaphore_t;
SemaphoreHandle_t xSemaphoreCreateBinaryStatic(StaticSemaphore_t *);
BaseType_t xSemaphoreTake(SemaphoreHandle_t, TickType_t);
BaseType_t xSemaphoreGive(SemaphoreHandle_t);
""",
    "wdt_manager.h": """
#pragma once
#include <stdbool.h>
bool wdt_manager_register_task(void);
void wdt_manager_reset(void);
""",
    "uart_hal.h": """
#pragma once
#include <stdint.h>
#include <stddef.h>
int z906_uart_hal_write_amp(const uint8_t *, size_t);
int z906_uart_hal_write_console(const uint8_t *, size_t);
""",
    "telemetry.h": """
#pragma once
#include <stdint.h>
typedef enum {
    TELEMETRY_QUERY_TEMP, TELEMETRY_QUERY_INPUT_VOL,
    TELEMETRY_QUERY_IDLE, TELEMETRY_QUERY_STATUS
} telemetry_query_id_t;
void telemetry_notify_response_received(telemetry_query_id_t, int64_t);
""",
    "z906_state.h": """
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "z906_commands.h"
typedef struct {
    bool cable_connected, power, mute;
    uint8_t volume, volume_bass, volume_center, volume_rear;
} z906_state_t;
void z906_state_get(z906_state_t *);
uint32_t z906_state_get_cable_epoch(void);
bool z906_state_confirm_main_for_epoch(uint32_t);
void z906_state_set_volume(uint8_t);
void z906_state_set_trim(uint8_t, uint8_t);
void z906_state_set_input(z906_input_t);
void z906_state_set_effect(z906_effect_t);
void z906_state_set_mute(bool);
void z906_state_set_digital_signal_status(uint8_t);
void z906_state_set_auto_standby(bool);
void z906_state_set_power(bool);
void z906_state_set_temperature(const int8_t *);
void z906_state_set_idle_seconds(uint32_t);
void z906_state_set_input_signal_level(uint32_t);
"""
}

HARNESS = r"""
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include "z906_fsm.h"
#include "z906_codec.h"
#include "z906_state.h"
#include "uart_hal.h"
#include "telemetry.h"

static z906_event_t s_events[192];
static unsigned s_read, s_write;
static bool s_drop_cable_events;
static z906_state_t s_state = { .cable_connected = true };
static uint32_t s_epoch = 1;
static unsigned s_confirmed, s_ota_replies, s_forwarded;

static const uint8_t s_payload[20] = {
    10, 21, 21, 21, 0, 0, 0, 0, 0, 0, 3, 0, 0, 0, 6, 1, 3, 0, 0, 0
};

bool event_bus_post(const z906_event_t *event, TickType_t wait) {
    (void)wait;
    if (event->type == EVENT_CABLE_STATUS_CHANGED && s_drop_cable_events) {
        return false; /* Simulates a full bounded event queue. */
    }
    assert(s_write < sizeof(s_events) / sizeof(s_events[0]));
    s_events[s_write++] = *event;
    return true;
}
bool event_bus_receive(z906_event_t *event, TickType_t wait) {
    (void)wait;
    if (s_read >= s_write) return false;
    *event = s_events[s_read++];
    return true;
}
uint32_t z906_state_get_cable_epoch(void) { return s_epoch; }
void z906_state_get(z906_state_t *out) { *out = s_state; }
bool z906_state_confirm_main_for_epoch(uint32_t e) {
    if (!s_state.cable_connected || e != s_epoch) return false;
    ++s_confirmed;
    return true;
}
void z906_state_set_volume(uint8_t v) { s_state.volume = v; }
void z906_state_set_trim(uint8_t i, uint8_t v) {
    if (i == 0) s_state.volume_bass = v;
    else if (i == 1) s_state.volume_center = v;
    else s_state.volume_rear = v;
}
void z906_state_set_input(z906_input_t i) { (void)i; }
void z906_state_set_effect(z906_effect_t e) { (void)e; }
void z906_state_set_mute(bool v) { s_state.mute = v; }
void z906_state_set_digital_signal_status(uint8_t v) { (void)v; }
void z906_state_set_auto_standby(bool v) { (void)v; }
void z906_state_set_power(bool v) { s_state.power = v; }
void z906_state_set_temperature(const int8_t *v) { (void)v; }
void z906_state_set_idle_seconds(uint32_t v) { (void)v; }
void z906_state_set_input_signal_level(uint32_t v) { (void)v; }
const char *z906_cmd_decode_state_str(uint8_t v) { (void)v; return "MOCK"; }
int z906_uart_hal_write_amp(const uint8_t *b, size_t n) {
    (void)b; return (int)n;
}
int z906_uart_hal_write_console(const uint8_t *b, size_t n) {
    (void)b; s_forwarded += (unsigned)n; return (int)n;
}
void telemetry_notify_response_received(telemetry_query_id_t q, int64_t t) {
    (void)t;
    if (q == TELEMETRY_QUERY_STATUS) ++s_ota_replies;
}

static void drain_all(void) {
    while (s_read < s_write) z906_fsm_step(0);
    s_read = s_write = 0;
}
static void push_amp(uint8_t value, uint32_t epoch, uint32_t stream) {
    z906_event_t e = {
        .type = EVENT_AMP_RX_BYTE,
        .data = { .raw_byte = value },
        .received_at_us = 1000000,
        .cable_epoch = epoch,
        .rx_stream_epoch = stream
    };
    assert(event_bus_post(&e, 0));
}
static void push_config(unsigned begin, unsigned end, uint32_t epoch, uint32_t stream) {
    uint8_t frame[24] = { 0xAA, 0x0A, 0x14 };
    memcpy(frame + 3, s_payload, 20);
    frame[23] = z906_codec_calc_frame_lrc(0x0A, 20, s_payload, 20);
    for (unsigned i = begin; i < end; ++i) {
        push_amp(frame[i], epoch, stream);
    }
}
static void cable_notice(bool connected) {
    z906_event_t e = { .type = EVENT_CABLE_STATUS_CHANGED };
    e.data.cable_connected = connected;
    assert(!event_bus_post(&e, 0));
}
int main(void) {
    assert(z906_fsm_init());
    drain_all(); /* Initial state change from FSM init. */

    /* Start a valid frame, then lose both cable change events. */
    push_config(0, 10, 1, 0);
    drain_all();
    assert(s_confirmed == 0);
    s_drop_cable_events = true;
    s_state.cable_connected = false;
    s_epoch = 2;
    cable_notice(false);
    s_state.cable_connected = true;
    s_epoch = 3;
    cable_notice(true);

    /* Remaining old bytes must be forwarded but NEVER decoded as new state. */
    push_config(10, 24, 1, 0);
    drain_all();
    assert(s_confirmed == 0 && s_ota_replies == 0);

    /* A complete frame from the new epoch can verify current hardware. */
    push_config(0, 24, 3, 1);
    drain_all();
    assert(s_confirmed == 1 && s_ota_replies == 1);
    assert(s_forwarded == 48);

    /* A lost RX byte changes stream generation: no cross-gap frame. */
    s_epoch = 4;
    push_config(0, 10, 4, 1);
    drain_all();
    push_config(11, 24, 4, 2); /* byte 10 was dropped by a full queue */
    drain_all();
    assert(s_confirmed == 1 && s_ota_replies == 1);
    push_config(0, 24, 4, 2);
    drain_all();
    assert(s_confirmed == 2 && s_ota_replies == 2);

    /* A fully queued previous-epoch frame remains passthrough-only. */
    push_config(0, 24, 4, 2);
    s_epoch = 5;
    drain_all();
    assert(s_confirmed == 2 && s_ota_replies == 2);

    puts("PASS: real FSM+codec cable epochs (dropped events, partial RX, stale queue)");
}
"""

def main():
    with TemporaryDirectory(prefix="conductor-fsm-epoch-") as tmp:
        root = Path(tmp)
        for name, source in HEADERS.items():
            file = root / name
            file.parent.mkdir(parents=True, exist_ok=True)
            file.write_text(source, encoding="utf-8")
        main_c = root / "fsm_epoch.c"
        binary = root / "fsm_epoch"
        main_c.write_text(HARNESS, encoding="utf-8")
        subprocess.run([
            "cc", "-std=gnu11", "-Wall", "-Wextra", "-Werror",
            "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections",
            "-I", str(root),
            "-I", str(ROOT / "src/core/state_machine"),
            "-I", str(ROOT / "src/protocol/codec"),
            "-I", str(ROOT / "src/protocol/commands"),
            "-I", str(ROOT / "src/core/event_bus"),
            str(ROOT / "src/core/state_machine/z906_fsm.c"),
            str(ROOT / "src/protocol/codec/z906_codec.c"),
            str(main_c), "-o", str(binary)
        ], check=True, cwd=ROOT)
        subprocess.run([str(binary)], check=True, cwd=ROOT)

if __name__ == "__main__":
    main()
