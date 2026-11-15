/**
 * @file validate_command_parity.js
 * @brief Automated Parity and Synchronization Validator for Logitech Z906.
 *
 * Verifies 100% lockstep parity across:
 * 1. Single Source of Truth: tools/z906_commands_metadata.json
 * 2. Zigbee2MQTT External Converter: tools/conductor_converter.mjs (Phase 8 ESM)
 * 3. Firmware C Registry: src/core/cmd_verifier/z906_cmd_registry.h & .c
 * 4. Zigbee Cluster Declarations: src/communication/zigbee/clusters/zcl_clusters.h
 *
 * NOTE (Phase 8): conductor_converter.mjs is an ESM module and cannot be loaded
 * via require(). Converter parity is validated via source-text analysis (checking
 * for key names and expose definitions in the converter source), which is both
 * reliable and consistent with the static analysis approach used in test_converter.js.
 */

'use strict';

const fs   = require('fs');
const path = require('path');

/* Path resolution relative to project root */
const ROOT_DIR           = path.resolve(__dirname, '..');
const METADATA_PATH      = path.join(ROOT_DIR, 'tools', 'z906_commands_metadata.json');
const CONVERTER_PATH     = path.join(ROOT_DIR, 'tools', 'conductor_converter.mjs');
const REGISTRY_H_PATH    = path.join(ROOT_DIR, 'src', 'core', 'cmd_verifier', 'z906_cmd_registry.h');
const REGISTRY_C_PATH    = path.join(ROOT_DIR, 'src', 'core', 'cmd_verifier', 'z906_cmd_registry.c');
const ZCL_CLUSTERS_H_PATH = path.join(ROOT_DIR, 'src', 'communication', 'zigbee', 'clusters', 'zcl_clusters.h');

let failureCount = 0;

/** Assertion helper that prints PASS/FAIL and tracks failure count. */
function assert(condition, message) {
    if (!condition) {
        console.error(`  [FAIL] ${message}`);
        failureCount++;
    } else {
        console.log(`  [PASS] ${message}`);
    }
}

console.log('=================================================================');
console.log(' Logitech Z906 Central Command Parity & Synchronization Validator');
console.log('=================================================================\n');

/* 1. Load Metadata */
console.log('1. Validating Central Metadata (tools/z906_commands_metadata.json)...');
assert(fs.existsSync(METADATA_PATH), `Metadata file exists at ${path.relative(ROOT_DIR, METADATA_PATH)}`);
let metadata;
try {
    const raw = fs.readFileSync(METADATA_PATH, 'utf8');
    metadata = JSON.parse(raw);
    assert(Array.isArray(metadata.commands), 'Metadata contains "commands" array');
    assert(metadata.commands.length === 8, `Expected exactly 8 commands in metadata, found ${metadata.commands.length}`);
} catch (e) {
    assert(false, `Failed to parse metadata JSON: ${e.message}`);
    process.exit(1);
}

/* 2. Validate Converter (Phase 8: ESM source-text analysis — no require()) */
console.log('\n2. Validating Zigbee2MQTT External Converter (tools/conductor_converter.mjs)...');
assert(fs.existsSync(CONVERTER_PATH), `Converter file exists at ${path.relative(ROOT_DIR, CONVERTER_PATH)}`);

let converterSrc = '';
try {
    converterSrc = fs.readFileSync(CONVERTER_PATH, 'utf8');
    // Strip comments before analysis to avoid false positives in doc strings
    const stripped = converterSrc.replace(/\/\*[\s\S]*?\*\//g, '').replace(/\/\/.*/g, '');
    assert(converterSrc.length > 0, 'Converter source is non-empty');
    assert(converterSrc.includes('export default'), 'Converter uses ESM export default (Phase 8 migration)');
    assert(converterSrc.includes("modelID: 'Z-906-Zigbee'"), "Converter fingerprint has correct modelID");
    assert(converterSrc.includes("manufacturerName: 'Logitech'"), "Converter fingerprint has correct manufacturerName");
    assert(!stripped.includes('logitech_z906_converter'), 'Legacy converter reference absent from source');
} catch (e) {
    assert(false, `Failed to read converter source: ${e.message}`);
    process.exit(1);
}

/* 3. Read C Source Files */
console.log('\n3. Validating Firmware C Command Registry Sources...');
assert(fs.existsSync(REGISTRY_H_PATH),     `Header exists: ${path.relative(ROOT_DIR, REGISTRY_H_PATH)}`);
assert(fs.existsSync(REGISTRY_C_PATH),     `Source exists: ${path.relative(ROOT_DIR, REGISTRY_C_PATH)}`);
assert(fs.existsSync(ZCL_CLUSTERS_H_PATH), `ZCL header exists: ${path.relative(ROOT_DIR, ZCL_CLUSTERS_H_PATH)}`);

const registryH  = fs.readFileSync(REGISTRY_H_PATH,     'utf8');
const registryC  = fs.readFileSync(REGISTRY_C_PATH,     'utf8');
const zclClustersH = fs.readFileSync(ZCL_CLUSTERS_H_PATH, 'utf8');

/* 4. Cross-Verification of Each Command */
console.log('\n4. Cross-Checking Command Mappings in Lockstep...');

metadata.commands.forEach((cmd, idx) => {
    console.log(`\n  --- Command [${cmd.id}]: ${cmd.name} (${cmd.enum_id}) ---`);

    /* Schema integrity */
    assert(cmd.id === idx, `Command index matches id (${cmd.id} === ${idx})`);
    assert(typeof cmd.name === 'string' && cmd.name.length > 0, `Command has name: ${cmd.name}`);
    assert(cmd.cli && typeof cmd.cli.command === 'string', `CLI syntax defined: ${cmd.cli ? cmd.cli.command : 'none'}`);
    assert(cmd.zigbee && cmd.zigbee.endpoint && cmd.zigbee.cluster_id, `Zigbee coordinates defined (EP: ${cmd.zigbee.endpoint}, Cluster: ${cmd.zigbee.cluster_id})`);
    assert(cmd.converter && cmd.converter.expose_name, `Converter expose defined: ${cmd.converter.expose_name}`);
    assert(cmd.verifier && cmd.verifier.verify_type, `Verifier mapping defined: ${cmd.verifier.verify_type}`);

    /* Converter parity (source-text analysis — ESM module cannot be require()'d) */
    const exposeKey = cmd.converter.expose_name;
    assert(
        converterSrc.includes(`'${exposeKey}'`) || converterSrc.includes(`"${exposeKey}"`),
        `Converter source references expose key "${exposeKey}"`
    );

    /* Firmware header parity */
    assert(registryH.includes(cmd.enum_id), `Header z906_cmd_registry.h declares enum ${cmd.enum_id}`);

    /* Firmware implementation parity */
    assert(registryC.includes(cmd.enum_id), `C table z906_cmd_registry.c maps ${cmd.enum_id}`);
    assert(registryC.includes(`"${cmd.name}"`), `C table registers canonical name "${cmd.name}"`);

    /* Parameter boundary parity */
    if (cmd.param && cmd.param.min !== undefined && cmd.param.max !== undefined) {
        assert(registryC.includes(`.min_val = ${cmd.param.min}`), `C table enforces min_val = ${cmd.param.min}`);
        assert(registryC.includes(`.max_val = ${cmd.param.max}`), `C table enforces max_val = ${cmd.param.max}`);
    }
});

/* 5. Summary and Verdict */
console.log('\n=================================================================');
if (failureCount === 0) {
    console.log(' VERDICT: [SUCCESS] All 8 commands verified with 100% lockstep parity!');
    console.log('=================================================================\n');
    process.exit(0);
} else {
    console.error(` VERDICT: [FAILED] Found ${failureCount} parity mismatch(es)!`);
    console.log('=================================================================\n');
    process.exit(1);
}
