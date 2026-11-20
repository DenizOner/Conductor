#!/usr/bin/env python3
"""Host-compile production Z906 state manager and verify atomic cable-epoch CONFIG.

ESP-IDF mutex primitives are deterministic host mocks: real task scheduling
and physical UART/ADC transitions still require device acceptance.
"""
from pathlib import Path
from tempfile import TemporaryDirectory
import subprocess

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "src/core/state_manager/z906_state.c"

HEADERS = {
    "config.h": """
#pragma once
#define Z906_DEFAULT_VOLUME_LEVEL 9
#define Z906_MAX_VOLUME_LEVEL 43
""",
    "esp_log.h": """
#pragma once
void host_log_never_called(const char *, ...);
#define ESP_LOGI(tag, fmt, ...) do { (void)(tag); if (0) host_log_never_called(fmt, ##__VA_ARGS__); } while (0)
#define ESP_LOGD(tag, fmt, ...) do { (void)(tag); if (0) host_log_never_called(fmt, ##__VA_ARGS__); } while (0)
#define ESP_LOGE(tag, fmt, ...) do { (void)(tag); if (0) host_log_never_called(fmt, ##__VA_ARGS__); } while (0)
""",
    "esp_timer.h": """
#pragma once
#include <stdint.h>
int64_t esp_timer_get_time(void);
""",
    "freertos/FreeRTOS.h": """
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
typedef uint32_t TickType_t;
typedef int BaseType_t;
#define pdTRUE 1
#define portMAX_DELAY 0xffffffffu
""",
    "freertos/semphr.h": """
#pragma once
#include "freertos/FreeRTOS.h"
typedef struct { int held; } StaticSemaphore_t;
typedef StaticSemaphore_t * SemaphoreHandle_t;
SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t *);
BaseType_t xSemaphoreTake(SemaphoreHandle_t, TickType_t);
BaseType_t xSemaphoreGive(SemaphoreHandle_t);
"""
}
HARNESS = r"""
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include "z906_state.h"
#include "freertos/semphr.h"

static int64_t s_now = 1000000;
static int s_observers;
static bool s_observed_coherent;
static uint8_t s_expected_volume = 31;
int64_t esp_timer_get_time(void) { return s_now; }
SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t *b) {
    b->held = 0; return b;
}
BaseType_t xSemaphoreTake(SemaphoreHandle_t m, TickType_t wait) {
    (void)wait; assert(m && m->held == 0); m->held = 1; return pdTRUE;
}
BaseType_t xSemaphoreGive(SemaphoreHandle_t m) {
    assert(m && m->held == 1); m->held = 0; return pdTRUE;
}
static void observer(uint32_t mask) {
    z906_state_t sample;
    z906_state_get(&sample); /* Proves no callback under held state mutex. */
    if (sample.main_state_valid) {
        assert(sample.cable_connected && sample.volume == s_expected_volume);
        assert(sample.power && sample.volume_bass == 21);
        assert(sample.volume_center == 20 && sample.volume_rear == 19);
        assert(sample.input == Z906_INPUT_OPTICAL_1);
        s_observed_coherent = true;
    }
    assert(mask != 0);
    ++s_observers;
}
int main(void) {
    z906_state_init();
    z906_state_register_change_observer(observer);
    uint32_t start_epoch = z906_state_get_cable_epoch();
    assert(start_epoch == 0);
    z906_state_set_cable_status(true);
    uint32_t first_epoch = z906_state_get_cable_epoch();
    uint32_t first_stream = z906_state_get_amp_rx_stream_epoch();
    assert(first_epoch == 1 && first_stream == 1);

    z906_state_t new_config = { 0 };
    new_config.power = true;
    new_config.volume = 31;
    new_config.volume_bass = 21;
    new_config.volume_center = 20;
    new_config.volume_rear = 19;
    new_config.input = Z906_INPUT_OPTICAL_1;
    new_config.effect = Z906_EFFECT_3D;
    new_config.mute = true;
    new_config.digital_signal_status = 0x17;
    new_config.auto_standby_enabled = true;

    int earlier = s_observers;
    assert(!z906_state_apply_verified_config_for_epoch(start_epoch, first_stream, &new_config));
    z906_state_t before;
    z906_state_get(&before);
    assert(before.volume == 9 && !before.main_state_valid);
    assert(s_observers == earlier);

    new_config.volume = 44;
    assert(!z906_state_apply_verified_config_for_epoch(first_epoch, first_stream, &new_config));
    new_config.volume = 31;
    assert(z906_state_apply_verified_config_for_epoch(first_epoch, first_stream, &new_config));
    assert(s_observed_coherent && s_observers == earlier + 1);
    assert(z906_state_is_main_valid());
    assert(z906_state_get_main_status_time_us() == s_now);

    /* Stale power/mute echoes cannot cross the shared RX generation. */
    int count_before_echo = s_observers;
    assert(!z906_state_apply_amp_echo_for_generations(first_epoch, first_stream - 1,
                                                      0x36));
    z906_state_t after_echo;
    z906_state_get(&after_echo);
    assert(after_echo.power && after_echo.volume == 31);
    assert(s_observers == count_before_echo);
    s_expected_volume = 32;
    assert(z906_state_apply_amp_echo_for_generations(first_epoch, first_stream,
                                                     0x08)); /* master +1 */
    z906_state_get(&after_echo);
    assert(after_echo.volume == 32 && after_echo.power);
    assert(!z906_state_apply_amp_echo_for_generations(first_epoch, first_stream - 1,
                                                      0x20)); /* stale mute */
    z906_state_get(&after_echo);
    assert(after_echo.mute);

    /* Last dropped RX byte then SILENCE must revoke matched OTA status now. */
    z906_state_note_matched_status_reply(s_now);
    assert(z906_state_get_matched_status_reply_time_us() == s_now);
    int before_drop = s_observers;
    uint32_t next_stream = z906_state_note_amp_rx_loss(first_epoch);
    assert(next_stream == first_stream + 1);
    assert(s_observers == before_drop + 1);
    assert(!z906_state_is_main_valid());
    assert(z906_state_get_matched_status_reply_time_us() == 0);
    assert(z906_state_get_main_status_time_us() == 0);
    assert(!z906_state_apply_verified_config_for_epoch(first_epoch, first_stream,
                                                        &new_config));
    assert(!z906_state_apply_amp_echo_for_generations(first_epoch, first_stream,
                                                       0x36));
    s_expected_volume = 31;
    assert(z906_state_apply_verified_config_for_epoch(first_epoch, next_stream,
                                                       &new_config));
    assert(z906_state_is_main_valid());

    z906_state_set_cable_status(false);
    z906_state_set_cable_status(true);
    uint32_t second_epoch = z906_state_get_cable_epoch();
    assert(second_epoch == first_epoch + 2);
    int another = s_observers;
    assert(!z906_state_apply_verified_config_for_epoch(first_epoch, first_stream, &new_config));
    z906_state_t current;
    z906_state_get(&current);
    assert(current.volume == 31 && !current.main_state_valid);
    assert(s_observers == another && z906_state_get_main_status_time_us() == 0);

    s_now += 1000000;
    assert(z906_state_apply_verified_config_for_epoch(second_epoch,
               z906_state_get_amp_rx_stream_epoch(), &new_config));
    assert(z906_state_get_main_status_time_us() == s_now);

    /* G-03: other decoded frames cannot resurrect state across RX loss. */
    z906_state_init();
    z906_state_set_cable_status(true);
    uint32_t live_cable = z906_state_get_cable_epoch();
    uint32_t live_rx = z906_state_get_amp_rx_stream_epoch();
    int8_t temps[3] = { 22, 23, 24 };
    int start_observers = s_observers;
    assert(z906_state_apply_temperature_for_generations(live_cable, live_rx, temps));
    z906_state_t temperature_only;
    z906_state_get(&temperature_only);
    assert(temperature_only.temp_valid && !temperature_only.main_state_valid);
    assert(s_observers == start_observers + 1);
    (void)z906_state_note_amp_rx_loss(live_cable);
    z906_state_get(&temperature_only);
    assert(!temperature_only.temp_valid && !temperature_only.main_state_valid);
    assert(s_observers == start_observers + 2); /* G-04: temp-only observer */
    int drop_observers = s_observers;
    assert(!z906_state_apply_temperature_for_generations(live_cable, live_rx, temps));
    assert(!z906_state_apply_idle_for_generations(live_cable, live_rx, 600));
    assert(!z906_state_apply_input_level_for_generations(live_cable, live_rx, 777));
    z906_state_get(&temperature_only);
    assert(!temperature_only.temp_valid && temperature_only.idle_seconds == 0);
    assert(temperature_only.input_signal_level == 0 && s_observers == drop_observers);
    uint32_t next_rx = z906_state_get_amp_rx_stream_epoch();
    assert(z906_state_apply_temperature_for_generations(live_cable, next_rx, temps));
    assert(z906_state_apply_idle_for_generations(live_cable, next_rx, 600));
    assert(z906_state_apply_input_level_for_generations(live_cable, next_rx, 777));
    z906_state_get(&temperature_only);
    assert(temperature_only.temp_valid && temperature_only.idle_seconds == 600);
    assert(temperature_only.input_signal_level == 777);
    z906_state_set_cable_status(false);
    assert(!z906_state_apply_temperature_for_generations(live_cable, next_rx, temps));

    puts("PASS: production state manager RX epochs, temp-only observer and telemetry recovery");
}
"""

def main():
    with TemporaryDirectory(prefix="conductor-state-epoch-") as tmp:
        root = Path(tmp)
        for name, source in HEADERS.items():
            file = root / name
            file.parent.mkdir(parents=True, exist_ok=True)
            file.write_text(source, encoding="utf-8")
        test = root / "state_epoch.c"
        binary = root / "state_epoch"
        test.write_text(HARNESS, encoding="utf-8")
        subprocess.run([
            "cc", "-std=gnu11", "-Wall", "-Wextra", "-Werror",
            "-I", str(root),
            "-I", str(ROOT / "src/core/state_manager"),
            "-I", str(ROOT / "src/protocol/commands"),
            str(SOURCE), str(test), "-o", str(binary)
        ], check=True, cwd=ROOT)
        subprocess.run([str(binary)], check=True, cwd=ROOT)

if __name__ == "__main__":
    main()
