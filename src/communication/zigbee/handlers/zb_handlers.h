/**
 * @file zb_handlers.h
 * @brief Inbound ZCL command and attribute change handlers bridging into Command Verifier.
 */

#ifndef ZB_HANDLERS_H
#define ZB_HANDLERS_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Handles inbound On/Off cluster toggle/write from Zigbee network.
 * @param power_on true to power on, false to enter Standby.
 */
void zb_handler_on_off(bool power_on);

/**
 * @brief Handles inbound Level Control (Master Volume) write from Zigbee network.
 * @param zcl_level Zigbee level (0-254).
 */
void zb_handler_set_level(uint8_t zcl_level);

/**
 * @brief Handles inbound audio mute command from Zigbee network.
 * @param mute true to mute amplifier, false to unmute.
 */
void zb_handler_set_mute(bool mute);

/**
 * @brief Handles custom channel trim write from Zigbee network.
 * @param channel 0: Bass, 1: Center, 2: Rear.
 * @param zcl_level Trim level (0-254).
 */
void zb_handler_set_trim(uint8_t channel, uint8_t zcl_level);

/**
 * @brief Handles source input change from Zigbee network.
 * @param input Input number (1-6).
 */
void zb_handler_set_input(uint8_t input);

/**
 * @brief Handles audio effect change from Zigbee network.
 * @param effect Effect code (0: 3D, 1: 2.1, 2: 4.1, 3: None).
 */
void zb_handler_set_effect(uint8_t effect);

/**
 * @brief Handles inbound onboard LED enable/disable write from Zigbee network.
 * @param enabled true to enable LED notifications, false to disable.
 */
void zb_handler_set_led_enabled(bool enabled);

/**
 * @brief Handles inbound Command Verifier enable/disable write from Zigbee network.
 * @param enabled true to enable state-based verification, false to disable.
 */
void zb_handler_set_verifier_enabled(bool enabled);

#ifdef __cplusplus
}
#endif

#endif /* ZB_HANDLERS_H */
