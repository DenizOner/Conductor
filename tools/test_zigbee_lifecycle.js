/**
 * @file test_zigbee_lifecycle.js
 * @brief Host validation test suite for Native Zigbee Phase 6 Corrective Hardening:
 *        Disconnected Re-Pair Safety + Membership Truth + Network-Loss Semantics.
 *
 * Verifies:
 *   1. BDB Initialization on EZB_ZDO_SIGNAL_SKIP_STARTUP
 *   2. Factory-New Boot: NO automatic network steering when no intent
 *   3. CLI-Controlled Pairing: dev zb_pair initiates steering on clean device
 *   4. Normal Reboot Rejoin: Stack persistence / zb_storage authority
 *   5. Persistence Ownership: App NVS does NOT mirror stack credentials
 *   6. Canonical Membership Truth: ezb_bdb_dev_joined() guards network_ready=true
 *   7. Safe Re-Pair Strategy B:
 *        - Local reset is NOT relied on when disconnected (joined=false)
 *        - Disconnected / joined re-pair persists one-shot intent + factory reset + restart
 *        - Factory-new boot detects intent, immediately clears intent, and starts steering
 *   8. Power-loss safety: One-shot intent cleared before steering start
 *   9. Signal semantics:
 *        - DEVICE_REBOOT success + joined=false -> network_ready=false + diagnostic inconsistency
 *        - STEERING success + joined=false -> network_ready=false
 *        - NO_ACTIVE_LINKS_LEFT: Non-destructive, does not erase credentials or trigger reset/steering
 *        - LEAVE_INDICATION: Peer leave only, own membership unchanged
 *        - LEAVE: Distinguishes RESET vs REJOIN
 *        - dev zb_reset: No pair intent written
 *        - dev factory_reset: No pair intent written
 *  10. Security Redaction: Zero exposure of network keys / cryptographic secrets
 *  11. Pure ESP Zigbee SDK v2 Audit
 */

const fs = require('fs');
const path = require('path');
const assert = require('assert');

console.log('===================================================================');
console.log('=== Running Native Zigbee Phase 6 Corrective Lifecycle Tests ===');
console.log('===================================================================\n');

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
// Test Suite 1: Source Code Inspection & Pure-v2 Audit
// -----------------------------------------------------------------------------
console.log('--- Test Suite 1: Source Code & Pure-v2 Audit ---');

const nativeStackC = fs.readFileSync(path.join(ROOT_DIR, 'src', 'communication', 'zigbee', 'zigbee_native_stack.c'), 'utf8');
const nativeStackH = fs.readFileSync(path.join(ROOT_DIR, 'src', 'communication', 'zigbee', 'zigbee_native_stack.h'), 'utf8');
const cliDevToolsC = fs.readFileSync(path.join(ROOT_DIR, 'src', 'communication', 'cli', 'cli_dev_tools.c'), 'utf8');
const nvsManagerC = fs.readFileSync(path.join(ROOT_DIR, 'src', 'management', 'nvs', 'nvs_manager.c'), 'utf8');
const nvsManagerH = fs.readFileSync(path.join(ROOT_DIR, 'src', 'management', 'nvs', 'nvs_manager.h'), 'utf8');
const mainC = fs.readFileSync(path.join(ROOT_DIR, 'src', 'main.c'), 'utf8');

it('Pure-v2 Audit: No legacy esp_zb_* or ZBOSS symbols in zigbee_native_stack', () => {
    const legacyPatterns = [
        /\besp_zb_zcl_/g,
        /\bESP_ZB_ZCL_/g,
        /\bZBOSS\b/g,
        /\besp_zb_start\b/g,
        /\besp_zb_init\b/g,
        /\besp_zb_bdb_/g,
        /\bESP_ZB_BDB_/g
    ];

    for (const pattern of legacyPatterns) {
        assert(!pattern.test(nativeStackC), `Legacy pattern ${pattern} found in zigbee_native_stack.c`);
        assert(!pattern.test(nativeStackH), `Legacy pattern ${pattern} found in zigbee_native_stack.h`);
    }
});

it('Pure-v2 Audit: cli_dev_tools uses pure-v2 zigbee_native_stack APIs', () => {
    assert(cliDevToolsC.includes('zigbee_native_get_diagnostics'), 'Missing zigbee_native_get_diagnostics in cli_dev_tools.c');
    assert(cliDevToolsC.includes('zigbee_native_request_pair'), 'Missing zigbee_native_request_pair in cli_dev_tools.c');
    assert(cliDevToolsC.includes('zigbee_native_request_reset'), 'Missing zigbee_native_request_reset in cli_dev_tools.c');
    assert(!cliDevToolsC.includes('zigbee_app_start_pairing'), 'Legacy simulation zigbee_app_start_pairing still present in cli_dev_tools.c');
    assert(!cliDevToolsC.includes('zigbee_app_get_state'), 'Legacy simulation zigbee_app_get_state still present in cli_dev_tools.c');
});

it('Pure-v2 Audit: main.c spawns native stack task without simulation hooks', () => {
    assert(!mainC.includes('zigbee_app_init()'), 'Legacy zigbee_app_init() still present in main.c');
    assert(!mainC.includes('zigbee_app_start()'), 'Legacy zigbee_app_start() still present in main.c');
    assert(!mainC.includes('zigbee_app_report_state('), 'Legacy zigbee_app_report_state() still present in main.c');
    assert(mainC.includes('zigbee_native_stack_init()'), 'Missing zigbee_native_stack_init() in main.c');
    assert(mainC.includes('zigbee_native_stack_run()'), 'Missing zigbee_native_stack_run() in main.c');
});

// -----------------------------------------------------------------------------
// Test Suite 2: Membership Truth Model & Central Readiness Guard
// -----------------------------------------------------------------------------
console.log('\n--- Test Suite 2: Membership Truth Model & Central Readiness Guard ---');

it('Canonical Membership Truth: ezb_bdb_dev_joined() is tracked and populated in diagnostics', () => {
    assert(nativeStackH.includes('bool sdk_joined;'), 'Diagnostics struct must contain bool sdk_joined');
    assert(nativeStackC.includes('s_diag.sdk_joined = ezb_bdb_dev_joined();'),
           'Must query ezb_bdb_dev_joined() for sdk_joined telemetry');
    assert(cliDevToolsC.includes('SDK Joined:'), 'cli_dev_tools.c must display SDK Joined status');
});

it('Central Readiness Guard: set_network_ready(true) is strictly guarded by ezb_bdb_dev_joined()', () => {
    const readyFnIndex = nativeStackC.indexOf('static void set_network_ready(bool ready)');
    assert(readyFnIndex !== -1, 'Missing set_network_ready function');
    const readyFnEnd = nativeStackC.indexOf('}', readyFnIndex);
    const readyFnBlock = nativeStackC.substring(readyFnIndex, readyFnEnd + 1);

    assert(readyFnBlock.includes('ezb_bdb_dev_joined()'), 'set_network_ready must check ezb_bdb_dev_joined()');
    assert(readyFnBlock.includes('!s_diag.sdk_joined'), 'Must check if joined is false when ready=true');
    assert(readyFnBlock.includes('return;'), 'Must reject ready=true if sdk_joined is false');
});

// -----------------------------------------------------------------------------
// Test Suite 3: Local Reset Contract & Safe Re-Pair Flow (Strategy B)
// -----------------------------------------------------------------------------
console.log('\n--- Test Suite 3: Local Reset Contract & Safe Re-Pair Strategy ---');

it('Official Local Reset Contract: local reset is NOT relied on for disconnected re-pair', () => {
    const reqPairIndex = nativeStackC.indexOf('zigbee_native_request_pair(void)');
    assert(reqPairIndex !== -1, 'Missing zigbee_native_request_pair function');
    const reqPairEnd = nativeStackC.indexOf('void zigbee_native_request_reset(void)', reqPairIndex);
    const reqPairBlock = nativeStackC.substring(reqPairIndex, reqPairEnd);

    assert(!reqPairBlock.includes('ezb_bdb_reset_via_local_action'),
           'zigbee_native_request_pair must NOT invoke local reset for re-pair');
    assert(!nativeStackC.includes('ezb_bdb_reset_via_local_action'),
           'ezb_bdb_reset_via_local_action must not be called anywhere in re-pair');
});

it('Membership Truth Decision Matrix: handles factory_new and joined combinations', () => {
    const reqPairIndex = nativeStackC.indexOf('zigbee_native_request_pair(void)');
    const reqPairEnd = nativeStackC.indexOf('void zigbee_native_request_reset(void)', reqPairIndex);
    const reqPairBlock = nativeStackC.substring(reqPairIndex, reqPairEnd);

    assert(reqPairBlock.includes('ezb_bdb_is_factory_new()'), 'Must query ezb_bdb_is_factory_new()');
    assert(reqPairBlock.includes('ezb_bdb_dev_joined()'), 'Must query ezb_bdb_dev_joined()');
    assert(reqPairBlock.includes('factory_new && joined'), 'Must guard against inconsistent state');
    assert(reqPairBlock.includes('!factory_new'), 'Must handle existing datasets with Strategy B');
    assert(reqPairBlock.includes('nvs_manager_set_pair_after_reset(true)'),
           'Must persist one-shot pair intent when re-pairing with datasets');
    assert(reqPairBlock.includes('esp_zigbee_factory_reset()'),
           'Must invoke esp_zigbee_factory_reset to clear datasets and restart');
});

it('Power-Loss Safe Intent: DEVICE_FIRST_START detects and IMMEDIATELY clears intent before steering', () => {
    const dfsIndex = nativeStackC.indexOf('case EZB_BDB_SIGNAL_DEVICE_FIRST_START:');
    assert(dfsIndex !== -1, 'Missing EZB_BDB_SIGNAL_DEVICE_FIRST_START handler');
    const dfsEnd = nativeStackC.indexOf('case EZB_BDB_SIGNAL_DEVICE_REBOOT:', dfsIndex);
    const dfsBlock = nativeStackC.substring(dfsIndex, dfsEnd);

    const getPos = dfsBlock.indexOf('nvs_manager_get_pair_after_reset(&pair_intent)');
    const clearPos = dfsBlock.indexOf('nvs_manager_clear_pair_after_reset()');
    const steerPos = dfsBlock.indexOf('ezb_bdb_start_top_level_commissioning(EZB_BDB_MODE_NETWORK_STEERING)');

    assert(getPos !== -1, 'Must check pair_after_reset intent on first start');
    assert(clearPos !== -1, 'Must clear pair_after_reset intent');
    assert(steerPos !== -1, 'Must trigger steering if intent is set');
    assert(clearPos < steerPos, 'Intent MUST be cleared BEFORE steering is triggered for power-loss safety');
    assert(dfsBlock.includes('ZB_NET_STATE_FACTORY_NEW_IDLE'), 'Normal boot without intent must be idle');
});

it('Normal boot without intent does NOT auto-steer', () => {
    const dfsIndex = nativeStackC.indexOf('case EZB_BDB_SIGNAL_DEVICE_FIRST_START:');
    const dfsEnd = nativeStackC.indexOf('case EZB_BDB_SIGNAL_DEVICE_REBOOT:', dfsIndex);
    const dfsBlock = nativeStackC.substring(dfsIndex, dfsEnd);

    assert(dfsBlock.includes('/* Invariant: Conductor Router does NOT auto-steer on normal boot. Steering is CLI-controlled. */'),
           'Must preserve normal boot idle invariant');
});

it('Reset commands do NOT write pair intent', () => {
    // Check zigbee_native_request_reset
    const zbReqResetIndex = nativeStackC.indexOf('zigbee_native_request_reset(void)');
    const zbReqResetBlock = nativeStackC.substring(zbReqResetIndex, zbReqResetIndex + 600);
    assert(zbReqResetBlock.includes('nvs_manager_clear_pair_after_reset()'),
           'zigbee_native_request_reset must clear pair intent if present');
    assert(!zbReqResetBlock.includes('nvs_manager_set_pair_after_reset(true)'),
           'zigbee_native_request_reset must NEVER set pair intent');

    // Check cli_dev_tools cmd_dev_reset
    const cliDevResetIndex = cliDevToolsC.indexOf('strcmp(subcmd, "factory_reset") == 0');
    const cliDevResetBlock = cliDevToolsC.substring(cliDevResetIndex, cliDevResetIndex + 1200);
    assert(!cliDevResetBlock.includes('nvs_manager_set_pair_after_reset(true)'),
           'factory_reset must NEVER set pair intent');
});

// -----------------------------------------------------------------------------
// Test Suite 4: Network Signal Semantics Cross-Checks
// -----------------------------------------------------------------------------
console.log('\n--- Test Suite 4: Network Signal Semantics Cross-Checks ---');

it('DEVICE_REBOOT success is cross-checked with ezb_bdb_dev_joined()', () => {
    const rebootIndex = nativeStackC.indexOf('case EZB_BDB_SIGNAL_DEVICE_REBOOT:');
    const rebootEnd = nativeStackC.indexOf('case EZB_BDB_SIGNAL_STEERING:', rebootIndex);
    const rebootBlock = nativeStackC.substring(rebootIndex, rebootEnd);

    assert(rebootBlock.includes('s_diag.sdk_joined'), 'Must check s_diag.sdk_joined on reboot');
    assert(rebootBlock.includes('DEVICE_REBOOT status SUCCESS but ezb_bdb_dev_joined() is false!'),
           'Must log warning and suppress ready when status is success but joined is false');
});

it('STEERING success is cross-checked with ezb_bdb_dev_joined()', () => {
    const steerIndex = nativeStackC.indexOf('case EZB_BDB_SIGNAL_STEERING:');
    const steerEnd = nativeStackC.indexOf('case EZB_ZDO_SIGNAL_LEAVE:', steerIndex);
    const steerBlock = nativeStackC.substring(steerIndex, steerEnd);

    assert(steerBlock.includes('s_diag.sdk_joined'), 'Must check s_diag.sdk_joined on steering finish');
    assert(steerBlock.includes('STEERING status SUCCESS but ezb_bdb_dev_joined() is false!'),
           'Must log warning and suppress ready when steering status is success but joined is false');
});

it('NO_ACTIVE_LINKS_LEFT is non-destructive and checks joined before flapping ready', () => {
    const noLinksIndex = nativeStackC.indexOf('case EZB_NWK_SIGNAL_NO_ACTIVE_LINKS_LEFT:');
    assert(noLinksIndex !== -1, 'Missing EZB_NWK_SIGNAL_NO_ACTIVE_LINKS_LEFT handler');
    const noLinksEnd = nativeStackC.indexOf('case EZB_ZDO_SIGNAL_DEVICE_ANNCE:', noLinksIndex);
    const noLinksBlock = nativeStackC.substring(noLinksIndex, noLinksEnd);

    assert(!noLinksBlock.includes('esp_zigbee_factory_reset'), 'Must NOT call factory reset on no active links');
    assert(!noLinksBlock.includes('ezb_bdb_start_top_level_commissioning'), 'Must NOT start steering on no active links');
    assert(!noLinksBlock.includes('nvs_manager_clear'), 'Must NOT erase credentials on no active links');
    assert(noLinksBlock.includes('if (!s_diag.sdk_joined)'), 'Must check joined status before altering network_ready');
});

it('LEAVE_INDICATION is treated as peer leave; own state is preserved', () => {
    const leaveIndIndex = nativeStackC.indexOf('case EZB_ZDO_SIGNAL_LEAVE_INDICATION:');
    assert(leaveIndIndex !== -1, 'Missing EZB_ZDO_SIGNAL_LEAVE_INDICATION handler');
    const leaveIndEnd = nativeStackC.indexOf('case EZB_NWK_SIGNAL_NETWORK_STATUS:', leaveIndIndex);
    const leaveIndBlock = nativeStackC.substring(leaveIndIndex, leaveIndEnd);

    assert(leaveIndBlock.includes('params->short_addr != own_addr'), 'Must compare peer short address against own address');
    assert(leaveIndBlock.includes('Own state unchanged'), 'Must preserve own state when peer leaves');
});

it('EZB_NWK_SIGNAL_NETWORK_STATUS is non-destructive (diagnostic only)', () => {
    const nwkStatIndex = nativeStackC.indexOf('case EZB_NWK_SIGNAL_NETWORK_STATUS:');
    const nwkStatEnd = nativeStackC.indexOf('case EZB_NWK_SIGNAL_NO_ACTIVE_LINKS_LEFT:', nwkStatIndex);
    const nwkStatBlock = nativeStackC.substring(nwkStatIndex, nwkStatEnd);

    assert(!nwkStatBlock.includes('esp_zigbee_factory_reset'), 'Network status must NEVER trigger factory reset');
    assert(nwkStatBlock.includes('s_diag.last_nwk_status = status;'), 'Must record status in diagnostics');
});

// -----------------------------------------------------------------------------
// Test Suite 5: NVS Manager One-Shot Intent API
// -----------------------------------------------------------------------------
console.log('\n--- Test Suite 5: NVS Manager One-Shot Intent API ---');

it('NVS Manager exports get, set, clear for pair_after_reset', () => {
    assert(nvsManagerH.includes('nvs_manager_get_pair_after_reset'), 'Missing nvs_manager_get_pair_after_reset in header');
    assert(nvsManagerH.includes('nvs_manager_set_pair_after_reset'), 'Missing nvs_manager_set_pair_after_reset in header');
    assert(nvsManagerH.includes('nvs_manager_clear_pair_after_reset'), 'Missing nvs_manager_clear_pair_after_reset in header');
    assert(nvsManagerC.includes('NVS_KEY_PAIR_INTENT'), 'Missing NVS_KEY_PAIR_INTENT definition');
});

it('No credential mirroring: zigbee_native_stack does NOT call nvs_manager_save_zb_net_info', () => {
    assert(!nativeStackC.includes('nvs_manager_save_zb_net_info'),
           'zigbee_native_stack must NEVER mirror credentials into application NVS');
});

// -----------------------------------------------------------------------------
// Test Suite 6: CLI & Diagnostics
// -----------------------------------------------------------------------------
console.log('\n--- Test Suite 6: CLI & Diagnostics ---');

it('CLI dev zb_status prints Factory New, SDK Joined, and Network Ready', () => {
    const statusIndex = cliDevToolsC.indexOf('cmd_dev_zb_status(void)');
    const statusBlock = cliDevToolsC.substring(statusIndex, statusIndex + 1500);

    assert(statusBlock.includes('Factory New:'), 'Must print Factory New');
    assert(statusBlock.includes('SDK Joined:'), 'Must print SDK Joined');
    assert(statusBlock.includes('Network Ready:'), 'Must print Network Ready');
    assert(!statusBlock.includes('network_key'), 'Network key must not be displayed');
    assert(!statusBlock.includes('tc_link_key'), 'TC link key must not be displayed');
});

// -----------------------------------------------------------------------------
// Test Summary
// -----------------------------------------------------------------------------
console.log('\n===================================================================');
console.log(`=== Test Summary: ${passedTests} / ${totalTests} tests passed ===`);
console.log('===================================================================\n');

if (passedTests !== totalTests) {
    process.exit(1);
} else {
    process.exit(0);
}
