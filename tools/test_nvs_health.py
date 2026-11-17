#!/usr/bin/env python3
"""Compile real nvs_manager.c and verify read-only OTA NVS error classification.

All SDK NVS calls are host mocks; no persistent storage is accessed.
"""
from pathlib import Path
from tempfile import TemporaryDirectory
import subprocess

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "src/management/nvs/nvs_manager.c"

HEADERS = {
    "esp_err.h": """
#pragma once
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_ERR_NVS_NOT_FOUND 0x1102
#define ESP_ERR_NVS_TYPE_MISMATCH 0x1103
#define ESP_ERR_NVS_INVALID_LENGTH 0x110c
#define ESP_ERR_NVS_NO_FREE_PAGES 0x110d
#define ESP_ERR_NVS_NEW_VERSION_FOUND 0x1110
#define ESP_ERR_NVS_NOT_INITIALIZED 0x1101
#define ESP_ERROR_CHECK(x) ((void)(x))
""",
    "nvs_flash.h": """
#pragma once
#include "esp_err.h"
esp_err_t nvs_flash_init(void);
esp_err_t nvs_flash_erase(void);
""",
    "nvs.h": """
#pragma once
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
typedef unsigned nvs_handle_t;
typedef enum { NVS_READONLY, NVS_READWRITE } nvs_open_mode_t;
esp_err_t nvs_open(const char *, nvs_open_mode_t, nvs_handle_t *);
esp_err_t nvs_get_u8(nvs_handle_t, const char *, uint8_t *);
esp_err_t nvs_get_u16(nvs_handle_t, const char *, uint16_t *);
esp_err_t nvs_get_blob(nvs_handle_t, const char *, void *, size_t *);
esp_err_t nvs_set_u8(nvs_handle_t, const char *, uint8_t);
esp_err_t nvs_set_u16(nvs_handle_t, const char *, uint16_t);
esp_err_t nvs_set_blob(nvs_handle_t, const char *, const void *, size_t);
esp_err_t nvs_erase_all(nvs_handle_t);
esp_err_t nvs_erase_key(nvs_handle_t, const char *);
esp_err_t nvs_commit(nvs_handle_t);
void nvs_close(nvs_handle_t);
""",
    "esp_log.h": """
#pragma once
#define ESP_LOGE(tag, ...) ((void)(tag))
#define ESP_LOGW(tag, ...) ((void)(tag))
#define ESP_LOGI(tag, ...) ((void)(tag))
#define ESP_LOGD(tag, ...) ((void)(tag))
"""
}
TEST = r"""
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include "esp_err.h"
#include "nvs.h"
#include "nvs_manager.h"

static esp_err_t s_open_result;
static esp_err_t s_read_result;
static int s_close_count;
static int s_read_count;
static int s_open_count;
static int s_write_count;
esp_err_t nvs_open(const char *ns, nvs_open_mode_t mode, nvs_handle_t *handle) {
    assert(ns != NULL && mode == NVS_READONLY);
    ++s_open_count;
    *handle = 23;
    return s_open_result;
}
esp_err_t nvs_get_u8(nvs_handle_t handle, const char *key, uint8_t *out) {
    assert(handle == 23 && key != NULL);
    ++s_read_count;
    *out = 1;
    return s_read_result;
}
void nvs_close(nvs_handle_t handle) { assert(handle == 23); ++s_close_count; }

static void check(esp_err_t open, esp_err_t read, bool expected, int closes) {
    s_open_result = open;
    s_read_result = read;
    s_close_count = s_read_count = s_open_count = s_write_count = 0;
    assert(nvs_manager_check_read_health() == expected);
    assert(s_open_count == 1);
    assert(s_read_count == (open == ESP_OK ? 1 : 0));
    assert(s_close_count == closes);
    assert(s_write_count == 0);
}
int main(void) {
    check(ESP_OK, ESP_OK, true, 1);
    check(ESP_OK, ESP_ERR_NVS_NOT_FOUND, true, 1);
    check(ESP_ERR_NVS_NOT_FOUND, ESP_OK, true, 0);
    check(ESP_ERR_NVS_NOT_INITIALIZED, ESP_OK, false, 0);
    check(ESP_OK, ESP_ERR_NVS_TYPE_MISMATCH, false, 1);
    check(ESP_OK, ESP_ERR_NVS_INVALID_LENGTH, false, 1);
    puts("PASS: real NVS read-only health probe (6 error-path cases)");
}
"""

def main():
    with TemporaryDirectory(prefix="conductor-nvs-health-") as tmp:
        path = Path(tmp)
        for name, content in HEADERS.items():
            (path / name).write_text(content, encoding="utf-8")
        harness = path / "nvs_probe.c"
        binary = path / "nvs_probe"
        harness.write_text(TEST, encoding="utf-8")
        subprocess.run([
            "cc", "-std=gnu11", "-Wall", "-Wextra", "-Werror",
            "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections",
            "-I", str(path), "-I", str(SOURCE.parent),
            str(SOURCE), str(harness), "-o", str(binary)
        ], check=True, cwd=ROOT)
        subprocess.run([str(binary)], check=True, cwd=ROOT)

if __name__ == "__main__":
    main()
