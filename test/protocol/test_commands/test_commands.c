/**
 * @file test_commands.c
 * @brief Unit tests for Z906 volume scale calculations and command header mappings.
 *
 * Implements Rule 12.2: Tests use independent reference values directly from
 * protocol.rst rather than echoing the implementation's own enum symbols.
 */

#include <unity.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "z906_commands.h"
#include "config.h"

void setUp(void) {}
void tearDown(void) {}

/**
 * @brief Tests bidirectional mapping between hardware volume (0-43) and percent (0-100%).
 */
void test_volume_percentage_conversions(void)
{
    /* Minimum boundary */
    TEST_ASSERT_EQUAL_UINT8(0, z906_cmd_volume_to_percent(0));
    TEST_ASSERT_EQUAL_UINT8(0, z906_cmd_percent_to_volume(0));

    /* Maximum boundary */
    TEST_ASSERT_EQUAL_UINT8(100, z906_cmd_volume_to_percent(43));
    TEST_ASSERT_EQUAL_UINT8(43, z906_cmd_percent_to_volume(100));

    /* Midpoint (~50%) */
    uint8_t mid_vol = 21;
    uint8_t mid_pct = z906_cmd_volume_to_percent(mid_vol);
    TEST_ASSERT_INT_WITHIN(2, 49, mid_pct);
    TEST_ASSERT_EQUAL_UINT8(mid_vol, z906_cmd_percent_to_volume(mid_pct));

    /* Clamping above bounds */
    TEST_ASSERT_EQUAL_UINT8(100, z906_cmd_volume_to_percent(50));
    TEST_ASSERT_EQUAL_UINT8(43, z906_cmd_percent_to_volume(150));
}

/**
 * @brief Tests bidirectional mapping between hardware volume (0-43) and Zigbee ZCL levels (0-254).
 */
void test_volume_zcl_conversions(void)
{
    TEST_ASSERT_EQUAL_UINT8(0, z906_cmd_volume_to_zcl(0));
    TEST_ASSERT_EQUAL_UINT8(0, z906_cmd_zcl_to_volume(0));

    TEST_ASSERT_EQUAL_UINT8(254, z906_cmd_volume_to_zcl(43));
    TEST_ASSERT_EQUAL_UINT8(43, z906_cmd_zcl_to_volume(254));
}

/**
 * @brief Tests input selection mapping against literal reference opcodes from protocol.rst.
 *
 * Canonical input mapping (protocol opcodes are NOT sequential with input numbers):
 *   Z906_INPUT_SIX_CHANNEL_DIRECT (Input 1) -> 0x02: 6-Channel Direct, three 3.5mm TRS
 *   Z906_INPUT_RCA_STEREO         (Input 2) -> 0x05: RCA Stereo, red/white RCA
 *   Z906_INPUT_OPTICAL_1          (Input 3) -> 0x03: Optical 1, first TOSLINK
 *   Z906_INPUT_OPTICAL_2          (Input 4) -> 0x04: Optical 2, second TOSLINK
 *   Z906_INPUT_DIGITAL_COAXIAL    (Input 5) -> 0x06: Digital Coaxial, 75-ohm S/PDIF
 *   Z906_INPUT_AUXILIARY          (Input 6) -> 0x07: Auxiliary (AUX), 3.5mm console side
 */
void test_canonical_input_opcodes(void)
{
    /* Canonical reference values from protocol.rst Section 'Input selection' */
    TEST_ASSERT_EQUAL_HEX8(0x02, (uint8_t)z906_cmd_from_input(Z906_INPUT_SIX_CHANNEL_DIRECT));  /* Input 1: 6-Channel Direct */
    TEST_ASSERT_EQUAL_HEX8(0x05, (uint8_t)z906_cmd_from_input(Z906_INPUT_RCA_STEREO));           /* Input 2: RCA Stereo */
    TEST_ASSERT_EQUAL_HEX8(0x03, (uint8_t)z906_cmd_from_input(Z906_INPUT_OPTICAL_1));            /* Input 3: Optical 1 */
    TEST_ASSERT_EQUAL_HEX8(0x04, (uint8_t)z906_cmd_from_input(Z906_INPUT_OPTICAL_2));            /* Input 4: Optical 2 */
    TEST_ASSERT_EQUAL_HEX8(0x06, (uint8_t)z906_cmd_from_input(Z906_INPUT_DIGITAL_COAXIAL));      /* Input 5: Digital Coaxial */
    TEST_ASSERT_EQUAL_HEX8(0x07, (uint8_t)z906_cmd_from_input(Z906_INPUT_AUXILIARY));            /* Input 6: Auxiliary (AUX) */
}

/**
 * @brief Tests effect selection mapping against literal reference opcodes from protocol.rst.
 */
void test_canonical_effect_opcodes(void)
{
    /* Canonical reference values from protocol.rst Section 'Effect selection' */
    TEST_ASSERT_EQUAL_HEX8(0x14, (uint8_t)z906_cmd_from_effect(Z906_EFFECT_3D));
    TEST_ASSERT_EQUAL_HEX8(0x15, (uint8_t)z906_cmd_from_effect(Z906_EFFECT_4_1));
    TEST_ASSERT_EQUAL_HEX8(0x16, (uint8_t)z906_cmd_from_effect(Z906_EFFECT_2_1));
    TEST_ASSERT_EQUAL_HEX8(0x35, (uint8_t)z906_cmd_from_effect(Z906_EFFECT_NONE));
}

/**
 * @brief Tests all core single-byte protocol opcodes against independent literal reference constants.
 */
void test_canonical_protocol_opcodes(void)
{
    /* Volume / Level opcodes */
    TEST_ASSERT_EQUAL_HEX8(0x08, (uint8_t)Z906_CMD_VOLUME_UP);
    TEST_ASSERT_EQUAL_HEX8(0x09, (uint8_t)Z906_CMD_VOLUME_DOWN);
    TEST_ASSERT_EQUAL_HEX8(0x0A, (uint8_t)Z906_CMD_VOLUME_BASS_UP);
    TEST_ASSERT_EQUAL_HEX8(0x0B, (uint8_t)Z906_CMD_VOLUME_BASS_DOWN);
    TEST_ASSERT_EQUAL_HEX8(0x0C, (uint8_t)Z906_CMD_VOLUME_CENTER_UP);
    TEST_ASSERT_EQUAL_HEX8(0x0D, (uint8_t)Z906_CMD_VOLUME_CENTER_DOWN);
    TEST_ASSERT_EQUAL_HEX8(0x0E, (uint8_t)Z906_CMD_VOLUME_REAR_UP);
    TEST_ASSERT_EQUAL_HEX8(0x0F, (uint8_t)Z906_CMD_VOLUME_REAR_DOWN);

    /* Telemetry and state opcodes */
    TEST_ASSERT_EQUAL_HEX8(0x20, (uint8_t)Z906_CMD_AMP_MUTED);
    TEST_ASSERT_EQUAL_HEX8(0x21, (uint8_t)Z906_CMD_AMP_UNMUTED);
    TEST_ASSERT_EQUAL_HEX8(0x22, (uint8_t)Z906_CMD_START_TEST);
    TEST_ASSERT_EQUAL_HEX8(0x25, (uint8_t)Z906_CMD_READ_TEMP);
    TEST_ASSERT_EQUAL_HEX8(0x2F, (uint8_t)Z906_CMD_READ_INPUT_VOL);
    TEST_ASSERT_EQUAL_HEX8(0x30, (uint8_t)Z906_CMD_RESET_IDLE);
    TEST_ASSERT_EQUAL_HEX8(0x31, (uint8_t)Z906_CMD_READ_IDLE);
    TEST_ASSERT_EQUAL_HEX8(0x33, (uint8_t)Z906_CMD_STOP_TEST);
    TEST_ASSERT_EQUAL_HEX8(0x34, (uint8_t)Z906_CMD_READ_STATUS);
    TEST_ASSERT_EQUAL_HEX8(0x36, (uint8_t)Z906_CMD_STANDBY_SAVE);
    TEST_ASSERT_EQUAL_HEX8(0x37, (uint8_t)Z906_CMD_STANDBY_PREP);
    TEST_ASSERT_EQUAL_HEX8(0x38, (uint8_t)Z906_CMD_UNMUTE_START);
    TEST_ASSERT_EQUAL_HEX8(0x39, (uint8_t)Z906_CMD_UNMUTE_END);
    TEST_ASSERT_EQUAL_HEX8(0x3F, (uint8_t)Z906_CMD_HEADPHONES);
    TEST_ASSERT_EQUAL_HEX8(0xAA, (uint8_t)Z906_CMD_MULTIBYTE_REPLY);
}

/**
 * @brief Tests spatial effect compatibility rules per input type.
 *
 * Analog inputs (6-Channel Direct, RCA Stereo, Auxiliary/AUX) support all spatial effects.
 * Digital inputs (Optical 1, Optical 2, Digital Coaxial) force effect to NONE.
 */
void test_input_and_effect_compatibility(void)
{
    /* Spatial effects permitted on analog inputs */
    TEST_ASSERT_EQUAL(Z906_EFFECT_3D,  z906_cmd_validate_effect(Z906_INPUT_SIX_CHANNEL_DIRECT, Z906_EFFECT_3D));   /* Input 1: 6-Channel Direct */
    TEST_ASSERT_EQUAL(Z906_EFFECT_4_1, z906_cmd_validate_effect(Z906_INPUT_RCA_STEREO,          Z906_EFFECT_4_1)); /* Input 2: RCA Stereo */

    /* Spatial effects must be forced to NONE on digital inputs */
    TEST_ASSERT_EQUAL(Z906_EFFECT_NONE, z906_cmd_validate_effect(Z906_INPUT_OPTICAL_1,       Z906_EFFECT_3D));   /* Input 3: Optical 1 */
    TEST_ASSERT_EQUAL(Z906_EFFECT_NONE, z906_cmd_validate_effect(Z906_INPUT_OPTICAL_2,       Z906_EFFECT_2_1)); /* Input 4: Optical 2 */
    TEST_ASSERT_EQUAL(Z906_EFFECT_NONE, z906_cmd_validate_effect(Z906_INPUT_DIGITAL_COAXIAL, Z906_EFFECT_3D));  /* Input 5: Digital Coaxial */
    /* AUX (Input 6) is analog, so it also supports spatial effects */
    TEST_ASSERT_EQUAL(Z906_EFFECT_2_1,  z906_cmd_validate_effect(Z906_INPUT_AUXILIARY, Z906_EFFECT_2_1));       /* Input 6: Auxiliary (AUX) */
}

int runUnityTests(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_volume_percentage_conversions);
    RUN_TEST(test_volume_zcl_conversions);
    RUN_TEST(test_canonical_input_opcodes);
    RUN_TEST(test_canonical_effect_opcodes);
    RUN_TEST(test_canonical_protocol_opcodes);
    RUN_TEST(test_input_and_effect_compatibility);
    return UNITY_END();
}

#if defined(ESP_PLATFORM)
void app_main(void)
{
    vTaskDelay(pdMS_TO_TICKS(3000)); /* Allow test runner to open serial port */
    runUnityTests();
}
#else
int main(void)
{
    return runUnityTests();
}
#endif
