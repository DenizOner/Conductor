#!/usr/bin/env python3
"""Compile production OTA health logic with isolated host stubs and exercise decisions.

Host-only: no ESP32, RF, flash, SDK or repository-side generated files are touched.
The real ota_health.c is compiled; mocks implement only its external dependencies.
"""
from pathlib import Path
from tempfile import TemporaryDirectory
import subprocess

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "src/management/ota/ota_health.c"

HEADERS = {
    "esp_err.h": """
#pragma once
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL (-1)
const char *esp_err_to_name(esp_err_t err);
""",
    "esp_ota_ops.h": """
#pragma once
#include <stdbool.h>
#include <string.h>
#include "esp_err.h"
typedef struct { char label[16]; } esp_partition_t;
typedef enum { ESP_OTA_IMG_PENDING_VERIFY = 1, ESP_OTA_IMG_VALID = 2 } esp_ota_img_states_t;
const esp_partition_t *esp_ota_get_running_partition(void);
const esp_partition_t *esp_ota_get_last_invalid_partition(void);
esp_err_t esp_ota_get_state_partition(const esp_partition_t *, esp_ota_img_states_t *);
bool esp_ota_check_rollback_is_possible(void);
esp_err_t esp_ota_mark_app_valid_cancel_rollback(void);
esp_err_t esp_ota_mark_app_invalid_rollback_and_reboot(void);
""",
    "esp_log.h": """
#pragma once
#include <stdio.h>
#define ESP_LOGI(tag, fmt, ...) do { (void)(tag); fprintf(stderr, fmt "\\n", ##__VA_ARGS__); } while (0)
#define ESP_LOGW(tag, fmt, ...) do { (void)(tag); fprintf(stderr, fmt "\\n", ##__VA_ARGS__); } while (0)
#define ESP_LOGE(tag, fmt, ...) do { (void)(tag); fprintf(stderr, fmt "\\n", ##__VA_ARGS__); } while (0)
""",
    "esp_timer.h": """
#pragma once
#include <stdint.h>
int64_t esp_timer_get_time(void);
""",
    "esp_system.h": """
#pragma once
void esp_restart(void);
""",
    "freertos/FreeRTOS.h": """
#pragma once
typedef int portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(m) ((void)(m))
#define portEXIT_CRITICAL(m) ((void)(m))
""",
    "freertos/task.h": """
#pragma once
#define taskSCHEDULER_RUNNING 1
int xTaskGetSchedulerState(void);
""",
    "wdt_manager.h": """
#pragma once
#include <stdbool.h>
bool wdt_manager_is_initialized(void);
bool wdt_manager_is_current_task_registered(void);
bool wdt_manager_was_watchdog_reset(void);
""",
    "nvs_manager.h": """
#pragma once
#include <stdbool.h>
bool nvs_manager_check_read_health(void);
""",
    "zigbee_native_stack.h": """
#pragma once
typedef enum { ZB_STACK_STATE_UNINITIALIZED, ZB_STACK_STATE_RUNNING } zb_stack_state_t;
zb_stack_state_t zigbee_native_stack_get_state(void);
""",
    "zigbee_device_model.h": """
#pragma once
#include <stdbool.h>
bool zigbee_device_model_is_registered(void);
""",
    "z906_state.h": """
#pragma once
#include <stdbool.h>
bool z906_state_is_main_valid(void);
typedef struct {
    bool main_valid;
    uint32_t cable_epoch;
    uint32_t rx_stream_epoch;
    int64_t matched_status_us;
} z906_ota_status_snapshot_t;
bool z906_state_get_ota_status_snapshot(z906_ota_status_snapshot_t *);
int64_t z906_state_get_main_status_time_us(void);
int64_t z906_state_get_matched_status_reply_time_us(void);
""",
    "led_notification_manager.h": """
#pragma once
#include <stdbool.h>
void led_notification_post_cmd_result(bool);
""",
    "ota_client.h": """
#pragma once
void ota_client_note_health_failure(const char *);
""",
}

HARNESS = r"""
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stddef.h>
#include "ota_health.h"
#include "esp_ota_ops.h"
#include "zigbee_native_stack.h"
#include "z906_state.h"

static int64_t s_now = 1000000;
static bool s_z906_ok;
static int64_t s_status_at_us;
static uint32_t s_rx_epoch;
static bool s_zigbee_ok;
static bool s_fallback_ok;
static esp_err_t s_rollback_result = ESP_OK;
static int s_valid_count;
static bool s_inject_rollback_while_sdk_runs;
static int s_rollback_count;
static int s_failure_count;
static esp_partition_t s_running = { "ota_0" };

int64_t esp_timer_get_time(void) { return s_now; }
int xTaskGetSchedulerState(void) { return 1; }
bool wdt_manager_is_initialized(void) { return true; }
bool wdt_manager_was_watchdog_reset(void) { return false; }
bool wdt_manager_is_current_task_registered(void) { return true; }
static bool s_nvs_ok = true;
bool nvs_manager_check_read_health(void) { return s_nvs_ok; }
zb_stack_state_t zigbee_native_stack_get_state(void) {
    return s_zigbee_ok ? ZB_STACK_STATE_RUNNING : ZB_STACK_STATE_UNINITIALIZED;
}
bool zigbee_device_model_is_registered(void) { return s_zigbee_ok; }
bool z906_state_is_main_valid(void) { return s_z906_ok; }
bool z906_state_get_ota_status_snapshot(z906_ota_status_snapshot_t *out) {
    if (!out) return false;
    out->main_valid = s_z906_ok;
    out->cable_epoch = 1;
    out->rx_stream_epoch = s_rx_epoch;
    out->matched_status_us = s_z906_ok ? s_status_at_us : 0;
    return true;
}
int64_t z906_state_get_main_status_time_us(void) { return s_z906_ok ? s_status_at_us : 0; }
int64_t z906_state_get_matched_status_reply_time_us(void) { return s_z906_ok ? s_status_at_us : 0; }
void led_notification_post_cmd_result(bool success) { (void)success; }
void ota_client_note_health_failure(const char *s) { (void)s; ++s_failure_count; }
const char *esp_err_to_name(esp_err_t err) { (void)err; return "mock"; }
const esp_partition_t *esp_ota_get_running_partition(void) { return &s_running; }
const esp_partition_t *esp_ota_get_last_invalid_partition(void) { return NULL; }
esp_err_t esp_ota_get_state_partition(const esp_partition_t *p, esp_ota_img_states_t *state) {
    (void)p; *state = ESP_OTA_IMG_PENDING_VERIFY; return ESP_OK;
}
bool esp_ota_check_rollback_is_possible(void) { return s_fallback_ok; }
esp_err_t esp_ota_mark_app_valid_cancel_rollback(void) {
    ++s_valid_count;
    if (s_inject_rollback_while_sdk_runs) {
        ota_health_trigger_rollback("Manual rollback during SDK commit");
    }
    return ESP_OK;
}
esp_err_t esp_ota_mark_app_invalid_rollback_and_reboot(void) { ++s_rollback_count; return s_rollback_result; }
void esp_restart(void) { assert(!"unexpected forced reboot"); }

static void reset_case(void) {
    ota_health_reset_for_test();
    s_now = 1000000;
    s_z906_ok = false;
    s_status_at_us = 0;
    s_rx_epoch = 0;
    s_zigbee_ok = true;
    s_nvs_ok = true;
    s_fallback_ok = true;
    s_rollback_result = ESP_OK;
    s_valid_count = 0;
    s_inject_rollback_while_sdk_runs = false;
    s_rollback_count = 0;
    s_failure_count = 0;
    assert(ota_health_init());
    ota_health_report_twdt_observation(true);
    assert(ota_health_is_pending_verify());
}
static void advance(unsigned seconds) {
    s_now += (int64_t)seconds * 1000000;
    ota_health_process_cycle(1);
}
static void report_verified_status(void) {
    s_z906_ok = true;
    s_status_at_us = s_now;
}
static void drop_after_pass_before_mark_valid(void) {
    /* Simulates RX owner preempting finalization after the criteria PASS. */
    s_z906_ok = false;
    s_status_at_us = 0;
    ++s_rx_epoch;
}
static void change_epoch_after_pass(void) {
    /* Even a stale unchanged status timestamp cannot cross generations. */
    ++s_rx_epoch;
}
static void manual_rollback_during_admission(void) {
    ota_health_trigger_rollback("Manual CLI rollback while final proof is checked");
}
static void rx_drop_between_proof_and_sdk_handoff(void) {
    /* Preemption after first snapshot, before the final SDK claim. */
    drop_after_pass_before_mark_valid();
}
int main(void) {
    /* 15 s with no verified amplifier frame must roll back. */
    reset_case();
    advance(15);
    assert(s_valid_count == 0 && s_rollback_count == 1 && s_failure_count == 1);

    /* Native stack running, unjoined, and valid Z906 status permits commit at 15 s. */
    reset_case();
    advance(11);
    report_verified_status();
    advance(3);
    assert(s_valid_count == 0);
    advance(1);
    assert(s_valid_count == 1 && s_rollback_count == 0 && ota_health_is_validated());

    /* Not merely "network state != commissioning failed": stack must run. */
    reset_case();
    advance(11);
    report_verified_status();
    s_zigbee_ok = false;
    advance(4);
    assert(s_valid_count == 0 && s_rollback_count == 1);

    /* The current telemetry task must report a successful TWDT feed. */
    reset_case();
    advance(11);
    report_verified_status();
    ota_health_report_twdt_observation(false);
    advance(4);
    assert(s_valid_count == 0 && s_rollback_count == 1);

    /* Genuine NVS read errors must not approve firmware. */
    reset_case();
    advance(11);
    report_verified_status();
    s_nvs_ok = false;
    advance(4);
    assert(s_valid_count == 0 && s_rollback_count == 1);

    /* E19F-01: matched status at t=11, LAST dropped RX byte at t=14,
     * then silence. Production state manager revokes both main validity
     * and matched timestamp synchronously; this stub models that revocation. */
    reset_case();
    advance(11);
    report_verified_status();
    advance(3);
    s_z906_ok = false;
    s_status_at_us = 0;
    advance(1);
    assert(s_valid_count == 0 && s_rollback_count == 1);
    assert(!ota_health_is_validated());

    /* H-05: PASS at 15s, then LAST RX drop after finalization claim but
     * BEFORE irreversible SDK approval. No permanent VALID is permitted. */
    reset_case();
    advance(11);
    report_verified_status();
    ota_health_set_before_final_admission_fn_for_test(drop_after_pass_before_mark_valid);
    advance(4);
    assert(s_valid_count == 0 && s_rollback_count == 1 && s_failure_count == 1);
    assert(!ota_health_is_validated());

    /* An RX epoch change without changing the test timestamp also fails. */
    reset_case();
    advance(11);
    report_verified_status();
    ota_health_set_before_final_admission_fn_for_test(change_epoch_after_pass);
    advance(4);
    assert(s_valid_count == 0 && s_rollback_count == 1);

    /* H-05: if no fallback exists, do not flash VALID or restart-loop. */
    reset_case();
    s_fallback_ok = false;
    advance(11);
    report_verified_status();
    ota_health_set_before_final_admission_fn_for_test(drop_after_pass_before_mark_valid);
    advance(4);
    assert(s_valid_count == 0 && s_rollback_count == 0);
    assert(ota_health_get_recovery_state() == OTA_HEALTH_RECOVERY_NO_FALLBACK);

    /* I-01: preempt AFTER the first successful proof check; the final
     * handoff guard must reject before the irreversible SDK call. */
    reset_case();
    advance(11);
    report_verified_status();
    ota_health_set_before_sdk_handoff_fn_for_test(rx_drop_between_proof_and_sdk_handoff);
    advance(4);
    assert(s_valid_count == 0 && s_rollback_count == 1 && !ota_health_is_validated());

    /* I-02: manual rollback during the cancellable admission is latched and
     * wins; no marked-valid call and exactly one rollback action. */
    reset_case();
    advance(11);
    report_verified_status();
    ota_health_set_before_sdk_handoff_fn_for_test(manual_rollback_during_admission);
    advance(4);
    assert(s_valid_count == 0 && s_rollback_count == 1);
    assert(ota_health_get_recovery_state() == OTA_HEALTH_RECOVERY_REQUESTED);

    /* I-02 no-fallback: queued manual rollback never forces a reboot. */
    reset_case();
    s_fallback_ok = false;
    advance(11);
    report_verified_status();
    ota_health_set_before_sdk_handoff_fn_for_test(manual_rollback_during_admission);
    advance(4);
    assert(s_valid_count == 0 && s_rollback_count == 0);
    assert(ota_health_get_recovery_state() == OTA_HEALTH_RECOVERY_NO_FALLBACK);

    /* I-02 after SDK entry: rollback request is diagnosed TOO_LATE, never
     * silently ignored, and a second irreversible OTA call does not race. */
    reset_case();
    advance(11);
    report_verified_status();
    s_inject_rollback_while_sdk_runs = true;
    advance(4);
    assert(s_valid_count == 1 && s_rollback_count == 0 && ota_health_is_validated());
    assert(ota_health_get_recovery_state() == OTA_HEALTH_RECOVERY_REQUEST_TOO_LATE);

    /* No fallback image: no forced restart, no false validation or repeat abort. */
    reset_case();
    s_fallback_ok = false;
    advance(15);
    advance(2);
    assert(s_valid_count == 0 && s_rollback_count == 0);
    assert(ota_health_is_pending_verify());
    assert(ota_health_get_recovery_state() == OTA_HEALTH_RECOVERY_NO_FALLBACK);

    /* A rejected rollback API must be reported without repeated reboots. */
    reset_case();
    s_rollback_result = ESP_FAIL;
    advance(15);
    advance(1);
    assert(s_rollback_count == 1 && s_valid_count == 0);
    assert(ota_health_get_recovery_state() == OTA_HEALTH_RECOVERY_ROLLBACK_ERROR);

    /* A stalled scheduler must not shorten or stretch the observation window. */
    reset_case();
    advance(12);
    report_verified_status();
    advance(4);
    assert(s_valid_count == 1 && s_rollback_count == 0);

    /* Negative elapsed time must not allow image approval. */
    reset_case();
    advance(1);
    report_verified_status();
    s_now -= 2000000;
    ota_health_process_cycle(1000);
    assert(s_valid_count == 0 && s_rollback_count == 0);

    /* A lone first-second CONFIG reply must become stale before the decision. */
    reset_case();
    advance(1);
    report_verified_status();
    advance(14);
    assert(s_valid_count == 0 && s_rollback_count == 1);

    /* A response before the pending window is not usable OTA evidence. */
    reset_case();
    s_z906_ok = true;
    s_status_at_us = s_now - 500000;
    advance(15);
    assert(s_valid_count == 0 && s_rollback_count == 1);

    puts("PASS: OTA runtime health C host harness (19 scenarios)");
    return 0;
}
"""

def main():
    with TemporaryDirectory(prefix="conductor-ota-health-") as tmp:
        root = Path(tmp)
        for name, content in HEADERS.items():
            file = root / name
            file.parent.mkdir(parents=True, exist_ok=True)
            file.write_text(content, encoding="utf-8")
        harness = root / "ota_health_host.c"
        harness.write_text(HARNESS, encoding="utf-8")
        binary = root / "ota_health_host"
        cmd = [
            "cc", "-std=gnu11", "-Wall", "-Wextra", "-Werror",
            "-I", str(root), "-I", str(SOURCE.parent),
            str(SOURCE), str(harness), "-o", str(binary),
        ]
        subprocess.run(cmd, cwd=ROOT, check=True)
        subprocess.run([str(binary)], cwd=ROOT, check=True)

if __name__ == "__main__":
    main()
