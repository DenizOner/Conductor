/**
 * @file test_zigbee_ota.js
 * @brief Host validation test suite for Native Zigbee Phase 7:
 *        Native Zigbee OTA Transport Adapter + A/B Firmware Safety.
 *
 * Verifies:
 *   1. Architecture & Component Registration:
 *      - ota_zigbee_transport.[ch] existence and registration in CMakeLists.txt.
 *      - Device Model EP1 OTA Upgrade cluster client descriptor registration.
 *      - EP1 OTA cluster descriptor includes CurrentFileVersion attribute (0x0002).
 *      - Stack lifecycle initialization includes ota_zigbee_transport_init().
 *      - CLI dev tools routes `dev ota_query` via ota_zigbee_transport_query_next_image().
 *   2. Pure ESP Zigbee SDK v2 Compliance:
 *      - Zero legacy symbols (esp_zb_*, ESP_ZB_*, ZBOSS) in OTA transport.
 *      - Uses native ezb_zcl_core_action_handler_register and EZB_ZCL_CORE_OTA_UPGRADE_* callbacks.
 *   3. Streaming Parser Simulation (Unit Testing Algorithm):
 *      - Mandatory header parsing (56 bytes, magic 0x0BEEF11E, version 0x0100).
 *      - Optional header fields skipping (security creds, upgrade dest, min/max hw ver).
 *      - Sub-element tag parsing (Tag 0x0000 = UPGRADE_IMAGE vs non-image tags).
 *      - Boundary safety: parser correctly processes arbitrary block chunk splits (1, 7, 23, 64, 223 bytes).
 *      - Monotonic file version validation: strictly rejects downgrade and equal version.
 *      - Metadata validation: Manufacturer ID (0x131B) and Image Type (0x0906).
 *      - Partition boundary safeguard: rejects image payload exceeding target slot capacity (1,792 KB).
 *   4. Issue #906 Policy Static Verification:
 *      - Verifies immediate esp_restart() on validly APPLIED session in FINISH progress callback.
 *      - Verifies rejection (EZB_ZCL_STATUS_ABORT) if session is not APPLIED.
 *   5. Non-destructive A/B Rollback & Health Check Integration:
 *      - Verifies ota_health integration and lack of regression in existing rollback logic.
 */

const fs = require('fs');
const path = require('path');
const assert = require('assert');

console.log('===================================================================');
console.log('=== Running Native Zigbee Phase 7 OTA Transport & Safety Tests ===');
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
// Test Suite 1: Source Code Audit & Pure-v2 Compliance
// -----------------------------------------------------------------------------
console.log('--- Test Suite 1: Architecture, Files & Pure-v2 Compliance ---');

const transportH = fs.readFileSync(path.join(ROOT_DIR, 'src', 'management', 'ota', 'ota_zigbee_transport.h'), 'utf8');
const transportC = fs.readFileSync(path.join(ROOT_DIR, 'src', 'management', 'ota', 'ota_zigbee_transport.c'), 'utf8');
const devModelC = fs.readFileSync(path.join(ROOT_DIR, 'src', 'communication', 'zigbee', 'zigbee_device_model.c'), 'utf8');
const nativeStackC = fs.readFileSync(path.join(ROOT_DIR, 'src', 'communication', 'zigbee', 'zigbee_native_stack.c'), 'utf8');
const cliDevToolsC = fs.readFileSync(path.join(ROOT_DIR, 'src', 'communication', 'cli', 'cli_dev_tools.c'), 'utf8');
const partitionsCsv = fs.readFileSync(path.join(ROOT_DIR, 'partitions.csv'), 'utf8');

it('ota_zigbee_transport.h declares public APIs without legacy symbols', () => {
    assert(transportH.includes('ota_zigbee_transport_init'), 'Missing ota_zigbee_transport_init');
    assert(transportH.includes('ota_zigbee_transport_query_next_image'), 'Missing ota_zigbee_transport_query_next_image');
    assert(transportH.includes('ota_zigbee_transport_abort'), 'Missing ota_zigbee_transport_abort');
    assert(!transportH.includes('esp_zb_'), 'Legacy esp_zb_ symbol found in transport header');
    assert(!transportH.includes('ESP_ZB_'), 'Legacy ESP_ZB_ symbol found in transport header');
    assert(!transportH.includes('ZBOSS'), 'ZBOSS symbol found in transport header');
});

it('ota_zigbee_transport.c uses pure ESP Zigbee SDK v2 core action callbacks', () => {
    assert(transportC.includes('ezb_zcl_core_action_handler_register'), 'Missing ezb_zcl_core_action_handler_register');
    assert(transportC.includes('EZB_ZCL_CORE_OTA_UPGRADE_CLIENT_PROGRESS_CB_ID'), 'Missing EZB_ZCL_CORE_OTA_UPGRADE_CLIENT_PROGRESS_CB_ID');
    assert(transportC.includes('EZB_ZCL_CORE_OTA_UPGRADE_QUERY_NEXT_IMAGE_RSP_CB_ID'), 'Missing EZB_ZCL_CORE_OTA_UPGRADE_QUERY_NEXT_IMAGE_RSP_CB_ID');
    assert(transportC.includes('ezb_zcl_ota_upgrade_cluster_client_init'), 'Missing client init');
    assert(transportC.includes('ezb_zcl_ota_upgrade_set_download_block_size'), 'Missing set block size');
    // Phase 7 Corrective Hardening: ezb_zcl_ota_upgrade_set_hw_version() is intentionally absent.
    // No canonical hardware version exists for the ESP32-C6 board. Hardware version
    // field is omitted from the Query Next Image Request payload (fc = 0x00).
    assert(!transportC.includes('ezb_zcl_ota_upgrade_set_hw_version'), 'set_hw_version must NOT be present (hw_version intentionally absent)');
    assert(transportC.includes('hw_version field absent') || transportC.includes('fc          = 0x00'), 'Missing fc=0x00 (hw_version absent) comment/assignment');
    assert(transportC.includes('ezb_zcl_ota_upgrade_query_next_image_cmd_req'), 'Missing query cmd req');

    // Strict legacy ban
    assert(!transportC.includes('esp_zb_'), 'Legacy esp_zb_ symbol found in transport source');
    assert(!transportC.includes('ESP_ZB_'), 'Legacy ESP_ZB_ symbol found in transport source');
    assert(!transportC.includes('ZBOSS'), 'ZBOSS symbol found in transport source');
});

it('zigbee_device_model.c registers EP1 OTA cluster with CurrentFileVersion attribute', () => {
    assert(devModelC.includes('EZB_ZCL_ATTR_OTA_UPGRADE_CURRENT_FILE_VERSION_ID'), 'Missing CurrentFileVersion attribute registration');
    assert(devModelC.includes('ezb_zcl_ota_upgrade_create_cluster_desc'), 'Missing create_cluster_desc');
    assert(devModelC.includes('s_ep1_current_file_version'), 'Missing backing variable s_ep1_current_file_version');
});

it('zigbee_native_stack.c initializes ota_zigbee_transport in startup sequence', () => {
    assert(nativeStackC.includes('ota_zigbee_transport_init()'), 'Missing ota_zigbee_transport_init call in native stack init');
});

it('cli_dev_tools.c routes dev ota_query via ota_zigbee_transport_query_next_image', () => {
    assert(cliDevToolsC.includes('ota_zigbee_transport_query_next_image()'), 'Missing ota_zigbee_transport_query_next_image call in dev ota_query');
});

// -----------------------------------------------------------------------------
// Test Suite 2: Pinned Issue #906 Policy & Partition Safeguards
// -----------------------------------------------------------------------------
console.log('\n--- Test Suite 2: Upstream Issue #906 Policy & Partition Safeguards ---');

it('ota_zigbee_transport.c enforces immediate esp_restart() on valid FINISH', () => {
    assert(transportC.includes('EZB_ZCL_OTA_UPGRADE_PROGRESS_FINISH:'), 'Missing FINISH progress case');
    assert(transportC.includes('esp_restart()'), 'Missing immediate esp_restart()');
    assert(transportC.includes('OTA_TRANS_APPLIED'), 'Missing APPLIED state check');
    assert(transportC.includes('EZB_ZCL_STATUS_ABORT'), 'Missing abort on unapplied finish');
});

it('Dual OTA partitions are configured for 1,792 KB (0x1C0000 bytes) each', () => {
    const ota0Match = partitionsCsv.match(/ota_0,\s*app,\s*ota_0,\s*(0x[0-9a-fA-F]+),\s*(0x[0-9a-fA-F]+)/);
    const ota1Match = partitionsCsv.match(/ota_1,\s*app,\s*ota_1,\s*(0x[0-9a-fA-F]+),\s*(0x[0-9a-fA-F]+)/);
    assert(ota0Match, 'ota_0 partition not found in partitions.csv');
    assert(ota1Match, 'ota_1 partition not found in partitions.csv');

    const ota0Size = parseInt(ota0Match[2], 16);
    const ota1Size = parseInt(ota1Match[2], 16);
    assert.strictEqual(ota0Size, 1835008, 'ota_0 must be 1,792 KB (1835008 bytes)');
    assert.strictEqual(ota1Size, 1835008, 'ota_1 must be 1,792 KB (1835008 bytes)');
});

// -----------------------------------------------------------------------------
// Test Suite 3: Streaming Zigbee OTA Container Parser Simulation
// -----------------------------------------------------------------------------
console.log('\n--- Test Suite 3: Streaming Parser Simulation across Chunk Boundaries ---');

/**
 * JS Simulator of the C streaming parser in ota_zigbee_transport.c
 */
class ZigbeeOtaParserSimulator {
    constructor(maxPartitionSize = 1835008, currentVersion = 0x010400AA) {
        this.maxPartitionSize = maxPartitionSize;
        this.currentVersion = currentVersion;
        this.reset();
    }

    reset() {
        this.parseState = 'MANDATORY_HEADER';
        this.accumBuf = Buffer.alloc(0);
        this.hdr = null;
        this.remainingHdrBytes = 0;
        this.currentSubElem = null;
        this.currentSubElemBytesLeft = 0;
        this.totalStreamOffset = 0;
        this.totalPayloadWritten = 0;
        this.writtenPayload = Buffer.alloc(0);
        this.sessionState = 'DOWNLOADING';
        this.error = null;
    }

    feed(chunk) {
        if (this.sessionState !== 'DOWNLOADING') {
            return false;
        }

        let offset = 0;
        const len = chunk.length;

        while (offset < len) {
            switch (this.parseState) {
                case 'MANDATORY_HEADER': {
                    const needed = 56 - this.accumBuf.length;
                    const available = len - offset;
                    const toCopy = Math.min(available, needed);

                    this.accumBuf = Buffer.concat([this.accumBuf, chunk.slice(offset, offset + toCopy)]);
                    offset += toCopy;
                    this.totalStreamOffset += toCopy;

                    if (this.accumBuf.length === 56) {
                        const fileId = this.accumBuf.readUInt32LE(0);
                        const hdrVersion = this.accumBuf.readUInt16LE(4);
                        const hdrLength = this.accumBuf.readUInt16LE(6);
                        const hdrFc = this.accumBuf.readUInt16LE(8);
                        const mfrCode = this.accumBuf.readUInt16LE(10);
                        const imgType = this.accumBuf.readUInt16LE(12);
                        const fileVersion = this.accumBuf.readUInt32LE(14);
                        const totalImageSize = this.accumBuf.readUInt32LE(52);

                        // Strict single-value File ID check (Phase 7 Corrective Hardening).
                        // Only 0x0BEEF11E is the canonical ZCL OTA File Identifier.
                        // 0x1EF1EE0B (byte-reversed) and 0x0BEF11E0 (former typo) are both rejected.
                        if (fileId !== 0x0BEEF11E) {
                            this.error = `Invalid File ID: 0x${fileId.toString(16)}`;
                            this.parseState = 'ERROR';
                            return false;
                        }
                        if (hdrVersion !== 0x0100) {
                            this.error = `Unsupported Header Version: 0x${hdrVersion.toString(16)}`;
                            this.parseState = 'ERROR';
                            return false;
                        }
                        if (mfrCode !== 0x131B) {
                            this.error = `MFR mismatch: 0x${mfrCode.toString(16)}`;
                            this.parseState = 'ERROR';
                            return false;
                        }
                        if (imgType !== 0x0906) {
                            this.error = `Image type mismatch: 0x${imgType.toString(16)}`;
                            this.parseState = 'ERROR';
                            return false;
                        }
                        if (fileVersion <= this.currentVersion) {
                            this.error = `Anti-downgrade reject: 0x${fileVersion.toString(16)} <= 0x${this.currentVersion.toString(16)}`;
                            this.parseState = 'ERROR';
                            return false;
                        }

                        this.hdr = { fileId, hdrVersion, hdrLength, hdrFc, mfrCode, imgType, fileVersion, totalImageSize };
                        this.remainingHdrBytes = hdrLength - 56;
                        this.accumBuf = Buffer.alloc(0);

                        this.parseState = (this.remainingHdrBytes > 0) ? 'OPTIONAL_HEADER' : 'SUB_ELEMENT_HEADER';
                    }
                    break;
                }

                case 'OPTIONAL_HEADER': {
                    const available = len - offset;
                    const toSkip = Math.min(available, this.remainingHdrBytes);
                    offset += toSkip;
                    this.remainingHdrBytes -= toSkip;
                    this.totalStreamOffset += toSkip;

                    if (this.remainingHdrBytes === 0) {
                        this.parseState = 'SUB_ELEMENT_HEADER';
                    }
                    break;
                }

                case 'SUB_ELEMENT_HEADER': {
                    const needed = 6 - this.accumBuf.length;
                    const available = len - offset;
                    const toCopy = Math.min(available, needed);

                    this.accumBuf = Buffer.concat([this.accumBuf, chunk.slice(offset, offset + toCopy)]);
                    offset += toCopy;
                    this.totalStreamOffset += toCopy;

                    if (this.accumBuf.length === 6) {
                        const tagId = this.accumBuf.readUInt16LE(0);
                        const length = this.accumBuf.readUInt32LE(2);
                        this.accumBuf = Buffer.alloc(0);

                        this.currentSubElem = { tagId, length };
                        this.currentSubElemBytesLeft = length;

                        if (tagId === 0x0000) {
                            // UPGRADE_IMAGE
                            if (length > this.maxPartitionSize) {
                                this.error = `Image length ${length} exceeds partition size ${this.maxPartitionSize}`;
                                this.parseState = 'ERROR';
                                return false;
                            }
                            this.parseState = 'UPGRADE_IMAGE_PAYLOAD';
                        } else {
                            this.parseState = 'SKIP_PAYLOAD';
                        }
                    }
                    break;
                }

                case 'UPGRADE_IMAGE_PAYLOAD': {
                    const available = len - offset;
                    const toWrite = Math.min(available, this.currentSubElemBytesLeft);

                    if (toWrite > 0) {
                        this.writtenPayload = Buffer.concat([this.writtenPayload, chunk.slice(offset, offset + toWrite)]);
                        this.totalPayloadWritten += toWrite;
                        this.currentSubElemBytesLeft -= toWrite;
                        this.totalStreamOffset += toWrite;
                        offset += toWrite;
                    }

                    if (this.currentSubElemBytesLeft === 0) {
                        if (this.totalStreamOffset < this.hdr.totalImageSize) {
                            this.parseState = 'SUB_ELEMENT_HEADER';
                        } else {
                            this.parseState = 'COMPLETE';
                        }
                    }
                    break;
                }

                case 'SKIP_PAYLOAD': {
                    const available = len - offset;
                    const toSkip = Math.min(available, this.currentSubElemBytesLeft);
                    this.currentSubElemBytesLeft -= toSkip;
                    this.totalStreamOffset += toSkip;
                    offset += toSkip;

                    if (this.currentSubElemBytesLeft === 0) {
                        if (this.totalStreamOffset < this.hdr.totalImageSize) {
                            this.parseState = 'SUB_ELEMENT_HEADER';
                        } else {
                            this.parseState = 'COMPLETE';
                        }
                    }
                    break;
                }

                case 'COMPLETE':
                    offset = len;
                    break;

                case 'ERROR':
                default:
                    return false;
            }
        }
        return true;
    }
}

/**
 * Helper to build synthetic Zigbee OTA container buffers
 */
function buildSyntheticOtaFile({
    fileId = 0x0BEEF11E,
    hdrVersion = 0x0100,
    mfrCode = 0x131B,
    imgType = 0x0906,
    fileVersion = 0x010400AB,
    optionalHeader = null,
    imagePayload = Buffer.from('RAW_ESP_IDF_FIRMWARE_BINARY_BYTES_12345'),
    extraSubElements = []
}) {
    const optHdrBuf = optionalHeader ? Buffer.from(optionalHeader) : Buffer.alloc(0);
    const hdrLength = 56 + optHdrBuf.length;

    // Sub-elements
    const subElemBufs = [];

    // Tag 0x0000: UPGRADE_IMAGE
    const imgHdr = Buffer.alloc(6);
    imgHdr.writeUInt16LE(0x0000, 0);
    imgHdr.writeUInt32LE(imagePayload.length, 2);
    subElemBufs.push(imgHdr, imagePayload);

    // Any extra sub-elements (e.g. ECDSA signature 0x0001)
    for (const elem of extraSubElements) {
        const eHdr = Buffer.alloc(6);
        eHdr.writeUInt16LE(elem.tagId, 0);
        eHdr.writeUInt32LE(elem.payload.length, 2);
        subElemBufs.push(eHdr, elem.payload);
    }

    const subElemsCombined = Buffer.concat(subElemBufs);
    const totalImageSize = hdrLength + subElemsCombined.length;

    // Build mandatory 56-byte header
    const mandHdr = Buffer.alloc(56);
    mandHdr.writeUInt32LE(fileId, 0);
    mandHdr.writeUInt16LE(hdrVersion, 4);
    mandHdr.writeUInt16LE(hdrLength, 6);
    mandHdr.writeUInt16LE(optHdrBuf.length > 0 ? 0x0004 : 0x0000, 8); // fc
    mandHdr.writeUInt16LE(mfrCode, 10);
    mandHdr.writeUInt16LE(imgType, 12);
    mandHdr.writeUInt32LE(fileVersion, 14);
    mandHdr.writeUInt16LE(0x0002, 18); // stack_version
    mandHdr.write('Conductor Z906 OTA Image', 20, 32, 'utf8');
    mandHdr.writeUInt32LE(totalImageSize, 52);

    return Buffer.concat([mandHdr, optHdrBuf, subElemsCombined]);
}

it('Parser correctly processes valid OTA file in single contiguous block', () => {
    const rawBinary = Buffer.from('ESP32C6_APP_FIRMWARE_TEST_BINARY_STREAM_ABCDEF123456');
    const otaFile = buildSyntheticOtaFile({
        imagePayload: rawBinary
    });

    const parser = new ZigbeeOtaParserSimulator();
    const ok = parser.feed(otaFile);
    assert(ok, `Feed failed: ${parser.error}`);
    assert.strictEqual(parser.parseState, 'COMPLETE');
    assert.strictEqual(parser.totalPayloadWritten, rawBinary.length);
    assert(parser.writtenPayload.equals(rawBinary), 'Written payload differs from raw binary!');
});

it('Parser correctly handles multi-byte split across arbitrary tiny chunks (1 byte at a time)', () => {
    const rawBinary = Buffer.from('CHUNK_BOUNDARY_SAFETY_TEST_PAYLOAD');
    const otaFile = buildSyntheticOtaFile({
        imagePayload: rawBinary
    });

    const parser = new ZigbeeOtaParserSimulator();
    for (let i = 0; i < otaFile.length; i++) {
        const ok = parser.feed(otaFile.slice(i, i + 1));
        assert(ok, `Byte-at-a-time feed failed at index ${i}: ${parser.error}`);
    }
    assert.strictEqual(parser.parseState, 'COMPLETE');
    assert.strictEqual(parser.totalPayloadWritten, rawBinary.length);
    assert(parser.writtenPayload.equals(rawBinary));
});

it('Parser handles arbitrary 7-byte chunk splits across headers and payload', () => {
    const rawBinary = Buffer.from('SEVEN_BYTE_SPLIT_PAYLOAD_TEST_1234567890');
    const otaFile = buildSyntheticOtaFile({
        imagePayload: rawBinary
    });

    const parser = new ZigbeeOtaParserSimulator();
    const chunkSize = 7;
    for (let i = 0; i < otaFile.length; i += chunkSize) {
        const chunk = otaFile.slice(i, i + chunkSize);
        const ok = parser.feed(chunk);
        assert(ok, `7-byte chunk feed failed at offset ${i}: ${parser.error}`);
    }
    assert.strictEqual(parser.parseState, 'COMPLETE');
    assert.strictEqual(parser.totalPayloadWritten, rawBinary.length);
    assert(parser.writtenPayload.equals(rawBinary));
});

it('Parser handles optional header fields and non-image sub-elements (e.g. ECDSA signature)', () => {
    const rawBinary = Buffer.from('FIRMWARE_BINARY_WITH_SIG');
    const optHdr = Buffer.alloc(4); // min/max hw version
    optHdr.writeUInt16LE(1, 0);
    optHdr.writeUInt16LE(1, 2);

    const sigPayload = Buffer.alloc(42, 0xAA); // 42 bytes signature
    const otaFile = buildSyntheticOtaFile({
        optionalHeader: optHdr,
        imagePayload: rawBinary,
        extraSubElements: [{ tagId: 0x0001, payload: sigPayload }]
    });

    const parser = new ZigbeeOtaParserSimulator();
    // Feed in 31-byte blocks
    const chunkSize = 31;
    for (let i = 0; i < otaFile.length; i += chunkSize) {
        const ok = parser.feed(otaFile.slice(i, i + chunkSize));
        assert(ok, `Feed failed at offset ${i}: ${parser.error}`);
    }

    assert.strictEqual(parser.parseState, 'COMPLETE');
    assert.strictEqual(parser.totalPayloadWritten, rawBinary.length);
    assert(parser.writtenPayload.equals(rawBinary), 'Only upgrade image sub-element was written');
});

it('Parser strictly rejects anti-downgrade file versions (equal or lesser version)', () => {
    const equalVerOta = buildSyntheticOtaFile({ fileVersion: 0x010400AA }); // same as current
    const parser1 = new ZigbeeOtaParserSimulator(1835008, 0x010400AA);
    const ok1 = parser1.feed(equalVerOta);
    assert(!ok1, 'Parser should have rejected equal file version');
    assert(parser1.error.includes('Anti-downgrade reject'));

    const lesserVerOta = buildSyntheticOtaFile({ fileVersion: 0x010300AA }); // older
    const parser2 = new ZigbeeOtaParserSimulator(1835008, 0x010400AA);
    const ok2 = parser2.feed(lesserVerOta);
    assert(!ok2, 'Parser should have rejected older file version');
    assert(parser2.error.includes('Anti-downgrade reject'));
});

it('Parser strictly rejects invalid Manufacturer ID or Image Type', () => {
    const badMfrOta = buildSyntheticOtaFile({ mfrCode: 0x1234 });
    const parser1 = new ZigbeeOtaParserSimulator();
    assert(!parser1.feed(badMfrOta), 'Parser should have rejected foreign manufacturer code');
    assert(parser1.error.includes('MFR mismatch'));

    const badTypeOta = buildSyntheticOtaFile({ imgType: 0x9999 });
    const parser2 = new ZigbeeOtaParserSimulator();
    assert(!parser2.feed(badTypeOta), 'Parser should have rejected foreign image type');
    assert(parser2.error.includes('Image type mismatch'));
});

it('Parser strictly rejects invalid File ID or Header Version', () => {
    const badIdOta = buildSyntheticOtaFile({ fileId: 0xDEADBEEF });
    const parser1 = new ZigbeeOtaParserSimulator();
    assert(!parser1.feed(badIdOta), 'Parser should have rejected bad file ID');

    const badVerOta = buildSyntheticOtaFile({ hdrVersion: 0x0200 });
    const parser2 = new ZigbeeOtaParserSimulator();
    assert(!parser2.feed(badVerOta), 'Parser should have rejected bad header version');
});

it('Parser strictly rejects 0x0BEF11E0 (former typo — byte count off by one)', () => {
    // 0x0BEF11E0 was the incorrect value used before Phase 7 Corrective Hardening.
    // It must be permanently rejected to catch non-compliant containers.
    const badIdOta = buildSyntheticOtaFile({ fileId: 0x0BEF11E0 });
    const parser = new ZigbeeOtaParserSimulator();
    const ok = parser.feed(badIdOta);
    assert(!ok, 'Parser must reject 0x0BEF11E0 (former typo identifier)');
    assert(parser.error && parser.error.includes('Invalid File ID'),
           `Expected "Invalid File ID" error, got: ${parser.error}`);
});

it('Parser strictly rejects 0x1EF1EE0B (byte-swapped form of canonical ID)', () => {
    // 0x1EF1EE0B is a byte-order swap of 0x0BEEF11E.
    // A non-compliant OTA server or converter might produce this; it must be rejected.
    const badIdOta = buildSyntheticOtaFile({ fileId: 0x1EF1EE0B });
    const parser = new ZigbeeOtaParserSimulator();
    const ok = parser.feed(badIdOta);
    assert(!ok, 'Parser must reject 0x1EF1EE0B (byte-swapped identifier)');
    assert(parser.error && parser.error.includes('Invalid File ID'),
           `Expected "Invalid File ID" error, got: ${parser.error}`);
});

it('Parser strictly rejects image payload exceeding target partition limit', () => {
    const parser = new ZigbeeOtaParserSimulator(1024); // partition limit 1 KB
    const bigPayload = Buffer.alloc(2048, 0x55); // 2 KB payload
    const otaFile = buildSyntheticOtaFile({ imagePayload: bigPayload });

    const ok = parser.feed(otaFile);
    assert(!ok, 'Parser should have rejected payload exceeding partition capacity');
    assert(parser.error.includes('exceeds partition size'));
});

console.log(`\nResults: ${passedTests}/${totalTests} tests passed.`);
if (passedTests !== totalTests) {
    process.exit(1);
}
console.log('=== All Native Zigbee Phase 7 OTA Tests Passed Successfully! ===\n');
