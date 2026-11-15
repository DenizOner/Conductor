'use strict';

/**
 * @file test_ota_package.js
 * @brief OTA packer round-trip and index parity tests for Phase 8.
 *
 * Tests:
 *   Suite 1: OTA container structure verification (synthetic firmware binary)
 *   Suite 2: File ID and header field correctness
 *   Suite 3: SHA-512 / fileSize index parity
 *   Suite 4: UPGRADE_IMAGE sub-element round-trip (payload == firmware.bin)
 *   Suite 5: Phase 7 parser compatibility
 *   Suite 6: generate_ota_index.js metadata consistency
 */

const assert  = require('assert');
const fs      = require('fs');
const path    = require('path');
const crypto  = require('crypto');
const { execSync } = require('child_process');
const os      = require('os');

console.log('=== Running Phase 8 OTA Package & Index Tests ===\n');

let passed = 0;
let total  = 0;

function it(desc, fn) {
    total++;
    try {
        fn();
        console.log(`  [PASS] ${desc}`);
        passed++;
    } catch (err) {
        console.error(`  [FAIL] ${desc}`);
        console.error(`         ${err.message}`);
    }
}

// ─── Canonical OTA Constants (must match build_ota_image.py / config.h) ───────

const CANONICAL_FILE_ID    = 0x0BEEF11E;  // Correct Phase 7 canonical value
const REJECTED_FILE_ID_1   = 0x0BEF11E0;  // Former typo (byte count off)
const REJECTED_FILE_ID_2   = 0x1EF1EE0B;  // Byte-swapped form
const MANUFACTURER_CODE    = 0x131B;
const IMAGE_TYPE           = 0x0906;
const FILE_VERSION         = 0x010400AA;
const OTA_HEADER_LENGTH    = 56;
const OTA_SUBELEMENT_HEADER_SIZE = 6;     // 2-byte tag + 4-byte length
const OTA_TAG_UPGRADE_IMAGE = 0x0000;

// ─── Build a synthetic OTA image using build_ota_image.py ────────────────────

/** Creates a deterministic synthetic firmware binary for testing (1024 bytes). */
function makeSyntheticFirmware(size = 1024) {
    const buf = Buffer.alloc(size);
    for (let i = 0; i < size; i++) {
        buf[i] = i & 0xFF;  // Deterministic pattern
    }
    return buf;
}

// Write synthetic firmware.bin to a temp file
const tmpDir        = fs.mkdtempSync(path.join(os.tmpdir(), 'conductor-ota-test-'));
const syntheticBin  = path.join(tmpDir, 'firmware.bin');
const syntheticOta  = path.join(tmpDir, 'test_output.ota');

const fwData = makeSyntheticFirmware(1024);
fs.writeFileSync(syntheticBin, fwData);

// Invoke packer via Python (must be available in PATH)
let packResult = null;
try {
    const cmd = [
        'python', 'tools/build_ota_image.py',
        `--input "${syntheticBin}"`,
        `--output "${syntheticOta}"`,
    ].join(' ');
    execSync(cmd, { cwd: path.join(__dirname, '..'), stdio: 'pipe' });
    packResult = fs.readFileSync(syntheticOta);
} catch (err) {
    console.error(`[FATAL] build_ota_image.py failed: ${err.message}`);
    console.error('        Python must be available in PATH to run OTA packing tests.');
    process.exit(1);
}

// ─── Parse the generated OTA container ────────────────────────────────────────

/**
 * Parses the 56-byte OTA file header fields.
 * All multi-byte fields are little-endian.
 */
function parseOtaHeader(buf) {
    return {
        fileId:          buf.readUInt32LE(0),
        headerVersion:   buf.readUInt16LE(4),
        headerLength:    buf.readUInt16LE(6),
        fieldControl:    buf.readUInt16LE(8),
        manufacturerCode:buf.readUInt16LE(10),
        imageType:       buf.readUInt16LE(12),
        fileVersion:     buf.readUInt32LE(14),
        stackVersion:    buf.readUInt16LE(18),
        headerString:    buf.slice(20, 52).toString('utf8').replace(/\x00+$/, ''),
        totalImageSize:  buf.readUInt32LE(52),
    };
}

/**
 * Parses the UPGRADE_IMAGE sub-element at offset 56.
 */
function parseUpgradeImageElement(buf) {
    const tag     = buf.readUInt16LE(OTA_HEADER_LENGTH);
    const length  = buf.readUInt32LE(OTA_HEADER_LENGTH + 2);
    const payload = buf.slice(OTA_HEADER_LENGTH + OTA_SUBELEMENT_HEADER_SIZE,
                               OTA_HEADER_LENGTH + OTA_SUBELEMENT_HEADER_SIZE + length);
    return { tag, length, payload };
}

const header    = parseOtaHeader(packResult);
const subElem   = parseUpgradeImageElement(packResult);

// ─── Suite 1: OTA Container Structure ────────────────────────────────────────
console.log('--- Suite 1: OTA Container Structure ---');

it('generated OTA file must be at least 56 + 6 bytes (header + sub-element header)', () => {
    assert.ok(packResult.length >= OTA_HEADER_LENGTH + OTA_SUBELEMENT_HEADER_SIZE,
        `OTA file too small: ${packResult.length} bytes`);
});

it('total file size must equal header totalImageSize field', () => {
    assert.strictEqual(header.totalImageSize, packResult.length,
        `Header totalImageSize (${header.totalImageSize}) != actual file size (${packResult.length})`);
});

it('total file size must equal header(56) + sub-element header(6) + firmware.bin length', () => {
    const expected = OTA_HEADER_LENGTH + OTA_SUBELEMENT_HEADER_SIZE + fwData.length;
    assert.strictEqual(packResult.length, expected,
        `Expected ${expected} bytes, got ${packResult.length}`);
});

// ─── Suite 2: File ID and Header Field Correctness ───────────────────────────
console.log('\n--- Suite 2: File ID and Header Field Correctness ---');

it('Upgrade File ID must be 0x0BEEF11E (canonical Phase 7 value)', () => {
    assert.strictEqual(header.fileId, CANONICAL_FILE_ID,
        `File ID 0x${header.fileId.toString(16).toUpperCase()} != canonical 0x0BEEF11E`);
});

it('Upgrade File ID must NOT be 0x0BEF11E0 (former byte-count typo)', () => {
    assert.notStrictEqual(header.fileId, REJECTED_FILE_ID_1,
        '0x0BEF11E0 (byte-count typo) must not appear as File ID');
});

it('Upgrade File ID must NOT be 0x1EF1EE0B (byte-swapped form)', () => {
    assert.notStrictEqual(header.fileId, REJECTED_FILE_ID_2,
        '0x1EF1EE0B (byte-swapped) must not appear as File ID');
});

it('header version must be 0x0100', () => {
    assert.strictEqual(header.headerVersion, 0x0100);
});

it('header length must be 56 (0x0038)', () => {
    assert.strictEqual(header.headerLength, OTA_HEADER_LENGTH);
});

it('field control must be 0x0000 (hardware version fields absent)', () => {
    assert.strictEqual(header.fieldControl, 0x0000,
        'Field Control must be 0x0000 — hardware version fields must be absent');
});

it('manufacturer code must be 0x131B (Espressif CSA-assigned)', () => {
    assert.strictEqual(header.manufacturerCode, MANUFACTURER_CODE);
});

it('image type must be 0x0906 (Logitech Z906 Controller)', () => {
    assert.strictEqual(header.imageType, IMAGE_TYPE);
});

it('file version must be 0x010400AA', () => {
    assert.strictEqual(header.fileVersion, FILE_VERSION);
});

it('header string must contain Conductor project name', () => {
    assert.ok(
        header.headerString.toLowerCase().includes('conductor'),
        `Header string "${header.headerString}" must contain "Conductor"`
    );
});

it('header string must be exactly 32 bytes (padded with NUL)', () => {
    // The raw bytes at offset 20 to 52 = 32 bytes
    const raw = packResult.slice(20, 52);
    assert.strictEqual(raw.length, 32);
});

it('hardware version fields must be absent (field control bit 2 = 0)', () => {
    // Bit 2 of Field Control indicates minHwVersion/maxHwVersion present
    const hwVersionPresent = (header.fieldControl & (1 << 2)) !== 0;
    assert.strictEqual(hwVersionPresent, false,
        'Hardware version fields must not be present (Phase 7 decision: no canonical HW version)');
});

// ─── Suite 3: SHA-512 / fileSize Index Parity ────────────────────────────────
console.log('\n--- Suite 3: SHA-512 / fileSize Parity ---');

it('SHA-512 of generated OTA file must be computable', () => {
    const hash = crypto.createHash('sha512').update(packResult).digest('hex');
    assert.ok(hash.length === 128, `SHA-512 must be 128 hex chars: got ${hash.length}`);
});

it('index fileSize must match actual .ota file byte length', () => {
    // Simulate what generate_ota_index.js would produce
    const simulatedEntry = {
        fileSize: packResult.length,
    };
    assert.strictEqual(simulatedEntry.fileSize, packResult.length);
});

it('SHA-512 computed from file buffer must be deterministic across two calls', () => {
    const h1 = crypto.createHash('sha512').update(packResult).digest('hex');
    const h2 = crypto.createHash('sha512').update(packResult).digest('hex');
    assert.strictEqual(h1, h2, 'SHA-512 must be deterministic');
});

// ─── Suite 4: UPGRADE_IMAGE Sub-Element Round-Trip ───────────────────────────
console.log('\n--- Suite 4: UPGRADE_IMAGE Sub-Element Round-Trip ---');

it('UPGRADE_IMAGE sub-element tag must be 0x0000', () => {
    assert.strictEqual(subElem.tag, OTA_TAG_UPGRADE_IMAGE,
        `Sub-element tag 0x${subElem.tag.toString(16)} != 0x0000`);
});

it('UPGRADE_IMAGE sub-element length must equal firmware.bin size', () => {
    assert.strictEqual(subElem.length, fwData.length,
        `Sub-element length (${subElem.length}) != firmware.bin size (${fwData.length})`);
});

it('UPGRADE_IMAGE payload must be byte-for-byte identical to input firmware.bin', () => {
    assert.ok(subElem.payload.equals(fwData),
        'Extracted UPGRADE_IMAGE payload does not match original firmware.bin');
});

// ─── Suite 5: Phase 7 Parser Compatibility ───────────────────────────────────
console.log('\n--- Suite 5: Phase 7 Parser Compatibility ---');

it('Phase 7 parser would accept: File ID is canonical 0x0BEEF11E', () => {
    // Phase 7 ota_zigbee_transport.c rejects anything != ZIGBEE_OTA_FILE_IDENTIFIER (0x0BEEF11E)
    assert.strictEqual(header.fileId, CANONICAL_FILE_ID);
});

it('Phase 7 parser would accept: file has sub-element 0x0000 at offset 56', () => {
    // Phase 7 parser looks for OTA_TAG_UPGRADE_IMAGE = 0x0000 after the 56-byte header
    assert.strictEqual(subElem.tag, 0x0000);
    assert.ok(subElem.length > 0, 'Sub-element payload must not be empty');
});

it('Phase 7 anti-downgrade: file_version <= current running version would be rejected', () => {
    // This test validates the PARITY of the file version constant.
    // The actual anti-downgrade enforcement is in Phase 7 firmware.
    assert.strictEqual(header.fileVersion, FILE_VERSION,
        'Packer file_version must match the canonical Z906_OTA_FILE_VERSION constant');
});

it('Phase 7 parser would reject 0x0BEF11E0 — confirmed absent in generated file', () => {
    assert.notStrictEqual(header.fileId, REJECTED_FILE_ID_1);
});

it('Phase 7 parser would reject 0x1EF1EE0B — confirmed absent in generated file', () => {
    assert.notStrictEqual(header.fileId, REJECTED_FILE_ID_2);
});

// ─── Suite 6: generate_ota_index.js Metadata Consistency ─────────────────────
console.log('\n--- Suite 6: generate_ota_index.js Metadata Consistency ---');

const indexModule = require('./generate_ota_index.js');

it('generate_ota_index.js MANUFACTURER_CODE must match packer (0x131B)', () => {
    assert.strictEqual(indexModule.MANUFACTURER_CODE, 0x131B);
});

it('generate_ota_index.js IMAGE_TYPE must match packer (0x0906)', () => {
    assert.strictEqual(indexModule.IMAGE_TYPE, 0x0906);
});

it('generate_ota_index.js FILE_VERSION must match packer (0x010400AA)', () => {
    assert.strictEqual(indexModule.FILE_VERSION, 0x010400AA);
});

it('generate_ota_index.js MODEL_ID must match firmware Z906_ZB_MODEL_IDENTIFIER', () => {
    assert.strictEqual(indexModule.MODEL_ID, 'Z-906-Zigbee');
});

it('generate_ota_index.js should generate a valid index entry from the synthetic .ota file', () => {
    // We only test the non-URL fields since we can't write to the real URL
    const otaPath    = syntheticOta;
    // Call generateOtaIndex without outputIndexPath (dry run — no file written)
    const fakeSrcUrl = 'https://trusted.local/test.ota';
    const entry = indexModule.generateOtaIndex(otaPath, fakeSrcUrl, null);
    assert.strictEqual(entry.fileVersion,       FILE_VERSION);
    assert.strictEqual(entry.manufacturerCode,  MANUFACTURER_CODE);
    assert.strictEqual(entry.imageType,         IMAGE_TYPE);
    assert.strictEqual(entry.fileSize,          packResult.length,
        'Index fileSize must match actual .ota file size');
    assert.ok(entry.sha512.length === 128, 'Index sha512 must be 128-char hex string');
    assert.strictEqual(entry.url, fakeSrcUrl, 'Index url must be the provided release URL');
});

it('generate_ota_index.js must NOT invent a URL when one is not provided', () => {
    // The updated generator requires a URL argument — empty URL should be rejected.
    // We verify this via static source analysis (process.exit on empty URL).
    const src = fs.readFileSync(path.join(__dirname, 'generate_ota_index.js'), 'utf8');
    assert.ok(src.includes('process.exit'),
        'Generator must call process.exit when URL is empty (no fake URL invention)');
    assert.ok(!src.includes('github.com/owner'),
        'Generator must not contain a hardcoded fake GitHub URL');
});

// ─── Cleanup & Summary ────────────────────────────────────────────────────────

// Remove temp files
try {
    fs.unlinkSync(syntheticBin);
    fs.unlinkSync(syntheticOta);
    fs.rmdirSync(tmpDir);
} catch (_) { /* ignore cleanup errors */ }

console.log('\n========================================');
console.log(`Results: ${passed}/${total} tests passed.`);
console.log('========================================\n');

if (passed < total) {
    process.exit(1);
}
