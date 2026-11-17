/**
 * @file z906_state.h
 * @brief Thread-safe confirmed hardware state storage for Logitech Z906.
 *
 * Stores authoritative physical telemetry confirmed by the amplifier's responses,
 * eliminating all optimistic assumptions.
 *
 * ## Logitech Z906 Audio Input Canonical Reference
 * The six physical audio inputs are identified by their connection type:
 *
 * | Enum Identifier                 | Input # | Protocol byte | Physical Connection                        |
 * |---------------------------------|---------|---------------|--------------------------------------------|
 * | Z906_INPUT_SIX_CHANNEL_DIRECT   | Input 1 | 0x02          | Three 3.5mm TRS jacks (6-channel analog)   |
 * | Z906_INPUT_RCA_STEREO           | Input 2 | 0x05          | Red/white RCA connectors (stereo analog)   |
 * | Z906_INPUT_OPTICAL_1            | Input 3 | 0x03          | First TOSLINK digital optical              |
 * | Z906_INPUT_OPTICAL_2            | Input 4 | 0x04          | Second TOSLINK digital optical             |
 * | Z906_INPUT_DIGITAL_COAXIAL      | Input 5 | 0x06          | Digital coaxial S/PDIF (75-ohm)            |
 * | Z906_INPUT_AUXILIARY            | Input 6 | 0x07          | 3.5mm TRS AUX on the side of the console  |
 *
 * IMPORTANT: The enum values (1-6) map directly to the z906Sources ZCL attribute
 * and the CLI 'input' parameter. The protocol byte (UART opcode) is determined by
 * z906_cmd_from_input() and does NOT follow sequential order. Never change the
 * enum integer values without updating all ZCL, CLI, and converter mappings.
 */

#ifndef Z906_STATE_H
#define Z906_STATE_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Canonical audio input source identifiers for Logitech Z906.
 *
 * Integer values (1-6) are protocol-stable and must not be changed.
 * They correspond directly to the CLI 'input <1-6>' parameter and the
 * sourceInput ZCL attribute on cluster 0xFC02 endpoint 3.
 *
 * so that any code still referencing them continues to compile unchanged.
 */
typedef enum {
    Z906_INPUT_SIX_CHANNEL_DIRECT  = 1, /**< Input 1: Three 3.5mm TRS jacks — 6-Channel Direct analog */
    Z906_INPUT_RCA_STEREO          = 2, /**< Input 2: Red/white RCA connectors — Stereo analog */
    Z906_INPUT_OPTICAL_1           = 3, /**< Input 3: First TOSLINK connector — Optical digital */
    Z906_INPUT_OPTICAL_2           = 4, /**< Input 4: Second TOSLINK connector — Optical digital */
    Z906_INPUT_DIGITAL_COAXIAL     = 5, /**< Input 5: 75-ohm S/PDIF connector — Digital Coaxial */
    Z906_INPUT_AUXILIARY           = 6  /**< Input 6: 3.5mm TRS on console side panel — Auxiliary (AUX) */
} z906_input_t;

typedef enum {
    Z906_EFFECT_3D = 0,
    Z906_EFFECT_2_1 = 1,
    Z906_EFFECT_4_1 = 2,
    Z906_EFFECT_NONE = 3
} z906_effect_t;

/** @brief Authoritative physical state of the Z906 speaker system */
typedef struct {
    bool power;                     /**< Confirmed power state (true = ON, false = Standby) */
    bool mute;                      /**< Confirmed mute state */
    uint8_t volume;                 /**< Master volume (0 - 43) */
    uint8_t volume_bass;            /**< Subwoofer volume (0 - 43) */
    uint8_t volume_center;          /**< Center speaker volume (0 - 43) */
    uint8_t volume_rear;            /**< Rear surround volume (0 - 43) */
    z906_input_t input;             /**< Confirmed input source (1-6, see z906_input_t) */
    z906_effect_t effect;           /**< Confirmed spatial effect */
    uint8_t digital_signal_status;  /**< Digital signal code from Byte 12 of AA 0A 14 */
    bool auto_standby_enabled;      /**< True if auto-standby 2h idle timer is active */
    int8_t temp_celsius[3];         /**< Amplifier temperatures in Celsius (Temp 1, Temp 2, Temp 3: Power Supply Transformer) */
    bool temp_valid;                /**< true only after at least one successful, integrity-verified temperature query.
                                     *   false at boot and whenever the last query failed or was skipped.
                                     *   Zigbee and HA MUST NOT report temperatures while this is false. */
    uint32_t idle_seconds;          /**< Amplifier idle seconds telemetry */
    uint32_t input_signal_level;    /**< Raw U24BE input signal level telemetry from 0x2F */
    bool cable_connected;           /**< Hardware DE-15 cable presence */
    bool is_operational;            /**< System operational status */
    bool main_state_valid;          /**< true only after first verified main status (0x34 / CONFIG) frame is decoded.
                                     *   false at boot and on cable disconnect.
                                     *   Hardware attributes MUST NOT be published while this is false. */
} z906_state_t;

/**
 * @brief Initializes the state manager and its static mutex.
 */
void z906_state_init(void);

/**
 * @brief Retrieves a snapshot of the current confirmed hardware state.
 * @param out_state Pointer to receive the state snapshot.
 */
void z906_state_get(z906_state_t *out_state);

/**
 * @brief Updates master volume with bounds enforcement (0 - 43).
 * @param volume Desired volume level.
 * @return true if state changed, false if unchanged.
 */
bool z906_state_set_volume(uint8_t volume);

/**
 * @brief Updates trim volume levels.
 * @param channel Channel to update (0=Bass, 1=Center, 2=Rear).
 * @param level Desired level (0 - 43).
 * @return true if state changed.
 */
bool z906_state_set_trim(uint8_t channel, uint8_t level);

/**
 * @brief Updates the active audio input source.
 * @param input Selected input (use z906_input_t canonical names, e.g. Z906_INPUT_OPTICAL_1).
 * @return true if changed.
 */
bool z906_state_set_input(z906_input_t input);

/**
 * @brief Updates the active audio effect mode.
 * @param effect Selected effect.
 * @return true if changed.
 */
bool z906_state_set_effect(z906_effect_t effect);

/**
 * @brief Updates power state.
 * @param power true if powered on, false if in standby.
 * @return true if changed.
 */
bool z906_state_set_power(bool power);

/**
 * @brief Updates mute state.
 * @param mute true if muted.
 * @return true if changed.
 */
bool z906_state_set_mute(bool mute);

/**
 * @brief Updates digital signal status code.
 * @param status Signal status code.
 * @return true if changed.
 */
bool z906_state_set_digital_signal_status(uint8_t status);

/**
 * @brief Updates auto-standby enabled status.
 * @param enabled true if auto-standby enabled.
 * @return true if changed.
 */
bool z906_state_set_auto_standby(bool enabled);

/**
 * @brief Updates amplifier temperature telemetry from a verified protocol measurement.
 *
 * Sets temp_valid = true to signal that at least one successful integrity-verified
 * temperature query result is available.
 * @param temps Array of 3 temperatures in Celsius (Index 2 is Power Supply Transformer).
 * @return true if state changed.
 */
bool z906_state_set_temperature(const int8_t temps[3]);

/**
 * @brief Updates amplifier idle seconds telemetry.
 * @param seconds Idle seconds.
 * @return true if state changed.
 */
bool z906_state_set_idle_seconds(uint32_t seconds);

/**
 * @brief Updates raw U24BE input signal level telemetry.
 * @param level Raw level from 0x2F query.
 * @return true if state changed.
 */
bool z906_state_set_input_signal_level(uint32_t level);

/**
 * @brief Updates cable presence state.
 * @param connected true if DE-15 cable is detected.
 */
void z906_state_set_cable_status(bool connected);

/**
 * @brief Sets the validity of main control hardware state (power, volume, trims, etc.).
 * @param valid true once first complete status configuration frame is processed.
 */
void z906_state_set_main_valid(bool valid);

/**
 * @brief Checks if main control hardware state has been verified via status frame.
 * @return true if verified status frame received and cable connected, false otherwise.
 */
bool z906_state_is_main_valid(void);

/**
 * @brief Monotonic timestamp of the last integrity-verified CONFIG frame.
 * @return Microseconds from esp_timer_get_time(), or 0 when cable/status is invalid.
 *         Serialized by the existing state mutex; no dynamic allocations.
 */
int64_t z906_state_get_main_status_time_us(void);

/**
 * @brief Records a status response matched to a live telemetry STATUS request.
 * Called after verified CONFIG decoding, by the telemetry response owner only.
 * @param response_time_us Monotonic reception time, or ignored if invalid.
 */
void z906_state_note_matched_status_reply(int64_t response_time_us);

/** @brief Returns the last matched live STATUS reply timestamp, or 0 if invalid. */
int64_t z906_state_get_matched_status_reply_time_us(void);

/**
 * @brief Callback type for state change observers.
 *
 * Called from within the state mutex immediately after any attribute changes.
 * The callback MUST be non-blocking and ISR-safe (no mutex acquisition, no I/O).
 * Typically used to post a FreeRTOS task notification or set a flag.
 *
 * @param change_mask Bitmask of changed attributes (ZB_PUB_NOTIF_* bits).
 *                    The exact bit definitions live in zigbee_state_publisher.h;
 *                    the state manager passes the raw mask value and does not
 *                    interpret it. Callers must define and pass the correct mask.
 */
typedef void (*z906_state_change_cb_t)(uint32_t change_mask);

/**
 * @brief Registers a single global state change observer callback.
 *
 * Only one observer is supported. A second call overwrites the previous registration.
 * Pass NULL to deregister.
 *
 * WHY ONE OBSERVER: The only consumer of state change notifications in this system
 * is zigbee_state_publisher. A single-slot design prevents unbounded observer sprawl,
 * eliminates array management, and keeps the state manager minimal.
 *
 * @param cb Callback function pointer, or NULL to deregister.
 */
void z906_state_register_change_observer(z906_state_change_cb_t cb);

#ifdef __cplusplus
}
#endif

#endif /* Z906_STATE_H */
