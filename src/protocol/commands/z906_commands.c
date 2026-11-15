/**
 * @file z906_commands.c
 * @brief Implementation of protocol command conversions and volume scale utilities.
 *
 * Fully compliant with the canonical reverse engineering reference documentation
 * (nomis/logitech-z906): protocol.rst, interface.rst, firmware.rst, README.rst.
 */

#include "z906_commands.h"
#include "config.h"

uint8_t z906_cmd_volume_to_percent(uint8_t volume)
{
    if (volume >= Z906_MAX_VOLUME_LEVEL) {
        return 100;
    }
    return (uint8_t)(((uint32_t)volume * 100 + (Z906_MAX_VOLUME_LEVEL / 2)) / Z906_MAX_VOLUME_LEVEL);
}

uint8_t z906_cmd_percent_to_volume(uint8_t percent)
{
    if (percent >= 100) {
        return Z906_MAX_VOLUME_LEVEL;
    }
    return (uint8_t)(((uint32_t)percent * Z906_MAX_VOLUME_LEVEL + 50) / 100);
}

uint8_t z906_cmd_volume_to_zcl(uint8_t volume)
{
    if (volume >= Z906_MAX_VOLUME_LEVEL) {
        return 254;
    }
    return (uint8_t)(((uint32_t)volume * 254 + (Z906_MAX_VOLUME_LEVEL / 2)) / Z906_MAX_VOLUME_LEVEL);
}

uint8_t z906_cmd_zcl_to_volume(uint8_t zcl_level)
{
    if (zcl_level >= 254) {
        return Z906_MAX_VOLUME_LEVEL;
    }
    return (uint8_t)(((uint32_t)zcl_level * Z906_MAX_VOLUME_LEVEL + 127) / 254);
}

z906_cmd_header_t z906_cmd_from_input(z906_input_t input)
{
    switch (input) {
        /* Protocol opcodes are NOT sequential — see protocol.rst Section 'Input selection' */
        case Z906_INPUT_SIX_CHANNEL_DIRECT: return Z906_CMD_SELECT_INPUT_1; /* 0x02: three 3.5mm TRS */
        case Z906_INPUT_RCA_STEREO:         return Z906_CMD_SELECT_INPUT_2; /* 0x05: red/white RCA */
        case Z906_INPUT_OPTICAL_1:          return Z906_CMD_SELECT_INPUT_3; /* 0x03: first TOSLINK */
        case Z906_INPUT_OPTICAL_2:          return Z906_CMD_SELECT_INPUT_4; /* 0x04: second TOSLINK */
        case Z906_INPUT_DIGITAL_COAXIAL:    return Z906_CMD_SELECT_INPUT_5; /* 0x06: 75-ohm S/PDIF */
        case Z906_INPUT_AUXILIARY:          return Z906_CMD_SELECT_INPUT_6; /* 0x07: 3.5mm AUX */
        default:                            return Z906_CMD_SELECT_INPUT_1;  /* safe fallback */
    }
}

z906_cmd_header_t z906_cmd_from_effect(z906_effect_t effect)
{
    switch (effect) {
        case Z906_EFFECT_3D:   return Z906_CMD_SELECT_EFFECT_3D;
        case Z906_EFFECT_4_1:  return Z906_CMD_SELECT_EFFECT_4_1;
        case Z906_EFFECT_2_1:  return Z906_CMD_SELECT_EFFECT_2_1;
        case Z906_EFFECT_NONE: return Z906_CMD_SELECT_EFFECT_NONE;
        default:               return Z906_CMD_SELECT_EFFECT_NONE;
    }
}

z906_effect_t z906_cmd_validate_effect(z906_input_t input, z906_effect_t effect)
{
    /* Analog inputs support spatial effects:
     *   - Input 1 (Z906_INPUT_SIX_CHANNEL_DIRECT)  : 6-Channel Direct — three 3.5mm TRS jacks
     *   - Input 2 (Z906_INPUT_RCA_STEREO)           : RCA Stereo — red/white RCA connectors
     *   - Input 6 (Z906_INPUT_AUXILIARY)            : Auxiliary (AUX) — 3.5mm TRS side panel
     * Digital inputs force effect to NONE:
     *   - Input 3 (Z906_INPUT_OPTICAL_1)            : Optical 1 — first TOSLINK
     *   - Input 4 (Z906_INPUT_OPTICAL_2)            : Optical 2 — second TOSLINK
     *   - Input 5 (Z906_INPUT_DIGITAL_COAXIAL)      : Digital Coaxial — 75-ohm S/PDIF
     */
    if (input == Z906_INPUT_SIX_CHANNEL_DIRECT ||
        input == Z906_INPUT_RCA_STEREO         ||
        input == Z906_INPUT_AUXILIARY) {
        return effect;
    }
    return Z906_EFFECT_NONE;
}

const char *z906_cmd_decode_state_str(uint8_t decode_state)
{
    switch ((z906_decode_state_t)decode_state) {
        case Z906_DECODE_5_1:              return "5.1 Digital Signal (5.1 levels, no effects)";
        case Z906_DECODE_NO_SIGNAL:        return "No Signal";
        case Z906_DECODE_4_1:              return "4.1 Digital Signal (4.1 levels, no effects)";
        case Z906_DECODE_3_1:              return "3.1 Digital Signal (5.1 levels, no effects)";
        case Z906_DECODE_2_1:              return "2.1 Digital Signal (2.1 levels, no effects)";
        case Z906_DECODE_2_0:              return "2.0 Digital Signal (2.1 levels, no effects)";
        case Z906_DECODE_SIGNAL_5_1:       return "Signal Detected (5.1 levels, no effects)";
        case Z906_DECODE_2_0_MANDATORY_FX: return "2.0 Digital Signal (effects mandatory)";
        case Z906_DECODE_UNKNOWN:          return "Unknown Digital Signal";
        default:                           return "Unrecognized Decode State";
    }
}

const char *z906_cmd_digital_signal_status_str(uint8_t status)
{
    switch ((z906_digital_signal_status_t)status) {
        case Z906_SIG_AMP_NA:      return "N/A (Analog or Standby)";
        case Z906_SIG_AMP_5_1:     return "5.1 Signal Detected";
        case Z906_SIG_AMP_2_0:     return "2.0 Signal Detected";
        case Z906_SIG_AMP_UNKNOWN: return "Unknown Signal (Cycling LEDs)";
        default:                   return "Custom Signal Status";
    }
}
