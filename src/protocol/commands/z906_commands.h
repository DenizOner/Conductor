/**
 * @file z906_commands.h
 * @brief Logitech Z906 UART protocol command definitions and enum structures.
 *
 * Fully compliant with the canonical reverse engineering reference documentation
 * (nomis/logitech-z906): protocol.rst, interface.rst, firmware.rst, README.rst.
 */

#ifndef Z906_COMMANDS_H
#define Z906_COMMANDS_H

#include <stdint.h>
#include <stdbool.h>
#include "z906_state.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 
 * @brief Single-byte command headers exchanged between console and amplifier.
 * Reference: protocol.rst
 */
typedef enum {
    /* Input selection opcodes                                                                      */
    /* NOTE: Opcodes are NOT sequential with input numbers — this is a hardware protocol detail.   */
    /* See z906_input_t in z906_state.h for the canonical connection-type enum names.              */
    Z906_CMD_SELECT_INPUT_1         = 0x02, /**< Input 1 — 6-Channel Direct: three 3.5mm TRS jacks (6-channel analog) */
    Z906_CMD_SELECT_INPUT_3         = 0x03, /**< Input 3 — Optical 1: first TOSLINK digital optical connector */
    Z906_CMD_SELECT_INPUT_4         = 0x04, /**< Input 4 — Optical 2: second TOSLINK digital optical connector */
    Z906_CMD_SELECT_INPUT_2         = 0x05, /**< Input 2 — RCA Stereo: red/white RCA connectors (stereo analog) */
    Z906_CMD_SELECT_INPUT_5         = 0x06, /**< Input 5 — Digital Coaxial: 75-ohm S/PDIF coaxial connector */
    Z906_CMD_SELECT_INPUT_6         = 0x07, /**< Input 6 — Auxiliary (AUX): 3.5mm TRS on the console side panel */

    /* Volume adjustment opcodes (step up/down by 1, bounds 0-43) */
    Z906_CMD_VOLUME_UP              = 0x08, /**< Master volume increment (+1) */
    Z906_CMD_VOLUME_DOWN            = 0x09, /**< Master volume decrement (-1) */
    Z906_CMD_VOLUME_BASS_UP         = 0x0A, /**< Subwoofer level increment (+1) */
    Z906_CMD_VOLUME_BASS_DOWN       = 0x0B, /**< Subwoofer level decrement (-1) */
    Z906_CMD_VOLUME_CENTER_UP       = 0x0C, /**< Centre level increment (+1) */
    Z906_CMD_VOLUME_CENTER_DOWN     = 0x0D, /**< Centre level decrement (-1) */
    Z906_CMD_VOLUME_REAR_UP         = 0x0E, /**< Rear surround level increment (+1) */
    Z906_CMD_VOLUME_REAR_DOWN       = 0x0F, /**< Rear surround level decrement (-1) */

    /* Headphones & Power sequence headers (protocol.rst) */
    Z906_CMD_HEADPHONES_CONNECTED   = 0x10, /**< Headphones connected prefix (sent as 10 10 3F) */
    Z906_CMD_POWER_OFF              = 0x10, /**< Legacy alias for 0x10 */
    Z906_CMD_HEADPHONES_DISCONNECTED= 0x11, /**< Headphones disconnected prefix (sent as 11 11 <fx>) */
    Z906_CMD_POWER_ON               = 0x11, /**< Legacy alias: Power on starts with 11 11 */

    /* Spatial audio effect opcodes */
    Z906_CMD_SELECT_EFFECT_3D       = 0x14, /**< 3D spatial effect */
    Z906_CMD_SELECT_EFFECT_4_1      = 0x15, /**< 4.1 spatial effect */
    Z906_CMD_SELECT_EFFECT_2_1      = 0x16, /**< 2.1 spatial effect */
    Z906_CMD_SELECT_EFFECT_NONE     = 0x35, /**< Effect disabled (None) */
    Z906_CMD_HEADPHONES             = 0x3F, /**< Headphones mode (no effect) */

    /* Telemetry queries and status feedback from amplifier */
    Z906_CMD_AMP_MUTED              = 0x20, /**< Amplifier reports muted status */
    Z906_CMD_AMP_UNMUTED            = 0x21, /**< Amplifier reports unmuted status */
    Z906_CMD_START_TEST             = 0x22, /**< Speaker test mode enter */
    Z906_CMD_READ_TEMP              = 0x25, /**< Read amplifier temperature (responds with AA 0C 05) */
    Z906_CMD_READ_INPUT_VOL         = 0x2F, /**< Read input volume amplitude (responds with AA 08 03) */
    Z906_CMD_RESET_IDLE             = 0x30, /**< Reset amplifier idle timer */
    Z906_CMD_READ_IDLE              = 0x31, /**< Read amplifier idle time (responds with AA 0F 03) */
    Z906_CMD_STOP_TEST              = 0x33, /**< Speaker test mode exit */
    Z906_CMD_READ_STATUS            = 0x34, /**< Read 20-byte configuration frame (AA 0A 14) */
    Z906_CMD_STANDBY_SAVE           = 0x36, /**< Turns speakers off and saves settings (30 37 36) */
    Z906_CMD_SAVE                   = 0x36, /**< Legacy alias for 0x36 */
    Z906_CMD_STANDBY_PREP           = 0x37, /**< Standby sequence byte (30 37 36) */
    Z906_CMD_UNMUTE_START           = 0x38, /**< Unmute adjustment bracket start */
    Z906_CMD_MUTE_ON                = 0x38, /**< Legacy alias for 0x38 */
    Z906_CMD_UNMUTE_END             = 0x39, /**< Unmute adjustment bracket end */
    Z906_CMD_MUTE_OFF               = 0x39, /**< Legacy alias for 0x39 */

    /* Multi-byte packet preamble */
    Z906_CMD_MULTIBYTE_REPLY        = 0xAA  /**< Multi-byte packet header */
} z906_cmd_header_t;

/**
 * @brief Sub-types of 0xAA multibyte framed packets.
 * Reference: protocol.rst
 */
typedef enum {
    Z906_MB_CONFIG_AMP              = 0x0A, /**< 20-byte amplifier status payload (AA 0A 14 ...) */
    Z906_MB_CONFIG_CONSOLE          = 0x0E, /**< Console status payload or idle MSB reset (AA 0E 03) */
    Z906_MB_READ_IDLE               = 0x0F, /**< U24BE idle time in seconds (AA 0F 03 ...) */
    Z906_MB_SPEAKER_TEST            = 0x07, /**< Speaker test channel select (AA 07 08 ...) */
    Z906_MB_INPUT_VOLUME            = 0x08, /**< U24BE input audio volume amplitude (AA 08 03 ...) */
    Z906_MB_AMP_TEMP                = 0x0C, /**< 3-channel temperature in Celsius (AA 0C 05 ...) */
    Z906_MB_RESET_IDLE              = 0x0E  /**< Idle time reset packet type */
} z906_multibyte_type_t;

/**
 * @brief Decode state responses emitted by amplifier upon digital input selection.
 * Reference: protocol.rst (Section: Decode state)
 */
typedef enum {
    Z906_DECODE_5_1                 = 0x17, /**< 5.1 signal detected (5.1 levels, no effects) */
    Z906_DECODE_NO_SIGNAL           = 0x18, /**< No digital signal detected */
    Z906_DECODE_4_1                 = 0x19, /**< 4.1 signal detected (4.1 levels, no effects) */
    Z906_DECODE_3_1                 = 0x1A, /**< 3.1 signal detected (5.1 levels, no effects) */
    Z906_DECODE_2_1                 = 0x1B, /**< 2.1 signal detected (2.1 levels, no effects) */
    Z906_DECODE_2_0                 = 0x1C, /**< 2.0 signal detected (2.1 levels, no effects) */
    Z906_DECODE_SIGNAL_5_1          = 0x1D, /**< Signal detected (5.1 levels, no effects) */
    Z906_DECODE_2_0_MANDATORY_FX    = 0x1E, /**< 2.0 signal detected (5.1 levels, effects mandatory) */
    Z906_DECODE_UNKNOWN             = 0x1F  /**< Unknown digital signal */
} z906_decode_state_t;

/**
 * @brief Digital signal status codes reported in Byte 12 of AA 0A 14 status frame.
 * Reference: protocol.rst (Section: Digital signal status (amplifier))
 */
typedef enum {
    Z906_SIG_AMP_NA                 = 0x00, /**< N/A */
    Z906_SIG_AMP_5_1                = 0x01, /**< 5.1 signal detected (5.1 levels, no effects) */
    Z906_SIG_AMP_2_0                = 0x0C, /**< 2.0 signal detected (5.1 levels, effects mandatory) */
    Z906_SIG_AMP_UNKNOWN            = 0x0E  /**< Unknown signal (decode/effect LEDs cycle) */
} z906_digital_signal_status_t;

/**
 * @brief Converts raw hardware volume (0-43) to percentage (0-100%).
 */
uint8_t z906_cmd_volume_to_percent(uint8_t volume);

/**
 * @brief Converts percentage (0-100%) to raw hardware volume (0-43).
 */
uint8_t z906_cmd_percent_to_volume(uint8_t percent);

/**
 * @brief Converts hardware volume (0-43) to Zigbee ZCL level (0-254).
 */
uint8_t z906_cmd_volume_to_zcl(uint8_t volume);

/**
 * @brief Converts Zigbee ZCL level (0-254) to hardware volume (0-43).
 */
uint8_t z906_cmd_zcl_to_volume(uint8_t zcl_level);

/**
 * @brief Maps input enum to corresponding UART command header.
 */
z906_cmd_header_t z906_cmd_from_input(z906_input_t input);

/**
 * @brief Maps effect enum to corresponding UART command header.
 */
z906_cmd_header_t z906_cmd_from_effect(z906_effect_t effect);

/**
 * @brief Checks if a given effect is supported on the active input source.
 */
z906_effect_t z906_cmd_validate_effect(z906_input_t input, z906_effect_t effect);

/**
 * @brief Returns human-readable name for a decode state byte.
 */
const char *z906_cmd_decode_state_str(uint8_t decode_state);

/**
 * @brief Returns human-readable name for an amplifier digital signal status byte.
 */
const char *z906_cmd_digital_signal_status_str(uint8_t status);

#ifdef __cplusplus
}
#endif

#endif /* Z906_COMMANDS_H */
