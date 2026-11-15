/**
 * @file validate_ota_image.js
 * @brief Automated validation of standard Zigbee .ota image container and index.json entry.
 */

'use strict';

const fs = require('fs');
const path = require('path');
const crypto = require('crypto');

const ZIGBEE_OTA_FILE_IDENTIFIER = 0x0BEF11E0;
const EXPECTED_MFR_CODE = 0x131B;
const EXPECTED_IMAGE_TYPE = 0x0906;
const EXPECTED_FILE_VERSION = 0x010400AA;
const HEADER_LENGTH = 56;

function validateOta(otaPath, indexPath) {
    console.log('=== Validating Zigbee OTA Container & Index ===\n');

    if (!fs.existsSync(otaPath)) {
        console.error(`[FAIL] OTA file does not exist: ${otaPath}`);
        process.exit(1);
    }

    const buf = fs.readFileSync(otaPath);
    if (buf.length < HEADER_LENGTH) {
        console.error(`[FAIL] File size ${buf.length} is smaller than minimum header length ${HEADER_LENGTH}`);
        process.exit(1);
    }

    // 1. Magic check
    const fileId = buf.readUInt32LE(0);
    if (fileId !== ZIGBEE_OTA_FILE_IDENTIFIER) {
        console.error(`[FAIL] Invalid Upgrade File ID: 0x${fileId.toString(16).toUpperCase()} != 0x${ZIGBEE_OTA_FILE_IDENTIFIER.toString(16).toUpperCase()}`);
        process.exit(1);
    }
    console.log(`  [PASS] Zigbee OTA Magic: 0x${fileId.toString(16).toUpperCase()}`);

    // 2. Header version
    const headerVer = buf.readUInt16LE(4);
    console.log(`  [PASS] Header Version: 0x${headerVer.toString(16).padStart(4, '0')}`);

    // 3. Header length
    const headerLen = buf.readUInt16LE(6);
    if (headerLen !== HEADER_LENGTH) {
        console.error(`[FAIL] Invalid header length: ${headerLen} != ${HEADER_LENGTH}`);
        process.exit(1);
    }
    console.log(`  [PASS] Header Length: ${headerLen} bytes`);

    // 4. MFR & Image Type
    const mfrCode = buf.readUInt16LE(10);
    const imgType = buf.readUInt16LE(12);
    if (mfrCode !== EXPECTED_MFR_CODE || imgType !== EXPECTED_IMAGE_TYPE) {
        console.error(`[FAIL] Identity mismatch: MFR=0x${mfrCode.toString(16)}, Type=0x${imgType.toString(16)}`);
        process.exit(1);
    }
    console.log(`  [PASS] Manufacturer Code: 0x${mfrCode.toString(16).toUpperCase()}, Image Type: 0x${imgType.toString(16).toUpperCase()}`);

    // 5. File Version
    const fileVer = buf.readUInt32LE(14);
    if (fileVer !== EXPECTED_FILE_VERSION) {
        console.error(`[FAIL] File version mismatch: 0x${fileVer.toString(16)} != 0x${EXPECTED_FILE_VERSION.toString(16)}`);
        process.exit(1);
    }
    console.log(`  [PASS] File Version: 0x${fileVer.toString(16).toUpperCase()} (${fileVer})`);

    // 6. Total image size check
    const totalSize = buf.readUInt32LE(52);
    if (totalSize !== buf.length) {
        console.error(`[FAIL] Header total size ${totalSize} does not match actual file length ${buf.length}`);
        process.exit(1);
    }
    console.log(`  [PASS] Header Total Size: ${totalSize} bytes matches actual file size`);

    // 7. Check index.json if present
    if (indexPath && fs.existsSync(indexPath)) {
        const indexRaw = fs.readFileSync(indexPath, 'utf8');
        const indexData = JSON.parse(indexRaw);
        const entry = Array.isArray(indexData) ? indexData[0] : indexData;

        const sha512Actual = crypto.createHash('sha512').update(buf).digest('hex');
        if (entry.sha512.toLowerCase() !== sha512Actual.toLowerCase()) {
            console.error(`[FAIL] SHA-512 in index does not match actual image SHA-512`);
            process.exit(1);
        }
        console.log(`  [PASS] Index SHA-512 matches file SHA-512`);

        if (entry.fileVersion !== fileVer || entry.fileSize !== totalSize) {
            console.error(`[FAIL] Index metadata mismatch with OTA container`);
            process.exit(1);
        }
        console.log(`  [PASS] Index fileVersion & fileSize match OTA container`);
    }

    console.log('\n=================================================================');
    console.log(' VERDICT: [SUCCESS] Zigbee OTA Container & Index fully validated!');
    console.log('=================================================================\n');
}

if (require.main === module) {
    const otaPath = process.argv[2] || 'dist/1000-0906-010400aa-LogitechZ906.ota';
    const indexPath = process.argv[3] || 'dist/index.json';
    validateOta(otaPath, indexPath);
}

module.exports = { validateOta };
