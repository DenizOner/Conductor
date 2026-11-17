/**
 * @file test_codec.c
 * @brief Unit tests for Z906 packet framing, multi-byte parsing, and LRC checksums.
 *
 * All test fixtures use independent reference data directly extracted from canonical
 * reverse engineering documentation (protocol.rst):
 * - Example frame from protocol.rst line 661-662 (Type 0x0A, Len 0x14, Checksum 0x8C)
 * - Idle time sample from protocol.rst line 181 (Type 0x0F, Len 0x03, Payload 0x00061C)
 * - Temperature sample (Type 0x0C, Len 0x05, Index 2: Power Supply Transformer)
 * - Input volume sample (Type 0x08, Len 0x03)
 */

/* Include Unity test framework header */
#include <unity.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
/* Include Z906 codec header under test */
#include "z906_codec.h"
/* Include protocol command and constants header */
#include "z906_commands.h"

/**
 * @brief Set up test fixtures before each test execution.
 */
void setUp(void)
{
    /* No persistent state between tests */
}

/**
 * @brief Clean up test fixtures after each test execution.
 */
void tearDown(void)
{
    /* No cleanup required */
}

/**
 * @brief Tests Longitudinal Redundancy Check (LRC) calculation against canonical reference.
 * protocol.rst Line 661-662 gives exact sum and checksum 0x8C.
 */
void test_z906_lrc_canonical_formula(void)
{
    /* Canonical payload from protocol.rst line 661:
     * Type: 0x0A, Len: 0x14
     * Data: 0A 15 15 15 00 00 00 00 00 00 03 00 00 00 06 01 03 00 00 00
     * Expected checksum: 0x8C
     */
    uint8_t payload[20] = {
        0x0A, 0x15, 0x15, 0x15, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x03, 0x00, 0x00, 0x00, 0x06, 0x01, 0x03, 0x00, 0x00, 0x00
    };

    /* Calculate frame LRC using two's complement formula */
    uint8_t lrc = z906_codec_calc_frame_lrc(0x0A, 0x14, payload, sizeof(payload));
    /* Assert calculated checksum matches canonical value 0x8C */
    TEST_ASSERT_EQUAL_HEX8(0x8C, lrc);

    /* Verify LRC helper validates true for 0x8C */
    TEST_ASSERT_TRUE(z906_codec_verify_lrc(0x0A, 0x14, payload, sizeof(payload), 0x8C));

    /* Verify LRC helper rejects any corrupted checksum */
    TEST_ASSERT_FALSE(z906_codec_verify_lrc(0x0A, 0x14, payload, sizeof(payload), 0x8D));
}

/**
 * @brief Tests streaming byte parser for full 20-byte CONFIG_AMP multi-byte reply packets.
 * Uses exact canonical frame from protocol.rst line 671:
 * AA 0A 14 0A 15 15 15 00 00 00 00 00 00 03 00 00 00 06 01 03 00 00 00 8C
 */
void test_z906_codec_parse_canonical_amp_config(void)
{
    /* Declare codec context instance */
    z906_codec_ctx_t ctx;
    /* Initialize codec context */
    z906_codec_init(&ctx);

    /* Byte stream representing the canonical config packet */
    uint8_t stream[] = {
        0xAA, 0x0A, 0x14,
        0x0A, 0x15, 0x15, 0x15, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x03, 0x00,
        0x00, 0x00, 0x06, 0x01, 0x03, 0x00,
        0x00, 0x00,
        0x8C /* Canonical reference checksum */
    };

    /* Structure to receive decoded configuration */
    z906_amp_config_t config;
    /* Flag tracking whether frame reached completion */
    bool frame_complete = false;

    /* Feed stream byte-by-byte into the parser */
    for (size_t i = 0; i < sizeof(stream); i++) {
        if (z906_codec_parse_byte(&ctx, stream[i], &config)) {
            frame_complete = true;
            break;
        }
    }

    /* Assert that frame completed successfully */
    TEST_ASSERT_TRUE(frame_complete);
    /* Verify decoded master volume */
    TEST_ASSERT_EQUAL_UINT8(10, config.volume);
    /* Verify rear surround volume */
    TEST_ASSERT_EQUAL_UINT8(21, config.volume_rear);
    /* Verify centre speaker volume */
    TEST_ASSERT_EQUAL_UINT8(21, config.volume_center);
    /* Verify subwoofer bass volume */
    TEST_ASSERT_EQUAL_UINT8(21, config.volume_bass);
    /* Verify active input — canonical frame byte 0x0A maps to 6-Channel Direct (Input 1) */
    TEST_ASSERT_EQUAL(Z906_INPUT_SIX_CHANNEL_DIRECT, config.input);
    /* Verify mute state */
    TEST_ASSERT_FALSE(config.mute);
    /* Verify standby state (Byte 17 is 0 = ON) */
    TEST_ASSERT_FALSE(config.standby);
    /* Verify auto standby timer state (Byte 19 is 0 = Enabled) */
    TEST_ASSERT_TRUE(config.auto_standby_enabled);
}

/**
 * @brief Tests that corrupted checksums cause frame rejection (zero assumption security).
 */
void test_z906_codec_reject_corrupted_checksum(void)
{
    /* Declare codec context instance */
    z906_codec_ctx_t ctx;
    /* Initialize codec context */
    z906_codec_init(&ctx);

    /* Frame identical to canonical, but with corrupted trailing checksum 0x00 instead of 0x8C */
    uint8_t stream[] = {
        0xAA, 0x0A, 0x14,
        0x0A, 0x15, 0x15, 0x15, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x03, 0x00,
        0x00, 0x00, 0x06, 0x01, 0x03, 0x00,
        0x00, 0x00,
        0x00 /* Bad checksum */
    };

    /* Structure to receive decoded configuration */
    z906_amp_config_t config;
    /* Flag tracking whether frame reached completion */
    bool frame_complete = false;

    /* Feed corrupted stream into parser */
    for (size_t i = 0; i < sizeof(stream); i++) {
        if (z906_codec_parse_byte(&ctx, stream[i], &config)) {
            frame_complete = true;
            break;
        }
    }

    /* Assert that frame was rejected due to checksum error */
    TEST_ASSERT_FALSE(frame_complete);
}

/**
 * @brief Tests that Standby state (Byte 17 == 1) is accurately extracted.
 */
void test_z906_codec_standby_flag_extraction(void)
{
    /* Declare codec context instance */
    z906_codec_ctx_t ctx;
    /* Initialize codec context */
    z906_codec_init(&ctx);

    /* Byte 17 set to 0x01 (Standby mode) */
    uint8_t payload[20] = {
        0x0A, 0x15, 0x15, 0x15, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x03, 0x00,
        0x00, 0x00, 0x06, 0x01, 0x03,
        0x01, /* Standby = 1 */
        0x00, 0x00
    };

    /* Calculate valid LRC for this modified payload */
    uint8_t lrc = z906_codec_calc_frame_lrc(0x0A, 0x14, payload, sizeof(payload));

    /* Assemble complete UART byte sequence */
    uint8_t stream[24];
    stream[0] = 0xAA;
    stream[1] = 0x0A;
    stream[2] = 0x14;
    for (int i = 0; i < 20; i++) stream[3 + i] = payload[i];
    stream[23] = lrc;

    /* Output struct */
    z906_amp_config_t config;
    /* Completion flag */
    bool frame_complete = false;

    /* Feed stream into parser */
    for (size_t i = 0; i < sizeof(stream); i++) {
        if (z906_codec_parse_byte(&ctx, stream[i], &config)) {
            frame_complete = true;
            break;
        }
    }

    /* Verify frame was parsed */
    TEST_ASSERT_TRUE(frame_complete);
    /* Verify standby flag was correctly identified */
    TEST_ASSERT_TRUE(config.standby);
}

/**
 * @brief Tests amplifier temperature telemetry frame decoding (Type 0x0C, Len 5).
 * Note: Index 2 represents the power supply transformer temperature.
 */
void test_z906_codec_decode_temperature(void)
{
    /* Format: <00> <T1> <T2> <T3> <00> (Celsius, T3 is power transformer) */
    uint8_t temp_payload[] = { 0x00, 28, 35, 42, 0x00 };
    z906_amp_temp_t temp;

    /* Decode raw payload */
    bool ok = z906_codec_decode_temperature(temp_payload, sizeof(temp_payload), &temp);
    /* Assert success */
    TEST_ASSERT_TRUE(ok);
    /* Assert validity flag */
    TEST_ASSERT_TRUE(temp.is_valid);
    /* Verify heatsink temperature 1 */
    TEST_ASSERT_EQUAL_INT8(28, temp.temp_celsius[0]);
    /* Verify heatsink temperature 2 */
    TEST_ASSERT_EQUAL_INT8(35, temp.temp_celsius[1]);
    /* Verify power supply transformer temperature 3 */
    TEST_ASSERT_EQUAL_INT8(42, temp.temp_celsius[2]);
}

/**
 * @brief Tests amplifier idle timer telemetry frame decoding (Type 0x0F, Len 3).
 * Uses sample from protocol.rst line 181: 0x00 0x06 0x1C = 1564 seconds (~26 minutes).
 */
void test_z906_codec_decode_idle(void)
{
    /* 3-byte big-endian payload: 0x00 0x06 0x1C */
    uint8_t idle_payload[] = { 0x00, 0x06, 0x1C };
    z906_amp_idle_t idle;

    /* Decode raw payload */
    bool ok = z906_codec_decode_idle(idle_payload, sizeof(idle_payload), &idle);
    /* Assert success */
    TEST_ASSERT_TRUE(ok);
    /* Assert validity flag */
    TEST_ASSERT_TRUE(idle.is_valid);
    /* Verify converted seconds */
    TEST_ASSERT_EQUAL_UINT32(1564, idle.idle_seconds);
}

/**
 * @brief Tests amplifier input volume amplitude frame decoding (Type 0x08, Len 3).
 */
void test_z906_codec_decode_input_volume(void)
{
    /* 3-byte big-endian payload: 0x01 0x02 0x03 = 0x010203 = 66051 */
    uint8_t vol_payload[] = { 0x01, 0x02, 0x03 };
    z906_amp_input_vol_t vol;

    /* Decode raw payload */
    bool ok = z906_codec_decode_input_volume(vol_payload, sizeof(vol_payload), &vol);
    /* Assert success */
    TEST_ASSERT_TRUE(ok);
    /* Assert validity flag */
    TEST_ASSERT_TRUE(vol.is_valid);
    /* Verify converted 24-bit raw magnitude */
    TEST_ASSERT_EQUAL_UINT32(66051, vol.input_volume);
}

/**
 * @brief Tests multi-packet unified stream parser (z906_codec_parse_packet).
 * Validates decoding of temperature frame (Type 0x0C, Len 0x05) through the unified API.
 */
void test_z906_codec_parse_packet_temperature(void)
{
    /* Initialize codec context */
    z906_codec_ctx_t ctx;
    z906_codec_init(&ctx);

    /* Temperature payload: <00> <T1=30> <T2=36> <T3=45> <00> (T3: transformer) */
    uint8_t payload[5] = { 0x00, 30, 36, 45, 0x00 };
    /* Calculate valid LRC: type 0x0C, len 0x05 */
    uint8_t lrc = z906_codec_calc_frame_lrc(0x0C, 0x05, payload, sizeof(payload));

    /* Assemble UART frame: AA 0C 05 00 1E 24 2D 00 <LRC> */
    uint8_t stream[9];
    stream[0] = 0xAA;
    stream[1] = 0x0C;
    stream[2] = 0x05;
    for (int i = 0; i < 5; i++) stream[3 + i] = payload[i];
    stream[8] = lrc;

    /* Decoded packet container */
    z906_decoded_packet_t packet;
    bool completed = false;

    /* Stream bytes through parser */
    for (size_t i = 0; i < sizeof(stream); i++) {
        if (z906_codec_parse_packet(&ctx, stream[i], &packet)) {
            completed = true;
            break;
        }
    }

    /* Assert that packet was fully parsed */
    TEST_ASSERT_TRUE(completed);
    /* Assert packet type is temperature */
    TEST_ASSERT_EQUAL(Z906_PACKET_TEMP, packet.type);
    /* Verify sensor values */
    TEST_ASSERT_EQUAL_INT8(30, packet.data.temp.temp_celsius[0]);
    TEST_ASSERT_EQUAL_INT8(36, packet.data.temp.temp_celsius[1]);
    TEST_ASSERT_EQUAL_INT8(45, packet.data.temp.temp_celsius[2]);
}

/**
 * @brief Tests that a temperature packet with a bad checksum is rejected entirely.
 * No temperature values must be produced from an integrity-failed packet.
 */
void test_z906_codec_temperature_bad_checksum_rejected(void)
{
    z906_codec_ctx_t ctx;
    z906_codec_init(&ctx);

    /* Valid temp payload: 00 1E 24 2D 00 (30, 36, 45 °C) */
    uint8_t payload[5] = { 0x00, 30, 36, 45, 0x00 };
    /* Deliberately corrupt the checksum byte (use 0xFF instead of computed LRC) */
    uint8_t stream[9];
    stream[0] = 0xAA;
    stream[1] = 0x0C;
    stream[2] = 0x05;
    for (int i = 0; i < 5; i++) stream[3 + i] = payload[i];
    stream[8] = 0xFF; /* Bad checksum — must cause rejection */

    z906_decoded_packet_t packet;
    bool completed = false;

    for (size_t i = 0; i < sizeof(stream); i++) {
        if (z906_codec_parse_packet(&ctx, stream[i], &packet)) {
            completed = true;
            break;
        }
    }

    /* Packet must be rejected — no valid temperature must be produced */
    TEST_ASSERT_FALSE(completed);
}

/**
 * @brief Tests that a multi-byte packet with the wrong type byte does not produce
 * a temperature result. A CONFIG_AMP type (0x0A) must not be misinterpreted as TEMP (0x0C).
 */
void test_z906_codec_temperature_wrong_type_not_decoded_as_temp(void)
{
    z906_codec_ctx_t ctx;
    z906_codec_init(&ctx);

    /* Full canonical CONFIG_AMP frame (Type 0x0A, not 0x0C) */
    uint8_t stream[] = {
        0xAA, 0x0A, 0x14,
        0x0A, 0x15, 0x15, 0x15, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x03, 0x00,
        0x00, 0x00, 0x06, 0x01, 0x03, 0x00,
        0x00, 0x00,
        0x8C
    };

    z906_decoded_packet_t packet;
    bool completed = false;

    for (size_t i = 0; i < sizeof(stream); i++) {
        if (z906_codec_parse_packet(&ctx, stream[i], &packet)) {
            completed = true;
            break;
        }
    }

    /* A CONFIG_AMP packet must be parsed, but must NOT produce Z906_PACKET_TEMP */
    TEST_ASSERT_TRUE(completed);
    TEST_ASSERT_NOT_EQUAL(Z906_PACKET_TEMP, packet.type);
    TEST_ASSERT_EQUAL(Z906_PACKET_CONFIG, packet.type);
}

/**
 * @brief Tests that z906_codec_decode_temperature rejects a truncated payload (< 5 bytes).
 * A payload shorter than 5 bytes must produce is_valid=false and return false.
 */
void test_z906_codec_temperature_truncated_payload_rejected(void)
{
    /* Only 3 bytes instead of required 5 — simulates a truncated or corrupted frame */
    uint8_t short_payload[3] = { 0x00, 28, 35 };
    z906_amp_temp_t temp;

    bool ok = z906_codec_decode_temperature(short_payload, sizeof(short_payload), &temp);
    /* Must return false — truncated payload is invalid */
    TEST_ASSERT_FALSE(ok);
}

/**
 * @brief Tests that a valid 0 °C temperature measurement is correctly decoded and accepted.
 * This is Test A (Section 11.3): valid 0 °C must NOT be treated as a communication error.
 * The is_valid flag must be true even when all three temperature channels read 0.
 */
void test_z906_codec_temperature_valid_zero_celsius_accepted(void)
{
    /* Payload: 00 00 00 00 00 — all three temperatures are 0 °C */
    uint8_t payload[5] = { 0x00, 0x00, 0x00, 0x00, 0x00 };
    z906_amp_temp_t temp;

    bool ok = z906_codec_decode_temperature(payload, sizeof(payload), &temp);
    /* Must succeed — 0 °C is a valid physical temperature */
    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_TRUE(temp.is_valid);
    /* All three channels must report 0 — not a sentinel error value */
    TEST_ASSERT_EQUAL_INT8(0, temp.temp_celsius[0]);
    TEST_ASSERT_EQUAL_INT8(0, temp.temp_celsius[1]);
    TEST_ASSERT_EQUAL_INT8(0, temp.temp_celsius[2]);
}

/**
 * @brief Entry point for Unity test suite execution.
 */
/** @brief A checksum-correct but extended 21-byte CONFIG must be rejected. */
void test_z906_codec_reject_extended_config(void)
{
    const uint8_t frame_payload[21] = {
        10, 21, 21, 21, 0, 0, 0, 0, 0, 0, 3, 0,
        0, 0, 6, 1, 3, 0, 0, 0, 0
    };
    uint8_t frame[25] = { 0xAA, 0x0A, 21 };
    memcpy(&frame[3], frame_payload, sizeof(frame_payload));
    frame[24] = z906_codec_calc_frame_lrc(0x0A, 21, frame_payload, 21);
    z906_codec_ctx_t ctx;
    z906_decoded_packet_t decoded;
    z906_codec_init(&ctx);
    for (size_t i = 0; i < sizeof(frame) - 1; i++) {
        TEST_ASSERT_EQUAL(Z906_PARSE_RESULT_IN_PROGRESS,
                          z906_codec_feed_byte(&ctx, frame[i], &decoded));
    }
    TEST_ASSERT_EQUAL(Z906_PARSE_RESULT_RESYNC_DROP,
                      z906_codec_feed_byte(&ctx, frame[24], &decoded));
    TEST_ASSERT_FALSE(z906_codec_is_frame_in_progress(&ctx));
}

/** @brief Invalid physical CONFIG fields cannot be legitimized by recomputed LRC. */
void test_z906_codec_reject_semantically_invalid_config(void)
{
    static const uint8_t canonical[20] = {
        0x0A, 0x15, 0x15, 0x15, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x03, 0x00,
        0x00, 0x00, 0x06, 0x01, 0x03, 0x00,
        0x00, 0x00
    };
    const uint8_t bad_indices[] = { 0, 1, 2, 3, 4, 5, 6, 17, 19 };
    const uint8_t bad_values[]  = { 44, 44, 44, 44, 6, 2, 4, 2, 2 };
    for (size_t n = 0; n < sizeof(bad_indices); n++) {
        uint8_t data[20];
        memcpy(data, canonical, sizeof(data));
        data[bad_indices[n]] = bad_values[n];
        uint8_t frame[24] = { 0xAA, 0x0A, 0x14 };
        memcpy(&frame[3], data, 20);
        frame[23] = z906_codec_calc_frame_lrc(0x0A, 20, data, 20);
        z906_codec_ctx_t ctx;
        z906_decoded_packet_t decoded;
        z906_codec_init(&ctx);
        for (size_t i = 0; i < sizeof(frame) - 1; i++) {
            (void)z906_codec_feed_byte(&ctx, frame[i], &decoded);
        }
        TEST_ASSERT_EQUAL(Z906_PARSE_RESULT_RESYNC_DROP,
                          z906_codec_feed_byte(&ctx, frame[23], &decoded));
    }
}

int runUnityTests(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_z906_lrc_canonical_formula);
    RUN_TEST(test_z906_codec_parse_canonical_amp_config);
    RUN_TEST(test_z906_codec_reject_extended_config);
    RUN_TEST(test_z906_codec_reject_semantically_invalid_config);
    RUN_TEST(test_z906_codec_reject_corrupted_checksum);
    RUN_TEST(test_z906_codec_standby_flag_extraction);
    RUN_TEST(test_z906_codec_decode_temperature);
    RUN_TEST(test_z906_codec_decode_idle);
    RUN_TEST(test_z906_codec_decode_input_volume);
    RUN_TEST(test_z906_codec_parse_packet_temperature);
    RUN_TEST(test_z906_codec_temperature_bad_checksum_rejected);
    RUN_TEST(test_z906_codec_temperature_wrong_type_not_decoded_as_temp);
    RUN_TEST(test_z906_codec_temperature_truncated_payload_rejected);
    RUN_TEST(test_z906_codec_temperature_valid_zero_celsius_accepted);
    return UNITY_END();
}


#if defined(ESP_PLATFORM)
/**
 * @brief ESP-IDF application entry point for testing.
 */
void app_main(void)
{
    vTaskDelay(pdMS_TO_TICKS(3000)); /* Allow test runner to open serial port */
    runUnityTests();
}
#else
/**
 * @brief Host-based test runner entry point.
 */
int main(void)
{
    return runUnityTests();
}
#endif
