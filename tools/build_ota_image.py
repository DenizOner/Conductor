#!/usr/bin/env python3
"""
@file build_ota_image.py
@brief Builds and packages a standard Zigbee OTA binary image (.ota) from an ESP-IDF firmware binary.

Follows the Zigbee Cluster Library (ZCL) OTA Upgrade Cluster specification (Section 11.4):

Container structure:
  [56-byte OTA file header]
  [UPGRADE_IMAGE sub-element: tag=0x0000, length=4 LE, payload=firmware.bin]

OTA File Header (56 bytes, all multi-byte fields little-endian):
  Offset  Size  Field
  ─────────────────────────────────────────────────────
  0       4     Upgrade File ID       : 0x0BEEF11E (canonical; Phase 7 parser requires this exact value)
  4       2     Header Version        : 0x0100
  6       2     Header Length         : 56 (0x0038)
  8       2     Field Control         : 0x0000 (hardware version fields absent — intentional)
  10      2     Manufacturer Code     : 0x131B (Espressif CSA-assigned manufacturer code)
  12      2     Image Type            : 0x0906 (Logitech Z906 Controller image type ID)
  14      4     File Version          : 0x010400AA (matches Z906_OTA_FILE_VERSION in config.h)
  18      2     Zigbee Stack Version  : 0x0002
  20      32    Header String         : "Conductor - Smart Zigbee Console for Logitech z906" (NUL padded)
  52      4     Total Image Size      : 56 (header) + 6 (sub-element overhead) + len(firmware.bin)

UPGRADE_IMAGE Sub-Element (tag 0x0000):
  Offset  Size  Field
  ─────────────────────────────────────────────────────
  0       2     Tag ID     : 0x0000 (ZCL_OTA_TAG_UPGRADE_IMAGE)
  2       4     Length     : len(firmware.bin) — length of payload only, not including tag+len fields
  6       N     Payload    : firmware.bin (byte-for-byte copy)

Hardware Version Note:
  Field Control bit 2 (hardware version present) is NOT set.
  minimum_hardware_version / maximum_hardware_version fields are absent.
  This is intentional: no canonical hardware revision exists on ESP32-C6. (Phase 7 decision)

Anti-Downgrade Note:
  Phase 7 firmware parser rejects any OTA image whose file_version <= current running version.
  Increment Z906_OTA_FILE_VERSION in include/config.h before each firmware release.

Computes SHA-512 and SHA-256 checksums for Zigbee2MQTT index descriptor generation.
"""

import sys
import os
import struct
import hashlib
import argparse

# ─── Canonical OTA Constants (must match include/config.h) ───────────────────

# Upgrade File ID: 0x0BEEF11E in host byte order → 0x1EF1EE0B little-endian on wire
# This is the ONLY accepted value by the Phase 7 ota_zigbee_transport.c parser.
# 0x0BEF11E0 (former typo) and 0x1EF1EE0B (byte-swapped) are strictly rejected.
ZIGBEE_OTA_FILE_IDENTIFIER = 0x0BEEF11E

ZIGBEE_OTA_HEADER_VERSION  = 0x0100
HEADER_LENGTH              = 56         # Fixed 56-byte OTA file header
FIELD_CONTROL              = 0x0000     # Hardware version fields absent (intentional)
MANUFACTURER_CODE          = 0x131B     # Espressif CSA-assigned manufacturer code
IMAGE_TYPE                 = 0x0906     # Logitech Z906 Controller image type ID
FILE_VERSION               = 0x010400AA # 1.4.0 stable release (matches Z906_OTA_FILE_VERSION)
STACK_VERSION              = 0x0002
HEADER_STRING              = b"Conductor - Smart Zigbee Console for Logitech z906"

# UPGRADE_IMAGE sub-element tag (ZCL OTA spec §11.4.2)
OTA_TAG_UPGRADE_IMAGE      = 0x0000
OTA_SUBELEMENT_HEADER_SIZE = 6          # 2 bytes tag + 4 bytes length field


def build_zigbee_ota_header(raw_bin_size: int,
                             mfr_code: int = MANUFACTURER_CODE,
                             img_type: int = IMAGE_TYPE,
                             file_version: int = FILE_VERSION) -> bytes:
    """
    Builds the 56-byte Zigbee OTA file header.

    Total image size in the header includes:
      56 (header) + 6 (sub-element tag+length) + raw_bin_size (payload).

    Args:
        raw_bin_size: Length of the raw firmware.bin payload.
        mfr_code:     Manufacturer code (default: 0x131B).
        img_type:     Image type ID (default: 0x0906).
        file_version: 32-bit file version (default: 0x010400AA).

    Returns:
        56-byte header as bytes.
    """
    # Total image size covers the complete file: header + sub-element header + payload
    total_size = HEADER_LENGTH + OTA_SUBELEMENT_HEADER_SIZE + raw_bin_size

    # Pad or truncate header string to exactly 32 bytes
    padded_string = HEADER_STRING.ljust(32, b'\x00')[:32]

    # Struct format: all little-endian
    #  I  = uint32 (Upgrade File ID)        4 bytes
    #  H  = uint16 (Header Version)         2 bytes
    #  H  = uint16 (Header Length)          2 bytes
    #  H  = uint16 (Field Control)          2 bytes
    #  H  = uint16 (Manufacturer Code)      2 bytes
    #  H  = uint16 (Image Type)             2 bytes
    #  I  = uint32 (File Version)           4 bytes  ← uint32, not H
    #  H  = uint16 (Zigbee Stack Version)   2 bytes
    #  32s= 32-byte char array (Header String)
    #  I  = uint32 (Total Image Size)       4 bytes
    # Total: 4+2+2+2+2+2+4+2+32+4 = 56 bytes ✓
    header = struct.pack(
        '<IHHHHHIH32sI',
        ZIGBEE_OTA_FILE_IDENTIFIER,
        ZIGBEE_OTA_HEADER_VERSION,
        HEADER_LENGTH,
        FIELD_CONTROL,
        mfr_code,
        img_type,
        file_version,
        STACK_VERSION,
        padded_string,
        total_size
    )



    assert len(header) == HEADER_LENGTH, (
        f'Header length mismatch: {len(header)} != {HEADER_LENGTH}'
    )
    return header


def build_upgrade_image_subelement(payload: bytes) -> bytes:
    """
    Wraps the raw firmware binary in a ZCL OTA UPGRADE_IMAGE sub-element (tag 0x0000).

    Sub-element layout (ZCL OTA spec §11.4.2):
      [2 bytes] Tag ID     = 0x0000
      [4 bytes] Length     = len(payload)
      [N bytes] Payload    = firmware.bin bytes

    The Length field covers the payload only (not the tag + length fields themselves).

    Args:
        payload: Raw firmware.bin bytes.

    Returns:
        6-byte sub-element header + payload bytes.
    """
    sub_header = struct.pack('<HI', OTA_TAG_UPGRADE_IMAGE, len(payload))
    assert len(sub_header) == OTA_SUBELEMENT_HEADER_SIZE, (
        f'Sub-element header size mismatch: {len(sub_header)} != {OTA_SUBELEMENT_HEADER_SIZE}'
    )
    return sub_header + payload


def package_ota(input_bin_path: str,
                output_ota_path: str,
                mfr_code: int = MANUFACTURER_CODE,
                img_type: int = IMAGE_TYPE,
                file_version: int = FILE_VERSION) -> dict:
    """
    Packages a raw ESP-IDF firmware.bin into a complete Zigbee OTA container file.

    The output file layout:
      [56-byte OTA file header]
      [6-byte UPGRADE_IMAGE sub-element header: tag=0x0000, length=LE32]
      [firmware.bin payload]

    Performs round-trip validation after writing:
      - total file size matches header Total Image Size field
      - UPGRADE_IMAGE payload length matches input firmware.bin length

    Args:
        input_bin_path:  Path to raw ESP-IDF firmware binary.
        output_ota_path: Destination path for the .ota container file.
        mfr_code:        Manufacturer code override.
        img_type:        Image type override.
        file_version:    File version override.

    Returns:
        dict with fileVersion, fileSize, manufacturerCode, imageType, sha512, sha256, otaPath.
    """
    if not os.path.isfile(input_bin_path):
        print(f'Error: Input firmware binary not found: {input_bin_path}', file=sys.stderr)
        sys.exit(1)

    with open(input_bin_path, 'rb') as f:
        bin_data = f.read()

    bin_size   = len(bin_data)
    header     = build_zigbee_ota_header(bin_size, mfr_code, img_type, file_version)
    subelement = build_upgrade_image_subelement(bin_data)
    ota_data   = header + subelement

    expected_total = HEADER_LENGTH + OTA_SUBELEMENT_HEADER_SIZE + bin_size
    assert len(ota_data) == expected_total, (
        f'OTA container size mismatch: {len(ota_data)} != {expected_total}'
    )

    # Round-trip validation: re-parse total_image_size from written header
    (parsed_total_size,) = struct.unpack_from('<I', ota_data, offset=52)
    assert parsed_total_size == len(ota_data), (
        f'Header Total Image Size ({parsed_total_size}) != actual file size ({len(ota_data)})'
    )

    # Round-trip validation: re-parse UPGRADE_IMAGE sub-element tag and length
    (parsed_tag, parsed_sub_len) = struct.unpack_from('<HI', ota_data, offset=HEADER_LENGTH)
    assert parsed_tag == OTA_TAG_UPGRADE_IMAGE, (
        f'Sub-element tag mismatch: 0x{parsed_tag:04X} != 0x{OTA_TAG_UPGRADE_IMAGE:04X}'
    )
    assert parsed_sub_len == bin_size, (
        f'Sub-element payload length ({parsed_sub_len}) != firmware.bin size ({bin_size})'
    )

    os.makedirs(os.path.dirname(os.path.abspath(output_ota_path)), exist_ok=True)
    with open(output_ota_path, 'wb') as f:
        f.write(ota_data)

    sha512_hash = hashlib.sha512(ota_data).hexdigest()
    sha256_hash = hashlib.sha256(ota_data).hexdigest()
    total_size  = len(ota_data)

    print('=================================================================')
    print(' Zigbee OTA Image Built Successfully')
    print('=================================================================')
    print(f'  Input Binary:       {input_bin_path} ({bin_size:,} bytes)')
    print(f'  Output OTA Image:   {output_ota_path} ({total_size:,} bytes)')
    print(f'  Manufacturer Code:  0x{mfr_code:04X}')
    print(f'  Image Type:         0x{img_type:04X}')
    print(f'  File Version:       0x{file_version:08X} ({file_version})')
    print(f'  Upgrade File ID:    0x{ZIGBEE_OTA_FILE_IDENTIFIER:08X}')
    print(f'  Header:             {HEADER_LENGTH} bytes')
    print(f'  Sub-element header: {OTA_SUBELEMENT_HEADER_SIZE} bytes (tag=0x{OTA_TAG_UPGRADE_IMAGE:04X})')
    print(f'  Firmware payload:   {bin_size:,} bytes')
    print(f'  Total OTA size:     {total_size:,} bytes')
    print(f'  SHA-256:            {sha256_hash}')
    print(f'  SHA-512:            {sha512_hash}')
    print('=================================================================')

    return {
        'fileVersion':      file_version,
        'fileSize':         total_size,
        'manufacturerCode': mfr_code,
        'imageType':        img_type,
        'sha512':           sha512_hash,
        'sha256':           sha256_hash,
        'otaPath':          output_ota_path,
    }


def main():
    parser = argparse.ArgumentParser(
        description='Package ESP32-C6 firmware binary into a standard Zigbee .ota image.'
    )
    parser.add_argument(
        '--input', '-i',
        default='.pio/build/esp32-c6-devkitc-1/firmware.bin',
        help='Path to raw firmware binary (default: .pio/build/esp32-c6-devkitc-1/firmware.bin)'
    )
    parser.add_argument(
        '--output', '-o',
        default='dist/131B-0906-010400AA-Conductor.ota',
        help='Output path for .ota file (default: dist/131B-0906-010400AA-Conductor.ota)'
    )
    parser.add_argument(
        '--mfr', type=lambda x: int(x, 0),
        default=MANUFACTURER_CODE,
        help=f'Manufacturer code (default: 0x{MANUFACTURER_CODE:04X})'
    )
    parser.add_argument(
        '--image-type', type=lambda x: int(x, 0),
        default=IMAGE_TYPE,
        help=f'Image type ID (default: 0x{IMAGE_TYPE:04X})'
    )
    parser.add_argument(
        '--version', type=lambda x: int(x, 0),
        default=FILE_VERSION,
        help=f'32-bit file version (default: 0x{FILE_VERSION:08X})'
    )

    args = parser.parse_args()
    package_ota(args.input, args.output, args.mfr, args.image_type, args.version)


if __name__ == '__main__':
    main()
