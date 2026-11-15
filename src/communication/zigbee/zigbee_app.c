/**
 * @file zigbee_app.c
 * @brief Implementation of Native Zigbee 3.0 router lifecycle, pairing management, and attribute reporting.
 */

#include "zigbee_app.h"
#include "config.h"
#include "z906_commands.h"
#include "zcl_clusters.h"
#include "nvs_manager.h"
#include "led_notification_manager.h"
#include "z906_cmd_verifier.h"
#include "zigbee_state_publisher.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

/** @brief Logging tag for Zigbee application operations */
static const char *TAG = "ZIGBEE_APP";

/** @brief Current active Zigbee network commissioning and connectivity state */
static zb_network_state_t s_state = ZB_STATE_UNPAIRED;

/** @brief Cached persistent network parameters (PAN ID, Channel, EUI-64) */
static zb_persistent_net_info_t s_net_info = { 0 };

/** @brief Countdown timer in seconds for network pairing/steering window */
static int s_pairing_timer_s = 0;

/** @brief Elapsed timer in seconds for periodic rejoin attempts */
static int s_rejoin_timer_s = 0;

/** @brief Last reported state for delta detection */
static z906_state_t s_reported_state = { 0 };
static bool s_has_reported = false;
static uint32_t s_report_count = 0;

bool zigbee_app_init(void)
{
    ESP_LOGI(TAG, "Initializing Native Zigbee 3.0 stack as Router (Device: %s %s)",
             Z906_ZB_MANUFACTURER_NAME, Z906_ZB_MODEL_IDENTIFIER);

    /* Check NVS to determine if device was previously paired to a Zigbee network */
    bool was_paired = false;
    if (nvs_manager_get_zb_paired(&was_paired) && was_paired) {
        /* Load stored network credentials from NVS */
        if (nvs_manager_load_zb_net_info(&s_net_info)) {
            s_state = ZB_STATE_REJOINING;
            s_rejoin_timer_s = 0;
            ESP_LOGI(TAG, "Rebooted with existing pairing: PAN=0x%04X, Channel=%d. Entering REJOINING state.",
                     s_net_info.pan_id, s_net_info.channel);
            return true;
        }
    }

    /* No valid previous pairing found; start in UNPAIRED idle state */
    s_state = ZB_STATE_UNPAIRED;
    memset(&s_net_info, 0, sizeof(s_net_info));
    ESP_LOGI(TAG, "No persistent Zigbee network found. Device ready in UNPAIRED state.");
    return true;
}

bool zigbee_app_start(void)
{
    if (s_state == ZB_STATE_REJOINING) {
        ESP_LOGI(TAG, "Zigbee stack active: Initiating automatic rejoin to PAN 0x%04X...", s_net_info.pan_id);
    } else {
        ESP_LOGI(TAG, "Zigbee stack active: Idle, waiting for commissioning/pairing command.");
    }
    return true;
}

zb_network_state_t zigbee_app_get_state(void)
{
    return s_state;
}

const char *zigbee_app_state_str(zb_network_state_t state)
{
    switch (state) {
        case ZB_STATE_UNPAIRED:          return "UNPAIRED";
        case ZB_STATE_PAIRING:           return "PAIRING";
        case ZB_STATE_JOINED:            return "JOINED";
        case ZB_STATE_REJOINING:         return "REJOINING";
        case ZB_STATE_COMMISSION_FAILED: return "COMMISSION_FAILED";
        case ZB_STATE_LEAVING:           return "LEAVING";
        default:                         return "UNKNOWN";
    }
}

bool zigbee_app_start_pairing(bool force_repair)
{
    if ((s_state == ZB_STATE_JOINED || s_state == ZB_STATE_REJOINING) && !force_repair) {
        ESP_LOGW(TAG, "Device already possesses valid network credentials. Use force_repair to clear.");
        return false;
    }

    if (force_repair) {
        ESP_LOGI(TAG, "Force re-pairing requested: Clearing persistent Zigbee credentials from NVS.");
        nvs_manager_clear_zb_net_info();
        memset(&s_net_info, 0, sizeof(s_net_info));
    }

    s_state = ZB_STATE_PAIRING;
    s_pairing_timer_s = Z906_ZB_PAIRING_TIMEOUT_S;
    ESP_LOGI(TAG, "Opened Zigbee network steering window for %d seconds. Waiting for coordinator...",
             Z906_ZB_PAIRING_TIMEOUT_S);
    return true;
}

bool zigbee_app_leave_network(void)
{
    ESP_LOGI(TAG, "Executing clean network leave and resetting persistent credentials.");

    s_state = ZB_STATE_LEAVING;
    nvs_manager_clear_zb_net_info();
    memset(&s_net_info, 0, sizeof(s_net_info));

    s_pairing_timer_s = 0;
    s_rejoin_timer_s = 0;
    s_state = ZB_STATE_UNPAIRED;

    ESP_LOGI(TAG, "Network leave complete. Device in UNPAIRED state.");
    return true;
}

void zigbee_app_process_cycle(void)
{
    switch (s_state) {
        case ZB_STATE_PAIRING:
            if (s_pairing_timer_s > 0) {
                s_pairing_timer_s--;
                if (s_pairing_timer_s % 30 == 0 || s_pairing_timer_s <= 5) {
                    ESP_LOGI(TAG, "Zigbee pairing window active: %d seconds remaining", s_pairing_timer_s);
                }
            }
            /* Transition immediately when timer reaches zero — same cycle as last decrement.
             * WHY: avoids an off-by-one where the caller must drive one extra cycle to see
             * the UNPAIRED state after the timeout expires. */
            if (s_pairing_timer_s == 0) {
                ESP_LOGW(TAG, "Zigbee pairing window timed out. No coordinator found. Returning to UNPAIRED.");
                s_state = ZB_STATE_UNPAIRED;
            }
            break;


        case ZB_STATE_REJOINING:
            s_rejoin_timer_s++;
            if (s_rejoin_timer_s >= Z906_ZB_REJOIN_INTERVAL_S) {
                s_rejoin_timer_s = 0;
                ESP_LOGI(TAG, "Attempting non-blocking rejoin to PAN 0x%04X on Channel %d...",
                         s_net_info.pan_id, s_net_info.channel);
            }
            break;

        case ZB_STATE_JOINED:
        case ZB_STATE_UNPAIRED:
        case ZB_STATE_COMMISSION_FAILED:
        case ZB_STATE_LEAVING:
        default:
            break;
    }
}

bool zigbee_app_is_joined(void)
{
    return (s_state == ZB_STATE_JOINED);
}

bool zigbee_app_is_paired(void)
{
    return (s_state == ZB_STATE_JOINED || s_state == ZB_STATE_REJOINING);
}

bool zigbee_app_get_net_info(zb_persistent_net_info_t *out_info)
{
    if (out_info == NULL) {
        return false;
    }

    if (s_state != ZB_STATE_JOINED && s_state != ZB_STATE_REJOINING) {
        return false;
    }

    memcpy(out_info, &s_net_info, sizeof(zb_persistent_net_info_t));
    return true;
}

int zigbee_app_get_pairing_time_left(void)
{
    return (s_state == ZB_STATE_PAIRING) ? s_pairing_timer_s : 0;
}

static bool has_state_changed(const z906_state_t *a, const z906_state_t *b)
{
    if (a->power != b->power ||
        a->mute != b->mute ||
        a->volume != b->volume ||
        a->volume_bass != b->volume_bass ||
        a->volume_center != b->volume_center ||
        a->volume_rear != b->volume_rear ||
        a->input != b->input ||
        a->effect != b->effect ||
        a->digital_signal_status != b->digital_signal_status ||
        a->auto_standby_enabled != b->auto_standby_enabled ||
        /* Only report temperature changes after the first integrity-verified measurement arrives.
         * This prevents the boot-time temp_valid=false / 0 °C state from being broadcast
         * to Home Assistant as if it were a real sensor reading. */
        (a->temp_valid && b->temp_valid &&
            (a->temp_celsius[0] != b->temp_celsius[0] ||
             a->temp_celsius[1] != b->temp_celsius[1] ||
             a->temp_celsius[2] != b->temp_celsius[2])) ||
        /* Force a report when temp_valid transitions false → true for the first time */
        (a->temp_valid != b->temp_valid) ||
        a->input_signal_level != b->input_signal_level ||
        a->idle_seconds != b->idle_seconds ||
        a->cable_connected != b->cable_connected ||
        a->is_operational != b->is_operational) {
        return true;
    }
    return false;
}

static int16_t s_reported_temps[3] = { ZCL_TEMP_INVALID_VALUE, ZCL_TEMP_INVALID_VALUE, ZCL_TEMP_INVALID_VALUE };

bool zigbee_app_report_zcl_attribute(const zcl_report_record_t *rec)
{
    if (rec == NULL || s_state != ZB_STATE_JOINED) {
        return false;
    }

#if defined(Z906_HAS_NATIVE_ZB_SDK) && (Z906_HAS_NATIVE_ZB_SDK == 1)
    /* Real ESP-Zigbee-SDK attribute write and report pipeline */
    esp_zb_lock_acquire(portMAX_DELAY);
    esp_err_t err = ESP_OK;

    if (rec->attr_type == 0x29) { /* int16_t */
        err = esp_zb_zcl_set_attribute_val(rec->endpoint, rec->cluster_id, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE,
                                            rec->attr_id, (void *)&rec->val_i16, false);
    } else {
        err = esp_zb_zcl_set_attribute_val(rec->endpoint, rec->cluster_id, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE,
                                            rec->attr_id, (void *)&rec->val_u32, false);
    }

    if (err == ESP_OK) {
        esp_zb_zcl_report_attr_cmd_t cmd_req = {
            .zcl_basic_cmd = {
                .dst_addr_u.addr_short = 0x0000,
                .dst_endpoint = 1,
                .src_endpoint = rec->endpoint,
            },
            .address_mode = ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT,
            .clusterID = rec->cluster_id,
            .attributeID = rec->attr_id,
            .cluster_role = ESP_ZB_ZCL_CLUSTER_SERVER_ROLE,
        };
        err = esp_zb_zcl_report_attr_cmd_req(&cmd_req);
    }
    esp_zb_lock_release();

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "ZCL Report failed (EP:%d, Clust:0x%04X, Attr:0x%04X, err:0x%x)",
                 rec->endpoint, rec->cluster_id, rec->attr_id, err);
        return false;
    }
#endif

    return true;
}

static void do_report_state(const z906_state_t *state)
{
    uint8_t zcl_volume = z906_cmd_volume_to_zcl(state->volume);
    uint8_t zcl_bass = z906_cmd_volume_to_zcl(state->volume_bass);
    uint8_t zcl_center = z906_cmd_volume_to_zcl(state->volume_center);
    uint8_t zcl_rear = z906_cmd_volume_to_zcl(state->volume_rear);

    /* 1. EP 1: Main (OnOff + LevelControl) */
    zcl_report_record_t rec_power = {
        .endpoint = Z906_ZB_EP_MAIN, .cluster_id = ZCL_CLUSTER_ON_OFF,
        .attr_id = 0x0000, .attr_type = 0x10, .val_u32 = state->power ? 1 : 0
    };
    zigbee_app_report_zcl_attribute(&rec_power);

    zcl_report_record_t rec_vol = {
        .endpoint = Z906_ZB_EP_MAIN, .cluster_id = ZCL_CLUSTER_LEVEL_CONTROL,
        .attr_id = 0x0000, .attr_type = 0x20, .val_u32 = zcl_volume
    };
    zigbee_app_report_zcl_attribute(&rec_vol);

    /* 2. EP 2: Trims & Mute (0xFC01) */
    zcl_report_record_t rec_mute = {
        .endpoint = Z906_ZB_EP_TRIMS, .cluster_id = ZCL_CLUSTER_Z906_TRIMS,
        .attr_id = ZCL_ATTR_AUDIO_MUTE, .attr_type = 0x10, .val_u32 = state->mute ? 1 : 0
    };
    zigbee_app_report_zcl_attribute(&rec_mute);

    zcl_report_record_t rec_bass = {
        .endpoint = Z906_ZB_EP_TRIMS, .cluster_id = ZCL_CLUSTER_Z906_TRIMS,
        .attr_id = ZCL_ATTR_TRIM_BASS, .attr_type = 0x20, .val_u32 = zcl_bass
    };
    zigbee_app_report_zcl_attribute(&rec_bass);

    zcl_report_record_t rec_center = {
        .endpoint = Z906_ZB_EP_TRIMS, .cluster_id = ZCL_CLUSTER_Z906_TRIMS,
        .attr_id = ZCL_ATTR_TRIM_CENTER, .attr_type = 0x20, .val_u32 = zcl_center
    };
    zigbee_app_report_zcl_attribute(&rec_center);

    zcl_report_record_t rec_rear = {
        .endpoint = Z906_ZB_EP_TRIMS, .cluster_id = ZCL_CLUSTER_Z906_TRIMS,
        .attr_id = ZCL_ATTR_TRIM_REAR, .attr_type = 0x20, .val_u32 = zcl_rear
    };
    zigbee_app_report_zcl_attribute(&rec_rear);

    /* 3. EP 3: Sources (0xFC02) */
    zcl_report_record_t rec_input = {
        .endpoint = Z906_ZB_EP_SOURCES, .cluster_id = ZCL_CLUSTER_Z906_SOURCES,
        .attr_id = ZCL_ATTR_SOURCE_INPUT, .attr_type = 0x20, .val_u32 = (uint32_t)state->input
    };
    zigbee_app_report_zcl_attribute(&rec_input);

    zcl_report_record_t rec_effect = {
        .endpoint = Z906_ZB_EP_SOURCES, .cluster_id = ZCL_CLUSTER_Z906_SOURCES,
        .attr_id = ZCL_ATTR_SOURCE_EFFECT, .attr_type = 0x20, .val_u32 = (uint32_t)state->effect
    };
    zigbee_app_report_zcl_attribute(&rec_effect);

    /* 4. EP 4: Non-Temperature Telemetry (0xFC03) */
    zcl_report_record_t rec_signal = {
        .endpoint = Z906_ZB_EP_TELEMETRY, .cluster_id = ZCL_CLUSTER_Z906_TELEMETRY,
        .attr_id = ZCL_ATTR_INPUT_SIGNAL_LEVEL, .attr_type = 0x22, .val_u32 = state->input_signal_level
    };
    zigbee_app_report_zcl_attribute(&rec_signal);

    zcl_report_record_t rec_idle = {
        .endpoint = Z906_ZB_EP_TELEMETRY, .cluster_id = ZCL_CLUSTER_Z906_TELEMETRY,
        .attr_id = ZCL_ATTR_IDLE_TIME, .attr_type = 0x22, .val_u32 = state->idle_seconds
    };
    zigbee_app_report_zcl_attribute(&rec_idle);

    zcl_report_record_t rec_digital_sig = {
        .endpoint = Z906_ZB_EP_TELEMETRY, .cluster_id = ZCL_CLUSTER_Z906_TELEMETRY,
        .attr_id = ZCL_ATTR_DIGITAL_SIGNAL_STATUS, .attr_type = 0x20, .val_u32 = state->digital_signal_status
    };
    zigbee_app_report_zcl_attribute(&rec_digital_sig);

    zcl_report_record_t rec_auto_standby = {
        .endpoint = Z906_ZB_EP_TELEMETRY, .cluster_id = ZCL_CLUSTER_Z906_TELEMETRY,
        .attr_id = ZCL_ATTR_AUTO_STANDBY, .attr_type = 0x10, .val_u32 = state->auto_standby_enabled ? 1 : 0
    };
    zigbee_app_report_zcl_attribute(&rec_auto_standby);

    zcl_report_record_t rec_led = {
        .endpoint = Z906_ZB_EP_TELEMETRY, .cluster_id = ZCL_CLUSTER_Z906_TELEMETRY,
        .attr_id = ZCL_ATTR_BUILT_IN_LED_ENABLED, .attr_type = 0x10, .val_u32 = led_notification_is_enabled() ? 1 : 0
    };
    zigbee_app_report_zcl_attribute(&rec_led);

    zcl_report_record_t rec_verif = {
        .endpoint = Z906_ZB_EP_TELEMETRY, .cluster_id = ZCL_CLUSTER_Z906_TELEMETRY,
        .attr_id = ZCL_ATTR_CMD_VERIFIER_ENABLED, .attr_type = 0x10, .val_u32 = z906_cmd_verifier_is_enabled() ? 1 : 0
    };
    zigbee_app_report_zcl_attribute(&rec_verif);

    /* 5. Standard 0x0402 Temperature Measurement Endpoints: EP 11, 12, 13 */
    static const uint8_t temp_eps[3] = { Z906_ZB_EP_TEMP1, Z906_ZB_EP_TEMP2, Z906_ZB_EP_TEMP3 };
    for (int i = 0; i < 3; i++) {
        int16_t zcl_val = zigbee_state_publisher_encode_temperature((float)state->temp_celsius[i], state->temp_valid);
        s_reported_temps[i] = zcl_val;

        zcl_report_record_t rec_temp = {
            .endpoint = temp_eps[i],
            .cluster_id = ZCL_CLUSTER_TEMP_MEASUREMENT,
            .attr_id = ZCL_ATTR_TEMP_MEASURED_VALUE,
            .attr_type = 0x29, /* int16_t */
            .val_i16 = zcl_val
        };
        zigbee_app_report_zcl_attribute(&rec_temp);
    }

    ESP_LOGI(TAG, "Publishing ZCL Attribute Report: Power=%s, Mute=%s, Volume=%d (ZCL: %d), Trims=[Bass:%d (ZCL:%d), Center:%d (ZCL:%d), Rear:%d (ZCL:%d)], Input=%d, Effect=%d",
             state->power ? "ON" : "STANDBY", state->mute ? "MUTED" : "UNMUTED",
             state->volume, zcl_volume,
             state->volume_bass, zcl_bass,
             state->volume_center, zcl_center,
             state->volume_rear, zcl_rear,
             (int)state->input, (int)state->effect);

    ESP_LOGI(TAG, "Publishing ZCL Standard 0x0402 Temps: EP11=%d (0.01°C), EP12=%d (0.01°C), EP13=%d (0.01°C), Telemetry 0xFC03: InputLevel=%lu, IdleTime=%lu s, DigitalSig=0x%02X, AutoStandby=%s, BuiltInLed=%s",
             s_reported_temps[0], s_reported_temps[1], s_reported_temps[2],
             (unsigned long)state->input_signal_level,
             (unsigned long)state->idle_seconds,
             state->digital_signal_status,
             state->auto_standby_enabled ? "Enabled" : "Disabled",
             led_notification_is_enabled() ? "ON" : "OFF");

    s_reported_state = *state;
    s_has_reported = true;
    s_report_count++;
}

void zigbee_app_report_state(const z906_state_t *state)
{
    if (state == NULL || s_state != ZB_STATE_JOINED) {
        return;
    }

    if (!s_has_reported || has_state_changed(state, &s_reported_state)) {
        do_report_state(state);
    }
}

void zigbee_app_report_state_force(const z906_state_t *state)
{
    if (state == NULL || s_state != ZB_STATE_JOINED) {
        return;
    }
    do_report_state(state);
}

uint32_t zigbee_app_get_report_count(void)
{
    return s_report_count;
}

void zigbee_app_get_last_reported_state(z906_state_t *out_state)
{
    if (out_state != NULL) {
        *out_state = s_reported_state;
    }
}

int16_t zigbee_app_get_last_temp_reported(uint8_t endpoint)
{
    switch (endpoint) {
        case Z906_ZB_EP_TEMP1: return s_reported_temps[0];
        case Z906_ZB_EP_TEMP2: return s_reported_temps[1];
        case Z906_ZB_EP_TEMP3: return s_reported_temps[2];
        default: return ZCL_TEMP_INVALID_VALUE;
    }
}

/* ========================================================================= */
/* Test & Simulation API Hooks                                               */
/* ========================================================================= */

bool zigbee_app_simulate_join(uint16_t pan_id, uint8_t channel, const uint8_t *ext_pan)
{
    s_net_info.is_paired = true;
    s_net_info.pan_id = pan_id;
    s_net_info.channel = channel;

    if (ext_pan != NULL) {
        memcpy(s_net_info.ext_pan_id, ext_pan, sizeof(s_net_info.ext_pan_id));
    } else {
        memset(s_net_info.ext_pan_id, 0xAA, sizeof(s_net_info.ext_pan_id));
    }

    /* Persist credentials to NVS */
    if (!nvs_manager_save_zb_net_info(&s_net_info)) {
        ESP_LOGE(TAG, "Failed to persist network info to NVS on simulated join");
        return false;
    }

    s_state = ZB_STATE_JOINED;
    s_pairing_timer_s = 0;
    s_rejoin_timer_s = 0;

    ESP_LOGI(TAG, "Zigbee association successful: Joined PAN 0x%04X on Channel %d", pan_id, channel);
    return true;
}

void zigbee_app_simulate_rejoin_failure(void)
{
    if (s_state == ZB_STATE_REJOINING) {
        ESP_LOGW(TAG, "Rejoin attempt failed: Coordinator unreachable. Will retry in %d seconds.",
                 Z906_ZB_REJOIN_INTERVAL_S);
    }
}

void zigbee_app_simulate_commission_failure(void)
{
    s_state = ZB_STATE_COMMISSION_FAILED;
    s_pairing_timer_s = 0;
    s_rejoin_timer_s = 0;
}

void zigbee_app_reset_state(void)
{
    s_state = ZB_STATE_UNPAIRED;
    memset(&s_net_info, 0, sizeof(s_net_info));
    s_pairing_timer_s = 0;
    s_rejoin_timer_s = 0;
    s_has_reported = false;
    s_report_count = 0;
    memset(&s_reported_state, 0, sizeof(s_reported_state));
}
