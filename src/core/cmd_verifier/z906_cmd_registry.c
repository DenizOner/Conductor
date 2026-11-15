/**
 * @file z906_cmd_registry.c
 * @brief Implementation of the centralized command registry and validation table.
 */

#include "z906_cmd_registry.h"
#include <string.h>

/**
 * @brief Static centralized command table defining all supported Z906 commands.
 */
static const z906_cmd_descriptor_t s_cmd_descriptors[Z906_CMD_COUNT] = {
    [Z906_CMD_ID_POWER] = {
        .id = Z906_CMD_ID_POWER,
        .name = "power",
        .cli_cmd = "power",
        .cli_subcmd = NULL,
        .param_type = PARAM_TYPE_BOOL,
        .min_val = 0,
        .max_val = 1,
        .zcl_endpoint = 1,
        .zcl_cluster = ZCL_CLUSTER_ON_OFF,
        .zcl_attr = 0x0000,
        .verify_type = VERIFY_CMD_POWER,
        .trim_channel = TRIM_CHANNEL_BASS,
        .is_verifiable = true,
        .description = "Power state control (Standby / ON)"
    },
    [Z906_CMD_ID_VOLUME] = {
        .id = Z906_CMD_ID_VOLUME,
        .name = "volume",
        .cli_cmd = "volume",
        .cli_subcmd = NULL,
        .param_type = PARAM_TYPE_INT,
        .min_val = 0,
        .max_val = 43,
        .zcl_endpoint = 1,
        .zcl_cluster = ZCL_CLUSTER_LEVEL_CONTROL,
        .zcl_attr = 0x0000,
        .verify_type = VERIFY_CMD_VOLUME,
        .trim_channel = TRIM_CHANNEL_BASS,
        .is_verifiable = true,
        .description = "Master volume control in discrete steps (0-43)"
    },
    [Z906_CMD_ID_MUTE] = {
        .id = Z906_CMD_ID_MUTE,
        .name = "mute",
        .cli_cmd = "mute",
        .cli_subcmd = NULL,
        .param_type = PARAM_TYPE_BOOL,
        .min_val = 0,
        .max_val = 1,
        .zcl_endpoint = 2,
        .zcl_cluster = ZCL_CLUSTER_Z906_TRIMS,
        .zcl_attr = ZCL_ATTR_AUDIO_MUTE,
        .verify_type = VERIFY_CMD_MUTE,
        .trim_channel = TRIM_CHANNEL_BASS,
        .is_verifiable = true,
        .description = "Amplifier audio mute toggle"
    },
    [Z906_CMD_ID_INPUT] = {
        .id = Z906_CMD_ID_INPUT,
        .name = "input",
        .cli_cmd = "input",
        .cli_subcmd = NULL,
        .param_type = PARAM_TYPE_ENUM,
        .min_val = 1,
        .max_val = 6,
        .zcl_endpoint = 3,
        .zcl_cluster = ZCL_CLUSTER_Z906_SOURCES,
        .zcl_attr = ZCL_ATTR_SOURCE_INPUT,
        .verify_type = VERIFY_CMD_INPUT,
        .trim_channel = TRIM_CHANNEL_BASS,
        .is_verifiable = true,
        .description = "Audio input source (1=6-Channel Direct, 2=RCA Stereo, 3=Optical 1, 4=Optical 2, 5=Digital Coaxial, 6=Auxiliary/AUX)"
    },
    [Z906_CMD_ID_EFFECT] = {
        .id = Z906_CMD_ID_EFFECT,
        .name = "effect",
        .cli_cmd = "effect",
        .cli_subcmd = NULL,
        .param_type = PARAM_TYPE_ENUM,
        .min_val = 0,
        .max_val = 3,
        .zcl_endpoint = 3,
        .zcl_cluster = ZCL_CLUSTER_Z906_SOURCES,
        .zcl_attr = ZCL_ATTR_SOURCE_EFFECT,
        .verify_type = VERIFY_CMD_EFFECT,
        .trim_channel = TRIM_CHANNEL_BASS,
        .is_verifiable = true,
        .description = "Spatial audio processing effect selection"
    },
    [Z906_CMD_ID_TRIM_BASS] = {
        .id = Z906_CMD_ID_TRIM_BASS,
        .name = "trim_bass",
        .cli_cmd = "trim",
        .cli_subcmd = "bass",
        .param_type = PARAM_TYPE_INT,
        .min_val = 0,
        .max_val = 43,
        .zcl_endpoint = 2,
        .zcl_cluster = ZCL_CLUSTER_Z906_TRIMS,
        .zcl_attr = ZCL_ATTR_TRIM_BASS,
        .verify_type = VERIFY_CMD_TRIM,
        .trim_channel = TRIM_CHANNEL_BASS,
        .is_verifiable = true,
        .description = "Subwoofer bass channel level (0-43)"
    },
    [Z906_CMD_ID_TRIM_CENTER] = {
        .id = Z906_CMD_ID_TRIM_CENTER,
        .name = "trim_center",
        .cli_cmd = "trim",
        .cli_subcmd = "center",
        .param_type = PARAM_TYPE_INT,
        .min_val = 0,
        .max_val = 43,
        .zcl_endpoint = 2,
        .zcl_cluster = ZCL_CLUSTER_Z906_TRIMS,
        .zcl_attr = ZCL_ATTR_TRIM_CENTER,
        .verify_type = VERIFY_CMD_TRIM,
        .trim_channel = TRIM_CHANNEL_CENTER,
        .is_verifiable = true,
        .description = "Center speaker channel level (0-43)"
    },
    [Z906_CMD_ID_TRIM_REAR] = {
        .id = Z906_CMD_ID_TRIM_REAR,
        .name = "trim_rear",
        .cli_cmd = "trim",
        .cli_subcmd = "rear",
        .param_type = PARAM_TYPE_INT,
        .min_val = 0,
        .max_val = 43,
        .zcl_endpoint = 2,
        .zcl_cluster = ZCL_CLUSTER_Z906_TRIMS,
        .zcl_attr = ZCL_ATTR_TRIM_REAR,
        .verify_type = VERIFY_CMD_TRIM,
        .trim_channel = TRIM_CHANNEL_REAR,
        .is_verifiable = true,
        .description = "Rear surround speakers level (0-43)"
    }
};

size_t z906_cmd_registry_get_count(void)
{
    return (size_t)Z906_CMD_COUNT;
}

const z906_cmd_descriptor_t *z906_cmd_registry_get_descriptor(size_t index)
{
    if (index >= Z906_CMD_COUNT) {
        return NULL;
    }
    return &s_cmd_descriptors[index];
}

const z906_cmd_descriptor_t *z906_cmd_registry_find_by_id(z906_cmd_id_t id)
{
    if (id < 0 || id >= Z906_CMD_COUNT) {
        return NULL;
    }
    return &s_cmd_descriptors[id];
}

const z906_cmd_descriptor_t *z906_cmd_registry_find_by_cli_name(const char *cmd_name, const char *subcmd)
{
    if (cmd_name == NULL) {
        return NULL;
    }

    for (size_t i = 0; i < Z906_CMD_COUNT; i++) {
        const z906_cmd_descriptor_t *desc = &s_cmd_descriptors[i];
        if (strcmp(desc->cli_cmd, cmd_name) == 0) {
            /* If this descriptor expects a sub-command (e.g. trim bass/center/rear) */
            if (desc->cli_subcmd != NULL) {
                if (subcmd != NULL && strcmp(desc->cli_subcmd, subcmd) == 0) {
                    return desc;
                }
            } else {
                /* Descriptor does not have a sub-command */
                if (subcmd == NULL || strlen(subcmd) == 0) {
                    return desc;
                }
            }
        }
    }

    return NULL;
}

const z906_cmd_descriptor_t *z906_cmd_registry_find_by_zcl(uint8_t endpoint, uint16_t cluster, uint16_t attr)
{
    for (size_t i = 0; i < Z906_CMD_COUNT; i++) {
        const z906_cmd_descriptor_t *desc = &s_cmd_descriptors[i];
        if (desc->zcl_endpoint == endpoint &&
            desc->zcl_cluster == cluster &&
            desc->zcl_attr == attr) {
            return desc;
        }
    }

    return NULL;
}

bool z906_cmd_registry_validate_param(z906_cmd_id_t id, int32_t value)
{
    const z906_cmd_descriptor_t *desc = z906_cmd_registry_find_by_id(id);
    if (desc == NULL) {
        return false;
    }

    return (value >= desc->min_val && value <= desc->max_val);
}

bool z906_cmd_registry_build_request(z906_cmd_id_t id,
                                     cmd_source_t source,
                                     int32_t param_val,
                                     cmd_request_t *out_req)
{
    if (out_req == NULL) {
        return false;
    }

    const z906_cmd_descriptor_t *desc = z906_cmd_registry_find_by_id(id);
    if (desc == NULL) {
        return false;
    }

    if (!z906_cmd_registry_validate_param(id, param_val)) {
        return false;
    }

    memset(out_req, 0, sizeof(*out_req));
    out_req->type = desc->verify_type;
    out_req->source = source;

    switch (desc->id) {
        case Z906_CMD_ID_POWER:
            out_req->param.power = (param_val != 0);
            break;

        case Z906_CMD_ID_VOLUME:
            out_req->param.volume = (uint8_t)param_val;
            break;

        case Z906_CMD_ID_MUTE:
            out_req->param.mute = (param_val != 0);
            break;

        case Z906_CMD_ID_INPUT:
            out_req->param.input = (z906_input_t)param_val;
            break;

        case Z906_CMD_ID_EFFECT:
            out_req->param.effect = (z906_effect_t)param_val;
            break;

        case Z906_CMD_ID_TRIM_BASS:
        case Z906_CMD_ID_TRIM_CENTER:
        case Z906_CMD_ID_TRIM_REAR:
            out_req->param.trim.channel = desc->trim_channel;
            out_req->param.trim.level = (uint8_t)param_val;
            break;

        default:
            return false;
    }

    return true;
}

const char *z906_cmd_registry_id_to_str(z906_cmd_id_t id)
{
    const z906_cmd_descriptor_t *desc = z906_cmd_registry_find_by_id(id);
    if (desc != NULL) {
        return desc->name;
    }
    return "unknown";
}
