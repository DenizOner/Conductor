/**
 * @file zigbee_state_publisher.h
 * @brief Native ESP Zigbee SDK v2 Verified State Publishing & Changed-Only ZCL Reporting.
 *
 * Architecture & Guarantees (Phase 5 & Selective Health Diagnostics):
 *   1. Authoritative Source Only:
 *      - Only confirmed, verified physical telemetry (z906_state), canonical application settings
 *        (led_notification_is_enabled(), z906_cmd_verifier_is_enabled()), and authoritative diagnostic
 *        snapshots (health_manager_get_snapshot(), system_metrics_get_snapshot()) are published.
 *      - Incoming Zigbee desired values, command ingress events, or optimistic states are NEVER published.
 *   2. No Polling Loop:
 *      - State publisher task waits on FreeRTOS direct task notifications (xTaskNotifyWait).
 *      - Producers invoke non-blocking zigbee_state_publisher_notify() with a bitmask of changed attributes.
 *      - Producer contexts NEVER acquire Zigbee SDK locks or invoke ZCL APIs directly.
 *   3. Changed-Only Discrete State & High-Rate Telemetry Coalescing:
 *      - Immediate ZCL report on discrete state changes (power, volume, trims, mute, input, effect, digital status,
 *        auto standby, LED enabled, verifier enabled, device_health, health_primary_comp, health_primary_reas).
 *      - Rate-limited latest-value coalescing for continuous telemetry (input_signal_level, idle_time, temperatures,
 *        and soc_temperature) to prevent flooding the Zigbee network.
 *      - Health critical transitions bypass normal SoC temperature minimum-interval gate for coalesced reporting.
 *   4. Pure ESP Zigbee SDK v2 API:
 *      - ezb_zcl_attr_desc_set_value() for descriptor backing synchronization.
 *      - ezb_zcl_report_attr_cmd_req() for binding-based ZCL attribute reporting (EZB_ADDR_MODE_NONE).
 *      - esp_zigbee_lock_acquire() / esp_zigbee_lock_release() with bounded timeout (500 ms).
 *      - Zero legacy esp_zb_* / ESP_ZB_* / ZBOSS symbols.
 *   5. Correct Desired / Applied / Pending State Semantics:
 *      - s_backing_values advances ONLY on ezb_zcl_attr_desc_set_value() SUCCESS (EZB_ERR_NONE).
 *      - A failed write (EZB_ERR_INV_STATE or other) MUST NOT advance applied/backing state.
 *      - EZB_ERR_INV_STATE is recorded in s_deferred_on_inv_state[] to suppress blind periodic
 *        retry. Reconciliation is triggered by force_full_sync() on the network join lifecycle event.
 *      - Zero dynamic memory allocation and zero flash/NVS writes during runtime.
 */

#ifndef ZIGBEE_STATE_PUBLISHER_H
#define ZIGBEE_STATE_PUBLISHER_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Canonical reportable attribute enumeration for Conductor Zigbee data model.
 * Exactly 21 reportable attributes mapped across Endpoints 1, 2, 3, 4, 11, 12, 13.
 */
typedef enum {
    ZB_PUB_ATTR_POWER = 0,             /**< EP1  / 0x0006 / 0x0000 OnOff (bool) */
    ZB_PUB_ATTR_VOLUME,                /**< EP1  / 0x0008 / 0x0000 CurrentLevel (uint8, ZCL 0..254) */
    ZB_PUB_ATTR_TRIM_BASS,             /**< EP2  / 0xFC01 / 0x0000 TrimBass (uint8, ZCL 0..254) */
    ZB_PUB_ATTR_TRIM_CENTER,           /**< EP2  / 0xFC01 / 0x0001 TrimCenter (uint8, ZCL 0..254) */
    ZB_PUB_ATTR_TRIM_REAR,             /**< EP2  / 0xFC01 / 0x0002 TrimRear (uint8, ZCL 0..254) */
    ZB_PUB_ATTR_MUTE,                  /**< EP2  / 0xFC01 / 0x0003 AudioMute (uint8, 0/1) */
    ZB_PUB_ATTR_INPUT,                 /**< EP3  / 0xFC02 / 0x0000 SourceInput (uint8, 1..6) */
    ZB_PUB_ATTR_EFFECT,                /**< EP3  / 0xFC02 / 0x0001 SourceEffect (uint8, 0..3) */
    ZB_PUB_ATTR_INPUT_SIGNAL_LEVEL,    /**< EP4  / 0xFC03 / 0x0000 InputSignalLevel (DATA24, 3 bytes) */
    ZB_PUB_ATTR_IDLE_TIME,             /**< EP4  / 0xFC03 / 0x0001 IdleTime (DATA32/uint32 seconds) */
    ZB_PUB_ATTR_DIGITAL_SIGNAL_STATUS, /**< EP4  / 0xFC03 / 0x0002 DigitalSignalStatus (uint8 code) */
    ZB_PUB_ATTR_AUTO_STANDBY,          /**< EP4  / 0xFC03 / 0x0003 AutoStandby (uint8, 0/1) */
    ZB_PUB_ATTR_BUILT_IN_LED,          /**< EP4  / 0xFC03 / 0x0007 BuiltInLedEnabled (uint8, 0/1) */
    ZB_PUB_ATTR_CMD_VERIFIER,          /**< EP4  / 0xFC03 / 0x0008 CmdVerifierEnabled (uint8, 0/1) */
    ZB_PUB_ATTR_TEMP1,                 /**< EP11 / 0x0402 / 0x0000 MeasuredValue (int16, 0.01 °C) */
    ZB_PUB_ATTR_TEMP2,                 /**< EP12 / 0x0402 / 0x0000 MeasuredValue (int16, 0.01 °C) */
    ZB_PUB_ATTR_TEMP3,                 /**< EP13 / 0x0402 / 0x0000 MeasuredValue (int16, 0.01 °C) */
    ZB_PUB_ATTR_DEVICE_HEALTH,         /**< EP4  / 0xFC03 / 0x0009 DeviceHealth (uint8, 0..3) */
    ZB_PUB_ATTR_HEALTH_PRIMARY_COMP,   /**< EP4  / 0xFC03 / 0x000A HealthPrimaryComponent (uint8) */
    ZB_PUB_ATTR_HEALTH_PRIMARY_REAS,   /**< EP4  / 0xFC03 / 0x000B HealthPrimaryReason (uint8) */
    ZB_PUB_ATTR_SOC_TEMP,              /**< EP4  / 0xFC03 / 0x000C SocTemperature (int16, 0.01 °C) */
    ZB_PUB_ATTR_COUNT                  /**< Total reportable attributes: 21 */
} zb_pub_attr_key_t;

/** Compile-time assertion verifying exactly 21 reportable attributes */
_Static_assert(ZB_PUB_ATTR_COUNT == 21, "Canonical reportable attribute count must be exactly 21!");

/** Bitmask constants for fast, non-blocking task notification */
#define ZB_PUB_NOTIF_POWER               (1UL << ZB_PUB_ATTR_POWER)
#define ZB_PUB_NOTIF_VOLUME              (1UL << ZB_PUB_ATTR_VOLUME)
#define ZB_PUB_NOTIF_TRIM_BASS           (1UL << ZB_PUB_ATTR_TRIM_BASS)
#define ZB_PUB_NOTIF_TRIM_CENTER         (1UL << ZB_PUB_ATTR_TRIM_CENTER)
#define ZB_PUB_NOTIF_TRIM_REAR           (1UL << ZB_PUB_ATTR_TRIM_REAR)
#define ZB_PUB_NOTIF_MUTE                (1UL << ZB_PUB_ATTR_MUTE)
#define ZB_PUB_NOTIF_INPUT               (1UL << ZB_PUB_ATTR_INPUT)
#define ZB_PUB_NOTIF_EFFECT              (1UL << ZB_PUB_ATTR_EFFECT)
#define ZB_PUB_NOTIF_INPUT_SIGNAL_LEVEL  (1UL << ZB_PUB_ATTR_INPUT_SIGNAL_LEVEL)
#define ZB_PUB_NOTIF_IDLE_TIME           (1UL << ZB_PUB_ATTR_IDLE_TIME)
#define ZB_PUB_NOTIF_DIGITAL_STATUS      (1UL << ZB_PUB_ATTR_DIGITAL_SIGNAL_STATUS)
#define ZB_PUB_NOTIF_AUTO_STANDBY        (1UL << ZB_PUB_ATTR_AUTO_STANDBY)
#define ZB_PUB_NOTIF_BUILT_IN_LED        (1UL << ZB_PUB_ATTR_BUILT_IN_LED)
#define ZB_PUB_NOTIF_CMD_VERIFIER        (1UL << ZB_PUB_ATTR_CMD_VERIFIER)
#define ZB_PUB_NOTIF_TEMP1               (1UL << ZB_PUB_ATTR_TEMP1)
#define ZB_PUB_NOTIF_TEMP2               (1UL << ZB_PUB_ATTR_TEMP2)
#define ZB_PUB_NOTIF_TEMP3               (1UL << ZB_PUB_ATTR_TEMP3)
#define ZB_PUB_NOTIF_DEVICE_HEALTH       (1UL << ZB_PUB_ATTR_DEVICE_HEALTH)
#define ZB_PUB_NOTIF_HEALTH_PRIMARY_COMP (1UL << ZB_PUB_ATTR_HEALTH_PRIMARY_COMP)
#define ZB_PUB_NOTIF_HEALTH_PRIMARY_REAS (1UL << ZB_PUB_ATTR_HEALTH_PRIMARY_REAS)
#define ZB_PUB_NOTIF_SOC_TEMP            (1UL << ZB_PUB_ATTR_SOC_TEMP)

/** Convenience mask for combined health transition notification */
#define ZB_PUB_NOTIF_HEALTH              (ZB_PUB_NOTIF_DEVICE_HEALTH | \
                                          ZB_PUB_NOTIF_HEALTH_PRIMARY_COMP | \
                                          ZB_PUB_NOTIF_HEALTH_PRIMARY_REAS)

/** Mask covering all 21 reportable attributes */
#define ZB_PUB_NOTIF_ALL_ATTRS           ((1UL << ZB_PUB_ATTR_COUNT) - 1UL)

/** Special control bit for requesting a forced full synchronization */
#define ZB_PUB_NOTIF_FORCE_FULL_SYNC     (1UL << 31)

/**
 * @brief Diagnostic and telemetry counters for state publisher operations (in-memory only).
 */
typedef struct {
    uint32_t notif_count;           /**< Total notifications received */
    uint32_t report_attempts;       /**< Total ZCL report request calls */
    uint32_t report_submit_success; /**< Total ZCL report requests successfully accepted by SDK */
    uint32_t report_submit_fail;    /**< Total ZCL report requests rejected by SDK */
    uint32_t lock_timeouts;         /**< Total Zigbee SDK lock acquisition timeouts */
    uint32_t full_sync_count;       /**< Total full synchronizations triggered */
    uint32_t coalesced_updates;     /**< Total telemetry updates coalesced due to rate limiting */
} zb_pub_diagnostics_t;

/**
 * @brief Initializes the Zigbee State Publisher service and spawns its static FreeRTOS task.
 *
 * Allocates static task stack and TCB. Resolves and caches attribute descriptor handles
 * from the registered device model. Registers observers with State Manager and Health Manager.
 *
 * @return ESP_OK on success, ESP_FAIL otherwise.
 */
esp_err_t zigbee_state_publisher_init(void);

/**
 * @brief Non-blocking notification function invoked by state-change producers.
 *
 * Safely posts an attribute bitmask to the publisher task using FreeRTOS xTaskNotify(..., eSetBits).
 * Executes in O(1) time without acquiring Zigbee locks, allocating memory, or performing I/O.
 *
 * @param change_mask Bitmask of changed attributes (e.g. ZB_PUB_NOTIF_VOLUME | ZB_PUB_NOTIF_POWER).
 */
void zigbee_state_publisher_notify(uint32_t change_mask);

/**
 * @brief Controls the network-ready reporting gate.
 *
 * When set to false (default at boot), reports to the Zigbee network are suppressed,
 * but descriptor backing memory updates and pending dirty flags are maintained.
 * When transitioned false -> true (e.g. after network join/rejoin in Phase 6),
 * all valid attributes are marked dirty and scheduled for reporting.
 *
 * @param ready true if device is joined and network is ready for ZCL reporting, false otherwise.
 */
void zigbee_state_publisher_set_network_ready(bool ready);

/**
 * @brief Checks whether the network-ready reporting gate is currently open.
 *
 * @return true if network is ready for ZCL reports, false otherwise.
 */
bool zigbee_state_publisher_is_network_ready(void);

/**
 * @brief Triggers an immediate, complete synchronization of all 21 reportable attributes.
 *
 * Forces descriptor updates from authoritative state and marks all valid attributes dirty.
 * If network is ready, reports are dispatched with bounded batching to prevent stack buffer exhaustion.
 */
void zigbee_state_publisher_force_full_sync(void);

/**
 * @brief Retrieves runtime diagnostic counters.
 *
 * @param out_diag Pointer to receive diagnostic statistics.
 */
void zigbee_state_publisher_get_diagnostics(zb_pub_diagnostics_t *out_diag);

/**
 * @brief Encodes a 24-bit unsigned audio signal level into standard ZCL DATA24 3-byte wire format (little-endian).
 *
 * @param signal_level 24-bit numeric signal value (0..0xFFFFFF).
 * @param out_bytes Pointer to 3-byte destination buffer.
 */
void zigbee_state_publisher_encode_data24(uint32_t signal_level, uint8_t out_bytes[3]);

/**
 * @brief Deterministically encodes a temperature measurement in Celsius to standard ZCL 0x0402 0.01 °C units.
 *
 * Guarantees:
 *   - Returns ZCL_TEMP_INVALID_VALUE (0x8000 / -32768) if !is_valid.
 *   - Checks isfinite(); non-finite values (NaN, +Inf, -Inf) return 0x8000.
 *   - Multiplies Celsius by 100.0 with deterministic nearest rounding (no float truncation).
 *   - Enforces int16 representable range (-32767 .. 32767).
 *   - Sentinel collision safeguard: rounded value of -32768 is safely remapped to 0x8000 (invalid sentinel).
 *
 * @param temp_celsius Temperature in Celsius (-128 .. 127 for int8_t hardware readings).
 * @param is_valid Validity flag indicating integrity-verified hardware reading.
 * @return Encoded int16_t ZCL value in 0.01 °C or 0x8000 invalid sentinel.
 */
int16_t zigbee_state_publisher_encode_temperature(float temp_celsius, bool is_valid);

/**
 * @brief Test seam: replaces the ezb_zcl_attr_desc_set_value() implementation.
 *
 * Default: wraps the real Zigbee SDK function.
 * Test binaries may replace this to inject controlled return codes such as
 * EZB_ERR_INV_STATE without linking the full Zigbee SDK at test time.
 * Pass NULL to restore the default real SDK implementation.
 *
 * Signature compatible with ezb_zcl_attr_desc_set_value():
 *   - First arg: opaque descriptor handle (ezb_zcl_attr_desc_t, a pointer type).
 *   - Second arg: pointer to new attribute value.
 *   - Return: int (ezb_err_t: 0 = success, non-zero = error code).
 */
typedef int (*pub_set_value_fn_t)(void *desc, const void *val_ptr);
void zigbee_state_publisher_set_value_fn(pub_set_value_fn_t fn);

/**
 * @brief Test helpers for direct production C code verification (FD-03).
 */
void zigbee_state_publisher_test_step(bool force_sync);
void zigbee_state_publisher_test_reset(void);
void zigbee_state_publisher_test_set_mock_descs(void);
void zigbee_state_publisher_test_set_desired(zb_pub_attr_key_t key, uint32_t val);
bool zigbee_state_publisher_test_get_applied(zb_pub_attr_key_t key, uint32_t *out_val);
bool zigbee_state_publisher_test_is_pending_deferred(zb_pub_attr_key_t key, uint32_t *out_val);
bool zigbee_state_publisher_test_is_dirty(zb_pub_attr_key_t key);

#ifdef __cplusplus
}
#endif

#endif /* ZIGBEE_STATE_PUBLISHER_H */
