#!/usr/bin/env python3
"""Compile the production Z906 byte codec and check LRC-correct malformed frames.

Uses temporary host files only. Does not modify the checkout or access hardware.
"""
from pathlib import Path
from tempfile import TemporaryDirectory
import subprocess

ROOT = Path(__file__).resolve().parents[1]
CODEC = ROOT / "src/protocol/codec/z906_codec.c"

HARNESS = r"""
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "z906_codec.h"

static const uint8_t reference[20] = {
    0x0A, 0x15, 0x15, 0x15, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x03, 0x00,
    0x00, 0x00, 0x06, 0x01, 0x03, 0x00,
    0x00, 0x00
};

static bool parse(const uint8_t *payload, uint8_t len,
                  bool expect_valid) {
    z906_codec_ctx_t codec;
    z906_decoded_packet_t decoded;
    z906_codec_init(&codec);
    assert(z906_codec_feed_byte(&codec, 0xAA, &decoded)
           == Z906_PARSE_RESULT_IN_PROGRESS);
    assert(z906_codec_feed_byte(&codec, 0x0A, &decoded)
           == Z906_PARSE_RESULT_IN_PROGRESS);
    assert(z906_codec_feed_byte(&codec, len, &decoded)
           == Z906_PARSE_RESULT_IN_PROGRESS);
    for (size_t i = 0; i < len; ++i) {
        assert(z906_codec_feed_byte(&codec, payload[i], &decoded)
               == Z906_PARSE_RESULT_IN_PROGRESS);
    }
    uint8_t lrc = z906_codec_calc_frame_lrc(0x0A, len, payload, len);
    z906_parse_result_t result = z906_codec_feed_byte(&codec, lrc, &decoded);
    assert(!z906_codec_is_frame_in_progress(&codec));
    assert(result == (expect_valid ? Z906_PARSE_RESULT_FRAME_COMPLETE
                                   : Z906_PARSE_RESULT_RESYNC_DROP));
    if (expect_valid) {
        assert(decoded.type == Z906_PACKET_CONFIG);
        assert(decoded.data.config.raw_len == 20);
    }
    return result == Z906_PARSE_RESULT_FRAME_COMPLETE;
}
int main(void) {
    parse(reference, 20, true);
    uint8_t extended[21];
    memcpy(extended, reference, 20);
    extended[20] = 0;
    parse(extended, 21, false);
    parse(reference, 19, false);
    const uint8_t invalid_offsets[] = {0, 1, 2, 3, 4, 5, 6, 11, 17, 19};
    const uint8_t invalid_values[]  = {44,44,44,44, 6, 2, 4, 4,  2,  2};
    for (size_t i = 0; i < sizeof(invalid_offsets); ++i) {
        uint8_t data[20];
        memcpy(data, reference, 20);
        data[invalid_offsets[i]] = invalid_values[i];
        parse(data, 20, false);
    }
    uint8_t data[20];
    memcpy(data, reference, 20);
    data[3] = 43;
    data[4] = 5;
    data[11] = 3;
    data[17] = 1;
    data[19] = 1;
    parse(data, 20, true);
    puts("PASS: Z906 production codec host tests (14 cases)");
}
"""

def main():
    with TemporaryDirectory(prefix="conductor-codec-") as tmp:
        source = Path(tmp) / "codec_host.c"
        binary = Path(tmp) / "codec_host"
        source.write_text(HARNESS, encoding="utf-8")
        subprocess.run([
            "cc", "-std=gnu11", "-Wall", "-Wextra", "-Werror",
            "-I", str(ROOT / "src/protocol/codec"),
            "-I", str(ROOT / "src/protocol/commands"),
            "-I", str(ROOT / "src/core/state_manager"),
            str(CODEC), str(source), "-o", str(binary)
        ], check=True, cwd=ROOT)
        subprocess.run([str(binary)], check=True, cwd=ROOT)

if __name__ == "__main__":
    main()
