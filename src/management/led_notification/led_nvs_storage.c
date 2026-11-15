/**
 * @file led_nvs_storage.c
 * @brief Implementation of persistent storage for LED notification settings.
 */

#include "led_nvs_storage.h"
#include "led_animation_registry.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "LED_NVS";

static const char *NVS_NAMESPACE_LED = "led_store";
static const char *NVS_KEY_ENABLED   = "led_en";

static const char *EVENT_NAMES[LED_EVENT_COUNT] = {
    "boot",
    "pairing",
    "normal",
    "command_failure",
    "command_success"
};

static const char *NVS_MAPPING_KEYS[LED_EVENT_COUNT] = {
    "m_boot",
    "m_pair",
    "m_norm",
    "m_fail",
    "m_succ"
};

static const char *DEFAULT_ANIMATIONS[LED_EVENT_COUNT] = {
    "rainbow",            /* Boot default */
    "police_flasher",     /* Pairing default */
    "white_double_blink", /* Normal default */
    "red_sos",            /* Command failure default */
    "green_blink"         /* Command success default */
};

const char *led_event_to_string(led_event_type_t event)
{
    if ((int)event >= 0 && event < LED_EVENT_COUNT) {
        return EVENT_NAMES[event];
    }
    return "unknown";
}

bool led_event_from_string(const char *str, led_event_type_t *out_event)
{
    if (str == NULL || out_event == NULL) {
        return false;
    }
    for (int i = 0; i < LED_EVENT_COUNT; i++) {
        if (strcmp(str, EVENT_NAMES[i]) == 0) {
            *out_event = (led_event_type_t)i;
            return true;
        }
    }
    return false;
}

const char *led_get_default_animation_for_event(led_event_type_t event)
{
    if ((int)event >= 0 && event < LED_EVENT_COUNT) {
        return DEFAULT_ANIMATIONS[event];
    }
    return "white_double_blink";
}

bool led_nvs_storage_load(led_config_bundle_t *out_config)
{
    if (out_config == NULL) {
        return false;
    }

    /* 1. Apply baseline factory defaults first */
    out_config->enabled = true;
    for (int i = 0; i < LED_EVENT_COUNT; i++) {
        strncpy(out_config->mapping[i], DEFAULT_ANIMATIONS[i], LED_ANIM_ID_MAX_LEN - 1);
        out_config->mapping[i][LED_ANIM_ID_MAX_LEN - 1] = '\0';
    }

    /* 2. Try loading from NVS */
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE_LED, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        ESP_LOGI(TAG, "No existing LED configuration in NVS, using factory defaults (enabled: ON)");
        return true;
    }

    /* Load enabled flag */
    uint8_t en_val = 1;
    if (nvs_get_u8(handle, NVS_KEY_ENABLED, &en_val) == ESP_OK) {
        out_config->enabled = (en_val != 0);
    }

    /* Load mappings individually */
    for (int i = 0; i < LED_EVENT_COUNT; i++) {
        char buf[LED_ANIM_ID_MAX_LEN];
        size_t len = sizeof(buf);
        if (nvs_get_str(handle, NVS_MAPPING_KEYS[i], buf, &len) == ESP_OK) {
            /* Verify that the animation ID still exists in the registry */
            if (led_animation_registry_find_by_id(buf) != NULL) {
                strncpy(out_config->mapping[i], buf, LED_ANIM_ID_MAX_LEN - 1);
                out_config->mapping[i][LED_ANIM_ID_MAX_LEN - 1] = '\0';
            } else {
                ESP_LOGW(TAG, "Stored animation '%s' for event '%s' not registered, falling back to default '%s'",
                         buf, EVENT_NAMES[i], DEFAULT_ANIMATIONS[i]);
            }
        }
    }

    nvs_close(handle);
    ESP_LOGI(TAG, "LED configuration loaded from NVS (enabled: %s)", out_config->enabled ? "ON" : "OFF");
    return true;
}

bool led_nvs_storage_save_enabled(bool enabled)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE_LED, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS namespace '%s' for writing enabled state (err: 0x%x)", NVS_NAMESPACE_LED, err);
        return false;
    }

    uint8_t curr = 255;
    if (nvs_get_u8(handle, NVS_KEY_ENABLED, &curr) == ESP_OK) {
        if ((curr != 0) == enabled) {
            /* Deduplicated: no write needed */
            nvs_close(handle);
            return true;
        }
    }

    err = nvs_set_u8(handle, NVS_KEY_ENABLED, enabled ? 1 : 0);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to commit LED enabled state to NVS (err: 0x%x)", err);
        return false;
    }

    ESP_LOGI(TAG, "Saved LED enabled state: %s", enabled ? "ON" : "OFF");
    return true;
}

bool led_nvs_storage_save_mapping(led_event_type_t event, const char *anim_id)
{
    if ((int)event < 0 || event >= LED_EVENT_COUNT || anim_id == NULL) {
        return false;
    }

    /* Validate animation existence */
    if (led_animation_registry_find_by_id(anim_id) == NULL) {
        ESP_LOGE(TAG, "Cannot save unregistered animation ID '%s'", anim_id);
        return false;
    }

    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE_LED, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS namespace '%s' for writing mapping (err: 0x%x)", NVS_NAMESPACE_LED, err);
        return false;
    }

    char curr[LED_ANIM_ID_MAX_LEN] = {0};
    size_t len = sizeof(curr);
    if (nvs_get_str(handle, NVS_MAPPING_KEYS[event], curr, &len) == ESP_OK) {
        if (strcmp(curr, anim_id) == 0) {
            /* Deduplicated: already stored */
            nvs_close(handle);
            return true;
        }
    }

    err = nvs_set_str(handle, NVS_MAPPING_KEYS[event], anim_id);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to commit LED mapping for '%s' to NVS (err: 0x%x)", EVENT_NAMES[event], err);
        return false;
    }

    ESP_LOGI(TAG, "Saved LED mapping: %s -> %s", EVENT_NAMES[event], anim_id);
    return true;
}

bool led_nvs_storage_reset_mappings(int event_or_all, led_config_bundle_t *config)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE_LED, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS namespace '%s' for resetting mappings", NVS_NAMESPACE_LED);
        return false;
    }

    if (event_or_all == -1) {
        /* Reset all 5 mappings */
        for (int i = 0; i < LED_EVENT_COUNT; i++) {
            nvs_erase_key(handle, NVS_MAPPING_KEYS[i]);
            if (config != NULL) {
                strncpy(config->mapping[i], DEFAULT_ANIMATIONS[i], LED_ANIM_ID_MAX_LEN - 1);
                config->mapping[i][LED_ANIM_ID_MAX_LEN - 1] = '\0';
            }
        }
    } else if (event_or_all >= 0 && event_or_all < LED_EVENT_COUNT) {
        nvs_erase_key(handle, NVS_MAPPING_KEYS[event_or_all]);
        if (config != NULL) {
            strncpy(config->mapping[event_or_all], DEFAULT_ANIMATIONS[event_or_all], LED_ANIM_ID_MAX_LEN - 1);
            config->mapping[event_or_all][LED_ANIM_ID_MAX_LEN - 1] = '\0';
        }
    } else {
        nvs_close(handle);
        return false;
    }

    nvs_commit(handle);
    nvs_close(handle);
    ESP_LOGI(TAG, "Reset LED mapping(s) to factory defaults");
    return true;
}
