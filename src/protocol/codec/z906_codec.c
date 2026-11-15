/**
 * @file z906_codec.c
 * @brief Implementation of packet serialization, deserialization, and strict LRC validation.
 */

#include "z906_codec.h"
#include <string.h>

void z906_codec_init(z906_codec_ctx_t *ctx)
{
    if (ctx == NULL) {
        return;
    }
    ctx->state = CODEC_STATE_IDLE;
    ctx->type = 0;
    ctx->expected_len = 0;
    ctx->bytes_read = 0;
    memset(ctx->buffer, 0, sizeof(ctx->buffer));
}

bool z906_codec_is_frame_in_progress(const z906_codec_ctx_t *ctx)
{
    if (ctx == NULL) {
        return false;
    }
    return (ctx->state != CODEC_STATE_IDLE);
}

uint8_t z906_codec_calc_lrc(const uint8_t *data, size_t len)
{
    if (data == NULL || len == 0) {
        return 0;
    }
    uint8_t sum = 0;
    for (size_t i = 0; i < len; i++) {
        sum += data[i];
    }
    return (uint8_t)((~sum) + 1);
}

uint8_t z906_codec_calc_frame_lrc(uint8_t type, uint8_t len, const uint8_t *payload, size_t payload_len)
{
    uint8_t sum = type + len;
    if (payload != NULL) {
        for (size_t i = 0; i < payload_len; i++) {
            sum += payload[i];
        }
    }
    return (uint8_t)((~sum) + 1);
}

bool z906_codec_verify_lrc(uint8_t type, uint8_t len, const uint8_t *payload, size_t payload_len, uint8_t lrc_byte)
{
    uint8_t sum = type + len + lrc_byte;
    if (payload != NULL) {
        for (size_t i = 0; i < payload_len; i++) {
            sum += payload[i];
        }
    }
    return (sum == 0);
}

z906_parse_result_t z906_codec_feed_byte(z906_codec_ctx_t *ctx, uint8_t byte, z906_decoded_packet_t *out_packet)
{
    if (ctx == NULL) {
        return Z906_PARSE_RESULT_STANDALONE;
    }

    switch (ctx->state) {
        case CODEC_STATE_IDLE:
            if (byte == Z906_CMD_MULTIBYTE_REPLY) {
                ctx->state = CODEC_STATE_WAIT_TYPE;
                ctx->bytes_read = 0;
                return Z906_PARSE_RESULT_IN_PROGRESS;
            }
            /* Not the preamble of a multi-byte frame: this is a standalone byte candidate */
            return Z906_PARSE_RESULT_STANDALONE;

        case CODEC_STATE_WAIT_TYPE:
            ctx->type = byte;
            ctx->state = CODEC_STATE_WAIT_LEN;
            return Z906_PARSE_RESULT_IN_PROGRESS;

        case CODEC_STATE_WAIT_LEN:
            ctx->expected_len = byte;
            ctx->bytes_read = 0;
            if (ctx->expected_len > 0 && ctx->expected_len <= Z906_MAX_PAYLOAD_LEN) {
                ctx->state = CODEC_STATE_WAIT_PAYLOAD;
                return Z906_PARSE_RESULT_IN_PROGRESS;
            } else if (ctx->expected_len == 0) {
                ctx->state = CODEC_STATE_WAIT_LRC;
                return Z906_PARSE_RESULT_IN_PROGRESS;
            } else {
                /* Invalid length: reset parser to IDLE */
                ctx->state = CODEC_STATE_IDLE;
                return Z906_PARSE_RESULT_RESYNC_DROP;
            }

        case CODEC_STATE_WAIT_PAYLOAD:
            ctx->buffer[ctx->bytes_read++] = byte;
            if (ctx->bytes_read >= ctx->expected_len) {
                ctx->state = CODEC_STATE_WAIT_LRC;
            }
            return Z906_PARSE_RESULT_IN_PROGRESS;

        case CODEC_STATE_WAIT_LRC: {
            /* Always return to IDLE state upon processing the trailing LRC byte */
            ctx->state = CODEC_STATE_IDLE;

            /* Mandatory LRC Checksum Verification per protocol.rst */
            if (!z906_codec_verify_lrc(ctx->type, ctx->expected_len, ctx->buffer, ctx->bytes_read, byte)) {
                /* Checksum mismatch, discard frame and signal resync drop */
                return Z906_PARSE_RESULT_RESYNC_DROP;
            }

            if (out_packet == NULL) {
                return Z906_PARSE_RESULT_FRAME_COMPLETE;
            }

            memset(out_packet, 0, sizeof(z906_decoded_packet_t));

            /* 1. Decode 20-byte amplifier configuration packet (Type 0x0A, Length 0x14) */
            if (ctx->type == Z906_MB_CONFIG_AMP && ctx->bytes_read >= 20) {
                out_packet->type = Z906_PACKET_CONFIG;
                out_packet->data.config.volume                = ctx->buffer[0];
                out_packet->data.config.volume_rear           = ctx->buffer[1];
                out_packet->data.config.volume_center         = ctx->buffer[2];
                out_packet->data.config.volume_bass           = ctx->buffer[3];
                out_packet->data.config.input                 = (z906_input_t)(ctx->buffer[4] + 1);
                out_packet->data.config.mute                  = (ctx->buffer[5] != 0);

                /* Channel LUT for spatial effect order from amplifier: 4, 5, 2, 6, 1, 3 */
                static const int ch_lut[6] = {4, 5, 2, 6, 1, 3};
                out_packet->data.config.effect = Z906_EFFECT_NONE;
                for (int i = 0; i < 6; i++) {
                    if (ch_lut[i] == (int)out_packet->data.config.input) {
                        uint8_t eff_code = ctx->buffer[6 + i];
                        if (eff_code == 0) out_packet->data.config.effect = Z906_EFFECT_3D;
                        else if (eff_code == 1) out_packet->data.config.effect = Z906_EFFECT_2_1;
                        else if (eff_code == 2) out_packet->data.config.effect = Z906_EFFECT_4_1;
                        else out_packet->data.config.effect = Z906_EFFECT_NONE;
                        break;
                    }
                }

                out_packet->data.config.digital_signal_status = ctx->buffer[12];
                out_packet->data.config.standby               = (ctx->buffer[17] != 0);
                out_packet->data.config.auto_standby_enabled  = (ctx->buffer[19] == 0);
                out_packet->data.config.raw_len               = ctx->bytes_read;
                memcpy(out_packet->data.config.raw_payload, ctx->buffer, ctx->bytes_read);
                return Z906_PARSE_RESULT_FRAME_COMPLETE;
            }

            /* 2. Decode Temperature telemetry packet (Type 0x0C, Length 5) */
            if (ctx->type == Z906_MB_AMP_TEMP && ctx->bytes_read >= 5) {
                out_packet->type = Z906_PACKET_TEMP;
                if (z906_codec_decode_temperature(ctx->buffer, ctx->bytes_read, &out_packet->data.temp)) {
                    return Z906_PARSE_RESULT_FRAME_COMPLETE;
                }
                return Z906_PARSE_RESULT_RESYNC_DROP;
            }

            /* 3. Decode Idle duration telemetry packet (Type 0x0F, Length 3) */
            if (ctx->type == Z906_MB_READ_IDLE && ctx->bytes_read >= 3) {
                out_packet->type = Z906_PACKET_IDLE;
                if (z906_codec_decode_idle(ctx->buffer, ctx->bytes_read, &out_packet->data.idle)) {
                    return Z906_PARSE_RESULT_FRAME_COMPLETE;
                }
                return Z906_PARSE_RESULT_RESYNC_DROP;
            }

            /* 4. Decode Input volume telemetry packet (Type 0x08, Length 3) */
            if (ctx->type == Z906_MB_INPUT_VOLUME && ctx->bytes_read >= 3) {
                out_packet->type = Z906_PACKET_INPUT_VOL;
                if (z906_codec_decode_input_volume(ctx->buffer, ctx->bytes_read, &out_packet->data.input_vol)) {
                    return Z906_PARSE_RESULT_FRAME_COMPLETE;
                }
                return Z906_PARSE_RESULT_RESYNC_DROP;
            }

            return Z906_PARSE_RESULT_RESYNC_DROP;
        }

        default:
            ctx->state = CODEC_STATE_IDLE;
            return Z906_PARSE_RESULT_RESYNC_DROP;
    }
}

bool z906_codec_parse_packet(z906_codec_ctx_t *ctx, uint8_t byte, z906_decoded_packet_t *out_packet)
{
    z906_parse_result_t res = z906_codec_feed_byte(ctx, byte, out_packet);
    return (res == Z906_PARSE_RESULT_FRAME_COMPLETE);
}

bool z906_codec_parse_byte(z906_codec_ctx_t *ctx, uint8_t byte, z906_amp_config_t *out_config)
{
    z906_decoded_packet_t pkt;
    if (z906_codec_parse_packet(ctx, byte, &pkt)) {
        if (pkt.type == Z906_PACKET_CONFIG) {
            if (out_config != NULL) {
                *out_config = pkt.data.config;
            }
            return true;
        }
    }
    return false;
}

bool z906_codec_decode_temperature(const uint8_t *payload, size_t len, z906_amp_temp_t *out_temp)
{
    if (payload == NULL || out_temp == NULL || len < 5) {
        return false;
    }

    out_temp->temp_celsius[0] = (int8_t)payload[1];
    out_temp->temp_celsius[1] = (int8_t)payload[2];
    out_temp->temp_celsius[2] = (int8_t)payload[3];
    out_temp->is_valid = true;
    return true;
}

bool z906_codec_decode_idle(const uint8_t *payload, size_t len, z906_amp_idle_t *out_idle)
{
    if (payload == NULL || out_idle == NULL || len < 3) {
        return false;
    }

    out_idle->idle_seconds = ((uint32_t)payload[0] << 16) |
                             ((uint32_t)payload[1] << 8)  |
                              (uint32_t)payload[2];
    out_idle->is_valid = true;
    return true;
}

bool z906_codec_decode_input_volume(const uint8_t *payload, size_t len, z906_amp_input_vol_t *out_vol)
{
    if (payload == NULL || out_vol == NULL || len < 3) {
        return false;
    }

    out_vol->input_volume = ((uint32_t)payload[0] << 16) |
                            ((uint32_t)payload[1] << 8)  |
                             (uint32_t)payload[2];
    out_vol->is_valid = true;
    return true;
}
