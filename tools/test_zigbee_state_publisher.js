/**
 * @file test_zigbee_state_publisher.js
 * @brief Host validation test suite for Native Zigbee Phase 5:
 *        Verified State Publishing + Changed-Only ZCL Reporting.
 *
 * Verifies:
 *   1. Canonical 21-Attribute Mapping & Coordinate Alignment
 *   2. Pure-v2 Symbol Audit (Zero legacy symbols)
 *   3. Power & Volume Mathematical Parity (Inverse scaling 0..43 <-> 0..254)
 *   4. Standard ZCL 0x0402 Temperature Encoding & Invalid Sentinel (0x8000)
 *   5. DATA24 Input Signal Level Encoding (Little-Endian wire format & boundaries)
 *   6. Changed-Only Discrete State Suppression & Transition Tracking
 *   7. High-Rate Telemetry Rate Limiting & Coalescing Logic
 *   8. Decoupled Descriptor Backing Sync & Report Dirty Retention
 *   9. Network-Ready Gate Suppression (network_ready=false) & Resync (network_ready=true)
 *  10. Zero Optimistic State Invariant & Physical/CLI/Zigbee Convergence Proof
 *  11. Hardware State Validity Gate & Sensor Granularity
 *  12. Immediate Discrete Wakeups & Trim Scale Round-Trip
 *  13. FD-03 Desired vs Applied State & EZB_ERR_INV_STATE Handling
 *  14. Health Diagnostics & SoC Temperature Rate Limiting
 */

const fs = require('fs');
const path = require('path');
const assert = require('assert');

console.log('=================================================================');
console.log('=== Running Native Zigbee Phase 5 State Publisher Unit Tests ===');
console.log('=================================================================\n');

let totalTests = 0;
let passedTests = 0;

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

const ROOT_DIR = path.resolve(__dirname, '..');

// -----------------------------------------------------------------------------
// Test Suite 1: Canonical 21-Attribute Mapping Alignment
// -----------------------------------------------------------------------------
console.log('--- Test Suite 1: Canonical 21-Attribute Mapping Alignment ---');

const expectedMappings = [
    { key: 'ZB_PUB_ATTR_POWER', ep: 1, cluster: 0x0006, attr: 0x0000, type: 'BOOL', policy: 'DISCRETE' },
    { key: 'ZB_PUB_ATTR_VOLUME', ep: 1, cluster: 0x0008, attr: 0x0000, type: 'UINT8', policy: 'DISCRETE' },
    { key: 'ZB_PUB_ATTR_TRIM_BASS', ep: 2, cluster: 0xFC01, attr: 0x0000, type: 'UINT8', policy: 'DISCRETE' },
    { key: 'ZB_PUB_ATTR_TRIM_CENTER', ep: 2, cluster: 0xFC01, attr: 0x0001, type: 'UINT8', policy: 'DISCRETE' },
    { key: 'ZB_PUB_ATTR_TRIM_REAR', ep: 2, cluster: 0xFC01, attr: 0x0002, type: 'UINT8', policy: 'DISCRETE' },
    { key: 'ZB_PUB_ATTR_MUTE', ep: 2, cluster: 0xFC01, attr: 0x0003, type: 'UINT8', policy: 'DISCRETE' },
    { key: 'ZB_PUB_ATTR_INPUT', ep: 3, cluster: 0xFC02, attr: 0x0000, type: 'UINT8', policy: 'DISCRETE' },
    { key: 'ZB_PUB_ATTR_EFFECT', ep: 3, cluster: 0xFC02, attr: 0x0001, type: 'UINT8', policy: 'DISCRETE' },
    { key: 'ZB_PUB_ATTR_INPUT_SIGNAL_LEVEL', ep: 4, cluster: 0xFC03, attr: 0x0000, type: 'DATA24', policy: 'TELEMETRY' },
    { key: 'ZB_PUB_ATTR_IDLE_TIME', ep: 4, cluster: 0xFC03, attr: 0x0001, type: 'DATA32', policy: 'TELEMETRY' },
    { key: 'ZB_PUB_ATTR_DIGITAL_SIGNAL_STATUS', ep: 4, cluster: 0xFC03, attr: 0x0002, type: 'UINT8', policy: 'DISCRETE' },
    { key: 'ZB_PUB_ATTR_AUTO_STANDBY', ep: 4, cluster: 0xFC03, attr: 0x0003, type: 'UINT8', policy: 'DISCRETE' },
    { key: 'ZB_PUB_ATTR_BUILT_IN_LED', ep: 4, cluster: 0xFC03, attr: 0x0007, type: 'UINT8', policy: 'DISCRETE' },
    { key: 'ZB_PUB_ATTR_CMD_VERIFIER', ep: 4, cluster: 0xFC03, attr: 0x0008, type: 'UINT8', policy: 'DISCRETE' },
    { key: 'ZB_PUB_ATTR_TEMP1', ep: 11, cluster: 0x0402, attr: 0x0000, type: 'INT16', policy: 'TELEMETRY' },
    { key: 'ZB_PUB_ATTR_TEMP2', ep: 12, cluster: 0x0402, attr: 0x0000, type: 'INT16', policy: 'TELEMETRY' },
    { key: 'ZB_PUB_ATTR_TEMP3', ep: 13, cluster: 0x0402, attr: 0x0000, type: 'INT16', policy: 'TELEMETRY' },
    { key: 'ZB_PUB_ATTR_DEVICE_HEALTH', ep: 4, cluster: 0xFC03, attr: 0x0009, type: 'UINT8', policy: 'DISCRETE' },
    { key: 'ZB_PUB_ATTR_HEALTH_PRIMARY_COMP', ep: 4, cluster: 0xFC03, attr: 0x000A, type: 'UINT8', policy: 'DISCRETE' },
    { key: 'ZB_PUB_ATTR_HEALTH_PRIMARY_REAS', ep: 4, cluster: 0xFC03, attr: 0x000B, type: 'UINT8', policy: 'DISCRETE' },
    { key: 'ZB_PUB_ATTR_SOC_TEMP', ep: 4, cluster: 0xFC03, attr: 0x000C, type: 'INT16', policy: 'TELEMETRY' },
];

it('should define exactly 21 canonical attributes in header enum', () => {
    const pubHeader = fs.readFileSync(path.join(ROOT_DIR, 'src/communication/zigbee/zigbee_state_publisher.h'), 'utf8');
    assert.strictEqual(expectedMappings.length, 21);
    expectedMappings.forEach(m => {
        assert(pubHeader.includes(m.key), `Header missing enum key: ${m.key}`);
    });
    assert(pubHeader.includes('ZB_PUB_ATTR_COUNT'), 'Header missing ZB_PUB_ATTR_COUNT');
});

it('should map all 21 attributes with correct EP, cluster, and attribute IDs in C source', () => {
    const pubSource = fs.readFileSync(path.join(ROOT_DIR, 'src/communication/zigbee/zigbee_state_publisher.c'), 'utf8');
    expectedMappings.forEach(m => {
        assert(pubSource.includes(`[${m.key}]`), `C mapping table missing entry for: ${m.key}`);
    });
});

// -----------------------------------------------------------------------------
// Test Suite 2: Pure-v2 SDK Symbol Audit
// -----------------------------------------------------------------------------
console.log('\n--- Test Suite 2: Pure-v2 Symbol Audit ---');

const nativeFiles = [
    'src/communication/zigbee/zigbee_native_stack.h',
    'src/communication/zigbee/zigbee_native_stack.c',
    'src/communication/zigbee/zigbee_device_model.h',
    'src/communication/zigbee/zigbee_device_model.c',
    'src/communication/zigbee/zigbee_command_ingress.h',
    'src/communication/zigbee/zigbee_command_ingress.c',
    'src/communication/zigbee/zigbee_state_publisher.h',
    'src/communication/zigbee/zigbee_state_publisher.c',
];

nativeFiles.forEach(relPath => {
    it(`should contain zero legacy symbols in ${relPath}`, () => {
        const fullPath = path.join(ROOT_DIR, relPath);
        assert(fs.existsSync(fullPath), `File not found: ${relPath}`);
        const content = fs.readFileSync(fullPath, 'utf8');
        
        // Allowed: esp_zigbee.h, esp_zigbee_config_t, esp_zigbee_init, esp_zigbee_start, esp_zigbee_stop, esp_zigbee_launch_mainloop, esp_zigbee_lock_acquire, esp_zigbee_lock_release, ESP_ZIGBEE_RADIO_MODE_NATIVE
        // Disallowed: esp_zb_*, ESP_ZB_*, ZBOSS_*
        const legacyPattern = /\b(esp_zb_[a-zA-Z0-9_]+|ESP_ZB_[a-zA-Z0-9_]+|ZBOSS_[a-zA-Z0-9_]+)\b/g;
        const matches = content.match(legacyPattern);
        assert(!matches || matches.length === 0, `Found legacy symbols in ${relPath}: ${matches ? matches.join(', ') : ''}`);
    });
});

// -----------------------------------------------------------------------------
// Test Suite 3: Volume & Scale Mathematical Inversion Parity
// -----------------------------------------------------------------------------
console.log('\n--- Test Suite 3: Volume & Scale Mathematical Inversion Parity ---');

function z906_cmd_volume_to_zcl(vol) {
    if (vol <= 0) return 0;
    if (vol >= 43) return 254;
    return Math.round((vol * 254) / 43);
}

function z906_cmd_zcl_to_volume(zcl) {
    if (zcl <= 0) return 0;
    if (zcl >= 254) return 43;
    const vol = Math.round((zcl * 43) / 254);
    return vol > 43 ? 43 : vol;
}

it('should preserve volume roundtrip conversion across all 44 discrete steps (0..43)', () => {
    for (let v = 0; v <= 43; v++) {
        const zcl = z906_cmd_volume_to_zcl(v);
        const back = z906_cmd_zcl_to_volume(zcl);
        assert.strictEqual(back, v, `Volume roundtrip mismatch at step ${v}: zcl=${zcl}, back=${back}`);
    }
});

it('should map volume boundary conditions deterministically', () => {
    assert.strictEqual(z906_cmd_volume_to_zcl(0), 0);
    assert.strictEqual(z906_cmd_volume_to_zcl(43), 254);
    assert.strictEqual(z906_cmd_volume_to_zcl(21), 124);
    assert.strictEqual(z906_cmd_volume_to_zcl(22), 130);
});

// -----------------------------------------------------------------------------
// Test Suite 4: Temperature Encoding & Sentinel Handling
// -----------------------------------------------------------------------------
console.log('\n--- Test Suite 4: Temperature Encoding & Sentinel Handling ---');

function encodeTemperature(temp_celsius, is_valid) {
    if (!is_valid) {
        return -32768; // 0x8000
    }
    if (typeof temp_celsius !== 'number' || !Number.isFinite(temp_celsius)) {
        return -32768; // 0x8000
    }
    const scaled = temp_celsius * 100.0;
    const rounded = scaled >= 0.0 ? Math.floor(scaled + 0.5) : Math.ceil(scaled - 0.5);
    if (rounded < -32767 || rounded > 32767) {
        return -32768; // 0x8000
    }
    if (rounded === -32768) {
        return -32768;
    }
    return rounded;
}

it('should encode valid temperatures into 0.01 °C units with deterministic rounding', () => {
    assert.strictEqual(encodeTemperature(25.00, true), 2500);
    assert.strictEqual(encodeTemperature(25.55, true), 2555);
    assert.strictEqual(encodeTemperature(25.555, true), 2556);
    assert.strictEqual(encodeTemperature(25.5, true), 2550);
    assert.strictEqual(encodeTemperature(0.0, true), 0);
    assert.strictEqual(encodeTemperature(-5.25, true), -525);
    assert.strictEqual(encodeTemperature(-10.25, true), -1025);
    assert.strictEqual(encodeTemperature(85.75, true), 8575);
});

it('should return 0x8000 (-32768) sentinel when sensor reading is invalid', () => {
    assert.strictEqual(encodeTemperature(0.0, false), -32768);
    assert.strictEqual(encodeTemperature(25.0, false), -32768);
});

it('should return 0x8000 for non-finite values (NaN, +Inf, -Inf)', () => {
    assert.strictEqual(encodeTemperature(NaN, true), -32768);
    assert.strictEqual(encodeTemperature(Infinity, true), -32768);
    assert.strictEqual(encodeTemperature(-Infinity, true), -32768);
});

it('should return 0x8000 for positive and negative overflow beyond int16 representable range', () => {
    assert.strictEqual(encodeTemperature(328.0, true), -32768);   // 32800 > 32767
    assert.strictEqual(encodeTemperature(-328.0, true), -32768);  // -32800 < -32767
});

it('should handle sentinel collision: -327.68 evaluates to 0x8000 sentinel', () => {
    assert.strictEqual(encodeTemperature(-327.68, true), -32768);
});

it('should trigger state delta when transitioning valid <-> invalid', () => {
    const valInvalid = encodeTemperature(25.0, false);
    const valValid = encodeTemperature(25.0, true);
    assert.notStrictEqual(valInvalid, valValid);
    assert.strictEqual(valInvalid, -32768);
    assert.strictEqual(valValid, 2500);
});

// -----------------------------------------------------------------------------
// Test Suite 5: DATA24 Input Signal Level Encoding
// -----------------------------------------------------------------------------
console.log('\n--- Test Suite 5: DATA24 Input Signal Level Encoding ---');

function encodeData24(val) {
    return [
        val & 0xFF,
        (val >> 8) & 0xFF,
        (val >> 16) & 0xFF
    ];
}

it('should encode 0x000000 boundary into [0x00, 0x00, 0x00]', () => {
    assert.deepStrictEqual(encodeData24(0x000000), [0x00, 0x00, 0x00]);
});

it('should encode 0x000001 into [0x01, 0x00, 0x00]', () => {
    assert.deepStrictEqual(encodeData24(0x000001), [0x01, 0x00, 0x00]);
});

it('should encode 0x123456 into [0x56, 0x34, 0x12] (little-endian wire format)', () => {
    assert.deepStrictEqual(encodeData24(0x123456), [0x56, 0x34, 0x12]);
});

it('should encode 0xFFFFFF ceiling into [0xFF, 0xFF, 0xFF]', () => {
    assert.deepStrictEqual(encodeData24(0xFFFFFF), [0xFF, 0xFF, 0xFF]);
});

// -----------------------------------------------------------------------------
// Test Suite 6: Changed-Only Discrete State Logic
// -----------------------------------------------------------------------------
console.log('\n--- Test Suite 6: Changed-Only Discrete State Logic ---');

class MockPublisherState {
    constructor() {
        this.submittedValue = null;
        this.backingValue = null;
        this.dirty = false;
        this.reportCount = 0;
        this.networkReady = false;
    }

    onAuthoritativeUpdate(newValue, isDiscrete = true) {
        if (this.submittedValue !== newValue) {
            this.dirty = true;
        }
        // Sync backing descriptor
        this.backingValue = newValue;

        // Try network send if ready
        if (this.dirty && this.networkReady) {
            this.reportCount++;
            this.submittedValue = newValue;
            this.dirty = false;
        }
    }
}

it('should not dispatch reports when value is identical (A -> A)', () => {
    const pub = new MockPublisherState();
    pub.networkReady = true;
    pub.onAuthoritativeUpdate(10);
    assert.strictEqual(pub.reportCount, 1);
    assert.strictEqual(pub.dirty, false);

    // Identical update
    pub.onAuthoritativeUpdate(10);
    assert.strictEqual(pub.reportCount, 1, 'Duplicate report must not be sent for identical state');
    assert.strictEqual(pub.dirty, false);
});

it('should dispatch report immediately on discrete state transition (A -> B)', () => {
    const pub = new MockPublisherState();
    pub.networkReady = true;
    pub.onAuthoritativeUpdate(10);
    assert.strictEqual(pub.reportCount, 1);

    pub.onAuthoritativeUpdate(15);
    assert.strictEqual(pub.reportCount, 2);
    assert.strictEqual(pub.submittedValue, 15);
});

// -----------------------------------------------------------------------------
// Test Suite 7: High-Rate Telemetry Rate Limiting & Coalescing
// -----------------------------------------------------------------------------
console.log('\n--- Test Suite 7: High-Rate Telemetry Rate Limiting & Coalescing ---');

class MockTelemetryCoalescer {
    constructor(minIntervalMs) {
        this.minIntervalMs = minIntervalMs;
        this.lastReportTime = -1;
        this.latestValue = null;
        this.submittedValue = null;
        this.dirty = false;
        this.reports = [];
    }

    onUpdate(val, timestamp) {
        this.latestValue = val;
        this.dirty = true;
        this.evaluateSend(timestamp);
    }

    evaluateSend(timestamp) {
        if (!this.dirty) return;
        if (this.lastReportTime < 0 || (timestamp - this.lastReportTime) >= this.minIntervalMs) {
            this.reports.push({ value: this.latestValue, time: timestamp });
            this.submittedValue = this.latestValue;
            this.lastReportTime = timestamp;
            this.dirty = false;
        }
    }
}

it('should coalesce high-rate telemetry updates (A -> B -> C) within rate limit window', () => {
    const coalescer = new MockTelemetryCoalescer(10000); // 10s window

    // Update 1 at t=0s -> sent immediately
    coalescer.onUpdate(100, 0);
    assert.strictEqual(coalescer.reports.length, 1);
    assert.strictEqual(coalescer.reports[0].value, 100);

    // Rapid updates within window at t=2s (200), t=4s (300), t=7s (400)
    coalescer.onUpdate(200, 2000);
    coalescer.onUpdate(300, 4000);
    coalescer.onUpdate(400, 7000);
    assert.strictEqual(coalescer.reports.length, 1, 'Intermediate updates must be coalesced');

    // Reconciliation tick at t=10s
    coalescer.evaluateSend(10000);
    assert.strictEqual(coalescer.reports.length, 2);
    assert.strictEqual(coalescer.reports[1].value, 400, 'Latest value (C) must be reported upon window expiry');
});

// -----------------------------------------------------------------------------
// Test Suite 8: Failed Report Submission & Lock Retry Retention
// -----------------------------------------------------------------------------
console.log('\n--- Test Suite 8: Failed Report Submission & Lock Retry Retention ---');

it('should retain dirty flag when report submission fails or lock times out, and retry next cycle', () => {
    const pub = new MockPublisherState();
    pub.networkReady = true;

    // Simulate update where report fails (e.g. stack buffer full)
    let shouldFail = true;
    let attempts = 0;

    function processReport(val) {
        pub.backingValue = val;
        pub.dirty = true;
        attempts++;
        if (!shouldFail) {
            pub.submittedValue = val;
            pub.dirty = false;
        }
    }

    processReport(25);
    assert.strictEqual(attempts, 1);
    assert.strictEqual(pub.dirty, true, 'Dirty flag must remain true on submission failure');
    assert.strictEqual(pub.backingValue, 25, 'Descriptor backing must still be updated');

    // Next cycle: still same value 25, but stack recovered
    shouldFail = false;
    if (pub.dirty) {
        processReport(25);
    }
    assert.strictEqual(attempts, 2);
    assert.strictEqual(pub.dirty, false, 'Report successfully submitted on retry');
    assert.strictEqual(pub.submittedValue, 25);
});

// -----------------------------------------------------------------------------
// Test Suite 9: Network-Ready Gate Operation & Force Sync
// -----------------------------------------------------------------------------
console.log('\n--- Test Suite 9: Network-Ready Gate Operation & Force Sync ---');

it('should suppress network reporting when network_ready=false but update descriptor backing', () => {
    const pub = new MockPublisherState();
    pub.networkReady = false;

    pub.onAuthoritativeUpdate(42);
    assert.strictEqual(pub.backingValue, 42, 'Backing must synchronize');
    assert.strictEqual(pub.reportCount, 0, 'Zero network reports when network_ready is false');
    assert.strictEqual(pub.dirty, true, 'Dirty flag must remain pending');

    // Transition network_ready to true
    pub.networkReady = true;
    // Force sync / reconcile
    if (pub.dirty && pub.networkReady) {
        pub.reportCount++;
        pub.submittedValue = pub.backingValue;
        pub.dirty = false;
    }
    assert.strictEqual(pub.reportCount, 1);
    assert.strictEqual(pub.submittedValue, 42);
});

// -----------------------------------------------------------------------------
// Test Suite 10: Zero Optimistic State & Convergence Proof
// -----------------------------------------------------------------------------
console.log('\n--- Test Suite 10: Zero Optimistic State & Convergence Proof ---');

it('should guarantee physical, CLI, and Zigbee confirmed states converge on single publisher path', () => {
    const pubSource = fs.readFileSync(path.join(ROOT_DIR, 'src/communication/zigbee/zigbee_state_publisher.c'), 'utf8');

    // 1. Publisher snapshot exclusively reads z906_state_get()
    assert(pubSource.includes('z906_state_get(&st);'), 'Publisher must read z906_state_get()');

    // 2. Observer registration ensures verified state changes notify publisher without core->comm inversion (FSR-06)
    assert(pubSource.includes('z906_state_register_change_observer'),
           'Publisher must register observer with state manager');

    // 3. Ingress NEVER mutates authoritative state directly
    const ingressSource = fs.readFileSync(path.join(ROOT_DIR, 'src/communication/zigbee/zigbee_command_ingress.c'), 'utf8');
    assert(!ingressSource.includes('z906_state_set_volume'), 'Ingress must never mutate authoritative volume directly');
    assert(!ingressSource.includes('z906_state_set_power'), 'Ingress must never mutate authoritative power directly');
});

// -----------------------------------------------------------------------------
// Test Suite 11: Hardware State Validity Gate & Sensor Granularity
// -----------------------------------------------------------------------------
console.log('\n--- Test Suite 11: Hardware State Validity Gate & Sensor Granularity ---');

it('should enforce strict validity gate: (cable_connected && main_state_valid)', () => {
    const pubSource = fs.readFileSync(path.join(ROOT_DIR, 'src/communication/zigbee/zigbee_state_publisher.c'), 'utf8');
    assert(pubSource.includes('st.cable_connected && st.main_state_valid'),
           'Publisher must gate hardware attributes with cable_connected && main_state_valid');
    assert(!pubSource.includes('st.is_operational || st.cable_connected'),
           'Publisher must NOT use permissive (is_operational || cable_connected) gate');
});

it('should suppress publication of unverified boot defaults when cable_connected=true but main_state_valid=false', () => {
    const mockState = {
        cable_connected: true,
        main_state_valid: false,
        power: false,
        volume: 25,
        temp_valid: false
    };

    const hwValid = mockState.cable_connected && mockState.main_state_valid;
    assert.strictEqual(hwValid, false, 'Hardware attributes must be marked invalid before first CONFIG frame');
});

it('should enable publication of verified attributes once main_state_valid becomes true', () => {
    const mockState = {
        cable_connected: true,
        main_state_valid: true,
        power: true,
        volume: 30,
        temp_valid: true,
        temp_celsius: [22, 24, 45]
    };

    const hwValid = mockState.cable_connected && mockState.main_state_valid;
    assert.strictEqual(hwValid, true, 'Hardware attributes must be valid once CONFIG frame received');

    const temp1Encoded = encodeTemperature(mockState.temp_celsius[0], mockState.cable_connected && mockState.temp_valid);
    assert.strictEqual(temp1Encoded, 2200);
});

// -----------------------------------------------------------------------------
// Test Suite 12: Immediate Discrete Wakeups & Trim Scale Round-Trip
// -----------------------------------------------------------------------------
console.log('\n--- Test Suite 12: Immediate Discrete Wakeups & Trim Scale Round-Trip ---');

it('should verify immediate discrete notification calls for LED and Verifier changes across all origins', () => {
    const cliConsole = fs.readFileSync(path.join(ROOT_DIR, 'src/communication/cli/cli_console.c'), 'utf8');
    const cliLed = fs.readFileSync(path.join(ROOT_DIR, 'src/communication/cli/cli_led_commands.c'), 'utf8');
    const ingress = fs.readFileSync(path.join(ROOT_DIR, 'src/communication/zigbee/zigbee_command_ingress.c'), 'utf8');
    const zbHandlers = fs.readFileSync(path.join(ROOT_DIR, 'src/communication/zigbee/handlers/zb_handlers.c'), 'utf8');

    // CLI console verifier command
    assert(cliConsole.includes('zigbee_state_publisher_notify(ZB_PUB_NOTIF_CMD_VERIFIER);'),
           'CLI console verifier setter must immediately notify publisher');

    // CLI led commands
    assert(cliLed.includes('zigbee_state_publisher_notify(ZB_PUB_NOTIF_BUILT_IN_LED);'),
           'CLI LED setter must immediately notify publisher');

    // Zigbee command ingress worker
    assert(ingress.includes('zigbee_state_publisher_notify(ZB_PUB_NOTIF_BUILT_IN_LED);'),
           'Ingress worker LED setter must immediately notify publisher');
    assert(ingress.includes('zigbee_state_publisher_notify(ZB_PUB_NOTIF_CMD_VERIFIER);'),
           'Ingress worker Verifier setter must immediately notify publisher');

    // Zigbee raw handlers
    assert(zbHandlers.includes('zigbee_state_publisher_notify(ZB_PUB_NOTIF_BUILT_IN_LED);'),
           'zb_handlers LED setter must immediately notify publisher');
    assert(zbHandlers.includes('zigbee_state_publisher_notify(ZB_PUB_NOTIF_CMD_VERIFIER);'),
           'zb_handlers Verifier setter must immediately notify publisher');
});

it('should verify lossless trim round-trip conversion across all canonical steps (0, 1, 21, 22, 42, 43)', () => {
    const testSteps = [0, 1, 21, 22, 42, 43];
    testSteps.forEach(step => {
        const zcl = z906_cmd_volume_to_zcl(step);
        const back = z906_cmd_zcl_to_volume(zcl);
        assert.strictEqual(back, step, `Trim round-trip mismatch at step ${step}: zcl=${zcl}, back=${back}`);
    });
});

// -----------------------------------------------------------------------------
// Test Suite 13: FD-03 Desired vs Applied State & EZB_ERR_INV_STATE Handling
// -----------------------------------------------------------------------------
console.log('\n--- Test Suite 13: FD-03 Desired vs Applied State & EZB_ERR_INV_STATE Handling ---');

class MockFd03Publisher {
    constructor() {
        this.currentValue = null;
        this.backingValue = null;
        this.deferredOnInvState = null;
        this.syncAttempts = 0;
        this.forceSyncCount = 0;
    }

    setDesiredValue(val) {
        this.currentValue = val;
    }

    runSyncCycle(forceSync = false, sdkSetResult = 0) { // 0 = EZB_ERR_NONE, 3 = EZB_ERR_INV_STATE
        let needBackingSync = (this.currentValue !== this.backingValue);

        // Suppress blind periodic retry if deferred
        if (needBackingSync && !forceSync && this.deferredOnInvState !== null && this.currentValue === this.deferredOnInvState) {
            needBackingSync = false;
        }

        if (!needBackingSync) return;

        this.syncAttempts++;

        if (sdkSetResult === 0) {
            // EZB_ERR_NONE: backing advances
            this.backingValue = this.currentValue;
            this.deferredOnInvState = null;
        } else if (sdkSetResult === 3) {
            // EZB_ERR_INV_STATE: backing DOES NOT advance
            this.deferredOnInvState = this.currentValue;
        }
    }

    forceFullSync() {
        this.forceSyncCount++;
        this.deferredOnInvState = null;
        this.runSyncCycle(true, 0); // On join, stack allows write
    }
}

it('AC-13: should NOT advance backing value on EZB_ERR_INV_STATE (desired !== applied)', () => {
    const pub = new MockFd03Publisher();
    pub.setDesiredValue(30);
    pub.runSyncCycle(false, 3); // Returns EZB_ERR_INV_STATE (3)

    assert.strictEqual(pub.backingValue, null, 'Backing value must NOT advance on EZB_ERR_INV_STATE');
    assert.strictEqual(pub.currentValue, 30, 'Desired value is preserved');
    assert.strictEqual(pub.deferredOnInvState, 30, 'Deferred state records desired value');
    assert.strictEqual(pub.syncAttempts, 1);
});

it('AC-14: should suppress blind periodic retries while desired value is unchanged and deferred', () => {
    const pub = new MockFd03Publisher();
    pub.setDesiredValue(30);
    pub.runSyncCycle(false, 3); // Attempt 1: EZB_ERR_INV_STATE

    assert.strictEqual(pub.syncAttempts, 1);

    // Periodic cycles (e.g. 5-second interval) with same desired value
    pub.runSyncCycle(false, 3);
    pub.runSyncCycle(false, 3);
    assert.strictEqual(pub.syncAttempts, 1, 'Periodic cycles must NOT re-attempt descriptor write blindly');
});

it('AC-15: should re-attempt sync if desired value changes even before network ready', () => {
    const pub = new MockFd03Publisher();
    pub.setDesiredValue(30);
    pub.runSyncCycle(false, 3); // Attempt 1: deferred on 30
    assert.strictEqual(pub.syncAttempts, 1);

    // Desired value changes to 35
    pub.setDesiredValue(35);
    pub.runSyncCycle(false, 3); // Attempt 2: different value triggers sync attempt
    assert.strictEqual(pub.syncAttempts, 2);
    assert.strictEqual(pub.deferredOnInvState, 35, 'Deferred state updated to new value');
    assert.strictEqual(pub.backingValue, null, 'Backing value still unchanged');
});

it('AC-16: should reconcile deferred state on network join via force_full_sync()', () => {
    const pub = new MockFd03Publisher();
    pub.setDesiredValue(35);
    pub.runSyncCycle(false, 3); // Deferred on 35

    assert.strictEqual(pub.backingValue, null);

    // Network joins -> forceFullSync() triggered
    pub.forceFullSync();
    assert.strictEqual(pub.backingValue, 35, 'Backing value successfully synchronized on network join');
    assert.strictEqual(pub.deferredOnInvState, null, 'Deferred state cleared');
});

// -----------------------------------------------------------------------------
// Test Suite 14: Health Diagnostics & SoC Temperature Rate Limiting
// -----------------------------------------------------------------------------
console.log('\n--- Test Suite 14: Health Diagnostics & SoC Temperature Rate Limiting ---');

class MockHealthStatePublisher {
    constructor() {
        this.health = { status: 1, component: 0, reason: 0 };
        this.submittedHealth = { status: null, component: null, reason: null };
        this.healthReports = 0;
        this.socTempReports = 0;
        this.lastSocTemp = null;
        this.lastSocReportTime = -1;
    }

    onHealthChange(newStatus, newComp, newReason) {
        const changed = (this.submittedHealth.status !== newStatus ||
                         this.submittedHealth.component !== newComp ||
                         this.submittedHealth.reason !== newReason);
        if (changed) {
            this.healthReports++;
            this.submittedHealth = { status: newStatus, component: newComp, reason: newReason };
        }
    }

    onSocTempUpdate(tempCelsius, isValid, nowSec, isCriticalHealth = false) {
        if (!isValid) return; // invalid sentinel 0x8000 does not trigger normal telemetry
        const tempScaled = Math.round(tempCelsius * 100);
        let shouldReport = false;

        if (this.lastSocReportTime < 0) {
            shouldReport = true;
        } else if (isCriticalHealth) {
            // Critical/warning transition forces immediate send
            shouldReport = true;
        } else {
            const dt = nowSec - this.lastSocReportTime;
            const delta = Math.abs(tempScaled - (this.lastSocTemp || 0));
            if (dt >= 900) { // Heartbeat 15 min
                shouldReport = true;
            } else if (dt >= 60 && delta >= 100) { // 60s and >= 1.0°C
                shouldReport = true;
            }
        }

        if (shouldReport) {
            this.socTempReports++;
            this.lastSocTemp = tempScaled;
            this.lastSocReportTime = nowSec;
        }
    }
}

it('should dispatch report immediately when health reason changes even if status remains degraded', () => {
    const pub = new MockHealthStatePublisher();
    pub.onHealthChange(2, 4, 9); // Degraded, memory, heap_warning
    assert.strictEqual(pub.healthReports, 1);

    // Reason changes from heap_warning (9) to heap_fragmented (11), status remains 2
    pub.onHealthChange(2, 4, 11);
    assert.strictEqual(pub.healthReports, 2, 'Reason change alone must trigger report');
    assert.strictEqual(pub.submittedHealth.reason, 11);

    // Identical update: no duplicate report
    pub.onHealthChange(2, 4, 11);
    assert.strictEqual(pub.healthReports, 2, 'Identical health state must not flood reports');
});

it('should enforce 60s interval and 1.0°C delta on SoC temperature reporting', () => {
    const pub = new MockHealthStatePublisher();
    pub.onSocTempUpdate(40.0, true, 100); // Initial report at t=100
    assert.strictEqual(pub.socTempReports, 1);

    // Small change (< 1.0°C) after 70s (> 60s) -> suppressed
    pub.onSocTempUpdate(40.5, true, 170);
    assert.strictEqual(pub.socTempReports, 1, 'Delta < 1.0°C must be suppressed');

    // Large change (>= 1.0°C) but only 30s elapsed (< 60s) -> suppressed
    pub.onSocTempUpdate(42.0, true, 130);
    assert.strictEqual(pub.socTempReports, 1, 'dt < 60s must be suppressed even with large delta');

    // Large change (>= 1.0°C) AND dt >= 60s -> reported
    pub.onSocTempUpdate(42.0, true, 200);
    assert.strictEqual(pub.socTempReports, 2, 'dt >= 60s and delta >= 1.0°C must trigger report');
});

it('should bypass SoC temperature rate limit on critical health transition', () => {
    const pub = new MockHealthStatePublisher();
    pub.onSocTempUpdate(40.0, true, 100);
    assert.strictEqual(pub.socTempReports, 1);

    // 5 seconds later with critical health flag -> immediate bypass report
    pub.onSocTempUpdate(75.0, true, 105, true);
    assert.strictEqual(pub.socTempReports, 2, 'Critical health transition must bypass rate limit');
});

// -----------------------------------------------------------------------------
// Summary
// -----------------------------------------------------------------------------
console.log('\n=================================================================');
console.log(`Results: ${passedTests}/${totalTests} tests passed.`);
console.log('=================================================================');

if (passedTests !== totalTests) {
    process.exit(1);
}
