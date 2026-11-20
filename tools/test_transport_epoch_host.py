#!/usr/bin/env python3
"""Compile production UART transport and inject cable-epoch / EventBus drops.

Mocked UART and FreeRTOS task scheduling; no real hardware or NVS is modified.
"""
from pathlib import Path
from tempfile import TemporaryDirectory
import subprocess

ROOT = Path(__file__).resolve().parents[1]

HEADERS = {
    "esp_timer.h": """
#pragma once
#include <stdint.h>
int64_t esp_timer_get_time(void);
""",
    "esp_err.h": """
#pragma once
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_ERR_NO_MEM 0x101
#define ESP_ERR_TIMEOUT 0x107
""",
    "esp_log.h": """
#pragma once
void host_log_never_called(const char *, ...);
#define ESP_LOGI(tag, fmt, ...) do { (void)(tag); if (0) host_log_never_called(fmt, ##__VA_ARGS__); } while (0)
#define ESP_LOGE(tag, fmt, ...) do { (void)(tag); if (0) host_log_never_called(fmt, ##__VA_ARGS__); } while (0)
#define ESP_LOGW(tag, fmt, ...) do { (void)(tag); if (0) host_log_never_called(fmt, ##__VA_ARGS__); } while (0)
""",
    "freertos/FreeRTOS.h": """
#pragma once
#include <stdint.h>
#include <stdbool.h>
typedef uint32_t TickType_t;
typedef uint32_t StackType_t;
typedef unsigned UBaseType_t;
typedef int BaseType_t;
#define pdTRUE 1
#define pdMS_TO_TICKS(ms) (ms)
""",
    "freertos/queue.h": "#pragma once\n",
    "freertos/task.h": """
#pragma once
#include "freertos/FreeRTOS.h"
typedef void *TaskHandle_t;
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
    "uart_hal.h": """
#pragma once
#include <stdint.h>
#include <stddef.h>
#include "freertos/FreeRTOS.h"
int z906_uart_hal_read_console(uint8_t *, size_t, TickType_t);
int z906_uart_hal_read_amp(uint8_t *, size_t, TickType_t);
void z906_uart_hal_flush_amp_rx(void);
""",
    "wdt_manager.h": """
#pragma once
#include <stdbool.h>
bool wdt_manager_register_task(void);
void wdt_manager_reset(void);
""",
    "z906_state.h": """
#pragma once
#include <stdint.h>
uint32_t z906_state_get_cable_epoch(void);
uint32_t z906_state_get_amp_rx_stream_epoch(void);
uint32_t z906_state_note_amp_rx_loss(uint32_t);
"""
}

HARNESS = r"""
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <setjmp.h>
#include "z906_transport.h"
#include "event_bus.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "uart_hal.h"

static jmp_buf s_stop;
static uint32_t s_epoch, s_stream;
static unsigned s_cycles, s_flushes, s_rx_reads, s_posts, s_losses;
static bool s_status_valid = true;
static z906_event_t s_recorded[4];
static bool s_registered;

int64_t esp_timer_get_time(void) { return 1000000LL + (int64_t)s_cycles * 1000000LL; }
uint32_t z906_state_get_cable_epoch(void) { return s_epoch; }
uint32_t z906_state_get_amp_rx_stream_epoch(void) { return s_stream; }
uint32_t z906_state_note_amp_rx_loss(uint32_t epoch) {
    assert(epoch == s_epoch);
    ++s_losses;
    s_status_valid = false; /* No later RX is needed to revoke health. */
    return ++s_stream;
}
bool wdt_manager_register_task(void) { s_registered = true; return true; }
void wdt_manager_reset(void) { assert(s_registered); }
int z906_uart_hal_read_console(uint8_t *b, size_t n, TickType_t t) {
    (void)b; (void)n; (void)t; return 0;
}
int z906_uart_hal_read_amp(uint8_t *b, size_t n, TickType_t t) {
    assert(n == 1); (void)t; *b = 0xAA; ++s_rx_reads; return 1;
}
void z906_uart_hal_flush_amp_rx(void) { ++s_flushes; }
bool event_bus_post(const z906_event_t *e, TickType_t wait) {
    (void)wait;
    assert(e->type == EVENT_AMP_RX_BYTE && s_posts < 4);
    s_recorded[s_posts] = *e;
    return (++s_posts != 3); /* Third RX byte is lost in a full EventBus. */
}
SemaphoreHandle_t xSemaphoreCreateBinaryStatic(StaticSemaphore_t *b) {
    (void)b; return (SemaphoreHandle_t)1;
}
BaseType_t xSemaphoreGive(SemaphoreHandle_t s) { assert(s); return pdTRUE; }
BaseType_t xSemaphoreTake(SemaphoreHandle_t s, TickType_t t) {
    (void)t; assert(s); return pdTRUE;
}
TaskHandle_t xTaskCreateStatic(void (*fn)(void *), const char *name,
                              unsigned stack, void *arg, UBaseType_t priority,
                              StackType_t *stackbuf, StaticTask_t *tcb) {
    assert(fn && name && stack && priority && stackbuf && tcb);
    fn(arg);
    return (TaskHandle_t)1;
}
void vTaskDelay(TickType_t delay) {
    assert(delay > 0);
    ++s_cycles;
    if (s_cycles == 1) { s_epoch = 1; ++s_stream; }
    if (s_cycles == 2) { s_epoch = 2; ++s_stream; }
    if (s_cycles == 3) {
        /* Third post was lost: last-RX-drop proof must already be revoked. */
        assert(s_losses == 1 && !s_status_valid);
    }
    if (s_cycles == 4) longjmp(s_stop, 1);
}
int main(void) {
    if (setjmp(s_stop) == 0) {
        (void)z906_transport_start();
        assert(!"the mock task should exit through the test jump");
    }
    assert(s_cycles == 4 && s_rx_reads == 4 && s_posts == 4);
    assert(s_flushes == 2 && s_losses == 1 && !s_status_valid);
    assert(s_recorded[0].cable_epoch == 0);
    assert(s_recorded[1].cable_epoch == 1);
    assert(s_recorded[2].cable_epoch == 2);
    assert(s_recorded[3].cable_epoch == 2);
    assert(s_recorded[1].rx_stream_epoch == s_recorded[0].rx_stream_epoch + 1);
    assert(s_recorded[2].rx_stream_epoch == s_recorded[1].rx_stream_epoch + 1);
    assert(s_recorded[3].rx_stream_epoch == s_recorded[2].rx_stream_epoch + 1);
    puts("PASS: real UART transport immediate last-drop invalidation and epochs");
}
"""

def main():
    with TemporaryDirectory(prefix="conductor-transport-epoch-") as tmp:
        root = Path(tmp)
        for name, content in HEADERS.items():
            file = root / name
            file.parent.mkdir(parents=True, exist_ok=True)
            file.write_text(content, encoding="utf-8")
        harness = root / "transport_epoch.c"
        binary = root / "transport_epoch"
        harness.write_text(HARNESS, encoding="utf-8")
        subprocess.run([
            "cc", "-std=gnu11", "-Wall", "-Wextra", "-Werror",
            "-I", str(root),
            "-I", str(ROOT / "src/communication/z906_transport"),
            "-I", str(ROOT / "src/core/event_bus"),
            str(ROOT / "src/communication/z906_transport/z906_transport.c"),
            str(harness), "-o", str(binary)
        ], check=True, cwd=ROOT)
        subprocess.run([str(binary)], check=True, cwd=ROOT)

if __name__ == "__main__":
    main()
