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
    ota.writeUInt16LE(0x131B, 10);
    ota.writeUInt16LE(0x0906, 12);
    ota.writeUInt32LE(0x010400AA, 14);
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

function caseCheck(name, change, shouldFail = true) {
    const f = fixture();
    try {
        change(f);
        if (shouldFail) {
            assert.throws(() => verifyReleaseAssets(f.dir, REPO, TAG, f.release), Error, name);
        } else {
            assert.equal(verifyReleaseAssets(f.dir, REPO, TAG, f.release).length, 4);
        }
        passed++;
        console.log('[PASS] ' + name);
    } finally {
        fs.rmSync(f.dir, { recursive: true, force: true });
    }
}

caseCheck('complete four-asset draft is valid', () => {}, false);
for (const name of NAMES) {
    caseCheck('missing ' + name + ' fails closed', f => fs.unlinkSync(path.join(f.dir, name)));
}
caseCheck('extra unexpected asset fails closed', f =>
    fs.writeFileSync(path.join(f.dir, 'extra.txt'), 'untrusted'));
caseCheck('empty converter fails closed', f =>
    fs.writeFileSync(path.join(f.dir, 'conductor_converter.mjs'), ''));
caseCheck('mismatched firmware payload fails closed', f =>
    fs.writeFileSync(path.join(f.dir, 'firmware.bin'), 'different'));
caseCheck('corrupt OTA image fails closed', f => {
    const p = path.join(f.dir, OTA_NAME);
    const bytes = fs.readFileSync(p);
    bytes[0] ^= 1;
    fs.writeFileSync(p, bytes);
});
caseCheck('incorrect OTA SHA-512 fails closed', f => {
    const p = path.join(f.dir, 'index.json');
    const entry = JSON.parse(fs.readFileSync(p, 'utf8'));
    entry[0].sha512 = '0'.repeat(128);
    fs.writeFileSync(p, JSON.stringify(entry));
});
caseCheck('wrong index release URL fails closed', f => {
    const p = path.join(f.dir, 'index.json');
    const entry = JSON.parse(fs.readFileSync(p, 'utf8'));
    entry[0].url = 'https://example.org/wrong.ota';
    fs.writeFileSync(p, JSON.stringify(entry));
});
caseCheck('wrong OTA index identity fails closed', f => {
    const p = path.join(f.dir, 'index.json');
    const entry = JSON.parse(fs.readFileSync(p, 'utf8'));
    entry[0].manufacturerCode = 1;
    fs.writeFileSync(p, JSON.stringify(entry));
});
caseCheck('public release before validation fails', f => { f.release.draft = false; });
caseCheck('missing remote asset fails', f => { f.release.assets.pop(); });
caseCheck('duplicate remote asset fails', f => {
    f.release.assets[3] = { ...f.release.assets[0] };
});
caseCheck('remote asset SHA-256 mismatch fails', f => {
    f.release.assets[0].digest = 'sha256:' + '0'.repeat(64);
});
caseCheck('remote asset size mismatch fails', f => { f.release.assets[0].size += 1; });
caseCheck('remote asset upload incomplete fails', f => { f.release.assets[0].state = 'starter'; });
caseCheck('remote release tag mismatch fails', f => { f.release.tag_name = 'v2.0.0'; });
console.log('PASS: release fail-closed tests (' + passed + ' cases)');
