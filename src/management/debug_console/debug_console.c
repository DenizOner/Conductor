/**
 * @file debug_console.c
 * @brief Implementation of the centralized Debug Console and logging control service.
 */

#include "debug_console.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <stdio.h>
#include <string.h>
#include <strings.h>

static const char *TAG = "DEBUG_CONSOLE";

/** @brief Flag indicating if Debug Console has been initialized */
static bool s_debug_console_initialized = false;

/** @brief Current global log level applied to wildcard ("*") */
static esp_log_level_t s_global_log_level = ESP_LOG_INFO;

/** @brief Static storage for module tag filters */
static debug_tag_filter_t s_tag_filters[DEBUG_CONSOLE_MAX_TAG_FILTERS];

/** @brief Active count of registered tag filters */
static size_t s_tag_filter_count = 0;

/** @brief Static FreeRTOS mutex ensuring thread-safe filter operations */
static SemaphoreHandle_t s_filter_mutex = NULL;
static StaticSemaphore_t s_filter_mutex_buffer;

bool debug_console_init(void)
{
    if (s_filter_mutex == NULL) {
        s_filter_mutex = xSemaphoreCreateMutexStatic(&s_filter_mutex_buffer);
        if (s_filter_mutex == NULL) {
            ESP_LOGE(TAG, "Failed to create Debug Console static mutex");
            return false;
        }
    }

    s_global_log_level = ESP_LOG_INFO;
    s_tag_filter_count = 0;
    memset(s_tag_filters, 0, sizeof(s_tag_filters));

    /* Apply default global log level to ESP-IDF logging engine */
    esp_log_level_set("*", s_global_log_level);

    s_debug_console_initialized = true;
    ESP_LOGI(TAG, "Debug Console service initialized successfully (Global Level: %s)",
             debug_console_level_to_str(s_global_log_level));
    return true;
}

bool debug_console_is_initialized(void)
{
    return s_debug_console_initialized;
}

const char *debug_console_level_to_str(esp_log_level_t level)
{
    switch (level) {
        case ESP_LOG_NONE:    return "none";
        case ESP_LOG_ERROR:   return "error";
        case ESP_LOG_WARN:    return "warn";
        case ESP_LOG_INFO:    return "info";
        case ESP_LOG_DEBUG:   return "debug";
        case ESP_LOG_VERBOSE: return "verbose";
        default:              return "unknown";
    }
}

esp_log_level_t debug_console_str_to_level(const char *str)
{
    if (str == NULL) {
        return ESP_LOG_NONE;
    }

    if (strcasecmp(str, "none") == 0)    return ESP_LOG_NONE;
    if (strcasecmp(str, "error") == 0)   return ESP_LOG_ERROR;
    if (strcasecmp(str, "warn") == 0)    return ESP_LOG_WARN;
    if (strcasecmp(str, "warning") == 0) return ESP_LOG_WARN;
    if (strcasecmp(str, "info") == 0)    return ESP_LOG_INFO;
    if (strcasecmp(str, "debug") == 0)   return ESP_LOG_DEBUG;
    if (strcasecmp(str, "verbose") == 0) return ESP_LOG_VERBOSE;

    return ESP_LOG_NONE;
}

bool debug_console_set_global_level(esp_log_level_t level)
{
    if (!s_debug_console_initialized) {
        debug_console_init();
    }

    s_global_log_level = level;
    esp_log_level_set("*", level);

    /* Re-apply custom module tag overrides */
    if (s_filter_mutex != NULL && xSemaphoreTake(s_filter_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        for (size_t i = 0; i < s_tag_filter_count; i++) {
            esp_log_level_set(s_tag_filters[i].tag, s_tag_filters[i].level);
        }
        xSemaphoreGive(s_filter_mutex);
    }

    return true;
}

esp_log_level_t debug_console_get_global_level(void)
{
    return s_global_log_level;
}

bool debug_console_set_tag_level(const char *tag, esp_log_level_t level)
{
    if (tag == NULL || strlen(tag) == 0) {
        return false;
    }

    if (!s_debug_console_initialized) {
        debug_console_init();
    }

    if (s_filter_mutex == NULL) {
        return false;
    }

    if (xSemaphoreTake(s_filter_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        return false;
    }

    /* 1. Check if tag is already in table and update */
    bool updated = false;
    for (size_t i = 0; i < s_tag_filter_count; i++) {
        if (strcmp(s_tag_filters[i].tag, tag) == 0) {
            s_tag_filters[i].level = level;
            updated = true;
            break;
        }
    }

    /* 2. If not found, insert new entry if space allows */
    if (!updated) {
        if (s_tag_filter_count < DEBUG_CONSOLE_MAX_TAG_FILTERS) {
            strncpy(s_tag_filters[s_tag_filter_count].tag, tag, DEBUG_CONSOLE_TAG_MAX_LEN - 1);
            s_tag_filters[s_tag_filter_count].tag[DEBUG_CONSOLE_TAG_MAX_LEN - 1] = '\0';
            s_tag_filters[s_tag_filter_count].level = level;
            s_tag_filter_count++;
            updated = true;
        } else {
            xSemaphoreGive(s_filter_mutex);
            ESP_LOGW(TAG, "Tag filter table is full (Max: %d)", DEBUG_CONSOLE_MAX_TAG_FILTERS);
            return false;
        }
    }

    xSemaphoreGive(s_filter_mutex);

    /* Update native ESP-IDF logging engine */
    esp_log_level_set(tag, level);
    return true;
}

esp_log_level_t debug_console_get_tag_level(const char *tag)
{
    if (tag == NULL || strlen(tag) == 0) {
        return s_global_log_level;
    }

    esp_log_level_t result = s_global_log_level;

    if (s_filter_mutex != NULL && xSemaphoreTake(s_filter_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        for (size_t i = 0; i < s_tag_filter_count; i++) {
            if (strcmp(s_tag_filters[i].tag, tag) == 0) {
                result = s_tag_filters[i].level;
                break;
            }
        }
        xSemaphoreGive(s_filter_mutex);
    }

    return result;
}

size_t debug_console_get_filter_count(void)
{
    return s_tag_filter_count;
}

bool debug_console_get_filter(size_t index, char *out_tag, size_t tag_max, esp_log_level_t *out_level)
{
    if (out_tag == NULL || out_level == NULL || tag_max == 0) {
        return false;
    }

    bool success = false;
    if (s_filter_mutex != NULL && xSemaphoreTake(s_filter_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        if (index < s_tag_filter_count) {
            strncpy(out_tag, s_tag_filters[index].tag, tag_max - 1);
            out_tag[tag_max - 1] = '\0';
            *out_level = s_tag_filters[index].level;
            success = true;
        }
        xSemaphoreGive(s_filter_mutex);
    }

    return success;
}

void debug_console_clear_tag_filters(void)
{
    if (s_filter_mutex != NULL && xSemaphoreTake(s_filter_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        s_tag_filter_count = 0;
        memset(s_tag_filters, 0, sizeof(s_tag_filters));
        xSemaphoreGive(s_filter_mutex);
    }

    /* Restore all modules to active global log level */
    esp_log_level_set("*", s_global_log_level);
}

void debug_console_print_status(void)
{
    int64_t uptime_sec = esp_timer_get_time() / 1000000;
    uint32_t hours = (uint32_t)(uptime_sec / 3600);
    uint32_t mins = (uint32_t)((uptime_sec % 3600) / 60);
    uint32_t secs = (uint32_t)(uptime_sec % 60);

    printf("\n=== Debug Console & System Diagnostics ===\n");
    printf("Status:              %s\n", s_debug_console_initialized ? "INITIALIZED" : "UNINITIALIZED");
    printf("Uptime:              %lu h %lu m %lu s (%lld seconds total)\n",
           (unsigned long)hours, (unsigned long)mins, (unsigned long)secs, (long long)uptime_sec);
    printf("Free Heap:           %lu bytes\n", (unsigned long)esp_get_free_heap_size());
    printf("Min Ever Free Heap:  %lu bytes\n", (unsigned long)esp_get_minimum_free_heap_size());
    printf("Reset Reason:        %s\n", wdt_manager_get_reset_reason_str());
    printf("Global Log Level:    %s\n", debug_console_level_to_str(s_global_log_level));

    printf("Active Tag Filters:  %lu / %d\n", (unsigned long)s_tag_filter_count, DEBUG_CONSOLE_MAX_TAG_FILTERS);
    if (s_tag_filter_count > 0) {
        for (size_t i = 0; i < s_tag_filter_count; i++) {
            printf("  [%zu] %-18s -> %s\n", i + 1, s_tag_filters[i].tag,
                   debug_console_level_to_str(s_tag_filters[i].level));
        }
    } else {
        printf("  (No tag overrides active; all tags follow global level)\n");
    }

    printf("Watchdog (TWDT):     %s (Timeout: %lu s)\n",
           wdt_manager_is_initialized() ? "ACTIVE" : "INACTIVE",
           (unsigned long)wdt_manager_get_timeout_seconds());
    printf("==========================================\n\n");
}

void debug_console_print_wdt_status(void)
{
    printf("\n=== Task Watchdog Timer (TWDT) Status ===\n");
    printf("TWDT Initialized:    %s\n", wdt_manager_is_initialized() ? "YES" : "NO");
    printf("Configured Timeout:  %lu seconds\n", (unsigned long)wdt_manager_get_timeout_seconds());
    printf("Panic Trigger:       ENABLED (Hardware core panic on timeout)\n");
    printf("Last Boot Reason:    %s\n", wdt_manager_get_reset_reason_str());
    printf("Watchdog Boot Event: %s\n", wdt_manager_was_watchdog_reset() ? "YES (Previous reset was Watchdog)" : "NO");

    printf("\nMonitored FreeRTOS Tasks in System:\n");
    printf("  1. uart_rx_task    [Priority 5] - Drains UART FIFOs (Monitored by TWDT)\n");
    printf("  2. z906_fsm_task   [Priority 4] - EventBus State Machine (Monitored by TWDT)\n");
    printf("  3. zigbee_task     [Priority 3] - Zigbee 3.0 Stack (Monitored by TWDT)\n");
    printf("  4. telemetry_task  [Priority 1] - Voltage & Health Monitor (Monitored by TWDT)\n");
    printf("  Note: cli_task     [Priority 2] - Interactive USB console (Exempt from TWDT to avoid keyboard blocking)\n");
    printf("=========================================\n\n");
}
