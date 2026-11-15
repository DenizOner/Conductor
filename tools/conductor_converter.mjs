/**
 * @file conductor_converter.mjs
 * @brief Zigbee2MQTT External Converter (ESM) for Conductor – Smart Zigbee Console for Logitech Z906.
 *
 * Format: ESM — `import` / `export default` per current Zigbee2MQTT external converter spec.
 *
 * ─────────────────────────────────────────────────────────────────────────────
 * DEPLOYMENT (Zigbee2MQTT v2.x+):
 *   1. Remove any previously deployed legacy converter:
 *        rm <z2m-data-dir>/external_converters/logitech_z906_converter.js
 *   2. Copy THIS file to:
 *        <z2m-data-dir>/external_converters/conductor_converter.mjs
 *   3. Ensure configuration.yaml contains:
 *        advanced:
 *          enable_external_js: true
 *   4. Restart Zigbee2MQTT.
 *
 *   WARNING: Running both the legacy .js converter and this .mjs converter simultaneously
 *   will create a duplicate-definition conflict. Always remove the old converter first.
 *
 * ─────────────────────────────────────────────────────────────────────────────
 * FIRMWARE IDENTITY (do not modify — must match firmware Basic cluster strings):
 *   ManufacturerName : "Logitech"      (Z906_ZB_MANUFACTURER_NAME in config.h)
 *   ModelIdentifier  : "Z-906-Zigbee"  (Z906_ZB_MODEL_IDENTIFIER in config.h)
 *
 * ─────────────────────────────────────────────────────────────────────────────
 * ARCHITECTURE — WHY NOT modernExtend:
 *   Three manufacturer-specific ZCL clusters (0xFC01, 0xFC02, 0xFC03) have no
 *   modernExtend equivalent. Explicit fromZigbee / toZigbee is mandatory for full
 *   support. Standard 0x0402 Temperature Measurement on EP11/12/13 uses explicit
 *   custom handlers to enforce the 0x8000 invalid-sentinel policy (no fake -327.68°C).
 *
 * ─────────────────────────────────────────────────────────────────────────────
 * OPTIMISTIC STATE POLICY:
 *   All convertSet handlers return null. The firmware performs real physical state
 *   verification via z906_cmd_verifier_execute() before writing Zigbee attribute
 *   reports. Z2M updates Home Assistant only after receiving the confirmed attribute
 *   report. This prevents HA from displaying incorrect states after failed or
 *   rejected commands.
 *
 * ─────────────────────────────────────────────────────────────────────────────
 * BINDING / CONFIGURE ARCHITECTURE:
 *   configure() performs ONLY:
 *     (a) Cluster bindings — required so Phase-5 firmware manual reports reach
 *         the coordinator destination correctly.
 *     (b) Initial attribute reads — bootstrap state before the first hardware report.
 *   configure() does NOT call endpoint.configureReporting(). Conductor already uses
 *   an application-level changed-only publisher (Phase 5). A second ZCL Configure
 *   Reporting would create a duplicate reporting engine.
 *
 * ─────────────────────────────────────────────────────────────────────────────
 * TEMPERATURE 0x8000 SENTINEL POLICY:
 *   When the firmware has not yet received a valid temperature measurement, it
 *   publishes ZCL measuredValue = 0x8000 (ZCL_TEMP_INVALID_VALUE, per spec §4.4.2.1).
 *   This converter suppresses 0x8000 — the state property is left unchanged (no
 *   -327.68°C fake value is ever published).
 *
 * ─────────────────────────────────────────────────────────────────────────────
 * OTA:
 *   ota: ota.zigbeeOTA — enables Zigbee2MQTT OTA Upgrade protocol support.
 *   For firmware image distribution: use a custom OTA index (see OTA_INDEX_DEPLOYMENT
 *   section at the bottom of this file).
 *
 * ─────────────────────────────────────────────────────────────────────────────
 * SECURITY:
 *   No filesystem access, no network access, no eval() or dynamic code execution.
 *   All writable attributes enforce strict input validation.
 *   fromZigbee handlers are wrapped in try/catch to prevent a single malformed
 *   Zigbee message from crashing the Zigbee2MQTT process.
 *   Only deploy this converter from a reviewed, trusted local source.
 */

// ─── Imports ────────────────────────────────────────────────────────────────
// NOTE: zigbee-herdsman-converters@21.x requires explicit .js extension —
// the package has no exports map; bare subpath imports ('lib/exposes' without .js)
// fail under Node.js ESM strict resolution (verified against ZHC 21.38.0).
//
// NOTE: In ZHC 21.x, OTA is enabled with `ota: true` in the definition.
// The older `ota.zigbeeOTA` export was removed from lib/ota.js in ZHC ≥ 18.
import * as exposes from 'zigbee-herdsman-converters/lib/exposes.js';

// Shorthand aliases used throughout converter
const ea = exposes.access;   // { ALL, STATE, SET, STATE_SET, STATE_GET, GET }
const e  = exposes;          // expose builder namespace

// ─── Custom Cluster Identifiers ─────────────────────────────────────────────
// Manufacturer-specific cluster IDs — must match firmware zcl_clusters.h

/** @constant {number} Trims & Mute control cluster (EP 2) */
const ZCL_CLUSTER_Z906_TRIMS     = 0xFC01;
/** @constant {number} Input source & spatial effect cluster (EP 3) */
const ZCL_CLUSTER_Z906_SOURCES   = 0xFC02;
/** @constant {number} Telemetry, feature toggles & diagnostic cluster (EP 4) */
const ZCL_CLUSTER_Z906_TELEMETRY = 0xFC03;

// ─── FC01 Attribute IDs — Trims & Mute (EP 2) ───────────────────────────────
// Source of truth: src/communication/zigbee/clusters/zcl_clusters.h
// Wire type: UINT8 (0x20) for all four attributes

/** Bass channel trim level: UINT8, R/W, 0x0000 */
const ZCL_ATTR_TRIM_BASS   = 0x0000;
/** Center speaker trim level: UINT8, R/W, 0x0001 */
const ZCL_ATTR_TRIM_CENTER = 0x0001;
/** Rear surround trim level: UINT8, R/W, 0x0002 */
const ZCL_ATTR_TRIM_REAR   = 0x0002;
/** Amplifier mute: UINT8 (0=unmuted, 1=muted), R/W, 0x0003 */
const ZCL_ATTR_AUDIO_MUTE  = 0x0003;

// ─── FC02 Attribute IDs — Source Selection (EP 3) ───────────────────────────
// Wire type: UINT8 (0x20) for both attributes

/** Active audio input (1-6 canonical integer): UINT8, R/W, 0x0000 */
const ZCL_ATTR_SOURCE_INPUT  = 0x0000;
/** Spatial audio effect (0-3): UINT8, R/W, 0x0001 */
const ZCL_ATTR_SOURCE_EFFECT = 0x0001;

// ─── FC03 Attribute IDs — Telemetry & Diagnostics (EP 4) ────────────────────
// Source of truth: src/communication/zigbee/clusters/zcl_clusters.h

/** Raw U24BE input signal amplitude: DATA24, R/O, 0x0000 */
const ZCL_ATTR_INPUT_SIGNAL_LEVEL        = 0x0000;
/** Elapsed idle time in seconds: DATA32, R/O, 0x0001 */
const ZCL_ATTR_IDLE_TIME                 = 0x0001;
/** Byte-12 decode state code from 0xAA 0x0A 0x14 frame: UINT8, R/O, 0x0002 */
const ZCL_ATTR_DIGITAL_SIGNAL_STATUS     = 0x0002;
/** Auto-standby enable bit: UINT8, R/O, 0x0003 */
const ZCL_ATTR_AUTO_STANDBY              = 0x0003;
// 0x0004, 0x0005, 0x0006 — permanently absent (amplifier temperatures on EP11/12/13)
/** Onboard LED notification enable: UINT8, R/W, 0x0007 */
const ZCL_ATTR_BUILT_IN_LED_ENABLED      = 0x0007;
/** State-based command verifier enable: UINT8, R/W, 0x0008 */
const ZCL_ATTR_CMD_VERIFIER_ENABLED      = 0x0008;

/** Overall device health status: UINT8, R/O, 0x0009 */
const ZCL_ATTR_DEVICE_HEALTH             = 0x0009;
/** Primary failing/degraded component: UINT8, R/O, 0x000A */
const ZCL_ATTR_HEALTH_PRIMARY_COMPONENT  = 0x000A;
/** Primary root cause reason code: UINT8, R/O, 0x000B */
const ZCL_ATTR_HEALTH_PRIMARY_REASON     = 0x000B;
/** ESP32-C6 internal SoC die temperature: INT16, R/O, 0x000C */
const ZCL_ATTR_SOC_TEMPERATURE           = 0x000C;

// ─── ZCL DataType Constants ──────────────────────────────────────────────────
// Used in endpoint.write() calls to specify wire format.
// Values match Zcl.DataType enum from zigbee-herdsman.

/** ZCL UINT8 data type (0x20) — used by trims, mute, input, effect, UINT8 telemetry flags */
const ZCL_TYPE_UINT8  = 0x20;
/** ZCL DATA24 data type (0x0A) — used by input_signal_level (3-byte backing) */
const ZCL_TYPE_DATA24 = 0x0A;
/** ZCL DATA32 data type (0x08) — used by idle_time (4-byte backing) */
const ZCL_TYPE_DATA32 = 0x08;
/** ZCL INT16 data type (0x29) — signed 16-bit integer used by soc_temperature */
const ZCL_TYPE_INT16  = 0x29;

// ─── Standard ZCL 0x0402 Temperature Measurement ────────────────────────────

/** ZCL Temperature Measurement cluster ID (standard) */
const ZCL_CLUSTER_TEMP_MEASUREMENT  = 0x0402;
/** ZCL measuredValue attribute ID (int16_t, °C × 100) */
const ZCL_ATTR_TEMP_MEASURED_VALUE  = 0x0000;
/**
 * Invalid temperature sentinel per ZCL spec §4.4.2.1.
 * Firmware publishes 0x8000 when temp_valid=false. Must NOT be published as -327.68°C.
 */
const ZCL_TEMP_INVALID_SENTINEL     = 0x8000;

// ─── Canonical Audio Input Mapping ───────────────────────────────────────────
// Source of truth: AGENTS.md §5.2 canonical input naming table.
// Integer values 1-6 are protocol-stable and must never change.

/**
 * Canonical connection-type string → protocol integer (1-6).
 * STRICT: only these exact string keys are accepted. No numeric fallback.
 */
const INPUT_MAP_TO_NUM = {
    '6-Channel Direct (Input 1)': 1,
    'RCA Stereo (Input 2)':       2,
    'Optical 1 (Input 3)':        3,
    'Optical 2 (Input 4)':        4,
    'Digital Coaxial (Input 5)':  5,
    'Auxiliary / AUX (Input 6)':  6,
};

/** Protocol integer (1-6) → canonical display string (reverse map). */
const INPUT_MAP_FROM_NUM = {
    1: '6-Channel Direct (Input 1)',
    2: 'RCA Stereo (Input 2)',
    3: 'Optical 1 (Input 3)',
    4: 'Optical 2 (Input 4)',
    5: 'Digital Coaxial (Input 5)',
    6: 'Auxiliary / AUX (Input 6)',
};

// ─── Spatial Effect Mapping ───────────────────────────────────────────────────
// Source of truth: z906_commands_metadata.json effect enum.
// Values from z906_effect_t: 3D=0, 2.1=1, 4.1=2, None=3.

/** Effect display name → protocol integer (0-3). */
const EFFECT_MAP_TO_NUM = {
    '3D':   0,
    '2.1':  1,
    '4.1':  2,
    'None': 3,
};

/** Protocol integer (0-3) → effect display name. */
const EFFECT_MAP_FROM_NUM = {
    0: '3D',
    1: '2.1',
    2: '4.1',
    3: 'None',
};

// ─── Health Manager Diagnostic Mappings ───────────────────────────────────────
// Source of truth: src/management/health_manager/health_manager.h

/** Overall device health status string mapping (matches health_status_t) */
const HEALTH_STATUS_MAP = {
    0: 'unknown',
    1: 'healthy',
    2: 'degraded',
    3: 'fault',
};

/** Primary component causing health degradation/fault (matches health_component_t) */
const HEALTH_COMPONENT_MAP = {
    0: 'system',
    1: 'system_metrics',
    2: 'soc_temperature',
    3: 'cpu',
    4: 'memory',
    5: 'watchdog',
    6: 'nvs',
    7: 'initializer',
    8: 'zigbee',
    9: 'z906_transport',
    10: 'command_verifier',
    11: 'ota',
};

/** Root cause reason code explaining degradation/fault (matches health_reason_t) */
const HEALTH_REASON_MAP = {
    0: 'none',
    1: 'not_initialized',
    2: 'metric_not_ready',
    3: 'metric_stale',
    4: 'soc_temp_warning',
    5: 'soc_temp_critical',
    6: 'soc_temp_sensor_error',
    7: 'cpu_load_sustained_high',
    8: 'cpu_load_critical',
    9: 'heap_warning',
    10: 'heap_critical',
    11: 'heap_fragmented',
    12: 'heap_historical_pressure',
    13: 'watchdog_reset_detected',
    14: 'watchdog_task_stalled',
    15: 'zigbee_commission_failed',
    16: 'zigbee_degraded',
    17: 'z906_stale',
    18: 'z906_communication_fault',
    19: 'command_verifier_unavailable',
    20: 'ota_health_fault',
    21: 'nvs_fault',
    22: 'initializer_failed',
};

// ─── Scale Conversion Helpers ─────────────────────────────────────────────────

/**
 * Converts Z906 discrete hardware volume step (0-43) to ZCL Level (0-254).
 * Mathematical parity with z906_cmd_volume_to_zcl() in firmware.
 *
 * @param {number} vol - Z906 hardware step (0 to 43)
 * @returns {number} ZCL level (0 to 254)
 */
function volumeToZcl(vol) {
    if (vol <= 0)  return 0;
    if (vol >= 43) return 254;
    return Math.round((vol / 43.0) * 254.0);
}

/**
 * Converts ZCL Level (0-254) to Z906 discrete hardware volume step (0-43).
 * Mathematical parity with z906_cmd_zcl_to_volume() in firmware.
 *
 * @param {number} zcl - ZCL level (0 to 254)
 * @returns {number} Z906 hardware step (0 to 43)
 */
function zclToVolume(zcl) {
    if (zcl <= 0)   return 0;
    if (zcl >= 254) return 43;
    return Math.round((zcl / 254.0) * 43.0);
}

// ─── Digital Signal Decoder ───────────────────────────────────────────────────

/**
 * Decodes the Z906 digital audio signal / decode-state byte into a readable string.
 *
 * Raw value: Byte 12 of the 0xAA 0x0A 0x14 amplifier status frame.
 * Decoded from z906_decode_state_t enum (nomis/logitech-z906 reverse-engineering).
 *
 * @param {number} val - Raw UINT8 value from ZCL_ATTR_DIGITAL_SIGNAL_STATUS (0xFC03/0x0002)
 * @returns {string} Human-readable signal state string
 */
function decodeDigitalSignalStatus(val) {
    switch (val) {
        case 0x00: return 'No Signal';
        case 0x01: return 'PCM / Streaming';
        case 0x0E: return 'DTS / Bitstream';
        case 0x17: return 'DTS / Bitstream'; // Z906_DECODE_5_1
        case 0x18: return 'No Signal';        // Z906_DECODE_NO_SIGNAL
        case 0x19: return 'DTS / Bitstream';  // Z906_DECODE_4_1
        case 0x1A: return 'DTS / Bitstream';  // Z906_DECODE_3_1
        case 0x1B: return 'PCM / Streaming';  // Z906_DECODE_2_1
        case 0x1C: return 'PCM / Streaming';  // Z906_DECODE_2_0
        case 0x1D: return 'DTS / Bitstream';  // Z906_DECODE_SIGNAL_5_1
        case 0x1E: return 'PCM / Streaming';  // Z906_DECODE_2_0_MANDATORY_FX
        case 0x0C:
        case 0x1F:
        default:   return 'Unknown';
    }
}

// ─── DATA24 / DATA32 Parsing Helpers ─────────────────────────────────────────

/**
 * Parses a DATA24 attribute value as a 24-bit unsigned integer.
 *
 * zigbee-herdsman returns DATA24 as a Buffer (3 bytes, little-endian on the wire).
 * This converts it to a host numeric value for JSON state publication.
 *
 * @param {Buffer|number|object} raw - Raw value from herdsman attribute report
 * @returns {number} 24-bit unsigned integer
 */
function parseData24(raw) {
    if (Buffer.isBuffer(raw) && raw.length >= 3) {
        return raw[0] | (raw[1] << 8) | (raw[2] << 16);
    }
    if (typeof raw === 'number') {
        return raw & 0xFFFFFF;
    }
    return 0;
}

/**
 * Parses a DATA32 attribute value as a 32-bit unsigned integer.
 *
 * @param {Buffer|number|object} raw - Raw value from herdsman attribute report
 * @returns {number} 32-bit unsigned integer
 */
function parseData32(raw) {
    if (Buffer.isBuffer(raw) && raw.length >= 4) {
        return raw.readUInt32LE(0);
    }
    if (typeof raw === 'number') {
        return raw >>> 0; // unsigned 32-bit
    }
    return 0;
}

// ─── fromZigbee Inbound Handlers ─────────────────────────────────────────────

/**
 * Parses power on/off from genOnOff cluster, EP 1.
 * onOff: 1 = Active/ON, 0 = Standby/OFF.
 */
const fzMain = {
    cluster: 'genOnOff',
    type: ['attributeReport', 'readResponse'],
    convert: (model, msg, publish, options, meta) => {
        try {
            const result = {};
            if (msg.data.onOff !== undefined) {
                result.state = msg.data.onOff === 1 ? 'ON' : 'OFF';
            }
            return result;
        } catch (_err) {
            return {};
        }
    },
};

/**
 * Parses master volume from genLevelCtrl cluster, EP 1.
 * Converts ZCL 0-254 → Z906 hardware steps 0-43.
 */
const fzVolume = {
    cluster: 'genLevelCtrl',
    type: ['attributeReport', 'readResponse'],
    convert: (model, msg, publish, options, meta) => {
        try {
            const result = {};
            if (msg.data.currentLevel !== undefined) {
                result.volume = zclToVolume(msg.data.currentLevel);
            }
            return result;
        } catch (_err) {
            return {};
        }
    },
};

/**
 * Parses channel trims and mute state from FC01 custom cluster, EP 2.
 * Trim values use UINT8 (ZCL 0-254 wire) → hardware steps 0-43.
 * Mute uses UINT8 (0 = unmuted, 1 = muted).
 */
const fzTrims = {
    cluster: 'conductorTrims',
    type: ['attributeReport', 'readResponse'],
    convert: (model, msg, publish, options, meta) => {
        try {
            const result = {};
            if (msg.data[ZCL_ATTR_TRIM_BASS]   !== undefined) result.trim_bass   = zclToVolume(msg.data[ZCL_ATTR_TRIM_BASS]);
            if (msg.data[ZCL_ATTR_TRIM_CENTER]  !== undefined) result.trim_center = zclToVolume(msg.data[ZCL_ATTR_TRIM_CENTER]);
            if (msg.data[ZCL_ATTR_TRIM_REAR]    !== undefined) result.trim_rear   = zclToVolume(msg.data[ZCL_ATTR_TRIM_REAR]);
            if (msg.data[ZCL_ATTR_AUDIO_MUTE]   !== undefined) result.mute        = msg.data[ZCL_ATTR_AUDIO_MUTE] === 1 ? 'ON' : 'OFF';
            return result;
        } catch (_err) {
            return {};
        }
    },
};

/**
 * Parses input source and effect from FC02 custom cluster, EP 3.
 */
const fzSources = {
    cluster: 'conductorMode',
    type: ['attributeReport', 'readResponse'],
    convert: (model, msg, publish, options, meta) => {
        try {
            const result = {};
            if (msg.data[ZCL_ATTR_SOURCE_INPUT] !== undefined) {
                const inNum = msg.data[ZCL_ATTR_SOURCE_INPUT];
                result.input = INPUT_MAP_FROM_NUM[inNum] || `Input ${inNum}`;
            }
            if (msg.data[ZCL_ATTR_SOURCE_EFFECT] !== undefined) {
                const effNum = msg.data[ZCL_ATTR_SOURCE_EFFECT];
                result.effect = EFFECT_MAP_FROM_NUM[effNum] || 'None';
            }
            return result;
        } catch (_err) {
            return {};
        }
    },
};

/**
 * Parses telemetry, feature-toggle, and selective health diagnostic attributes from FC03 custom cluster, EP 4.
 *
 * Canonical attributes:
 *   0x0000 = input_signal_level    (DATA24, R/O)
 *   0x0001 = idle_time             (DATA32, R/O, seconds)
 *   0x0002 = digital_signal_status (UINT8,  R/O, decoded string)
 *   0x0003 = auto_standby          (UINT8,  R/O, 'Enabled'/'Disabled')
 *   0x0007 = built_in_led_enabled  (UINT8,  R/W, 'ON'/'OFF')
 *   0x0008 = cmd_verifier_enabled  (UINT8,  R/W, 'ON'/'OFF')
 *   0x0009 = device_health         (UINT8,  R/O, 'unknown'|'healthy'|'degraded'|'fault')
 *   0x000A = health_primary_component (UINT8, R/O, mapped string)
 *   0x000B = health_primary_reason (UINT8,  R/O, mapped string)
 *   0x000C = soc_temperature       (INT16,  R/O, °C, 0x8000 sentinel suppressed)
 */
const fzTelemetry = {
    cluster: 'conductorTelemetry',
    type: ['attributeReport', 'readResponse'],
    convert: (model, msg, publish, options, meta) => {
        try {
            const result = {};

            // 0x0000: input_signal_level — DATA24 (3-byte LE buffer)
            if (msg.data[ZCL_ATTR_INPUT_SIGNAL_LEVEL] !== undefined) {
                result.input_signal_level = parseData24(msg.data[ZCL_ATTR_INPUT_SIGNAL_LEVEL]);
            }

            // 0x0001: idle_time — DATA32 (4-byte LE, seconds)
            if (msg.data[ZCL_ATTR_IDLE_TIME] !== undefined) {
                result.idle_time = parseData32(msg.data[ZCL_ATTR_IDLE_TIME]);
            }

            // 0x0002: digital_signal_status — UINT8 → decoded human-readable string
            if (msg.data[ZCL_ATTR_DIGITAL_SIGNAL_STATUS] !== undefined) {
                result.digital_signal_status = decodeDigitalSignalStatus(msg.data[ZCL_ATTR_DIGITAL_SIGNAL_STATUS]);
            }

            // 0x0003: auto_standby — UINT8 (read-only)
            if (msg.data[ZCL_ATTR_AUTO_STANDBY] !== undefined) {
                result.auto_standby = msg.data[ZCL_ATTR_AUTO_STANDBY] ? 'Enabled' : 'Disabled';
            }

            // 0x0007: built_in_led_enabled — UINT8 R/W feature toggle
            if (msg.data[ZCL_ATTR_BUILT_IN_LED_ENABLED] !== undefined) {
                result.built_in_led_enabled = msg.data[ZCL_ATTR_BUILT_IN_LED_ENABLED] ? 'ON' : 'OFF';
            }

            // 0x0008: command_verifier_enabled — UINT8 R/W feature toggle
            if (msg.data[ZCL_ATTR_CMD_VERIFIER_ENABLED] !== undefined) {
                result.command_verifier_enabled = msg.data[ZCL_ATTR_CMD_VERIFIER_ENABLED] ? 'ON' : 'OFF';
            }

            // 0x0009: device_health — UINT8 → 'unknown' | 'healthy' | 'degraded' | 'fault'
            if (msg.data[ZCL_ATTR_DEVICE_HEALTH] !== undefined) {
                result.device_health = HEALTH_STATUS_MAP[msg.data[ZCL_ATTR_DEVICE_HEALTH]] || 'unknown';
            }

            // 0x000A: health_primary_component — UINT8 → mapped component string
            if (msg.data[ZCL_ATTR_HEALTH_PRIMARY_COMPONENT] !== undefined) {
                result.health_primary_component = HEALTH_COMPONENT_MAP[msg.data[ZCL_ATTR_HEALTH_PRIMARY_COMPONENT]] || 'unknown';
            }

            // 0x000B: health_primary_reason — UINT8 → mapped reason string
            if (msg.data[ZCL_ATTR_HEALTH_PRIMARY_REASON] !== undefined) {
                result.health_primary_reason = HEALTH_REASON_MAP[msg.data[ZCL_ATTR_HEALTH_PRIMARY_REASON]] || 'unknown';
            }

            // 0x000C: soc_temperature — INT16 (°C × 100), sentinel 0x8000 / -32768 suppressed
            if (msg.data[ZCL_ATTR_SOC_TEMPERATURE] !== undefined) {
                const raw = msg.data[ZCL_ATTR_SOC_TEMPERATURE];
                const rawUint16 = raw & 0xFFFF;
                if (rawUint16 !== ZCL_TEMP_INVALID_SENTINEL && raw !== -32768) {
                    const celsius = (raw << 16 >> 16) / 100.0;
                    result.soc_temperature = parseFloat(celsius.toFixed(2));
                }
            }

            return result;
        } catch (_err) {
            return {};
        }
    },
};

/**
 * Parses standard ZCL 0x0402 Temperature Measurement from EP 11, 12, or 13.
 *
 * ZCL encoding: measuredValue (int16_t) = temperature_celsius × 100.
 * Sentinel 0x8000 = invalid/unavailable (ZCL spec §4.4.2.1).
 */
const fzTemperature = {
    cluster: 'msTemperatureMeasurement',
    type: ['attributeReport', 'readResponse'],
    convert: (model, msg, publish, options, meta) => {
        try {
            const epId = msg.endpoint ? msg.endpoint.ID : null;
            const keyMap = { 11: 'temperature_1', 12: 'temperature_2', 13: 'temperature_3' };
            const key = keyMap[epId];
            if (!key) return {};

            const raw = msg.data.measuredValue;
            if (raw === undefined) return {};

            const rawUint16 = raw & 0xFFFF;
            if (rawUint16 === ZCL_TEMP_INVALID_SENTINEL || raw === -32768) {
                return {};
            }

            const celsius = (raw << 16 >> 16) / 100.0;
            return { [key]: parseFloat(celsius.toFixed(2)) };
        } catch (_err) {
            return {};
        }
    },
};

// ─── toZigbee Outbound Command Generators ────────────────────────────────────
// ALL handlers return null — no optimistic state is ever emitted.

/**
 * Sends power ON or STANDBY via standard genOnOff cluster, EP 1.
 */
const tzPower = {
    key: ['state'],
    convertSet: async (entity, key, value, meta) => {
        const powerOn = (String(value).toUpperCase() === 'ON' ||
                         value === true || value === 1);
        const ep = entity.getEndpoint ? entity.getEndpoint(1) : entity;
        await ep.command('genOnOff', powerOn ? 'on' : 'off', {}, {});
        return null;
    },
};

/**
 * Sends master volume via standard genLevelCtrl moveToLevel, EP 1.
 */
const tzVolume = {
    key: ['volume'],
    convertSet: async (entity, key, value, meta) => {
        let vol = parseInt(value, 10);
        if (isNaN(vol)) throw new Error('Volume must be a numeric integer 0-43');
        vol = Math.max(0, Math.min(43, vol));
        const zclLevel = volumeToZcl(vol);
        const ep = entity.getEndpoint ? entity.getEndpoint(1) : entity;
        await ep.command('genLevelCtrl', 'moveToLevel', { level: zclLevel, transtime: 0 }, {});
        return null;
    },
};

/**
 * Sends mute command by writing ZCL_ATTR_AUDIO_MUTE (0x0003) to FC01 cluster, EP 2.
 */
const tzMute = {
    key: ['mute'],
    convertSet: async (entity, key, value, meta) => {
        const isMuted = (String(value).toUpperCase() === 'ON' || value === true || value === 1);
        const ep = entity.getEndpoint ? entity.getEndpoint(2) : entity;
        await ep.write(ZCL_CLUSTER_Z906_TRIMS, {
            [ZCL_ATTR_AUDIO_MUTE]: { value: isMuted ? 1 : 0, type: ZCL_TYPE_UINT8 },
        });
        return null;
    },
};

/**
 * Sends audio input selection by writing ZCL_ATTR_SOURCE_INPUT (0x0000) to FC02, EP 3.
 */
const tzInput = {
    key: ['input'],
    convertSet: async (entity, key, value, meta) => {
        const inNum = INPUT_MAP_TO_NUM[value];
        if (inNum === undefined) {
            throw new Error(
                `Invalid input: "${value}". Canonical values: ${Object.keys(INPUT_MAP_TO_NUM).join(', ')}`
            );
        }
        const ep = entity.getEndpoint ? entity.getEndpoint(3) : entity;
        await ep.write(ZCL_CLUSTER_Z906_SOURCES, {
            [ZCL_ATTR_SOURCE_INPUT]: { value: inNum, type: ZCL_TYPE_UINT8 },
        });
        return null;
    },
};

/**
 * Sends spatial effect selection by writing ZCL_ATTR_SOURCE_EFFECT (0x0001) to FC02, EP 3.
 */
const tzEffect = {
    key: ['effect'],
    convertSet: async (entity, key, value, meta) => {
        const effNum = EFFECT_MAP_TO_NUM[value];
        if (effNum === undefined) {
            throw new Error(
                `Invalid effect: "${value}". Accepted values: ${Object.keys(EFFECT_MAP_TO_NUM).join(', ')}`
            );
        }
        const ep = entity.getEndpoint ? entity.getEndpoint(3) : entity;
        await ep.write(ZCL_CLUSTER_Z906_SOURCES, {
            [ZCL_ATTR_SOURCE_EFFECT]: { value: effNum, type: ZCL_TYPE_UINT8 },
        });
        return null;
    },
};

/**
 * Sends bass trim level by writing ZCL_ATTR_TRIM_BASS (0x0000) to FC01, EP 2.
 */
const tzTrimBass = {
    key: ['trim_bass'],
    convertSet: async (entity, key, value, meta) => {
        let lvl = parseInt(value, 10);
        if (isNaN(lvl)) throw new Error('Bass trim must be an integer 0-43');
        lvl = Math.max(0, Math.min(43, lvl));
        const ep = entity.getEndpoint ? entity.getEndpoint(2) : entity;
        await ep.write(ZCL_CLUSTER_Z906_TRIMS, {
            [ZCL_ATTR_TRIM_BASS]: { value: volumeToZcl(lvl), type: ZCL_TYPE_UINT8 },
        });
        return null;
    },
};

/**
 * Sends center trim level by writing ZCL_ATTR_TRIM_CENTER (0x0001) to FC01, EP 2.
 */
const tzTrimCenter = {
    key: ['trim_center'],
    convertSet: async (entity, key, value, meta) => {
        let lvl = parseInt(value, 10);
        if (isNaN(lvl)) throw new Error('Center trim must be an integer 0-43');
        lvl = Math.max(0, Math.min(43, lvl));
        const ep = entity.getEndpoint ? entity.getEndpoint(2) : entity;
        await ep.write(ZCL_CLUSTER_Z906_TRIMS, {
            [ZCL_ATTR_TRIM_CENTER]: { value: volumeToZcl(lvl), type: ZCL_TYPE_UINT8 },
        });
        return null;
    },
};

/**
 * Sends rear trim level by writing ZCL_ATTR_TRIM_REAR (0x0002) to FC01, EP 2.
 */
const tzTrimRear = {
    key: ['trim_rear'],
    convertSet: async (entity, key, value, meta) => {
        let lvl = parseInt(value, 10);
        if (isNaN(lvl)) throw new Error('Rear trim must be an integer 0-43');
        lvl = Math.max(0, Math.min(43, lvl));
        const ep = entity.getEndpoint ? entity.getEndpoint(2) : entity;
        await ep.write(ZCL_CLUSTER_Z906_TRIMS, {
            [ZCL_ATTR_TRIM_REAR]: { value: volumeToZcl(lvl), type: ZCL_TYPE_UINT8 },
        });
        return null;
    },
};

/**
 * Controls onboard LED notifications by writing ZCL_ATTR_BUILT_IN_LED_ENABLED (0x0007) to FC03, EP 4.
 */
const tzBuiltInLed = {
    key: ['built_in_led_enabled'],
    convertSet: async (entity, key, value, meta) => {
        const enable = (String(value).toUpperCase() === 'ON' || value === true || value === 1);
        const ep = entity.getEndpoint ? entity.getEndpoint(4) : entity;
        await ep.write(ZCL_CLUSTER_Z906_TELEMETRY, {
            [ZCL_ATTR_BUILT_IN_LED_ENABLED]: { value: enable ? 1 : 0, type: ZCL_TYPE_UINT8 },
        });
        return null;
    },
};

/**
 * Controls state-based command verifier by writing ZCL_ATTR_CMD_VERIFIER_ENABLED (0x0008) to FC03, EP 4.
 */
const tzCommandVerifier = {
    key: ['command_verifier_enabled'],
    convertSet: async (entity, key, value, meta) => {
        const enable = (String(value).toUpperCase() === 'ON' || value === true || value === 1);
        const ep = entity.getEndpoint ? entity.getEndpoint(4) : entity;
        await ep.write(ZCL_CLUSTER_Z906_TELEMETRY, {
            [ZCL_ATTR_CMD_VERIFIER_ENABLED]: { value: enable ? 1 : 0, type: ZCL_TYPE_UINT8 },
        });
        return null;
    },
};

// ─── Custom Cluster Definitions ───────────────────────────────────────────────

const customClusters = {
    /**
     * 0xFC01 — Conductor Trims & Mute (EP 2).
     */
    conductorTrims: {
        ID: ZCL_CLUSTER_Z906_TRIMS,
        attributes: {
            trimBass:   { ID: ZCL_ATTR_TRIM_BASS,   type: ZCL_TYPE_UINT8,  manufacturerCode: null },
            trimCenter: { ID: ZCL_ATTR_TRIM_CENTER,  type: ZCL_TYPE_UINT8,  manufacturerCode: null },
            trimRear:   { ID: ZCL_ATTR_TRIM_REAR,    type: ZCL_TYPE_UINT8,  manufacturerCode: null },
            audioMute:  { ID: ZCL_ATTR_AUDIO_MUTE,   type: ZCL_TYPE_UINT8,  manufacturerCode: null },
        },
        commands: {},
        commandsResponse: {},
    },

    /**
     * 0xFC02 — Conductor Mode (Input & Effect) (EP 3).
     */
    conductorMode: {
        ID: ZCL_CLUSTER_Z906_SOURCES,
        attributes: {
            sourceInput:  { ID: ZCL_ATTR_SOURCE_INPUT,  type: ZCL_TYPE_UINT8,  manufacturerCode: null },
            sourceEffect: { ID: ZCL_ATTR_SOURCE_EFFECT, type: ZCL_TYPE_UINT8,  manufacturerCode: null },
        },
        commands: {},
        commandsResponse: {},
    },

    /**
     * 0xFC03 — Conductor Telemetry & Diagnostics (EP 4).
     * Exactly 10 canonical attributes (6 telemetry/toggles + 4 selective diagnostics).
     */
    conductorTelemetry: {
        ID: ZCL_CLUSTER_Z906_TELEMETRY,
        attributes: {
            inputSignalLevel:       { ID: ZCL_ATTR_INPUT_SIGNAL_LEVEL,        type: ZCL_TYPE_DATA24, manufacturerCode: null },
            idleTime:               { ID: ZCL_ATTR_IDLE_TIME,                 type: ZCL_TYPE_DATA32, manufacturerCode: null },
            digitalSignalStatus:    { ID: ZCL_ATTR_DIGITAL_SIGNAL_STATUS,     type: ZCL_TYPE_UINT8,  manufacturerCode: null },
            autoStandby:            { ID: ZCL_ATTR_AUTO_STANDBY,              type: ZCL_TYPE_UINT8,  manufacturerCode: null },
            builtInLedEnabled:      { ID: ZCL_ATTR_BUILT_IN_LED_ENABLED,      type: ZCL_TYPE_UINT8,  manufacturerCode: null },
            cmdVerifierEnabled:     { ID: ZCL_ATTR_CMD_VERIFIER_ENABLED,      type: ZCL_TYPE_UINT8,  manufacturerCode: null },
            deviceHealth:           { ID: ZCL_ATTR_DEVICE_HEALTH,             type: ZCL_TYPE_UINT8,  manufacturerCode: null },
            healthPrimaryComponent: { ID: ZCL_ATTR_HEALTH_PRIMARY_COMPONENT, type: ZCL_TYPE_UINT8,  manufacturerCode: null },
            healthPrimaryReason:    { ID: ZCL_ATTR_HEALTH_PRIMARY_REASON,    type: ZCL_TYPE_UINT8,  manufacturerCode: null },
            socTemperature:         { ID: ZCL_ATTR_SOC_TEMPERATURE,          type: ZCL_TYPE_INT16,  manufacturerCode: null },
        },
        commands: {},
        commandsResponse: {},
    },
};

// ─── Configure Function ───────────────────────────────────────────────────────

/**
 * Performs cluster bindings and initial attribute reads on device interview.
 */
async function configure(device, coordinatorEndpoint, definition) {
    // ── EP 1: Standard clusters ────────────────────────────────────────────
    const ep1 = device.getEndpoint(1);
    await ep1.bind('genOnOff',     coordinatorEndpoint);
    await ep1.bind('genLevelCtrl', coordinatorEndpoint);

    // ── EP 2: Trims & Mute ─────────────────────────────────────────────────
    const ep2 = device.getEndpoint(2);
    await ep2.bind('conductorTrims', coordinatorEndpoint);

    // ── EP 3: Input & Effect ───────────────────────────────────────────────
    const ep3 = device.getEndpoint(3);
    await ep3.bind('conductorMode', coordinatorEndpoint);

    // ── EP 4: Telemetry ────────────────────────────────────────────────────
    const ep4 = device.getEndpoint(4);
    await ep4.bind('conductorTelemetry', coordinatorEndpoint);

    // ── EP 11/12/13: Temperature Measurement ─────────────────────────────
    const ep11 = device.getEndpoint(11);
    const ep12 = device.getEndpoint(12);
    const ep13 = device.getEndpoint(13);
    await ep11.bind('msTemperatureMeasurement', coordinatorEndpoint);
    await ep12.bind('msTemperatureMeasurement', coordinatorEndpoint);
    await ep13.bind('msTemperatureMeasurement', coordinatorEndpoint);

    // ── Initial Reads — bootstrap state before first hardware report ───────
    try { await ep1.read('genOnOff',     ['onOff']);       } catch (_) { /* tolerable */ }
    try { await ep1.read('genLevelCtrl', ['currentLevel']); } catch (_) { /* tolerable */ }
    try {
        await ep2.read('conductorTrims', [
            ZCL_ATTR_TRIM_BASS, ZCL_ATTR_TRIM_CENTER, ZCL_ATTR_TRIM_REAR, ZCL_ATTR_AUDIO_MUTE,
        ]);
    } catch (_) { /* tolerable */ }
    try {
        await ep3.read('conductorMode', [ZCL_ATTR_SOURCE_INPUT, ZCL_ATTR_SOURCE_EFFECT]);
    } catch (_) { /* tolerable */ }
    try {
        await ep4.read('conductorTelemetry', [
            ZCL_ATTR_INPUT_SIGNAL_LEVEL,
            ZCL_ATTR_IDLE_TIME,
            ZCL_ATTR_DIGITAL_SIGNAL_STATUS,
            ZCL_ATTR_AUTO_STANDBY,
            ZCL_ATTR_BUILT_IN_LED_ENABLED,
            ZCL_ATTR_CMD_VERIFIER_ENABLED,
            ZCL_ATTR_DEVICE_HEALTH,
            ZCL_ATTR_HEALTH_PRIMARY_COMPONENT,
            ZCL_ATTR_HEALTH_PRIMARY_REASON,
            ZCL_ATTR_SOC_TEMPERATURE,
        ]);
    } catch (_) { /* tolerable */ }
    try { await ep11.read('msTemperatureMeasurement', [ZCL_ATTR_TEMP_MEASURED_VALUE]); } catch (_) { /* tolerable */ }
    try { await ep12.read('msTemperatureMeasurement', [ZCL_ATTR_TEMP_MEASURED_VALUE]); } catch (_) { /* tolerable */ }
    try { await ep13.read('msTemperatureMeasurement', [ZCL_ATTR_TEMP_MEASURED_VALUE]); } catch (_) { /* tolerable */ }
}

// ─── Device Definition ────────────────────────────────────────────────────────

const definition = {
    fingerprint: [
        {
            modelID: 'Z-906-Zigbee',    // Matches firmware Z906_ZB_MODEL_IDENTIFIER
            manufacturerName: 'Logitech', // Matches firmware Z906_ZB_MANUFACTURER_NAME
        },
    ],
    model:       'Conductor',
    vendor:      'Conductor',
    description: 'Smart Zigbee Console for Logitech Z906',
    ota:         true,  // ZHC 21.x: OTA enabled via `ota: true`

    /** Internal endpoint map */
    endpoint: (device) => ({
        main:          1,   // genOnOff + genLevelCtrl + OTA client
        trims:         2,   // 0xFC01 Trims & Mute
        mode:          3,   // 0xFC02 Input & Effect
        telemetry:     4,   // 0xFC03 Telemetry, Feature Toggles & Health Diagnostics
        temperature_1: 11,  // Standard 0x0402 Temp Sensor 1
        temperature_2: 12,  // Standard 0x0402 Temp Sensor 2
        temperature_3: 13,  // Standard 0x0402 Temp Sensor 3
    }),

    /** fromZigbee inbound report/read-response parsers */
    fromZigbee: [
        fzMain,
        fzVolume,
        fzTrims,
        fzSources,
        fzTelemetry,
        fzTemperature,
    ],

    /** toZigbee outbound command/write generators — all return null (non-optimistic) */
    toZigbee: [
        tzPower,
        tzVolume,
        tzMute,
        tzInput,
        tzEffect,
        tzTrimBass,
        tzTrimCenter,
        tzTrimRear,
        tzBuiltInLed,
        tzCommandVerifier,
    ],

    configure,
    zcl: { clusters: customClusters },

    /** Public property schema exposed to Home Assistant via Zigbee2MQTT */
    exposes: [
        // ── EP 1: Main Control ──────────────────────────────────────────────
        e.switch().withState('state', true,
            'Power: ON = Active, OFF = Standby. ' +
            'Firmware verifies physical state before publishing the attribute report.'
        ),

        e.numeric('volume', ea.ALL)
            .withValueMin(0)
            .withValueMax(43)
            .withDescription('Master volume in Z906 discrete hardware steps (0 = silent, 43 = maximum)'),

        // ── EP 2: Channel Trims & Mute ──────────────────────────────────────
        e.numeric('trim_bass', ea.ALL)
            .withValueMin(0)
            .withValueMax(43)
            .withDescription('Subwoofer bass channel level (0-43 hardware steps)'),

        e.numeric('trim_center', ea.ALL)
            .withValueMin(0)
            .withValueMax(43)
            .withDescription('Center speaker channel level (0-43 hardware steps)'),

        e.numeric('trim_rear', ea.ALL)
            .withValueMin(0)
            .withValueMax(43)
            .withDescription('Rear surround speakers level (0-43 hardware steps)'),

        e.binary('mute', ea.ALL, 'ON', 'OFF')
            .withDescription('Amplifier audio mute. ON = muted (canonical bracket sequence), OFF = unmuted.'),

        // ── EP 3: Source Selection ───────────────────────────────────────────
        e.enum('input', ea.ALL, [
            '6-Channel Direct (Input 1)',
            'RCA Stereo (Input 2)',
            'Optical 1 (Input 3)',
            'Optical 2 (Input 4)',
            'Digital Coaxial (Input 5)',
            'Auxiliary / AUX (Input 6)',
        ]).withDescription(
            'Active audio input. Analog inputs support all spatial effects. ' +
            'Digital inputs (Optical 1/2, Coaxial) force effect to None via firmware.'
        ),

        e.enum('effect', ea.ALL, ['3D', '2.1', '4.1', 'None'])
            .withDescription('Spatial audio effect. Digital inputs always force None.'),

        // ── EP 4: Telemetry (Diagnostic, Read-Only) ─────────────────────────
        e.numeric('input_signal_level', ea.STATE)
            .withDescription(
                'Raw input signal amplitude (U24 value from 0x25 telemetry frame). ' +
                'Physical unit undocumented in nomis/logitech-z906 reverse-engineering reference.'
            ),

        e.numeric('idle_time', ea.STATE)
            .withUnit('s')
            .withDescription('Elapsed idle time in seconds (0x31 query response).'),

        e.enum('digital_signal_status', ea.STATE,
            ['No Signal', 'PCM / Streaming', 'DTS / Bitstream', 'Unknown']
        ).withDescription(
            'Digital audio stream detection / decode state (Byte 12 of 0xAA 0x0A 0x14 frame).'
        ),

        e.binary('auto_standby', ea.STATE, 'Enabled', 'Disabled')
            .withDescription(
                'Z906 auto-standby (2-hour idle auto-shutdown). ' +
                'Read-only: no firmware write command exists for this setting.'
            ),

        // ── EP 4: Feature Toggles (Configuration, Read/Write) ───────────────
        e.binary('built_in_led_enabled', ea.ALL, 'ON', 'OFF')
            .withDescription(
                'Onboard RGB LED notification system. ' +
                'OFF disables all notification animations and turns off the physical LED.'
            ),

        e.binary('command_verifier_enabled', ea.ALL, 'ON', 'OFF')
            .withDescription(
                'State-based command verification engine. ' +
                'When OFF, commands are dispatched immediately without physical verification polling.'
            ),

        // ── EP 4: Selective Health Diagnostics (Diagnostic, Read-Only) ───────
        e.enum('device_health', ea.STATE, ['unknown', 'healthy', 'degraded', 'fault'])
            .withCategory('diagnostic')
            .withDescription('Overall operational health status classified by central Health Manager.'),

        e.enum('health_primary_component', ea.STATE, [
            'system',
            'system_metrics',
            'soc_temperature',
            'cpu',
            'memory',
            'watchdog',
            'nvs',
            'initializer',
            'zigbee',
            'z906_transport',
            'command_verifier',
            'ota',
            'unknown',
        ])
            .withCategory('diagnostic')
            .withDescription('Primary system component causing health degradation or fault.'),

        e.enum('health_primary_reason', ea.STATE, [
            'none',
            'not_initialized',
            'metric_not_ready',
            'metric_stale',
            'soc_temp_warning',
            'soc_temp_critical',
            'soc_temp_sensor_error',
            'cpu_load_sustained_high',
            'cpu_load_critical',
            'heap_warning',
            'heap_critical',
            'heap_fragmented',
            'heap_historical_pressure',
            'watchdog_reset_detected',
            'watchdog_task_stalled',
            'zigbee_commission_failed',
            'zigbee_degraded',
            'z906_stale',
            'z906_communication_fault',
            'command_verifier_unavailable',
            'ota_health_fault',
            'nvs_fault',
            'initializer_failed',
            'unknown',
        ])
            .withCategory('diagnostic')
            .withDescription('Root cause reason code explaining current health degradation or fault.'),

        e.numeric('soc_temperature', ea.STATE)
            .withUnit('°C')
            .withCategory('diagnostic')
            .withDescription(
                'ESP32-C6 internal SoC die temperature. ' +
                'Rate-limited continuous telemetry from System Metrics (0x8000 sentinel suppressed).'
            ),

        // ── EP 11/12/13: Temperature (Diagnostic, Read-Only) ─────────────────
        e.numeric('temperature_1', ea.STATE)
            .withUnit('°C')
            .withDescription(
                'Z906 amplifier temperature sensor 1 (heatsink area). ' +
                'Source: 0x25 telemetry query, byte 0. ' +
                '0x8000 sentinel (no valid reading) is suppressed — property not updated.'
            ),

        e.numeric('temperature_2', ea.STATE)
            .withUnit('°C')
            .withDescription(
                'Z906 amplifier temperature sensor 2 (DSP/amplifier area). ' +
                'Source: 0x25 telemetry query, byte 1. ' +
                '0x8000 sentinel (no valid reading) is suppressed — property not updated.'
            ),

        e.numeric('temperature_3', ea.STATE)
            .withUnit('°C')
            .withDescription(
                'Z906 power supply transformer temperature. ' +
                'Source: 0x25 telemetry query, byte 2. ' +
                '0x8000 sentinel (no valid reading) is suppressed — property not updated.'
            ),

        // ── Connection Diagnostics (Read-Only, No Endpoint) ──────────────────
        e.binary('cable_connected', ea.STATE, true, false)
            .withDescription('DE-15 console cable physical connection state (true = connected).'),

        e.binary('is_operational', ea.STATE, true, false)
            .withDescription('Z906 operational health (true = cable connected and console fault-free).'),
    ],
};

// ─── ESM Default Export ───────────────────────────────────────────────────────

export default definition;

// ─── Internal Exports for Test Harness ───────────────────────────────────────
export {
    volumeToZcl,
    zclToVolume,
    decodeDigitalSignalStatus,
    parseData24,
    parseData32,
    INPUT_MAP_TO_NUM,
    INPUT_MAP_FROM_NUM,
    EFFECT_MAP_TO_NUM,
    EFFECT_MAP_FROM_NUM,
    HEALTH_STATUS_MAP,
    HEALTH_COMPONENT_MAP,
    HEALTH_REASON_MAP,
    fzMain,
    fzVolume,
    fzTrims,
    fzSources,
    fzTelemetry,
    fzTemperature,
    tzPower,
    tzVolume,
    tzMute,
    tzInput,
    tzEffect,
    tzTrimBass,
    tzTrimCenter,
    tzTrimRear,
    tzBuiltInLed,
    tzCommandVerifier,
    customClusters,
    ZCL_CLUSTER_Z906_TRIMS,
    ZCL_CLUSTER_Z906_SOURCES,
    ZCL_CLUSTER_Z906_TELEMETRY,
    ZCL_ATTR_TRIM_BASS, ZCL_ATTR_TRIM_CENTER, ZCL_ATTR_TRIM_REAR, ZCL_ATTR_AUDIO_MUTE,
    ZCL_ATTR_SOURCE_INPUT, ZCL_ATTR_SOURCE_EFFECT,
    ZCL_ATTR_INPUT_SIGNAL_LEVEL, ZCL_ATTR_IDLE_TIME, ZCL_ATTR_DIGITAL_SIGNAL_STATUS,
    ZCL_ATTR_AUTO_STANDBY, ZCL_ATTR_BUILT_IN_LED_ENABLED, ZCL_ATTR_CMD_VERIFIER_ENABLED,
    ZCL_ATTR_DEVICE_HEALTH, ZCL_ATTR_HEALTH_PRIMARY_COMPONENT, ZCL_ATTR_HEALTH_PRIMARY_REASON, ZCL_ATTR_SOC_TEMPERATURE,
    ZCL_TYPE_UINT8, ZCL_TYPE_DATA24, ZCL_TYPE_DATA32, ZCL_TYPE_INT16,
    ZCL_TEMP_INVALID_SENTINEL,
};
