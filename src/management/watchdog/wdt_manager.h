/**
 * @file wdt_manager.h
 * @brief Task Watchdog Timer (TWDT) manager to guard against task hang or deadlock,
 *        with diagnostic queries and system reset reason inspection.
 */

#ifndef WDT_MANAGER_H
#define WDT_MANAGER_H

#include <stdbool.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Configures and enables the ESP-IDF Task Watchdog Timer.
 * @param timeout_seconds Timeout in seconds before triggering reset.
 * @return true on success, false otherwise.
 */
bool wdt_manager_init(uint32_t timeout_seconds);

/**
 * @brief Registers the calling FreeRTOS task with the TWDT.
 * @return true on success, false otherwise.
 */
bool wdt_manager_register_task(void);

/**
 * @brief Resets (pets) the TWDT for the calling task.
 */
void wdt_manager_reset(void);

/**
 * @brief Returns whether the TWDT is currently initialized and active.
 * @return true if initialized, false otherwise.
 */
bool wdt_manager_is_initialized(void);

/**
 * @brief Returns the configured TWDT timeout duration in seconds.
 * @return Timeout duration in seconds (0 if not initialized).
 */
uint32_t wdt_manager_get_timeout_seconds(void);

/**
 * @brief Returns the hardware/system reset reason code from ESP-IDF.
 * @return esp_reset_reason_t enum value.
 */
esp_reset_reason_t wdt_manager_get_reset_reason(void);

/**
 * @brief Returns a human-readable description of the system reset reason.
 * @return Constant string description.
 */
const char *wdt_manager_get_reset_reason_str(void);

/**
 * @brief Checks if the previous system restart was triggered by a Watchdog Timer.
 * @return true if last reset was caused by TWDT or other Watchdog, false otherwise.
 */
bool wdt_manager_was_watchdog_reset(void);

#ifdef __cplusplus
}
#endif

#endif /* WDT_MANAGER_H */
