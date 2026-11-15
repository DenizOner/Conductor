/**
 * @file z906_codec.h
 * @brief Packet framing, stream parser, and LRC checksum validator for Z906 UART protocol.
 *
 * Implements strict Longitudinal Redundancy Check (LRC) validation and parsing of
 * 20-byte amplifier status configuration frames (AA 0A 14 ...) and telemetry frames
 * (temperature, idle time, input volume) per canonical reference (protocol.rst).
 */

#ifndef Z906_CODEC_H
#define Z906_CODEC_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "z906_commands.h"

#ifdef __cplusplus
extern "C" {
#endif

#define Z906_MAX_PAYLOAD_LEN 64

/** @brief Decoded configuration payload extracted from 20-byte amplifier status frame (AA 0A 14) */
typedef struct {
    uint8_t volume;                 /**< Master volume (0 - 43) */
    uint8_t volume_rear;            /**< Rear surround level (0 - 43) */
    uint8_t volume_center;          /**< Centre speaker level (0 - 43) */
    uint8_t volume_bass;            /**< Subwoofer bass level (0 - 43) */
    z906_input_t input;             /**< Confirmed input source (1 - 6) */
    bool mute;                      /**< True if audio is muted */
    z906_effect_t effect;           /**< Confirmed spatial effect on active input */
    uint8_t digital_signal_status;  /**< Digital signal status code (Byte 12, protocol.rst) */
    bool standby;                   /**< True if system is in STANDBY mode (Byte 17 != 0) */
    bool auto_standby_enabled;      /**< True if automatic standby timer is enabled (Byte 19 == 0) */
    uint8_t raw_payload[Z906_MAX_PAYLOAD_LEN];
    uint8_t raw_len;
} z906_amp_config_t;

/** @brief Decoded amplifier temperature telemetry (AA 0C 05 ...) */
typedef struct {
    int8_t temp_celsius[3];         /**< Temperatures 1, 2, and 3 in Celsius (Index 2: Transformer) */
    bool is_valid;
} z906_amp_temp_t;

/** @brief Decoded amplifier idle timer telemetry (AA 0F 03 ...) */
typedef struct {
    uint32_t idle_seconds;          /**< U24BE idle duration in seconds */
    bool is_valid;
} z906_amp_idle_t;

/** @brief Decoded amplifier input audio volume amplitude (AA 08 03 ...) */
typedef struct {
    uint32_t input_volume;          /**< U24BE audio amplitude */
    bool is_valid;
} z906_amp_input_vol_t;

/** @brief Classified multi-byte packet type identifiers */
typedef enum {
    Z906_PACKET_NONE = 0,
    Z906_PACKET_CONFIG,
    Z906_PACKET_TEMP,
    Z906_PACKET_IDLE,
    Z906_PACKET_INPUT_VOL
} z906_packet_type_t;

/** @brief Unified decoded multi-byte packet container */
typedef struct {
    z906_packet_type_t type;
    union {
        z906_amp_config_t config;
        z906_amp_temp_t temp;
        z906_amp_idle_t idle;
        z906_amp_input_vol_t input_vol;
    } data;
} z906_decoded_packet_t;

/** @brief Parser state machine stages */
typedef enum {
    CODEC_STATE_IDLE = 0,
    CODEC_STATE_WAIT_TYPE,
    CODEC_STATE_WAIT_LEN,
    CODEC_STATE_WAIT_PAYLOAD,
    CODEC_STATE_WAIT_LRC
} z906_codec_state_t;

/** @brief Stream classification result for received raw bytes */
typedef enum {
    Z906_PARSE_RESULT_STANDALONE = 0,   /**< Byte is not part of any multi-byte frame; candidate for single-byte dispatch */
    Z906_PARSE_RESULT_IN_PROGRESS,      /**< Byte consumed into active multi-byte frame header/payload/LRC */
    Z906_PARSE_RESULT_FRAME_COMPLETE,   /**< Multi-byte frame successfully parsed with verified LRC */
    Z906_PARSE_RESULT_RESYNC_DROP       /**< Frame discarded due to invalid length, checksum mismatch, or resync */
} z906_parse_result_t;

/** @brief Stream decoder context instance */
typedef struct {
    z906_codec_state_t state;
    uint8_t type;
    uint8_t expected_len;
    uint8_t bytes_read;
    uint8_t buffer[Z906_MAX_PAYLOAD_LEN];
} z906_codec_ctx_t;

/**
 * @brief Resets decoder context to idle state.
 */
void z906_codec_init(z906_codec_ctx_t *ctx);

/**
 * @brief Checks if the codec is currently accumulating an in-flight multi-byte frame.
 * @param ctx Parser context.
 * @return true if currently waiting for header, length, payload, or LRC; false if in CODEC_STATE_IDLE.
 */
bool z906_codec_is_frame_in_progress(const z906_codec_ctx_t *ctx);

/**
 * @brief Computes 8-bit Longitudinal Redundancy Check (LRC) over a contiguous byte buffer.
 * Formula: (~sum) + 1 (Two's complement checksum).
 */
uint8_t z906_codec_calc_lrc(const uint8_t *data, size_t len);

/**
 * @brief Computes LRC for a framed packet spanning type, length, and payload.
 *
 * Per protocol.rst: "The checksum is verified by calculating the U8 sum of all bytes
 * from the type (inclusive) to the checksum (inclusive) so that the final result is 0."
 */
uint8_t z906_codec_calc_frame_lrc(uint8_t type, uint8_t len, const uint8_t *payload, size_t payload_len);

/**
 * @brief Verifies whether the frame sum (type + len + payload + lrc_byte) equals 0 modulo 256.
 */
bool z906_codec_verify_lrc(uint8_t type, uint8_t len, const uint8_t *payload, size_t payload_len, uint8_t lrc_byte);

/**
 * @brief Feeds a single byte into the decoder stream and returns fine-grained stream classification.
 *
 * Distinguishes standalone single bytes from multi-byte frame payload bytes and completed frames.
 *
 * @param ctx Parser context.
 * @param byte Input byte from UART.
 * @param out_packet Output struct populated when any valid packet finishes (can be NULL).
 * @return Stream classification result (z906_parse_result_t).
 */
z906_parse_result_t z906_codec_feed_byte(z906_codec_ctx_t *ctx, uint8_t byte, z906_decoded_packet_t *out_packet);

/**
 * @brief Feeds a single byte into the decoder stream and parses any completed packet.
 * Backwards-compatible wrapper returning true on complete verified frame.
 *
 * @param ctx Parser context.
 * @param byte Input byte from UART.
 * @param out_packet Output struct populated when any valid packet finishes.
 * @return true if a complete, checksum-verified packet was parsed, false otherwise.
 */
bool z906_codec_parse_packet(z906_codec_ctx_t *ctx, uint8_t byte, z906_decoded_packet_t *out_packet);

/**
 * @brief Feeds a single byte from UART into the decoder stream (backwards-compatible wrapper).
 *
 * @param ctx Parser context.
 * @param byte Input byte from UART.
 * @param out_config Output struct populated when a valid CONFIG_AMP packet finishes.
 * @return true if a complete, checksum-verified CONFIG_AMP frame was parsed, false otherwise.
 */
bool z906_codec_parse_byte(z906_codec_ctx_t *ctx, uint8_t byte, z906_amp_config_t *out_config);

/**
 * @brief Decodes an amplifier temperature packet (Type 0x0C, Length 5).
 */
bool z906_codec_decode_temperature(const uint8_t *payload, size_t len, z906_amp_temp_t *out_temp);

/**
 * @brief Decodes an amplifier idle duration packet (Type 0x0F, Length 3).
 */
bool z906_codec_decode_idle(const uint8_t *payload, size_t len, z906_amp_idle_t *out_idle);

/**
 * @brief Decodes an amplifier input audio volume amplitude packet (Type 0x08, Length 3).
 */
bool z906_codec_decode_input_volume(const uint8_t *payload, size_t len, z906_amp_input_vol_t *out_vol);

#ifdef __cplusplus
}
#endif

#endif /* Z906_CODEC_H */
