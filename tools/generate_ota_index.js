'use strict';

/**
 * @file generate_ota_index.js
 * @brief Generates a Zigbee2MQTT OTA index.json entry for Conductor firmware updates.
 *
 * Output format adheres to the official Zigbee2MQTT OTA index schema:
 * [
 *   {
 *     "fileVersion":      17039530,
 *     "fileSize":         703754,
 *     "manufacturerCode": 4891,
 *     "imageType":        2310,
 *     "sha512":           "...",
 *     "url":              "https://<your-trusted-server>/131B-0906-010400AA-Conductor.ota",
 *     "modelId":          "Z-906-Zigbee",
 *     "manufacturerName": ["Logitech"]
 *   }
 * ]
 *
 * SECURITY NOTE:
 *   Only host firmware images on a TRUSTED local or HTTPS server that you control.
 *   Conductor firmware does not implement cryptographic image signature verification (Phase 7).
 *   Do NOT use unreviewed public or third-party URLs.
 *
 * USAGE:
 *   node tools/generate_ota_index.js \
 *       dist/131B-0906-010400AA-Conductor.ota \
 *       https://<your-trusted-server>/131B-0906-010400AA-Conductor.ota \
 *       dist/index.json
 *
 * Arguments:
 *   $1 : Path to the .ota file (required)
 *   $2 : Public URL where the .ota file will be served (required — do NOT leave empty)
 *   $3 : Output path for the index.json file (default: dist/index.json)
 *
 * ZIGBEE2MQTT CONFIGURATION:
 *   After generating dist/index.json, add to Zigbee2MQTT configuration.yaml:
 *
 *     ota:
 *       zigbee_ota_override_index_location: /path/to/dist/index.json
 *       # or for remote:
 *       # zigbee_ota_override_index_location: https://<your-trusted-server>/index.json
 */

const fs     = require('fs');
const path   = require('path');
const crypto = require('crypto');

// ─── Canonical OTA Constants (must match include/config.h and build_ota_image.py) ────────────

/** Espressif CSA-assigned manufacturer code. Used by Conductor OTA metadata. */
const MANUFACTURER_CODE = 0x131B;   // 4891 decimal

/** Logitech Z906 Controller image type identifier. */
const IMAGE_TYPE = 0x0906;         // 2310 decimal

/**
 * Current firmware file version.
 * Must be incremented in include/config.h (Z906_OTA_FILE_VERSION) for each release.
 * Phase 7 anti-downgrade logic rejects images with file_version <= running version.
 */
const FILE_VERSION = 0x010400AA;   // 17039530 decimal

/** Firmware model identifier — matches Z906_ZB_MODEL_IDENTIFIER in config.h. */
const MODEL_ID = 'Z-906-Zigbee';

/**
 * Manufacturer name narrowing for the OTA index.
 * Reduces collision risk with other devices sharing the same manufacturer code.
 * Array format matches Zigbee2MQTT index schema.
 */
const MANUFACTURER_NAME = ['Logitech'];

// ─── Index Generator ─────────────────────────────────────────────────────────

/**
 * Generates a Zigbee2MQTT OTA index.json entry from a packaged .ota file.
 *
 * Reads the .ota file to compute its actual size and SHA-512 hash.
 * If an existing index.json exists at outputIndexPath, merges the entry
 * (replacing any existing entry for the same modelId + imageType).
 *
 * Hardware version constraints are intentionally absent — Phase 7 firmware
 * has no canonical hardware revision source (FC=0x00 in Query Next Image Request).
 *
 * @param {string} otaFilePath      - Absolute or relative path to the .ota file.
 * @param {string} releaseUrl       - Full HTTPS URL where the .ota file will be served.
 *                                    Must be a real URL — this generator does not invent URLs.
 * @param {string} [outputIndexPath] - Output path for the generated index.json.
 * @returns {object} The generated index entry object.
 */
function generateOtaIndex(otaFilePath, releaseUrl, outputIndexPath) {
    if (!fs.existsSync(otaFilePath)) {
        console.error(`Error: OTA file not found at "${otaFilePath}"`);
        process.exit(1);
    }

    if (!releaseUrl || releaseUrl.trim() === '') {
        console.error('Error: A real release URL is required. Do not leave the URL empty.');
        console.error('Usage: node tools/generate_ota_index.js <ota-file> <url> [output-index]');
        process.exit(1);
    }

    const otaBuffer = fs.readFileSync(otaFilePath);
    const fileSize  = otaBuffer.length;
    const sha512    = crypto.createHash('sha512').update(otaBuffer).digest('hex');

    const entry = {
        fileVersion:      FILE_VERSION,
        fileSize:         fileSize,
        manufacturerCode: MANUFACTURER_CODE,
        imageType:        IMAGE_TYPE,
        sha512:           sha512,
        url:              releaseUrl.trim(),
        modelId:          MODEL_ID,
        manufacturerName: MANUFACTURER_NAME,
        // Note: No hardware version constraints (minimum/maximum) — Phase 7 decision.
        // Note: fileName is informational only; not required by Zigbee2MQTT OTA subsystem.
        fileName:         path.basename(otaFilePath),
        otaHeaderString:  'Conductor - Smart Zigbee Console for Logitech z906',
    };

    let indexData = [entry];

    // Merge with existing index: replace entry with matching modelId + imageType
    if (outputIndexPath && fs.existsSync(outputIndexPath)) {
        try {
            const raw      = fs.readFileSync(outputIndexPath, 'utf8');
            const existing = JSON.parse(raw);
            if (Array.isArray(existing)) {
                const filtered = existing.filter(
                    (e) => !(e.modelId === MODEL_ID && e.imageType === IMAGE_TYPE)
                );
                filtered.push(entry);
                indexData = filtered;
            }
        } catch (_err) {
            // Overwrite corrupt index
        }
    }

    const jsonStr = JSON.stringify(indexData, null, 2);

    if (outputIndexPath) {
        fs.mkdirSync(path.dirname(path.resolve(outputIndexPath)), { recursive: true });
        fs.writeFileSync(outputIndexPath, jsonStr, 'utf8');
        console.log(`Generated OTA index: ${outputIndexPath}`);
    }

    console.log('─── Generated Zigbee2MQTT OTA Index Entry ───────────────────────────');
    console.log(JSON.stringify(entry, null, 2));
    console.log('─────────────────────────────────────────────────────────────────────');

    return entry;
}

// ─── CLI Entry Point ──────────────────────────────────────────────────────────

if (require.main === module) {
    const args           = process.argv.slice(2);
    const otaPath        = args[0] || 'dist/131B-0906-010400AA-Conductor.ota';
    const releaseUrl     = args[1] || '';
    const outputIndex    = args[2] || 'dist/index.json';

    generateOtaIndex(otaPath, releaseUrl, outputIndex);
}

// ─── CommonJS Exports (for test harness) ─────────────────────────────────────

module.exports = {
    generateOtaIndex,
    MANUFACTURER_CODE,
    IMAGE_TYPE,
    FILE_VERSION,
    MODEL_ID,
    MANUFACTURER_NAME,
};
