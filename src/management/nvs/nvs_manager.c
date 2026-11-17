/**
 * @file nvs_manager.c
 * @brief Implementation of NVS storage and persistent Zigbee credentials management.
 */

#include "nvs_manager.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include <string.h>

/** @brief Logging tag for Non-Volatile Storage operations */
static const char *TAG = "NVS_MGR";

/** @brief Dedicated NVS namespace identifier for Zigbee network metadata */
static const char *NVS_NAMESPACE_ZB = "zb_store";

/** @brief Key for boolean paired flag (stored as uint8_t: 1 = true, 0 = false) */
static const char *NVS_KEY_PAIRED = "zb_paired";

/** @brief Key for 16-bit Zigbee PAN Identifier */
static const char *NVS_KEY_PAN_ID = "pan_id";

/** @brief Key for active 802.15.4 channel number (11-26) */
static const char *NVS_KEY_CHANNEL = "channel";

/** @brief Key for 64-bit Extended PAN Identifier (8-byte binary blob) */
static const char *NVS_KEY_EXT_PAN = "ext_pan";

/** @brief Key for boolean Command Verifier enabled flag (1 = true, 0 = false) */
static const char *NVS_KEY_VERIF_EN = "verif_en";

/** @brief Key for boolean one-shot pair intent after reset flag (1 = true, 0 = false) */
static const char *NVS_KEY_PAIR_INTENT = "pair_intent";

bool nvs_manager_init(void)
{
    /* Initialize default NVS partition */
    esp_err_t ret = nvs_flash_init();

    /* If NVS partition is truncated or contains newer version structure, erase and reinitialize */
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS partition truncated or incompatible, erasing and recovering...");
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize default NVS partition (err: 0x%x)", ret);
        return false;
    }

    ESP_LOGI(TAG, "NVS partition successfully initialized and verified");
    return true;
}

bool nvs_manager_get_zb_paired(bool *out_is_paired)
{
    /* Validate output pointer parameter */
    if (out_is_paired == NULL) {
        ESP_LOGE(TAG, "nvs_manager_get_zb_paired: NULL output pointer provided");
        return false;
    }

    /* Default output value to false */
    *out_is_paired = false;

    nvs_handle_t handle;
    /* Open the dedicated Zigbee storage namespace in read-only mode */
    esp_err_t err = nvs_open(NVS_NAMESPACE_ZB, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        /* Namespace does not exist yet (virgin device state), return false gracefully */
        return false;
    }

    uint8_t val = 0;
    /* Read uint8 pairing status flag from flash */
    err = nvs_get_u8(handle, NVS_KEY_PAIRED, &val);
    nvs_close(handle);

    if (err == ESP_OK) {
        *out_is_paired = (val != 0);
        return true;
    }

    return false;
}

bool nvs_manager_save_zb_net_info(const zb_persistent_net_info_t *net_info)
{
    /* Validate input pointer parameter */
    if (net_info == NULL) {
        ESP_LOGE(TAG, "nvs_manager_save_zb_net_info: NULL pointer provided");
        return false;
    }

    nvs_handle_t handle;
    /* Open Zigbee storage namespace in read-write mode */
    esp_err_t err = nvs_open(NVS_NAMESPACE_ZB, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS namespace '%s' for saving network info (err: %s)",
                 NVS_NAMESPACE_ZB, esp_err_to_name(err));
        return false;
    }

    /* -------------------------------------------------------------------
     * Write deduplication: read all four current NVS values and compare
     * with the incoming struct. If all four match exactly, skip every
     * nvs_set_* and the nvs_commit() call entirely.
     *
     * Three distinct result classes for each nvs_get_*() call:
     *   ESP_OK           — value exists; compare it with incoming value.
     *   ESP_ERR_NVS_NOT_FOUND — key absent (e.g. virgin device or cleared
     *                     namespace); key must be initialized on write.
     *   Any other error  — real NVS subsystem / API failure; abort the
     *                     entire operation, close the handle, return false.
     *                     Do not silently overwrite on an unknown fault.
     * ------------------------------------------------------------------- */
    bool needs_write = false;
    esp_err_t rd_err;

    /* 1. Check paired flag */
    uint8_t stored_paired = 0;
    uint8_t incoming_paired = net_info->is_paired ? 1 : 0;
    rd_err = nvs_get_u8(handle, NVS_KEY_PAIRED, &stored_paired);
    if (rd_err == ESP_OK) {
        if (stored_paired != incoming_paired) {
            needs_write = true;
        }
    } else if (rd_err == ESP_ERR_NVS_NOT_FOUND) {
        needs_write = true; /* Key absent: must create */
    } else {
        /* Real NVS subsystem failure — abort; do not attempt silent overwrite */
        ESP_LOGE(TAG, "Failed to read NVS key '%s': %s", NVS_KEY_PAIRED, esp_err_to_name(rd_err));
        nvs_close(handle);
        return false;
    }

    /* 2. Check PAN ID */
    uint16_t stored_pan = 0;
    rd_err = nvs_get_u16(handle, NVS_KEY_PAN_ID, &stored_pan);
    if (rd_err == ESP_OK) {
        if (stored_pan != net_info->pan_id) {
            needs_write = true;
        }
    } else if (rd_err == ESP_ERR_NVS_NOT_FOUND) {
        needs_write = true;
    } else {
        ESP_LOGE(TAG, "Failed to read NVS key '%s': %s", NVS_KEY_PAN_ID, esp_err_to_name(rd_err));
        nvs_close(handle);
        return false;
    }

    /* 3. Check channel */
    uint8_t stored_channel = 0;
    rd_err = nvs_get_u8(handle, NVS_KEY_CHANNEL, &stored_channel);
    if (rd_err == ESP_OK) {
        if (stored_channel != net_info->channel) {
            needs_write = true;
        }
    } else if (rd_err == ESP_ERR_NVS_NOT_FOUND) {
        needs_write = true;
    } else {
        ESP_LOGE(TAG, "Failed to read NVS key '%s': %s", NVS_KEY_CHANNEL, esp_err_to_name(rd_err));
        nvs_close(handle);
        return false;
    }

    /* 4. Check Extended PAN ID (8-byte blob).
     *
     * nvs_get_blob() result classes:
     *   ESP_OK                 — blob exists; verify size then compare bytes.
     *   ESP_ERR_NVS_NOT_FOUND  — key absent; initialize on write.
     *   ESP_ERR_NVS_INVALID_LENGTH — stored blob exists but has a different
     *                            length than requested. This is an application-
     *                            level data issue (e.g. schema change or old
     *                            firmware wrote a different-sized blob), not a
     *                            hardware storage fault. Treat as stale data:
     *                            overwrite required.
     *   Any other error        — real NVS subsystem failure; abort. */
    uint8_t stored_ext_pan[8] = {0};
    size_t blob_len = sizeof(stored_ext_pan);
    rd_err = nvs_get_blob(handle, NVS_KEY_EXT_PAN, stored_ext_pan, &blob_len);
    if (rd_err == ESP_OK) {
        /* Blob read succeeded; check size integrity then compare all bytes */
        if (blob_len != sizeof(net_info->ext_pan_id) ||
            memcmp(stored_ext_pan, net_info->ext_pan_id, sizeof(net_info->ext_pan_id)) != 0) {
            needs_write = true;
        }
    } else if (rd_err == ESP_ERR_NVS_NOT_FOUND) {
        needs_write = true; /* Key absent */
    } else if (rd_err == ESP_ERR_NVS_INVALID_LENGTH) {
        /* Stored blob has unexpected length: stale / schema-changed application
         * data. Treat as requiring rewrite rather than a storage fault. */
        ESP_LOGW(TAG, "NVS key '%s' has unexpected stored length — will overwrite", NVS_KEY_EXT_PAN);
        needs_write = true;
    } else {
        /* Real NVS subsystem failure */
        ESP_LOGE(TAG, "Failed to read NVS key '%s': %s", NVS_KEY_EXT_PAN, esp_err_to_name(rd_err));
        nvs_close(handle);
        return false;
    }

    /* Short-circuit: all four values identical — no write, no commit */
    if (!needs_write) {
        nvs_close(handle);
        ESP_LOGD(TAG, "Zigbee net info unchanged — NVS write skipped (dedup)");
        return true;
    }

    /* At least one value changed or is missing. Stage all four join-state
     * values and persist them through one commit call. Writing all four
     * (rather than only the changed subset) ensures that all keys are present
     * and consistent after a successful save, even if a prior partial-write
     * left some keys absent. */
    err = nvs_set_u8(handle, NVS_KEY_PAIRED, incoming_paired);
    if (err != ESP_OK) goto save_error;

    err = nvs_set_u16(handle, NVS_KEY_PAN_ID, net_info->pan_id);
    if (err != ESP_OK) goto save_error;

    err = nvs_set_u8(handle, NVS_KEY_CHANNEL, net_info->channel);
    if (err != ESP_OK) goto save_error;

    err = nvs_set_blob(handle, NVS_KEY_EXT_PAN, net_info->ext_pan_id, sizeof(net_info->ext_pan_id));
    if (err != ESP_OK) goto save_error;

    /* Persist all staged join-state values through a single commit */
    err = nvs_commit(handle);
    nvs_close(handle);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to commit Zigbee network info to NVS (err: %s)", esp_err_to_name(err));
        return false;
    }

    ESP_LOGI(TAG, "Saved persistent Zigbee network: PAN=0x%04X, Ch=%d, Paired=%d",
             net_info->pan_id, net_info->channel, net_info->is_paired);
    return true;

save_error:
    ESP_LOGE(TAG, "Failed to write NVS key (err: %s)", esp_err_to_name(err));
    nvs_close(handle);
    return false;
}


bool nvs_manager_load_zb_net_info(zb_persistent_net_info_t *out_net_info)
{
    /* Validate output structure pointer */
    if (out_net_info == NULL) {
        ESP_LOGE(TAG, "nvs_manager_load_zb_net_info: NULL pointer provided");
        return false;
    }

    /* Clear output structure to ensure zeroed defaults on partial failure */
    memset(out_net_info, 0, sizeof(zb_persistent_net_info_t));

    nvs_handle_t handle;
    /* Open Zigbee namespace in read-only mode */
    esp_err_t err = nvs_open(NVS_NAMESPACE_ZB, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        return false;
    }

    /* 1. Read paired flag */
    uint8_t paired_val = 0;
    err = nvs_get_u8(handle, NVS_KEY_PAIRED, &paired_val);
    if (err != ESP_OK) {
        nvs_close(handle);
        return false;
    }
    out_net_info->is_paired = (paired_val != 0);

    /* 2. Read 16-bit PAN Identifier */
    err = nvs_get_u16(handle, NVS_KEY_PAN_ID, &out_net_info->pan_id);
    if (err != ESP_OK) {
        nvs_close(handle);
        return false;
    }

    /* 3. Read 802.15.4 Channel */
    err = nvs_get_u8(handle, NVS_KEY_CHANNEL, &out_net_info->channel);
    if (err != ESP_OK) {
        nvs_close(handle);
        return false;
    }

    /* 4. Read 64-bit Extended PAN ID */
    size_t blob_len = sizeof(out_net_info->ext_pan_id);
    err = nvs_get_blob(handle, NVS_KEY_EXT_PAN, out_net_info->ext_pan_id, &blob_len);
    nvs_close(handle);

    if (err != ESP_OK || blob_len != sizeof(out_net_info->ext_pan_id)) {
        return false;
    }

    return true;
}

bool nvs_manager_clear_zb_net_info(void)
{
    nvs_handle_t handle;
    /* Open Zigbee namespace in read-write mode */
    esp_err_t err = nvs_open(NVS_NAMESPACE_ZB, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        /* Namespace does not exist; nothing to clear */
        return true;
    }

    /* Erase all keys residing within the zb_store namespace */
    err = nvs_erase_all(handle);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to erase Zigbee namespace '%s' (err: 0x%x)", NVS_NAMESPACE_ZB, err);
        return false;
    }

    ESP_LOGI(TAG, "Cleared all persistent Zigbee credentials from namespace '%s'", NVS_NAMESPACE_ZB);
    return true;
}

bool nvs_manager_check_read_health(void)
{
    /* Missing namespace/key on a virgin device is not a flash fault.
     * Real NVS open/read/type errors must prevent post-OTA confirmation.
     * No NVS mutation occurs in this probe. */
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE_ZB, NVS_READONLY, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return true;
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS health probe open failed (err: 0x%x)", err);
        return false;
    }

    uint8_t verifier_enabled = 1;
    err = nvs_get_u8(handle, NVS_KEY_VERIF_EN, &verifier_enabled);
    nvs_close(handle);
    if (err == ESP_OK || err == ESP_ERR_NVS_NOT_FOUND) {
        return true;
    }
    ESP_LOGE(TAG, "NVS health probe read failed (err: 0x%x)", err);
    return false;
}

bool nvs_manager_get_cmd_verifier_enabled(bool *out_enabled)
{
    /* Validate output pointer parameter */
    if (out_enabled == NULL) {
        ESP_LOGE(TAG, "nvs_manager_get_cmd_verifier_enabled: NULL pointer provided");
        return false;
    }

    /* Default output value to true (enabled by default) */
    *out_enabled = true;

    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE_ZB, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        /* Namespace does not exist yet; return true default cleanly */
        return true;
    }

    uint8_t val = 1;
    err = nvs_get_u8(handle, NVS_KEY_VERIF_EN, &val);
    nvs_close(handle);

    if (err == ESP_OK) {
        *out_enabled = (val != 0);
        return true;
    }

    /* Key not found in NVS: retain true default */
    return true;
}

bool nvs_manager_set_cmd_verifier_enabled(bool enabled)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE_ZB, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS namespace '%s' for writing verifier flag (err: 0x%x)",
                 NVS_NAMESPACE_ZB, err);
        return false;
    }

    /* Write deduplication: check current value before writing to flash */
    uint8_t current_val = 255;
    if (nvs_get_u8(handle, NVS_KEY_VERIF_EN, &current_val) == ESP_OK) {
        if ((current_val != 0) == enabled) {
            nvs_close(handle);
            return true;
        }
    }

    uint8_t val = enabled ? 1 : 0;
    err = nvs_set_u8(handle, NVS_KEY_VERIF_EN, val);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to write '%s' to NVS (err: 0x%x)", NVS_KEY_VERIF_EN, err);
        return false;
    }

    ESP_LOGI(TAG, "Command Verifier enabled flag persisted to NVS: %s", enabled ? "ENABLED" : "DISABLED");
    return true;
}

bool nvs_manager_get_pair_after_reset(bool *out_intent)
{
    if (out_intent == NULL) {
        return false;
    }
    *out_intent = false;

    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE_ZB, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        return false;
    }

    uint8_t val = 0;
    err = nvs_get_u8(handle, NVS_KEY_PAIR_INTENT, &val);
    nvs_close(handle);

    if (err == ESP_OK) {
        *out_intent = (val != 0);
        return true;
    }

    return false;
}

bool nvs_manager_set_pair_after_reset(bool intent)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE_ZB, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS namespace '%s' for writing pair intent (err: 0x%x)",
                 NVS_NAMESPACE_ZB, err);
        return false;
    }

    /* Write deduplication */
    uint8_t current_val = 255;
    if (nvs_get_u8(handle, NVS_KEY_PAIR_INTENT, &current_val) == ESP_OK) {
        if ((current_val != 0) == intent) {
            nvs_close(handle);
            return true;
        }
    }

    uint8_t val = intent ? 1 : 0;
    err = nvs_set_u8(handle, NVS_KEY_PAIR_INTENT, val);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to write '%s' to NVS (err: 0x%x)", NVS_KEY_PAIR_INTENT, err);
        return false;
    }

    ESP_LOGI(TAG, "One-shot pair intent persisted to NVS: %s", intent ? "TRUE" : "FALSE");
    return true;
}

bool nvs_manager_clear_pair_after_reset(void)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE_ZB, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        /* If namespace doesn't exist, key is already absent */
        return true;
    }

    err = nvs_erase_key(handle, NVS_KEY_PAIR_INTENT);
    if (err == ESP_OK) {
        nvs_commit(handle);
        ESP_LOGI(TAG, "One-shot pair intent cleared from NVS.");
    } else if (err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "Failed to erase key '%s' from NVS (err: 0x%x)", NVS_KEY_PAIR_INTENT, err);
    }
    nvs_close(handle);
    return true;
}

bool nvs_manager_factory_reset(void)
{
    /* Wipe entire physical NVS partition (true factory reset).
     * WARNING: This erases ALL namespaces — 'zb_store', 'led_store', and
     * any others. For Zigbee-only clearing, use nvs_manager_clear_zb_net_info(). */
    esp_err_t err = nvs_flash_erase();
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Complete NVS partition erased successfully (factory reset)");
        nvs_flash_init();
        return true;
    }
    ESP_LOGE(TAG, "Failed to erase full NVS partition (err: 0x%x)", err);
    return false;
}
