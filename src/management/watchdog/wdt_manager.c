/**
 * @file wdt_manager.c
 * @brief Implementation of the Task Watchdog Timer manager with diagnostic telemetry.
 */

#include "wdt_manager.h"
#include "esp_task_wdt.h"
#include "esp_log.h"
#include "esp_system.h"

static const char *TAG = "WDT_MGR";

/** @brief Flag indicating if TWDT has been successfully configured and activated */
static bool s_wdt_initialized = false;

/** @brief Configured TWDT timeout duration in seconds */
static uint32_t s_wdt_timeout_seconds = 0;

bool wdt_manager_init(uint32_t timeout_seconds)
{
    esp_task_wdt_config_t twdt_config = {
        .timeout_ms = timeout_seconds * 1000,
        .idle_core_mask = 0,
        .trigger_panic = true
    };

    esp_err_t err = esp_task_wdt_reconfigure(&twdt_config);
    if (err != ESP_OK) {
        err = esp_task_wdt_init(&twdt_config);
    }

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize TWDT (err: 0x%x)", err);
        s_wdt_initialized = false;
        s_wdt_timeout_seconds = 0;
        return false;
    }

    s_wdt_initialized = true;
    s_wdt_timeout_seconds = timeout_seconds;

    ESP_LOGI(TAG, "Task Watchdog Timer configured (Timeout: %lu s, Last Reset: %s)",
             (unsigned long)timeout_seconds, wdt_manager_get_reset_reason_str());
    return true;
}

bool wdt_manager_register_task(void)
{
    esp_err_t err = esp_task_wdt_add(NULL);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "Failed to register task with TWDT (err: 0x%x)", err);
        return false;
    }
    return true;
}

void wdt_manager_reset(void)
{
    esp_task_wdt_reset();
}

bool wdt_manager_is_initialized(void)
{
    return s_wdt_initialized;
}

uint32_t wdt_manager_get_timeout_seconds(void)
{
    return s_wdt_timeout_seconds;
}

esp_reset_reason_t wdt_manager_get_reset_reason(void)
{
    return esp_reset_reason();
}

const char *wdt_manager_get_reset_reason_str(void)
{
    switch (esp_reset_reason()) {
        case ESP_RST_POWERON:
            return "POWERON (Normal power-on boot)";
        case ESP_RST_EXT:
            return "EXT_PIN (External pin reset)";
        case ESP_RST_SW:
            return "SW_RESET (Software restart via esp_restart)";
        case ESP_RST_PANIC:
            return "PANIC (Software crash / exception panic)";
        case ESP_RST_INT_WDT:
            return "INT_WDT (Interrupt Watchdog timeout reset)";
        case ESP_RST_TASK_WDT:
            return "TASK_WDT (Task Watchdog Timer timeout reset)";
        case ESP_RST_WDT:
            return "RTC_WDT (Other RTC/Hardware Watchdog reset)";
        case ESP_RST_DEEPSLEEP:
            return "DEEPSLEEP (Wakeup from deep sleep)";
        case ESP_RST_BROWNOUT:
            return "BROWNOUT (Voltage drop / brownout reset)";
        case ESP_RST_SDIO:
            return "SDIO (Reset over SDIO interface)";
        default:
            return "UNKNOWN (Unrecognized reset reason)";
    }
}

bool wdt_manager_was_watchdog_reset(void)
{
    esp_reset_reason_t reason = esp_reset_reason();
    return (reason == ESP_RST_TASK_WDT ||
            reason == ESP_RST_INT_WDT ||
            reason == ESP_RST_WDT);
}
