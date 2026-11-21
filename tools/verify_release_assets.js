'use strict';

/**
 * @file verify_release_assets.js
 * @brief Fail-closed local and draft-release integrity gate for Conductor OTA.
 *
 * Usage: node tools/verify_release_assets.js <directory> <owner/repo> <v-tag> [release-json]
 * Uses only Node.js built-ins; never creates, uploads, or publishes a release.
 */
const fs = require('node:fs');
const path = require('node:path');
const crypto = require('node:crypto');

const OTA_NAME = '1000-0906-010400aa-LogitechZ906.ota';
const NAMES = Object.freeze(['firmware.bin', OTA_NAME, 'index.json', 'conductor_converter.mjs']);
const MAX_OTA_SLOT_BYTES = 0x1C0000;
const FILE_VERSION = 0x010400AA;
const MANUFACTURER = 0x131B;
const IMAGE_TYPE = 0x0906;

function insist(condition, message) {
    if (!condition) throw new Error(message);
}

function sha(buffer, algorithm) {
    return crypto.createHash(algorithm).update(buffer).digest('hex');
}

function verifyReleaseAssets(directory, repository, tag, release) {
    insist(/^[A-Za-z0-9_.-]+\/[A-Za-z0-9_.-]+$/.test(repository), 'Invalid repository identifier');
    insist(/^v[0-9][A-Za-z0-9._-]*$/.test(tag), 'Invalid version tag');

    const actualNames = fs.readdirSync(directory).sort();
    insist(actualNames.length === NAMES.length &&
        NAMES.every(name => actualNames.includes(name)), 'Exactly four canonical release assets required');

    const files = new Map();
    for (const name of NAMES) {
        const filename = path.join(directory, name);
        const info = fs.lstatSync(filename);
        insist(info.isFile() && !info.isSymbolicLink(), 'Refuse non-regular release asset: ' + name);
        const maxSize = name === OTA_NAME ? MAX_OTA_SLOT_BYTES + 62 : MAX_OTA_SLOT_BYTES;
        insist(info.size > 0 && info.size <= maxSize,
            'Invalid release asset size: ' + name);
        files.set(name, fs.readFileSync(filename));
    }

    const firmware = files.get('firmware.bin');
    const ota = files.get(OTA_NAME);
    insist(ota.length >= 62, 'Truncated Zigbee OTA container');
    insist(ota.readUInt32LE(0) === 0x0BEEF11E, 'OTA magic mismatch');
    insist(ota.readUInt16LE(4) === 0x0100 && ota.readUInt16LE(6) === 56,
        'OTA header version or length mismatch');
    insist(ota.readUInt16LE(10) === MANUFACTURER && ota.readUInt16LE(12) === IMAGE_TYPE,
        'OTA device identity mismatch');
    insist(ota.readUInt32LE(14) === FILE_VERSION, 'OTA firmware version mismatch');
    insist(ota.readUInt32LE(52) === ota.length, 'OTA container length mismatch');
    insist(ota.readUInt16LE(56) === 0 && ota.readUInt32LE(58) === firmware.length,
        'OTA firmware sub-element mismatch');
    insist(ota.subarray(62).equals(firmware), 'OTA firmware bytes differ from firmware.bin');

    const index = JSON.parse(files.get('index.json').toString('utf8'));
    insist(Array.isArray(index) && index.length === 1, 'Exactly one OTA index entry required');
    const entry = index[0];
    insist(entry !== null && typeof entry === 'object', 'Malformed OTA index entry');
    const expectedUrl = 'https://github.com/' + repository + '/releases/download/' + tag + '/' + OTA_NAME;
    insist(entry.url === expectedUrl, 'OTA index release URL mismatch');
    insist(entry.fileName === OTA_NAME, 'OTA index fileName mismatch');
    insist(entry.fileSize === ota.length && entry.fileVersion === FILE_VERSION,
        'OTA index size/version mismatch');
    insist(entry.manufacturerCode === MANUFACTURER && entry.imageType === IMAGE_TYPE &&
        entry.modelId === 'Z-906-Zigbee' && Array.isArray(entry.manufacturerName) &&
        entry.manufacturerName.length === 1 && entry.manufacturerName[0] === 'Logitech',
        'OTA index device identity mismatch');
    insist(entry.sha512 === sha(ota, 'sha512'), 'OTA index SHA-512 mismatch');

    if (release !== undefined) {
        insist(release !== null && typeof release === 'object' &&
            release.tag_name === tag && release.draft === true,
            'Release must remain a draft for pre-publication verification');
        insist(Array.isArray(release.assets) && release.assets.length === NAMES.length,
            'Draft release must contain exactly four assets');
        const seen = new Set();
        for (const asset of release.assets) {
            insist(asset && NAMES.includes(asset.name) && !seen.has(asset.name),
                'Unexpected or duplicated draft release asset');
            seen.add(asset.name);
            insist(asset.state === 'uploaded', 'Draft release asset not fully uploaded: ' + asset.name);
            const buffer = files.get(asset.name);
            insist(asset.size === buffer.length, 'Draft release asset size mismatch: ' + asset.name);
            insist(asset.digest === 'sha256:' + sha(buffer, 'sha256'),
                'Draft release SHA-256 mismatch: ' + asset.name);
        }
    }
    return NAMES.map(name => ({
        name, size: files.get(name).length, sha256: sha(files.get(name), 'sha256')
    }));
}

if (require.main === module) {
    try {
        const [directory, repository, tag, releasePath] = process.argv.slice(2);
        insist(directory && repository && tag, 'Usage: verify_release_assets.js <directory> <owner/repo> <v-tag> [release-json]');
        const release = releasePath ? JSON.parse(fs.readFileSync(releasePath, 'utf8')) : undefined;
        const files = verifyReleaseAssets(directory, repository, tag, release);
        for (const file of files) console.log('[PASS] ' + file.name + ' (' + file.size + ' bytes)');
        console.log('[PASS] Release integrity gate; no publication performed');
    } catch (err) {
        console.error('[FAIL] Release integrity: ' + err.message);
        process.exitCode = 1;
    }
}

module.exports = { verifyReleaseAssets, OTA_NAME, NAMES };
