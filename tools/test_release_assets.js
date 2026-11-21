'use strict';

/** @file test_release_assets.js
 * @brief Local positive/negative tests for the fail-closed Zigbee OTA release gate.
 * No GitHub API access, tags, or releases are created.
 */
const assert = require('node:assert/strict');
const crypto = require('node:crypto');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const { verifyReleaseAssets, OTA_NAME, NAMES } = require('./verify_release_assets.js');

const REPO = 'DenizOner/Conductor';
const TAG = 'v1.4.0';
let passed = 0;

function hash(bytes, alg) {
    return crypto.createHash(alg).update(bytes).digest('hex');
}

function fixture() {
    const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'conductor-release-check-'));
    const fw = Buffer.from([0xE9, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07]);
    const ota = Buffer.alloc(62 + fw.length);
    ota.writeUInt32LE(0x0BEEF11E, 0);
    ota.writeUInt16LE(0x0100, 4);
    ota.writeUInt16LE(56, 6);
    ota.writeUInt16LE(0x0000, 8); // No optional hardware-version fields.
    ota.writeUInt16LE(0x131B, 10);
    ota.writeUInt16LE(0x0906, 12);
    ota.writeUInt32LE(0x010400AA, 14);
    ota.writeUInt16LE(0x0002, 18);
    Buffer.from('Conductor - Smart Zigbee Console for Logitech z906', 'ascii')
        .copy(ota, 20, 0, 32);
    ota.writeUInt32LE(ota.length, 52);
    ota.writeUInt16LE(0, 56);
    ota.writeUInt32LE(fw.length, 58);
    fw.copy(ota, 62);
    const entry = {
        fileVersion: 0x010400AA, fileSize: ota.length,
        manufacturerCode: 0x131B, imageType: 0x0906,
        sha512: hash(ota, 'sha512'),
        url: 'https://github.com/' + REPO + '/releases/download/' + TAG + '/' + OTA_NAME,
        modelId: 'Z-906-Zigbee', manufacturerName: ['Logitech'], fileName: OTA_NAME
    };
    fs.writeFileSync(path.join(dir, 'firmware.bin'), fw);
    fs.writeFileSync(path.join(dir, OTA_NAME), ota);
    fs.writeFileSync(path.join(dir, 'index.json'), JSON.stringify([entry]));
    fs.writeFileSync(path.join(dir, 'conductor_converter.mjs'), 'export default {};\n');
    const release = {
        tag_name: TAG, draft: true,
        assets: NAMES.map(name => {
            const bytes = fs.readFileSync(path.join(dir, name));
            return { name, size: bytes.length, state: 'uploaded',
                digest: 'sha256:' + hash(bytes, 'sha256') };
        })
    };
    return { dir, release };
}

/* Negative tests assert the intended invariant, not an unrelated exception. */
function caseCheck(name, change, expectedError, tag = TAG) {
    const f = fixture();
    try {
        change(f);
        if (expectedError) {
            assert.throws(() => verifyReleaseAssets(f.dir, REPO, tag, f.release),
                expectedError, name);
        } else {
            assert.equal(verifyReleaseAssets(f.dir, REPO, tag, f.release).length, 4);
        }
        passed++;
        console.log('[PASS] ' + name);
    } finally {
        fs.rmSync(f.dir, { recursive: true, force: true });
    }
}

/* Hash-consistent malformed OTA data must still fail the header contract. */
function changeOtaAndRehash(f, mutate) {
    const otaPath = path.join(f.dir, OTA_NAME);
    const bytes = fs.readFileSync(otaPath);
    mutate(bytes);
    fs.writeFileSync(otaPath, bytes);
    const indexPath = path.join(f.dir, 'index.json');
    const index = JSON.parse(fs.readFileSync(indexPath, 'utf8'));
    index[0].sha512 = hash(bytes, 'sha512');
    fs.writeFileSync(indexPath, JSON.stringify(index));
    const remote = f.release.assets.find(asset => asset.name === OTA_NAME);
    remote.digest = 'sha256:' + hash(bytes, 'sha256');
}

/* Make the remote tag AND the index URL coherent to isolate version failures. */
function changeTag(f, tag) {
    f.release.tag_name = tag;
    const indexPath = path.join(f.dir, 'index.json');
    const index = JSON.parse(fs.readFileSync(indexPath, 'utf8'));
    index[0].url = 'https://github.com/' + REPO + '/releases/download/' + tag + '/' + OTA_NAME;
    fs.writeFileSync(indexPath, JSON.stringify(index));
}

caseCheck('complete four-asset draft is valid', () => {});
for (const name of NAMES) {
    caseCheck('missing ' + name + ' fails closed', f => fs.unlinkSync(path.join(f.dir, name)),
        /Exactly four canonical release assets required/);
}
caseCheck('extra unexpected asset fails closed', f =>
    fs.writeFileSync(path.join(f.dir, 'extra.txt'), 'untrusted'),
    /Exactly four canonical release assets required/);
caseCheck('empty converter fails closed', f =>
    fs.writeFileSync(path.join(f.dir, 'conductor_converter.mjs'), ''),
    /Invalid release asset size: conductor_converter.mjs/);
caseCheck('mismatched firmware payload fails closed', f => {
    const firmwarePath = path.join(f.dir, 'firmware.bin');
    const bytes = fs.readFileSync(firmwarePath);
    bytes[0] ^= 1;
    fs.writeFileSync(firmwarePath, bytes);
}, /OTA firmware bytes differ from firmware.bin/);
caseCheck('corrupt OTA image fails closed', f => {
    const p = path.join(f.dir, OTA_NAME);
    const bytes = fs.readFileSync(p);
    bytes[0] ^= 1;
    fs.writeFileSync(p, bytes);
}, /OTA magic mismatch/);
caseCheck('incorrect OTA SHA-512 fails closed', f => {
    const p = path.join(f.dir, 'index.json');
    const entry = JSON.parse(fs.readFileSync(p, 'utf8'));
    entry[0].sha512 = '0'.repeat(128);
    fs.writeFileSync(p, JSON.stringify(entry));
}, /OTA index SHA-512 mismatch/);
caseCheck('wrong index release URL fails closed', f => {
    const p = path.join(f.dir, 'index.json');
    const entry = JSON.parse(fs.readFileSync(p, 'utf8'));
    entry[0].url = 'https://example.org/wrong.ota';
    fs.writeFileSync(p, JSON.stringify(entry));
}, /OTA index release URL mismatch/);
caseCheck('wrong OTA index identity fails closed', f => {
    const p = path.join(f.dir, 'index.json');
    const entry = JSON.parse(fs.readFileSync(p, 'utf8'));
    entry[0].manufacturerCode = 1;
    fs.writeFileSync(p, JSON.stringify(entry));
}, /OTA index device identity mismatch/);
caseCheck('public release before validation fails', f => { f.release.draft = false; },
    /Release must remain a draft/);
caseCheck('missing remote asset fails', f => { f.release.assets.pop(); },
    /Draft release must contain exactly four assets/);
caseCheck('duplicate remote asset fails', f => {
    f.release.assets[3] = { ...f.release.assets[0] };
}, /Unexpected or duplicated draft release asset/);
caseCheck('remote asset SHA-256 mismatch fails', f => {
    f.release.assets[0].digest = 'sha256:' + '0'.repeat(64);
}, /Draft release SHA-256 mismatch/);
caseCheck('remote asset size mismatch fails', f => { f.release.assets[0].size += 1; },
    /Draft release asset size mismatch/);
caseCheck('remote asset upload incomplete fails', f => { f.release.assets[0].state = 'starter'; },
    /Draft release asset not fully uploaded/);
caseCheck('remote release tag mismatch fails', f => { f.release.tag_name = 'v2.0.0'; },
    /Release must remain a draft/);
/* AUD-P20-002: hashes can match an OTA whose header violates the format. */
caseCheck('optional hardware-version flag rejected with matching hashes',
    f => changeOtaAndRehash(f, bytes => bytes.writeUInt16LE(0x0004, 8)),
    /OTA header field control mismatch/);
caseCheck('reserved field-control bit rejected with matching hashes',
    f => changeOtaAndRehash(f, bytes => bytes.writeUInt16LE(0x8000, 8)),
    /OTA header field control mismatch/);
caseCheck('Zigbee stack version mismatch rejected with matching hashes',
    f => changeOtaAndRehash(f, bytes => bytes.writeUInt16LE(0x0003, 18)),
    /OTA Zigbee stack version mismatch/);
caseCheck('corrupt header string rejected with matching hashes',
    f => changeOtaAndRehash(f, bytes => { bytes[20] ^= 1; }),
    /OTA header string mismatch/);
caseCheck('wrong header length rejected with matching hashes',
    f => changeOtaAndRehash(f, bytes => bytes.writeUInt16LE(58, 6)),
    /OTA header version or length mismatch/);
caseCheck('wrong OTA file version rejected with matching hashes',
    f => changeOtaAndRehash(f, bytes => bytes.writeUInt32LE(0x010401AA, 14)),
    /OTA firmware version mismatch/);

/* AUD-P20-003: internally consistent labels do not override firmware config. */
caseCheck('v2.0.0 tag rejected even with matching index URL and remote tag',
    f => changeTag(f, 'v2.0.0'), /Release tag does not match firmware version/, 'v2.0.0');
caseCheck('v1.4.1 tag rejected even with matching index URL and remote tag',
    f => changeTag(f, 'v1.4.1'), /Release tag does not match firmware version/, 'v1.4.1');
caseCheck('stable firmware cannot use a prerelease tag',
    f => changeTag(f, 'v1.4.0-rc1'), /Invalid version tag/, 'v1.4.0-rc1');
caseCheck('malformed OTA index JSON rejected',
    f => fs.writeFileSync(path.join(f.dir, 'index.json'), '{invalid json}'),
    /Unexpected token|Expected property name/);
caseCheck('missing remote SHA-256 digest rejected',
    f => { delete f.release.assets[0].digest; }, /Draft release SHA-256 mismatch/);
caseCheck('wrong remote digest algorithm rejected',
    f => { f.release.assets[0].digest = 'sha512:' + '0'.repeat(128); },
    /Draft release SHA-256 mismatch/);

console.log('PASS: release fail-closed tests (' + passed + ' cases)');
