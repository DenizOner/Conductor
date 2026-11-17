/**
 * @file ota_health.h
 * @brief Post-update health check engine and self-validation window for A/B rollback.
 */

#ifndef OTA_HEALTH_H
#define OTA_HEALTH_H

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Evaluation criteria flags checked during post-update observation window.
 */
typedef struct {
    bool scheduler_healthy;         /**< FreeRTOS task scheduler and core tasks running */
    bool watchdog_healthy;          /**< TWDT active and tasks feeding without timeout */
    bool nvs_healthy;               /**< NVS partition read/write valid without corruption */
    bool zigbee_healthy;            /**< Zigbee stack running and endpoint handlers registered */
    bool z906_comm_healthy;         /**< Z906 UART HAL and Command Verifier active without fatal errors */
} ota_health_criteria_t;

#define OTA_CRITERION_SCHEDULER   (1u << 0)
#define OTA_CRITERION_TWDT        (1u << 1)
#define OTA_CRITERION_NVS         (1u << 2)
#define OTA_CRITERION_ZIGBEE      (1u << 3)
#define OTA_CRITERION_Z906_COMM    (1u << 4)
#define OTA_ALL_CRITERIA_PASS_MASK (OTA_CRITERION_SCHEDULER | OTA_CRITERION_TWDT | OTA_CRITERION_NVS | OTA_CRITERION_ZIGBEE | OTA_CRITERION_Z906_COMM)

/**
 * @brief Initializes the post-update health check service at early boot.
 * Detects if the current running image is in ESP_OTA_IMG_PENDING_VERIFY state.
 * @return true if initialized successfully.
 */
bool ota_health_init(void);

/**
 * @brief Ticks the health evaluation window (typically called every 1 second).
 * Evaluates critical system criteria for a 15-second validation window.
 * If all criteria pass, commits firmware with esp_ota_mark_app_valid_cancel_rollback().
 * If any fatal criteria fail or timeout expires with unfulfilled criteria,
 * executes automatic rollback with esp_ota_mark_app_invalid_rollback_and_reboot().
 * @param elapsed_sec Legacy caller hint; real time is measured by esp_timer_get_time().
 */
void ota_health_process_cycle(uint32_t elapsed_sec);

/**
 * @brief Checks if the running application is currently in pending verification state.
 * @return true if in first boot post-upgrade pending health validation.
 */
bool ota_health_is_pending_verify(void);

/**
 * @brief Manually triggers an immediate rollback to previous workable firmware slot.
 * @param reason Reason description string for logging.
 */
void ota_health_trigger_rollback(const char *reason);

/**
 * @brief Retrieves current health criteria status snapshot.
 * @param out_criteria Pointer to receive criteria snapshot.
 */
void ota_health_get_criteria(ota_health_criteria_t *out_criteria);

/**
 * @brief Returns the bitmask of experimentally or manually passed health criteria.
 * Compatible with the OTA health validation test fixtures.
 */
uint32_t ota_health_get_passed_criteria_mask(void);

/**
 * @brief Mark the scheduler criterion as successfully verified.
 */
void ota_health_report_scheduler_ok(void);

/**
 * @brief Mark the NVS criterion as successfully verified.
 */
void ota_health_report_nvs_ok(void);

/**
 * @brief Mark the TWDT criterion as successfully verified.
 */
void ota_health_report_twdt_ok(void);

/** @brief Reports only the telemetry owner task\x27s actual TWDT feed outcome. */
void ota_health_report_twdt_observation(bool fed);

/**
 * @brief Mark the Zigbee criterion as successfully verified.
 */
void ota_health_report_zigbee_ok(void);

/**
 * @brief Mark the Z906 communication criterion as successfully verified.
 */
void ota_health_report_z906_comm_ok(void);

/**
 * @brief Checks if a rollback was detected upon boot from a previous failed upgrade.
 * @return true if previous boot was rolled back by bootloader.
 */
bool ota_health_was_rollback_detected(void);

/**
 * @brief Checks if firmware image was successfully validated and committed.
 * @return true if validated.
 */
bool ota_health_is_validated(void);

/**
 * @brief Test helpers for OTA health state machine & transition verification.
 */
typedef void (*ota_rollback_fn_t)(const char *reason);
typedef esp_err_t (*ota_mark_valid_fn_t)(void);

typedef int64_t (*ota_health_time_fn_t)(void);
typedef bool (*ota_health_bool_fn_t)(void);

void ota_health_set_pending_verify_for_test(bool pending);
void ota_health_set_time_fn_for_test(ota_health_time_fn_t fn);
void ota_health_set_z906_status_time_fn_for_test(ota_health_time_fn_t fn);
void ota_health_set_zigbee_ready_fn_for_test(ota_health_bool_fn_t fn);
void ota_health_set_nvs_healthy_fn_for_test(ota_health_bool_fn_t fn);
void ota_health_set_twdt_healthy_fn_for_test(ota_health_bool_fn_t fn);
void ota_health_set_fallback_available_fn_for_test(ota_health_bool_fn_t fn);
void ota_health_set_rollback_fn_for_test(ota_rollback_fn_t fn);
void ota_health_set_mark_valid_fn_for_test(ota_mark_valid_fn_t fn);
void ota_health_reset_for_test(void);

#ifdef __cplusplus
}
#endif

#endif /* OTA_HEALTH_H */
