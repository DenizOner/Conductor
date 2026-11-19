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
/* Preserve argument use without imposing host printf type assumptions. */
void host_log_never_called(const char *, ...);
#define ESP_LOGI(tag, fmt, ...) do { (void)(tag); if (0) host_log_never_called(fmt, ##__VA_ARGS__); } while (0)
#define ESP_LOGW(tag, fmt, ...) do { (void)(tag); if (0) host_log_never_called(fmt, ##__VA_ARGS__); } while (0)
#define ESP_LOGE(tag, fmt, ...) do { (void)(tag); if (0) host_log_never_called(fmt, ##__VA_ARGS__); } while (0)
#define ESP_LOGD(tag, fmt, ...) do { (void)(tag); if (0) host_log_never_called(fmt, ##__VA_ARGS__); } while (0)
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
typedef enum {
    Z906_INPUT_SIX_CHANNEL_DIRECT = 1,
    Z906_INPUT_RCA_STEREO = 2,
    Z906_INPUT_OPTICAL_1 = 3,
    Z906_INPUT_OPTICAL_2 = 4,
    Z906_INPUT_DIGITAL_COAXIAL = 5,
    Z906_INPUT_AUXILIARY = 6
} z906_input_t;
typedef enum {
    Z906_EFFECT_3D = 0,
    Z906_EFFECT_2_1 = 1,
    Z906_EFFECT_4_1 = 2,
    Z906_EFFECT_NONE = 3
} z906_effect_t;
#include "z906_commands.h"
typedef struct {
    bool cable_connected, power, mute, auto_standby_enabled, temp_valid;
    uint8_t volume, volume_bass, volume_center, volume_rear;
    uint8_t digital_signal_status;
    z906_input_t input;
    z906_effect_t effect;
    int8_t temp_celsius[3];
    uint32_t idle_seconds, input_signal_level;
} z906_state_t;
void z906_state_get(z906_state_t *);
uint32_t z906_state_get_cable_epoch(void);
bool z906_state_confirm_main_for_epoch(uint32_t);
bool z906_state_apply_verified_config_for_epoch(uint32_t, uint32_t,
                                                const z906_state_t *);
uint32_t z906_state_get_amp_rx_stream_epoch(void);
uint32_t z906_state_note_amp_rx_loss(uint32_t);
bool z906_state_apply_amp_echo_for_generations(uint32_t, uint32_t, uint8_t);
void z906_state_set_main_valid(bool);
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
bool z906_state_apply_temperature_for_generations(uint32_t, uint32_t, const int8_t *);
bool z906_state_apply_idle_for_generations(uint32_t, uint32_t, uint32_t);
bool z906_state_apply_input_level_for_generations(uint32_t, uint32_t, uint32_t);
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
static uint32_t s_stream = 0;
static unsigned s_confirmed, s_ota_replies, s_forwarded;
static unsigned s_invalidations, s_input_updates;
static bool s_switch_during_console_write;
static unsigned s_interleave_after_console_bytes;

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
uint32_t z906_state_get_amp_rx_stream_epoch(void) { return s_stream; }
uint32_t z906_state_note_amp_rx_loss(uint32_t cable) {
    assert(cable == s_epoch);
    ++s_invalidations;
    return ++s_stream;
}
void z906_state_get(z906_state_t *out) { *out = s_state; }
void z906_state_set_main_valid(bool valid) {
    assert(!valid); ++s_invalidations;
}
bool z906_state_confirm_main_for_epoch(uint32_t e) {
    if (!s_state.cable_connected || e != s_epoch) return false;
    ++s_confirmed;
    return true;
}
bool z906_state_apply_verified_config_for_epoch(uint32_t e, uint32_t rx,
                                                const z906_state_t *cfg) {
    if (!s_state.cable_connected || e != s_epoch || rx != s_stream ||
        cfg == NULL) return false;
    ++s_confirmed;
    ++s_input_updates;
    s_state.volume = cfg->volume;
    s_state.input = cfg->input;
    s_state.power = cfg->power;
    return true;
}
bool z906_state_apply_amp_echo_for_generations(uint32_t cable, uint32_t rx,
                                               uint8_t cmd) {
    if (!s_state.cable_connected || cable != s_epoch || rx != s_stream) {
        return false;
    }
    switch (cmd) {
        case 0x36: s_state.power = false; return true;
        case 0x20: s_state.mute = true; return true;
        case 0x21: s_state.mute = false; return true;
        case 0x08: ++s_state.volume; return true;
        case 0x03: s_state.input = Z906_INPUT_OPTICAL_1; ++s_input_updates; return true;
        default: return false;
    }
}
void z906_state_set_volume(uint8_t v) { s_state.volume = v; }
void z906_state_set_trim(uint8_t i, uint8_t v) {
    if (i == 0) s_state.volume_bass = v;
    else if (i == 1) s_state.volume_center = v;
    else s_state.volume_rear = v;
}
void z906_state_set_input(z906_input_t i) { (void)i; ++s_input_updates; }
void z906_state_set_effect(z906_effect_t e) { (void)e; }
void z906_state_set_mute(bool v) { s_state.mute = v; }
void z906_state_set_digital_signal_status(uint8_t v) { (void)v; }
void z906_state_set_auto_standby(bool v) { (void)v; }
void z906_state_set_power(bool v) { s_state.power = v; }
void z906_state_set_temperature(const int8_t *v) { (void)v; }
void z906_state_set_idle_seconds(uint32_t v) { (void)v; }
void z906_state_set_input_signal_level(uint32_t v) { (void)v; }
bool z906_state_apply_temperature_for_generations(uint32_t cable, uint32_t rx,
                                                  const int8_t *v) {
    if (cable != s_epoch || rx != s_stream || !s_state.cable_connected) return false;
    for (int i = 0; i < 3; i++) s_state.temp_celsius[i] = v[i];
    s_state.temp_valid = true;
    return true;
}
bool z906_state_apply_idle_for_generations(uint32_t cable, uint32_t rx, uint32_t v) {
    if (cable != s_epoch || rx != s_stream || !s_state.cable_connected) return false;
    s_state.idle_seconds = v;
    return true;
}
bool z906_state_apply_input_level_for_generations(uint32_t cable, uint32_t rx, uint32_t v) {
    if (cable != s_epoch || rx != s_stream || !s_state.cable_connected) return false;
    s_state.input_signal_level = v;
    return true;
}
const char *z906_cmd_decode_state_str(uint8_t v) { (void)v; return "MOCK"; }
int z906_uart_hal_write_amp(const uint8_t *b, size_t n) {
    (void)b; return (int)n;
}
int z906_uart_hal_write_console(const uint8_t *b, size_t n) {
    (void)b;
    s_forwarded += (unsigned)n;
    if (s_switch_during_console_write ||
        (s_interleave_after_console_bytes > 0 && --s_interleave_after_console_bytes == 0)) {
        /* Interrupt UART console forwarding on the FINAL packet byte. */
        s_switch_during_console_write = false;
        ++s_epoch;
        ++s_stream;
    }
    return (int)n;
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
static void push_multi(uint8_t type, uint8_t len, const uint8_t *payload,
                       uint32_t epoch, uint32_t rx) {
    push_amp(0xAA, epoch, rx);
    push_amp(type, epoch, rx);
    push_amp(len, epoch, rx);
    for (unsigned i = 0; i < len; i++) push_amp(payload[i], epoch, rx);
    push_amp(z906_codec_calc_frame_lrc(type, len, payload, len), epoch, rx);
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
    ++s_stream;
    cable_notice(false);
    s_state.cable_connected = true;
    s_epoch = 3;
    ++s_stream;
    cable_notice(true);

    /* Remaining old bytes must be forwarded but NEVER decoded as new state. */
    push_config(10, 24, 1, 0);
    drain_all();
    assert(s_confirmed == 0 && s_ota_replies == 0);

    /* A complete frame from the new epoch can verify current hardware. */
    push_config(0, 24, 3, s_stream);
    drain_all();
    assert(s_confirmed == 1 && s_ota_replies == 1);
    assert(s_forwarded == 48);

    /* A lost RX byte changes stream generation: no cross-gap frame and
     * no orphan payload byte can be interpreted as standalone input echo. */
    unsigned earlier_input_updates = s_input_updates;
    s_epoch = 4;
    ++s_stream;
    push_config(0, 10, 4, s_stream);
    drain_all();
    (void)z906_state_note_amp_rx_loss(4); /* Last dropped RX byte. */
    push_config(11, 24, 4, s_stream); /* byte 10 was lost */
    drain_all();
    assert(s_confirmed == 1 && s_ota_replies == 1);
    assert(s_input_updates == earlier_input_updates);
    assert(s_invalidations == 1);
    push_config(0, 24, 4, s_stream);
    drain_all();
    assert(s_confirmed == 2 && s_ota_replies == 2);

    /* A fully queued previous-epoch frame remains passthrough-only. */
    push_config(0, 24, 4, s_stream);
    s_epoch = 5;
    ++s_stream;
    drain_all();
    assert(s_confirmed == 2 && s_ota_replies == 2);

    /* E19F-02: a cable transition DURING raw forwarding must prevent
     * the queued single-byte standby/mute/input echo from changing state. */
    s_state.power = true;
    s_state.mute = false;
    s_state.input = Z906_INPUT_OPTICAL_1;
    unsigned previous_input_updates = s_input_updates;
    uint32_t old_epoch = s_epoch, old_stream = s_stream;
    s_switch_during_console_write = true;
    push_amp(0x36, old_epoch, old_stream);
    drain_all();
    assert(s_state.power && !s_state.mute);
    assert(s_epoch == old_epoch + 1 && s_stream == old_stream + 1);
    push_amp(0x20, old_epoch, old_stream); /* stale mute */
    push_amp(0x03, old_epoch, old_stream); /* stale input */
    drain_all();
    assert(!s_state.mute && s_input_updates == previous_input_updates);
    /* A cable generation gap requires a fresh full CONFIG frame before any
     * orphan-looking standalone byte can be trusted again. */
    push_config(0, 24, s_epoch, s_stream);
    drain_all();
    push_amp(0x20, s_epoch, s_stream);
    drain_all();
    assert(s_state.mute);

    /* G-03: a transition at the LAST byte of a valid multi-byte telemetry
     * frame must not write stale TEMP, IDLE or INPUT_VOL state. */
    static const uint8_t temp[5] = { 0, 22, 23, 24, 0 };
    static const uint8_t idle[3] = { 0, 1, 44 };
    static const uint8_t input_vol[3] = { 0, 3, 33 };
    const uint8_t packet_types[3] = { 0x0C, 0x0F, 0x08 };
    const uint8_t packet_sizes[3] = { 5, 3, 3 };
    const uint8_t *packets[3] = { temp, idle, input_vol };
    for (unsigned i = 0; i < 3; i++) {
        uint32_t old_cable = s_epoch, old_rx = s_stream;
        unsigned packet_bytes = 3u + packet_sizes[i] + 1u;
        s_interleave_after_console_bytes = packet_bytes;
        push_multi(packet_types[i], packet_sizes[i], packets[i], old_cable, old_rx);
        drain_all();
        assert(s_epoch == old_cable + 1 && s_stream == old_rx + 1);
        assert(!s_state.temp_valid && s_state.idle_seconds == 0 &&
               s_state.input_signal_level == 0);
        /* A fresh full frame resynchronizes and is accepted in this epoch. */
        push_multi(packet_types[i], packet_sizes[i], packets[i], s_epoch, s_stream);
        drain_all();
        if (i == 0) {
            assert(s_state.temp_valid && s_state.temp_celsius[0] == 22);
            s_state.temp_valid = false; /* Reset only the mock for next iteration. */
        } else if (i == 1) {
            assert(s_state.idle_seconds == 300);
            s_state.idle_seconds = 0;
        } else {
            assert(s_state.input_signal_level == 801);
        }
    }
    puts("PASS: FSM+codec cable loss, orphan RX, atomic multi-byte telemetry");
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
