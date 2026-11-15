/**
 * @file zb_handlers.c
 * @brief Implementation of Zigbee ZCL command handlers with central registry validation and state verification.
 */

#include "zb_handlers.h"
#include "z906_cmd_verifier.h"
#include "z906_cmd_registry.h"
#include "z906_commands.h"
#include "led_notification_manager.h"
#include "zigbee_state_publisher.h"
#include "esp_log.h"

static const char *TAG = "ZB_HANDLERS";

void zb_handler_on_off(bool power_on)
{
    ESP_LOGI(TAG, "Incoming ZCL Power request: %s", power_on ? "ON" : "OFF");

    int32_t val = power_on ? 1 : 0;
    if (!z906_cmd_registry_validate_param(Z906_CMD_ID_POWER, val)) {
        ESP_LOGW(TAG, "Rejected ZCL Power request: parameter %ld out of bounds", (long)val);
        return;
    }

    cmd_request_t req;
    if (!z906_cmd_registry_build_request(Z906_CMD_ID_POWER, CMD_SOURCE_ZIGBEE, val, &req)) {
        ESP_LOGE(TAG, "Failed to build command request for ZCL Power");
        return;
    }

    z906_cmd_verify_info_t info;
    cmd_verify_result_t res = z906_cmd_verifier_execute(&req, &info);

    if (res == CMD_VERIFY_SUCCESS) {
        ESP_LOGI(TAG, "ZCL Power %s successfully verified (Latency: %lu ms)",
                 info.post_state.power ? "ON" : "STANDBY", (unsigned long)info.duration_ms);
    } else {
        ESP_LOGW(TAG, "ZCL Power %s verification failed: %s (Code: %d, Latency: %lu ms)",
                 power_on ? "ON" : "OFF", z906_cmd_verify_result_str(res), (int)res, (unsigned long)info.duration_ms);
    }
}

void zb_handler_set_level(uint8_t zcl_level)
{
    uint8_t hw_volume = z906_cmd_zcl_to_volume(zcl_level);
    ESP_LOGI(TAG, "Incoming ZCL Level request: %d (ZCL: %d)", hw_volume, zcl_level);

    if (!z906_cmd_registry_validate_param(Z906_CMD_ID_VOLUME, hw_volume)) {
        ESP_LOGW(TAG, "Rejected ZCL Level request: volume %d out of bounds", hw_volume);
        return;
    }

    cmd_request_t req;
    if (!z906_cmd_registry_build_request(Z906_CMD_ID_VOLUME, CMD_SOURCE_ZIGBEE, hw_volume, &req)) {
        ESP_LOGE(TAG, "Failed to build command request for ZCL Level");
        return;
    }

    z906_cmd_verify_info_t info;
    cmd_verify_result_t res = z906_cmd_verifier_execute(&req, &info);

    if (res == CMD_VERIFY_SUCCESS) {
        ESP_LOGI(TAG, "ZCL Volume %d successfully verified (Latency: %lu ms)",
                 info.post_state.volume, (unsigned long)info.duration_ms);
    } else {
        ESP_LOGW(TAG, "ZCL Volume %d verification failed: %s (Code: %d, Latency: %lu ms)",
                 hw_volume, z906_cmd_verify_result_str(res), (int)res, (unsigned long)info.duration_ms);
    }
}

void zb_handler_set_mute(bool mute)
{
    ESP_LOGI(TAG, "Incoming ZCL Mute request: %s", mute ? "MUTED" : "UNMUTED");

    int32_t val = mute ? 1 : 0;
    if (!z906_cmd_registry_validate_param(Z906_CMD_ID_MUTE, val)) {
        ESP_LOGW(TAG, "Rejected ZCL Mute request: parameter %ld out of bounds", (long)val);
        return;
    }

    cmd_request_t req;
    if (!z906_cmd_registry_build_request(Z906_CMD_ID_MUTE, CMD_SOURCE_ZIGBEE, val, &req)) {
        ESP_LOGE(TAG, "Failed to build command request for ZCL Mute");
        return;
    }

    z906_cmd_verify_info_t info;
    cmd_verify_result_t res = z906_cmd_verifier_execute(&req, &info);

    if (res == CMD_VERIFY_SUCCESS) {
        ESP_LOGI(TAG, "ZCL Mute %s successfully verified (Latency: %lu ms)",
                 info.post_state.mute ? "MUTED" : "UNMUTED", (unsigned long)info.duration_ms);
    } else {
        ESP_LOGW(TAG, "ZCL Mute %s verification failed: %s (Code: %d, Latency: %lu ms)",
                 mute ? "MUTED" : "UNMUTED", z906_cmd_verify_result_str(res), (int)res, (unsigned long)info.duration_ms);
    }
}

void zb_handler_set_trim(uint8_t channel, uint8_t zcl_level)
{
    uint8_t hw_level = z906_cmd_zcl_to_volume(zcl_level);
    ESP_LOGI(TAG, "Incoming ZCL Trim channel %d request: %d", channel, hw_level);

    z906_cmd_id_t cmd_id;
    switch ((trim_channel_t)channel) {
        case TRIM_CHANNEL_BASS:   cmd_id = Z906_CMD_ID_TRIM_BASS;   break;
        case TRIM_CHANNEL_CENTER: cmd_id = Z906_CMD_ID_TRIM_CENTER; break;
        case TRIM_CHANNEL_REAR:   cmd_id = Z906_CMD_ID_TRIM_REAR;   break;
        default:
            ESP_LOGW(TAG, "Invalid trim channel %d from Zigbee", channel);
            return;
    }

    if (!z906_cmd_registry_validate_param(cmd_id, hw_level)) {
        ESP_LOGW(TAG, "Rejected ZCL Trim channel %d request: level %d out of bounds", channel, hw_level);
        return;
    }

    cmd_request_t req;
    if (!z906_cmd_registry_build_request(cmd_id, CMD_SOURCE_ZIGBEE, hw_level, &req)) {
        ESP_LOGE(TAG, "Failed to build command request for ZCL Trim");
        return;
    }

    z906_cmd_verify_info_t info;
    cmd_verify_result_t res = z906_cmd_verifier_execute(&req, &info);

    if (res == CMD_VERIFY_SUCCESS) {
        ESP_LOGI(TAG, "ZCL Trim channel %d successfully verified (Latency: %lu ms)",
                 channel, (unsigned long)info.duration_ms);
    } else {
        ESP_LOGW(TAG, "ZCL Trim channel %d verification failed: %s (Code: %d, Latency: %lu ms)",
                 channel, z906_cmd_verify_result_str(res), (int)res, (unsigned long)info.duration_ms);
    }
}

void zb_handler_set_input(uint8_t input)
{
    if (!z906_cmd_registry_validate_param(Z906_CMD_ID_INPUT, input)) {
        ESP_LOGW(TAG, "Invalid audio input value from Zigbee: %d (out of bounds)", input);
        return;
    }

    ESP_LOGI(TAG, "Incoming ZCL Input switch request: %d", input);

    cmd_request_t req;
    if (!z906_cmd_registry_build_request(Z906_CMD_ID_INPUT, CMD_SOURCE_ZIGBEE, input, &req)) {
        ESP_LOGE(TAG, "Failed to build command request for ZCL Input");
        return;
    }

    z906_cmd_verify_info_t info;
    cmd_verify_result_t res = z906_cmd_verifier_execute(&req, &info);

    if (res == CMD_VERIFY_SUCCESS) {
        ESP_LOGI(TAG, "ZCL Input switch to %d successfully verified (Latency: %lu ms)",
                 input, (unsigned long)info.duration_ms);
    } else {
        ESP_LOGW(TAG, "ZCL Input switch to %d verification failed: %s (Code: %d, Latency: %lu ms)",
                 input, z906_cmd_verify_result_str(res), (int)res, (unsigned long)info.duration_ms);
    }
}

void zb_handler_set_effect(uint8_t effect)
{
    if (!z906_cmd_registry_validate_param(Z906_CMD_ID_EFFECT, effect)) {
        ESP_LOGW(TAG, "Invalid audio effect value from Zigbee: %d (out of bounds)", effect);
        return;
    }

    ESP_LOGI(TAG, "Incoming ZCL Effect switch request: %d", effect);

    cmd_request_t req;
    if (!z906_cmd_registry_build_request(Z906_CMD_ID_EFFECT, CMD_SOURCE_ZIGBEE, effect, &req)) {
        ESP_LOGE(TAG, "Failed to build command request for ZCL Effect");
        return;
    }

    z906_cmd_verify_info_t info;
    cmd_verify_result_t res = z906_cmd_verifier_execute(&req, &info);

    if (res == CMD_VERIFY_SUCCESS) {
        ESP_LOGI(TAG, "ZCL Effect switch to %d successfully verified (Latency: %lu ms)",
                 effect, (unsigned long)info.duration_ms);
    } else {
        ESP_LOGW(TAG, "ZCL Effect switch to %d verification failed: %s (Code: %d, Latency: %lu ms)",
                 effect, z906_cmd_verify_result_str(res), (int)res, (unsigned long)info.duration_ms);
    }
}

void zb_handler_set_led_enabled(bool enabled)
{
    ESP_LOGI(TAG, "Incoming ZCL Built-in LED enable request: %s", enabled ? "ON" : "OFF");
    led_notification_set_enabled(enabled);
    zigbee_state_publisher_notify(ZB_PUB_NOTIF_BUILT_IN_LED);
}

void zb_handler_set_verifier_enabled(bool enabled)
{
    ESP_LOGI(TAG, "Incoming ZCL Command Verifier enable request: %s", enabled ? "ON" : "OFF");
    z906_cmd_verifier_set_enabled(enabled);
    zigbee_state_publisher_notify(ZB_PUB_NOTIF_CMD_VERIFIER);
}
