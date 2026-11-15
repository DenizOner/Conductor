/**
 * @file main.c
 * @brief Application entry point for "Conductor - Smart Zigbee Console for Logitech z906".
 *
 * ## Role of This File
 *
 * This file is an intentionally thin composition entry point. It has exactly
 * one responsibility: invoke the System Initializer.
 *
 * All boot orchestration — phase ordering, dependency management, failure
 * policy, task spawning, and system readiness tracking — lives in:
 *   src/system/initializer/system_initializer.c
 *
 * ## Why app_main Is Kept Separate From The Initializer
 *
 * ESP-IDF requires app_main() to be the C-linkage entry point. The System
 * Initializer is a normal module that can be independently tested. Keeping
 * them separate maintains a clean boundary: app_main is the platform hook;
 * system_initializer_run() is the firmware boot logic.
 *
 * The __attribute__((weak)) on app_main allows test runners (e.g., Unity on
 * ESP32) to override the entry point without linker conflicts.
 *
 * ## Fatal Boot Failure Policy
 *
 * If system_initializer_run() returns a non-ESP_OK error code, the system
 * has encountered a fatal initialization failure. In this case:
 *   1. The failure is already logged inside the initializer with component,
 *      phase, and error code details.
 *   2. app_main logs the summary and halts in a controlled safe state.
 *   3. We do NOT immediately restart: an automatic restart here risks a boot
 *      loop if the underlying hardware failure (e.g., NVS corruption, UART
 *      hardware fault) persists across reboots.
 *   4. OTA health will not confirm a VALID image (scheduler criterion not
 *      signaled), causing rollback to the previous known-good firmware slot
 *      if this is a post-OTA boot. This is the intended rollback trigger.
 */

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "config.h"
#include "system_initializer.h"

/** @brief Logging tag for the application entry point */
static const char *TAG = "APP_MAIN";

/**
 * @brief Application entry point.
 *
 * Declared weak to allow PlatformIO/Unity test runners to provide their own
 * app_main implementation without a linker symbol conflict.
 */
__attribute__((weak)) void app_main(void)
{
    ESP_LOGI(TAG, "=== Conductor - Smart Zigbee Console for Logitech z906 ===");
    ESP_LOGI(TAG, "=== Firmware %s | ESP32-C6 ===", Z906_FW_VERSION_STR);

    /* Run the complete boot sequence.
     * On success: all tasks are running and system is READY.
     * On failure: report is populated; system is in safe FAILED state. */
    esp_err_t err = system_initializer_run();

    if (err != ESP_OK) {
        /* Retrieve boot report for a concise failure summary */
        system_init_report_t report;
        system_initializer_get_report(&report);

        ESP_LOGE(TAG, "FATAL: System initialization failed!");
        ESP_LOGE(TAG, "  Phase:     %s", boot_phase_str(report.failed_phase));
        ESP_LOGE(TAG, "  Component: %s",
                 report.failed_component ? report.failed_component : "unknown");
        ESP_LOGE(TAG, "  Error:     %s (0x%x)",
                 esp_err_to_name(report.last_error), report.last_error);
        ESP_LOGE(TAG, "  Elapsed:   %lu ms", (unsigned long)report.elapsed_ms);
        ESP_LOGE(TAG, "System halted. OTA health will trigger rollback if applicable.");

        /* Controlled safe halt: yield indefinitely.
         * We do not restart here to avoid masking persistent hardware failures
         * with a boot loop. The OTA health watchdog will handle rollback. */
        while (1) {
            vTaskDelay(portMAX_DELAY);
        }
    }

    /* If system_initializer_run() returns ESP_OK, all tasks are now running.
     * app_main() returns here and the FreeRTOS scheduler continues normally.
     * No further code is needed in this file. */
}
