/**
 * @file debug_console.h
 * @brief Modern and modular Debug Console service for Logitech Z906 ESP32-C6 controller.
 *
 * Provides centralized diagnostic logging controls, runtime log level adjustment,
 * per-tag module filtering, system health inspection, and Watchdog (TWDT) status reporting.
 */

#ifndef DEBUG_CONSOLE_H
#define DEBUG_CONSOLE_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "esp_log.h"
#include "wdt_manager.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Maximum number of custom module/tag filters that can be retained statically */
#define DEBUG_CONSOLE_MAX_TAG_FILTERS   16

/** @brief Maximum string length of a module tag identifier */
#define DEBUG_CONSOLE_TAG_MAX_LEN       24

/**
 * @brief Structure representing an active module tag log filter.
 */
typedef struct {
    char tag[DEBUG_CONSOLE_TAG_MAX_LEN]; /**< Tag name (e.g. "ZIGBEE_APP", "CMD_VERIFY") */
    esp_log_level_t level;               /**< Configured log level for this tag */
} debug_tag_filter_t;

/**
 * @brief Initializes the Debug Console service and static synchronization locks.
 * @return true on success, false otherwise.
 */
bool debug_console_init(void);

/**
 * @brief Returns whether the Debug Console service is initialized.
 * @return true if initialized, false otherwise.
 */
bool debug_console_is_initialized(void);

/**
 * @brief Sets the global system log level for all tags.
 * @param level The target log level (ESP_LOG_NONE to ESP_LOG_VERBOSE).
 * @return true on success, false otherwise.
 */
bool debug_console_set_global_level(esp_log_level_t level);

/**
 * @brief Returns the current global log level.
 * @return Active global esp_log_level_t.
 */
esp_log_level_t debug_console_get_global_level(void);

/**
 * @brief Converts an esp_log_level_t enum to a human-readable lowercase string.
 * @param level The log level enum.
 * @return Constant string description (e.g., "error", "warn", "info", "debug", "verbose", "none").
 */
const char *debug_console_level_to_str(esp_log_level_t level);

/**
 * @brief Parses a string into the corresponding esp_log_level_t enum.
 * @param str The string representation ("error", "warn", "info", "debug", "verbose", "none").
 * @return The parsed esp_log_level_t, or ESP_LOG_NONE if unrecognized.
 */
esp_log_level_t debug_console_str_to_level(const char *str);

/**
 * @brief Configures a specific log level for a given module tag.
 *
 * Updates both the ESP-IDF native logging system and the internal static filter table.
 *
 * @param tag The module tag (e.g., "ZIGBEE_APP", "CMD_VERIFY", "UART_HAL").
 * @param level The target log level for this specific tag.
 * @return true on success, false if tag is NULL or table capacity is exceeded.
 */
bool debug_console_set_tag_level(const char *tag, esp_log_level_t level);

/**
 * @brief Returns the configured log level for a specific tag.
 * @param tag The module tag to query.
 * @return The tag-specific log level, or the global log level if no specific filter is set.
 */
esp_log_level_t debug_console_get_tag_level(const char *tag);

/**
 * @brief Returns the total count of active custom tag filters.
 * @return Count of custom filters in the static registry.
 */
size_t debug_console_get_filter_count(void);

/**
 * @brief Retrieves information about an active custom tag filter by index.
 * @param index 0-based filter index (< debug_console_get_filter_count()).
 * @param out_tag Buffer to receive the tag name string.
 * @param tag_max Maximum capacity of out_tag buffer.
 * @param out_level Pointer to receive the configured log level.
 * @return true on success, false if index is out of bounds or pointers are NULL.
 */
bool debug_console_get_filter(size_t index, char *out_tag, size_t tag_max, esp_log_level_t *out_level);

/**
 * @brief Clears all custom module tag filters and restores the global log level across all modules.
 */
void debug_console_clear_tag_filters(void);

/**
 * @brief Prints a comprehensive diagnostic status report over the console.
 *
 * Includes uptime, free heap memory, minimum ever free heap, active global log level,
 * active custom tag filters, and Watchdog status summary.
 */
void debug_console_print_status(void);

/**
 * @brief Prints detailed Watchdog Timer (TWDT) diagnostic status and reset reason analysis.
 */
void debug_console_print_wdt_status(void);

#ifdef __cplusplus
}
#endif

#endif /* DEBUG_CONSOLE_H */
