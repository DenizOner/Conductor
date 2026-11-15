/**
 * @file zigbee_device_model.c
 * @brief Native ESP Zigbee SDK v2 descriptor tree and attribute data model implementation.
 *
 * Implements the complete native Zigbee device descriptor hierarchy:
 *   - 1 device descriptor (ezb_af_device_desc_t)
 *   - EP 1  (Main): Basic (Server), Identify (Server), On/Off (Server), Level Control (Server), OTA Upgrade (Client)
 *   - EP 2  (Trims): Custom Cluster 0xFC01 (Server: trim_bass, trim_center, trim_rear, mute)
 *   - EP 3  (Sources): Custom Cluster 0xFC02 (Server: input, effect)
 *   - EP 4  (Telemetry): Custom Cluster 0xFC03 (Server: telemetry, feature toggles & selective health diagnostics)
 *   - EP 11 (Temp 1): Standard Temperature Measurement 0x0402 (Server: measuredValue = 0x8000 sentinel)
 *   - EP 12 (Temp 2): Standard Temperature Measurement 0x0402 (Server: measuredValue = 0x8000 sentinel)
 *   - EP 13 (Temp 3): Standard Temperature Measurement 0x0402 (Server: measuredValue = 0x8000 sentinel)
 *
 * Lifecycle and Storage Guarantees:
 *   - All attribute values reside in static, module-lifetime variables.
 *   - Values are initial descriptor defaults/sentinels; authoritative hardware state
 *     remains in z906_state and will be synchronized in Phase 5.
 *   - No incoming write dispatch or hardware command execution is implemented here (Phase 4).
 *   - No ZCL reporting or network steering is triggered here (Phase 5).
 */

#include "zigbee_device_model.h"
#include <string.h>
#include "esp_log.h"
#include "config.h"
#include "clusters/zcl_clusters.h"
#include "z906_state.h"
#include "led_notification_manager.h"
#include "z906_cmd_verifier.h"
#include <ezbee/af.h>
#include <ezbee/zha.h>
#include <ezbee/zcl/zcl_type.h>
#include <ezbee/zcl/zcl_desc.h>
#include <ezbee/zcl/cluster/basic_desc.h>
#include <ezbee/zcl/cluster/identify_desc.h>
#include <ezbee/zcl/cluster/on_off_desc.h>
#include <ezbee/zcl/cluster/level_desc.h>
#include <ezbee/zcl/cluster/ota_upgrade_desc.h>
#include <ezbee/zcl/cluster/temperature_measurement_desc.h>
#include <ezbee/zcl/cluster/custom.h>

static const char *TAG = "ZB_DEV_MODEL";

/* ========================================================================= */
/* Static Attribute Backing Storage (Module Lifetime)                        */
/* ========================================================================= */

/* Endpoint 1 — Basic Cluster Static Backing Storage */
/* Zigbee Character String format: Byte 0 = length, following bytes = ASCII */
#define Z906_MANUF_NAME_STR  Z906_ZB_MANUFACTURER_NAME /* "Logitech" (8 chars) */
#define Z906_MODEL_ID_STR    Z906_ZB_MODEL_IDENTIFIER  /* "Z-906-Zigbee" (12 chars) */

#define Z906_MANUF_NAME_LEN  (sizeof(Z906_MANUF_NAME_STR) - 1)
#define Z906_MODEL_ID_LEN    (sizeof(Z906_MODEL_ID_STR) - 1)

static const uint8_t s_basic_manufacturer_name[1 + Z906_MANUF_NAME_LEN] = {
    (uint8_t)Z906_MANUF_NAME_LEN,
    'L', 'o', 'g', 'i', 't', 'e', 'c', 'h'
};

static const uint8_t s_basic_model_identifier[1 + Z906_MODEL_ID_LEN] = {
    (uint8_t)Z906_MODEL_ID_LEN,
    'Z', '-', '9', '0', '6', '-', 'Z', 'i', 'g', 'b', 'e', 'e'
};

/* Compile-time static assertions guaranteeing length byte matches visible string length */
_Static_assert(sizeof(s_basic_manufacturer_name) == 1 + 8, "ManufacturerName length mismatch!");
_Static_assert(Z906_MANUF_NAME_LEN == 8, "ManufacturerName length must be exactly 8 (0x08)!");

_Static_assert(sizeof(s_basic_model_identifier) == 1 + 12, "ModelIdentifier length mismatch!");
_Static_assert(Z906_MODEL_ID_LEN == 12, "ModelIdentifier length must be exactly 12 (0x0C)!");

/* Endpoint 1 — On/Off & Level Control Initial Backing Values */
/* Note: Initial descriptor values. Authoritative hardware state is z906_state. */
static bool     s_ep1_on_off_val             = false;
static uint8_t  s_ep1_current_level_val      = 0;
static uint32_t s_ep1_current_file_version   = Z906_OTA_FILE_VERSION;

/* Endpoint 2 — Custom Cluster 0xFC01 (Trims & Mute) Backing Storage */
static uint8_t s_ep2_trim_bass_val    = 0;
static uint8_t s_ep2_trim_center_val  = 0;
static uint8_t s_ep2_trim_rear_val    = 0;
static uint8_t s_ep2_mute_val         = 0;

/* Endpoint 3 — Custom Cluster 0xFC02 (Sources & Effects) Backing Storage */
static uint8_t s_ep3_input_val        = 1; /* Default: 6-Channel Direct (Input 1) */
static uint8_t s_ep3_effect_val       = 0; /* Default: 3D */

/* Endpoint 4 — Custom Cluster 0xFC03 (Telemetry & Selective Diagnostics) */
/* Strictly non-amplifier temperature attributes (amplifiers are on EP 11/12/13). */
/* input_signal_level uses 3-byte backing buffer matching ZCL DATA24 wire size (3 bytes) */
static uint8_t  s_ep4_input_signal_level_val[EZB_ZCL_UINT24_SIZE] = { 0, 0, 0 };
static uint32_t s_ep4_idle_time_val          = 0; /* U24BE/DATA32 idle seconds */
static uint8_t  s_ep4_digital_signal_status  = 0; /* Byte 12 decode state code */
static uint8_t  s_ep4_auto_standby           = 0; /* Read-only hardware telemetry */
static uint8_t  s_ep4_built_in_led_enabled   = 1; /* Default: enabled */
static uint8_t  s_ep4_cmd_verifier_enabled   = 1; /* Default: enabled */

/* Health Manager Selective Diagnostics Backing Storage */
static uint8_t  s_ep4_device_health          = 0; /* Default: 0 (unknown) */
static uint8_t  s_ep4_health_primary_comp    = 0; /* Default: 0 (system) */
static uint8_t  s_ep4_health_primary_reas    = 0; /* Default: 0 (none) */
static int16_t  s_ep4_soc_temperature        = ZCL_TEMP_INVALID_VALUE; /* Default: 0x8000 sentinel */

/* Endpoints 11, 12, 13 — Standard Temperature Measurement (0x0402) */
/* Initial sentinel: 0x8000 (-32768) represents "unavailable / invalid" per ZCL spec */
static int16_t s_ep11_measured_temp_val = ZCL_TEMP_INVALID_VALUE;
static int16_t s_ep12_measured_temp_val = ZCL_TEMP_INVALID_VALUE;
static int16_t s_ep13_measured_temp_val = ZCL_TEMP_INVALID_VALUE;

/* Registration Guard & Retained Descriptor Handle */
static bool s_device_model_registered = false;
static ezb_af_device_desc_t s_device_desc = EZB_INVALID_AF_DEVICE_DESC;

/* ========================================================================= */
/* Helper Functions for Cluster & Endpoint Construction                      */
/* ========================================================================= */

/**
 * @brief Constructs Endpoint 1 (Main Control, Identity & OTA Upgrade Client).
 */
static ezb_af_ep_desc_t build_endpoint_1(void)
{
    ezb_af_ep_config_t ep_cfg = {
        .ep_id              = Z906_ZB_EP_MAIN,
        .app_profile_id     = EZB_AF_HA_PROFILE_ID,
        .app_device_id      = EZB_ZHA_ON_OFF_OUTPUT_DEVICE_ID,
        .app_device_version = 0,
        .reserved           = 0,
    };

    ezb_af_ep_desc_t ep_desc = ezb_af_create_endpoint_desc(&ep_cfg);
    if (ep_desc == EZB_INVALID_AF_EP_DESC) {
        ESP_LOGE(TAG, "Failed to create endpoint descriptor for EP 1");
        return EZB_INVALID_AF_EP_DESC;
    }

    /* 1. Basic Cluster (0x0000) — Server */
    ezb_zcl_basic_cluster_server_config_t basic_cfg = {
        .zcl_version  = EZB_ZCL_BASIC_ZCL_VERSION_DEFAULT_VALUE,
        .power_source = EZB_ZCL_BASIC_POWER_SOURCE_SINGLE_PHASE_MAINS,
    };
    ezb_zcl_cluster_desc_t basic_cluster = ezb_zcl_basic_create_cluster_desc(&basic_cfg, EZB_ZCL_CLUSTER_SERVER);
    if (basic_cluster == EZB_INVALID_ZCL_CLUSTER_DESC) {
        ESP_LOGE(TAG, "Failed to create Basic cluster descriptor on EP 1");
        ezb_af_free_endpoint_desc(ep_desc);
        return EZB_INVALID_AF_EP_DESC;
    }
    ezb_zcl_basic_cluster_desc_add_attr(basic_cluster, EZB_ZCL_ATTR_BASIC_MANUFACTURER_NAME_ID, s_basic_manufacturer_name);
    ezb_zcl_basic_cluster_desc_add_attr(basic_cluster, EZB_ZCL_ATTR_BASIC_MODEL_IDENTIFIER_ID, s_basic_model_identifier);
    ezb_af_endpoint_add_cluster_desc(ep_desc, basic_cluster);

    /* 2. Identify Cluster (0x0003) — Server */
    ezb_zcl_identify_cluster_server_config_t identify_cfg = {
        .identify_time = EZB_ZCL_IDENTIFY_IDENTIFY_TIME_DEFAULT_VALUE,
    };
    ezb_zcl_cluster_desc_t identify_cluster = ezb_zcl_identify_create_cluster_desc(&identify_cfg, EZB_ZCL_CLUSTER_SERVER);
    if (identify_cluster == EZB_INVALID_ZCL_CLUSTER_DESC) {
        ESP_LOGE(TAG, "Failed to create Identify cluster descriptor on EP 1");
        ezb_af_free_endpoint_desc(ep_desc);
        return EZB_INVALID_AF_EP_DESC;
    }
    ezb_af_endpoint_add_cluster_desc(ep_desc, identify_cluster);

    /* 3. On/Off Cluster (0x0006) — Server */
    ezb_zcl_on_off_cluster_server_config_t on_off_cfg = {
        .on_off = s_ep1_on_off_val,
    };
    ezb_zcl_cluster_desc_t on_off_cluster = ezb_zcl_on_off_create_cluster_desc(&on_off_cfg, EZB_ZCL_CLUSTER_SERVER);
    if (on_off_cluster == EZB_INVALID_ZCL_CLUSTER_DESC) {
        ESP_LOGE(TAG, "Failed to create On/Off cluster descriptor on EP 1");
        ezb_af_free_endpoint_desc(ep_desc);
        return EZB_INVALID_AF_EP_DESC;
    }
    ezb_af_endpoint_add_cluster_desc(ep_desc, on_off_cluster);

    /* 4. Level Control Cluster (0x0008) — Server */
    ezb_zcl_level_cluster_server_config_t level_cfg = {
        .current_level = s_ep1_current_level_val,
    };
    ezb_zcl_cluster_desc_t level_cluster = ezb_zcl_level_create_cluster_desc(&level_cfg, EZB_ZCL_CLUSTER_SERVER);
    if (level_cluster == EZB_INVALID_ZCL_CLUSTER_DESC) {
        ESP_LOGE(TAG, "Failed to create Level Control cluster descriptor on EP 1");
        ezb_af_free_endpoint_desc(ep_desc);
        return EZB_INVALID_AF_EP_DESC;
    }
    ezb_af_endpoint_add_cluster_desc(ep_desc, level_cluster);

    /* 5. OTA Upgrade Cluster (0x0019) — Client */
    ezb_zcl_ota_upgrade_cluster_client_config_t ota_cfg = {
        .upgrade_server_id    = EZB_ZCL_OTA_UPGRADE_UPGRADE_SERVER_ID_DEFAULT_VALUE,
        .file_offset          = EZB_ZCL_OTA_UPGRADE_FILE_OFFSET_DEFAULT_VALUE,
        .image_upgrade_status = EZB_ZCL_OTA_UPGRADE_IMAGE_UPGRADE_STATUS_DEFAULT_VALUE,
        .manufacturer_id      = EZB_ZCL_ESP_MANUF_CODE, /* 0x131B: Espressif manufacturer code used by current OTA metadata */
        .image_type_id        = 0x0906,                 /* Logitech Z906 Image Type ID */
    };
    ezb_zcl_cluster_desc_t ota_cluster = ezb_zcl_ota_upgrade_create_cluster_desc(&ota_cfg, EZB_ZCL_CLUSTER_CLIENT);
    if (ota_cluster == EZB_INVALID_ZCL_CLUSTER_DESC) {
        ESP_LOGE(TAG, "Failed to create OTA Upgrade client cluster descriptor on EP 1");
        ezb_af_free_endpoint_desc(ep_desc);
        return EZB_INVALID_AF_EP_DESC;
    }
    ezb_zcl_ota_upgrade_cluster_desc_add_attr(ota_cluster,
                                              EZB_ZCL_ATTR_OTA_UPGRADE_CURRENT_FILE_VERSION_ID,
                                              &s_ep1_current_file_version);
    ezb_af_endpoint_add_cluster_desc(ep_desc, ota_cluster);

    return ep_desc;
}

/**
 * @brief Constructs Endpoint 2 (Custom Cluster 0xFC01 — Trims & Mute).
 */
static ezb_af_ep_desc_t build_endpoint_2(void)
{
    ezb_af_ep_config_t ep_cfg = {
        .ep_id              = Z906_ZB_EP_TRIMS,
        .app_profile_id     = EZB_AF_HA_PROFILE_ID,
        .app_device_id      = EZB_ZHA_CUSTOM_GATEWAY_DEVICE_ID,
        .app_device_version = 0,
        .reserved           = 0,
    };

    ezb_af_ep_desc_t ep_desc = ezb_af_create_endpoint_desc(&ep_cfg);
    if (ep_desc == EZB_INVALID_AF_EP_DESC) {
        ESP_LOGE(TAG, "Failed to create endpoint descriptor for EP 2");
        return EZB_INVALID_AF_EP_DESC;
    }

    ezb_zcl_custom_cluster_config_t custom_cfg = {
        .cluster_id  = ZCL_CLUSTER_Z906_TRIMS,
        .init_func   = NULL,
        .deinit_func = NULL,
    };
    ezb_zcl_cluster_desc_t cluster_desc = ezb_zcl_custom_create_cluster_desc(&custom_cfg, EZB_ZCL_CLUSTER_SERVER);
    if (cluster_desc == EZB_INVALID_ZCL_CLUSTER_DESC) {
        ESP_LOGE(TAG, "Failed to create custom cluster 0xFC01 descriptor on EP 2");
        ezb_af_free_endpoint_desc(ep_desc);
        return EZB_INVALID_AF_EP_DESC;
    }

    const uint8_t rw_rep = EZB_ZCL_ATTR_ACCESS_READ | EZB_ZCL_ATTR_ACCESS_WRITE | EZB_ZCL_ATTR_ACCESS_REPORTING;

    ezb_zcl_custom_cluster_desc_add_attr(cluster_desc, ZCL_ATTR_TRIM_BASS,
                                         EZB_ZCL_ATTR_TYPE_UINT8, rw_rep, &s_ep2_trim_bass_val);
    ezb_zcl_custom_cluster_desc_add_attr(cluster_desc, ZCL_ATTR_TRIM_CENTER,
                                         EZB_ZCL_ATTR_TYPE_UINT8, rw_rep, &s_ep2_trim_center_val);
    ezb_zcl_custom_cluster_desc_add_attr(cluster_desc, ZCL_ATTR_TRIM_REAR,
                                         EZB_ZCL_ATTR_TYPE_UINT8, rw_rep, &s_ep2_trim_rear_val);
    ezb_zcl_custom_cluster_desc_add_attr(cluster_desc, ZCL_ATTR_AUDIO_MUTE,
                                         EZB_ZCL_ATTR_TYPE_UINT8, rw_rep, &s_ep2_mute_val);

    ezb_af_endpoint_add_cluster_desc(ep_desc, cluster_desc);
    return ep_desc;
}

/**
 * @brief Constructs Endpoint 3 (Custom Cluster 0xFC02 — Sources & Effects).
 */
static ezb_af_ep_desc_t build_endpoint_3(void)
{
    ezb_af_ep_config_t ep_cfg = {
        .ep_id              = Z906_ZB_EP_SOURCES,
        .app_profile_id     = EZB_AF_HA_PROFILE_ID,
        .app_device_id      = EZB_ZHA_CUSTOM_GATEWAY_DEVICE_ID,
        .app_device_version = 0,
        .reserved           = 0,
    };

    ezb_af_ep_desc_t ep_desc = ezb_af_create_endpoint_desc(&ep_cfg);
    if (ep_desc == EZB_INVALID_AF_EP_DESC) {
        ESP_LOGE(TAG, "Failed to create endpoint descriptor for EP 3");
        return EZB_INVALID_AF_EP_DESC;
    }

    ezb_zcl_custom_cluster_config_t custom_cfg = {
        .cluster_id  = ZCL_CLUSTER_Z906_SOURCES,
        .init_func   = NULL,
        .deinit_func = NULL,
    };
    ezb_zcl_cluster_desc_t cluster_desc = ezb_zcl_custom_create_cluster_desc(&custom_cfg, EZB_ZCL_CLUSTER_SERVER);
    if (cluster_desc == EZB_INVALID_ZCL_CLUSTER_DESC) {
        ESP_LOGE(TAG, "Failed to create custom cluster 0xFC02 descriptor on EP 3");
        ezb_af_free_endpoint_desc(ep_desc);
        return EZB_INVALID_AF_EP_DESC;
    }

    const uint8_t rw_rep = EZB_ZCL_ATTR_ACCESS_READ | EZB_ZCL_ATTR_ACCESS_WRITE | EZB_ZCL_ATTR_ACCESS_REPORTING;

    ezb_zcl_custom_cluster_desc_add_attr(cluster_desc, ZCL_ATTR_SOURCE_INPUT,
                                         EZB_ZCL_ATTR_TYPE_UINT8, rw_rep, &s_ep3_input_val);
    ezb_zcl_custom_cluster_desc_add_attr(cluster_desc, ZCL_ATTR_SOURCE_EFFECT,
                                         EZB_ZCL_ATTR_TYPE_UINT8, rw_rep, &s_ep3_effect_val);

    ezb_af_endpoint_add_cluster_desc(ep_desc, cluster_desc);
    return ep_desc;
}

/**
 * @brief Constructs Endpoint 4 (Custom Cluster 0xFC03 — Telemetry, Feature Toggles & Selective Diagnostics).
 */
static ezb_af_ep_desc_t build_endpoint_4(void)
{
    ezb_af_ep_config_t ep_cfg = {
        .ep_id              = Z906_ZB_EP_TELEMETRY,
        .app_profile_id     = EZB_AF_HA_PROFILE_ID,
        .app_device_id      = EZB_ZHA_CUSTOM_GATEWAY_DEVICE_ID,
        .app_device_version = 0,
        .reserved           = 0,
    };

    ezb_af_ep_desc_t ep_desc = ezb_af_create_endpoint_desc(&ep_cfg);
    if (ep_desc == EZB_INVALID_AF_EP_DESC) {
        ESP_LOGE(TAG, "Failed to create endpoint descriptor for EP 4");
        return EZB_INVALID_AF_EP_DESC;
    }

    ezb_zcl_custom_cluster_config_t custom_cfg = {
        .cluster_id  = ZCL_CLUSTER_Z906_TELEMETRY,
        .init_func   = NULL,
        .deinit_func = NULL,
    };
    ezb_zcl_cluster_desc_t cluster_desc = ezb_zcl_custom_create_cluster_desc(&custom_cfg, EZB_ZCL_CLUSTER_SERVER);
    if (cluster_desc == EZB_INVALID_ZCL_CLUSTER_DESC) {
        ESP_LOGE(TAG, "Failed to create custom cluster 0xFC03 descriptor on EP 4");
        ezb_af_free_endpoint_desc(ep_desc);
        return EZB_INVALID_AF_EP_DESC;
    }

    const uint8_t ro_rep = EZB_ZCL_ATTR_ACCESS_READ | EZB_ZCL_ATTR_ACCESS_REPORTING;
    const uint8_t rw_rep = EZB_ZCL_ATTR_ACCESS_READ | EZB_ZCL_ATTR_ACCESS_WRITE | EZB_ZCL_ATTR_ACCESS_REPORTING;

    /* 0x0000: input_signal_level (U24BE wire format / DATA24 3-byte backing) - Telemetry Read-Only */
    ezb_zcl_custom_cluster_desc_add_attr(cluster_desc, ZCL_ATTR_INPUT_SIGNAL_LEVEL,
                                         EZB_ZCL_ATTR_TYPE_DATA24, ro_rep, s_ep4_input_signal_level_val);

    /* 0x0001: idle_time (seconds / DATA32) - Telemetry Read-Only */
    ezb_zcl_custom_cluster_desc_add_attr(cluster_desc, ZCL_ATTR_IDLE_TIME,
                                         EZB_ZCL_ATTR_TYPE_DATA32, ro_rep, &s_ep4_idle_time_val);

    /* 0x0002: digital_signal_status (decode state / UINT8) - Telemetry Read-Only */
    ezb_zcl_custom_cluster_desc_add_attr(cluster_desc, ZCL_ATTR_DIGITAL_SIGNAL_STATUS,
                                         EZB_ZCL_ATTR_TYPE_UINT8, ro_rep, &s_ep4_digital_signal_status);

    /* 0x0003: auto_standby (bool / UINT8) - Telemetry Read-Only (no amplifier write command exists) */
    ezb_zcl_custom_cluster_desc_add_attr(cluster_desc, ZCL_ATTR_AUTO_STANDBY,
                                         EZB_ZCL_ATTR_TYPE_UINT8, ro_rep, &s_ep4_auto_standby);

    /* 0x0007: built_in_led_enabled (bool / UINT8) - Persistent Feature Toggle (Read/Write) */
    ezb_zcl_custom_cluster_desc_add_attr(cluster_desc, ZCL_ATTR_BUILT_IN_LED_ENABLED,
                                         EZB_ZCL_ATTR_TYPE_UINT8, rw_rep, &s_ep4_built_in_led_enabled);

    /* 0x0008: cmd_verifier_enabled (bool / UINT8) - Persistent Feature Toggle (Read/Write) */
    ezb_zcl_custom_cluster_desc_add_attr(cluster_desc, ZCL_ATTR_CMD_VERIFIER_ENABLED,
                                         EZB_ZCL_ATTR_TYPE_UINT8, rw_rep, &s_ep4_cmd_verifier_enabled);

    /* 0x0009: device_health (UINT8) - Selective Diagnostics (Read-Only) */
    ezb_zcl_custom_cluster_desc_add_attr(cluster_desc, ZCL_ATTR_DEVICE_HEALTH,
                                         EZB_ZCL_ATTR_TYPE_UINT8, ro_rep, &s_ep4_device_health);

    /* 0x000A: health_primary_component (UINT8) - Selective Diagnostics (Read-Only) */
    ezb_zcl_custom_cluster_desc_add_attr(cluster_desc, ZCL_ATTR_HEALTH_PRIMARY_COMPONENT,
                                         EZB_ZCL_ATTR_TYPE_UINT8, ro_rep, &s_ep4_health_primary_comp);

    /* 0x000B: health_primary_reason (UINT8) - Selective Diagnostics (Read-Only) */
    ezb_zcl_custom_cluster_desc_add_attr(cluster_desc, ZCL_ATTR_HEALTH_PRIMARY_REASON,
                                         EZB_ZCL_ATTR_TYPE_UINT8, ro_rep, &s_ep4_health_primary_reas);

    /* 0x000C: soc_temperature (INT16, °C × 100) - Selective Diagnostics (Read-Only) */
    ezb_zcl_custom_cluster_desc_add_attr(cluster_desc, ZCL_ATTR_SOC_TEMPERATURE,
                                         EZB_ZCL_ATTR_TYPE_INT16, ro_rep, &s_ep4_soc_temperature);

    ezb_af_endpoint_add_cluster_desc(ep_desc, cluster_desc);
    return ep_desc;
}

/**
 * @brief Constructs a standard Temperature Measurement sub-endpoint (Cluster 0x0402).
 *
 * @param ep_id Target endpoint ID (11, 12, or 13).
 * @param initial_measured_val Pointer to static backing storage holding initial sentinel.
 */
static ezb_af_ep_desc_t build_temperature_endpoint(uint8_t ep_id, int16_t *initial_measured_val)
{
    ezb_af_ep_config_t ep_cfg = {
        .ep_id              = ep_id,
        .app_profile_id     = EZB_AF_HA_PROFILE_ID,
        .app_device_id      = EZB_ZHA_TEMPERATURE_SENSOR_DEVICE_ID,
        .app_device_version = 0,
        .reserved           = 0,
    };

    ezb_af_ep_desc_t ep_desc = ezb_af_create_endpoint_desc(&ep_cfg);
    if (ep_desc == EZB_INVALID_AF_EP_DESC) {
        ESP_LOGE(TAG, "Failed to create endpoint descriptor for EP %u", ep_id);
        return EZB_INVALID_AF_EP_DESC;
    }

    ezb_zcl_temperature_measurement_cluster_server_config_t temp_cfg = {
        .measured_value     = *initial_measured_val,
        .min_measured_value = EZB_ZCL_TEMPERATURE_MEASUREMENT_MIN_MEASURED_VALUE_DEFAULT_VALUE,
        .max_measured_value = EZB_ZCL_TEMPERATURE_MEASUREMENT_MAX_MEASURED_VALUE_DEFAULT_VALUE,
    };

    ezb_zcl_cluster_desc_t cluster_desc = ezb_zcl_temperature_measurement_create_cluster_desc(&temp_cfg, EZB_ZCL_CLUSTER_SERVER);
    if (cluster_desc == EZB_INVALID_ZCL_CLUSTER_DESC) {
        ESP_LOGE(TAG, "Failed to create Temperature Measurement cluster descriptor on EP %u", ep_id);
        ezb_af_free_endpoint_desc(ep_desc);
        return EZB_INVALID_AF_EP_DESC;
    }

    ezb_af_endpoint_add_cluster_desc(ep_desc, cluster_desc);
    return ep_desc;
}

/* ========================================================================= */
/* Public Registration API                                                   */
/* ========================================================================= */

esp_err_t zigbee_device_model_register(void)
{
    if (s_device_model_registered) {
        ESP_LOGI(TAG, "Device model already registered (idempotent).");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Registering native Zigbee v2 device model (7 endpoints)...");

    /* 1. Create the single device descriptor */
    s_device_desc = ezb_af_create_device_desc();
    if (s_device_desc == EZB_INVALID_AF_DEVICE_DESC) {
        ESP_LOGE(TAG, "ezb_af_create_device_desc() failed.");
        return ESP_FAIL;
    }

    /* 2. Build and attach all 7 endpoints */
    ezb_af_ep_desc_t ep1 = build_endpoint_1();
    if (ep1 == EZB_INVALID_AF_EP_DESC) {
        ESP_LOGE(TAG, "Aborting device registration: EP 1 creation failed.");
        ezb_af_free_device_desc(s_device_desc);
        s_device_desc = EZB_INVALID_AF_DEVICE_DESC;
        return ESP_FAIL;
    }
    ezb_af_device_add_endpoint_desc(s_device_desc, ep1);

    ezb_af_ep_desc_t ep2 = build_endpoint_2();
    if (ep2 == EZB_INVALID_AF_EP_DESC) {
        ESP_LOGE(TAG, "Aborting device registration: EP 2 creation failed.");
        ezb_af_free_device_desc(s_device_desc);
        s_device_desc = EZB_INVALID_AF_DEVICE_DESC;
        return ESP_FAIL;
    }
    ezb_af_device_add_endpoint_desc(s_device_desc, ep2);

    ezb_af_ep_desc_t ep3 = build_endpoint_3();
    if (ep3 == EZB_INVALID_AF_EP_DESC) {
        ESP_LOGE(TAG, "Aborting device registration: EP 3 creation failed.");
        ezb_af_free_device_desc(s_device_desc);
        s_device_desc = EZB_INVALID_AF_DEVICE_DESC;
        return ESP_FAIL;
    }
    ezb_af_device_add_endpoint_desc(s_device_desc, ep3);

    ezb_af_ep_desc_t ep4 = build_endpoint_4();
    if (ep4 == EZB_INVALID_AF_EP_DESC) {
        ESP_LOGE(TAG, "Aborting device registration: EP 4 creation failed.");
        ezb_af_free_device_desc(s_device_desc);
        s_device_desc = EZB_INVALID_AF_DEVICE_DESC;
        return ESP_FAIL;
    }
    ezb_af_device_add_endpoint_desc(s_device_desc, ep4);

    ezb_af_ep_desc_t ep11 = build_temperature_endpoint(Z906_ZB_EP_TEMP1, &s_ep11_measured_temp_val);
    if (ep11 == EZB_INVALID_AF_EP_DESC) {
        ESP_LOGE(TAG, "Aborting device registration: EP 11 creation failed.");
        ezb_af_free_device_desc(s_device_desc);
        s_device_desc = EZB_INVALID_AF_DEVICE_DESC;
        return ESP_FAIL;
    }
    ezb_af_device_add_endpoint_desc(s_device_desc, ep11);

    ezb_af_ep_desc_t ep12 = build_temperature_endpoint(Z906_ZB_EP_TEMP2, &s_ep12_measured_temp_val);
    if (ep12 == EZB_INVALID_AF_EP_DESC) {
        ESP_LOGE(TAG, "Aborting device registration: EP 12 creation failed.");
        ezb_af_free_device_desc(s_device_desc);
        s_device_desc = EZB_INVALID_AF_DEVICE_DESC;
        return ESP_FAIL;
    }
    ezb_af_device_add_endpoint_desc(s_device_desc, ep12);

    ezb_af_ep_desc_t ep13 = build_temperature_endpoint(Z906_ZB_EP_TEMP3, &s_ep13_measured_temp_val);
    if (ep13 == EZB_INVALID_AF_EP_DESC) {
        ESP_LOGE(TAG, "Aborting device registration: EP 13 creation failed.");
        ezb_af_free_device_desc(s_device_desc);
        s_device_desc = EZB_INVALID_AF_DEVICE_DESC;
        return ESP_FAIL;
    }
    ezb_af_device_add_endpoint_desc(s_device_desc, ep13);

    /* 3. Register the device descriptor with the native stack */
    ezb_err_t ezb_err = ezb_af_device_desc_register(s_device_desc);
    if (ezb_err != EZB_ERR_NONE) {
        ESP_LOGE(TAG, "ezb_af_device_desc_register() failed with error: %d", (int)ezb_err);
        ezb_af_free_device_desc(s_device_desc);
        s_device_desc = EZB_INVALID_AF_DEVICE_DESC;
        return ESP_FAIL;
    }

    s_device_model_registered = true;
    ESP_LOGI(TAG, "Native Zigbee v2 device model registered successfully (EPs: 1, 2, 3, 4, 11, 12, 13).");
    return ESP_OK;
}

bool zigbee_device_model_is_registered(void)
{
    return s_device_model_registered;
}

ezb_af_device_desc_t zigbee_device_model_get_device_desc(void)
{
    return s_device_desc;
}

void zigbee_device_model_restore_attr(uint8_t ep_id, uint16_t cluster_id, uint16_t attr_id)
{
    z906_state_t st;
    z906_state_get(&st);

    if (ep_id == Z906_ZB_EP_MAIN) {
        if (cluster_id == ZCL_CLUSTER_ON_OFF && attr_id == 0x0000) {
            s_ep1_on_off_val = st.power;
        } else if (cluster_id == ZCL_CLUSTER_LEVEL_CONTROL && attr_id == 0x0000) {
            s_ep1_current_level_val = z906_cmd_volume_to_zcl(st.volume);
        }
    } else if (ep_id == Z906_ZB_EP_TRIMS && cluster_id == ZCL_CLUSTER_Z906_TRIMS) {
        switch (attr_id) {
            case ZCL_ATTR_TRIM_BASS:
                s_ep2_trim_bass_val = z906_cmd_volume_to_zcl(st.volume_bass);
                break;
            case ZCL_ATTR_TRIM_CENTER:
                s_ep2_trim_center_val = z906_cmd_volume_to_zcl(st.volume_center);
                break;
            case ZCL_ATTR_TRIM_REAR:
                s_ep2_trim_rear_val = z906_cmd_volume_to_zcl(st.volume_rear);
                break;
            case ZCL_ATTR_AUDIO_MUTE:
                s_ep2_mute_val = st.mute ? 1 : 0;
                break;
            default:
                break;
        }
    } else if (ep_id == Z906_ZB_EP_SOURCES && cluster_id == ZCL_CLUSTER_Z906_SOURCES) {
        switch (attr_id) {
            case ZCL_ATTR_SOURCE_INPUT:
                s_ep3_input_val = (uint8_t)st.input;
                break;
            case ZCL_ATTR_SOURCE_EFFECT:
                s_ep3_effect_val = (uint8_t)st.effect;
                break;
            default:
                break;
        }
    } else if (ep_id == Z906_ZB_EP_TELEMETRY && cluster_id == ZCL_CLUSTER_Z906_TELEMETRY) {
        switch (attr_id) {
            case ZCL_ATTR_BUILT_IN_LED_ENABLED:
                s_ep4_built_in_led_enabled = led_notification_is_enabled() ? 1 : 0;
                break;
            case ZCL_ATTR_CMD_VERIFIER_ENABLED:
                s_ep4_cmd_verifier_enabled = z906_cmd_verifier_is_enabled() ? 1 : 0;
                break;
            default:
                break;
        }
    }
}

ezb_zcl_attr_desc_t zigbee_device_model_get_attr_desc(uint8_t ep_id, uint16_t cluster_id, uint16_t attr_id)
{
    if (!s_device_model_registered || s_device_desc == EZB_INVALID_AF_DEVICE_DESC) {
        return EZB_INVALID_ZCL_ATTR_DESC;
    }

    return ezb_zcl_get_attr_desc(ep_id, cluster_id, EZB_ZCL_CLUSTER_SERVER, attr_id, EZB_ZCL_STD_MANUF_CODE);
}
