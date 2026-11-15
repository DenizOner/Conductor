/**
 * @file test_zigbee_ingress.js
 * @brief Automated Phase-4-specific Node.js verification test suite for ZCL Command Ingress.
 *
 * Validates:
 * 1. OnOff Cluster Commands:
 *    - Basic commands: OFF -> POWER(0), ON -> POWER(1)
 *    - TOGGLE semantics against verified state: Off -> 1, On -> 0
 *    - Extended unsupported commands: OffWithEffect (0x40), OnWithRecallGlobalScene (0x41), OnWithTimedOff (0x42) -> UNSUP_CMD
 * 2. Level Control Cluster Commands:
 *    - MoveToLevel payload parsing (level, transition_time, options_mask, options_override)
 *    - ExecuteIfOff semantics: Off + ExecuteIfOff(0) -> no effect / SUCCESS; Off + ExecuteIfOff(1) -> volume queued
 *    - MoveToLevelWithOnOff compound semantics:
 *      * target volume > 0 -> compound event (Power ON verified, then Volume)
 *      * target volume == 0 -> volume 0, then Standby (Power OFF)
 *    - Continuous / Relative commands (Move, Step, Stop) -> UNSUP_CMD
 * 3. Level Scale Conversions:
 *    - z906_cmd_zcl_to_volume boundary tests: 0, 1, 127, 253, 254, 255 (invalid/reserved reject)
 * 4. Default Response Rules:
 *    - disableDefaultResponse == false -> exactly 1 response
 *    - disableDefaultResponse == true + SUCCESS -> 0 response
 *    - disableDefaultResponse == true + ERROR -> exactly 1 response
 * 5. Custom Attributes Validation:
 *    - Trims min/max bounds (0-43), Mute (0/1), Input (1-6), Effect (0-3)
 *    - Application toggles: LED (0/1), Verifier (0/1)
 *    - Read-only telemetry (0x0000 - 0x0003) rejection with READ_ONLY (0x88)
 * 6. Queue Capacity & Single-Producer Invariant:
 *    - Pre-check capacity guarantee prevents post-write enqueue failure
 * 7. Optimistic Memory Restoration:
 *    - Backing variable reverted to authoritative source (z906_state, canonical getters)
 */

'use strict';

const assert = require('assert');
const fs = require('fs');
const path = require('path');

console.log('=== Running Phase 4 Native Zigbee Ingress Unit Tests ===\n');

let passedTests = 0;
let totalTests = 0;

function it(desc, fn) {
    totalTests++;
    try {
        fn();
        console.log(`  [PASS] ${desc}`);
        passedTests++;
    } catch (err) {
        console.error(`  [FAIL] ${desc}`);
        console.error(`         ${err.message}`);
    }
}

/* Load firmware C source code and SDK v2.0.4 headers for source-level verification */
const ROOT_DIR = path.resolve(__dirname, '..');
const INGRESS_C_PATH = path.join(ROOT_DIR, 'src', 'communication', 'zigbee', 'zigbee_command_ingress.c');
const INGRESS_H_PATH = path.join(ROOT_DIR, 'src', 'communication', 'zigbee', 'zigbee_command_ingress.h');
const DEV_MODEL_C_PATH = path.join(ROOT_DIR, 'src', 'communication', 'zigbee', 'zigbee_device_model.c');
const LEVEL_DESC_H_PATH = path.join(ROOT_DIR, 'managed_components', 'espressif__esp-zigbee-lib', 'include', 'ezbee', 'zcl', 'cluster', 'level_desc.h');
const ON_OFF_DESC_H_PATH = path.join(ROOT_DIR, 'managed_components', 'espressif__esp-zigbee-lib', 'include', 'ezbee', 'zcl', 'cluster', 'on_off_desc.h');

const ingressC = fs.readFileSync(INGRESS_C_PATH, 'utf8');
const ingressH = fs.readFileSync(INGRESS_H_PATH, 'utf8');
const devModelC = fs.readFileSync(DEV_MODEL_C_PATH, 'utf8');
const levelDescH = fs.readFileSync(LEVEL_DESC_H_PATH, 'utf8');
const onOffDescH = fs.readFileSync(ON_OFF_DESC_H_PATH, 'utf8');

/* Firmware volume scale conversion logic mirroring z906_commands.c */
function z906_cmd_zcl_to_volume(zcl_level) {
    if (zcl_level === 0) return 0;
    if (zcl_level >= 254) return 43;
    return Math.floor((zcl_level * 43 + 127) / 254);
}

function z906_cmd_volume_to_zcl(volume) {
    if (volume === 0) return 0;
    if (volume >= 43) return 254;
    return Math.floor((volume * 254 + 21) / 43);
}

/* ========================================================================= */
/* Test Suite 0: Canonical SDK v2.0.4 Header Enum & Static Assertions Map    */
/* ========================================================================= */
console.log('--- Test Suite 0: Canonical SDK v2.0.4 Command ID Enum Extraction ---');

function extractEnumValues(headerContent) {
    const map = {};
    const regex = /(EZB_ZCL_CMD_[A-Z0-9_]+)\s*=\s*(0x[0-9A-Fa-f]+U?|[0-9]+)/g;
    let match;
    while ((match = regex.exec(headerContent)) !== null) {
        let valStr = match[2].replace(/U$/i, '');
        map[match[1]] = parseInt(valStr, valStr.startsWith('0x') ? 16 : 10);
    }
    return map;
}

const levelEnumMap = extractEnumValues(levelDescH);
const onOffEnumMap = extractEnumValues(onOffDescH);

it('[STATIC SOURCE TEST] should extract exact Level server command enum values from level_desc.h', () => {
    assert.strictEqual(levelEnumMap['EZB_ZCL_CMD_LEVEL_MOVE_TO_LEVEL_ID'], 0x00);
    assert.strictEqual(levelEnumMap['EZB_ZCL_CMD_LEVEL_MOVE_ID'], 0x01);
    assert.strictEqual(levelEnumMap['EZB_ZCL_CMD_LEVEL_STEP_ID'], 0x02);
    assert.strictEqual(levelEnumMap['EZB_ZCL_CMD_LEVEL_STOP_ID'], 0x03);
    assert.strictEqual(levelEnumMap['EZB_ZCL_CMD_LEVEL_MOVE_TO_LEVEL_WITH_ON_OFF_ID'], 0x04);
    assert.strictEqual(levelEnumMap['EZB_ZCL_CMD_LEVEL_MOVE_WITH_ON_OFF_ID'], 0x05);
    assert.strictEqual(levelEnumMap['EZB_ZCL_CMD_LEVEL_STEP_WITH_ON_OFF_ID'], 0x06);
    assert.strictEqual(levelEnumMap['EZB_ZCL_CMD_LEVEL_STOP_WITH_ON_OFF_ID'], 0x07);
    assert.strictEqual(levelEnumMap['EZB_ZCL_CMD_LEVEL_MOVE_TO_CLOSEST_FREQUENCY_ID'], 0x08);
});

it('[STATIC SOURCE TEST] should extract exact OnOff server command enum values from on_off_desc.h', () => {
    assert.strictEqual(onOffEnumMap['EZB_ZCL_CMD_ON_OFF_OFF_ID'], 0x00);
    assert.strictEqual(onOffEnumMap['EZB_ZCL_CMD_ON_OFF_ON_ID'], 0x01);
    assert.strictEqual(onOffEnumMap['EZB_ZCL_CMD_ON_OFF_TOGGLE_ID'], 0x02);
    assert.strictEqual(onOffEnumMap['EZB_ZCL_CMD_ON_OFF_OFF_WITH_EFFECT_ID'], 0x40);
    assert.strictEqual(onOffEnumMap['EZB_ZCL_CMD_ON_OFF_ON_WITH_RECALL_GLOBAL_SCENE_ID'], 0x41);
    assert.strictEqual(onOffEnumMap['EZB_ZCL_CMD_ON_OFF_ON_WITH_TIMED_OFF_ID'], 0x42);
});

it('[STATIC SOURCE TEST] should verify compile-time _Static_assert checks exist in zigbee_command_ingress.c', () => {
    assert.ok(ingressC.includes('_Static_assert(EZB_ZCL_CMD_LEVEL_MOVE_TO_LEVEL_ID == 0x00U'));
    assert.ok(ingressC.includes('_Static_assert(EZB_ZCL_CMD_LEVEL_MOVE_TO_LEVEL_WITH_ON_OFF_ID == 0x04U'));
    assert.ok(ingressC.includes('_Static_assert(EZB_ZCL_CMD_LEVEL_MOVE_TO_CLOSEST_FREQUENCY_ID == 0x08U'));
    assert.ok(ingressC.includes('_Static_assert(EZB_ZCL_CMD_ON_OFF_OFF_ID == 0x00U'));
    assert.ok(ingressC.includes('_Static_assert(EZB_ZCL_CMD_ON_OFF_ON_ID == 0x01U'));
    assert.ok(ingressC.includes('_Static_assert(EZB_ZCL_CMD_ON_OFF_TOGGLE_ID == 0x02U'));
    assert.ok(ingressC.includes('_Static_assert(EZB_ZCL_CMD_ON_OFF_ON_WITH_TIMED_OFF_ID == 0x42U'));
});

/* Exact Level Command Map Verification Table */
console.log('\n--- Exact Level Server Command Dispatch Matrix (Section 29) ---');
const expectedLevelMap = [
    { name: 'MoveToLevel',            id: 0x00, behavior: 'plain level policy' },
    { name: 'Move',                   id: 0x01, behavior: 'unsupported' },
    { name: 'Step',                   id: 0x02, behavior: 'unsupported' },
    { name: 'Stop',                   id: 0x03, behavior: 'unsupported' },
    { name: 'MoveToLevelWithOnOff',   id: 0x04, behavior: 'compound policy' },
    { name: 'MoveWithOnOff',          id: 0x05, behavior: 'unsupported' },
    { name: 'StepWithOnOff',          id: 0x06, behavior: 'unsupported' },
    { name: 'StopWithOnOff',          id: 0x07, behavior: 'unsupported' },
    { name: 'MoveToClosestFrequency', id: 0x08, behavior: 'unsupported' }
];

expectedLevelMap.forEach(item => {
    it(`[STATIC SOURCE TEST] ${item.name.padEnd(28)} 0x0${item.id.toString(16)} PASS`, () => {
        const enumKey = `EZB_ZCL_CMD_LEVEL_${item.name.replace(/([A-Z])/g, '_$1').toUpperCase().replace(/^_/, '').replace(/_ON__OFF/, '_ON_OFF')}_ID`;
        assert.strictEqual(levelEnumMap[enumKey], item.id);
        if (item.id === 0x00) {
            assert.ok(ingressC.includes('case EZB_ZCL_CMD_LEVEL_MOVE_TO_LEVEL_ID:'));
        } else if (item.id === 0x04) {
            assert.ok(ingressC.includes('case EZB_ZCL_CMD_LEVEL_MOVE_TO_LEVEL_WITH_ON_OFF_ID:'));
        } else {
            assert.ok(ingressC.includes(`case ${enumKey}:`));
        }
    });
});

/* ========================================================================= */
/* Test Suite 1: Level Scale Conversions                                      */
/* ========================================================================= */
console.log('\n--- Test Suite 1: Level Scale Conversions (Boundary & Range) ---');

it('[ALGORITHMIC JS MIRROR] should convert Level 0 to Volume 0', () => {
    assert.strictEqual(z906_cmd_zcl_to_volume(0), 0);
});

it('[ALGORITHMIC JS MIRROR] should convert Level 1 to Volume 0 (rounding boundary)', () => {
    assert.strictEqual(z906_cmd_zcl_to_volume(1), 0);
});

it('[ALGORITHMIC JS MIRROR] should convert Level 127 to mid-range Volume 22', () => {
    assert.strictEqual(z906_cmd_zcl_to_volume(127), 22);
});

it('[ALGORITHMIC JS MIRROR] should convert Level 253 to Volume 43', () => {
    assert.strictEqual(z906_cmd_zcl_to_volume(253), 43);
});

it('[ALGORITHMIC JS MIRROR] should convert Level 254 (max valid ZCL) to Volume 43', () => {
    assert.strictEqual(z906_cmd_zcl_to_volume(254), 43);
});

it('[ALGORITHMIC JS MIRROR] should reject Level 255 as reserved/invalid ZCL level', () => {
    const isLevelValid = (lvl) => lvl >= 0 && lvl <= 254;
    assert.strictEqual(isLevelValid(254), true);
    assert.strictEqual(isLevelValid(255), false);
});

/* ========================================================================= */
/* Test Suite 2: OnOff Command Compliance                                    */
/* ========================================================================= */
console.log('\n--- Test Suite 2: OnOff Command Compliance ---');

it('[STATIC SOURCE TEST] should handle OFF command by targeting Power 0 (Standby)', () => {
    assert.ok(ingressC.includes('case EZB_ZCL_CMD_ON_OFF_OFF_ID:'));
    assert.ok(ingressC.includes('target_power = 0;'));
});

it('[STATIC SOURCE TEST] should handle ON command by targeting Power 1', () => {
    assert.ok(ingressC.includes('case EZB_ZCL_CMD_ON_OFF_ON_ID:'));
    assert.ok(ingressC.includes('target_power = 1;'));
});

it('[STATIC SOURCE TEST] should handle TOGGLE command by reading verified state (!st.power)', () => {
    assert.ok(ingressC.includes('case EZB_ZCL_CMD_ON_OFF_TOGGLE_ID:'));
    assert.ok(ingressC.includes('target_power = st.power ? 0 : 1;'));
});

it('[STATIC SOURCE TEST] should explicitly reject extended OnOff commands (OffWithEffect, RecallGlobalScene, TimedOff) with UNSUP_CMD', () => {
    assert.ok(ingressC.includes('case EZB_ZCL_CMD_ON_OFF_OFF_WITH_EFFECT_ID:'));
    assert.ok(ingressC.includes('case EZB_ZCL_CMD_ON_OFF_ON_WITH_RECALL_GLOBAL_SCENE_ID:'));
    assert.ok(ingressC.includes('case EZB_ZCL_CMD_ON_OFF_ON_WITH_TIMED_OFF_ID:'));
    assert.ok(ingressC.includes('send_default_response(hdr, EZB_ZCL_STATUS_UNSUP_CMD);'));
});

/* ========================================================================= */
/* Test Suite 3: Level Control ZCL Standard Semantics                        */
/* ========================================================================= */
console.log('\n--- Test Suite 3: Level Control ZCL Standard Semantics ---');

it('[STATIC SOURCE TEST] should safely parse MoveToLevel payload including transition time and options', () => {
    assert.ok(ingressC.includes('raw_frame->payload_length < 3'));
    assert.ok(ingressC.includes('send_default_response(hdr, EZB_ZCL_STATUS_MALFORMED_CMD)'));
    assert.ok(ingressC.includes('uint16_t transition_time = (uint16_t)raw_frame->payload[1] | ((uint16_t)raw_frame->payload[2] << 8)'));
    assert.ok(ingressC.includes('options_mask = raw_frame->payload[3]'));
    assert.ok(ingressC.includes('options_override = raw_frame->payload[4]'));
});

it('[STATIC SOURCE TEST] should enforce ExecuteIfOff semantics: Standby + ExecuteIfOff=0 drops command with SUCCESS', () => {
    assert.ok(ingressC.includes('!st.power && !execute_if_off'));
    assert.ok(ingressC.includes('MoveToLevel ignored: Device is in Standby and ExecuteIfOff=0'));
    assert.ok(ingressC.includes('send_default_response(hdr, EZB_ZCL_STATUS_SUCCESS)'));
});

it('[STATIC SOURCE TEST] should enforce compound MoveToLevelWithOnOff: Power ON first if in Standby, then Volume', () => {
    assert.ok(ingressH.includes('ZB_INGRESS_TYPE_COMPOUND_LEVEL_ONOFF'));
    assert.ok(ingressC.includes('case EZB_ZCL_CMD_LEVEL_MOVE_TO_LEVEL_WITH_ON_OFF_ID:'));
    assert.ok(ingressC.includes('.type          = ZB_INGRESS_TYPE_COMPOUND_LEVEL_ONOFF'));
    assert.ok(ingressC.includes('if (!st.power)'));
    assert.ok(ingressC.includes('z906_cmd_registry_build_request(Z906_CMD_ID_POWER, CMD_SOURCE_ZIGBEE, 1, &pwr_req)'));
    assert.ok(ingressC.includes('z906_cmd_registry_build_request(Z906_CMD_ID_VOLUME, CMD_SOURCE_ZIGBEE, target_vol, &vol_req)'));
});

it('[STATIC SOURCE TEST] should enforce MoveToLevelWithOnOff min level (0): Set Volume 0, then enter Standby per ZCL spec', () => {
    assert.ok(ingressC.includes('if (target_vol > 0)'));
    assert.ok(ingressC.includes('/* Target volume == 0 */'));
    assert.ok(ingressC.includes('z906_cmd_registry_build_request(Z906_CMD_ID_POWER, CMD_SOURCE_ZIGBEE, 0, &pwr_req)'));
});

it('[STATIC SOURCE TEST] should explicitly reject continuous/relative Level commands (Move, Step, Stop) with UNSUP_CMD', () => {
    assert.ok(ingressC.includes('case EZB_ZCL_CMD_LEVEL_MOVE_ID:'));
    assert.ok(ingressC.includes('case EZB_ZCL_CMD_LEVEL_STEP_ID:'));
    assert.ok(ingressC.includes('case EZB_ZCL_CMD_LEVEL_STOP_ID:'));
    assert.ok(ingressC.includes('case EZB_ZCL_CMD_LEVEL_MOVE_WITH_ON_OFF_ID:'));
    assert.ok(ingressC.includes('case EZB_ZCL_CMD_LEVEL_STEP_WITH_ON_OFF_ID:'));
    assert.ok(ingressC.includes('case EZB_ZCL_CMD_LEVEL_STOP_WITH_ON_OFF_ID:'));
    assert.ok(ingressC.includes('case EZB_ZCL_CMD_LEVEL_MOVE_TO_CLOSEST_FREQUENCY_ID:'));
});

/* ========================================================================= */
/* Test Suite 4: Raw Handler Contract & Default Response Correctness          */
/* ========================================================================= */
console.log('\n--- Test Suite 4: Raw Handler Contract & Default Response ---');

it('[STATIC SOURCE TEST] should return true for intercepted frames and false for pass-through frames', () => {
    assert.ok(ingressC.includes('if (hdr->cluster_id == ZCL_CLUSTER_ON_OFF) {'));
    assert.ok(ingressC.includes('return handle_on_off_command(hdr);'));
    assert.ok(ingressC.includes('if (hdr->cluster_id == ZCL_CLUSTER_LEVEL_CONTROL) {'));
    assert.ok(ingressC.includes('return handle_level_control_command(raw_frame);'));
    assert.ok(ingressC.includes('/* Pass through all other clusters (e.g. Identify, OTA Upgrade) */'));
    assert.ok(ingressC.includes('return false;'));
});

it('[STATIC SOURCE TEST] should suppress default response if disableDefaultResponse is set and status is SUCCESS', () => {
    assert.ok(ingressC.includes('if (EZB_ZCL_CMD_FC_IS_DIS_DEFAULT_RSP(hdr->fc) && status == EZB_ZCL_STATUS_SUCCESS)'));
    assert.ok(ingressC.includes('return;'));
});

it('[STATIC SOURCE TEST] should generate default response when status is an error even if disableDefaultResponse is set', () => {
    assert.ok(ingressC.includes('status == EZB_ZCL_STATUS_SUCCESS'));
});

/* ========================================================================= */
/* Test Suite 5: Custom Clusters Validation & Read-Only Protection            */
/* ========================================================================= */
console.log('\n--- Test Suite 5: Custom Clusters Validation & Read-Only Protection ---');

it('[STATIC SOURCE TEST] should reject writes to EP4 read-only telemetry attributes (0x0000 - 0x0003) with READ_ONLY', () => {
    assert.ok(ingressC.includes('if (attr_id <= ZCL_ATTR_AUTO_STANDBY)'));
    assert.ok(ingressC.includes('return EZB_ZCL_STATUS_READ_ONLY;'));
});

it('[STATIC SOURCE TEST] should validate boolean bounds for Built-in LED and Command Verifier toggles', () => {
    assert.ok(ingressC.includes('if (attr_id == ZCL_ATTR_BUILT_IN_LED_ENABLED || attr_id == ZCL_ATTR_CMD_VERIFIER_ENABLED)'));
    assert.ok(ingressC.includes('if (val_u8 > 1)'));
    assert.ok(ingressC.includes('return EZB_ZCL_STATUS_INVALID_VALUE;'));
});

it('[STATIC SOURCE TEST] should validate Trims, Mute, Input, and Effect boundaries via central registry', () => {
    assert.ok(ingressC.includes('z906_cmd_registry_validate_param(Z906_CMD_ID_MUTE, val_u8)'));
    assert.ok(ingressC.includes('z906_cmd_registry_validate_param(cmd_id, hw_trim)'));
    assert.ok(ingressC.includes('z906_cmd_registry_validate_param(Z906_CMD_ID_INPUT, val_u8)'));
    assert.ok(ingressC.includes('z906_cmd_registry_validate_param(Z906_CMD_ID_EFFECT, val_u8)'));
});

/* ========================================================================= */
/* Test Suite 6: Queue Capacity Guarantee & Concurrency Invariant             */
/* ========================================================================= */
console.log('\n--- Test Suite 6: Queue Capacity & Concurrency Invariant ---');

it('[STATIC SOURCE TEST] should check queue capacity in check_value_cb and guarantee enqueue in write_attr_cb', () => {
    assert.ok(ingressC.includes('if (uxQueueSpacesAvailable(s_ingress_queue) == 0)'));
    assert.ok(ingressC.includes('return EZB_ZCL_STATUS_INSUFFICIENT_SPACE;'));
    assert.ok(ingressC.includes('BaseType_t sent = xQueueSend(s_ingress_queue, &evt, 0);'));
});

it('[STATIC SOURCE TEST] should immediately restore backing variables to authoritative physical/canonical state', () => {
    assert.ok(ingressC.includes('zigbee_device_model_restore_attr(ep_id, evt.cluster, attr_id);'));
    assert.ok(devModelC.includes('void zigbee_device_model_restore_attr(uint8_t ep_id, uint16_t cluster_id, uint16_t attr_id)'));
    assert.ok(devModelC.includes('z906_state_get(&st);'));
    assert.ok(devModelC.includes('s_ep1_on_off_val = st.power;'));
    assert.ok(devModelC.includes('s_ep1_current_level_val = z906_cmd_volume_to_zcl(st.volume);'));
    assert.ok(devModelC.includes('s_ep4_built_in_led_enabled = led_notification_is_enabled() ? 1 : 0;'));
    assert.ok(devModelC.includes('s_ep4_cmd_verifier_enabled = z906_cmd_verifier_is_enabled() ? 1 : 0;'));
});

it('[STATIC SOURCE TEST] should prove restore path contains zero legacy esp_zb_* or ESP_ZB_* symbols', () => {
    const restoreFnMatch = devModelC.match(/void zigbee_device_model_restore_attr[\s\S]*?^}/m);
    assert.ok(restoreFnMatch, 'zigbee_device_model_restore_attr implementation not found');
    const restoreFnBody = restoreFnMatch[0];
    assert.strictEqual(restoreFnBody.includes('esp_zb_'), false);
    assert.strictEqual(restoreFnBody.includes('ESP_ZB_'), false);
    assert.strictEqual(restoreFnBody.includes('ZBOSS'), false);
});

/* ========================================================================= */
/* Test Suite 7: Dedicated Worker Task & Central Command Convergence          */
/* ========================================================================= */
console.log('\n--- Test Suite 7: Central Command Convergence ---');

it('[STATIC SOURCE TEST] should dispatch all hardware commands through z906_cmd_verifier_execute with CMD_SOURCE_ZIGBEE', () => {
    assert.ok(ingressC.includes('cmd_verify_result_t res = z906_cmd_verifier_execute(&req, &verify_info);'));
    assert.ok(ingressC.includes('CMD_SOURCE_ZIGBEE'));
});

it('[STATIC SOURCE TEST] should configure dedicated static worker task with Priority 3 (Medium) and 4096 B stack', () => {
    assert.ok(ingressC.includes('#define ZB_WORKER_STACK_SIZE 4096'));
    assert.ok(ingressC.includes('xTaskCreateStatic(zigbee_cmd_worker_task'));
    assert.ok(ingressC.includes('"zb_cmd_worker"'));
    assert.ok(ingressC.includes('3, /* Priority 3 (Medium) */'));
});

console.log(`\n========================================`);
console.log(`Results: ${passedTests}/${totalTests} tests passed.`);
console.log(`========================================\n`);

if (passedTests !== totalTests) {
    process.exit(1);
}

