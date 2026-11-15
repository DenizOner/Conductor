'use strict';

/**
 * @file test_converter.js
 * @brief Canonical automated unit test suite for conductor_converter.mjs (Phase 8 ESM converter).
 *
 * This is the single authoritative converter test entry point.
 * Run command: node tools/test_converter.js
 *
 * ─────────────────────────────────────────────────────────────────────────────
 * DEPENDENCY RESOLUTION:
 *   conductor_converter.mjs imports from 'zigbee-herdsman-converters'.
 *   When running outside a real Zigbee2MQTT environment, local stubs are required.
 *
 *   Stub setup (run once before test execution):
 *     node tools/setup_test_env.js
 *
 *   Or run directly from .temporary/phase8-z2m-real/ if real package is installed.
 *
 *   CREDIBILITY NOTE:
 *   These tests validate converter LOGIC (fromZigbee/toZigbee correctness,
 *   attribute ID mapping, data type parsing, error handling).
 *   They are NOT a substitute for real zigbee-herdsman-converters import smoke test.
 *   Real API compatibility is validated separately (see tools/test_converter_real_api.mjs).
 *
 * ─────────────────────────────────────────────────────────────────────────────
 * Test Suites:
 *   Suite 1:  Device Metadata, Fingerprint & Endpoint Map
 *   Suite 2:  Volume & Scale Conversions
 *   Suite 3:  toZigbee Outbound Handlers (non-optimistic)
 *   Suite 4:  fromZigbee Inbound Parsers
 *   Suite 5:  FC03 Telemetry (F001 migration & Health Diagnostics)
 *   Suite 6:  Temperature EP11/12/13 (0x8000 sentinel suppression)
 *   Suite 7:  F001 Migration Verification (static source analysis)
 *   Suite 8:  Binding & Configure Architecture (static analysis)
 *   Suite 9:  No Optimistic State, Strict Validation & Error Resilience
 *   Suite 10: OTA Capability
 *   Suite 11: DATA24/DATA32 Parsing
 *   Suite 12: Custom Cluster Schema
 */

const assert           = require('assert');
const path             = require('path');
const fs               = require('fs');
const { pathToFileURL } = require('url');  // Required on Windows for file:// ESM import

console.log('=== Running Conductor Converter Unit Tests ===\n');
console.log('[CREDIBILITY] MOCK UNIT TESTS — uses local ESM stubs, not real ZHC package.');
console.log('[CREDIBILITY] For real ZHC API smoke test: node tools/test_converter_real_api.mjs\n');

let passedTests = 0;
let totalTests  = 0;

/** Synchronous test runner */
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

/** Async test runner */
async function itAsync(desc, fn) {
    totalTests++;
    try {
        await fn();
        console.log(`  [PASS] ${desc}`);
        passedTests++;
    } catch (err) {
        console.error(`  [FAIL] ${desc}`);
        console.error(`         ${err.message}`);
    }
}

// ─── Mock helpers for toZigbee handler testing ────────────────────────────────

/** Creates a mock herdsman endpoint. Records all command/write calls. */
function makeMockEntity(epId) {
    const calls   = [];
    const makeEp  = (id) => ({
        ID: id,
        command: async (cluster, cmd, payload) => { calls.push({ type: 'command', ep: id, cluster, cmd, payload }); },
        write:   async (cluster, attrs)          => { calls.push({ type: 'write',   ep: id, cluster, attrs }); },
    });
    const entity  = makeEp(epId);
    entity.getEndpoint = (id) => makeEp(id);
    entity._calls = calls;
    return { entity, calls };
}

/** Builds a minimal simulated attribute report message. */
function makeMsg(data, epId) {
    return { data, endpoint: { ID: epId } };
}

/** Raw source text of conductor_converter.mjs for static analysis tests. */
const CONVERTER_SRC = fs.readFileSync(
    path.join(__dirname, 'conductor_converter.mjs'), 'utf8'
);

// ─── Stub setup: create minimal local stubs so ESM import resolves ───────────
// These stubs shadow zigbee-herdsman-converters only within the test context.
// They must NOT persist in the repo as node_modules entries.

const STUBS_DIR = path.join(__dirname, '_test_stubs_');
const ZHC_DIR   = path.join(STUBS_DIR, 'node_modules', 'zigbee-herdsman-converters');

function ensureStubs() {
    if (fs.existsSync(path.join(ZHC_DIR, 'package.json'))) return; // Already created
    fs.mkdirSync(path.join(ZHC_DIR, 'lib'), { recursive: true });

    // package.json with ESM exports map — includes both bare and .js paths
    // because conductor_converter.mjs now uses explicit .js extensions (ZHC 21.x requirement)
    fs.writeFileSync(path.join(ZHC_DIR, 'package.json'), JSON.stringify({
        name: 'zigbee-herdsman-converters',
        version: '0.0.0-test-stub',
        type: 'module',
        exports: {
            './lib/exposes':    './lib/exposes.mjs',
            './lib/exposes.js': './lib/exposes.mjs',  // ZHC 21.x explicit .js import path
            './lib/ota':        './lib/ota.mjs',
            './lib/ota.js':     './lib/ota.mjs',      // ZHC 21.x explicit .js import path
        },
    }));

    // exposes.mjs stub
    fs.writeFileSync(path.join(ZHC_DIR, 'lib', 'exposes.mjs'), `
export const access = { ALL: 7, STATE: 1, SET: 2, STATE_SET: 3, STATE_GET: 5, GET: 4 };
function makeBase(type, name, acc) {
    const o = { type, name, access: acc, unit: undefined, description: undefined, category: undefined };
    o.withUnit        = (u) => { o.unit        = u; return o; };
    o.withDescription = (d) => { o.description = d; return o; };
    o.withEndpoint    = (e) => { o.endpoint    = e; return o; };
    o.withValueMin    = (v) => { o.value_min   = v; return o; };
    o.withValueMax    = (v) => { o.value_max   = v; return o; };
    o.withCategory    = (c) => { o.category    = c; return o; };
    return o;
}
export const numeric = (n, a) => makeBase('numeric', n, a);
export const binary  = (n, a, on, off) => Object.assign(makeBase('binary', n, a), { value_on: on, value_off: off });
export function enum_(n, a, vals) { return Object.assign(makeBase('enum', n, a), { values: vals }); }
export { enum_ as enum };
export function switch_() {
    const o = { type: 'switch', name: undefined, description: undefined, access: 7,
        withState(n, s, d) { o.name = n; o.description = d; return o; },
        withEndpoint(e) { o.endpoint = e; return o; } };
    return o;
}
export { switch_ as switch };
`);

    // ota.mjs stub
    fs.writeFileSync(path.join(ZHC_DIR, 'lib', 'ota.mjs'),
        `// ota:true is ZHC 21.x OTA pattern (zigbeeOTA removed in >= v18)\nexport const zigbeeOTA = undefined;\nexport const update = async () => {};\nexport const isUpdateAvailable = async () => false;\n`
    );
}

// ─── Main async test runner ───────────────────────────────────────────────────

(async () => {
    // Create temporary stubs in _test_stubs_/ (NOT in tools/node_modules)
    ensureStubs();

    // Patch module resolution: add _test_stubs_ as a node_modules search path
    // via a transient package.json in the test stubs directory
    const stubPkg = path.join(STUBS_DIR, 'package.json');
    if (!fs.existsSync(stubPkg)) {
        fs.writeFileSync(stubPkg, JSON.stringify({ name: 'conductor-test-stubs', version: '0.0.0', private: true }));
    }

    // Import conductor_converter.mjs from the stubs directory (symlink approach)
    // Since Node resolves node_modules upward, we copy the converter to the stubs dir
    // and import it from there so it finds our stubs.
    const converterSrc  = path.join(__dirname, 'conductor_converter.mjs');
    const converterDest = path.join(STUBS_DIR, 'conductor_converter.mjs');
    fs.copyFileSync(converterSrc, converterDest);

    let mod;
    try {
        // On Windows, dynamic import() requires a file:// URL — bare paths fail.
        const converterUrl = pathToFileURL(converterDest).href;
        mod = await import(converterUrl);
    } catch (err) {
        console.error(`[FATAL] Failed to import conductor_converter.mjs: ${err.message}`);
        console.error('        Stack:', err.stack);
        process.exit(1);
    }

    const def      = mod.default;
    const internals = mod;  // Named exports are the internals

    // ─────────────────────────────────────────────────────────────────────────
    // Suite 1: Device Metadata, Fingerprint & Endpoint Map
    // ─────────────────────────────────────────────────────────────────────────
    console.log('--- Suite 1: Device Metadata, Fingerprint & Endpoint Map ---');

    it('should have user-facing Conductor metadata (model/vendor/description)', () => {
        assert.strictEqual(def.model,  'Conductor');
        assert.strictEqual(def.vendor, 'Conductor');
        assert.ok(def.description.toLowerCase().includes('logitech z906'));
    });

    it('should use fingerprint-based detection matching firmware Basic identity', () => {
        assert.ok(Array.isArray(def.fingerprint));
        assert.ok(def.fingerprint.length >= 1);
        const fp = def.fingerprint[0];
        assert.strictEqual(fp.modelID,          'Z-906-Zigbee');
        assert.strictEqual(fp.manufacturerName, 'Logitech');
    });

    it('should define endpoint map with correct firmware endpoint IDs', () => {
        const ep = def.endpoint(null);
        assert.strictEqual(ep.main,          1);
        assert.strictEqual(ep.trims,         2);
        assert.strictEqual(ep.mode,          3);
        assert.strictEqual(ep.telemetry,     4);
        assert.strictEqual(ep.temperature_1, 11);
        assert.strictEqual(ep.temperature_2, 12);
        assert.strictEqual(ep.temperature_3, 13);
    });

    it('should expose all required controls, diagnostics, and configuration entities', () => {
        const names = def.exposes.map((e) => e.name);
        for (const n of ['state', 'volume', 'mute', 'input', 'effect',
                          'trim_bass', 'trim_center', 'trim_rear',
                          'built_in_led_enabled', 'command_verifier_enabled',
                          'input_signal_level', 'idle_time', 'digital_signal_status',
                          'auto_standby', 'temperature_1', 'temperature_2', 'temperature_3',
                          'cable_connected', 'is_operational',
                          'device_health', 'health_primary_component', 'health_primary_reason', 'soc_temperature']) {
            assert.ok(names.includes(n), `Missing expose: ${n}`);
        }
    });

    it('should configure units and bounds correctly for telemetry entities', () => {
        const temp3 = def.exposes.find((e) => e.name === 'temperature_3');
        assert.strictEqual(temp3.unit, '°C');
        assert.ok(temp3.description.toLowerCase().includes('transformer') ||
                  temp3.description.toLowerCase().includes('power supply'));
        const idle = def.exposes.find((e) => e.name === 'idle_time');
        assert.strictEqual(idle.unit, 's');
        const isl = def.exposes.find((e) => e.name === 'input_signal_level');
        assert.strictEqual(isl.unit, undefined, 'input_signal_level must not have a unit');
    });

    // ─────────────────────────────────────────────────────────────────────────
    // Suite 2: Volume & Scale Conversions
    // ─────────────────────────────────────────────────────────────────────────
    console.log('\n--- Suite 2: Volume & Scale Conversions ---');

    it('should convert hardware volume 0-43 → ZCL 0-254', () => {
        assert.strictEqual(internals.volumeToZcl(0),   0);
        assert.strictEqual(internals.volumeToZcl(43),  254);
        assert.strictEqual(internals.volumeToZcl(-5),  0);
        assert.strictEqual(internals.volumeToZcl(100), 254);
        assert.ok(Math.abs(internals.volumeToZcl(21) - 124) <= 2);
    });

    it('should convert ZCL 0-254 → hardware volume 0-43', () => {
        assert.strictEqual(internals.zclToVolume(0),   0);
        assert.strictEqual(internals.zclToVolume(254), 43);
        assert.strictEqual(internals.zclToVolume(-1),  0);
        assert.strictEqual(internals.zclToVolume(999), 43);
    });

    it('should guarantee lossless round-trip for canonical trim steps', () => {
        for (const step of [0, 1, 21, 22, 42, 43]) {
            const back = internals.zclToVolume(internals.volumeToZcl(step));
            assert.ok(Math.abs(back - step) <= 1,
                `Round-trip loss for step ${step}: got ${back}`);
        }
    });

    // ─────────────────────────────────────────────────────────────────────────
    // Suite 3: toZigbee Outbound Handlers
    // ─────────────────────────────────────────────────────────────────────────
    console.log('\n--- Suite 3: toZigbee Outbound Handlers ---');

    await itAsync('should send genOnOff commands for power ON/OFF and return null', async () => {
        const { entity, calls } = makeMockEntity(1);
        assert.strictEqual(await internals.tzPower.convertSet(entity, 'state', 'ON', {}), null);
        assert.ok(calls.some((c) => c.cmd === 'on'));
        calls.length = 0;
        await internals.tzPower.convertSet(entity, 'state', 'OFF', {});
        assert.ok(calls.some((c) => c.cmd === 'off'));
    });

    await itAsync('should send genLevelCtrl moveToLevel with ZCL conversion and return null', async () => {
        const { entity, calls } = makeMockEntity(1);
        assert.strictEqual(await internals.tzVolume.convertSet(entity, 'volume', '22', {}), null);
        const call = calls.find((c) => c.cmd === 'moveToLevel');
        assert.ok(call, 'moveToLevel not sent');
        assert.ok(call.payload.level >= 0 && call.payload.level <= 254);
        assert.strictEqual(call.payload.transtime, 0);
    });

    await itAsync('should clamp volume at hardware boundaries 0 and 43', async () => {
        const { entity: e1, calls: c1 } = makeMockEntity(1);
        await internals.tzVolume.convertSet(e1, 'volume', '-10', {});
        assert.strictEqual(c1.find((c) => c.cmd === 'moveToLevel').payload.level, 0);
        const { entity: e2, calls: c2 } = makeMockEntity(1);
        await internals.tzVolume.convertSet(e2, 'volume', '100', {});
        assert.strictEqual(c2.find((c) => c.cmd === 'moveToLevel').payload.level, 254);
    });

    await itAsync('should write mute to FC01/0x0003 as UINT8 and return null', async () => {
        const { entity, calls } = makeMockEntity(2);
        assert.strictEqual(await internals.tzMute.convertSet(entity, 'mute', 'ON', {}), null);
        const wr = calls.find((c) => c.type === 'write');
        assert.strictEqual(wr.cluster, 0xFC01);
        assert.strictEqual(wr.attrs[0x0003].value, 1);
        const { entity: e2, calls: c2 } = makeMockEntity(2);
        await internals.tzMute.convertSet(e2, 'mute', 'OFF', {});
        assert.strictEqual(c2.find((c) => c.type === 'write').attrs[0x0003].value, 0);
    });

    await itAsync('should write input to FC02/0x0000 with canonical key validation', async () => {
        const { entity, calls } = makeMockEntity(3);
        assert.strictEqual(await internals.tzInput.convertSet(entity, 'input', 'RCA Stereo (Input 2)', {}), null);
        assert.strictEqual(calls.find((c) => c.type === 'write').attrs[0x0000].value, 2);
    });

    await itAsync('should write effect to FC02/0x0001 correctly', async () => {
        const { entity, calls } = makeMockEntity(3);
        assert.strictEqual(await internals.tzEffect.convertSet(entity, 'effect', '4.1', {}), null);
        assert.strictEqual(calls.find((c) => c.type === 'write').attrs[0x0001].value, 2);
    });

    await itAsync('should write bass/center/rear trims to FC01 with ZCL conversion', async () => {
        for (const [handler, attrId] of [
            [internals.tzTrimBass, 0x0000], [internals.tzTrimCenter, 0x0001], [internals.tzTrimRear, 0x0002],
        ]) {
            const { entity, calls } = makeMockEntity(2);
            assert.strictEqual(await handler.convertSet(entity, handler.key[0], '21', {}), null);
            const wr = calls.find((c) => c.type === 'write');
            assert.strictEqual(wr.cluster, 0xFC01);
            assert.ok(wr.attrs[attrId].value >= 0 && wr.attrs[attrId].value <= 254);
        }
    });

    await itAsync('should write built_in_led_enabled to FC03/0x0007', async () => {
        const { entity, calls } = makeMockEntity(4);
        assert.strictEqual(await internals.tzBuiltInLed.convertSet(entity, 'built_in_led_enabled', 'OFF', {}), null);
        const wr = calls.find((c) => c.type === 'write');
        assert.strictEqual(wr.cluster, 0xFC03);
        assert.strictEqual(wr.attrs[0x0007].value, 0);
    });

    await itAsync('should write command_verifier_enabled to FC03/0x0008', async () => {
        const { entity, calls } = makeMockEntity(4);
        assert.strictEqual(await internals.tzCommandVerifier.convertSet(entity, 'command_verifier_enabled', 'ON', {}), null);
        const wr = calls.find((c) => c.type === 'write');
        assert.strictEqual(wr.cluster, 0xFC03);
        assert.strictEqual(wr.attrs[0x0008].value, 1);
    });

    // ─────────────────────────────────────────────────────────────────────────
    // Suite 4: fromZigbee Inbound Parsers
    // ─────────────────────────────────────────────────────────────────────────
    console.log('\n--- Suite 4: fromZigbee Inbound Parsers ---');

    it('should parse Power from genOnOff report', () => {
        const c = internals.fzMain;
        assert.strictEqual(c.convert(null, makeMsg({ onOff: 1 }), null, null, null).state, 'ON');
        assert.strictEqual(c.convert(null, makeMsg({ onOff: 0 }), null, null, null).state, 'OFF');
    });

    it('should parse Volume and convert ZCL level to 0-43 step', () => {
        const res = internals.fzVolume.convert(null, makeMsg({ currentLevel: 127 }), null, null, null);
        assert.ok(res.volume >= 20 && res.volume <= 23);
    });

    it('should parse FC01 trims and mute correctly', () => {
        const res = internals.fzTrims.convert(null, makeMsg({
            0x0000: 127, 0x0001: 254, 0x0002: 0, 0x0003: 1,
        }), null, null, null);
        assert.ok(res.trim_bass >= 20 && res.trim_bass <= 23);
        assert.strictEqual(res.trim_center, 43);
        assert.strictEqual(res.trim_rear,   0);
        assert.strictEqual(res.mute,        'ON');
    });

    it('should parse FC02 input and effect correctly', () => {
        const res = internals.fzSources.convert(null, makeMsg({ 0x0000: 3, 0x0001: 1 }), null, null, null);
        assert.strictEqual(res.input,  'Optical 1 (Input 3)');
        assert.strictEqual(res.effect, '2.1');
    });

    it('should return {} from fromZigbee parsers on null data (error resilience)', () => {
        for (const h of [internals.fzMain, internals.fzVolume, internals.fzTrims,
                          internals.fzSources, internals.fzTelemetry]) {
            const res = h.convert(null, { data: null, endpoint: { ID: 1 } }, null, null, null);
            assert.deepStrictEqual(res, {}, `${h.cluster} must return {} on null data`);
        }
    });

    // ─────────────────────────────────────────────────────────────────────────
    // Suite 5: FC03 Telemetry — F001 Correct Attribute IDs & Health Diagnostics
    // ─────────────────────────────────────────────────────────────────────────
    console.log('\n--- Suite 5: FC03 Telemetry (F001 correct attr IDs & Health) ---');

    it('should parse input_signal_level from FC03/0x0000 as DATA24', () => {
        const res = internals.fzTelemetry.convert(null,
            makeMsg({ [0x0000]: Buffer.from([0x56, 0x34, 0x12]) }), null, null, null);
        assert.strictEqual(res.input_signal_level, 0x123456);
    });

    it('should parse idle_time from FC03/0x0001 as DATA32 (seconds)', () => {
        const buf = Buffer.allocUnsafe(4);
        buf.writeUInt32LE(3600, 0);
        const res = internals.fzTelemetry.convert(null, makeMsg({ [0x0001]: buf }), null, null, null);
        assert.strictEqual(res.idle_time, 3600);
    });

    it('should parse digital_signal_status from FC03/0x0002 as decoded string', () => {
        const c = internals.fzTelemetry;
        assert.strictEqual(c.convert(null, makeMsg({ [0x0002]: 0x01 }), null, null, null).digital_signal_status, 'PCM / Streaming');
        assert.strictEqual(c.convert(null, makeMsg({ [0x0002]: 0x00 }), null, null, null).digital_signal_status, 'No Signal');
    });

    it('should parse auto_standby from FC03/0x0003', () => {
        const c = internals.fzTelemetry;
        assert.strictEqual(c.convert(null, makeMsg({ [0x0003]: 1 }), null, null, null).auto_standby, 'Enabled');
        assert.strictEqual(c.convert(null, makeMsg({ [0x0003]: 0 }), null, null, null).auto_standby, 'Disabled');
    });

    it('should parse built_in_led_enabled from FC03/0x0007', () => {
        const res = internals.fzTelemetry.convert(null, makeMsg({ [0x0007]: 1 }), null, null, null);
        assert.strictEqual(res.built_in_led_enabled, 'ON');
    });

    it('should parse command_verifier_enabled from FC03/0x0008', () => {
        const res = internals.fzTelemetry.convert(null, makeMsg({ [0x0008]: 0 }), null, null, null);
        assert.strictEqual(res.command_verifier_enabled, 'OFF');
    });

    it('should parse device_health from FC03/0x0009', () => {
        const c = internals.fzTelemetry;
        assert.strictEqual(c.convert(null, makeMsg({ [0x0009]: 0 }), null, null, null).device_health, 'unknown');
        assert.strictEqual(c.convert(null, makeMsg({ [0x0009]: 1 }), null, null, null).device_health, 'healthy');
        assert.strictEqual(c.convert(null, makeMsg({ [0x0009]: 2 }), null, null, null).device_health, 'degraded');
        assert.strictEqual(c.convert(null, makeMsg({ [0x0009]: 3 }), null, null, null).device_health, 'fault');
    });

    it('should parse health_primary_component from FC03/0x000A', () => {
        const c = internals.fzTelemetry;
        assert.strictEqual(c.convert(null, makeMsg({ [0x000A]: 0 }), null, null, null).health_primary_component, 'system');
        assert.strictEqual(c.convert(null, makeMsg({ [0x000A]: 2 }), null, null, null).health_primary_component, 'soc_temperature');
        assert.strictEqual(c.convert(null, makeMsg({ [0x000A]: 4 }), null, null, null).health_primary_component, 'memory');
        assert.strictEqual(c.convert(null, makeMsg({ [0x000A]: 5 }), null, null, null).health_primary_component, 'watchdog');
    });

    it('should parse health_primary_reason from FC03/0x000B', () => {
        const c = internals.fzTelemetry;
        assert.strictEqual(c.convert(null, makeMsg({ [0x000B]: 0 }), null, null, null).health_primary_reason, 'none');
        assert.strictEqual(c.convert(null, makeMsg({ [0x000B]: 1 }), null, null, null).health_primary_reason, 'not_initialized');
        assert.strictEqual(c.convert(null, makeMsg({ [0x000B]: 4 }), null, null, null).health_primary_reason, 'soc_temp_warning');
        assert.strictEqual(c.convert(null, makeMsg({ [0x000B]: 9 }), null, null, null).health_primary_reason, 'heap_warning');
        assert.strictEqual(c.convert(null, makeMsg({ [0x000B]: 10 }), null, null, null).health_primary_reason, 'heap_critical');
        assert.strictEqual(c.convert(null, makeMsg({ [0x000B]: 13 }), null, null, null).health_primary_reason, 'watchdog_reset_detected');
    });

    it('should parse soc_temperature from FC03/0x000C and scale by / 100.0', () => {
        const c = internals.fzTelemetry;
        assert.strictEqual(c.convert(null, makeMsg({ [0x000C]: 4550 }), null, null, null).soc_temperature, 45.5);
    });

    it('should suppress 0x8000 sentinel on soc_temperature (no fake 0 or negative)', () => {
        const c = internals.fzTelemetry;
        assert.deepStrictEqual(c.convert(null, makeMsg({ [0x000C]: 0x8000 }), null, null, null), {});
        assert.deepStrictEqual(c.convert(null, makeMsg({ [0x000C]: -32768 }), null, null, null), {});
    });

    it('should decode all canonical digital signal decode states', () => {
        const d = internals.decodeDigitalSignalStatus;
        assert.strictEqual(d(0x00), 'No Signal');
        assert.strictEqual(d(0x01), 'PCM / Streaming');
        assert.strictEqual(d(0x0E), 'DTS / Bitstream');
        assert.strictEqual(d(0xFF), 'Unknown');
    });

    // ─────────────────────────────────────────────────────────────────────────
    // Suite 6: Temperature EP11/12/13 — 0x8000 Sentinel Suppression
    // ─────────────────────────────────────────────────────────────────────────
    console.log('\n--- Suite 6: Temperature EP11/12/13 (0x8000 sentinel) ---');

    it('should map EP11 measuredValue → temperature_1', () => {
        const res = internals.fzTemperature.convert(null, makeMsg({ measuredValue: 2500 }, 11), null, null, null);
        assert.strictEqual(res.temperature_1, 25.0);
    });

    it('should map EP12 measuredValue → temperature_2', () => {
        const res = internals.fzTemperature.convert(null, makeMsg({ measuredValue: 3500 }, 12), null, null, null);
        assert.strictEqual(res.temperature_2, 35.0);
    });

    it('should map EP13 measuredValue → temperature_3', () => {
        const res = internals.fzTemperature.convert(null, makeMsg({ measuredValue: 4200 }, 13), null, null, null);
        assert.strictEqual(res.temperature_3, 42.0);
    });

    it('should suppress 0x8000 sentinel on EP11 (no fake -327.68°C)', () => {
        const res = internals.fzTemperature.convert(null, makeMsg({ measuredValue: 0x8000 }, 11), null, null, null);
        assert.deepStrictEqual(res, {});
    });

    it('should suppress 0x8000 sentinel on EP12 and EP13', () => {
        assert.deepStrictEqual(internals.fzTemperature.convert(null, makeMsg({ measuredValue: 0x8000 }, 12), null, null, null), {});
        assert.deepStrictEqual(internals.fzTemperature.convert(null, makeMsg({ measuredValue: 0x8000 }, 13), null, null, null), {});
    });

    it('should suppress -32768 (signed form of 0x8000) on EP11', () => {
        const res = internals.fzTemperature.convert(null, makeMsg({ measuredValue: -32768 }, 11), null, null, null);
        assert.deepStrictEqual(res, {});
    });

    it('should return {} for unknown endpoint in temperature handler', () => {
        const res = internals.fzTemperature.convert(null, makeMsg({ measuredValue: 2500 }, 5), null, null, null);
        assert.deepStrictEqual(res, {});
    });

    it('should parse negative temperatures correctly', () => {
        const res = internals.fzTemperature.convert(null, makeMsg({ measuredValue: -500 }, 11), null, null, null);
        assert.strictEqual(res.temperature_1, -5.0);
    });

    // ─────────────────────────────────────────────────────────────────────────
    // Suite 7: F001 Migration Verification (static source analysis)
    // ─────────────────────────────────────────────────────────────────────────
    console.log('\n--- Suite 7: F001 Migration (static source analysis) ---');

    it('should have ZERO legacy FC03 attrs 0x0004/0x0005/0x0006 in production code', () => {
        const stripped = CONVERTER_SRC.replace(/\/\*[\s\S]*?\*\//g, '').replace(/\/\/.*/g, '');
        const hits = (stripped.match(/\[0x0004\]|\[0x0005\]|\[0x0006\]/g) || []).length;
        assert.strictEqual(hits, 0, `Legacy FC03 attrs 0x0004-0x0006 must be absent (found ${hits})`);
    });

    it('should have ZERO occurrences of 0xF001 anywhere', () => {
        assert.ok(!CONVERTER_SRC.includes('0xF001'), '0xF001 must not appear');
    });

    it('should not reference ZCL_ATTR_AMP_TEMP_1/2/3 legacy constants', () => {
        assert.ok(!CONVERTER_SRC.includes('ZCL_ATTR_AMP_TEMP_1'));
        assert.ok(!CONVERTER_SRC.includes('ZCL_ATTR_AMP_TEMP_2'));
        assert.ok(!CONVERTER_SRC.includes('ZCL_ATTR_AMP_TEMP_3'));
    });

    it('should have FC03/0x0000 mapped to input_signal_level (not temperature)', () => {
        assert.ok(CONVERTER_SRC.includes('ZCL_ATTR_INPUT_SIGNAL_LEVEL'));
        assert.ok(CONVERTER_SRC.includes('input_signal_level'));
        assert.ok(!CONVERTER_SRC.includes('AMP_TEMP'));
    });

    it('should confirm FC03 cluster has exactly 10 canonical attributes', () => {
        const fc03 = internals.customClusters.conductorTelemetry;
        assert.strictEqual(Object.keys(fc03.attributes).length, 10);
        const ids = Object.values(fc03.attributes).map((a) => a.ID);
        for (const id of [0x0000, 0x0001, 0x0002, 0x0003, 0x0007, 0x0008, 0x0009, 0x000A, 0x000B, 0x000C]) {
            assert.ok(ids.includes(id), `FC03 must include attr 0x${id.toString(16).padStart(4,'0')}`);
        }
        for (const id of [0x0004, 0x0005, 0x0006]) {
            assert.ok(!ids.includes(id), `FC03 must NOT include legacy attr 0x${id.toString(16).padStart(4,'0')}`);
        }
    });

    it('should confirm fzTelemetry never produces temperature_* keys', () => {
        const res = internals.fzTelemetry.convert(null,
            makeMsg({ [0x0000]: 0, [0x0001]: 0, [0x0002]: 0, [0x0003]: 0, [0x0007]: 0, [0x0008]: 0,
                      [0x0009]: 1, [0x000A]: 0, [0x000B]: 0, [0x000C]: 4000 }),
            null, null, null);
        assert.ok(!('temperature_1' in res));
        assert.ok(!('temperature_2' in res));
        assert.ok(!('temperature_3' in res));
    });

    // ─────────────────────────────────────────────────────────────────────────
    // Suite 8: Binding & Configure Architecture (static analysis)
    // ─────────────────────────────────────────────────────────────────────────
    console.log('\n--- Suite 8: Binding & Configure Architecture ---');

    it('should define a configure function', () => {
        assert.strictEqual(typeof def.configure, 'function');
    });

    it('should bind all 8 required cluster/endpoint pairs (static check)', () => {
        for (const [cluster, label] of [
            ['genOnOff',                 'EP1 genOnOff'],
            ['genLevelCtrl',             'EP1 genLevelCtrl'],
            ['conductorTrims',           'EP2 FC01'],
            ['conductorMode',            'EP3 FC02'],
            ['conductorTelemetry',       'EP4 FC03'],
            ['msTemperatureMeasurement', 'EP11/12/13 temperature'],
        ]) {
            assert.ok(CONVERTER_SRC.includes(`bind('${cluster}'`),
                `configure() must bind ${label}`);
        }
    });

    it('should NOT call configureReporting (no duplicate reporting engine)', () => {
        const stripped = CONVERTER_SRC.replace(/\/\*[\s\S]*?\*\//g, '').replace(/\/\/.*/g, '');
        assert.ok(!stripped.includes('configureReporting'), 'configureReporting must not be called');
    });

    it('should perform initial reads for all canonical endpoints', () => {
        assert.ok(CONVERTER_SRC.includes(".read('genOnOff'"));
        assert.ok(CONVERTER_SRC.includes(".read('genLevelCtrl'"));
        assert.ok(CONVERTER_SRC.includes(".read('conductorTrims'"));
        assert.ok(CONVERTER_SRC.includes(".read('conductorTelemetry'"));
    });

    it('should NOT bind the OTA Upgrade client cluster', () => {
        assert.ok(!CONVERTER_SRC.includes("bind('genOta") &&
                  !CONVERTER_SRC.includes("bind('otaUpgrade"));
    });

    // ─────────────────────────────────────────────────────────────────────────
    // Suite 9: No Optimistic State, Strict Validation & Error Resilience
    // ─────────────────────────────────────────────────────────────────────────
    console.log('\n--- Suite 9: No Optimistic State & Validation ---');

    await itAsync('should return null from ALL convertSet handlers', async () => {
        const { entity } = makeMockEntity(1);
        const handlers = [
            [internals.tzPower,           'state',                    'ON'],
            [internals.tzVolume,          'volume',                   '22'],
            [internals.tzMute,            'mute',                     'ON'],
            [internals.tzTrimBass,        'trim_bass',                '10'],
            [internals.tzTrimCenter,      'trim_center',              '10'],
            [internals.tzTrimRear,        'trim_rear',                '10'],
            [internals.tzBuiltInLed,      'built_in_led_enabled',     'ON'],
            [internals.tzCommandVerifier, 'command_verifier_enabled', 'ON'],
        ];
        for (const [handler, key, val] of handlers) {
            const res = await handler.convertSet(entity, key, val, {});
            assert.strictEqual(res, null, `${key} must return null`);
        }
    });

    await itAsync('should reject non-canonical input string', async () => {
        const { entity } = makeMockEntity(3);
        await assert.rejects(
            () => internals.tzInput.convertSet(entity, 'input', 'Input 3', {}),
            { message: /canonical/i }
        );
    });

    await itAsync('should reject invalid effect name', async () => {
        const { entity } = makeMockEntity(3);
        await assert.rejects(
            () => internals.tzEffect.convertSet(entity, 'effect', 'Surround', {}),
            /invalid/i
        );
    });

    it('should have exactly 6 canonical input entries and all string keys', () => {
        const keys = Object.keys(internals.INPUT_MAP_TO_NUM);
        assert.strictEqual(keys.length, 6);
        for (const k of keys) { assert.ok(isNaN(Number(k)), `"${k}" must be a string key`); }
    });

    // ─────────────────────────────────────────────────────────────────────────
    // Suite 10: OTA Capability
    // ─────────────────────────────────────────────────────────────────────────
    console.log('\n--- Suite 10: OTA Capability ---');

    it('should declare ota=true (ZHC 21.x: ota.zigbeeOTA removed in >=v18)', () => {
        assert.strictEqual(def.ota, true, 'def.ota must be exactly true (ZHC 21.x convention)');
    });

    it('should NOT import ota module (ota:true used instead of ota.zigbeeOTA)', () => {
        assert.ok(!CONVERTER_SRC.includes("import * as ota from"),
            'conductor_converter.mjs must not import ota module (ota:true is ZHC 21.x pattern)');
    });

    // ─────────────────────────────────────────────────────────────────────────
    // Suite 11: DATA24/DATA32 Parsing
    // ─────────────────────────────────────────────────────────────────────────
    console.log('\n--- Suite 11: DATA24/DATA32 Parsing ---');

    it('should parse DATA24 as little-endian 24-bit unsigned int', () => {
        const { parseData24 } = internals;
        assert.strictEqual(parseData24(Buffer.from([0x00, 0x00, 0x00])), 0);
        assert.strictEqual(parseData24(Buffer.from([0x56, 0x34, 0x12])), 0x123456);
        assert.strictEqual(parseData24(Buffer.from([0xFF, 0xFF, 0xFF])), 0xFFFFFF);
        assert.strictEqual(parseData24(0x1234), 0x1234);
    });

    it('should parse DATA32 as little-endian 32-bit unsigned int', () => {
        const buf = Buffer.allocUnsafe(4);
        buf.writeUInt32LE(3600, 0);
        assert.strictEqual(internals.parseData32(buf), 3600);
        assert.strictEqual(internals.parseData32(0), 0);
    });

    // ─────────────────────────────────────────────────────────────────────────
    // Suite 12: Custom Cluster Schema
    // ─────────────────────────────────────────────────────────────────────────
    console.log('\n--- Suite 12: Custom Cluster Schema ---');

    it('conductorTrims: ID=0xFC01, 4 attributes, all UINT8', () => {
        const cl = internals.customClusters.conductorTrims;
        assert.strictEqual(cl.ID, 0xFC01);
        assert.strictEqual(Object.keys(cl.attributes).length, 4);
        for (const a of Object.values(cl.attributes)) { assert.strictEqual(a.type, 0x20); }
    });

    it('conductorMode: ID=0xFC02, 2 attributes', () => {
        const cl = internals.customClusters.conductorMode;
        assert.strictEqual(cl.ID, 0xFC02);
        assert.strictEqual(Object.keys(cl.attributes).length, 2);
    });

    it('conductorTelemetry: ID=0xFC03, 10 attributes, correct DataTypes', () => {
        const cl = internals.customClusters.conductorTelemetry;
        assert.strictEqual(cl.ID, 0xFC03);
        assert.strictEqual(Object.keys(cl.attributes).length, 10);
        assert.strictEqual(cl.attributes.inputSignalLevel.type,        0x0A, 'input_signal_level must be DATA24');
        assert.strictEqual(cl.attributes.idleTime.type,                0x08, 'idle_time must be DATA32');
        assert.strictEqual(cl.attributes.digitalSignalStatus.type,     0x20, 'digital_signal_status must be UINT8');
        assert.strictEqual(cl.attributes.autoStandby.type,             0x20, 'auto_standby must be UINT8');
        assert.strictEqual(cl.attributes.builtInLedEnabled.type,       0x20, 'built_in_led_enabled must be UINT8');
        assert.strictEqual(cl.attributes.cmdVerifierEnabled.type,      0x20, 'cmd_verifier_enabled must be UINT8');
        assert.strictEqual(cl.attributes.deviceHealth.type,            0x20, 'device_health must be UINT8');
        assert.strictEqual(cl.attributes.healthPrimaryComponent.type, 0x20, 'health_primary_component must be UINT8');
        assert.strictEqual(cl.attributes.healthPrimaryReason.type,     0x20, 'health_primary_reason must be UINT8');
        assert.strictEqual(cl.attributes.socTemperature.type,          0x29, 'soc_temperature must be INT16');
    });

    it('def.zcl.clusters must expose all three custom clusters', () => {
        assert.ok(def.zcl?.clusters?.conductorTrims);
        assert.ok(def.zcl?.clusters?.conductorMode);
        assert.ok(def.zcl?.clusters?.conductorTelemetry);
    });

    // ─────────────────────────────────────────────────────────────────────────
    // Cleanup temp stubs
    // ─────────────────────────────────────────────────────────────────────────
    try { fs.rmSync(STUBS_DIR, { recursive: true, force: true }); } catch (_) { /* ignore */ }

    // ─────────────────────────────────────────────────────────────────────────
    // Final Report
    // ─────────────────────────────────────────────────────────────────────────
    console.log('\n========================================');
    console.log(`Results: ${passedTests}/${totalTests} tests passed.`);
    console.log('========================================\n');

    if (passedTests < totalTests) process.exit(1);
})();
